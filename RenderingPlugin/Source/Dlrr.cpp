#include "Dlrr.h"

#include <memory>
#include <mutex>
#include <unordered_map>

#include <directx/d3d12.h>

#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_dlssd.h"

#include "CommandStreamWire.h"
#include "NgxRuntime.h"
#include "UnityRhiLog.h"
#include "d3d12/d3d12-backend.h"

namespace unityrhi
{
namespace
{
constexpr unsigned int kNodeMask = 1;

enum class DlrrMode : uint8_t
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
    switch (static_cast<DlrrMode>(mode))
    {
    case DlrrMode::Native: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case DlrrMode::UltraQuality: return NVSDK_NGX_PerfQuality_Value_UltraQuality;
    case DlrrMode::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case DlrrMode::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case DlrrMode::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    default: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
}

const char* GetPresetParameter(uint8_t mode)
{
    switch (static_cast<DlrrMode>(mode))
    {
    case DlrrMode::Native:
        return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA;
    case DlrrMode::UltraQuality:
        return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality;
    case DlrrMode::Quality:
        return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality;
    case DlrrMode::Balanced:
        return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced;
    case DlrrMode::Performance:
        return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance;
    default:
        return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance;
    }
}

ID3D12Resource* GetResource(uint64_t handle)
{
    auto* texture = reinterpret_cast<Texture*>(uintptr_t(handle));
    return texture && texture->resource ? texture->resource.Get() : nullptr;
}

class DlrrInstance
{
public:
    explicit DlrrInstance(int32_t id)
        : m_Id(id), m_Parameters(CreateNgxFeatureParameters())
    {
    }

    ~DlrrInstance()
    {
        ReleaseFeature();
        DestroyNgxFeatureParameters(m_Parameters);
    }

    bool IsValid() const { return m_Parameters != nullptr; }
    bool Dispatch(const wire::DlrrDispatchPayload& command,
        ID3D12GraphicsCommandList* commandList);

private:
    void ReleaseFeature()
    {
        if (!m_Handle)
            return;
        std::scoped_lock ngxLock(NgxMutex());
        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_ReleaseFeature(m_Handle);
        if (NVSDK_NGX_FAILED(result))
            LogWarning("[UnityRHI.DLRR] Failed to release instance %d (result=0x%08X).",
                m_Id, unsigned(result));
        m_Handle = nullptr;
    }

    bool CreateFeature(const wire::DlrrDispatchPayload& command,
        ID3D12GraphicsCommandList* commandList);

    int32_t m_Id = 0;
    uint16_t m_OutputWidth = 0;
    uint16_t m_OutputHeight = 0;
    uint8_t m_Mode = 0;
    uint8_t m_Preset = 0;
    NVSDK_NGX_Handle* m_Handle = nullptr;
    NVSDK_NGX_Parameter* m_Parameters = nullptr;
};

bool DlrrInstance::CreateFeature(const wire::DlrrDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList)
{
    ReleaseFeature();

    m_OutputWidth = command.outputWidth;
    m_OutputHeight = command.outputHeight;
    m_Mode = command.upscalerMode;
    m_Preset = command.preset;

    const NVSDK_NGX_PerfQuality_Value quality = GetPerfQuality(m_Mode);
    unsigned int renderWidth = command.currentWidth;
    unsigned int renderHeight = command.currentHeight;
    unsigned int maxWidth = 0, maxHeight = 0, minWidth = 0, minHeight = 0;
    float sharpness = 0.0f;

    std::scoped_lock ngxLock(NgxMutex());
    // Prefer driver-reported dimensions, matching the former NRI path. Fall
    // back to the actual render size when the callback is unavailable.
    const NVSDK_NGX_Result settingsResult = NGX_DLSSD_GET_OPTIMAL_SETTINGS(
        m_Parameters, m_OutputWidth, m_OutputHeight, quality,
        &renderWidth, &renderHeight, &maxWidth, &maxHeight,
        &minWidth, &minHeight, &sharpness);
    if (NVSDK_NGX_FAILED(settingsResult) || renderWidth == 0 || renderHeight == 0)
    {
        renderWidth = command.currentWidth;
        renderHeight = command.currentHeight;
    }

    NVSDK_NGX_Parameter_SetUI(m_Parameters, GetPresetParameter(m_Mode), m_Preset);

    NVSDK_NGX_DLSSD_Create_Params params{};
    params.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    params.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    params.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
    params.InWidth = renderWidth;
    params.InHeight = renderHeight;
    params.InTargetWidth = m_OutputWidth;
    params.InTargetHeight = m_OutputHeight;
    params.InPerfQualityValue = quality;
    params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
        NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

    const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSSD_EXT(
        commandList, kNodeMask, kNodeMask, &m_Handle, m_Parameters, &params);
    if (NVSDK_NGX_FAILED(result) || !m_Handle)
    {
        LogError("[UnityRHI.DLRR] Direct NGX create failed for instance %d "
                 "(result=0x%08X, render=%ux%u, output=%ux%u, mode=%u, preset=%u).",
            m_Id, unsigned(result), renderWidth, renderHeight,
            unsigned(m_OutputWidth), unsigned(m_OutputHeight),
            unsigned(m_Mode), unsigned(m_Preset));
        m_Handle = nullptr;
        return false;
    }

    LogInfo("[UnityRHI.DLRR] Created direct NGX instance %d at %ux%u -> %ux%u "
            "(mode=%u, preset=%u).",
        m_Id, renderWidth, renderHeight,
        unsigned(m_OutputWidth), unsigned(m_OutputHeight),
        unsigned(m_Mode), unsigned(m_Preset));
    return true;
}

bool DlrrInstance::Dispatch(const wire::DlrrDispatchPayload& command,
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
    ID3D12Resource* diffuseAlbedo = GetResource(command.diffuseAlbedo);
    ID3D12Resource* specularAlbedo = GetResource(command.specularAlbedo);
    ID3D12Resource* normalRoughness = GetResource(command.normalRoughness);
    ID3D12Resource* specularMotion = GetResource(command.specularMotion);
    if (!input || !output || !motionVectors || !depth || !diffuseAlbedo ||
        !specularAlbedo || !normalRoughness || !specularMotion)
    {
        LogError("[UnityRHI.DLRR] Instance %d has an invalid direct-NGX resource.", m_Id);
        return false;
    }

    if (!m_Handle || m_OutputWidth != command.outputWidth ||
        m_OutputHeight != command.outputHeight || m_Mode != command.upscalerMode ||
        m_Preset != command.preset)
    {
        if (!CreateFeature(command, commandList))
            return false;
    }

    NVSDK_NGX_D3D12_DLSSD_Eval_Params params{};
    params.pInColor = input;
    params.pInOutput = output;
    params.pInMotionVectors = motionVectors;
    params.pInDepth = depth;
    // Packed normal/roughness mode consumes roughness from the alpha channel.
    params.pInNormals = normalRoughness;
    params.pInDiffuseAlbedo = diffuseAlbedo;
    params.pInSpecularAlbedo = specularAlbedo;
    params.InJitterOffsetX = command.cameraJitterX;
    params.InJitterOffsetY = command.cameraJitterY;
    params.InRenderSubrectDimensions = {command.currentWidth, command.currentHeight};
    params.InMVScaleX = 1.0f;
    params.InMVScaleY = 1.0f;
    if (command.useSpecularMotionVector)
    {
        params.pInMotionVectorsReflections = specularMotion;
    }
    else
    {
        params.pInSpecularHitDistance = specularMotion;
        params.pInWorldToViewMatrix = const_cast<float*>(command.worldToViewMatrix);
        params.pInViewToClipMatrix = const_cast<float*>(command.viewToClipMatrix);
    }

    std::scoped_lock ngxLock(NgxMutex());
    const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSSD_EXT(
        commandList, m_Handle, m_Parameters, &params);
    if (NVSDK_NGX_FAILED(result))
    {
        LogError("[UnityRHI.DLRR] Direct NGX evaluate failed for instance %d "
                 "(result=0x%08X).", m_Id, unsigned(result));
        return false;
    }
    return true;
}

class DlrrSystem
{
public:
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue*)
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
        if (!m_Device || !IsNgxInitialized() || !NgxDlrrAvailable())
            return 0;
        const int32_t id = m_NextInstanceId++;
        auto instance = std::make_unique<DlrrInstance>(id);
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

    bool Dispatch(const wire::DlrrDispatchPayload& command,
        ID3D12GraphicsCommandList* commandList)
    {
        std::scoped_lock lock(m_Mutex);
        const auto it = m_Instances.find(command.instanceId);
        return it != m_Instances.end() && it->second->Dispatch(command, commandList);
    }

private:
    std::mutex m_Mutex;
    ID3D12Device* m_Device = nullptr;
    std::unordered_map<int32_t, std::unique_ptr<DlrrInstance>> m_Instances;
    int32_t m_NextInstanceId = 1;
};

DlrrSystem g_Dlrr;
}

bool InitializeDlrr(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue)
{
    return g_Dlrr.Initialize(device, graphicsQueue);
}

void ShutdownDlrr()
{
    g_Dlrr.Shutdown();
}

int32_t CreateDlrrInstance()
{
    return g_Dlrr.CreateInstance();
}

void DestroyDlrrInstance(int32_t instanceId)
{
    g_Dlrr.DestroyInstance(instanceId);
}

bool DispatchDlrr(const wire::DlrrDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList)
{
    return g_Dlrr.Dispatch(command, commandList);
}
}
