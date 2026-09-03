#include "Dlss.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <directx/d3d12.h>

#include "nvsdk_ngx_helpers.h"

#include "CommandStreamWire.h"
#include "NgxRuntime.h"
#include "UnityRhiLog.h"
#include "d3d12/d3d12-backend.h"

namespace unityrhi
{
namespace
{
constexpr unsigned int kNodeMask = 1;
std::atomic<int32_t> g_LastCreateResult{int32_t(NVSDK_NGX_Result_Fail)};
std::atomic<int32_t> g_LastEvaluateResult{int32_t(NVSDK_NGX_Result_Fail)};

enum class DlssMode : uint8_t
{
    Native,
    UltraQuality,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
};

NVSDK_NGX_PerfQuality_Value GetPerfQuality(uint8_t mode)
{
    switch (static_cast<DlssMode>(mode))
    {
    case DlssMode::Native: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case DlssMode::UltraQuality: return NVSDK_NGX_PerfQuality_Value_UltraQuality;
    case DlssMode::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case DlssMode::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case DlssMode::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    default: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
}

const char* GetPresetParameter(uint8_t mode)
{
    switch (static_cast<DlssMode>(mode))
    {
    case DlssMode::Native:
        return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
    case DlssMode::UltraQuality:
        return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality;
    case DlssMode::Quality:
        return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
    case DlssMode::Balanced:
        return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
    case DlssMode::Performance:
        return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
    default:
        return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
    }
}

ID3D12Resource* GetResource(uint64_t handle)
{
    auto* texture = reinterpret_cast<Texture*>(uintptr_t(handle));
    return texture && texture->resource ? texture->resource.Get() : nullptr;
}

class DlssInstance
{
public:
    explicit DlssInstance(int32_t id)
        : m_Id(id), m_Parameters(CreateNgxFeatureParameters())
    {
    }

    ~DlssInstance()
    {
        ReleaseFeature();
        DestroyNgxFeatureParameters(m_Parameters);
    }

    bool IsValid() const { return m_Parameters != nullptr; }
    bool Dispatch(const wire::DlssDispatchPayload& command,
        ID3D12GraphicsCommandList* commandList);

private:
    void ReleaseFeature()
    {
        if (!m_Handle)
            return;
        std::scoped_lock ngxLock(NgxMutex());
        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_ReleaseFeature(m_Handle);
        if (NVSDK_NGX_FAILED(result))
            LogWarning("[UnityRHI.DLSS] Failed to release instance %d (result=0x%08X).",
                m_Id, unsigned(result));
        m_Handle = nullptr;
    }

    bool CreateFeature(const wire::DlssDispatchPayload& command,
        ID3D12GraphicsCommandList* commandList);

    int32_t m_Id = 0;
    uint16_t m_OutputWidth = 0;
    uint16_t m_OutputHeight = 0;
    uint16_t m_CurrentWidth = 0;
    uint16_t m_CurrentHeight = 0;
    uint8_t m_Mode = 0;
    uint8_t m_Preset = 0;
    uint8_t m_DepthInverted = 1;
    NVSDK_NGX_Handle* m_Handle = nullptr;
    NVSDK_NGX_Parameter* m_Parameters = nullptr;
};

bool QueryOptimalRenderSize(NVSDK_NGX_Parameter* parameters, uint32_t outputWidth,
    uint32_t outputHeight, uint8_t upscalerMode, uint32_t* renderWidth,
    uint32_t* renderHeight)
{
    if (!parameters || !renderWidth || !renderHeight || outputWidth == 0 ||
        outputHeight == 0)
        return false;

    if (static_cast<DlssMode>(upscalerMode) == DlssMode::Native)
    {
        *renderWidth = outputWidth;
        *renderHeight = outputHeight;
        return true;
    }

    unsigned int optimalW = 0;
    unsigned int optimalH = 0;
    unsigned int maxWidth = 0, maxHeight = 0, minWidth = 0, minHeight = 0;
    float sharpness = 0.0f;
    const NVSDK_NGX_PerfQuality_Value quality = GetPerfQuality(upscalerMode);
    std::scoped_lock ngxLock(NgxMutex());
    const NVSDK_NGX_Result settingsResult = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        parameters, outputWidth, outputHeight, quality,
        &optimalW, &optimalH, &maxWidth, &maxHeight,
        &minWidth, &minHeight, &sharpness);
    if (NVSDK_NGX_FAILED(settingsResult) || optimalW == 0 || optimalH == 0)
        return false;

    *renderWidth = optimalW;
    *renderHeight = optimalH;
    return true;
}

bool DlssInstance::CreateFeature(const wire::DlssDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList)
{
    ReleaseFeature();

    m_OutputWidth = command.outputWidth;
    m_OutputHeight = command.outputHeight;
    m_CurrentWidth = command.currentWidth;
    m_CurrentHeight = command.currentHeight;
    m_Mode = command.upscalerMode;
    m_Preset = command.preset;
    m_DepthInverted = command.depthInverted;

    // Use the already-negotiated render size from URP. Do not re-query optimal
    // settings here — that can drift from the buffers URP actually rendered.
    const unsigned int renderWidth = command.currentWidth;
    const unsigned int renderHeight = command.currentHeight;
    const NVSDK_NGX_PerfQuality_Value quality = GetPerfQuality(m_Mode);

    std::scoped_lock ngxLock(NgxMutex());
    NVSDK_NGX_Parameter_SetUI(m_Parameters, GetPresetParameter(m_Mode), m_Preset);

    NVSDK_NGX_DLSS_Create_Params params{};
    params.Feature.InWidth = renderWidth;
    params.Feature.InHeight = renderHeight;
    params.Feature.InTargetWidth = m_OutputWidth;
    params.Feature.InTargetHeight = m_OutputHeight;
    params.Feature.InPerfQualityValue = quality;
    params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (m_DepthInverted)
        params.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

    const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(
        commandList, kNodeMask, kNodeMask, &m_Handle, m_Parameters, &params);
    g_LastCreateResult.store(int32_t(result), std::memory_order_relaxed);
    if (NVSDK_NGX_FAILED(result) || !m_Handle)
    {
        LogError("[UnityRHI.DLSS] NGX create failed for instance %d "
                 "(result=0x%08X, render=%ux%u, output=%ux%u, mode=%u, preset=%u).",
            m_Id, unsigned(result), renderWidth, renderHeight,
            unsigned(m_OutputWidth), unsigned(m_OutputHeight),
            unsigned(m_Mode), unsigned(m_Preset));
        m_Handle = nullptr;
        return false;
    }

    LogInfo("[UnityRHI.DLSS] Created NGX SuperSampling instance %d at %ux%u -> %ux%u "
            "(mode=%u, preset=%u).",
        m_Id, renderWidth, renderHeight,
        unsigned(m_OutputWidth), unsigned(m_OutputHeight),
        unsigned(m_Mode), unsigned(m_Preset));
    return true;
}

bool DlssInstance::Dispatch(const wire::DlssDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList)
{
    if (!commandList || !m_Parameters || command.outputWidth == 0 ||
        command.outputHeight == 0 || command.currentWidth == 0 ||
        command.currentHeight == 0)
        return false;

    ID3D12Resource* input = GetResource(command.input);
    ID3D12Resource* output = GetResource(command.output);
    ID3D12Resource* motionVectors = GetResource(command.motionVectors);
    ID3D12Resource* depth = GetResource(command.depth);
    if (!input || !output || !motionVectors || !depth || input == output)
    {
        LogError("[UnityRHI.DLSS] Instance %d requires four valid resources and distinct color/output.",
            m_Id);
        return false;
    }

    if (!m_Handle || m_OutputWidth != command.outputWidth ||
        m_OutputHeight != command.outputHeight ||
        m_CurrentWidth != command.currentWidth ||
        m_CurrentHeight != command.currentHeight ||
        m_Mode != command.upscalerMode || m_Preset != command.preset ||
        m_DepthInverted != command.depthInverted)
    {
        if (!CreateFeature(command, commandList))
        {
            if (command.currentWidth == command.outputWidth &&
                command.currentHeight == command.outputHeight)
                commandList->CopyResource(output, input);
            return true;
        }
    }

    NVSDK_NGX_D3D12_DLSS_Eval_Params params{};
    params.Feature.pInColor = input;
    params.Feature.pInOutput = output;
    params.pInMotionVectors = motionVectors;
    params.pInDepth = depth;
    params.InJitterOffsetX = command.cameraJitterX;
    params.InJitterOffsetY = command.cameraJitterY;
    params.InRenderSubrectDimensions = {command.currentWidth, command.currentHeight};
    params.InMVScaleX = command.motionVectorScaleX;
    params.InMVScaleY = command.motionVectorScaleY;
    params.InReset = command.reset ? 1 : 0;
    // Unity D3D render targets are Y-flipped vs NGX's default indicator orientation.
    params.InIndicatorInvertYAxis = 1;

    std::scoped_lock ngxLock(NgxMutex());
    const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(
        commandList, m_Handle, m_Parameters, &params);
    g_LastEvaluateResult.store(int32_t(result), std::memory_order_relaxed);
    if (NVSDK_NGX_FAILED(result))
    {
        LogError("[UnityRHI.DLSS] NGX evaluate failed for instance %d (result=0x%08X).",
            m_Id, unsigned(result));
        if (command.currentWidth == command.outputWidth &&
            command.currentHeight == command.outputHeight)
            commandList->CopyResource(output, input);
        return true;
    }
    return true;
}

class DlssSystem
{
public:
    bool Initialize(ID3D12Device* device)
    {
        std::scoped_lock lock(m_Mutex);
        m_Device = device;
        return m_Device != nullptr;
    }

    void Shutdown()
    {
        std::scoped_lock lock(m_Mutex);
        m_Instances.clear();
        m_Device = nullptr;
        m_NextInstanceId = 1;
    }

    int32_t CreateInstance()
    {
        std::scoped_lock lock(m_Mutex);
        if (!m_Device || !IsNgxInitialized() || !NgxDlssAvailable())
            return 0;
        const int32_t id = m_NextInstanceId++;
        auto instance = std::make_unique<DlssInstance>(id);
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

    bool Dispatch(const wire::DlssDispatchPayload& command,
        ID3D12GraphicsCommandList* commandList)
    {
        std::scoped_lock lock(m_Mutex);
        const auto it = m_Instances.find(command.instanceId);
        return it != m_Instances.end() && it->second->Dispatch(command, commandList);
    }

private:
    std::mutex m_Mutex;
    ID3D12Device* m_Device = nullptr;
    std::unordered_map<int32_t, std::unique_ptr<DlssInstance>> m_Instances;
    int32_t m_NextInstanceId = 1;
};

DlssSystem g_Dlss;
}

bool InitializeDlss(ID3D12Device* device)
{
    return g_Dlss.Initialize(device);
}

void ShutdownDlss()
{
    g_Dlss.Shutdown();
}

int32_t CreateDlssInstance()
{
    return g_Dlss.CreateInstance();
}

void DestroyDlssInstance(int32_t instanceId)
{
    g_Dlss.DestroyInstance(instanceId);
}

bool DispatchDlss(const wire::DlssDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList)
{
    return g_Dlss.Dispatch(command, commandList);
}

int32_t DlssLastCreateResult()
{
    return g_LastCreateResult.load(std::memory_order_relaxed);
}

int32_t DlssLastEvaluateResult()
{
    return g_LastEvaluateResult.load(std::memory_order_relaxed);
}

int32_t QueryDlssOptimalSettings(uint32_t outputWidth, uint32_t outputHeight,
    uint8_t upscalerMode, uint32_t* renderWidth, uint32_t* renderHeight)
{
    if (!IsNgxInitialized() || !NgxDlssAvailable() || !renderWidth || !renderHeight)
        return 0;

    NVSDK_NGX_Parameter* parameters = CreateNgxFeatureParameters();
    if (!parameters)
        return 0;

    const bool ok = QueryOptimalRenderSize(parameters, outputWidth, outputHeight,
        upscalerMode, renderWidth, renderHeight);
    DestroyNgxFeatureParameters(parameters);
    return ok ? 1 : 0;
}
}
