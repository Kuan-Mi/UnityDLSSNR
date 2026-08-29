#include "NgxRuntime.h"

#include <array>
#include <string>
#include <type_traits>

#include <windows.h>
#include <directx/d3d12.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs_dlssd.h"
#include "nvsdk_ngx_defs_dlssg.h"

#include "UnityRhiLog.h"

namespace unityrhi
{
namespace
{
// Stable identifier for this open-source UnityRHI integration. A GUID-like
// project ID is the supported initialization path when no NVIDIA app ID has
// been assigned.
constexpr char kProjectId[] = "e40890d0-da76-467b-8130-12f3ada8d7c8";
constexpr char kEngineVersion[] = "6000.3";

struct NgxState
{
    std::mutex mutex;
    ID3D12Device* device = nullptr;
    NVSDK_NGX_Parameter* capabilities = nullptr;
    int32_t initResult = int32_t(NVSDK_NGX_Result_Fail);
    int32_t dlrrAvailable = 0;
    int32_t dlrrInitResult = int32_t(NVSDK_NGX_Result_Fail);
    int32_t frameGenerationAvailable = 0;
    int32_t frameGenerationInitResult = int32_t(NVSDK_NGX_Result_Fail);
    uint32_t multiFrameCountMax = 0;
    bool initialized = false;
};

NgxState g_Ngx;

void NVSDK_CONV NgxLogCallback(const char* message,
    NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    if (message && *message)
        LogInfo("[UnityRHI.NGX] %s", message);
}

std::wstring GetModuleDirectory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_Ngx), &module))
        return {};

    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    std::wstring directory(path.data(), length);
    const size_t slash = directory.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : directory.substr(0, slash);
}

std::wstring GetApplicationDataDirectory()
{
    std::array<wchar_t, MAX_PATH> temp{};
    const DWORD length = GetTempPathW(DWORD(temp.size()), temp.data());
    if (length == 0 || length >= temp.size())
        return L".";
    std::wstring path(temp.data(), length);
    path += L"UnityRHI-NGX";
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

template <typename T>
void QueryCapability(NVSDK_NGX_Parameter* parameters, const char* name, T& value)
{
    if (!parameters)
        return;
    if constexpr (std::is_same_v<T, uint32_t>)
        NVSDK_NGX_Parameter_GetUI(parameters, name, &value);
    else
        NVSDK_NGX_Parameter_GetI(parameters, name, &value);
}

void ResetCapabilities()
{
    g_Ngx.dlrrAvailable = 0;
    g_Ngx.dlrrInitResult = int32_t(NVSDK_NGX_Result_Fail);
    g_Ngx.frameGenerationAvailable = 0;
    g_Ngx.frameGenerationInitResult = int32_t(NVSDK_NGX_Result_Fail);
    g_Ngx.multiFrameCountMax = 0;
}
}

bool InitializeNgx(ID3D12Device* device)
{
    std::scoped_lock lock(g_Ngx.mutex);
    if (g_Ngx.initialized)
        return g_Ngx.device == device;
    if (!device)
        return false;

    const std::wstring featureDirectory = GetModuleDirectory();
    const wchar_t* featurePaths[] = {featureDirectory.c_str()};
    NVSDK_NGX_FeatureCommonInfo commonInfo{};
    if (!featureDirectory.empty())
    {
        commonInfo.PathListInfo.Path = featurePaths;
        commonInfo.PathListInfo.Length = 1;
    }
    // UnityRHI emits a compact capability summary below. Keep NGX's internal
    // per-model diagnostics disabled during normal Player startup; they can
    // still be enabled through the standard NGX debugging mechanisms.
    commonInfo.LoggingInfo.LoggingCallback = NgxLogCallback;
    commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    commonInfo.LoggingInfo.DisableOtherLoggingSinks = true;

    const std::wstring appDataDirectory = GetApplicationDataDirectory();
    const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_UNITY, kEngineVersion,
        appDataDirectory.c_str(), device, &commonInfo);
    g_Ngx.initResult = int32_t(result);
    if (NVSDK_NGX_FAILED(result))
    {
        LogError("[UnityRHI.NGX] Initialization failed (result=0x%08X).",
            unsigned(result));
        return false;
    }

    g_Ngx.device = device;
    g_Ngx.initialized = true;
    ResetCapabilities();
    const NVSDK_NGX_Result capabilityResult =
        NVSDK_NGX_D3D12_GetCapabilityParameters(&g_Ngx.capabilities);
    if (NVSDK_NGX_FAILED(capabilityResult) || !g_Ngx.capabilities)
    {
        LogWarning("[UnityRHI.NGX] Capability query failed (result=0x%08X).",
            unsigned(capabilityResult));
    }
    else
    {
        QueryCapability(g_Ngx.capabilities,
            NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,
            g_Ngx.dlrrAvailable);
        QueryCapability(g_Ngx.capabilities,
            NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult,
            g_Ngx.dlrrInitResult);
        QueryCapability(g_Ngx.capabilities,
            NVSDK_NGX_Parameter_FrameGeneration_Available,
            g_Ngx.frameGenerationAvailable);
        QueryCapability(g_Ngx.capabilities,
            NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult,
            g_Ngx.frameGenerationInitResult);
        QueryCapability(g_Ngx.capabilities,
            NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
            g_Ngx.multiFrameCountMax);
    }

    LogInfo("[UnityRHI.NGX] Direct NGX initialized. DLRR=%d (0x%08X), "
            "DLSS-G=%d (0x%08X), MFG max=%u, feature path='%ls'.",
        g_Ngx.dlrrAvailable, unsigned(g_Ngx.dlrrInitResult),
        g_Ngx.frameGenerationAvailable, unsigned(g_Ngx.frameGenerationInitResult),
        g_Ngx.multiFrameCountMax, featureDirectory.c_str());
    return true;
}

void ShutdownNgx()
{
    std::scoped_lock lock(g_Ngx.mutex);
    if (!g_Ngx.initialized)
        return;
    if (g_Ngx.capabilities)
        NVSDK_NGX_D3D12_DestroyParameters(g_Ngx.capabilities);
    g_Ngx.capabilities = nullptr;
    const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Shutdown1(g_Ngx.device);
    if (NVSDK_NGX_FAILED(result))
        LogWarning("[UnityRHI.NGX] Shutdown failed (result=0x%08X).", unsigned(result));
    g_Ngx.device = nullptr;
    g_Ngx.initialized = false;
    ResetCapabilities();
}

bool IsNgxInitialized()
{
    std::scoped_lock lock(g_Ngx.mutex);
    return g_Ngx.initialized;
}

std::mutex& NgxMutex()
{
    return g_Ngx.mutex;
}

NVSDK_NGX_Parameter* CreateNgxFeatureParameters()
{
    std::scoped_lock lock(g_Ngx.mutex);
    NVSDK_NGX_Parameter* parameters = nullptr;
    if (!g_Ngx.initialized ||
        NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters)))
        return nullptr;
    return parameters;
}

void DestroyNgxFeatureParameters(NVSDK_NGX_Parameter* parameters)
{
    if (!parameters)
        return;
    std::scoped_lock lock(g_Ngx.mutex);
    NVSDK_NGX_D3D12_DestroyParameters(parameters);
}

int32_t NgxInitResult()
{
    std::scoped_lock lock(g_Ngx.mutex);
    return g_Ngx.initResult;
}

int32_t NgxDlrrAvailable()
{
    std::scoped_lock lock(g_Ngx.mutex);
    return g_Ngx.dlrrAvailable;
}

int32_t NgxDlrrInitResult()
{
    std::scoped_lock lock(g_Ngx.mutex);
    return g_Ngx.dlrrInitResult;
}

int32_t NgxFrameGenerationAvailable()
{
    std::scoped_lock lock(g_Ngx.mutex);
    return g_Ngx.frameGenerationAvailable;
}

int32_t NgxFrameGenerationInitResult()
{
    std::scoped_lock lock(g_Ngx.mutex);
    return g_Ngx.frameGenerationInitResult;
}

uint32_t NgxFrameGenerationMultiFrameCountMax()
{
    std::scoped_lock lock(g_Ngx.mutex);
    return g_Ngx.multiFrameCountMax;
}
}
