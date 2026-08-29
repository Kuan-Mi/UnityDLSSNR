// Minimal vendored declarations for the D3D12 Agility SDK GPU-dump-file preview
// API. Transcribed verbatim from the pinned Microsoft.Direct3D.D3D12
// 1.721.1-preview headers (build/native/include/d3d12.h) so we do not have to
// swap the whole plugin's D3D12 include path (which is DirectX-Headers, pinned
// to NVRHI's revision) over to the 1.9 MB preview d3d12.h.
//
// Why a distinctly-named interface instead of ID3D12DevicePreview:
// DirectX-Headers already declares an OLDER ID3D12DevicePreview with a
// different vtable (only GetLinearAlgebraMatrixConversionDestinationInfo) and a
// different IID {55ea41d3-...}. That name/guard (__ID3D12DevicePreview_...) is
// already taken, and QueryInterface'ing the old GUID hands back a 1-method
// vtable that would crash the instant ConfigureDumpFile is called. So we mirror
// the NEW vtable under our own type name and QI with the NEW GUID
// {8f0856ad-...}. COM ABI is defined by vtable order + calling convention, so a
// matching struct works regardless of the type's name.
//
// IMPORTANT: this is pinned to 1.721.1-preview. The preview vtable churns
// between SDK versions - on any Agility SDK bump, re-diff this file against the
// new build/native/include/d3d12.h (ID3D12DevicePreview + the D3D12_DUMP_FILE_*
// / D3D12_DEVICE_ERROR_CODE / D3D12_FEATURE_DATA_DUMP_FILE definitions).
// See https://microsoft.github.io/DirectX-Specs/d3d/D3D12GpuDumps.html
#pragma once

#include <directx/d3d12.h>
#include <unknwn.h>

#ifndef UNITYRHI_GPU_DUMP_PREVIEW_TYPES_DEFINED
#define UNITYRHI_GPU_DUMP_PREVIEW_TYPES_DEFINED

// D3D12_FEATURE_DUMP_FILE (= 71) is not present in the pinned DirectX-Headers
// D3D12_FEATURE enum, so expose it as a typed constant for CheckFeatureSupport.
constexpr D3D12_FEATURE D3D12_FEATURE_DUMP_FILE = static_cast<D3D12_FEATURE>(71);

typedef enum D3D12_DUMP_FILE_DRIVER_TIER
{
    D3D12_DUMP_FILE_DRIVER_TIER_NOT_SUPPORTED = 0,
    D3D12_DUMP_FILE_DRIVER_TIER_1 = 1,
    D3D12_DUMP_FILE_DRIVER_TIER_2 = 2
} D3D12_DUMP_FILE_DRIVER_TIER;

typedef enum D3D12_DUMP_FILE_DRIVER_OPTIONS
{
    D3D12_DUMP_FILE_DRIVER_OPTION_NO_OVERHEAD = 0x1,
    D3D12_DUMP_FILE_DRIVER_OPTION_MEDIUM_OVERHEAD = 0x2,
    D3D12_DUMP_FILE_DRIVER_OPTION_HIGH_OVERHEAD = 0x4,
    D3D12_DUMP_FILE_DRIVER_OPTION_NO_DATA = 0x8,
    D3D12_DUMP_FILE_DRIVER_OPTION_SHADER_REGISTERS = 0x10,
    D3D12_DUMP_FILE_DRIVER_OPTION_RESOURCES = 0x20,
    D3D12_DUMP_FILE_DRIVER_OPTION_EVENT_MARKERS = 0x40
} D3D12_DUMP_FILE_DRIVER_OPTIONS;

#ifdef DEFINE_ENUM_FLAG_OPERATORS
DEFINE_ENUM_FLAG_OPERATORS(D3D12_DUMP_FILE_DRIVER_OPTIONS)
#endif

typedef struct D3D12_FEATURE_DATA_DUMP_FILE
{
    BOOL SupportedByOS;
    D3D12_DUMP_FILE_DRIVER_TIER DumpFileDriverTier;
    UINT DumpFileDriverOptionsMask;
} D3D12_FEATURE_DATA_DUMP_FILE;

typedef enum D3D12_DEVICE_ERROR_CODE
{
    D3D12_DEVICE_ERROR_NONE = 0,
    D3D12_DEVICE_ERROR_DEVICE_HUNG = 7,
    D3D12_DEVICE_ERROR_PAGE_FAULT = 9,
    D3D12_DEVICE_ERROR_UNKNOWN = 0x88000000
} D3D12_DEVICE_ERROR_CODE;

// Begin callback returns non-zero to allow the dump to proceed. End callback
// receives the final on-disk dump path. Both run on the device-error path.
typedef UINT(*D3D12_DUMPFILEBEGINCALLBACK)(UINT64 flags);
typedef void(*D3D12_DUMPFILEENDCALLBACK)(const wchar_t* pDumpPath);

// New-vtable view of ID3D12DevicePreview from 1.721.1-preview, under our own
// name. The first slot mirrors GetLinearAlgebraMatrixConversionDestinationInfo
// purely to preserve vtable layout; its parameter is typed void* because we
// never call it and do not want to depend on that preview struct.
struct __declspec(uuid("8f0856ad-e37c-4d75-be18-fbb3d8c04ced"))
    IUnityRhiDevicePreview : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetLinearAlgebraMatrixConversionDestinationInfo(void* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE ConfigureDumpFile(D3D12_DUMP_FILE_DRIVER_OPTIONS driverOptions) = 0;
    virtual void STDMETHODCALLTYPE SetDumpFileCallbacks(
        D3D12_DUMPFILEBEGINCALLBACK pBeginCallback, D3D12_DUMPFILEENDCALLBACK pEndCallback) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddBlobToDumpFile(void* pBlob, UINT sizeBytes, UINT64 metadata) = 0;
    virtual void STDMETHODCALLTYPE RetainDumpFile(BOOL retain) = 0;
    virtual D3D12_DEVICE_ERROR_CODE STDMETHODCALLTYPE GetDeviceErrorCode(void) = 0;
};

#endif // UNITYRHI_GPU_DUMP_PREVIEW_TYPES_DEFINED
