// D3D12 Agility SDK opt-in for hosts that do not opt in themselves.
//
// The normal way a process selects an Agility SDK is a pair of exports on its
// executable, D3D12SDKVersion and D3D12SDKPath, which the OS d3d12.dll reads
// before it loads a core runtime. Unity 6's Unity.exe carries them (its entire
// export table is those two symbols). Unity 2021's editor and player
// executables carry neither, so those processes always resolve D3D12Core.dll
// from System32 and run the runtime that shipped with Windows. That runtime has
// no Shader Model 6.9 and no D3D12_FEATURE_D3D12_OPTIONS22, which is what
// blocks SER and makes NRI's DLSS Ray Reconstruction path fault.
//
// ID3D12SDKConfiguration1::CreateDeviceFactory plus
// ID3D12DeviceFactory::ApplyToGlobalState is the component-friendly runtime
// equivalent of those exports. Unlike the older SetSDKVersion API, it accepts
// the package-local absolute path and does not require Windows Developer Mode.
// A native plugin marked "Load on startup" (isPreloaded: 1) is loaded during
// Unity's preload step, which happens before "Initialize engine version" and
// before "GfxDevice: creating device" — early enough. No injection, patching
// of Unity.exe, or copying into the Unity installation is involved.
//
// Applying the factory to global state also takes precedence over the
// executable's exports, so this runs on Unity 6 as well. The old SetSDKVersion
// route remains as a fallback for systems without the factory interfaces; that
// fallback still needs Developer Mode and a D3D12 folder beside the executable.
// Set UNITYRHI_AGILITY_SDK=0 to leave the host's own selection alone.

#include "AgilitySdk.h"

#include <directx/d3d12.h>
#include <windows.h>

#include <cstdlib>
#include <string>

#include "../UnityRhiLog.h"

namespace unityrhi
{
namespace
{
// Agility SDK 1.619.1. Keep in step with the D3D12Core.dll shipped in the
// native package's Plugins/x86_64/D3D12 folder.
constexpr UINT kDefaultAgilitySdkVersion = 619;

/// True when the process executable exports D3D12SDKVersion, i.e. the host
/// selects an Agility SDK itself. Reported in the log so it is obvious whose
/// choice is in effect; it does not stop the override.
bool HostExportsSdkVersion()
{
    auto base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    if (!base)
        return false;

    __try
    {
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (dir.VirtualAddress == 0 || dir.Size == 0)
            return false;

        auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        auto names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        for (DWORD i = 0; i < exports->NumberOfNames; ++i)
        {
            auto name = reinterpret_cast<const char*>(base + names[i]);
            if (lstrcmpA(name, "D3D12SDKVersion") == 0)
                return true;
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // A malformed or unusual image is not worth crashing the editor over;
        // treat it as "host does not opt in" and let SetSDKVersion decide.
        return false;
    }
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty())
        return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

UINT ConfiguredSdkVersion()
{
    // Escape hatch for trying another core without a rebuild.
    char buffer[32] = {};
    if (GetEnvironmentVariableA("UNITYRHI_AGILITY_SDK_VERSION", buffer, sizeof(buffer)) > 0)
    {
        const int value = std::atoi(buffer);
        if (value > 0)
            return static_cast<UINT>(value);
    }
    return kDefaultAgilitySdkVersion;
}

bool g_attempted = false;

std::wstring DirectoryOfThisPlugin()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_attempted), &module))
        return {};

    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(module, modulePath, MAX_PATH) == 0)
        return {};

    std::wstring directory(modulePath);
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    directory.resize(slash + 1);
    return directory;
}

bool ApplyPackageLocalSdk(
    decltype(&D3D12GetInterface) getInterface, UINT version, const std::wstring& sdkDirectory)
{
    if (sdkDirectory.empty() ||
        GetFileAttributesW((sdkDirectory + L"D3D12Core.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        LogInfo("[UnityRHI] Agility SDK: package-local D3D12Core.dll was not found in '%s'; "
                "trying the legacy executable-relative setup.",
                Narrow(sdkDirectory).c_str());
        return false;
    }

    ID3D12SDKConfiguration1* configuration = nullptr;
    HRESULT hr = getInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&configuration));
    if (FAILED(hr) || !configuration)
    {
        LogInfo("[UnityRHI] Agility SDK: D3D12SDKConfiguration1 unavailable "
                "(hr=0x%08lX); trying SetSDKVersion.",
                static_cast<unsigned long>(hr));
        return false;
    }

    ID3D12DeviceFactory* factory = nullptr;
    const std::string sdkPath = Narrow(sdkDirectory);
    hr = configuration->CreateDeviceFactory(version, sdkPath.c_str(), IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr) && factory)
        hr = factory->ApplyToGlobalState();

    if (factory)
        factory->Release();

    if (FAILED(hr))
        configuration->FreeUnusedSDKs();
    configuration->Release();

    if (SUCCEEDED(hr))
    {
        LogInfo("[UnityRHI] Agility SDK %u applied from package-local '%s'.",
                version, sdkPath.c_str());
        return true;
    }

    LogWarning("[UnityRHI] Agility SDK: package-local CreateDeviceFactory/"
               "ApplyToGlobalState(%u) failed (hr=0x%08lX); trying SetSDKVersion.",
               version, static_cast<unsigned long>(hr));
    return false;
}
} // namespace

void EnableAgilitySdkIfHostDoesNot()
{
    if (g_attempted)
        return;
    g_attempted = true;

    if (GetModuleHandleW(L"D3D12Core.dll") != nullptr)
    {
        // A core runtime is already resolved; neither global configuration path
        // can safely change it. Typical cause: the plugin loaded after GfxDevice
        // creation (isPreloaded not yet in effect, or a domain reload).
        LogError("[UnityRHI] Agility SDK: D3D12Core.dll is already loaded in this process, "
                 "so the package-local runtime cannot be applied. Restart the Unity editor.");
        return;
    }

    char disabled[8] = {};
    if (GetEnvironmentVariableA("UNITYRHI_AGILITY_SDK", disabled, sizeof(disabled)) > 0 &&
        disabled[0] == '0')
    {
        LogInfo("[UnityRHI] Agility SDK: disabled by UNITYRHI_AGILITY_SDK=0; "
                "leaving the host's own selection in place.");
        return;
    }

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12)
    {
        LogWarning("[UnityRHI] Agility SDK: d3d12.dll could not be loaded (error=%lu).",
                   static_cast<unsigned long>(GetLastError()));
        return;
    }

    auto getInterface =
        reinterpret_cast<decltype(&D3D12GetInterface)>(GetProcAddress(d3d12, "D3D12GetInterface"));
    if (!getInterface)
    {
        LogWarning("[UnityRHI] Agility SDK: this d3d12.dll has no D3D12GetInterface; "
                   "the OS runtime is too old to redirect.");
        return;
    }

    const UINT version = ConfiguredSdkVersion();
    const std::wstring pluginDirectory = DirectoryOfThisPlugin();
    std::wstring packageSdkDirectory = pluginDirectory;
    if (!packageSdkDirectory.empty())
        packageSdkDirectory += L"D3D12\\";
    if (ApplyPackageLocalSdk(getInterface, version, packageSdkDirectory))
        return;

    // Unity flattens enabled native plugins from nested package folders when
    // building a Player. In the source UPM, Agility lives in
    // Plugins/x86_64/D3D12; in the built Player, D3D12Core.dll can therefore
    // sit directly beside UnityRHI.dll. Accept that deployment without a
    // project-specific post-build copy step.
    if (pluginDirectory != packageSdkDirectory &&
        ApplyPackageLocalSdk(getInterface, version, pluginDirectory))
        return;

    const bool hostSelectsItsOwn = HostExportsSdkVersion();

    // Legacy fallback: SetSDKVersion requires Developer Mode and a relative
    // path from the process executable. For the editor this is the folder
    // holding Unity.exe; for a player it is the game executable's folder.
    wchar_t executablePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, executablePath, MAX_PATH) == 0)
        return;
    std::wstring sdkDirectory(executablePath);
    const size_t slash = sdkDirectory.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return;
    sdkDirectory.resize(slash + 1);
    sdkDirectory += L"D3D12\\";

    if (GetFileAttributesW((sdkDirectory + L"D3D12Core.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        LogInfo("[UnityRHI] Agility SDK: no legacy D3D12Core.dll in '%s'; the runtime selected by "
                "the host/Windows will be used.",
                Narrow(sdkDirectory).c_str());
        return;
    }

    ID3D12SDKConfiguration* configuration = nullptr;
    HRESULT hr = getInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&configuration));
    if (FAILED(hr) || !configuration)
    {
        LogWarning("[UnityRHI] Agility SDK: D3D12SDKConfiguration unavailable (hr=0x%08lX).",
                   static_cast<unsigned long>(hr));
        return;
    }

    // The path has to be relative, and it is only a *record*: SetSDKVersion
    // validates nothing, and D3D12 resolves the string later, when the first
    // device is created, against the working directory the process has at that
    // moment. An absolute path is accepted here and then rejected at device
    // creation with D3D12_ERROR_INVALID_REDIST (0x887E0003), by which point
    // Unity has already fallen back to D3D11 - so pass the canonical relative
    // form and let it resolve where D3D12 expects it.
    //
    // The practical consequence is that the core cannot be loaded out of this
    // package: it has to sit in a "D3D12" folder beside the process executable,
    // which is the editor's own folder in the editor and the game folder in a
    // player. Copy the package's D3D12Core.dll into that folder for the editor;
    // the copy shipped beside this DLL is the source.
    hr = configuration->SetSDKVersion(version, ".\\D3D12\\");
    configuration->Release();

    if (SUCCEEDED(hr))
    {
        LogInfo("[UnityRHI] Agility SDK %u requested from '%s' (host executable %s export "
                "D3D12SDKVersion). This selects the D3D12 runtime for the whole process.",
                version, Narrow(sdkDirectory).c_str(), hostSelectsItsOwn ? "does" : "does not");
    }
    else
    {
        LogWarning("[UnityRHI] Agility SDK: SetSDKVersion(%u) failed (hr=0x%08lX). This legacy "
                   "fallback requires Windows Developer Mode. Continuing on the D3D12 runtime "
                   "selected by the host/Windows.",
                   version, static_cast<unsigned long>(hr));
    }
}

const char* ResolvedD3D12CorePath()
{
    static std::string s_path;
    s_path.clear();
    HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
    if (!core)
        return "";
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(core, path, MAX_PATH) == 0)
        return "";
    s_path = Narrow(path);
    return s_path.c_str();
}
} // namespace unityrhi
