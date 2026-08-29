#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace unityrhi
{
// Flags for ShaderCompileArgs::flags (mirrored in C# ShaderCompileFlags).
enum ShaderCompileFlagBits : uint32_t
{
    kShaderCompileFlag_None = 0,
    kShaderCompileFlag_DebugInfo = 1 << 0,         // -Zi -Qembed_debug
    kShaderCompileFlag_SkipOptimizations = 1 << 1, // -Od (default is -O3)
    kShaderCompileFlag_WarningsAreErrors = 1 << 2, // -WX
    kShaderCompileFlag_Enable16BitTypes = 1 << 3,  // -enable-16bit-types
    kShaderCompileFlag_StripReflection = 1 << 4,   // -Qstrip_reflect
    kShaderCompileFlag_PayloadQualifiers = 1 << 5, // -enable-payload-qualifiers (lib_6_6 raytracing)
    kShaderCompileFlag_AllResourcesBound = 1 << 6, // -all_resources_bound
    kShaderCompileFlag_StableShaderHash = 1 << 7,  // -Zsb
};

struct ShaderCompileArgs
{
    const char* source = nullptr;        // UTF-8 HLSL source
    const char* sourceName = nullptr;    // for diagnostics, e.g. "VectorAdd.hlsl"
    const char* entryPoint = nullptr;    // ignored for lib_* profiles
    const char* targetProfile = nullptr; // e.g. "cs_6_6", "lib_6_6"
    const char* defines = nullptr;       // newline-separated "NAME" or "NAME=VALUE"
    const char* includeDirs = nullptr;   // newline-separated absolute paths
    uint32_t flags = kShaderCompileFlag_None;
};

// Owns the outputs of one compilation; exposed to C# as an opaque handle and
// freed with UnityRhiShaderCompileDestroy.
struct ShaderCompileResult
{
    bool succeeded = false;
    std::vector<uint8_t> bytecode; // DXIL container
    std::string errors;            // UTF-8 diagnostics (may be non-empty on success: warnings)
};

// Compiles HLSL via dxcompiler.dll (loaded from the plugin's own directory).
// Never returns null; failures are reported through the result.
ShaderCompileResult* CompileShader(const ShaderCompileArgs& args);
} // namespace unityrhi
