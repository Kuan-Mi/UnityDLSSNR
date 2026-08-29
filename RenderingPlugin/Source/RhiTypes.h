#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// C ABI types shared with the C# runtime (mirrored in
// Packages/com.unityrhi/Runtime/Interop/NativeTypes.cs - keep field order and
// sizes in sync). Enum values match NVRHI's public nvrhi.h
// so C#-side enums stay value-compatible with the native ABI.
//
// All bools are uint32_t and enums are widened to uint32_t so both sides agree
// on layout without packing pragmas. Structs with nvrhi counterparts that
// carry a std::string debugName (e.g. BufferDesc) have a Rhi-prefixed POD
// wire twin (e.g. RhiBufferDesc) that crosses the ABI and is converted in
// RhiExports.cpp.

namespace unityrhi
{
// Mirrors nvrhi::Format (values are indices into the format mapping table).
enum class Format : uint32_t
{
    UNKNOWN,

    R8_UINT, R8_SINT, R8_UNORM, R8_SNORM,
    RG8_UINT, RG8_SINT, RG8_UNORM, RG8_SNORM,
    R16_UINT, R16_SINT, R16_UNORM, R16_SNORM, R16_FLOAT,
    BGRA4_UNORM, B5G6R5_UNORM, B5G5R5A1_UNORM,
    RGBA8_UINT, RGBA8_SINT, RGBA8_UNORM, RGBA8_SNORM,
    BGRA8_UNORM, BGRX8_UNORM,
    SRGBA8_UNORM, SBGRA8_UNORM, SBGRX8_UNORM,
    R10G10B10A2_UNORM, R11G11B10_FLOAT,
    RG16_UINT, RG16_SINT, RG16_UNORM, RG16_SNORM, RG16_FLOAT,
    R32_UINT, R32_SINT, R32_FLOAT,
    RGBA16_UINT, RGBA16_SINT, RGBA16_FLOAT, RGBA16_UNORM, RGBA16_SNORM,
    RG32_UINT, RG32_SINT, RG32_FLOAT,
    RGB32_UINT, RGB32_SINT, RGB32_FLOAT,
    RGBA32_UINT, RGBA32_SINT, RGBA32_FLOAT,

    D16, D24S8, X24G8_UINT, D32, D32S8, X32G8_UINT,

    BC1_UNORM, BC1_UNORM_SRGB,
    BC2_UNORM, BC2_UNORM_SRGB,
    BC3_UNORM, BC3_UNORM_SRGB,
    BC4_UNORM, BC4_SNORM,
    BC5_UNORM, BC5_SNORM,
    BC6H_UFLOAT, BC6H_SFLOAT,
    BC7_UNORM, BC7_UNORM_SRGB,

    // Unity TextureFormat.Alpha8 / GraphicsFormat.A8_UNorm extension.
    A8_UNORM,

    COUNT,
};

// Mirrors nvrhi::TextureDimension.
enum class TextureDimension : uint32_t
{
    Unknown,
    Texture1D,
    Texture1DArray,
    Texture2D,
    Texture2DArray,
    TextureCube,
    TextureCubeArray,
    Texture2DMS,
    Texture2DMSArray,
    Texture3D
};

// Mirrors nvrhi::CpuAccessMode.
enum class CpuAccessMode : uint32_t
{
    None,
    Read,
    Write
};

// Mirrors nvrhi::ResourceStates (bit flags). The values must match the C#
// mirror in NvrhiTypes.cs - raw state bits travel in the command stream.
enum class ResourceStates : uint32_t
{
    Unknown                     = 0,
    Common                      = 0x00000001,
    ConstantBuffer              = 0x00000002,
    VertexBuffer                = 0x00000004,
    IndexBuffer                 = 0x00000008,
    IndirectArgument            = 0x00000010,
    PixelShaderResource         = 0x00000020,
    NonPixelShaderResource      = 0x00000040,
    ShaderResource              = PixelShaderResource | NonPixelShaderResource,
    UnorderedAccess             = 0x00000080,
    RenderTarget                = 0x00000100,
    DepthWrite                  = 0x00000200,
    DepthRead                   = 0x00000400,
    StreamOut                   = 0x00000800,
    CopyDest                    = 0x00001000,
    CopySource                  = 0x00002000,
    ResolveDest                 = 0x00004000,
    ResolveSource               = 0x00008000,
    Present                     = 0x00010000,
    AccelStructRead             = 0x00020000,
    AccelStructWrite            = 0x00040000,
    AccelStructBuildInput       = 0x00080000,
    AccelStructBuildBlas        = 0x00100000,
    ShadingRateSurface          = 0x00200000,
    OpacityMicromapWrite        = 0x00400000,
    OpacityMicromapBuildInput   = 0x00800000,
    ConvertCoopVecMatrixInput   = 0x01000000,
    ConvertCoopVecMatrixOutput  = 0x02000000,
};

inline ResourceStates operator|(ResourceStates a, ResourceStates b)
{
    return ResourceStates(uint32_t(a) | uint32_t(b));
}

inline uint32_t operator&(ResourceStates a, ResourceStates b)
{
    return uint32_t(a) & uint32_t(b);
}

// Mirrors nvrhi::SharedResourceFlags (bit flags).
enum class SharedResourceFlags : uint32_t
{
    None                = 0,
    Shared              = 0x01,
    Shared_NTHandle     = 0x02,
    Shared_CrossAdapter = 0x04,
};

inline uint32_t operator&(SharedResourceFlags a, SharedResourceFlags b)
{
    return uint32_t(a) & uint32_t(b);
}

// Mirrors nvrhi::SamplerAddressMode.
enum class SamplerAddressMode : uint32_t
{
    Clamp,
    Wrap,
    Border,
    Mirror,
    MirrorOnce
};

// Mirrors nvrhi::SamplerReductionType.
enum class SamplerReductionType : uint32_t
{
    Standard,
    Comparison,
    Minimum,
    Maximum
};

// Mirrors nvrhi::ShaderType (bit mask).
enum class ShaderType : uint32_t
{
    None          = 0x0000,
    Compute       = 0x0020,
    Vertex        = 0x0001,
    Hull          = 0x0002,
    Domain        = 0x0004,
    Geometry      = 0x0008,
    Pixel         = 0x0010,
    Amplification = 0x0040,
    Mesh          = 0x0080,
    AllGraphics   = 0x00DF,
    All           = 0x3FFF,
};

// Mirrors nvrhi::ShaderDesc. The NVAPI extension fields (hlslExtensionsUAV,
// custom semantics, fast GS, coordinate swizzling) are not ported.
struct ShaderDesc
{
    ShaderType shaderType = ShaderType::None;
    std::string debugName;
    std::string entryName = "main";

    constexpr ShaderDesc& setShaderType(ShaderType value) { shaderType = value; return *this; }
              ShaderDesc& setDebugName(const std::string& value) { debugName = value; return *this; }
              ShaderDesc& setEntryName(const std::string& value) { entryName = value; return *this; }
};

// Mirrors nvrhi::ResourceType - order and values must match exactly.
enum class ResourceType : uint32_t
{
    None = 0,
    Texture_SRV,
    Texture_UAV,
    TypedBuffer_SRV,
    TypedBuffer_UAV,
    StructuredBuffer_SRV,
    StructuredBuffer_UAV,
    RawBuffer_SRV,
    RawBuffer_UAV,
    ConstantBuffer,
    VolatileConstantBuffer,
    Sampler,
    RayTracingAccelStruct,
    PushConstants,
    SamplerFeedbackTexture_UAV,

    Count
};

// Mirrors nvrhi::Feature (names and values).
enum class RhiFeature : uint32_t
{
    ComputeQueue,
    ConservativeRasterization,
    ConstantBufferRanges,
    CopyQueue,
    DeferredCommandLists,
    FastGeometryShader,
    HeapDirectlyIndexed,
    HlslExtensionUAV,
    LinearSweptSpheres,
    Meshlets,
    RayQuery,
    RayTracingAccelStruct,
    RayTracingClusters,
    RayTracingOpacityMicromap,
    RayTracingPipeline,
    SamplerFeedback,
    ShaderExecutionReordering,
    ShaderSpecializations,
    SinglePassStereo,
    Spheres,
    VariableRateShading,
    VirtualResources,
        WaveLaneCountMinMax,
        CooperativeVectorInferencing,
        CooperativeVectorTraining,
        EnhancedBarriers,
};

// Mirrors nvrhi::HeapType / HeapDesc / MemoryRequirements.
enum class HeapType : uint32_t
{
    DeviceLocal,
    Upload,
    Readback
};

// Exact nvrhi::ObjectTypes values from common/resource.h.
enum class RhiObjectType : uint32_t
{
    SharedHandle = 0x00000001,
    D3D12_Device = 0x00020001,
    D3D12_CommandQueue = 0x00020002,
    D3D12_GraphicsCommandList = 0x00020003,
    D3D12_Resource = 0x00020004,
    D3D12_RenderTargetViewDescriptor = 0x00020005,
    D3D12_DepthStencilViewDescriptor = 0x00020006,
    D3D12_ShaderResourceViewGpuDescriptor = 0x00020007,
    D3D12_UnorderedAccessViewGpuDescriptor = 0x00020008,
    D3D12_RootSignature = 0x00020009,
    D3D12_PipelineState = 0x0002000a,
    D3D12_CommandAllocator = 0x0002000b,
    Nvrhi_D3D12_Device = 0x00020101,
    Nvrhi_D3D12_CommandList = 0x00020102,
};

// C ABI wire format of HeapDesc (debugName travels as a separate argument).
struct RhiHeapDesc
{
    uint64_t capacity;
    HeapType type;
};

// Mirrors nvrhi::HeapDesc.
struct HeapDesc
{
    uint64_t capacity = 0;
    HeapType type = HeapType::DeviceLocal;
    std::string debugName;

    constexpr HeapDesc& setCapacity(uint64_t value) { capacity = value; return *this; }
    constexpr HeapDesc& setType(HeapType value) { type = value; return *this; }
              HeapDesc& setDebugName(const std::string& value) { debugName = value; return *this; }

    // UnityRHI: conversion from the C ABI wire format.
    HeapDesc() = default;
    HeapDesc(const RhiHeapDesc& w, const char* name)
        : capacity(w.capacity), type(w.type), debugName(name ? name : "")
    {
    }
};

struct MemoryRequirements
{
    uint64_t size;
    uint64_t alignment;
};

struct RhiTiledTextureCoordinate
{
    uint16_t mipLevel;
    uint16_t arrayLevel;
    uint32_t x;
    uint32_t y;
    uint32_t z;
};

struct RhiTiledTextureRegion
{
    uint32_t tilesNum;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
};

struct RhiPackedMipDesc
{
    uint32_t numStandardMips;
    uint32_t numPackedMips;
    uint32_t numTilesForPackedMips;
    uint32_t startTileIndexInOverallResource;
};

struct RhiTileShape
{
    uint32_t widthInTexels;
    uint32_t heightInTexels;
    uint32_t depthInTexels;
};

struct RhiSubresourceTiling
{
    uint32_t widthInTiles;
    uint32_t heightInTiles;
    uint32_t depthInTiles;
    uint32_t startTileIndexInOverallResource;
};

enum class FormatSupport : uint32_t
{
    None = 0,
    Buffer = 0x00000001,
    IndexBuffer = 0x00000002,
    VertexBuffer = 0x00000004,
    Texture = 0x00000008,
    DepthStencil = 0x00000010,
    RenderTarget = 0x00000020,
    Blendable = 0x00000040,
    ShaderLoad = 0x00000080,
    ShaderSample = 0x00000100,
    ShaderUavLoad = 0x00000200,
    ShaderUavStore = 0x00000400,
    ShaderAtomic = 0x00000800,
};

inline FormatSupport operator|(FormatSupport a, FormatSupport b)
{
    return FormatSupport(uint32_t(a) | uint32_t(b));
}

// C ABI wire format of BufferDesc (debugName travels as a separate argument).
struct RhiBufferDesc
{
    uint64_t byteSize;
    uint32_t structStride;      // if non-zero it's structured
    uint32_t maxVersions;       // Vulkan-only in nvrhi; ignored here
    Format format;              // for typed buffer views
    CpuAccessMode cpuAccess;
    uint32_t canHaveUAVs;
    uint32_t canHaveTypedViews;
    uint32_t canHaveRawViews;
    uint32_t isVertexBuffer;
    uint32_t isIndexBuffer;
    uint32_t isConstantBuffer;
    uint32_t isDrawIndirectArgs;
    uint32_t isAccelStructBuildInput;
    uint32_t isAccelStructStorage;
    uint32_t isShaderBindingTable;
    uint32_t isVolatile;
    uint32_t isVirtual;
    ResourceStates initialState;
    uint32_t keepInitialState;
    SharedResourceFlags sharedResourceFlags;
};

// Mirrors nvrhi::BufferDesc.
struct BufferDesc
{
    uint64_t byteSize = 0;
    uint32_t structStride = 0; // if non-zero it's structured
    uint32_t maxVersions = 0; // only valid and required to be nonzero for volatile buffers on Vulkan
    std::string debugName;
    Format format = Format::UNKNOWN; // for typed buffer views
    bool canHaveUAVs = false;
    bool canHaveTypedViews = false;
    bool canHaveRawViews = false;
    bool isVertexBuffer = false;
    bool isIndexBuffer = false;
    bool isConstantBuffer = false;
    bool isDrawIndirectArgs = false;
    bool isAccelStructBuildInput = false;
    bool isAccelStructStorage = false;
    bool isShaderBindingTable = false;

    // A dynamic/upload buffer whose contents only live in the current command list
    bool isVolatile = false;

    // Indicates that the buffer is created with no backing memory,
    // and memory is bound to the buffer later using bindBufferMemory.
    // On DX12, the buffer resource is created at the time of memory binding.
    bool isVirtual = false;

    ResourceStates initialState = ResourceStates::Common;

    // see TextureDesc::keepInitialState
    bool keepInitialState = false;

    CpuAccessMode cpuAccess = CpuAccessMode::None;

    SharedResourceFlags sharedResourceFlags = SharedResourceFlags::None;

    constexpr BufferDesc& setByteSize(uint64_t value) { byteSize = value; return *this; }
    constexpr BufferDesc& setStructStride(uint32_t value) { structStride = value; return *this; }
    constexpr BufferDesc& setMaxVersions(uint32_t value) { maxVersions = value; return *this; }
              BufferDesc& setDebugName(const std::string& value) { debugName = value; return *this; }
    constexpr BufferDesc& setFormat(Format value) { format = value; return *this; }
    constexpr BufferDesc& setCanHaveUAVs(bool value) { canHaveUAVs = value; return *this; }
    constexpr BufferDesc& setCanHaveTypedViews(bool value) { canHaveTypedViews = value; return *this; }
    constexpr BufferDesc& setCanHaveRawViews(bool value) { canHaveRawViews = value; return *this; }
    constexpr BufferDesc& setIsVertexBuffer(bool value) { isVertexBuffer = value; return *this; }
    constexpr BufferDesc& setIsIndexBuffer(bool value) { isIndexBuffer = value; return *this; }
    constexpr BufferDesc& setIsConstantBuffer(bool value) { isConstantBuffer = value; return *this; }
    constexpr BufferDesc& setIsDrawIndirectArgs(bool value) { isDrawIndirectArgs = value; return *this; }
    constexpr BufferDesc& setIsAccelStructBuildInput(bool value) { isAccelStructBuildInput = value; return *this; }
    constexpr BufferDesc& setIsAccelStructStorage(bool value) { isAccelStructStorage = value; return *this; }
    constexpr BufferDesc& setIsShaderBindingTable(bool value) { isShaderBindingTable = value; return *this; }
    constexpr BufferDesc& setIsVolatile(bool value) { isVolatile = value; return *this; }
    constexpr BufferDesc& setIsVirtual(bool value) { isVirtual = value; return *this; }
    constexpr BufferDesc& setInitialState(ResourceStates value) { initialState = value; return *this; }
    constexpr BufferDesc& setKeepInitialState(bool value) { keepInitialState = value; return *this; }
    constexpr BufferDesc& setCpuAccess(CpuAccessMode value) { cpuAccess = value; return *this; }

    // UnityRHI: conversion from the C ABI wire format.
    BufferDesc() = default;
    BufferDesc(const RhiBufferDesc& w, const char* name)
        : byteSize(w.byteSize)
        , structStride(w.structStride)
        , maxVersions(w.maxVersions)
        , debugName(name ? name : "")
        , format(w.format)
        , canHaveUAVs(w.canHaveUAVs != 0)
        , canHaveTypedViews(w.canHaveTypedViews != 0)
        , canHaveRawViews(w.canHaveRawViews != 0)
        , isVertexBuffer(w.isVertexBuffer != 0)
        , isIndexBuffer(w.isIndexBuffer != 0)
        , isConstantBuffer(w.isConstantBuffer != 0)
        , isDrawIndirectArgs(w.isDrawIndirectArgs != 0)
        , isAccelStructBuildInput(w.isAccelStructBuildInput != 0)
        , isAccelStructStorage(w.isAccelStructStorage != 0)
        , isShaderBindingTable(w.isShaderBindingTable != 0)
        , isVolatile(w.isVolatile != 0)
        , isVirtual(w.isVirtual != 0)
        , initialState(w.initialState)
        , keepInitialState(w.keepInitialState != 0)
        , cpuAccess(w.cpuAccess)
        , sharedResourceFlags(w.sharedResourceFlags)
    {
    }
};

// Mirrors nvrhi::Color.
struct Color
{
    float r, g, b, a;

    Color() : r(0.f), g(0.f), b(0.f), a(0.f) { }
    Color(float c) : r(c), g(c), b(c), a(c) { }
    Color(float _r, float _g, float _b, float _a) : r(_r), g(_g), b(_b), a(_a) { }

    bool operator ==(const Color& _b) const { return r == _b.r && g == _b.g && b == _b.b && a == _b.a; }
    bool operator !=(const Color& _b) const { return !(*this == _b); }
};

// C ABI wire format of TextureDesc (debugName travels as a separate argument).
struct RhiTextureDesc
{
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t arraySize;
    uint32_t mipLevels;
    uint32_t sampleCount;
    uint32_t sampleQuality;
    Format format;
    TextureDimension dimension;
    uint32_t isShaderResource;
    uint32_t isRenderTarget;
    uint32_t isUAV;
    uint32_t isTypeless;
    uint32_t isShadingRateSurface;
    SharedResourceFlags sharedResourceFlags;
    uint32_t isVirtual;
    uint32_t isTiled;
    float clearValue[4];
    uint32_t useClearValue;
    ResourceStates initialState;
    uint32_t keepInitialState;
};

// Mirrors nvrhi::TextureDesc.
struct TextureDesc
{
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t arraySize = 1;
    uint32_t mipLevels = 1;
    uint32_t sampleCount = 1;
    uint32_t sampleQuality = 0;
    Format format = Format::UNKNOWN;
    TextureDimension dimension = TextureDimension::Texture2D;
    std::string debugName;

    bool isShaderResource = true; // Note: isShaderResource is initialized to 'true' for backward compatibility
    bool isRenderTarget = false;
    bool isUAV = false;
    bool isTypeless = false;
    bool isShadingRateSurface = false;

    SharedResourceFlags sharedResourceFlags = SharedResourceFlags::None;

    // Indicates that the texture is created with no backing memory,
    // and memory is bound to the texture later using bindTextureMemory.
    // On DX12, the texture resource is created at the time of memory binding.
    bool isVirtual = false;
    bool isTiled = false;

    Color clearValue;
    bool useClearValue = false;

    ResourceStates initialState = ResourceStates::Unknown;

    // If keepInitialState is true, command lists that use the texture will automatically
    // begin tracking the texture from the initial state and transition it to the initial state
    // on command list close.
    bool keepInitialState = false;

    constexpr TextureDesc& setWidth(uint32_t value) { width = value; return *this; }
    constexpr TextureDesc& setHeight(uint32_t value) { height = value; return *this; }
    constexpr TextureDesc& setDepth(uint32_t value) { depth = value; return *this; }
    constexpr TextureDesc& setArraySize(uint32_t value) { arraySize = value; return *this; }
    constexpr TextureDesc& setMipLevels(uint32_t value) { mipLevels = value; return *this; }
    constexpr TextureDesc& setSampleCount(uint32_t value) { sampleCount = value; return *this; }
    constexpr TextureDesc& setSampleQuality(uint32_t value) { sampleQuality = value; return *this; }
    constexpr TextureDesc& setFormat(Format value) { format = value; return *this; }
    constexpr TextureDesc& setDimension(TextureDimension value) { dimension = value; return *this; }
              TextureDesc& setDebugName(const std::string& value) { debugName = value; return *this; }
    constexpr TextureDesc& setIsShaderResource(bool value) { isShaderResource = value; return *this; }
    constexpr TextureDesc& setIsRenderTarget(bool value) { isRenderTarget = value; return *this; }
    constexpr TextureDesc& setIsUAV(bool value) { isUAV = value; return *this; }
    constexpr TextureDesc& setIsTypeless(bool value) { isTypeless = value; return *this; }
    constexpr TextureDesc& setIsVirtual(bool value) { isVirtual = value; return *this; }
    constexpr TextureDesc& setIsTiled(bool value) { isTiled = value; return *this; }
              TextureDesc& setClearValue(const Color& value) { clearValue = value; useClearValue = true; return *this; }
    constexpr TextureDesc& setUseClearValue(bool value) { useClearValue = value; return *this; }
    constexpr TextureDesc& setInitialState(ResourceStates value) { initialState = value; return *this; }
    constexpr TextureDesc& setKeepInitialState(bool value) { keepInitialState = value; return *this; }

    // UnityRHI: conversion from the C ABI wire format.
    TextureDesc() = default;
    TextureDesc(const RhiTextureDesc& w, const char* name)
        : width(w.width)
        , height(w.height)
        , depth(w.depth)
        , arraySize(w.arraySize)
        , mipLevels(w.mipLevels)
        , sampleCount(w.sampleCount)
        , sampleQuality(w.sampleQuality)
        , format(w.format)
        , dimension(w.dimension)
        , debugName(name ? name : "")
        , isShaderResource(w.isShaderResource != 0)
        , isRenderTarget(w.isRenderTarget != 0)
        , isUAV(w.isUAV != 0)
        , isTypeless(w.isTypeless != 0)
        , isShadingRateSurface(w.isShadingRateSurface != 0)
        , sharedResourceFlags(w.sharedResourceFlags)
        , isVirtual(w.isVirtual != 0)
        , isTiled(w.isTiled != 0)
        , clearValue(w.clearValue[0], w.clearValue[1], w.clearValue[2], w.clearValue[3])
        , useClearValue(w.useClearValue != 0)
        , initialState(w.initialState)
        , keepInitialState(w.keepInitialState != 0)
    {
    }
};

struct RhiTextureSlice
{
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mipLevel;
    uint32_t arraySlice;
};

// Mirrors nvrhi::SamplerDesc.
struct RhiSamplerDesc
{
    float borderColor[4];
    float maxAnisotropy;
    float mipBias;
    float minLod;
    float maxLod;
    uint32_t minFilter;
    uint32_t magFilter;
    uint32_t mipFilter;
    SamplerAddressMode addressU;
    SamplerAddressMode addressV;
    SamplerAddressMode addressW;
    SamplerReductionType reductionType;
};

// Live-object counters returned to C# (leak checks, debug UI).
struct RhiDeviceStats
{
    uint32_t liveBuffers;
    uint32_t liveTextures;
    uint32_t liveSamplers;
    uint32_t liveShaders;
    uint32_t liveBindingLayouts;
    uint32_t liveBindingSets;
    uint32_t liveComputePipelines;
    uint32_t liveShaderLibraries;
    uint32_t liveAccelStructs;
    uint32_t liveRayTracingPipelines;
    uint32_t liveShaderTables;
    uint32_t liveDescriptorTables;
    uint32_t liveInputLayouts;
    uint32_t liveFramebuffers;
    uint32_t liveGraphicsPipelines;
    uint32_t liveHeaps;
    uint32_t liveStagingTextures;
    uint32_t liveEventQueries;
    uint32_t liveTimerQueries;
    uint32_t pendingReleases;
};

// Fixed-size diagnostic snapshot of one native resource. This is intentionally
// a flat C ABI structure so the editor can inspect resources without retaining
// native pointers or calling back while Device::m_Mutex is held.
struct RhiResourceInfo
{
    uint64_t handle;
    uint64_t logicalSize;
    uint64_t allocationSize;
    uint32_t kind;
    uint32_t flags;
    uint32_t lastUseInstance;
    uint32_t releaseInstance;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t arraySize;
    uint32_t mipLevels;
    uint32_t sampleCount;
    uint32_t format;
    uint32_t initialState;
    uint32_t permanentState;
    uint32_t detail0;
    uint32_t detail1;
    char debugName[128];
};

enum RhiResourceInfoFlags : uint32_t
{
    RhiResourceInfo_PendingRelease = 1u << 0,
    RhiResourceInfo_UnityOwned = 1u << 1,
    RhiResourceInfo_HasNativeResource = 1u << 2,
    RhiResourceInfo_Placed = 1u << 3,
    RhiResourceInfo_Virtual = 1u << 4,
    RhiResourceInfo_Tiled = 1u << 5,
    RhiResourceInfo_CpuRead = 1u << 6,
    RhiResourceInfo_CpuWrite = 1u << 7,
    RhiResourceInfo_UnorderedAccess = 1u << 8,
    RhiResourceInfo_RenderTarget = 1u << 9,
    RhiResourceInfo_ShaderResource = 1u << 10,
    RhiResourceInfo_InternalBacking = 1u << 11,
    RhiResourceInfo_GpuUseCompleted = 1u << 31
};

// Mirrors nvrhi::BindingLayoutItem { slot, type, size }; fields are widened
// to uint32 instead of nvrhi's 8-byte bitfield packing.
struct RhiBindingLayoutItem
{
    uint32_t slot;
    ResourceType type;

    // Push constant byte size when (type == PushConstants);
    // descriptor array size (1 or more) for all other resource types.
    uint32_t size;

    uint32_t getArraySize() const
    {
        return type == ResourceType::PushConstants ? 1 : size;
    }
};

// Layout-level parameters of nvrhi::BindingLayoutDesc (items travel separately).
struct RhiBindingLayoutDesc
{
    ShaderType visibility;
    uint32_t registerSpace;
    uint32_t registerSpaceIsDescriptorSet; // Vulkan-only semantics; ignored
};

// Mirrors nvrhi::BufferRange.
struct BufferRange
{
    uint64_t byteOffset = 0;
    uint64_t byteSize = 0;

    BufferRange resolve(const BufferDesc& desc) const
    {
        BufferRange result;
        result.byteOffset = std::min(byteOffset, desc.byteSize);
        if (byteSize == 0)
            result.byteSize = desc.byteSize - result.byteOffset;
        else
            result.byteSize = std::min(byteSize, desc.byteSize - result.byteOffset);
        return result;
    }
};

static constexpr BufferRange EntireBuffer = {0, ~0ull};

// Mirrors nvrhi MipLevel / ArraySlice.
typedef uint32_t MipLevel;
typedef uint32_t ArraySlice;

// Mirrors nvrhi::TextureSubresourceSet.
struct TextureSubresourceSet
{
    static constexpr uint32_t AllMipLevels = ~0u;
    static constexpr uint32_t AllArraySlices = ~0u;

    uint32_t baseMipLevel = 0;
    uint32_t numMipLevels = 1;
    uint32_t baseArraySlice = 0;
    uint32_t numArraySlices = 1;

    // Mirrors nvrhi TextureSubresourceSet::isEntireTexture.
    bool isEntireTexture(const TextureDesc& desc) const
    {
        if (baseMipLevel > 0u || baseMipLevel + numMipLevels < desc.mipLevels)
            return false;

        switch (desc.dimension)
        {
        case TextureDimension::Texture1DArray:
        case TextureDimension::Texture2DArray:
        case TextureDimension::TextureCube:
        case TextureDimension::TextureCubeArray:
        case TextureDimension::Texture2DMSArray:
            if (baseArraySlice > 0u || baseArraySlice + numArraySlices < desc.arraySize)
                return false;
            break;
        default:
            break;
        }

        return true;
    }

    TextureSubresourceSet resolve(const TextureDesc& desc, bool singleMipLevel) const
    {
        TextureSubresourceSet ret;
        ret.baseMipLevel = baseMipLevel;
        if (singleMipLevel)
        {
            ret.numMipLevels = 1;
        }
        else
        {
            uint32_t lastMipLevelPlusOne = std::min(baseMipLevel + numMipLevels, desc.mipLevels);
            ret.numMipLevels = lastMipLevelPlusOne > baseMipLevel ? lastMipLevelPlusOne - baseMipLevel : 0;
        }

        switch (desc.dimension)
        {
        case TextureDimension::Texture1DArray:
        case TextureDimension::Texture2DArray:
        case TextureDimension::TextureCube:
        case TextureDimension::TextureCubeArray:
        case TextureDimension::Texture2DMSArray:
        {
            ret.baseArraySlice = baseArraySlice;
            uint32_t lastArraySlicePlusOne = std::min(baseArraySlice + numArraySlices, desc.arraySize);
            ret.numArraySlices = lastArraySlicePlusOne > baseArraySlice ? lastArraySlicePlusOne - baseArraySlice : 0;
            break;
        }
        default:
            ret.baseArraySlice = 0;
            ret.numArraySlices = 1;
            break;
        }
        return ret;
    }
};

// Mirrors nvrhi::AllSubresources.
static constexpr TextureSubresourceSet AllSubresources = {
    0, TextureSubresourceSet::AllMipLevels, 0, TextureSubresourceSet::AllArraySlices};

// ---------------------------------------------------------------------------
// Raster path (Phase 8). Mirrors the subset of nvrhi graphics types needed to
// replay draws; render state is flattened for the C ABI.
// ---------------------------------------------------------------------------

// Mirrors nvrhi::PrimitiveType.
enum class PrimitiveType : uint32_t
{
    PointList = 0,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
    TriangleFan,
    TriangleListWithAdjacency,
    TriangleStripWithAdjacency,
    PatchList,
};

// Mirrors nvrhi::RasterCullMode / RasterFillMode.
enum class RasterCullMode : uint32_t { Back = 0, Front, None };
enum class RasterFillMode : uint32_t { Solid = 0, Wireframe };

// Mirrors nvrhi::VertexAttributeDesc. UnityRHI deviation: the semantic name
// travels as a C string; nvrhi's arraySize is expanded by the caller.
struct RhiVertexAttributeDesc
{
    const char* name;       // HLSL semantic (e.g. "POSITION")
    uint32_t semanticIndex;
    Format format;
    uint32_t bufferIndex;   // input slot
    uint32_t offset;        // AlignedByteOffset
    uint32_t elementStride; // stride of the vertex buffer bound at bufferIndex
    uint32_t isInstanced;
};

// Reduced nvrhi::GraphicsPipelineDesc render state. UnityRHI deviation: one
// blend target, and blend/comparison values are D3D12 enum values passed
// through (D3D12_BLEND / D3D12_BLEND_OP / D3D12_COMPARISON_FUNC).
struct RhiGraphicsPipelineDesc
{
    uint32_t primType;      // PrimitiveType
    uint32_t cullMode;      // RasterCullMode
    uint32_t fillMode;      // RasterFillMode
    uint32_t frontCounterClockwise;
    uint32_t depthClipEnable;

    uint32_t depthTestEnable;
    uint32_t depthWriteEnable;
    uint32_t depthFunc;     // D3D12_COMPARISON_FUNC; 0 = LESS

    uint32_t blendEnable;   // render target 0
    uint32_t srcBlend;      // D3D12_BLEND; 0 = ONE
    uint32_t destBlend;     // D3D12_BLEND; 0 = ZERO
    uint32_t blendOp;       // D3D12_BLEND_OP; 0 = ADD
    uint32_t srcBlendAlpha;
    uint32_t destBlendAlpha;
    uint32_t blendOpAlpha;
    uint32_t colorWriteMask; // 0 = all channels
};

// Mirrors nvrhi::Viewport.
struct RhiViewport
{
    float minX, maxX;
    float minY, maxY;
    float minZ, maxZ;
};

// Layout-level parameters of nvrhi::BindlessLayoutDesc (registerSpaces travel
// separately as RhiBindingLayoutItem entries; item.slot = register space).
struct RhiBindlessLayoutDesc
{
    ShaderType visibility;
    uint32_t firstSlot;
    uint32_t maxCapacity;
    uint32_t layoutType; // mirrors nvrhi::BindlessLayoutDesc::LayoutType
};

// Mirrors nvrhi::BindlessLayoutDesc::LayoutType.
enum class RhiBindlessLayoutType : uint32_t
{
    Immutable = 0,
    MutableSrvUavCbv,
    MutableCounters,
    MutableSampler,
};

// ---------------------------------------------------------------------------
// Ray tracing (mirrors nvrhi::rt; NVAPI-only members - OMM linkage, spheres,
// LSS - are not ported).
// ---------------------------------------------------------------------------

// Mirrors nvrhi::rt::GeometryFlags.
enum class RhiRtGeometryFlags : uint32_t
{
    None = 0,
    Opaque = 1,
    NoDuplicateAnyHitInvocation = 2,
};

// Mirrors nvrhi::rt::GeometryType (Spheres/Lss are NVAPI extensions, not ported).
enum class RhiRtGeometryType : uint32_t
{
    Triangles = 0,
    AABBs = 1,
};

// Mirrors nvrhi::rt::InstanceFlags.
enum class RhiRtInstanceFlags : uint32_t
{
    None = 0,
    TriangleCullDisable = 1,
    TriangleFrontCounterclockwise = 2,
    ForceOpaque = 4,
    ForceNonOpaque = 8,
};

// Mirrors nvrhi::rt::AccelStructBuildFlags.
enum class RhiRtAccelStructBuildFlags : uint32_t
{
    None = 0,
    AllowUpdate = 1,
    AllowCompaction = 2,
    PreferFastTrace = 4,
    PreferFastBuild = 8,
    MinimizeMemory = 0x10,
    PerformUpdate = 0x20,
    AllowEmptyInstances = 0x80,
};

inline uint32_t operator&(RhiRtAccelStructBuildFlags a, RhiRtAccelStructBuildFlags b)
{
    return uint32_t(a) & uint32_t(b);
}

inline RhiRtAccelStructBuildFlags operator~(RhiRtAccelStructBuildFlags a)
{
    return RhiRtAccelStructBuildFlags(~uint32_t(a));
}

// Mirrors nvrhi::rt::GeometryTriangles (buffers are Buffer handles).
struct RhiRtGeometryTriangles
{
    uint64_t indexBuffer;  // Buffer*, may be 0
    uint64_t vertexBuffer; // Buffer*
    Format indexFormat;
    Format vertexFormat;
    uint64_t indexOffset;
    uint64_t vertexOffset;
    uint32_t indexCount;
    uint32_t vertexCount;
    uint32_t vertexStride;
    uint32_t padding;
};
static_assert(sizeof(RhiRtGeometryTriangles) == 56, "keep in sync with the C# mirror");

// Mirrors nvrhi::rt::GeometryAABBs.
struct RhiRtGeometryAABBs
{
    uint64_t buffer; // Buffer*
    uint64_t unused;
    uint64_t offset;
    uint32_t count;
    uint32_t stride;
};

// Mirrors nvrhi::rt::GeometryDesc.
struct RhiRtGeometryDesc
{
    union
    {
        RhiRtGeometryTriangles triangles;
        RhiRtGeometryAABBs aabbs;
    } geometryData;

    uint32_t useTransform;
    float transform[12]; // nvrhi::rt::AffineTransform
    RhiRtGeometryFlags flags;
    RhiRtGeometryType geometryType;
    uint32_t padding; // keeps sizeof a multiple of the uint64 alignment
};
static_assert(sizeof(RhiRtGeometryDesc) == 120, "keep in sync with the C# mirror");

// Mirrors nvrhi::rt::InstanceDesc - bit-compatible with
// D3D12_RAYTRACING_INSTANCE_DESC (the replay memcpys it and patches the
// acceleration-structure address).
struct RhiRtInstanceDesc
{
    float transform[12];
    uint32_t instanceIdAndMask;                 // instanceID : 24, instanceMask : 8
    uint32_t instanceContributionAndFlags;      // instanceContributionToHitGroupIndex : 24, flags : 8
    uint64_t bottomLevelAS;                     // AccelStruct* (or device address for the FromBuffer path)
};
static_assert(sizeof(RhiRtInstanceDesc) == 64, "sizeof(InstanceDesc) is supposed to be 64 bytes");

// C ABI wire format of AccelStructDesc (geometries and debugName travel as
// separate arguments).
struct RhiRtAccelStructDesc
{
    uint64_t topLevelMaxInstances; // only applies when isTopLevel = true
    RhiRtAccelStructBuildFlags buildFlags;
    uint32_t isTopLevel;
    uint32_t isVirtual;
};

// Mirrors nvrhi::rt::AccelStructDesc. UnityRHI deviations: the geometry
// entries are the C-ABI RhiRtGeometryDesc (they also travel in the command
// stream), and there is no trackLiveness (lifetime is owned by C#).
struct AccelStructDesc
{
    uint64_t topLevelMaxInstances = 0; // only applies when isTopLevel = true
    std::vector<RhiRtGeometryDesc> bottomLevelGeometries; // only applies when isTopLevel = false
    RhiRtAccelStructBuildFlags buildFlags = RhiRtAccelStructBuildFlags::None;
    std::string debugName;
    bool isTopLevel = false;
    bool isVirtual = false;

    AccelStructDesc& setTopLevelMaxInstances(uint64_t value) { topLevelMaxInstances = value; isTopLevel = true; return *this; }
    AccelStructDesc& addBottomLevelGeometry(const RhiRtGeometryDesc& value) { bottomLevelGeometries.push_back(value); isTopLevel = false; return *this; }
    AccelStructDesc& setBuildFlags(RhiRtAccelStructBuildFlags value) { buildFlags = value; return *this; }
    AccelStructDesc& setDebugName(const std::string& value) { debugName = value; return *this; }
    AccelStructDesc& setIsTopLevel(bool value) { isTopLevel = value; return *this; }
    AccelStructDesc& setIsVirtual(bool value) { isVirtual = value; return *this; }

    // UnityRHI: conversion from the C ABI wire format.
    AccelStructDesc() = default;
    AccelStructDesc(const RhiRtAccelStructDesc& w,
        const RhiRtGeometryDesc* geometries, uint32_t geometryCount, const char* name)
        : topLevelMaxInstances(w.topLevelMaxInstances)
        , bottomLevelGeometries(geometries, geometries + (geometries ? geometryCount : 0))
        , buildFlags(w.buildFlags)
        , debugName(name ? name : "")
        , isTopLevel(w.isTopLevel != 0)
        , isVirtual(w.isVirtual != 0)
    {
    }
};

// Mirrors nvrhi::rt::PipelineShaderDesc (strings live only for the call).
struct RhiRtPipelineShaderDesc
{
    const char* exportName; // may be null/empty: the shader's entry name is used
    uint64_t shader;        // Shader*
    uint64_t bindingLayout; // BindingLayout*, may be 0
};

// Mirrors nvrhi::rt::PipelineHitGroupDesc.
struct RhiRtPipelineHitGroupDesc
{
    const char* exportName;
    uint64_t closestHitShader;   // Shader*, may be 0
    uint64_t anyHitShader;       // Shader*, may be 0
    uint64_t intersectionShader; // Shader*, may be 0
    uint64_t bindingLayout;      // BindingLayout*, may be 0
    uint32_t isProceduralPrimitive;
};

// Pipeline-level parameters of nvrhi::rt::PipelineDesc (shader/hit-group/
// global-layout arrays travel separately).
struct RhiRtPipelineDesc
{
    uint32_t maxPayloadSize;
    uint32_t maxAttributeSize;
    uint32_t maxRecursionDepth;
    uint32_t allowOpacityMicromaps;
    int32_t hlslExtensionsUAV; // NVAPI extension slot; unsupported (must be -1)
};

// Mirrors nvrhi::rt::ShaderTableDesc (debugName travels as a separate argument).
struct RhiRtShaderTableDesc
{
    uint32_t isCached;
    uint32_t maxEntries;
};

// Mirrors nvrhi::BindingSetItem. rawData carries the union payload:
// BufferRange {byteOffset, byteSize} for buffers, or the packed
// TextureSubresourceSet for textures.
struct RhiBindingSetItem
{
    void* resource;
    uint32_t slot;
    uint32_t arrayElement;
    ResourceType type;
    TextureDimension dimension; // valid for Texture_SRV, Texture_UAV
    Format format;              // valid for Texture_SRV/UAV, Buffer_SRV/UAV
    uint32_t unused;
    uint64_t rawData[2];

    BufferRange range() const
    {
        return BufferRange{rawData[0], rawData[1]};
    }

    TextureSubresourceSet subresources() const
    {
        TextureSubresourceSet set;
        set.baseMipLevel = uint32_t(rawData[0]);
        set.numMipLevels = uint32_t(rawData[0] >> 32);
        set.baseArraySlice = uint32_t(rawData[1]);
        set.numArraySlices = uint32_t(rawData[1] >> 32);
        return set;
    }
};

static_assert(sizeof(RhiBindingSetItem) == 48, "RhiBindingSetItem must match the C# mirror (48 bytes)");
} // namespace unityrhi
