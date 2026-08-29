// Device lifecycle, samplers, root-signature cache, deferred release, and
// feature queries. Ported/adapted from NVRHI's d3d12-device.cpp and
// d3d12-resource-bindings.cpp (External/nvrhi/src/d3d12).
// Copyright (c) 2014-2021, NVIDIA CORPORATION. MIT license
// (see ThirdParty/NVRHI-LICENSE.txt).
//
// UnityRHI deviations: no queues of our own (commands are injected into the
// lists Unity submits), opaque-handle lifetime instead of RefCountPtr,
// singleton device bound to Unity's ID3D12Device. GPU lifetime is expressed
// in plugin-owned recording instances retired through MARKER_OUT writes
// (BeginRecordingInstance / CompletedLifetimeInstance) - never in Unity's
// frame-fence values, which are not a retire marker for injected work (see
// docs/rt-sbt-upload-corruption-report.md).

#include "d3d12-backend.h"

#include <cstdio>

#include <directx/d3d12.h>
#include <dxgi.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Breadcrumbs.h"
#include "common/dxgi-format.h"
#include "DredDiagnostics.h"
#include "GpuDumps.h"
#include "IUnityGraphicsD3D12.h"
#include "UnityRhiLog.h"

#if UNITYRHI_WITH_NVAPI
#include <nvapi.h> // driver-level ray tracing validation (uses the __d3d12_h__ guard, keep after d3d12.h)
#endif

namespace unityrhi
{
Device* Device::s_instance = nullptr;

namespace
{
const char* KindName(RhiResource::Kind kind)
{
    static const char* kKindNames[] = {"Buffer", "Texture", "Sampler", "Shader",
        "BindingLayout", "BindingSet", "ComputePipeline", "ShaderLibrary",
        "AccelStruct", "RayTracingPipeline", "ShaderTable", "DescriptorTable",
        "InputLayout", "Framebuffer", "GraphicsPipeline", "Heap", "StagingTexture",
        "EventQuery", "TimerQuery"};
    static_assert(sizeof(kKindNames) / sizeof(kKindNames[0]) == size_t(RhiResource::Kind::Count));
    return uint32_t(kind) < uint32_t(RhiResource::Kind::Count) ? kKindNames[uint32_t(kind)] : "?";
}

const char* DeviceRemovedReasonName(HRESULT reason)
{
    switch (reason)
    {
    case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
    default: return "unknown";
    }
}
} // namespace

void Context::error(const std::string& message) const
{
    LogError("[UnityRHI] %s", message.c_str());
}

void Context::info(const std::string& message) const
{
    LogInfo("[UnityRHI] %s", message.c_str());
}

#if UNITYRHI_WITH_NVAPI
namespace
{
std::atomic<uint32_t> g_rtValidationMessageCount{0};

// Port of nvrhi raytracingValidationMessageCallback (d3d12-device.cpp): routes
// NVAPI ray tracing validation messages to the Unity log. May be invoked from
// an arbitrary driver thread; it only logs and must not touch the device.
void __stdcall RaytracingValidationMessageCallback(
    void* /*pUserData*/,
    NVAPI_D3D12_RAYTRACING_VALIDATION_MESSAGE_SEVERITY severity,
    const char* messageCode,
    const char* message,
    const char* messageDetails)
{
    g_rtValidationMessageCount.fetch_add(1, std::memory_order_relaxed);
    const bool isError = severity == NVAPI_D3D12_RAYTRACING_VALIDATION_MESSAGE_SEVERITY_ERROR;
    (isError ? LogError : LogWarning)("[UnityRHI] Ray Tracing Validation [%s] %s%s%s",
        messageCode ? messageCode : "",
        message ? message : "",
        messageDetails && messageDetails[0] ? "\n" : "",
        messageDetails ? messageDetails : "");
}
} // namespace

uint32_t Device::RayTracingValidationMessageCount()
{
    return g_rtValidationMessageCount.load(std::memory_order_relaxed);
}
#endif

namespace
{
void SetDebugName(ID3D12Object* object, const char* debugName)
{
    if (!object || !debugName || !debugName[0])
        return;
    wchar_t wide[256];
    int written = MultiByteToWideChar(CP_UTF8, 0, debugName, -1, wide, 255);
    wide[written > 0 ? written : 0] = L'\0';
    object->SetName(wide);
}
} // namespace

void Device::Create(ID3D12Device* device, UnityD3D12Interface* unityD3D12)
{
    if (s_instance)
        return;
    s_instance = new Device(device, unityD3D12);
    LogInfo("[UnityRHI] Device created.");
}

void Device::Destroy()
{
    if (!s_instance)
        return;
    delete s_instance;
    s_instance = nullptr;
}

Device::Device(ID3D12Device* device, UnityD3D12Interface* unityD3D12)
    : m_Context{device}
    , m_Resources(m_Context)
    , m_UploadManager(m_Context, this, c_DefaultUploadChunkSize, 0, false)
    , m_DxrScratchManager(m_Context, this, c_DefaultScratchChunkSize, c_DefaultScratchMaxMemory, true)
    , m_unityD3D12(unityD3D12)
{
    // DXR entry points live on ID3D12Device5; null when the runtime is too old.
    m_Context.device->QueryInterface(IID_PPV_ARGS(&m_Context.device5));

#if UNITYRHI_WITH_NVAPI
    // Driver-level ray tracing validation, opt-in by launching Unity/the
    // player with NV_ALLOW_RAYTRACING_VALIDATION=1 (the same variable the
    // driver itself gates on; ~3% GPU overhead when enabled). Must be enabled
    // before any other ray tracing call on the device - including the AS
    // prebuild-info queries - or it fails with NVAPI_INVALID_CALL, hence its
    // place right after the device5 query. Port of the NVRHI enable block
    // (External/nvrhi/src/d3d12/d3d12-device.cpp), minus the _putenv_s: here
    // the env var is the opt-in switch rather than a desc flag.
    char rtValidationEnv[16] = {};
    size_t rtValidationEnvLen = 0;
    getenv_s(&rtValidationEnvLen, rtValidationEnv, sizeof(rtValidationEnv),
        "NV_ALLOW_RAYTRACING_VALIDATION");
    if (rtValidationEnvLen > 0 && strcmp(rtValidationEnv, "1") == 0 && m_Context.device5)
    {
        m_NvapiInitialized = NvAPI_Initialize() == NVAPI_OK;
        if (m_NvapiInitialized)
        {
            const NvAPI_Status enableStatus = NvAPI_D3D12_EnableRaytracingValidation(
                m_Context.device5.Get(), NVAPI_D3D12_RAYTRACING_VALIDATION_FLAG_NONE);
            if (enableStatus == NVAPI_OK)
            {
                const NvAPI_Status registerStatus =
                    NvAPI_D3D12_RegisterRaytracingValidationMessageCallback(
                        m_Context.device5.Get(), &RaytracingValidationMessageCallback,
                        nullptr, &m_RtValidationCallbackHandle);
                if (registerStatus == NVAPI_OK)
                {
                    m_RayTracingValidationEnabled = true;
                    LogInfo("[UnityRHI] NVAPI ray tracing validation enabled.");
                }
                else
                {
                    LogError("[UnityRHI] NvAPI_D3D12_RegisterRaytracingValidationMessageCallback "
                             "failed with NvAPI_Status %d.", int(registerStatus));
                }
            }
            else
            {
                LogError("[UnityRHI] NvAPI_D3D12_EnableRaytracingValidation failed with "
                         "NvAPI_Status %d (requires an NVIDIA driver >= 551.61).", int(enableStatus));
            }
        }
        else
        {
            LogWarning("[UnityRHI] NV_ALLOW_RAYTRACING_VALIDATION=1 is set but NvAPI_Initialize "
                       "failed (non-NVIDIA GPU or driver too old); ray tracing validation disabled.");
        }
    }
#endif

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    const bool hasOptions12 = SUCCEEDED(m_Context.device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12)));
    if (hasOptions12 && options12.EnhancedBarriersSupported &&
        SUCCEEDED(m_Context.device->QueryInterface(IID_PPV_ARGS(&m_Context.device10))))
    {
        m_EnhancedBarriersSupported = true;
        LogInfo("[UnityRHI] D3D12 enhanced barriers enabled for plugin-owned resources.");
    }

    // GPU dump-file diagnostics (Agility SDK preview). No-op on stock runtimes.
    GpuDumps::Get().Initialize(m_Context.device.Get());

    if (FAILED(m_Resources.shaderResourceViewHeap.allocateResources(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, c_DefaultShaderResourceViewHeapSize, true)))
        LogError("[UnityRHI] Failed to create the CBV/SRV/UAV descriptor heap.");

    if (FAILED(m_Resources.samplerHeap.allocateResources(
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, c_DefaultSamplerHeapSize, true)))
        LogError("[UnityRHI] Failed to create the sampler descriptor heap.");

    if (FAILED(m_Resources.renderTargetViewHeap.allocateResources(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV, c_DefaultRenderTargetViewHeapSize, false)))
        LogError("[UnityRHI] Failed to create the RTV descriptor heap.");

    if (FAILED(m_Resources.depthStencilViewHeap.allocateResources(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV, c_DefaultDepthStencilViewHeapSize, false)))
        LogError("[UnityRHI] Failed to create the DSV descriptor heap.");

    D3D12_INDIRECT_ARGUMENT_DESC argumentDesc{};
    argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
    signatureDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    signatureDesc.NumArgumentDescs = 1;
    signatureDesc.pArgumentDescs = &argumentDesc;
    if (FAILED(m_Context.device->CreateCommandSignature(
            &signatureDesc, nullptr, IID_PPV_ARGS(&m_Context.dispatchIndirectSignature))))
        LogError("[UnityRHI] Failed to create dispatch-indirect command signature.");

    argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    signatureDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    if (FAILED(m_Context.device->CreateCommandSignature(
            &signatureDesc, nullptr, IID_PPV_ARGS(&m_Context.drawIndirectSignature))))
        LogError("[UnityRHI] Failed to create draw-indirect command signature.");

    argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    signatureDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    if (FAILED(m_Context.device->CreateCommandSignature(
            &signatureDesc, nullptr, IID_PPV_ARGS(&m_Context.drawIndexedIndirectSignature))))
        LogError("[UnityRHI] Failed to create draw-indexed-indirect command signature.");

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_6};
    const bool hasOptions = SUCCEEDED(m_Context.device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
    const bool hasShaderModel = SUCCEEDED(m_Context.device->CheckFeatureSupport(
        D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)));
    m_HeapDirectlyIndexedEnabled =
        hasOptions && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3 &&
        hasShaderModel && shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_6;

    if (FAILED(m_Context.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_syncPointFence))))
        LogError("[UnityRHI] Failed to create the sync-point fence.");
    m_syncPointEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Lifetime marker ring (see RecordLifetimeMarker). Committed resources
    // are zero-initialized, so unused readback slots read as instance 0.
    {
        D3D12_RESOURCE_DESC markerDesc{};
        markerDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        markerDesc.Width = sizeof(uint32_t) * kLifetimeMarkerSlotCount;
        markerDesc.Height = 1;
        markerDesc.DepthOrArraySize = 1;
        markerDesc.MipLevels = 1;
        markerDesc.SampleDesc.Count = 1;
        markerDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

        HRESULT hr = m_Context.device->CreateCommittedResource(&readbackHeap,
            D3D12_HEAP_FLAG_NONE, &markerDesc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&m_lifetimeMarkerReadback));
        if (SUCCEEDED(hr))
        {
            D3D12_RANGE readAll{0, size_t(markerDesc.Width)};
            hr = m_lifetimeMarkerReadback->Map(0, &readAll,
                reinterpret_cast<void**>(const_cast<uint32_t**>(&m_lifetimeMarkerReadbackData)));
        }
        if (SUCCEEDED(hr))
        {
            m_lifetimeMarkerReadback->SetName(L"UnityRHI Lifetime Marker Readback");
        }
        else
        {
            LogError("[UnityRHI] Failed to create the lifetime marker buffers (hr=0x%08X); "
                     "GPU lifetime retirement disabled (retention only).",
                static_cast<unsigned>(hr));
            m_lifetimeMarkerReadbackData = nullptr;
            m_lifetimeMarkerReadback.Reset();
        }
    }
}

Device::~Device()
{
#if UNITYRHI_WITH_NVAPI
    if (m_RayTracingValidationEnabled)
    {
        // Shutdown implies the GPU is idle; drain any remaining validation
        // messages (including device-removal-time ones) before unregistering.
        NvAPI_D3D12_FlushRaytracingValidationMessages(m_Context.device5.Get());
        NvAPI_D3D12_UnregisterRaytracingValidationMessageCallback(
            m_Context.device5.Get(), m_RtValidationCallbackHandle);
        m_RayTracingValidationEnabled = false;
        m_RtValidationCallbackHandle = nullptr;
    }
    if (m_NvapiInitialized)
        NvAPI_Unload();
#endif

    GpuDumps::Get().Shutdown();

    if (m_syncPointEvent)
    {
        CloseHandle(m_syncPointEvent);
        m_syncPointEvent = nullptr;
    }

    if (m_lifetimeMarkerReadback && m_lifetimeMarkerReadbackData)
    {
        D3D12_RANGE noWrite{};
        m_lifetimeMarkerReadback->Unmap(0, &noWrite);
    }
    m_lifetimeMarkerReadbackData = nullptr;

    // Flush the deferred queue unconditionally: the device is going away, so
    // waiting on fences is pointless (shutdown implies the GPU is idle). Use
    // DestroyResource rather than delete so AS/SBT-owned internal buffers are
    // released through the same NVRHI-style ownership path.
    while (!m_pendingReleases.empty())
    {
        RhiResource* resource = m_pendingReleases.back().second;
        m_pendingReleases.pop_back();
        DestroyResource(resource);
    }

    if (!m_liveResources.empty())
    {
        LogError("[UnityRHI] %zu resource(s) leaked (not Disposed on the C# side):",
            m_liveResources.size());
        for (RhiResource* resource : m_liveResources)
        {
            LogError("[UnityRHI]   leaked %s '%s'",
                KindName(resource->kind), resource->debugName.c_str());
            delete resource;
        }
        m_liveResources.clear();
    }
}

void Device::RegisterResource(RhiResource* resource)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_liveResources.insert(resource);
    m_liveCounts[uint32_t(resource->kind)]++;
}

void Device::DestroyResource(RhiResource* resource)
{
    // Accel structs and shader tables own internal buffers (AS storage, SBT
    // cache) that are registered live resources of their own - NVRHI keeps
    // them alive through RefCountPtr; here they are released together with
    // their owner. Callers hold m_Mutex, so do the bookkeeping inline.
    Buffer* internalBuffer = nullptr;
    if (resource->kind == RhiResource::Kind::AccelStruct)
        internalBuffer = static_cast<AccelStruct*>(resource)->dataBuffer;
    else if (resource->kind == RhiResource::Kind::ShaderTable)
        internalBuffer = static_cast<ShaderTable*>(resource)->cache;
    else if (resource->kind == RhiResource::Kind::StagingTexture)
        internalBuffer = static_cast<StagingTexture*>(resource)->buffer;

    if (internalBuffer && m_liveResources.erase(internalBuffer) != 0)
    {
        m_liveCounts[uint32_t(internalBuffer->kind)]--;
        const uint32_t lastUse =
            std::max(resource->lastUseFenceValue, internalBuffer->lastUseFenceValue);
        if (lastUse == 0 || lastUse <= CompletedLifetimeInstance())
            delete internalBuffer;
        else
            m_pendingReleases.emplace_back(lastUse, internalBuffer);
    }

    delete resource;
}

uint32_t Device::BeginRecordingInstance()
{
    return m_RecordingInstance.fetch_add(1, std::memory_order_relaxed) + 1;
}

void Device::RecordLifetimeMarker(ID3D12GraphicsCommandList* commandList, uint32_t instance)
{
    if (!commandList || instance == 0 || !m_lifetimeMarkerReadback)
        return;

    ComPtr<ID3D12GraphicsCommandList2> commandList2;
    const HRESULT interfaceHr = commandList->QueryInterface(IID_PPV_ARGS(&commandList2));
    if (FAILED(interfaceHr))
    {
        LogError("[UnityRHI] ID3D12GraphicsCommandList2 is required for lifetime markers (hr=0x%08X).",
            static_cast<unsigned>(interfaceHr));
        return;
    }

    std::lock_guard<std::mutex> lock(m_lifetimeMutex);
    const uint32_t slot = m_lifetimeMarkerNextSlot % kLifetimeMarkerSlotCount;
    // Reusing a slot whose previous copy has not landed yet would overwrite
    // the upload value under a pending GPU read - the readback could then
    // show an instance whose work has NOT executed. With 256 slots and a
    // couple of replays per frame that means >100 frames of GPU lag; skip the
    // marker instead (pure retention, a later marker retires this instance).
    const uint32_t previousWrite = m_lifetimeMarkerLastWritten[slot];
    if (previousWrite != 0 && m_lifetimeMarkerReadbackData[slot] < previousWrite)
        return;
    ++m_lifetimeMarkerNextSlot;
    m_lifetimeMarkerLastWritten[slot] = instance;

    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameter{};
    parameter.Dest = m_lifetimeMarkerReadback->GetGPUVirtualAddress() + slot * sizeof(uint32_t);
    parameter.Value = instance;
    const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    commandList2->WriteBufferImmediate(1, &parameter, &mode);
}

uint32_t Device::CompletedLifetimeInstance() const
{
    // Without the marker ring (creation failed, or a device without replays)
    // report "nothing retired": the callers gate upload-chunk reuse and
    // deferred destruction on this, so over-reporting hands memory the GPU
    // may still read back to the CPU side.
    if (!m_lifetimeMarkerReadbackData)
        return 0;

    std::lock_guard<std::mutex> lock(m_lifetimeMutex);
    // Slots hold monotonically increasing instances (0 = never written); the
    // GPU writes each slot once per marker, so the max over the ring is the
    // newest instance whose replay fully executed. Aligned 32-bit marker
    // writes and reads cannot tear.
    uint32_t watermark = m_completedInstanceWatermark;
    const volatile uint32_t* slots =
        reinterpret_cast<const volatile uint32_t*>(m_lifetimeMarkerReadbackData);
    for (uint32_t slot = 0; slot < kLifetimeMarkerSlotCount; ++slot)
    {
        const uint32_t value = slots[slot];
        if (value > watermark)
            watermark = value;
    }
    m_completedInstanceWatermark = watermark;
    return watermark;
}

uint64_t Device::CreateSyncPoint()
{
    const uint64_t value = m_nextSyncPointValue.fetch_add(1, std::memory_order_relaxed) + 1;
    GpuDumps::Get().RecordSyncPoint("create", value);
    return value;
}

void Device::SignalSyncPoint(uint64_t value)
{
    ID3D12CommandQueue* queue = m_unityD3D12 ? m_unityD3D12->GetCommandQueue() : nullptr;
    if (!queue || !m_syncPointFence)
    {
        LogWarning("[UnityRHI] SignalSyncPoint %llu skipped: no queue or fence.",
            static_cast<unsigned long long>(value));
        return;
    }
    queue->Signal(m_syncPointFence.Get(), value);
    GpuDumps::Get().RecordSyncPoint("signal", value);

#if UNITYRHI_WITH_NVAPI
    // Runs on Unity's submission thread after pending command lists were
    // flushed - the analogue of NVRHI's post-fence-wait flush point.
    if (m_RayTracingValidationEnabled)
        NvAPI_D3D12_FlushRaytracingValidationMessages(m_Context.device5.Get());
#endif
}

bool Device::WaitSyncPoint(uint64_t value, uint32_t timeoutMs)
{
    if (!m_syncPointFence)
        return false;
    if (m_syncPointFence->GetCompletedValue() >= value)
        return true;
    if (timeoutMs == 0)
        return false;

    // One waiter at a time: the event is auto-reset and a timed-out
    // SetEventOnCompletion registration can fire later, so re-check the
    // fence value after every wakeup.
    std::lock_guard<std::mutex> lock(m_syncPointMutex);
    const DWORD start = GetTickCount();
    while (m_syncPointFence->GetCompletedValue() < value)
    {
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs)
            return false;
        if (!m_syncPointEvent ||
            FAILED(m_syncPointFence->SetEventOnCompletion(value, m_syncPointEvent)))
            return false;
        if (WaitForSingleObject(m_syncPointEvent, timeoutMs - elapsed) != WAIT_OBJECT_0)
            return false;
    }
    return true;
}

// Port of nvrhi Sampler::Sampler (d3d12-device.cpp).
Sampler::Sampler(const Context& context, const RhiSamplerDesc& desc_)
    : RhiResource(Kind::Sampler)
    , desc(desc_)
    , m_Context(context)
{
    UINT reductionType = convertSamplerReductionType(desc.reductionType);

    if (desc.maxAnisotropy > 1.0f)
    {
        m_d3d12desc.Filter = D3D12_ENCODE_ANISOTROPIC_FILTER(reductionType);
    }
    else
    {
        m_d3d12desc.Filter = D3D12_ENCODE_BASIC_FILTER(
            desc.minFilter ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            desc.magFilter ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            desc.mipFilter ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            reductionType);
    }

    m_d3d12desc.AddressU = convertSamplerAddressMode(desc.addressU);
    m_d3d12desc.AddressV = convertSamplerAddressMode(desc.addressV);
    m_d3d12desc.AddressW = convertSamplerAddressMode(desc.addressW);

    m_d3d12desc.MipLODBias = desc.mipBias;
    m_d3d12desc.MaxAnisotropy = UINT(desc.maxAnisotropy);
    m_d3d12desc.ComparisonFunc = desc.reductionType == SamplerReductionType::Comparison
        ? D3D12_COMPARISON_FUNC_LESS
        : D3D12_COMPARISON_FUNC(0);
    m_d3d12desc.BorderColor[0] = desc.borderColor[0];
    m_d3d12desc.BorderColor[1] = desc.borderColor[1];
    m_d3d12desc.BorderColor[2] = desc.borderColor[2];
    m_d3d12desc.BorderColor[3] = desc.borderColor[3];
    m_d3d12desc.MinLOD = desc.minLod;
    m_d3d12desc.MaxLOD = desc.maxLod;
}

void Sampler::createDescriptor(size_t descriptor) const
{
    m_Context.device->CreateSampler(&m_d3d12desc, {descriptor});
}

Sampler* Device::createSampler(const RhiSamplerDesc& desc, const char* debugName)
{
    auto* sampler = new Sampler(m_Context, desc);
    sampler->debugName = debugName ? debugName : "";
    RegisterResource(sampler);
    return sampler;
}

bool Device::waitForLifetimeInstance(uint32_t instance)
{
    // Native tests have no Unity interface and explicitly wait on their own
    // queue before mapping; nothing to wait for here.
    if (instance == 0 || !m_unityD3D12)
        return true;

    // The retire marker travels inside the replay's command list, so it lands
    // once the GPU executes past the replay - there is no fence to block on,
    // only the readback ring to poll. Callers should wait on a sync point
    // before mapping (by then the marker has landed and this returns
    // immediately); the timeout turns a skipped sync point into a failed map
    // instead of a hang.
    constexpr DWORD kTimeoutMs = 10000;
    const DWORD start = GetTickCount();
    while (CompletedLifetimeInstance() < instance)
    {
        if (GetTickCount() - start >= kTimeoutMs)
            return false;
        Sleep(1);
    }
    return true;
}

uint64_t Device::getNativeObject(RhiObjectType objectType) const
{
    switch (objectType)
    {
    case RhiObjectType::D3D12_Device:
        return reinterpret_cast<uint64_t>(m_Context.device.Get());
    case RhiObjectType::Nvrhi_D3D12_Device:
        return reinterpret_cast<uint64_t>(this);
    case RhiObjectType::D3D12_CommandQueue:
        return m_unityD3D12
            ? reinterpret_cast<uint64_t>(m_unityD3D12->GetCommandQueue())
            : 0;
    default:
        return 0;
    }
}

bool Device::getTextureTiling(Texture* texture, uint32_t* numTiles,
    RhiPackedMipDesc* packedMipDesc, RhiTileShape* tileShape,
    uint32_t* subresourceTilingCount, RhiSubresourceTiling* subresourceTilings) const
{
    if (!texture || !texture->resource || !texture->desc.isTiled || !numTiles ||
        !subresourceTilingCount)
        return false;

    D3D12_PACKED_MIP_INFO nativePacked{};
    D3D12_TILE_SHAPE nativeShape{};
    const uint32_t capacity = *subresourceTilingCount;
    std::vector<D3D12_SUBRESOURCE_TILING> nativeTilings(capacity);
    UINT nativeCount = capacity;
    m_Context.device->GetResourceTiling(texture->resource.Get(), numTiles,
        packedMipDesc ? &nativePacked : nullptr, tileShape ? &nativeShape : nullptr,
        &nativeCount, 0, capacity ? nativeTilings.data() : nullptr);
    *subresourceTilingCount = nativeCount;

    if (packedMipDesc)
    {
        packedMipDesc->numStandardMips = nativePacked.NumStandardMips;
        packedMipDesc->numPackedMips = nativePacked.NumPackedMips;
        packedMipDesc->numTilesForPackedMips = nativePacked.NumTilesForPackedMips;
        packedMipDesc->startTileIndexInOverallResource = nativePacked.StartTileIndexInOverallResource;
    }
    if (tileShape)
    {
        tileShape->widthInTexels = nativeShape.WidthInTexels;
        tileShape->heightInTexels = nativeShape.HeightInTexels;
        tileShape->depthInTexels = nativeShape.DepthInTexels;
    }
    if (subresourceTilings)
    {
        for (uint32_t index = 0; index < std::min(capacity, uint32_t(nativeCount)); ++index)
        {
            subresourceTilings[index].widthInTiles = nativeTilings[index].WidthInTiles;
            subresourceTilings[index].heightInTiles = nativeTilings[index].HeightInTiles;
            subresourceTilings[index].depthInTiles = nativeTilings[index].DepthInTiles;
            subresourceTilings[index].startTileIndexInOverallResource =
                nativeTilings[index].StartTileIndexInOverallResource;
        }
    }
    return true;
}

bool Device::updateTextureTileMappings(Texture* texture, Heap* heap,
    const RhiTiledTextureCoordinate* coordinates, const RhiTiledTextureRegion* regions,
    const uint64_t* byteOffsets, uint32_t regionCount, ID3D12CommandQueue* queue)
{
    if (!texture || !texture->resource || !texture->desc.isTiled || !coordinates ||
        !regions || regionCount == 0 || (heap && (!heap->heap || !byteOffsets)))
        return false;
    if (!queue && m_unityD3D12)
        queue = m_unityD3D12->GetCommandQueue();
    if (!queue)
        return false;

    D3D12_TILE_SHAPE tileShape{};
    UINT tilingCount = 0;
    m_Context.device->GetResourceTiling(
        texture->resource.Get(), nullptr, nullptr, &tileShape, &tilingCount, 0, nullptr);
    if (tileShape.WidthInTexels == 0 || tileShape.HeightInTexels == 0 || tileShape.DepthInTexels == 0)
        return false;

    std::vector<D3D12_TILED_RESOURCE_COORDINATE> nativeCoordinates(regionCount);
    std::vector<D3D12_TILE_REGION_SIZE> nativeRegions(regionCount);
    std::vector<D3D12_TILE_RANGE_FLAGS> rangeFlags(
        regionCount, heap ? D3D12_TILE_RANGE_FLAG_NONE : D3D12_TILE_RANGE_FLAG_NULL);
    std::vector<UINT> heapStartOffsets(regionCount);
    std::vector<UINT> rangeTileCounts(regionCount);

    for (uint32_t index = 0; index < regionCount; ++index)
    {
        if (coordinates[index].mipLevel >= texture->desc.mipLevels ||
            coordinates[index].arrayLevel >= texture->desc.arraySize)
            return false;
        auto& coordinate = nativeCoordinates[index];
        coordinate.Subresource = calcSubresource(coordinates[index].mipLevel,
            coordinates[index].arrayLevel, 0, texture->desc.mipLevels, texture->desc.arraySize);
        coordinate.X = coordinates[index].x;
        coordinate.Y = coordinates[index].y;
        coordinate.Z = coordinates[index].z;

        auto& region = nativeRegions[index];
        if (regions[index].tilesNum != 0)
        {
            region.NumTiles = regions[index].tilesNum;
            region.UseBox = FALSE;
        }
        else
        {
            const uint32_t tilesX = (regions[index].width + tileShape.WidthInTexels - 1) /
                tileShape.WidthInTexels;
            const uint32_t tilesY = (regions[index].height + tileShape.HeightInTexels - 1) /
                tileShape.HeightInTexels;
            const uint32_t tilesZ = (regions[index].depth + tileShape.DepthInTexels - 1) /
                tileShape.DepthInTexels;
            if (tilesX == 0 || tilesY == 0 || tilesZ == 0 || tilesY > UINT16_MAX || tilesZ > UINT16_MAX)
                return false;
            region.Width = tilesX;
            region.Height = uint16_t(tilesY);
            region.Depth = uint16_t(tilesZ);
            region.NumTiles = tilesX * tilesY * tilesZ;
            region.UseBox = TRUE;
        }
        if (heap)
        {
            if ((byteOffsets[index] % D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) != 0)
                return false;
            heapStartOffsets[index] = uint32_t(
                byteOffsets[index] / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
        }
        rangeTileCounts[index] = region.NumTiles;
    }

    queue->UpdateTileMappings(texture->resource.Get(), regionCount,
        nativeCoordinates.data(), nativeRegions.data(), heap ? heap->heap.Get() : nullptr,
        regionCount, rangeFlags.data(), heap ? heapStartOffsets.data() : nullptr,
        rangeTileCounts.data(), D3D12_TILE_MAPPING_FLAG_NONE);
    // UpdateTileMappings executes on the queue ahead of any not-yet-submitted
    // replay, so the current recording instance (which retires strictly after
    // this queue position) is a sound last-use tag.
    if (m_unityD3D12)
        texture->lastUseFenceValue = m_RecordingInstance.load(std::memory_order_relaxed);
    return true;
}

void Device::ReleaseResource(RhiResource* resource)
{
    if (!resource)
        return;

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto live = m_liveResources.find(resource);
    if (live == m_liveResources.end())
    {
        LogError("[UnityRHI] ReleaseResource: unknown or double-released handle %p.", resource);
        return;
    }

    if (--resource->referenceCount != 0)
        return;

    m_liveResources.erase(live);

    m_liveCounts[uint32_t(resource->kind)]--;

    if (resource->lastUseFenceValue == 0 ||
        resource->lastUseFenceValue <= CompletedLifetimeInstance())
    {
        DestroyResource(resource);
    }
    else
    {
        m_pendingReleases.emplace_back(resource->lastUseFenceValue, resource);
    }
}

bool Device::RetainResources(RhiResource* const* resources, uint32_t count)
{
    if (count == 0)
        return true;
    if (!resources)
        return false;

    std::lock_guard<std::mutex> lock(m_Mutex);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!resources[i] || m_liveResources.find(resources[i]) == m_liveResources.end())
        {
            LogError("[UnityRHI] RetainResources: submission references an unknown or disposed handle %p.",
                resources[i]);
            return false;
        }
    }

    for (uint32_t i = 0; i < count; ++i)
        ++resources[i]->referenceCount;
    return true;
}

bool Device::RetainSubmissionResources(RhiResource* const* resources, uint32_t count)
{
    if (count == 0)
        return true;
    if (!resources)
        return false;

    std::lock_guard<std::mutex> lock(m_Mutex);
    for (uint32_t i = 0; i < count; ++i)
        if (!resources[i] || m_liveResources.find(resources[i]) == m_liveResources.end())
            return false;

    for (uint32_t i = 0; i < count; ++i)
        ++resources[i]->referenceCount;
    return true;
}

bool Device::IsLiveResource(const RhiResource* resource)
{
    if (!resource)
        return false;
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_liveResources.find(const_cast<RhiResource*>(resource)) != m_liveResources.end();
}

void Device::LogResourceDiagnostic(const char* role, uint32_t index,
    const RhiResource* resource)
{
    const char* safeRole = role ? role : "resource";
    if (!resource)
    {
        LogError("[UnityRHI]   %s[%u]: handle=null, live=0.", safeRole, index);
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto live = m_liveResources.find(const_cast<RhiResource*>(resource));
    if (live == m_liveResources.end())
    {
        LogError("[UnityRHI]   %s[%u]: handle=%p, live=0 (unknown, disposed, or stale).",
            safeRole, index, resource);
        return;
    }

    LogError("[UnityRHI]   %s[%u]: handle=%p, live=1, kind=%s, name='%s', refs=%u, "
             "lastUse=%u, unityOwned=%u.",
        safeRole, index, resource, KindName(resource->kind), resource->debugName.c_str(),
        resource->referenceCount, resource->lastUseFenceValue,
        resource->unityOwnedResource ? 1u : 0u);
}

void Device::ReleaseSubmissionResources(RhiResource* const* resources, uint32_t count)
{
    if (!resources)
        return;

    std::lock_guard<std::mutex> lock(m_Mutex);
    for (uint32_t i = 0; i < count; ++i)
    {
        RhiResource* resource = resources[i];
        if (!resource)
            continue;
        auto live = m_liveResources.find(resource);
        if (live == m_liveResources.end())
            continue;
        if (--resource->referenceCount != 0)
            continue;

        m_liveResources.erase(live);
        m_liveCounts[uint32_t(resource->kind)]--;
        if (resource->lastUseFenceValue == 0 ||
            resource->lastUseFenceValue <= CompletedLifetimeInstance())
            DestroyResource(resource);
        else
            m_pendingReleases.emplace_back(resource->lastUseFenceValue, resource);
    }
}

void Device::GarbageCollect()
{
#if UNITYRHI_WITH_NVAPI
    // Drain ray tracing validation messages for GPU work completed since the
    // last call; GarbageCollect runs every render event, giving per-frame
    // granularity (mirrors NVRHI's flush in runGarbageCollection).
    if (m_RayTracingValidationEnabled)
        NvAPI_D3D12_FlushRaytracingValidationMessages(m_Context.device5.Get());
#endif

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_pendingReleases.empty())
        return;

    const uint32_t completed = CompletedLifetimeInstance();
    for (size_t i = 0; i < m_pendingReleases.size();)
    {
        if (m_pendingReleases[i].first <= completed)
        {
            DestroyResource(m_pendingReleases[i].second);
            m_pendingReleases[i] = m_pendingReleases.back();
            m_pendingReleases.pop_back();
        }
        else
        {
            ++i;
        }
    }
}

void Device::ReportLiveResources()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    LogInfo("[UnityRHI] Live resources: %zu live, %zu pending release, retired instance=%llu.",
        m_liveResources.size(), m_pendingReleases.size(),
        static_cast<unsigned long long>(CompletedLifetimeInstance()));
    for (RhiResource* resource : m_liveResources)
    {
        LogInfo("[UnityRHI]   live %s '%s' (lastUseFence=%llu)",
            KindName(resource->kind), resource->debugName.c_str(),
            static_cast<unsigned long long>(resource->lastUseFenceValue));
    }
    for (auto& [fence, resource] : m_pendingReleases)
    {
        LogInfo("[UnityRHI]   pending %s '%s' (releaseFence=%llu)",
            KindName(resource->kind), resource->debugName.c_str(),
            static_cast<unsigned long long>(fence));
    }
}

HRESULT Device::CheckDeviceRemoved()
{
    const HRESULT reason = m_Context.device ? m_Context.device->GetDeviceRemovedReason() : S_OK;
    if (SUCCEEDED(reason))
        return S_OK;
    if (!m_deviceRemovedLogged.exchange(true))
    {
        // The preview device (when present) classifies the fault more precisely
        // than the DXGI reason - PAGE_FAULT vs DEVICE_HUNG vs unknown.
        const uint32_t errorCode = GpuDumps::Get().DeviceErrorCode();
        LogError("[UnityRHI] DEVICE REMOVED (0x%08X %s, deviceErrorCode=0x%X). "
                 "Breadcrumb trail, oldest first:",
            static_cast<unsigned>(reason), DeviceRemovedReasonName(reason), errorCode);
        GpuDumps::Get().RecordEvent("DEVICE REMOVED reason=0x%08X errorCode=0x%X",
            static_cast<unsigned>(reason), errorCode);
        Breadcrumbs::Get().Dump([](uint64_t index, const char* line) {
            LogError("[UnityRHI]   #%llu %s", static_cast<unsigned long long>(index), line);
        });

        WriteDredReport(m_Context.device.Get(), reason);
        LogError("[UnityRHI] DRED capture completed; see UnityRHI-Dred.log beside the Player executable.");
        // Unity treats a failed Present as fatal immediately after this render
        // event returns. Give the Agility runtime's device-error worker the
        // same completion window used by the standalone GpuDumpRepro before
        // Unity's crash handler suspends the process.
        const bool dumpCompleted = GpuDumps::Get().WaitForDumpCompletion(5000);
        const uint32_t finalErrorCode = GpuDumps::Get().DeviceErrorCode();
        LogError("[UnityRHI] GPU dump wait finished: completed=%d beginCalled=%d "
                 "endCalled=%d deviceErrorCode=0x%X.",
            dumpCompleted ? 1 : 0,
            GpuDumps::Get().BeginCalled() ? 1 : 0,
            GpuDumps::Get().EndCalled() ? 1 : 0,
            finalErrorCode);
    }
    return reason;
}

void Device::HandleDeviceError(HRESULT operationResult, const char* operation)
{
    if (SUCCEEDED(operationResult) || !m_Context.device)
        return;

    const HRESULT reason = m_Context.device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason))
        return;

    LogError("[UnityRHI] D3D12 operation '%s' observed device failure: "
             "operationHr=0x%08X removedReason=0x%08X.",
        operation ? operation : "unknown",
        static_cast<unsigned>(operationResult), static_cast<unsigned>(reason));
    CheckDeviceRemoved();
}

void Device::GetStats(RhiDeviceStats& outStats)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    outStats.liveBuffers = m_liveCounts[uint32_t(RhiResource::Kind::Buffer)];
    outStats.liveTextures = m_liveCounts[uint32_t(RhiResource::Kind::Texture)];
    outStats.liveSamplers = m_liveCounts[uint32_t(RhiResource::Kind::Sampler)];
    outStats.liveShaders = m_liveCounts[uint32_t(RhiResource::Kind::Shader)];
    outStats.liveBindingLayouts = m_liveCounts[uint32_t(RhiResource::Kind::BindingLayout)];
    outStats.liveBindingSets = m_liveCounts[uint32_t(RhiResource::Kind::BindingSet)];
    outStats.liveComputePipelines = m_liveCounts[uint32_t(RhiResource::Kind::ComputePipeline)];
    outStats.liveShaderLibraries = m_liveCounts[uint32_t(RhiResource::Kind::ShaderLibrary)];
    outStats.liveAccelStructs = m_liveCounts[uint32_t(RhiResource::Kind::AccelStruct)];
    outStats.liveRayTracingPipelines = m_liveCounts[uint32_t(RhiResource::Kind::RayTracingPipeline)];
    outStats.liveShaderTables = m_liveCounts[uint32_t(RhiResource::Kind::ShaderTable)];
    outStats.liveDescriptorTables = m_liveCounts[uint32_t(RhiResource::Kind::DescriptorTable)];
    outStats.liveInputLayouts = m_liveCounts[uint32_t(RhiResource::Kind::InputLayout)];
    outStats.liveFramebuffers = m_liveCounts[uint32_t(RhiResource::Kind::Framebuffer)];
    outStats.liveGraphicsPipelines = m_liveCounts[uint32_t(RhiResource::Kind::GraphicsPipeline)];
    outStats.liveHeaps = m_liveCounts[uint32_t(RhiResource::Kind::Heap)];
    outStats.liveStagingTextures = m_liveCounts[uint32_t(RhiResource::Kind::StagingTexture)];
    outStats.liveEventQueries = m_liveCounts[uint32_t(RhiResource::Kind::EventQuery)];
    outStats.liveTimerQueries = m_liveCounts[uint32_t(RhiResource::Kind::TimerQuery)];
    outStats.pendingReleases = uint32_t(m_pendingReleases.size());
}

uint32_t Device::GetResourceSnapshot(RhiResourceInfo* outResources, uint32_t capacity)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const uint32_t total = uint32_t(m_liveResources.size() + m_pendingReleases.size());
    if (!outResources || capacity == 0)
        return total;

    const uint32_t completedInstance = CompletedLifetimeInstance();
    uint32_t written = 0;
    auto emit = [&](RhiResource* resource, uint32_t releaseInstance, bool pending)
    {
        if (!resource || written >= capacity)
            return;

        RhiResourceInfo& info = outResources[written++];
        info = {};
        info.handle = reinterpret_cast<uint64_t>(resource);
        info.kind = uint32_t(resource->kind);
        info.lastUseInstance = resource->lastUseFenceValue;
        info.releaseInstance = releaseInstance;
        if (pending) info.flags |= RhiResourceInfo_PendingRelease;
        if (resource->unityOwnedResource) info.flags |= RhiResourceInfo_UnityOwned;
        if (resource->resource) info.flags |= RhiResourceInfo_HasNativeResource;
        std::snprintf(info.debugName, sizeof(info.debugName), "%s", resource->debugName.c_str());

        auto allocationSize = [&](const D3D12_RESOURCE_DESC& desc) -> uint64_t
        {
            if (!m_Context.device)
                return 0;
            return m_Context.device->GetResourceAllocationInfo(1, 1, &desc).SizeInBytes;
        };

        switch (resource->kind)
        {
        case RhiResource::Kind::Buffer:
        {
            auto* value = static_cast<Buffer*>(resource);
            info.logicalSize = value->desc.byteSize;
            if (value->resource) info.allocationSize = allocationSize(value->resourceDesc);
            info.format = uint32_t(value->desc.format);
            info.initialState = uint32_t(value->desc.initialState);
            info.permanentState = uint32_t(value->permanentState);
            info.detail0 = value->desc.structStride;
            info.detail1 = value->gpuVA != 0 ? 1u : 0u;
            if (value->heap) info.flags |= RhiResourceInfo_Placed;
            if (value->desc.isVirtual) info.flags |= RhiResourceInfo_Virtual;
            if (value->desc.cpuAccess == CpuAccessMode::Read) info.flags |= RhiResourceInfo_CpuRead;
            if (value->desc.cpuAccess == CpuAccessMode::Write) info.flags |= RhiResourceInfo_CpuWrite;
            if (value->desc.canHaveUAVs) info.flags |= RhiResourceInfo_UnorderedAccess;
            break;
        }
        case RhiResource::Kind::Texture:
        {
            auto* value = static_cast<Texture*>(resource);
            if (value->resource || value->desc.isVirtual)
                info.allocationSize = allocationSize(value->resourceDesc);
            info.width = value->desc.width;
            info.height = value->desc.height;
            info.depth = value->desc.depth;
            info.arraySize = value->desc.arraySize;
            info.mipLevels = value->desc.mipLevels;
            info.sampleCount = value->desc.sampleCount;
            info.format = uint32_t(value->desc.format);
            info.initialState = uint32_t(value->desc.initialState);
            info.permanentState = uint32_t(value->permanentState);
            info.detail0 = uint32_t(value->desc.dimension);
            info.detail1 = value->planeCount;
            if (value->heap) info.flags |= RhiResourceInfo_Placed;
            if (value->desc.isVirtual) info.flags |= RhiResourceInfo_Virtual;
            if (value->desc.isTiled) info.flags |= RhiResourceInfo_Tiled;
            if (value->desc.isUAV) info.flags |= RhiResourceInfo_UnorderedAccess;
            if (value->desc.isRenderTarget) info.flags |= RhiResourceInfo_RenderTarget;
            if (value->desc.isShaderResource) info.flags |= RhiResourceInfo_ShaderResource;
            break;
        }
        case RhiResource::Kind::Shader:
        {
            auto* value = static_cast<Shader*>(resource);
            info.logicalSize = value->bytecode ? value->bytecode->size() : 0;
            info.detail0 = uint32_t(value->desc.shaderType);
            break;
        }
        case RhiResource::Kind::ShaderLibrary:
        {
            auto* value = static_cast<ShaderLibrary*>(resource);
            info.logicalSize = value->bytecode ? value->bytecode->size() : 0;
            break;
        }
        case RhiResource::Kind::BindingLayout:
        {
            auto* value = static_cast<BindingLayout*>(resource);
            info.detail0 = uint32_t(value->bindings.size());
            info.detail1 = value->descriptorTableSizeSRVetc + value->descriptorTableSizeSamplers;
            break;
        }
        case RhiResource::Kind::BindingSet:
        {
            auto* value = static_cast<BindingSet*>(resource);
            info.detail0 = uint32_t(value->bindings.size());
            info.detail1 = value->descriptorTableSizeSRVetc + value->descriptorTableSizeSamplers;
            break;
        }
        case RhiResource::Kind::ComputePipeline:
            info.detail0 = uint32_t(static_cast<ComputePipeline*>(resource)->layouts.size());
            break;
        case RhiResource::Kind::InputLayout:
            info.detail0 = uint32_t(static_cast<InputLayout*>(resource)->inputElements.size());
            break;
        case RhiResource::Kind::Framebuffer:
        {
            auto* value = static_cast<Framebuffer*>(resource);
            info.width = value->width;
            info.height = value->height;
            info.sampleCount = value->sampleCount;
            info.detail0 = uint32_t(value->colorTextures.size());
            info.detail1 = value->depthTexture ? 1u : 0u;
            break;
        }
        case RhiResource::Kind::GraphicsPipeline:
            info.detail0 = uint32_t(static_cast<GraphicsPipeline*>(resource)->layouts.size());
            break;
        case RhiResource::Kind::Heap:
        {
            auto* value = static_cast<Heap*>(resource);
            info.logicalSize = value->desc.capacity;
            info.allocationSize = value->desc.capacity;
            info.detail0 = uint32_t(value->desc.type);
            break;
        }
        case RhiResource::Kind::StagingTexture:
        {
            auto* value = static_cast<StagingTexture*>(resource);
            info.logicalSize = value->buffer ? value->buffer->desc.byteSize : 0;
            info.width = value->desc.width;
            info.height = value->desc.height;
            info.depth = value->desc.depth;
            info.arraySize = value->desc.arraySize;
            info.mipLevels = value->desc.mipLevels;
            info.format = uint32_t(value->desc.format);
            info.flags |= RhiResourceInfo_InternalBacking;
            break;
        }
        case RhiResource::Kind::DescriptorTable:
        {
            auto* value = static_cast<DescriptorTable*>(resource);
            info.detail0 = value->capacity;
            info.detail1 = value->firstDescriptor;
            break;
        }
        case RhiResource::Kind::AccelStruct:
        {
            auto* value = static_cast<AccelStruct*>(resource);
            info.logicalSize = value->dataBuffer ? value->dataBuffer->desc.byteSize : 0;
            info.detail0 = value->allowUpdate ? 1u : 0u;
            info.detail1 = value->compacted ? 1u : 0u;
            info.flags |= RhiResourceInfo_InternalBacking;
            break;
        }
        case RhiResource::Kind::RayTracingPipeline:
        {
            auto* value = static_cast<RayTracingPipeline*>(resource);
            info.detail0 = uint32_t(value->exports.size());
            info.detail1 = value->maxLocalRootParameters;
            break;
        }
        case RhiResource::Kind::ShaderTable:
        {
            auto* value = static_cast<ShaderTable*>(resource);
            info.logicalSize = value->cache ? value->cache->desc.byteSize : 0;
            info.detail0 = uint32_t(value->missShaders.size() + value->hitGroups.size() + value->callableShaders.size() + 1);
            info.detail1 = value->version;
            info.flags |= RhiResourceInfo_InternalBacking;
            break;
        }
        case RhiResource::Kind::EventQuery:
            info.detail0 = static_cast<EventQuery*>(resource)->started ? 1u : 0u;
            break;
        case RhiResource::Kind::TimerQuery:
        {
            auto* value = static_cast<TimerQuery*>(resource);
            info.detail0 = value->started ? 1u : 0u;
            info.detail1 = value->resolved ? 1u : 0u;
            break;
        }
        default:
            break;
        }

        // A zero last-use instance means the GPU has never seen the resource.
        // The editor derives Idle/In Flight from this completed watermark.
        if (info.lastUseInstance != 0 && info.lastUseInstance <= completedInstance)
            info.flags |= RhiResourceInfo_GpuUseCompleted;
    };

    for (RhiResource* resource : m_liveResources)
        emit(resource, 0, false);
    for (const auto& pending : m_pendingReleases)
        emit(pending.second, pending.first, true);
    return total;
}

// Port of nvrhi Device::queryFeatureSupport (d3d12-device.cpp), reduced to
// the features this backend can express. Queue-related features are false:
// all execution goes through Unity's graphics queue.
bool Device::QueryFeatureSupport(RhiFeature feature)
{
    if (!m_Context.device)
        return false;

    switch (feature)
    {
    case RhiFeature::DeferredCommandLists:
        return true;
    case RhiFeature::ConstantBufferRanges:
        return true;
    case RhiFeature::ConservativeRasterization:
        return true;
    case RhiFeature::WaveLaneCountMinMax:
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
        return SUCCEEDED(m_Context.device->CheckFeatureSupport(
                   D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options))) &&
            options.WaveOps;
    }
    case RhiFeature::HeapDirectlyIndexed:
        return m_HeapDirectlyIndexedEnabled;
    case RhiFeature::RayTracingAccelStruct:
    case RhiFeature::RayTracingPipeline:
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
        return SUCCEEDED(m_Context.device->CheckFeatureSupport(
                   D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) &&
            options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
    }
    case RhiFeature::RayQuery:
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
        return SUCCEEDED(m_Context.device->CheckFeatureSupport(
                   D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) &&
            options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    }
    case RhiFeature::VirtualResources:
        return true;
    case RhiFeature::Meshlets:
        // Hardware support alone is insufficient: UnityRHI does not expose
        // meshlet pipelines or dispatch commands yet.
        return false;
    case RhiFeature::SamplerFeedback:
        // The enum values are retained for ABI parity, but sampler-feedback
        // resources and commands are not implemented end-to-end.
        return false;
    case RhiFeature::VariableRateShading:
        // Do not advertise VRS until GraphicsState can express and replay it.
        return false;
    // Unity owns the queues; async compute/copy are not available through
    // this backend (see PLAN.md mode B).
    case RhiFeature::EnhancedBarriers:
        return m_EnhancedBarriersSupported;
    case RhiFeature::ComputeQueue:
    case RhiFeature::CopyQueue:
        return false;
    default:
        return false;
    }
}

FormatSupport Device::QueryFormatSupport(Format format)
{
    const DxgiFormatMapping& mapping = getDxgiFormatMapping(format);
    FormatSupport result = FormatSupport::None;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = mapping.rtvFormat;
    m_Context.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));

    if (support.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER) result = result | FormatSupport::Buffer;
    if (support.Support1 & (D3D12_FORMAT_SUPPORT1_TEXTURE1D | D3D12_FORMAT_SUPPORT1_TEXTURE2D |
        D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE)) result = result | FormatSupport::Texture;
    if (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) result = result | FormatSupport::DepthStencil;
    if (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) result = result | FormatSupport::RenderTarget;
    if (support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) result = result | FormatSupport::Blendable;

    if (mapping.srvFormat != support.Format)
    {
        support = {};
        support.Format = mapping.srvFormat;
        m_Context.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    }
    if (support.Support1 & D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER) result = result | FormatSupport::IndexBuffer;
    if (support.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER) result = result | FormatSupport::VertexBuffer;
    if (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) result = result | FormatSupport::ShaderLoad;
    if (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) result = result | FormatSupport::ShaderSample;
    if (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) result = result | FormatSupport::ShaderUavLoad;
    if (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) result = result | FormatSupport::ShaderUavStore;
    if (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD) result = result | FormatSupport::ShaderAtomic;
    return result;
}

Heap* Device::createHeap(const HeapDesc& d)
{
    D3D12_HEAP_DESC nativeDesc{};
    nativeDesc.SizeInBytes = d.capacity;
    nativeDesc.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
    nativeDesc.Properties.CreationNodeMask = 1;
    nativeDesc.Properties.VisibleNodeMask = 1;

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const bool tier1 = SUCCEEDED(m_Context.device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
        options.ResourceHeapTier == D3D12_RESOURCE_HEAP_TIER_1;
    nativeDesc.Flags = tier1
        ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
        : D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

    switch (d.type)
    {
    case HeapType::DeviceLocal: nativeDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT; break;
    case HeapType::Upload: nativeDesc.Properties.Type = D3D12_HEAP_TYPE_UPLOAD; break;
    case HeapType::Readback: nativeDesc.Properties.Type = D3D12_HEAP_TYPE_READBACK; break;
    default:
        LogError("[UnityRHI] createHeap '%s': invalid heap type %u.",
            d.debugName.c_str(), uint32_t(d.type));
        return nullptr;
    }

    auto* heap = new Heap();
    heap->desc = d;
    heap->debugName = d.debugName;
    const HRESULT hr = m_Context.device->CreateHeap(&nativeDesc, IID_PPV_ARGS(&heap->heap));
    if (FAILED(hr))
    {
        LogError("[UnityRHI] CreateHeap call failed for heap '%s' (hr=0x%08X, %llu bytes).",
            heap->debugName.c_str(), unsigned(hr), static_cast<unsigned long long>(d.capacity));
        HandleDeviceError(hr, "Create heap");
        delete heap;
        return nullptr;
    }
    SetDebugName(heap->heap.Get(), d.debugName.c_str());
    RegisterResource(heap);
    return heap;
}

void Device::RequestResourceState(ID3D12GraphicsCommandList* unityCommandList, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES state)
{
    ForwardRequestResourceState(m_unityD3D12, unityCommandList, resource, state);
}

void Device::NotifyResourceState(ID3D12GraphicsCommandList* unityCommandList, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES state, bool uavAccess)
{
    ForwardNotifyResourceState(m_unityD3D12, unityCommandList, resource, state, uavAccess);
}
} // namespace unityrhi
