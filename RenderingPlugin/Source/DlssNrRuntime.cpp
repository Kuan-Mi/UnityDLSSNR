#include "DlssNrRuntime.h"

#include <array>
#include <cstring>
#include <string>

#include <Windows.h>
#include <directx/d3d12.h>

#include "nvsdk_ngx.h"

#include "NgxRuntime.h"
#include "UnityRhiLog.h"

namespace unityrhi
{
namespace
{
constexpr unsigned long long kApplicationId = 0x0876232Cull;
constexpr NVSDK_NGX_Version kSnippetSdkVersion = static_cast<NVSDK_NGX_Version>(0x15);
constexpr int32_t kRuntimeUnavailable = int32_t(NVSDK_NGX_Result_FAIL_FeatureNotSupported);

using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*,
    ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
    NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
    const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using GetModuleFileNameWFn = DWORD (WINAPI*)(HMODULE, LPWSTR, DWORD);

struct Runtime
{
    std::mutex mutex;
    HMODULE module = nullptr;
    ID3D12Device* device = nullptr;
    InitFn init = nullptr;
    ShutdownFn shutdown = nullptr;
    CreateFn create = nullptr;
    EvaluateFn evaluate = nullptr;
    ReleaseFn release = nullptr;
    GetModuleFileNameWFn originalGetModuleFileNameW = nullptr;
    void** getModuleFileNameWIat = nullptr;
    int32_t initResult = kRuntimeUnavailable;
    bool initialized = false;
};

Runtime g_Runtime;
GetModuleFileNameWFn g_OriginalGetModuleFileNameW = nullptr;
HMODULE g_SnippetModule = nullptr;

std::wstring GetModuleDirectory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_Runtime), &module))
        return {};
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    std::wstring result(path.data(), length);
    const size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : result.substr(0, slash);
}

std::wstring GetApplicationDataDirectory()
{
    std::array<wchar_t, MAX_PATH> temp{};
    const DWORD length = GetTempPathW(DWORD(temp.size()), temp.data());
    if (length == 0 || length >= temp.size())
        return L".";
    std::wstring path(temp.data(), length);
    path += L"UnityRHI-DLSSNR";
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

bool WriteIatSlot(void** slot, void* value)
{
    if (!slot)
        return false;
    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection))
        return false;
    InterlockedExchangePointer(slot, value);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

// nvngx_dlssnr Init_Ext refuses callers whose module path does not contain
// "nvngx.dll". RenoDX IAT-hooks GetModuleFileNameW so the signed snippet sees
// the process NGX core (_nvngx.dll / nvngx.dll) instead of UnityRHI.dll.
DWORD WINAPI HookedGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD nSize)
{
    if (!g_OriginalGetModuleFileNameW)
        return 0;
    if (g_SnippetModule && module == g_SnippetModule)
        return g_OriginalGetModuleFileNameW(module, filename, nSize);

    HMODULE ngx = GetModuleHandleW(L"_nvngx.dll");
    if (!ngx)
        ngx = GetModuleHandleW(L"nvngx.dll");
    if (ngx)
        return g_OriginalGetModuleFileNameW(ngx, filename, nSize);

    if (!filename || nSize == 0)
        return 0;

    std::array<wchar_t, MAX_PATH> fake{};
    DWORD length = 0;
    if (g_SnippetModule)
        length = g_OriginalGetModuleFileNameW(g_SnippetModule, fake.data(), MAX_PATH);
    if (length && length < MAX_PATH)
    {
        wchar_t* slash = fake.data() + length;
        while (slash > fake.data() && slash[-1] != L'\\' && slash[-1] != L'/')
            --slash;
        const wchar_t name[] = L"nvngx.dll";
        const size_t remaining = fake.size() - size_t(slash - fake.data());
        if (remaining < sizeof(name) / sizeof(name[0]))
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return nSize;
        }
        std::memcpy(slash, name, sizeof(name));
        length = DWORD(slash - fake.data()) + DWORD(sizeof(name) / sizeof(name[0]) - 1);
    }
    else
    {
        const wchar_t name[] = L"nvngx.dll";
        std::memcpy(fake.data(), name, sizeof(name));
        length = DWORD(sizeof(name) / sizeof(name[0]) - 1);
    }
    if (length + 1 > nSize)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return nSize;
    }
    std::memcpy(filename, fake.data(), (length + 1) * sizeof(wchar_t));
    return length;
}

bool HookSnippetGetModuleFileNameW()
{
    if (!g_Runtime.module)
        return false;

    auto* base = reinterpret_cast<BYTE*>(g_Runtime.module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return false;

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const FARPROC wanted = kernel32
        ? GetProcAddress(kernel32, "GetModuleFileNameW") : nullptr;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, "KERNEL32.dll") != 0 &&
            _stricmp(dllName, "KERNELBASE.dll") != 0)
            continue;

        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk)
            : nullptr;
        for (size_t i = 0; slots[i].u1.Function; ++i)
        {
            bool match = false;
            if (names && names[i].u1.AddressOfData &&
                !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))
            {
                auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + names[i].u1.AddressOfData);
                match = std::strcmp(reinterpret_cast<const char*>(import->Name),
                    "GetModuleFileNameW") == 0;
            }
            else if (wanted && slots[i].u1.Function == reinterpret_cast<ULONG_PTR>(wanted))
                match = true;
            if (!match)
                continue;

            auto** slot = reinterpret_cast<void**>(&slots[i].u1.Function);
            g_Runtime.originalGetModuleFileNameW =
                reinterpret_cast<GetModuleFileNameWFn>(slots[i].u1.Function);
            g_OriginalGetModuleFileNameW = g_Runtime.originalGetModuleFileNameW;
            g_SnippetModule = g_Runtime.module;
            if (!WriteIatSlot(slot, reinterpret_cast<void*>(&HookedGetModuleFileNameW)))
            {
                g_OriginalGetModuleFileNameW = nullptr;
                g_SnippetModule = nullptr;
                g_Runtime.originalGetModuleFileNameW = nullptr;
                return false;
            }
            g_Runtime.getModuleFileNameWIat = slot;
            return true;
        }
    }
    return false;
}

void UnhookSnippetGetModuleFileNameW()
{
    if (g_Runtime.getModuleFileNameWIat && g_Runtime.originalGetModuleFileNameW)
    {
        WriteIatSlot(g_Runtime.getModuleFileNameWIat,
            reinterpret_cast<void*>(g_Runtime.originalGetModuleFileNameW));
    }
    g_Runtime.getModuleFileNameWIat = nullptr;
    g_Runtime.originalGetModuleFileNameW = nullptr;
    g_OriginalGetModuleFileNameW = nullptr;
    g_SnippetModule = nullptr;
}

template <typename T>
bool Resolve(T& target, const char* name)
{
    target = reinterpret_cast<T>(GetProcAddress(g_Runtime.module, name));
    if (!target)
        LogError("[UnityRHI.DLSSNR] Missing runtime export '%s'.", name);
    return target != nullptr;
}

void ClearFunctions()
{
    g_Runtime.init = nullptr;
    g_Runtime.shutdown = nullptr;
    g_Runtime.create = nullptr;
    g_Runtime.evaluate = nullptr;
    g_Runtime.release = nullptr;
}

void UnloadSnippet()
{
    UnhookSnippetGetModuleFileNameW();
    ClearFunctions();
    if (g_Runtime.module)
        FreeLibrary(g_Runtime.module);
    g_Runtime.module = nullptr;
}
}

bool InitializeDlssNrRuntime(ID3D12Device* device)
{
    std::scoped_lock lock(g_Runtime.mutex);
    if (g_Runtime.initialized)
        return g_Runtime.device == device;
    if (!device)
        return false;

    const std::wstring moduleDirectory = GetModuleDirectory();
    const std::wstring dllPath = moduleDirectory.empty()
        ? L"nvngx_dlssnr.dll" : moduleDirectory + L"\\nvngx_dlssnr.dll";
    g_Runtime.module = LoadLibraryExW(dllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_Runtime.module)
    {
        g_Runtime.initResult = kRuntimeUnavailable;
        LogWarning("[UnityRHI.DLSSNR] Runtime not loaded from '%ls' (Win32=%lu).",
            dllPath.c_str(), GetLastError());
        return false;
    }

    const bool exportsOk =
        Resolve(g_Runtime.init, "NVSDK_NGX_D3D12_Init_Ext") &
        Resolve(g_Runtime.shutdown, "NVSDK_NGX_D3D12_Shutdown1") &
        Resolve(g_Runtime.create, "NVSDK_NGX_D3D12_CreateFeature") &
        Resolve(g_Runtime.evaluate, "NVSDK_NGX_D3D12_EvaluateFeature") &
        Resolve(g_Runtime.release, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (!exportsOk)
    {
        UnloadSnippet();
        g_Runtime.initResult = kRuntimeUnavailable;
        return false;
    }

    if (!HookSnippetGetModuleFileNameW())
    {
        LogWarning("[UnityRHI.DLSSNR] Failed to make signed-feature GetModuleFileNameW IAT writable.");
        UnloadSnippet();
        g_Runtime.initResult = kRuntimeUnavailable;
        return false;
    }
    LogInfo("[UnityRHI.DLSSNR] Installed GetModuleFileNameW IAT hook for NGX caller check.");

    const std::wstring appData = GetApplicationDataDirectory();
    const NVSDK_NGX_Result result = g_Runtime.init(kApplicationId, appData.c_str(),
        device, kSnippetSdkVersion, nullptr);
    g_Runtime.initResult = int32_t(result);
    if (NVSDK_NGX_FAILED(result))
    {
        LogWarning("[UnityRHI.DLSSNR] Snippet initialization failed (result=0x%08X).",
            unsigned(result));
        UnloadSnippet();
        return false;
    }

    g_Runtime.device = device;
    g_Runtime.initialized = true;
    LogInfo("[UnityRHI.DLSSNR] Signed snippet initialized (feature=18, SDK=0x15)." );
    return true;
}

void ShutdownDlssNrRuntime()
{
    std::scoped_lock lock(g_Runtime.mutex);
    if (g_Runtime.initialized && g_Runtime.shutdown)
    {
        const NVSDK_NGX_Result result = g_Runtime.shutdown(g_Runtime.device);
        if (NVSDK_NGX_FAILED(result))
            LogWarning("[UnityRHI.DLSSNR] Shutdown failed (result=0x%08X).", unsigned(result));
    }
    g_Runtime.device = nullptr;
    g_Runtime.initialized = false;
    UnloadSnippet();
}

bool IsDlssNrRuntimeAvailable()
{
    std::scoped_lock lock(g_Runtime.mutex);
    return g_Runtime.initialized;
}

int32_t DlssNrRuntimeInitResult()
{
    std::scoped_lock lock(g_Runtime.mutex);
    return g_Runtime.initResult;
}

std::mutex& DlssNrMutex() { return g_Runtime.mutex; }

NVSDK_NGX_Parameter* AllocateDlssNrParameters()
{
    std::scoped_lock lock(g_Runtime.mutex, NgxMutex());
    NVSDK_NGX_Parameter* parameters = nullptr;
    // The feature DLL intentionally does not export parameter allocation.
    // The reference caller falls back to the process NGX core for these maps.
    if (!g_Runtime.initialized ||
        NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&parameters)))
        return nullptr;
    return parameters;
}

void DestroyDlssNrParameters(NVSDK_NGX_Parameter* parameters)
{
    if (!parameters)
        return;
    std::scoped_lock lock(g_Runtime.mutex, NgxMutex());
    NVSDK_NGX_D3D12_DestroyParameters(parameters);
}

int32_t CreateDlssNrFeature(ID3D12GraphicsCommandList* commandList,
    NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle)
{
    std::scoped_lock lock(g_Runtime.mutex, NgxMutex());
    if (!g_Runtime.initialized || !g_Runtime.create)
        return kRuntimeUnavailable;
    return int32_t(g_Runtime.create(commandList, static_cast<NVSDK_NGX_Feature>(18),
        parameters, handle));
}

int32_t EvaluateDlssNrFeature(ID3D12GraphicsCommandList* commandList,
    const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* parameters)
{
    std::scoped_lock lock(g_Runtime.mutex, NgxMutex());
    if (!g_Runtime.initialized || !g_Runtime.evaluate)
        return kRuntimeUnavailable;
    return int32_t(g_Runtime.evaluate(commandList, handle, parameters, nullptr));
}

int32_t ReleaseDlssNrFeature(NVSDK_NGX_Handle* handle)
{
    std::scoped_lock lock(g_Runtime.mutex, NgxMutex());
    if (!g_Runtime.initialized || !g_Runtime.release)
        return kRuntimeUnavailable;
    return int32_t(g_Runtime.release(handle));
}
}
