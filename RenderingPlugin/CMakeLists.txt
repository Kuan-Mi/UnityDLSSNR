cmake_minimum_required(VERSION 3.24)
project(UnityRHINative CXX)

include(FetchContent)

# Binaries land in <repo>/_Bin/<config>. 3-Pack.bat copies them into
# <repo>/Build/top.kuanmi.unityrhi.native (and the native UPM source tree).
get_filename_component(UNITYRHI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${UNITYRHI_REPO_ROOT}/_Bin" CACHE PATH
    "Native plugin binaries (UnityRHI, NRI, NRD, …)" FORCE)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${UNITYRHI_REPO_ROOT}/_Bin" CACHE PATH
    "Native plugin libraries" FORCE)

# Keep the native PluginAPI ABI used to build this plugin in the repository so
# configuring the build does not depend on a particular Unity installation.
set(UNITYRHI_UNITY_PLUGIN_API
    "${CMAKE_CURRENT_SOURCE_DIR}/UnityPluginAPI")
if(NOT EXISTS "${UNITYRHI_UNITY_PLUGIN_API}/IUnityProfiler.h")
    message(FATAL_ERROR
        "Vendored Unity PluginAPI headers were not found under '${UNITYRHI_UNITY_PLUGIN_API}'.")
endif()

# DXC runtime (dxcompiler.dll / dxil.dll) plus headers. Same family as the
# previously vendored 1.9.2602 binaries; the zip is fetched, not committed.
FetchContent_Declare(unityrhi_dxc
    URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602/dxc_2026_02_20.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DOWNLOAD_NO_PROGRESS 1
    SOURCE_SUBDIR "ignore CMakeLists.txt")
FetchContent_MakeAvailable(unityrhi_dxc)
set(UNITYRHI_DXC_INCLUDE "${unityrhi_dxc_SOURCE_DIR}/inc")
set(UNITYRHI_DXC_BIN "${unityrhi_dxc_SOURCE_DIR}/bin/x64")
if(NOT EXISTS "${UNITYRHI_DXC_BIN}/dxcompiler.dll"
   OR NOT EXISTS "${UNITYRHI_DXC_BIN}/dxil.dll")
    message(FATAL_ERROR
        "DXC FetchContent did not produce dxcompiler.dll / dxil.dll under '${UNITYRHI_DXC_BIN}'.")
endif()
if(NOT EXISTS "${UNITYRHI_DXC_INCLUDE}/dxcapi.h")
    message(FATAL_ERROR
        "DXC FetchContent did not produce dxcapi.h under '${UNITYRHI_DXC_INCLUDE}'.")
endif()

# D3D12 headers: same pin NVRHI used (v1.717.0-preview), fetched not committed.
set(DXHEADERS_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(DXHEADERS_BUILD_GOOGLE_TEST OFF CACHE BOOL "" FORCE)
set(DXHEADERS_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(unityrhi_directx_headers
    URL "https://github.com/microsoft/DirectX-Headers/archive/refs/tags/v1.717.0-preview.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DOWNLOAD_NO_PROGRESS 1)
FetchContent_MakeAvailable(unityrhi_directx_headers)
set(UNITYRHI_DIRECTX_HEADERS "${unityrhi_directx_headers_SOURCE_DIR}")

# NRI remains the backend for the legacy NRIPlugin (NRD/DLSR/NIS). Its NGX
# dependency also supplies the pinned SDK headers, import library and feature
# DLLs used directly by UnityRHI's DLRR and upcoming frame-generation bridge.
# Official NVIDIA-RTX/NRI, unmodified; pin the legacy plugin revision.
FetchContent_Declare(unityrhi_nri
    URL "https://github.com/NVIDIA-RTX/NRI/archive/7f1ceca506e6e9a8489c2a76e3da534a5f9074f2.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR ".")
set(NRI_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D11_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D12_SUPPORT ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_VK_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NONE_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_VALIDATION_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NVTX_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_AMDAGS OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NVAPI OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NIS_SDK ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_IMGUI_EXTENSION OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NGX_SDK ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_AGILITY_SDK_SUPPORT ON CACHE BOOL "" FORCE)
set(NRI_AGILITY_SDK_VERSION_MAJOR "619" CACHE STRING "" FORCE)
set(NRI_AGILITY_SDK_VERSION_MINOR "1" CACHE STRING "" FORCE)
FetchContent_MakeAvailable(unityrhi_nri)
FetchContent_GetProperties(ngx SOURCE_DIR UNITYRHI_NGX_DIR)
if(NOT EXISTS "${UNITYRHI_NGX_DIR}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "NRI's NGX dependency was not found at '${UNITYRHI_NGX_DIR}'.")
endif()

# NRD — NRIPlugin only. NRD_NRI is off because NRI is already in the tree.
set(NRD_NRI OFF CACHE BOOL "" FORCE)
set(NRD_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_NORMAL_ENCODING "2" CACHE STRING "Normal encoding variant (0-4, matches nrd::NormalEncoding)" FORCE)
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "Roughness encoding variant (0-2, matches nrd::RoughnessEncoding)" FORCE)
FetchContent_Declare(unityrhi_nrd
    URL "https://github.com/NVIDIA-RTX/NRD/archive/5bdef77420515fc887dea1ad5ff681937f208678.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR ".")
FetchContent_MakeAvailable(unityrhi_nrd)

# NVIDIA NVAPI for driver-level ray tracing validation. Only linked; the
# feature stays dormant unless NV_ALLOW_RAYTRACING_VALIDATION=1 is set at launch.
# Same GitHub zip NRI uses; not committed.
option(UNITYRHI_WITH_NVAPI "Link NVAPI for driver-level ray tracing validation" ON)
if(UNITYRHI_WITH_NVAPI)
    FetchContent_Declare(unityrhi_nvapi
        URL "https://github.com/NVIDIA/nvapi/archive/832a3673d66a0fdf6d6e522468821d5cbd925f23.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        DOWNLOAD_NO_PROGRESS 1
        SOURCE_SUBDIR "ignore CMakeLists.txt")
    FetchContent_MakeAvailable(unityrhi_nvapi)
    set(UNITYRHI_NVAPI_DIR "${unityrhi_nvapi_SOURCE_DIR}")
    if(NOT EXISTS "${UNITYRHI_NVAPI_DIR}/nvapi.h"
       OR NOT EXISTS "${UNITYRHI_NVAPI_DIR}/amd64/nvapi64.lib")
        message(FATAL_ERROR
            "NVAPI FetchContent did not produce nvapi.h / amd64/nvapi64.lib under '${UNITYRHI_NVAPI_DIR}'.")
    endif()
endif()
function(unityrhi_link_nvapi target)
    if(UNITYRHI_WITH_NVAPI)
        target_include_directories(${target} PRIVATE "${UNITYRHI_NVAPI_DIR}")
        target_link_libraries(${target} PRIVATE "${UNITYRHI_NVAPI_DIR}/amd64/nvapi64.lib")
        target_compile_definitions(${target} PRIVATE UNITYRHI_WITH_NVAPI=1)
    endif()
endfunction()

function(unityrhi_copy_dxc target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${UNITYRHI_DXC_BIN}/dxcompiler.dll"
            "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${UNITYRHI_DXC_BIN}/dxil.dll"
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying DXC runtime next to ${target}"
    )
endfunction()

add_library(UnityRHINative SHARED
    # plugin-specific (no NVRHI counterpart)
    Source/PluginMain.cpp
    Source/RhiExports.cpp
    Source/RhiTypes.h
    Source/Breadcrumbs.h
    Source/CommandStream.cpp
    Source/CommandStream.h
    Source/CommandSubmission.cpp
    Source/CommandSubmission.h
    Source/Dlrr.cpp
    Source/Dlrr.h
    Source/DlssNr.cpp
    Source/DlssNr.h
    Source/DlssNrRuntime.cpp
    Source/DlssNrRuntime.h
    Source/FrameGenerationDebug.cpp
    Source/FrameGenerationDebug.h
    Source/FrameGenerationHooks.cpp
    Source/FrameGenerationHooks.h
    Source/FrameGenerationSwapChain.cpp
    Source/FrameGenerationSwapChain.h
    Source/NgxRuntime.cpp
    Source/NgxRuntime.h
    Source/ShaderCompiler.cpp
    Source/ShaderCompiler.h
    Source/UnityRhiEvents.h
    Source/UnityRhiLog.h
    Source/UnityRhiProfiler.cpp
    Source/UnityRhiProfiler.h
    # Agility SDK opt-in for hosts without the D3D12SDKVersion exports
    Source/d3d12/AgilitySdk.cpp
    Source/d3d12/AgilitySdk.h
    # GPU dump-file diagnostics (Agility SDK preview; no NVRHI counterpart)
    Source/d3d12/GpuDumps.cpp
    Source/d3d12/GpuDumps.h
    Source/d3d12/GpuDumpPreview.h
    Source/d3d12/DredDiagnostics.cpp
    Source/d3d12/DredDiagnostics.h
    # 1:1 ports of NVRHI files (same names as External/nvrhi sources)
    Source/d3d12/d3d12-backend.h
    Source/d3d12/d3d12-buffer.cpp
    Source/d3d12/d3d12-compute.cpp
    Source/d3d12/d3d12-constants.cpp
    Source/d3d12/d3d12-descriptor-heap.cpp
    Source/d3d12/d3d12-device.cpp
    Source/d3d12/d3d12-graphics.cpp
    Source/d3d12/d3d12-queries.cpp
    Source/d3d12/d3d12-raytracing.cpp
    Source/d3d12/d3d12-resource-bindings.cpp
    Source/d3d12/d3d12-shader.cpp
    Source/d3d12/d3d12-state-tracking.cpp
    Source/d3d12/d3d12-texture.cpp
    Source/d3d12/d3d12-upload.cpp
    Source/common/dxgi-format.cpp
    Source/common/dxgi-format.h
    Source/common/format-info.cpp
    Source/common/format-info.h
    Source/common/state-tracking.cpp
    Source/common/state-tracking.h
)

set_target_properties(UnityRHINative PROPERTIES
    OUTPUT_NAME "UnityRHI"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(UnityRHINative PRIVATE
    Source "${UNITYRHI_UNITY_PLUGIN_API}" "${UNITYRHI_DXC_INCLUDE}"
    "${UNITYRHI_NGX_DIR}/include")
target_link_libraries(UnityRHINative PRIVATE
    Microsoft::DirectX-Headers
    Microsoft::DirectX-Guids
    "${UNITYRHI_NGX_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d$<$<CONFIG:Debug>:_dbg>.lib"
    d3d12
    dxgi
)
target_compile_definitions(UnityRHINative PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# Unity 2021.3 stops at IUnityGraphicsD3D12v7 and has no public
# RequestResourceState/NotifyResourceState. That used to be a build variant
# (UNITYRHI_UNITY_2021); it is now detected at runtime in
# Source/d3d12/UnityD3D12Compat.h, so a single DLL serves both 2021.3 and
# Unity 6 and the shared package ships one binary.

target_compile_options(UnityRHINative PRIVATE /W4 /permissive- /FS)
unityrhi_link_nvapi(UnityRHINative)
unityrhi_copy_dxc(UnityRHINative)

# Direct NGX features are loaded from UnityRHI's plugin directory. NRI already
# copies the SR/RR DLLs; frame generation is owned by UnityRHI and must be added
# explicitly until the Present bridge is enabled.
add_custom_command(TARGET UnityRHINative POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UNITYRHI_NGX_DIR}/lib/Windows_x86_64/$<IF:$<CONFIG:Debug>,dev,rel>/nvngx_dlssg.dll"
        "$<TARGET_FILE_DIR:UnityRHINative>"
    COMMENT "Copying direct NGX DLSS-G runtime next to UnityRHI"
)

set(UNITYRHI_DLSSNR_DLL "" CACHE FILEPATH
    "Path to the NVIDIA-signed nvngx_dlssnr.dll experimental snippet")
if(UNITYRHI_DLSSNR_DLL)
    if(NOT EXISTS "${UNITYRHI_DLSSNR_DLL}")
        message(FATAL_ERROR "UNITYRHI_DLSSNR_DLL does not exist: '${UNITYRHI_DLSSNR_DLL}'")
    endif()
    add_custom_command(TARGET UnityRHINative POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${UNITYRHI_DLSSNR_DLL}"
            "$<TARGET_FILE_DIR:UnityRHINative>/nvngx_dlssnr.dll"
        COMMENT "Copying experimental DLSS Neural Rendering snippet next to UnityRHI")
endif()

add_executable(UnityRHIFeatureLevelLauncher WIN32 Launcher/UnityRhiFeatureLevelLauncher.cpp)
set_target_properties(UnityRHIFeatureLevelLauncher PROPERTIES OUTPUT_NAME "UnityRhiFeatureLevelLauncher")
target_link_libraries(UnityRHIFeatureLevelLauncher PRIVATE
    Microsoft::DirectX-Headers Microsoft::DirectX-Guids d3d12)
target_compile_features(UnityRHIFeatureLevelLauncher PRIVATE cxx_std_20)
target_compile_definitions(UnityRHIFeatureLevelLauncher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(UnityRHIFeatureLevelLauncher PRIVATE /W4 /permissive- /FS)

# ---------------------------------------------------------------------------
# D3D12HeapHook — SetDescriptorHeaps vtable hook shared with NRIPlugin.
# UnityRHI loads it by ordinal (Begin=1, End=2, Install=3, Restore=4).
# ---------------------------------------------------------------------------
add_library(D3D12HeapHook SHARED
    D3D12HeapHook/D3D12HeapHook.cpp
    D3D12HeapHook/D3D12HeapHook.def
)
set_target_properties(D3D12HeapHook PROPERTIES
    OUTPUT_NAME "D3D12HeapHook"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(D3D12HeapHook PRIVATE D3D12HeapHook)
target_compile_definitions(D3D12HeapHook PRIVATE D3D12HEAPHOOK_EXPORTS WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(D3D12HeapHook PRIVATE /utf-8 /FS)
target_link_libraries(D3D12HeapHook PRIVATE d3d12)

# ---------------------------------------------------------------------------
# NRIPlugin — Unity native plugin wrapping NRD / DLSR / DLRR / NIS.
# ---------------------------------------------------------------------------
add_library(NRIPlugin SHARED
    NRIPlugin/DLRRInstance.cpp
    NRIPlugin/DLSRInstance.cpp
    NRIPlugin/NISInstance.cpp
    NRIPlugin/NrdInstance.cpp
    NRIPlugin/RenderingPlugin.cpp
    NRIPlugin/RenderSystem.cpp
)
set_target_properties(NRIPlugin PROPERTIES
    OUTPUT_NAME "NRIPlugin"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(NRIPlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    D3D12HeapHook
    "${unityrhi_nrd_SOURCE_DIR}/Integration"
    "${UNITYRHI_DIRECTX_HEADERS}/include/directx"
    "${UNITYRHI_UNITY_PLUGIN_API}"
)
target_compile_definitions(NRIPlugin PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(NRIPlugin PRIVATE /utf-8 /FS)
target_link_libraries(NRIPlugin PRIVATE
    NRD
    NRI
    D3D12HeapHook
    Microsoft::DirectX-Headers
)
add_dependencies(UnityRHINative D3D12HeapHook NRIPlugin)
