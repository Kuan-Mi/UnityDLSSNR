#include "FrameGenerationDebug.h"

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

#include "FrameGenerationSwapChain.h"
#include "NgxRuntime.h"
#include "UnityRhiLog.h"
#include "nvsdk_ngx_helpers_dlssg.h"

namespace unityrhi
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr uint32_t kCommandContextCount = 6;
constexpr uint32_t kRealSlotCount = 2;

struct CommandContext
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    uint64_t fenceValue = 0;
};

struct FgState
{
    std::mutex mutex;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12Resource> motionVectors;
    ComPtr<ID3D12Resource> output;
    // Ping-pong retained real frames so the next Evaluate can run while the
    // pacer still copies/presents the previous real frame.
    std::array<ComPtr<ID3D12Resource>, kRealSlotCount> outputReal;
    std::array<bool, kRealSlotCount> outputRealIsCopySource{{false, false}};
    uint32_t outputRealWriteIndex = 0;
    uint32_t pacerRealSlot = UINT32_MAX;
    std::array<CommandContext, kCommandContextCount> commandContexts;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValue = 0;
    uint32_t nextCommandContext = 0;

    FrameGenerationInputs inputs{};
    bool hasInputs = false;
    bool outputIsShaderResource = false;
    bool forceReset = true;
    uint64_t lastEvaluatedFrame = UINT64_MAX;

    NVSDK_NGX_Handle* feature = nullptr;
    NVSDK_NGX_Parameter* parameters = nullptr;
    uint32_t featureColorWidth = 0;
    uint32_t featureColorHeight = 0;
    uint32_t featureRenderWidth = 0;
    uint32_t featureRenderHeight = 0;
    DXGI_FORMAT featureFormat = DXGI_FORMAT_UNKNOWN;

    std::atomic<bool> enabled{false};
};

FgState g_Fg;
std::atomic<uint64_t> g_DisplayedPresentCount{0};
std::atomic<uint64_t> g_GeneratedPresentCount{0};
std::atomic<uint64_t> g_RealPresentCount{0};

D3D12_RESOURCE_BARRIER Transition(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

bool FenceValueCompleteLocked(uint64_t fenceValue)
{
    return fenceValue == 0 ||
           (g_Fg.fence && g_Fg.fence->GetCompletedValue() >= fenceValue);
}

bool WaitForFenceValueLocked(uint64_t fenceValue)
{
    if (FenceValueCompleteLocked(fenceValue))
        return true;
    if (FAILED(g_Fg.fence->SetEventOnCompletion(fenceValue, g_Fg.fenceEvent)))
        return false;
    return WaitForSingleObject(g_Fg.fenceEvent, 5000) == WAIT_OBJECT_0;
}

// Waits without holding g_Fg.mutex so SubmitFrameGenerationInputs is not blocked
// for the duration of a GPU fence.
bool WaitForFenceValueUnlocked(uint64_t fenceValue)
{
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    {
        std::lock_guard lock(g_Fg.mutex);
        if (FenceValueCompleteLocked(fenceValue))
            return true;
        fence = g_Fg.fence;
        event = g_Fg.fenceEvent;
    }
    if (!fence || !event)
        return false;
    if (fence->GetCompletedValue() >= fenceValue)
        return true;
    if (FAILED(fence->SetEventOnCompletion(fenceValue, event)))
        return false;
    return WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
}

void WaitForAllWorkLocked()
{
    WaitForFenceValueLocked(g_Fg.fenceValue);
}

CommandContext* AcquireCommandContextLocked()
{
    for (uint32_t offset = 0; offset < kCommandContextCount; ++offset)
    {
        const uint32_t index = (g_Fg.nextCommandContext + offset) % kCommandContextCount;
        auto& context = g_Fg.commandContexts[index];
        if (!FenceValueCompleteLocked(context.fenceValue))
            continue;
        g_Fg.nextCommandContext = (index + 1) % kCommandContextCount;
        return &context;
    }
    return nullptr;
}

void ReleaseFeatureLocked()
{
    if (g_Fg.feature)
    {
        std::lock_guard ngxLock(NgxMutex());
        NVSDK_NGX_D3D12_ReleaseFeature(g_Fg.feature);
        g_Fg.feature = nullptr;
    }
    if (g_Fg.parameters)
    {
        DestroyNgxFeatureParameters(g_Fg.parameters);
        g_Fg.parameters = nullptr;
    }
    g_Fg.output.Reset();
    for (uint32_t i = 0; i < kRealSlotCount; ++i)
    {
        g_Fg.outputReal[i].Reset();
        g_Fg.outputRealIsCopySource[i] = false;
    }
    g_Fg.outputRealWriteIndex = 0;
    g_Fg.pacerRealSlot = UINT32_MAX;
    g_Fg.outputIsShaderResource = false;
    g_Fg.featureColorWidth = 0;
    g_Fg.featureColorHeight = 0;
    g_Fg.featureRenderWidth = 0;
    g_Fg.featureRenderHeight = 0;
    g_Fg.featureFormat = DXGI_FORMAT_UNKNOWN;
}

bool SupportsTypedUavStore(ID3D12Device* device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    return SUCCEEDED(device->CheckFeatureSupport(
               D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

bool EnsureFeatureLocked(
    ID3D12GraphicsCommandList* commandList,
    const D3D12_RESOURCE_DESC& backbufferDesc)
{
    const auto& in = g_Fg.inputs;
    const DXGI_FORMAT format = backbufferDesc.Format;
    const bool matches = g_Fg.feature && g_Fg.output &&
        g_Fg.outputReal[0] && g_Fg.outputReal[1] &&
        g_Fg.featureColorWidth == backbufferDesc.Width &&
        g_Fg.featureColorHeight == backbufferDesc.Height &&
        g_Fg.featureRenderWidth == in.renderWidth &&
        g_Fg.featureRenderHeight == in.renderHeight &&
        g_Fg.featureFormat == format;
    if (matches)
        return true;

    WaitForAllWorkLocked();
    ReleaseFeatureLocked();
    if (!SupportsTypedUavStore(g_Fg.device.Get(), format))
    {
        LogError("[UnityRHI][DLSS-G] Backbuffer format %u does not support typed UAV stores.",
            static_cast<unsigned>(format));
        return false;
    }

    D3D12_RESOURCE_DESC outputDesc = backbufferDesc;
    outputDesc.Alignment = 0;
    outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    outputDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.SampleDesc.Quality = 0;
    const D3D12_HEAP_PROPERTIES heap{
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN,
        0,
        0};
    HRESULT hr = g_Fg.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &outputDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&g_Fg.output));
    if (FAILED(hr))
    {
        LogError("[UnityRHI][DLSS-G] Failed to create interpolated-frame texture (0x%08X).",
            static_cast<unsigned>(hr));
        return false;
    }
    g_Fg.output->SetName(L"UnityRHI DLSS-G Interpolated Frame");

    for (uint32_t i = 0; i < kRealSlotCount; ++i)
    {
        hr = g_Fg.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &outputDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&g_Fg.outputReal[i]));
        if (FAILED(hr))
        {
            LogError("[UnityRHI][DLSS-G] Failed to create retained real-frame texture %u (0x%08X).",
                i, static_cast<unsigned>(hr));
            ReleaseFeatureLocked();
            return false;
        }
        g_Fg.outputReal[i]->SetName(i == 0
            ? L"UnityRHI DLSS-G Retained Real Frame 0"
            : L"UnityRHI DLSS-G Retained Real Frame 1");
        g_Fg.outputRealIsCopySource[i] = false;
    }

    g_Fg.parameters = CreateNgxFeatureParameters();
    if (!g_Fg.parameters)
    {
        LogError("[UnityRHI][DLSS-G] Failed to allocate feature parameters.");
        ReleaseFeatureLocked();
        return false;
    }

    NVSDK_NGX_DLSSG_Create_Params create{};
    create.Width = static_cast<unsigned>(backbufferDesc.Width);
    create.Height = backbufferDesc.Height;
    create.NativeBackbufferFormat = static_cast<unsigned>(format);
    create.RenderWidth = in.renderWidth;
    create.RenderHeight = in.renderHeight;
    create.DynamicResolutionScaling = false;

    NVSDK_NGX_Result result;
    {
        std::lock_guard ngxLock(NgxMutex());
        result = NGX_D3D12_CREATE_DLSSG(
            commandList, 1, 1, &g_Fg.feature, g_Fg.parameters, &create);
    }
    if (NVSDK_NGX_FAILED(result))
    {
        LogError("[UnityRHI][DLSS-G] Feature creation failed (0x%08X).",
            static_cast<unsigned>(result));
        ReleaseFeatureLocked();
        return false;
    }

    g_Fg.featureColorWidth = static_cast<uint32_t>(backbufferDesc.Width);
    g_Fg.featureColorHeight = backbufferDesc.Height;
    g_Fg.featureRenderWidth = in.renderWidth;
    g_Fg.featureRenderHeight = in.renderHeight;
    g_Fg.featureFormat = format;
    g_Fg.outputIsShaderResource = false;
    g_Fg.outputRealWriteIndex = 0;
    g_Fg.pacerRealSlot = UINT32_MAX;
    g_Fg.forceReset = true;
    LogInfo("[UnityRHI][DLSS-G] Created NGX frame-generation feature: color=%ux%u format=%u render=%ux%u (2x real buffers).",
        g_Fg.featureColorWidth, g_Fg.featureColorHeight,
        static_cast<unsigned>(format), in.renderWidth, in.renderHeight);
    return true;
}

void CopyMatrix(float (&destination)[4][4], const float* source)
{
    std::memcpy(destination, source, sizeof(destination));
}
} // namespace

bool InitializeFrameGeneration(ID3D12Device* device)
{
    if (!device)
        return false;
    std::lock_guard lock(g_Fg.mutex);
    if (g_Fg.device.Get() == device)
        return true;

    g_Fg.device = device;
    HRESULT hr = S_OK;
    for (auto& context : g_Fg.commandContexts)
    {
        hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&context.allocator));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                context.allocator.Get(), nullptr, IID_PPV_ARGS(&context.commandList));
        if (SUCCEEDED(hr))
            hr = context.commandList->Close();
        if (FAILED(hr))
            break;
    }
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fg.fence));
    if (SUCCEEDED(hr))
        g_Fg.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !g_Fg.fenceEvent)
    {
        if (SUCCEEDED(hr))
            hr = HRESULT_FROM_WIN32(GetLastError());
        LogError("[UnityRHI][DLSS-G] Failed to initialize frame-generation command context (0x%08X).",
            static_cast<unsigned>(hr));
        for (auto& context : g_Fg.commandContexts)
        {
            context.commandList.Reset();
            context.allocator.Reset();
        }
        g_Fg.fence.Reset();
        g_Fg.device.Reset();
        return false;
    }
    LogInfo("[UnityRHI][DLSS-G] Frame generation initialized with %u command contexts.",
        kCommandContextCount);
    return true;
}

void ShutdownFrameGeneration()
{
    std::lock_guard lock(g_Fg.mutex);
    if (g_Fg.fence)
        WaitForAllWorkLocked();
    ReleaseFeatureLocked();
    g_Fg.depth.Reset();
    g_Fg.motionVectors.Reset();
    for (auto& context : g_Fg.commandContexts)
    {
        context.commandList.Reset();
        context.allocator.Reset();
        context.fenceValue = 0;
    }
    g_Fg.fence.Reset();
    g_Fg.device.Reset();
    if (g_Fg.fenceEvent)
    {
        CloseHandle(g_Fg.fenceEvent);
        g_Fg.fenceEvent = nullptr;
    }
    g_Fg.fenceValue = 0;
    g_Fg.nextCommandContext = 0;
    g_Fg.hasInputs = false;
    g_Fg.forceReset = true;
    g_Fg.lastEvaluatedFrame = UINT64_MAX;
    g_Fg.enabled.store(false, std::memory_order_release);
}

void SetFrameGenerationEnabled(bool enabled)
{
    const bool wasEnabled = g_Fg.enabled.exchange(enabled, std::memory_order_acq_rel);
    if (enabled == wasEnabled)
        return;
    ResetFrameGenerationPacing();
    if (enabled)
    {
        std::lock_guard lock(g_Fg.mutex);
        g_Fg.forceReset = true;
    }
    LogInfo("[UnityRHI][DLSS-G] Frame generation %s.", enabled ? "enabled" : "disabled");
}

bool IsFrameGenerationEnabled()
{
    return g_Fg.enabled.load(std::memory_order_acquire);
}

void NoteDisplayedPresent()
{
    g_DisplayedPresentCount.fetch_add(1, std::memory_order_relaxed);
}

void NoteGeneratedPresent()
{
    g_DisplayedPresentCount.fetch_add(1, std::memory_order_relaxed);
    g_GeneratedPresentCount.fetch_add(1, std::memory_order_relaxed);
}

void NoteRealPresent()
{
    g_DisplayedPresentCount.fetch_add(1, std::memory_order_relaxed);
    g_RealPresentCount.fetch_add(1, std::memory_order_relaxed);
}

uint64_t GetDisplayedPresentCount()
{
    return g_DisplayedPresentCount.load(std::memory_order_relaxed);
}

uint64_t GetGeneratedPresentCount()
{
    return g_GeneratedPresentCount.load(std::memory_order_relaxed);
}

uint64_t GetRealPresentCount()
{
    return g_RealPresentCount.load(std::memory_order_relaxed);
}

uint32_t ConsumeFrameGenerationPacerRealSlot()
{
    std::lock_guard lock(g_Fg.mutex);
    const uint32_t slot = g_Fg.pacerRealSlot;
    g_Fg.pacerRealSlot = UINT32_MAX;
    return slot;
}

void SubmitFrameGenerationInputs(const FrameGenerationInputs& inputs)
{
    if (!inputs.depth || !inputs.motionVectors)
        return;
    std::lock_guard lock(g_Fg.mutex);
    g_Fg.depth = inputs.depth;
    g_Fg.motionVectors = inputs.motionVectors;
    g_Fg.inputs = inputs;
    g_Fg.inputs.depth = g_Fg.depth.Get();
    g_Fg.inputs.motionVectors = g_Fg.motionVectors.Get();
    g_Fg.hasInputs = true;
}

FrameGenerationPresentAction EvaluateFrameGeneration(
    IDXGISwapChain3* presentSwapChain, ID3D12CommandQueue* presentQueue,
    ID3D12Resource* colorBuffer)
{
    if (!IsFrameGenerationEnabled() || !presentSwapChain || !presentQueue ||
        !IsNgxInitialized() || NgxFrameGenerationAvailable() != 1)
        return FrameGenerationPresentAction::None;

    for (int acquireAttempt = 0; acquireAttempt < 4; ++acquireAttempt)
    {
        uint64_t waitFence = 0;
        {
            std::lock_guard lock(g_Fg.mutex);
            if (!g_Fg.device || !g_Fg.hasInputs || !g_Fg.fence ||
                g_Fg.inputs.frameId == g_Fg.lastEvaluatedFrame)
                return FrameGenerationPresentAction::None;

            CommandContext* context = AcquireCommandContextLocked();
            if (!context)
            {
                waitFence = g_Fg.commandContexts[g_Fg.nextCommandContext].fenceValue;
            }
            else
            {
                ComPtr<ID3D12Resource> color;
                ComPtr<ID3D12Resource> presentTarget;
                HRESULT hr = S_OK;
                if (colorBuffer)
                {
                    color = colorBuffer;
                    const UINT index = presentSwapChain->GetCurrentBackBufferIndex();
                    hr = presentSwapChain->GetBuffer(index, IID_PPV_ARGS(&presentTarget));
                }
                else
                {
                    const UINT index = presentSwapChain->GetCurrentBackBufferIndex();
                    hr = presentSwapChain->GetBuffer(index, IID_PPV_ARGS(&color));
                    presentTarget = color;
                }
                if (FAILED(hr) || !color || !presentTarget)
                    return FrameGenerationPresentAction::None;
                const D3D12_RESOURCE_DESC backbufferDesc = color->GetDesc();
                if (backbufferDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                    backbufferDesc.Width != g_Fg.inputs.colorWidth ||
                    backbufferDesc.Height != g_Fg.inputs.colorHeight)
                    return FrameGenerationPresentAction::None;

                hr = context->allocator->Reset();
                if (SUCCEEDED(hr))
                    hr = context->commandList->Reset(context->allocator.Get(), nullptr);
                if (FAILED(hr))
                    return FrameGenerationPresentAction::None;

                ID3D12GraphicsCommandList* commandList = context->commandList.Get();
                if (!EnsureFeatureLocked(commandList, backbufferDesc))
                {
                    commandList->Close();
                    return FrameGenerationPresentAction::None;
                }

                const uint32_t realSlot = g_Fg.outputRealWriteIndex % kRealSlotCount;
                ID3D12Resource* realResource = g_Fg.outputReal[realSlot].Get();

                D3D12_RESOURCE_BARRIER before[5];
                UINT beforeCount = 0;
                before[beforeCount++] = Transition(color.Get(),
                    D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                before[beforeCount++] = Transition(g_Fg.depth.Get(),
                    static_cast<D3D12_RESOURCE_STATES>(g_Fg.inputs.depthState),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                before[beforeCount++] = Transition(g_Fg.motionVectors.Get(),
                    static_cast<D3D12_RESOURCE_STATES>(g_Fg.inputs.motionVectorsState),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (g_Fg.outputIsShaderResource)
                    before[beforeCount++] = Transition(g_Fg.output.Get(),
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (g_Fg.outputRealIsCopySource[realSlot])
                    before[beforeCount++] = Transition(realResource,
                        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                commandList->ResourceBarrier(beforeCount, before);

                NVSDK_NGX_Parameter_SetULL(g_Fg.parameters,
                    NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,
                    static_cast<unsigned long long>(g_Fg.inputs.frameId));

                NVSDK_NGX_D3D12_DLSSG_Eval_Params eval{};
                eval.pBackbuffer = color.Get();
                eval.pDepth = g_Fg.depth.Get();
                eval.pMVecs = g_Fg.motionVectors.Get();
                eval.pOutputInterpFrame = g_Fg.output.Get();
                eval.pOutputRealFrame = realResource;

                NVSDK_NGX_DLSSG_Opt_Eval_Params opt{};
                opt.multiFrameCount = 1;
                opt.multiFrameIndex = 1;
                CopyMatrix(opt.cameraViewToClip, g_Fg.inputs.cameraViewToClip);
                CopyMatrix(opt.clipToCameraView, g_Fg.inputs.clipToCameraView);
                CopyMatrix(opt.clipToPrevClip, g_Fg.inputs.clipToPrevClip);
                CopyMatrix(opt.prevClipToClip, g_Fg.inputs.prevClipToClip);
                for (unsigned i = 0; i < 4; ++i)
                    opt.clipToLensClip[i][i] = 1.0f;
                opt.jitterOffset[0] = -g_Fg.inputs.jitterX;
                opt.jitterOffset[1] = -g_Fg.inputs.jitterY;
                opt.mvecScale[0] = g_Fg.inputs.motionVectorScaleX;
                opt.mvecScale[1] = g_Fg.inputs.motionVectorScaleY;
                std::memcpy(opt.cameraPos, g_Fg.inputs.cameraPos, sizeof(opt.cameraPos));
                std::memcpy(opt.cameraUp, g_Fg.inputs.cameraUp, sizeof(opt.cameraUp));
                std::memcpy(opt.cameraRight, g_Fg.inputs.cameraRight, sizeof(opt.cameraRight));
                std::memcpy(opt.cameraFwd, g_Fg.inputs.cameraFwd, sizeof(opt.cameraFwd));
                opt.cameraNear = g_Fg.inputs.cameraNear;
                opt.cameraFar = g_Fg.inputs.cameraFar;
                opt.cameraFOV = g_Fg.inputs.cameraFov;
                opt.cameraAspectRatio = g_Fg.inputs.cameraAspect;
                opt.colorBuffersHDR = g_Fg.inputs.colorBuffersHdr != 0;
                opt.depthInverted = g_Fg.inputs.depthInverted != 0;
                opt.cameraMotionIncluded = g_Fg.inputs.cameraMotionIncluded != 0;
                opt.reset = g_Fg.forceReset || g_Fg.inputs.reset != 0;
                opt.mvecsSubrectSize = {g_Fg.inputs.renderWidth, g_Fg.inputs.renderHeight};
                opt.depthSubrectSize = {g_Fg.inputs.renderWidth, g_Fg.inputs.renderHeight};
                opt.backbufferSubrectSize = {g_Fg.inputs.colorWidth, g_Fg.inputs.colorHeight};

                NVSDK_NGX_Result result;
                {
                    std::lock_guard ngxLock(NgxMutex());
                    result = NGX_D3D12_EVALUATE_DLSSG(
                        commandList, g_Fg.feature, g_Fg.parameters, &eval, &opt);
                }
                if (NVSDK_NGX_FAILED(result))
                {
                    LogError("[UnityRHI][DLSS-G] Evaluate failed for frame %llu (0x%08X).",
                        static_cast<unsigned long long>(g_Fg.inputs.frameId),
                        static_cast<unsigned>(result));
                    commandList->Close();
                    return FrameGenerationPresentAction::None;
                }

                const bool copyOntoColor = presentTarget.Get() == color.Get();
                D3D12_RESOURCE_BARRIER after[5];
                UINT afterCount = 0;
                after[afterCount++] = Transition(color.Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    copyOntoColor ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_PRESENT);
                after[afterCount++] = Transition(g_Fg.depth.Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    static_cast<D3D12_RESOURCE_STATES>(g_Fg.inputs.depthState));
                after[afterCount++] = Transition(g_Fg.motionVectors.Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    static_cast<D3D12_RESOURCE_STATES>(g_Fg.inputs.motionVectorsState));
                after[afterCount++] = Transition(g_Fg.output.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                after[afterCount++] = Transition(realResource,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                commandList->ResourceBarrier(afterCount, after);

                if (!copyOntoColor)
                {
                    D3D12_RESOURCE_BARRIER destBefore[] = {
                        Transition(presentTarget.Get(),
                            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                    };
                    commandList->ResourceBarrier(1, destBefore);
                    commandList->CopyResource(presentTarget.Get(), g_Fg.output.Get());
                }
                else
                {
                    commandList->CopyResource(color.Get(), g_Fg.output.Get());
                }

                D3D12_RESOURCE_BARRIER finalBarriers[2];
                UINT finalBarrierCount = 0;
                finalBarriers[finalBarrierCount++] = Transition(g_Fg.output.Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (!copyOntoColor)
                    finalBarriers[finalBarrierCount++] = Transition(presentTarget.Get(),
                        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                else
                    finalBarriers[finalBarrierCount++] = Transition(color.Get(),
                        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
                commandList->ResourceBarrier(finalBarrierCount, finalBarriers);
                hr = commandList->Close();
                if (FAILED(hr))
                    return FrameGenerationPresentAction::None;

                ID3D12CommandList* lists[] = {commandList};
                presentQueue->ExecuteCommandLists(1, lists);
                const uint64_t signalValue = ++g_Fg.fenceValue;
                context->fenceValue = signalValue;
                hr = presentQueue->Signal(g_Fg.fence.Get(), signalValue);
                if (FAILED(hr))
                    return FrameGenerationPresentAction::None;

                // No CPU wait: Present is queued on the same graphics queue after
                // this Execute, so the GPU orders Evaluate/copy before the flip.
                g_Fg.outputIsShaderResource = true;
                g_Fg.outputRealIsCopySource[realSlot] = true;
                g_Fg.pacerRealSlot = realSlot;
                g_Fg.outputRealWriteIndex = (realSlot + 1) % kRealSlotCount;
                g_Fg.lastEvaluatedFrame = g_Fg.inputs.frameId;
                g_Fg.forceReset = false;
                return FrameGenerationPresentAction::Insert;
            }
        }

        if (waitFence == 0 || !WaitForFenceValueUnlocked(waitFence))
            return FrameGenerationPresentAction::None;
    }
    return FrameGenerationPresentAction::None;
}

bool CopyFrameGenerationRetainedReal(
    IDXGISwapChain3* swapChain, ID3D12CommandQueue* presentQueue, uint32_t realSlot)
{
    if (!swapChain || !presentQueue || realSlot >= kRealSlotCount)
        return false;

    for (int acquireAttempt = 0; acquireAttempt < 8; ++acquireAttempt)
    {
        uint64_t waitFence = 0;
        {
            std::lock_guard lock(g_Fg.mutex);
            if (!g_Fg.device || !g_Fg.outputReal[realSlot] ||
                !g_Fg.outputRealIsCopySource[realSlot] || !g_Fg.fence)
                return false;

            CommandContext* context = AcquireCommandContextLocked();
            if (!context)
            {
                waitFence = g_Fg.commandContexts[g_Fg.nextCommandContext].fenceValue;
            }
            else
            {
                ComPtr<ID3D12Resource> backbuffer;
                const UINT index = swapChain->GetCurrentBackBufferIndex();
                HRESULT hr = swapChain->GetBuffer(index, IID_PPV_ARGS(&backbuffer));
                if (FAILED(hr) || !backbuffer)
                    return false;
                const D3D12_RESOURCE_DESC backbufferDesc = backbuffer->GetDesc();
                if (backbufferDesc.Width != g_Fg.featureColorWidth ||
                    backbufferDesc.Height != g_Fg.featureColorHeight ||
                    backbufferDesc.Format != g_Fg.featureFormat)
                    return false;

                hr = context->allocator->Reset();
                if (SUCCEEDED(hr))
                    hr = context->commandList->Reset(context->allocator.Get(), nullptr);
                if (FAILED(hr))
                    return false;

                ID3D12GraphicsCommandList* commandList = context->commandList.Get();
                D3D12_RESOURCE_BARRIER before[] = {
                    Transition(backbuffer.Get(),
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                };
                commandList->ResourceBarrier(1, before);
                commandList->CopyResource(backbuffer.Get(), g_Fg.outputReal[realSlot].Get());
                D3D12_RESOURCE_BARRIER after[] = {
                    Transition(backbuffer.Get(),
                        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
                };
                commandList->ResourceBarrier(1, after);
                hr = commandList->Close();
                if (FAILED(hr))
                    return false;

                ID3D12CommandList* lists[] = {commandList};
                presentQueue->ExecuteCommandLists(1, lists);
                const uint64_t signalValue = ++g_Fg.fenceValue;
                context->fenceValue = signalValue;
                hr = presentQueue->Signal(g_Fg.fence.Get(), signalValue);
                // No CPU wait: the following Present on this queue is ordered after the copy.
                return SUCCEEDED(hr);
            }
        }

        if (waitFence == 0 || !WaitForFenceValueUnlocked(waitFence))
            return false;
    }
    return false;
}
} // namespace unityrhi
