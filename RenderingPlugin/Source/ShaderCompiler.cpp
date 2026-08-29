#include "ShaderCompiler.h"

#include <windows.h>
#include <wrl/client.h>

#include <cstring>
#include <mutex>

#include "dxcapi.h"

#include "UnityRhiLog.h"

using Microsoft::WRL::ComPtr;

namespace unityrhi
{
namespace
{
// dxcompiler.dll / dxil.dll are loaded explicitly from the directory that
// contains UnityRHI.dll (Packages/top.kuanmi.unityrhi.native/Plugins/x86_64) so we
// never pick up stray copies from PATH. dxil.dll must be loaded first: the
// compiler looks it up for DXIL signing when producing release shaders.
DxcCreateInstanceProc LoadDxcCreateInstance()
{
    static DxcCreateInstanceProc s_proc = nullptr;
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        HMODULE selfModule = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LoadDxcCreateInstance), &selfModule);

        wchar_t path[MAX_PATH];
        DWORD length = GetModuleFileNameW(selfModule, path, MAX_PATH);
        std::wstring directory(path, length);
        size_t slash = directory.find_last_of(L"\\/");
        directory = slash == std::wstring::npos ? L"" : directory.substr(0, slash + 1);

        LoadLibraryW((directory + L"dxil.dll").c_str());
        HMODULE compiler = LoadLibraryW((directory + L"dxcompiler.dll").c_str());
        if (!compiler)
            compiler = LoadLibraryW(L"dxcompiler.dll"); // last resort: PATH
        if (!compiler)
        {
            LogError("[UnityRHI] Failed to load dxcompiler.dll (expected next to UnityRHI.dll).");
            return;
        }
        s_proc = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(compiler, "DxcCreateInstance"));
        if (!s_proc)
            LogError("[UnityRHI] dxcompiler.dll has no DxcCreateInstance export.");
    });
    return s_proc;
}

std::wstring Utf8ToWide(const char* utf8)
{
    if (!utf8 || !utf8[0])
        return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring wide(size_t(length > 0 ? length - 1 : 0), L'\0');
    if (length > 1)
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), length);
    return wide;
}

// Splits a newline-separated UTF-8 list into wide strings, dropping empties.
std::vector<std::wstring> SplitList(const char* list)
{
    std::vector<std::wstring> items;
    if (!list)
        return items;
    const char* begin = list;
    for (const char* p = list;; ++p)
    {
        if (*p == '\n' || *p == '\0')
        {
            if (p > begin)
                items.push_back(Utf8ToWide(std::string(begin, p).c_str()));
            if (*p == '\0')
                break;
            begin = p + 1;
        }
    }
    return items;
}
} // namespace

ShaderCompileResult* CompileShader(const ShaderCompileArgs& args)
{
    auto* result = new ShaderCompileResult();

    DxcCreateInstanceProc createInstance = LoadDxcCreateInstance();
    if (!createInstance)
    {
        result->errors = "dxcompiler.dll could not be loaded; see the editor log.";
        return result;
    }
    if (!args.source || !args.targetProfile)
    {
        result->errors = "source and targetProfile are required.";
        return result;
    }

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
    {
        result->errors = "failed to create DXC instances.";
        return result;
    }

    // `arguments` holds pointers into the wide strings below; they all outlive
    // the Compile call.
    std::vector<LPCWSTR> arguments;

    std::wstring sourceName = Utf8ToWide(args.sourceName ? args.sourceName : "shader.hlsl");
    arguments.push_back(sourceName.c_str());

    std::wstring entry = Utf8ToWide(args.entryPoint);
    if (!entry.empty())
    {
        arguments.push_back(L"-E");
        arguments.push_back(entry.c_str());
    }

    std::wstring profile = Utf8ToWide(args.targetProfile);
    arguments.push_back(L"-T");
    arguments.push_back(profile.c_str());

    std::vector<std::wstring> defines = SplitList(args.defines);
    for (const std::wstring& define : defines)
    {
        arguments.push_back(L"-D");
        arguments.push_back(define.c_str());
    }

    std::vector<std::wstring> includeDirs = SplitList(args.includeDirs);
    for (const std::wstring& dir : includeDirs)
    {
        arguments.push_back(L"-I");
        arguments.push_back(dir.c_str());
    }

    if (args.flags & kShaderCompileFlag_DebugInfo)
    {
        arguments.push_back(L"-Zi");
        arguments.push_back(L"-Qembed_debug");
    }
    arguments.push_back(args.flags & kShaderCompileFlag_SkipOptimizations ? L"-Od" : L"-O3");
    if (args.flags & kShaderCompileFlag_WarningsAreErrors)
        arguments.push_back(L"-WX");
    if (args.flags & kShaderCompileFlag_Enable16BitTypes)
        arguments.push_back(L"-enable-16bit-types");
    if (args.flags & kShaderCompileFlag_StripReflection)
        arguments.push_back(L"-Qstrip_reflect");
    if (args.flags & kShaderCompileFlag_PayloadQualifiers)
        arguments.push_back(L"-enable-payload-qualifiers");
    if (args.flags & kShaderCompileFlag_AllResourcesBound)
        arguments.push_back(L"-all_resources_bound");
    if (args.flags & kShaderCompileFlag_StableShaderHash)
        arguments.push_back(L"-Zsb");

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = args.source;
    sourceBuffer.Size = strlen(args.source);
    sourceBuffer.Encoding = DXC_CP_UTF8;

    ComPtr<IDxcIncludeHandler> includeHandler;
    utils->CreateDefaultIncludeHandler(&includeHandler);

    ComPtr<IDxcResult> dxcResult;
    HRESULT hr = compiler->Compile(
        &sourceBuffer, arguments.data(), UINT32(arguments.size()),
        includeHandler.Get(), IID_PPV_ARGS(&dxcResult));
    if (FAILED(hr) || !dxcResult)
    {
        result->errors = "IDxcCompiler3::Compile call failed.";
        return result;
    }

    ComPtr<IDxcBlobUtf8> errorsBlob;
    if (SUCCEEDED(dxcResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errorsBlob), nullptr)) &&
        errorsBlob && errorsBlob->GetStringLength() > 0)
    {
        result->errors.assign(errorsBlob->GetStringPointer(), errorsBlob->GetStringLength());
    }

    HRESULT status = E_FAIL;
    dxcResult->GetStatus(&status);
    if (FAILED(status))
        return result; // errors already captured

    ComPtr<IDxcBlob> objectBlob;
    if (FAILED(dxcResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&objectBlob), nullptr)) || !objectBlob)
    {
        result->errors += "\ncompilation succeeded but no object blob was produced.";
        return result;
    }

    const auto* data = static_cast<const uint8_t*>(objectBlob->GetBufferPointer());
    result->bytecode.assign(data, data + objectBlob->GetBufferSize());
    result->succeeded = true;
    return result;
}
} // namespace unityrhi
