#include "DlssNr.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <directx/d3d12.h>

#include "nvsdk_ngx.h"

#include "CommandStreamWire.h"
#include "DlssNrRuntime.h"
#include "UnityRhiLog.h"
#include "d3d12/d3d12-backend.h"

namespace unityrhi
{
namespace
{
std::atomic<int32_t> g_LastCreateResult{int32_t(NVSDK_NGX_Result_Fail)};
std::atomic<int32_t> g_LastEvaluateResult{int32_t(NVSDK_NGX_Result_Fail)};

ID3D12Resource* GetResource(uint64_t handle)
{
    auto* texture = reinterpret_cast<Texture*>(uintptr_t(handle));
    return texture && texture->resource ? texture->resource.Get() : nullptr;
}

// nvngx_dlssnr.dll uses the NGX 0x15 parameter ABI. The pinned public SDK in
// this repository predates that layout, so its C++ overloads cannot be called
// on parameter maps allocated by the snippet. These slots are recovered from
// the signed snippet's caller and kept local to the experimental integration.
void** ParameterVtable(NVSDK_NGX_Parameter* parameters)
{
    return *reinterpret_cast<void***>(parameters);
}

void SetUll(NVSDK_NGX_Parameter* p, const char* name, unsigned long long value)
{
    using Fn = void(NVSDK_CONV*)(NVSDK_NGX_Parameter*, const char*, unsigned long long);
    reinterpret_cast<Fn>(ParameterVtable(p)[0])(p, name, value);
}

void SetResource(NVSDK_NGX_Parameter* p, const char* name, ID3D12Resource* value)
{
    using Fn = void(NVSDK_CONV*)(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*);
    reinterpret_cast<Fn>(ParameterVtable(p)[1])(p, name, value);
}

void SetUi(NVSDK_NGX_Parameter* p, const char* name, unsigned int value)
{
    using Fn = void(NVSDK_CONV*)(NVSDK_NGX_Parameter*, const char*, unsigned int);
    reinterpret_cast<Fn>(ParameterVtable(p)[3])(p, name, value);
}

void SetI(NVSDK_NGX_Parameter* p, const char* name, int value)
{
    using Fn = void(NVSDK_CONV*)(NVSDK_NGX_Parameter*, const char*, int);
    reinterpret_cast<Fn>(ParameterVtable(p)[4])(p, name, value);
}

void SetF(NVSDK_NGX_Parameter* p, const char* name, float value)
{
    using Fn = void(NVSDK_CONV*)(NVSDK_NGX_Parameter*, const char*, float);
    reinterpret_cast<Fn>(ParameterVtable(p)[6])(p, name, value);
}

NVSDK_NGX_Result GetUi(NVSDK_NGX_Parameter* p, const char* name, unsigned int* value)
{
    using Fn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*, const char*, unsigned int*);
    return reinterpret_cast<Fn>(ParameterVtable(p)[11])(p, name, value);
}

NVSDK_NGX_Result NVSDK_CONV ComputeScalingRatio(NVSDK_NGX_Parameter* parameters)
{
    if (!parameters)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    unsigned int upscaling = 0;
    GetUi(parameters, "DLSSNR.Upscaling", &upscaling);
    SetF(parameters, "DLSSNR.ScalingRatio", upscaling ? 0.5f : 1.0f);
    return NVSDK_NGX_Result_Success;
}

struct PendingFeatureRelease
{
    NVSDK_NGX_Handle* handle = nullptr;
    uint32_t lastUseInstance = 0;
    int32_t instanceId = 0;
};

uint32_t CompletedLifetime()
{
    Device* device = Device::Get();
    return device ? device->CompletedLifetimeInstance() : 0;
}

uint32_t CurrentLifetime()
{
    Device* device = Device::Get();
    return device ? device->CurrentRecordingInstance() : 0;
}

bool LifetimeRetired(uint32_t lastUseInstance)
{
    return lastUseInstance == 0 || CompletedLifetime() >= lastUseInstance;
}

void ReleaseNgxHandle(NVSDK_NGX_Handle* handle, int32_t instanceId)
{
    if (!handle)
        return;
    const int32_t result = ReleaseDlssNrFeature(handle);
    if (NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(result)))
        LogWarning("[UnityRHI.DLSSNR] Release failed for instance %d (result=0x%08X).",
            instanceId, unsigned(result));
}

class DlssNrInstance
{
public:
    explicit DlssNrInstance(int32_t id)
        : m_Id(id), m_Parameters(AllocateDlssNrParameters()) {}
    ~DlssNrInstance()
    {
        RetireHandle(false);
        DestroyDlssNrParameters(m_Parameters);
    }
    bool IsValid() const { return m_Parameters != nullptr; }
    bool Dispatch(const wire::DlssNrDispatchPayload& command,
        ID3D12GraphicsCommandList* commandList);
    void RetireHandle(bool force);

    static void FlushPendingReleases(bool force);
    static void QueuePendingRelease(NVSDK_NGX_Handle* handle, uint32_t lastUseInstance,
        int32_t instanceId);

private:
    bool Create(const wire::DlssNrDispatchPayload& command,
        ID3D12GraphicsCommandList* commandList);

    int32_t m_Id;
    uint16_t m_InputWidth = 0, m_InputHeight = 0;
    uint16_t m_OutputWidth = 0, m_OutputHeight = 0;
    uint8_t m_Upscaling = 0, m_Preset = 0;
    uint32_t m_LastUseInstance = 0;
    NVSDK_NGX_Handle* m_Handle = nullptr;
    NVSDK_NGX_Parameter* m_Parameters = nullptr;
    static std::vector<PendingFeatureRelease> s_PendingReleases;
};

std::vector<PendingFeatureRelease> DlssNrInstance::s_PendingReleases;

void DlssNrInstance::FlushPendingReleases(bool force)
{
    if (s_PendingReleases.empty())
        return;
    const uint32_t completed = CompletedLifetime();
    size_t write = 0;
    for (size_t read = 0; read < s_PendingReleases.size(); ++read)
    {
        PendingFeatureRelease pending = s_PendingReleases[read];
        if (force || pending.lastUseInstance == 0 || completed >= pending.lastUseInstance)
        {
            ReleaseNgxHandle(pending.handle, pending.instanceId);
            continue;
        }
        s_PendingReleases[write++] = pending;
    }
    s_PendingReleases.resize(write);
}

void DlssNrInstance::QueuePendingRelease(NVSDK_NGX_Handle* handle, uint32_t lastUseInstance,
    int32_t instanceId)
{
    if (!handle)
        return;
    s_PendingReleases.push_back({handle, lastUseInstance, instanceId});
    LogInfo("[UnityRHI.DLSSNR] Deferred feature 18 release for instance %d until lifetime %u retires.",
        instanceId, unsigned(lastUseInstance));
}

void DlssNrInstance::RetireHandle(bool force)
{
    if (!m_Handle)
        return;
    if (!force && !LifetimeRetired(m_LastUseInstance))
        QueuePendingRelease(m_Handle, m_LastUseInstance, m_Id);
    else
        ReleaseNgxHandle(m_Handle, m_Id);
    m_Handle = nullptr;
}

bool DlssNrInstance::Create(const wire::DlssNrDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList)
{
    FlushPendingReleases(false);
    // NGX ReleaseFeature is not safe while a previous Evaluate is still on the
    // GPU. Keep the old handle alive until its recording instance retires, and
    // create the replacement immediately so preset/size changes do not hitch.
    RetireHandle(false);
    m_LastUseInstance = 0;
    m_InputWidth = command.inputWidth;
    m_InputHeight = command.inputHeight;
    m_OutputWidth = command.outputWidth;
    m_OutputHeight = command.outputHeight;
    m_Upscaling = command.upscaling;
    m_Preset = command.preset;

    auto setI = [this](const char* name, int value) { SetI(m_Parameters, name, value); };
    setI("Width", m_InputWidth);
    setI("Height", m_InputHeight);
    setI("OutWidth", m_OutputWidth);
    setI("OutHeight", m_OutputHeight);
    setI("DLSSNR.Width", m_OutputWidth);
    setI("DLSSNR.Height", m_OutputHeight);
    setI("DLSSNR.InputWidth", m_InputWidth);
    setI("DLSSNR.InputHeight", m_InputHeight);
    setI("DLSSNR.OutputWidth", m_OutputWidth);
    setI("DLSSNR.OutputHeight", m_OutputHeight);
    setI("DLSSNR.Output.Width", m_OutputWidth);
    setI("DLSSNR.Output.Height", m_OutputHeight);
    setI("DLSSNR.Hint.Render.Preset", m_Preset);
    setI("CreationNodeMask", 1);
    setI("VisibilityNodeMask", 1);
    SetUi(m_Parameters, "DLSS.Output.Subrect.Base.X", 0);
    SetUi(m_Parameters, "DLSS.Output.Subrect.Base.Y", 0);
    SetUi(m_Parameters, "DLSSNR.Upscaling", m_Upscaling != 0);
    SetF(m_Parameters, "DLSSNR.Scale", m_Upscaling ? 0.5f : 1.0f);
    SetF(m_Parameters, "DLSSNR.ScalingRatio", m_Upscaling ? 0.5f : 1.0f);
    SetUll(m_Parameters, "DLSSNRComputeScalingRatioCallback",
        reinterpret_cast<unsigned long long>(&ComputeScalingRatio));

    const int32_t result = CreateDlssNrFeature(commandList, m_Parameters, &m_Handle);
    g_LastCreateResult.store(result, std::memory_order_relaxed);
    if (NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(result)) || !m_Handle)
    {
        LogError("[UnityRHI.DLSSNR] Feature 18 create failed for instance %d "
                 "(result=0x%08X, %ux%u -> %ux%u, upscaling=%u, preset=%u).",
            m_Id, unsigned(result), unsigned(m_InputWidth), unsigned(m_InputHeight),
            unsigned(m_OutputWidth), unsigned(m_OutputHeight),
            unsigned(m_Upscaling), unsigned(m_Preset));
        m_Handle = nullptr;
        return false;
    }
    LogInfo("[UnityRHI.DLSSNR] Created feature 18 instance %d (%ux%u -> %ux%u).",
        m_Id, unsigned(m_InputWidth), unsigned(m_InputHeight),
        unsigned(m_OutputWidth), unsigned(m_OutputHeight));
    return true;
}

bool DlssNrInstance::Dispatch(const wire::DlssNrDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList)
{
    if (!commandList || !m_Parameters || !command.inputWidth || !command.inputHeight ||
        !command.outputWidth || !command.outputHeight)
        return false;
    ID3D12Resource* color = GetResource(command.color);
    ID3D12Resource* output = GetResource(command.output);
    ID3D12Resource* motionVectors = GetResource(command.motionVectors);
    ID3D12Resource* depth = GetResource(command.depth);
    if (!color || !output || !motionVectors || !depth || color == output)
    {
        LogError("[UnityRHI.DLSSNR] Instance %d requires four valid resources and distinct color/output.", m_Id);
        return false;
    }
    if (!m_Handle || m_InputWidth != command.inputWidth ||
        m_InputHeight != command.inputHeight || m_OutputWidth != command.outputWidth ||
        m_OutputHeight != command.outputHeight || m_Upscaling != command.upscaling ||
        m_Preset != command.preset)
    {
        if (!Create(command, commandList))
        {
            commandList->CopyResource(output, color);
            return true;
        }
    }

    auto setI = [this](const char* name, int value) { SetI(m_Parameters, name, value); };
    SetResource(m_Parameters, "DLSSNR.Color", color);
    SetResource(m_Parameters, "DLSSNR.Output", output);
    SetResource(m_Parameters, "DLSSNR.MVec", motionVectors);
    SetResource(m_Parameters, "DLSSNR.Depth", depth);
    setI("Width", command.inputWidth);
    setI("Height", command.inputHeight);
    setI("OutWidth", command.outputWidth);
    setI("OutHeight", command.outputHeight);
    const char* inputs[] = {"Color", "Depth", "MVec"};
    for (const char* input : inputs)
    {
        std::string prefix = std::string("DLSSNR.") + input + "Subrect";
        setI((prefix + "BaseX").c_str(), 0);
        setI((prefix + "BaseY").c_str(), 0);
        setI((prefix + "Width").c_str(), command.inputWidth);
        setI((prefix + "Height").c_str(), command.inputHeight);
    }
    setI("DLSSNR.OutputSubrectBaseX", 0);
    setI("DLSSNR.OutputSubrectBaseY", 0);
    setI("DLSSNR.OutputSubrectWidth", command.outputWidth);
    setI("DLSSNR.OutputSubrectHeight", command.outputHeight);
    SetF(m_Parameters, "DLSSNR.MVecScaleX", command.motionVectorScaleX);
    SetF(m_Parameters, "DLSSNR.MVecScaleY", command.motionVectorScaleY);
    SetUi(m_Parameters, "DLSSNR.DepthInverted", command.depthInverted != 0);
    SetUi(m_Parameters, "DLSSNR.Enabled", 1);
    SetUi(m_Parameters, "DLSSNR.Reset", command.reset != 0);
    SetF(m_Parameters, "DLSSNR.Intensity", command.intensity);
    SetF(m_Parameters, "DLSSNR.LocalToneStrength", command.localToneStrength);
    SetF(m_Parameters, "DLSSNR.LocalStructureStrength", command.localStructureStrength);
    SetF(m_Parameters, "DLSSNR.SkinStructureStrength", command.skinStructureStrength);
    SetUi(m_Parameters, "DLSSNR.UseAutoMask", command.useAutoMask != 0);
    SetI(m_Parameters, "DLSSNR.Style", command.style);
    SetUi(m_Parameters, "DLSSNR.UICorrection", command.uiCorrection != 0);

    const int32_t result = EvaluateDlssNrFeature(commandList, m_Handle, m_Parameters);
    g_LastEvaluateResult.store(result, std::memory_order_relaxed);
    if (NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(result)))
    {
        LogError("[UnityRHI.DLSSNR] Evaluate failed for instance %d (result=0x%08X).",
            m_Id, unsigned(result));
        // Keep the display path valid: post already wrote LDR into Color, and the
        // engine presents Output. Without this copy the screen shows uncleared UAV garbage.
        commandList->CopyResource(output, color);
        m_LastUseInstance = CurrentLifetime();
        return true;
    }
    m_LastUseInstance = CurrentLifetime();
    return true;
}

class DlssNrSystem
{
public:
    bool Initialize(ID3D12Device* device)
    {
        std::scoped_lock lock(m_Mutex);
        m_Device = device;
        return InitializeDlssNrRuntime(device);
    }
    void Shutdown()
    {
        std::scoped_lock lock(m_Mutex);
        for (auto& [id, instance] : m_Instances)
        {
            (void)id;
            if (instance)
                instance->RetireHandle(true);
        }
        m_Instances.clear();
        DlssNrInstance::FlushPendingReleases(true);
        ShutdownDlssNrRuntime();
        m_Device = nullptr;
        m_NextId = 1;
    }
    void Tick()
    {
        std::scoped_lock lock(m_Mutex);
        DlssNrInstance::FlushPendingReleases(false);
    }
    int32_t CreateInstance()
    {
        std::scoped_lock lock(m_Mutex);
        if (!m_Device || !IsDlssNrRuntimeAvailable())
            return 0;
        const int32_t id = m_NextId++;
        auto instance = std::make_unique<DlssNrInstance>(id);
        if (!instance->IsValid())
            return 0;
        m_Instances.emplace(id, std::move(instance));
        return id;
    }
    void DestroyInstance(int32_t id)
    {
        std::scoped_lock lock(m_Mutex);
        m_Instances.erase(id);
    }
    bool Dispatch(const wire::DlssNrDispatchPayload& command,
        ID3D12GraphicsCommandList* commandList)
    {
        std::scoped_lock lock(m_Mutex);
        const auto it = m_Instances.find(command.instanceId);
        return it != m_Instances.end() && it->second->Dispatch(command, commandList);
    }
private:
    std::mutex m_Mutex;
    ID3D12Device* m_Device = nullptr;
    std::unordered_map<int32_t, std::unique_ptr<DlssNrInstance>> m_Instances;
    int32_t m_NextId = 1;
};

DlssNrSystem g_DlssNr;
}

bool InitializeDlssNr(ID3D12Device* device) { return g_DlssNr.Initialize(device); }
void ShutdownDlssNr() { g_DlssNr.Shutdown(); }
void TickDlssNr() { g_DlssNr.Tick(); }
int32_t CreateDlssNrInstance() { return g_DlssNr.CreateInstance(); }
void DestroyDlssNrInstance(int32_t instanceId) { g_DlssNr.DestroyInstance(instanceId); }
bool DispatchDlssNr(const wire::DlssNrDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList) { return g_DlssNr.Dispatch(command, commandList); }
int32_t DlssNrLastCreateResult() { return g_LastCreateResult.load(std::memory_order_relaxed); }
int32_t DlssNrLastEvaluateResult() { return g_LastEvaluateResult.load(std::memory_order_relaxed); }
}
