#include <directx/d3d12.h>
#include <windows.h>

#include <cstring>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace
{
using UnityMainFn = int(WINAPI*)(HINSTANCE, HINSTANCE, wchar_t*, int);
using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

decltype(&GetProcAddress) g_RealGetProcAddress = &GetProcAddress;
D3D12CreateDeviceFn g_RealD3D12CreateDevice = nullptr;

HRESULT WINAPI HookD3D12CreateDevice(
    IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid, void** device)
{
    return g_RealD3D12CreateDevice(adapter,
        minimumFeatureLevel < D3D_FEATURE_LEVEL_12_2 ? D3D_FEATURE_LEVEL_12_2 : minimumFeatureLevel,
        riid, device);
}

FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC result = g_RealGetProcAddress(module, name);
    const uintptr_t ordinal = reinterpret_cast<uintptr_t>(name);
    if (ordinal <= 0xffff || !name)
    {
        if (ordinal == 101 && result)
        {
            wchar_t modulePath[MAX_PATH] = {};
            GetModuleFileNameW(module, modulePath, MAX_PATH);
            const wchar_t* fileName = wcsrchr(modulePath, L'\\');
            fileName = fileName ? fileName + 1 : modulePath;
            if (_wcsicmp(fileName, L"d3d12.dll") == 0)
            {
                g_RealD3D12CreateDevice = reinterpret_cast<D3D12CreateDeviceFn>(result);
                return reinterpret_cast<FARPROC>(&HookD3D12CreateDevice);
            }
        }
        return result;
    }

    if (std::strncmp(name, "D3D12", 5) == 0)
    {
        HANDLE file = CreateFileW(L"d3d12_proc_names.txt", FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(file, name, static_cast<DWORD>(std::strlen(name)), &written, nullptr);
            WriteFile(file, "\r\n", 2, &written, nullptr);
            CloseHandle(file);
        }
    }

    if (std::strcmp(name, "D3D12CreateDevice") == 0 && result)
    {
        g_RealD3D12CreateDevice = reinterpret_cast<D3D12CreateDeviceFn>(result);
        return reinterpret_cast<FARPROC>(&HookD3D12CreateDevice);
    }
    return result;
}

bool PatchGetProcAddressImport(HMODULE module)
{
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return false;

    auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, "KERNEL32.dll") != 0 && _stricmp(dllName, "KERNELBASE.dll") != 0)
            continue;

        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), "GetProcAddress") != 0)
                continue;
            DWORD oldProtection = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtection))
                return false;
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(&HookGetProcAddress);
            VirtualProtect(&slots->u1.Function, sizeof(void*), oldProtection, &oldProtection);
            FlushInstructionCache(GetCurrentProcess(), &slots->u1.Function, sizeof(void*));
            return true;
        }
    }
    return false;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previousInstance, wchar_t* commandLine, int showCommand)
{
    HMODULE player = LoadLibraryW(L"UnityPlayer.dll");
    if (!player || !PatchGetProcAddressImport(player))
        return EXIT_FAILURE;
    auto unityMain = reinterpret_cast<UnityMainFn>(g_RealGetProcAddress(player, "UnityMain"));
    return unityMain ? unityMain(instance, previousInstance, commandLine, showCommand) : EXIT_FAILURE;
}
