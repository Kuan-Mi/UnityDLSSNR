#pragma once

// Internal declarations of the UnityRHI D3D12 backend. Class layout, names,
// and logic are ported from NVRHI's d3d12-backend.h (External/nvrhi/src/d3d12),
// Copyright (c) 2014-2021, NVIDIA CORPORATION, MIT license - adapted to record
// into Unity's command list and retire work through GPU-carried markers.
//
// UnityRHI deviations from the original are marked "UnityRHI deviation".
//
// Naming convention: classes and methods with an NVRHI counterpart keep the
// NVRHI name and camelCase (Buffer, Device::createBuffer); UnityRHI-specific
// APIs use PascalCase (Device::ReleaseResource) and C-ABI types keep the Rhi
// prefix (RhiBufferDesc, the POD wire twin of BufferDesc).

#include <directx/d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RhiTypes.h"
#include "common/state-tracking.h"

// Mirrors NVRHI_D3D12_WITH_NVAPI; set to 1 by CMake when the NVAPI SDK is
// linked (driver-level ray tracing validation).
#ifndef UNITYRHI_WITH_NVAPI
#define UNITYRHI_WITH_NVAPI 0
#endif

#include "UnityD3D12Compat.h"

namespace unityrhi
{
using Microsoft::WRL::ComPtr;

typedef uint32_t DescriptorIndex;
typedef uint32_t RootParameterIndex;

constexpr DescriptorIndex c_InvalidDescriptorIndex = ~0u;
constexpr RootParameterIndex c_InvalidRootParameterIndex = ~0u;
constexpr uint32_t c_ConstantBufferOffsetSizeAlignment = 256;

// nvrhi::DeviceDesc / CommandListParameters defaults.
constexpr uint32_t c_DefaultShaderResourceViewHeapSize = 16384;
constexpr uint32_t c_DefaultSamplerHeapSize = 1024;
constexpr uint32_t c_DefaultRenderTargetViewHeapSize = 256;
constexpr uint32_t c_DefaultDepthStencilViewHeapSize = 64;
constexpr uint64_t c_DefaultUploadChunkSize = 64 * 1024;
constexpr uint64_t c_DefaultScratchChunkSize = 64 * 1024;
constexpr uint64_t c_DefaultScratchMaxMemory = 1024ull * 1024 * 1024;

template <typename T>
constexpr T align(T value, T alignment)
{
    return (value + alignment - T(1)) & ~(alignment - T(1));
}

// Mirrors nvrhi::d3d12::Context (minus queues - Unity owns those).
struct Context
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Device5> device5; // null when DXR is unavailable
    ComPtr<ID3D12Device10> device10; // null when enhanced barriers are unavailable
    ComPtr<ID3D12CommandSignature> dispatchIndirectSignature;
    ComPtr<ID3D12CommandSignature> drawIndirectSignature;
    ComPtr<ID3D12CommandSignature> drawIndexedIndirectSignature;

    void error(const std::string& message) const;
    void info(const std::string& message) const;
};

// Port of nvrhi::d3d12::StaticDescriptorHeap: a CPU descriptor heap with an
// optional shader-visible mirror, freelist allocation, and growth.
class StaticDescriptorHeap
{
public:
    explicit StaticDescriptorHeap(const Context& context);
    HRESULT allocateResources(D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32_t numDescriptors, bool shaderVisible);
    DescriptorIndex allocateDescriptors(uint32_t count);
    DescriptorIndex allocateDescriptor();
    void releaseDescriptors(DescriptorIndex baseIndex, uint32_t count);
    void releaseDescriptor(DescriptorIndex index);
    void copyToShaderVisibleHeap(DescriptorIndex index, uint32_t count = 1);

    D3D12_CPU_DESCRIPTOR_HANDLE getCpuHandle(DescriptorIndex index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE getCpuHandleShaderVisible(DescriptorIndex index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE getGpuHandle(DescriptorIndex index) const;
    D3D12_DESCRIPTOR_HEAP_TYPE getHeapType() const { return m_HeapType; }
    ID3D12DescriptorHeap* getHeap() const { return m_Heap.Get(); }
    ID3D12DescriptorHeap* getShaderVisibleHeap() const { return m_ShaderVisibleHeap.Get(); }

private:
    HRESULT Grow(uint32_t minRequiredSize);

    const Context& m_Context;
    ComPtr<ID3D12DescriptorHeap> m_Heap;
    ComPtr<ID3D12DescriptorHeap> m_ShaderVisibleHeap;
    D3D12_DESCRIPTOR_HEAP_TYPE m_HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    D3D12_CPU_DESCRIPTOR_HANDLE m_StartCpuHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_StartCpuHandleShaderVisible{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_StartGpuHandleShaderVisible{};
    uint32_t m_Stride = 0;
    uint32_t m_NumDescriptors = 0;
    std::vector<bool> m_AllocatedDescriptors;
    DescriptorIndex m_SearchStart = 0;
    uint32_t m_NumAllocatedDescriptors = 0;
    std::mutex m_Mutex;

    // UnityRHI deviation from NVRHI: command lists recorded before a Grow may
    // still reference the old heaps (SetDescriptorHeaps, descriptor tables),
    // so retired heaps are kept alive until the device shuts down. Growth
    // doubles, so at most ~2x the final heap size is retained.
    std::vector<ComPtr<ID3D12DescriptorHeap>> m_RetiredHeaps;
};

struct BindingLayout;
struct RootSignature;
class Device;

// Mirrors nvrhi::d3d12::DeviceResources.
struct DeviceResources
{
    StaticDescriptorHeap shaderResourceViewHeap;
    StaticDescriptorHeap samplerHeap;
    StaticDescriptorHeap renderTargetViewHeap;
    StaticDescriptorHeap depthStencilViewHeap;

    // Root-signature cache. UnityRHI deviation: keyed on layout pointers via
    // weak_ptr instead of NVRHI's refcounted cache entries.
    std::unordered_multimap<size_t, std::weak_ptr<RootSignature>> rootsigCache;

    // Port of nvrhi DeviceResources::getFormatPlaneCount (d3d12-texture.cpp):
    // cached D3D12_FEATURE_FORMAT_INFO plane counts; 0 = unsupported format.
    uint8_t getFormatPlaneCount(DXGI_FORMAT format);

    explicit DeviceResources(const Context& context)
        : shaderResourceViewHeap(context)
        , samplerHeap(context)
        , renderTargetViewHeap(context)
        , depthStencilViewHeap(context)
        , m_Context(context)
    {
    }

private:
    const Context& m_Context;
    std::unordered_map<DXGI_FORMAT, uint8_t> m_DxgiFormatPlaneCounts;
};

// Base of every RHI object exposed to C# as an opaque handle. The managed
// wrapper owns the native object and the caller controls its lifetime.
// UnityRHI deviation: replaces NVRHI's RefCountPtr model. Resource states are
// tracked per command stream by CommandListResourceStateTracker via the
// Buffer/TextureStateExtension bases (see state-tracking.h).
struct RhiResource
{
    enum class Kind : uint32_t
    {
        Buffer,
        Texture,
        Sampler,
        Shader,
        BindingLayout,
        BindingSet,
        ComputePipeline,
        ShaderLibrary,
        AccelStruct,
        RayTracingPipeline,
        ShaderTable,
        DescriptorTable,
        InputLayout,
        Framebuffer,
        GraphicsPipeline,
        Heap,
        StagingTexture,
        EventQuery,
        TimerQuery,

        Count
    };

    Kind kind;
    std::string debugName;
    ComPtr<ID3D12Resource> resource; // null for samplers/shaders/bindings
    bool unityOwnedResource = false;

    // Recording instance of the last replay that referenced this resource;
    // 0 means the GPU never saw it and it can be destroyed immediately.
    uint32_t lastUseFenceValue = 0;

    // One reference belongs to the public handle returned to C#. Submitted
    // command streams add references for every encoded resource and release
    // them when the stream allocation retires. GPU completion remains a
    // separate concern handled by lastUseFenceValue/CompletedLifetimeInstance.
    uint32_t referenceCount = 1;

    virtual ~RhiResource() = default;

protected:
    explicit RhiResource(Kind k) : kind(k) {}
};

struct Heap final : RhiResource
{
    Heap() : RhiResource(Kind::Heap) {}

    HeapDesc desc{};
    ComPtr<ID3D12Heap> heap;
};

struct EventQuery final : RhiResource
{
    EventQuery() : RhiResource(Kind::EventQuery) {}
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
    bool started = false;
};

struct TimerQuery final : RhiResource
{
    TimerQuery() : RhiResource(Kind::TimerQuery) {}
    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource> resolveBuffer;
    // Recording instance whose GPU-carried lifetime marker follows this
    // query's ResolveQueryData in the same command list. Polling this value
    // avoids a queue-access plugin event and its forced submission boundary.
    std::atomic<uint32_t> completionInstance{0};
    bool started = false;
    bool resolved = false;
    float time = 0.f;
};

// Mirrors nvrhi::d3d12::Buffer (which derives from BufferStateExtension the
// same way for the state tracker).
struct Buffer final : RhiResource, BufferStateExtension
{
    Buffer(const Context& context, DeviceResources& resources, const BufferDesc& desc_)
        : RhiResource(Kind::Buffer)
        , BufferStateExtension(this->desc)
        , desc(desc_)
        , m_Context(context)
        , m_Resources(resources)
    {
        debugNameRef = &debugName;
    }
    ~Buffer() override;

    BufferDesc desc{};
    D3D12_RESOURCE_DESC resourceDesc{};
    D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
    Heap* heap = nullptr; // caller-managed lifetime, matching UnityRHI ownership policy
    HANDLE sharedHandle = nullptr;

    void postCreate();

    void createCBV(size_t descriptor, BufferRange range) const;
    void createSRV(size_t descriptor, Format format, BufferRange range, ResourceType type) const;
    void createUAV(size_t descriptor, Format format, BufferRange range, ResourceType type) const;
    static void createNullSRV(size_t descriptor, Format format, const Context& context);
    static void createNullUAV(size_t descriptor, Format format, const Context& context);

    DescriptorIndex getClearUAV();

    const Context& m_Context;
    DeviceResources& m_Resources;

private:
    DescriptorIndex m_ClearUAV = c_InvalidDescriptorIndex;
};

// Mirrors nvrhi::d3d12::Texture (which derives from TextureStateExtension the
// same way for the state tracker).
struct Texture final : RhiResource, TextureStateExtension
{
    Texture(const Context& context, DeviceResources& resources, const TextureDesc& desc_)
        : RhiResource(Kind::Texture)
        , TextureStateExtension(this->desc)
        , desc(desc_)
        , m_Context(context)
        , m_Resources(resources)
    {
        // Match nvrhi::d3d12::Texture: the native resource is created in
        // desc.initialState, so keepInitialState tracking must start there
        // instead of treating the first command list as COMMON.
        TextureStateExtension::stateInitialized = true;
        debugNameRef = &debugName;
    }

    ~Texture() override;

    TextureDesc desc{};
    D3D12_RESOURCE_DESC resourceDesc{};
    uint8_t planeCount = 1; // nvrhi Texture::planeCount (from getFormatPlaneCount)
    Heap* heap = nullptr; // caller-managed lifetime, matching UnityRHI ownership policy
    HANDLE sharedHandle = nullptr;

    void postCreate();

    void createSRV(size_t descriptor, Format format, TextureDimension dimension, TextureSubresourceSet subresources) const;
    void createUAV(size_t descriptor, Format format, TextureDimension dimension, TextureSubresourceSet subresources) const;
    void createRTV(size_t descriptor, Format format, TextureSubresourceSet subresources) const;
    void createDSV(size_t descriptor, TextureSubresourceSet subresources, bool isReadOnly) const;

    // Lazily allocated whole-resource views used by the replay-time clears
    // (nvrhi caches these in Texture::getNativeView).
    DescriptorIndex getClearRTV();
    DescriptorIndex getClearDSV();
    DescriptorIndex getClearRTV(TextureSubresourceSet subresources);
    DescriptorIndex getClearDSV(TextureSubresourceSet subresources);
    DescriptorIndex getClearUAV(MipLevel mipLevel, ArraySlice baseArraySlice,
        ArraySlice arraySize, Format format = Format::UNKNOWN);
    uint64_t getNativeView(RhiObjectType objectType, Format format,
        TextureSubresourceSet subresources, TextureDimension dimension, bool isReadOnlyDSV);

    const Context& m_Context;
    DeviceResources& m_Resources;

private:
    DescriptorIndex m_ClearRTV = c_InvalidDescriptorIndex;
    DescriptorIndex m_ClearDSV = c_InvalidDescriptorIndex;
    std::unordered_map<uint64_t, DescriptorIndex> m_ClearRTVs;
    std::unordered_map<uint64_t, DescriptorIndex> m_ClearDSVs;
    std::unordered_map<uint64_t, DescriptorIndex> m_ClearUAVs;
    std::unordered_map<std::string, DescriptorIndex> m_NativeSRVs;
    std::unordered_map<std::string, DescriptorIndex> m_NativeUAVs;
    std::unordered_map<std::string, DescriptorIndex> m_NativeRTVs;
    std::unordered_map<std::string, DescriptorIndex> m_NativeDSVs;
};

// Port of nvrhi::d3d12::StagingTexture. The backing Buffer is owned by this
// wrapper; the wrapper handle itself follows UnityRHI's caller-managed policy.
struct StagingTexture final : RhiResource
{
    StagingTexture() : RhiResource(Kind::StagingTexture) {}

    struct SliceRegion
    {
        uint64_t offset = 0;
        uint64_t size = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    };

    TextureDesc desc{};
    D3D12_RESOURCE_DESC resourceDesc{};
    Buffer* buffer = nullptr;
    CpuAccessMode cpuAccess = CpuAccessMode::None;
    std::vector<uint64_t> subresourceOffsets;
    SliceRegion mappedRegion{};
    CpuAccessMode mappedAccess = CpuAccessMode::None;

    SliceRegion getSliceRegion(ID3D12Device* device, const RhiTextureSlice& slice) const;
    uint64_t getSizeInBytes(ID3D12Device* device) const;
    void computeSubresourceOffsets(ID3D12Device* device);
};

// Mirrors nvrhi::d3d12::Sampler: the D3D12_SAMPLER_DESC is prebuilt at
// creation; createDescriptor writes it into a heap slot.
struct Sampler final : RhiResource
{
    Sampler(const Context& context, const RhiSamplerDesc& desc_);

    void createDescriptor(size_t descriptor) const;

    RhiSamplerDesc desc{};

private:
    const Context& m_Context;
    D3D12_SAMPLER_DESC m_d3d12desc{};
};

// Mirrors nvrhi::d3d12::Shader: immutable DXIL bytecode + metadata.
// UnityRHI deviation: the bytecode is a shared_ptr so ShaderLibraryEntry
// shaders can share the library's blob without NVRHI's RefCountPtr model.
struct Shader final : RhiResource
{
    Shader() : RhiResource(Kind::Shader) {}
    ShaderDesc desc;
    std::shared_ptr<std::vector<uint8_t>> bytecode;

    // Mirrors nvrhi Shader::getBytecode / ShaderLibraryEntry::getBytecode.
    void getBytecode(const void** ppBytecode, size_t* pSize) const
    {
        if (ppBytecode) *ppBytecode = bytecode ? bytecode->data() : nullptr;
        if (pSize) *pSize = bytecode ? bytecode->size() : 0;
    }
};

// Mirrors nvrhi::d3d12::ShaderLibrary. getShader (Device::createLibraryShader)
// creates Shader entries that share this bytecode.
struct ShaderLibrary final : RhiResource
{
    ShaderLibrary() : RhiResource(Kind::ShaderLibrary) {}
    std::shared_ptr<std::vector<uint8_t>> bytecode;
};

// Mirrors nvrhi::d3d12::BindingLayout: precomputes the root-parameter segment
// this layout occupies (descriptor tables for SRV/UAV/CBV and samplers, root
// descriptors for volatile CBs, root constants for push constants).
struct BindingLayout final : RhiResource
{
    BindingLayout(const RhiBindingLayoutDesc& desc, const RhiBindingLayoutItem* items, uint32_t itemCount);

    // Port of nvrhi::d3d12::BindlessLayout::BindlessLayout. UnityRHI deviation:
    // NVRHI models bindless layouts as a separate IBindingLayout subclass; the
    // C ABI has no interface dispatch, so both live in BindingLayout and
    // isBindless selects the root-signature construction path.
    BindingLayout(const RhiBindlessLayoutDesc& desc, const RhiBindingLayoutItem* items, uint32_t itemCount);

    RhiBindingLayoutDesc desc{};
    std::vector<RhiBindingLayoutItem> bindings;
    uint64_t uniqueId = 0;

    bool isBindless = false;
    RhiBindlessLayoutDesc bindlessDesc{};
    std::vector<D3D12_DESCRIPTOR_RANGE1> bindlessRanges; // unbounded, one per register space
    D3D12_ROOT_PARAMETER1 bindlessRootParameter{};

    uint32_t pushConstantByteSize = 0;
    RootParameterIndex rootParameterPushConstants = c_InvalidRootParameterIndex;

    std::vector<std::pair<RootParameterIndex, D3D12_ROOT_DESCRIPTOR1>> rootParametersVolatileCB;

    uint32_t descriptorTableSizeSamplers = 0;
    RootParameterIndex rootParameterSamplers = c_InvalidRootParameterIndex;
    std::vector<D3D12_DESCRIPTOR_RANGE1> descriptorRangesSamplers;

    uint32_t descriptorTableSizeSRVetc = 0;
    RootParameterIndex rootParameterSRVetc = c_InvalidRootParameterIndex;
    std::vector<D3D12_DESCRIPTOR_RANGE1> descriptorRangesSRVetc;
    std::vector<RhiBindingLayoutItem> bindingLayoutsSRVetc;

    // The root parameters this layout contributes; indices above are relative
    // to the start of this segment. Descriptor-table parameters point into the
    // descriptorRanges* vectors, which are immutable after construction.
    std::vector<D3D12_ROOT_PARAMETER1> rootParameters;
};

// Mirrors nvrhi::d3d12::RootSignature (cached per layout combination).
struct RootSignature
{
    ~RootSignature();

    ComPtr<ID3D12RootSignature> handle;
    std::vector<std::pair<BindingLayout*, RootParameterIndex>> pipelineLayouts;
    uint32_t pushConstantByteSize = 0;
    RootParameterIndex rootParameterPushConstants = c_InvalidRootParameterIndex;
    size_t hash = 0;
    bool allowInputLayout = false;
    DeviceResources* m_Resources = nullptr;
};

// Mirrors nvrhi::d3d12::BindingSet: descriptors are staged into the CPU heaps
// at creation and copied to the shader-visible mirrors.
struct BindingSet final : RhiResource
{
    BindingSet(const Context& context, DeviceResources& resources)
        : RhiResource(Kind::BindingSet)
        , m_Context(context)
        , m_Resources(resources)
    {
    }
    ~BindingSet() override;

    BindingLayout* layout = nullptr;
    std::vector<RhiBindingSetItem> bindings; // desc.bindings

    std::vector<std::pair<RootParameterIndex, Buffer*>> rootParametersVolatileCB;

    DescriptorIndex descriptorTableSRVetc = 0;
    // UnityRHI deviation: NVRHI's BindingSet owns a ref-counted layout handle,
    // so its destructor can read layout->descriptorTableSize*. Our C ABI
    // handles are independent raw pointers, so cache the allocation sizes at
    // creation time and release exactly what this binding set allocated.
    uint32_t descriptorTableSizeSRVetc = 0;
    RootParameterIndex rootParameterIndexSRVetc = c_InvalidRootParameterIndex;
    bool descriptorTableValidSRVetc = false;

    DescriptorIndex descriptorTableSamplers = 0;
    // See descriptorTableSizeSRVetc.
    uint32_t descriptorTableSizeSamplers = 0;
    RootParameterIndex rootParameterIndexSamplers = c_InvalidRootParameterIndex;
    bool descriptorTableValidSamplers = false;

    bool hasUavBindings = false;
    std::vector<uint16_t> bindingsThatNeedTransitions;

    // Returns false when the bindings do not satisfy the layout.
    bool createDescriptors();

    const Context& m_Context;
    DeviceResources& m_Resources;
};

// Mirrors nvrhi::d3d12::ComputePipeline.
struct ComputePipeline final : RhiResource
{
    ComputePipeline() : RhiResource(Kind::ComputePipeline) {}
    Shader* shader = nullptr;
    std::vector<BindingLayout*> layouts;
    std::shared_ptr<RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
};

// Mirrors nvrhi::d3d12::InputLayout: prebuilt D3D12 input element array plus
// the per-slot vertex strides used when binding vertex buffers.
struct InputLayout final : RhiResource
{
    InputLayout() : RhiResource(Kind::InputLayout) {}

    // inputElements' SemanticName pointers alias semanticNames entries.
    std::vector<std::string> semanticNames;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    std::unordered_map<uint32_t, uint32_t> elementStrides; // bufferIndex -> stride
};

// Mirrors nvrhi::d3d12::Framebuffer: RTV/DSV descriptors for a set of
// attachments, allocated from the CPU-only RTV/DSV heaps.
struct Framebuffer final : RhiResource
{
    explicit Framebuffer(DeviceResources& resources)
        : RhiResource(Kind::Framebuffer)
        , m_Resources(resources)
    {
    }
    ~Framebuffer() override;

    std::vector<Texture*> colorTextures;
    Texture* depthTexture = nullptr;
    std::vector<DescriptorIndex> RTVs;
    DescriptorIndex DSV = c_InvalidDescriptorIndex;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sampleCount = 1;
    uint32_t sampleQuality = 0;

    DeviceResources& m_Resources;
};

// Mirrors nvrhi::d3d12::GraphicsPipeline.
struct GraphicsPipeline final : RhiResource
{
    GraphicsPipeline() : RhiResource(Kind::GraphicsPipeline) {}

    RhiGraphicsPipelineDesc desc{};
    InputLayout* inputLayout = nullptr;
    std::vector<BindingLayout*> layouts;
    std::shared_ptr<RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
};

// Mirrors nvrhi::d3d12::DescriptorTable: mutable descriptor storage associated
// with a bindless layout. The layout selects the resource or sampler heap.
struct DescriptorTable final : RhiResource
{
    explicit DescriptorTable(DeviceResources& resources)
        : RhiResource(Kind::DescriptorTable)
        , m_Resources(resources)
    {
    }
    ~DescriptorTable() override;

    uint32_t capacity = 0;
    DescriptorIndex firstDescriptor = 0;
    BindingLayout* layout = nullptr;

    // NVRHI leaves descriptor-table resource states to the application because
    // table contents are otherwise opaque at command-recording time. Unity-owned
    // resources are different: their transitions must go through Unity's active
    // command-list state tracker. Keep a sparse-by-value copy of those bindings
    // so replay can request their states before the table is consumed.
    std::vector<RhiBindingSetItem> unityResourceBindings;

    bool isSamplerTable() const;
    StaticDescriptorHeap& getDescriptorHeap() const;

private:
    DeviceResources& m_Resources;
};

// Mirrors nvrhi::d3d12::AccelStruct. The AS storage is a plugin-created
// buffer (isAccelStructStorage); the desc kept here is sanitized - geometry
// buffer handles are cleared at creation, exactly like the original.
struct AccelStruct final : RhiResource
{
    explicit AccelStruct(const Context& context)
        : RhiResource(Kind::AccelStruct)
        , m_Context(context)
    {
    }

    Buffer* dataBuffer = nullptr; // released by Device::DestroyResource
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> dxrInstances;
    AccelStructDesc desc{}; // sanitized (geometry buffer handles nulled)
    bool allowUpdate = false;
    bool compacted = false;

    void createSRV(size_t descriptor) const;
    uint64_t getDeviceAddress() const;

    const Context& m_Context;
};

// Mirrors nvrhi::d3d12::RayTracingPipeline (DXR state object + export table).
struct RayTracingPipeline final : RhiResource
{
    explicit RayTracingPipeline(const Context& context)
        : RhiResource(Kind::RayTracingPipeline)
        , m_Context(context)
    {
    }

    std::unordered_map<BindingLayout*, std::shared_ptr<RootSignature>> localRootSignatures;
    std::shared_ptr<RootSignature> globalRootSignature;
    ComPtr<ID3D12StateObject> pipelineState;
    ComPtr<ID3D12StateObjectProperties> pipelineInfo;

    struct ExportTableEntry
    {
        BindingLayout* bindingLayout;
        const void* pShaderIdentifier;
    };

    std::unordered_map<std::string, ExportTableEntry> exports;
    uint32_t maxLocalRootParameters = 0;

    const ExportTableEntry* getExport(const char* name);
    uint32_t getShaderTableEntrySize() const;
    bool hasLocalResources() const { return maxLocalRootParameters != 0; }

    const Context& m_Context;
};

// Mirrors nvrhi::d3d12::ShaderTableState.
class ShaderTableState
{
public:
    uint32_t committedVersion = 0;
    ID3D12DescriptorHeap* descriptorHeapSRV = nullptr;
    ID3D12DescriptorHeap* descriptorHeapSamplers = nullptr;
    D3D12_DISPATCH_RAYS_DESC dispatchRaysTemplate = {};
};

// Mirrors nvrhi::d3d12::ShaderTable.
struct ShaderTable final : RhiResource
{
    ShaderTable(const Context& context, RayTracingPipeline* pipeline_, const RhiRtShaderTableDesc& desc_)
        : RhiResource(Kind::ShaderTable)
        , pipeline(pipeline_)
        , desc(desc_)
        , m_Context(context)
    {
    }

    struct Entry
    {
        const void* pShaderIdentifier = nullptr;
        BindingSet* localBindings = nullptr;
    };

    RayTracingPipeline* pipeline = nullptr;
    RhiRtShaderTableDesc desc{};

    Entry rayGenerationShader = {};
    std::vector<Entry> missShaders;
    std::vector<Entry> callableShaders;
    std::vector<Entry> hitGroups;

    uint32_t version = 0;

    Buffer* cache = nullptr; // released by Device::DestroyResource
    ShaderTableState cacheState;

    uint32_t getNumEntries() const;
    size_t getUploadSize() const { return pipeline->getShaderTableEntrySize() * size_t(getNumEntries()); }
    bool verifyExport(const RayTracingPipeline::ExportTableEntry* pExport, BindingSet* bindings) const;
    void setRayGenerationShader(const char* exportName, BindingSet* bindings = nullptr);
    int addMissShader(const char* exportName, BindingSet* bindings = nullptr);
    int addHitGroup(const char* exportName, BindingSet* bindings = nullptr);
    int addCallableShader(const char* exportName, BindingSet* bindings = nullptr);
    void clearMissShaders();
    void clearHitShaders();
    void clearCallableShaders();

    bool isStateValid(ShaderTableState const& state, DeviceResources const& resources) const;
    void bake(uint8_t* cpuVA, D3D12_GPU_VIRTUAL_ADDRESS gpuVA, DeviceResources& resources,
        ShaderTableState& state);

    const Context& m_Context;
};

// Port of nvrhi::d3d12::BufferChunk / UploadManager: a fence-gated chunked
// suballocator backing writeBuffer and volatile constant buffers.
struct BufferChunk
{
    static constexpr uint64_t c_sizeAlignment = 4096; // GPU page size

    ComPtr<ID3D12Resource> buffer;
    uint32_t version = 0; // recording instance of the last use (BeginRecordingInstance); 0 = free
    uint64_t bufferSize = 0;
    uint64_t writePointer = 0;
    void* cpuVA = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
    uint32_t identifier = 0;
    uint32_t pendingUploadTickets = 0;

    ~BufferChunk();
};

struct UploadTicket
{
    std::atomic<uint32_t> referenceCount{1};
    std::shared_ptr<BufferChunk> chunk;
    uint64_t offset = 0;
    uint64_t size = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
};

class UploadManager
{
public:
    UploadManager(const Context& context, Device* parent, uint64_t defaultChunkSize,
        uint64_t memoryLimit, bool isScratchBuffer)
        : m_Context(context)
        , m_Parent(parent)
        , m_DefaultChunkSize(defaultChunkSize)
        , m_MemoryLimit(memoryLimit)
        , m_IsScratchBuffer(isScratchBuffer)
    {
    }

    bool suballocateBuffer(uint64_t size, ID3D12GraphicsCommandList* pCommandList,
        ID3D12Resource** pBuffer, uint64_t* pOffset,
        void** pCpuVA, D3D12_GPU_VIRTUAL_ADDRESS* pGpuVA, uint32_t currentVersion, uint32_t alignment,
        std::shared_ptr<BufferChunk>* pTicketChunk = nullptr);
    UploadTicket* stageBuffer(const void* data, uint64_t size, uint32_t alignment);
    UploadTicket* stageTexture(Texture* texture, uint32_t arraySlice, uint32_t mipLevel,
        const void* data, uint64_t dataSize, uint64_t rowPitch, uint64_t depthPitch);
    UploadTicket* allocateTicket(uint64_t size, uint32_t alignment, void** cpuVA);
    bool resolveTicket(UploadTicket* ticket, uint32_t fenceValue, ID3D12Resource** buffer,
        uint64_t* offset, D3D12_GPU_VIRTUAL_ADDRESS* gpuVA);
    void retainTicket(UploadTicket* ticket);
    void releaseTicket(UploadTicket* ticket);

private:
    std::shared_ptr<BufferChunk> createChunk(uint64_t size);
    const Context& m_Context;
    Device* m_Parent;
    uint64_t m_DefaultChunkSize;
    uint64_t m_MemoryLimit = 0;
    uint64_t m_AllocatedMemory = 0;
    bool m_IsScratchBuffer = false;
    std::vector<std::shared_ptr<BufferChunk>> m_ChunkPool;
    std::shared_ptr<BufferChunk> m_CurrentChunk;
    std::mutex m_Mutex;
};

// Mirrors nvrhi::d3d12::VolatileConstantBufferBinding.
struct VolatileConstantBufferBinding
{
    RootParameterIndex bindingPoint;
    Buffer* buffer;
    D3D12_GPU_VIRTUAL_ADDRESS address;
};

// Mirrors nvrhi::VertexBufferBinding (handles instead of RefCountPtr).
struct VertexBufferBinding
{
    Buffer* buffer = nullptr;
    uint32_t slot = 0;
    uint64_t offset = 0;
};

// Replay-time command-list state: plays the role of nvrhi::d3d12::CommandList's
// recording state for the command-stream replayer (CommandStream.cpp decodes
// opcodes and drives these methods against Unity's live command list). Method
// bodies live in the same files as their NVRHI CommandList counterparts
// (d3d12-state-tracking.cpp etc.); members mirror the CommandList m_* fields.
struct ReplayContext
{
    explicit ReplayContext(const Context* deviceContext)
        : stateTracker(deviceContext)
    {
    }

    ID3D12GraphicsCommandList* commandList = nullptr;
    ComPtr<ID3D12GraphicsCommandList4> commandList4; // null when DXR is unavailable
    ComPtr<ID3D12GraphicsCommandList7> commandList7; // null when enhanced barriers are unavailable
    uint32_t fenceValue = 0;

    // Mirrors NVRHI CommandList::m_StateTracker / m_D3DBarriers /
    // m_EnableAutomaticBarriers / m_BindingStatesDirty.
    CommandListResourceStateTracker stateTracker;
    std::vector<D3D12_RESOURCE_BARRIER> d3dBarriers;
    std::vector<D3D12_TEXTURE_BARRIER> d3dTextureBarriers;
    std::vector<D3D12_BUFFER_BARRIER> d3dBufferBarriers;
    bool enableAutomaticBarriers = true;
    bool bindingStatesDirty = false;

    // Mirrors NVRHI CommandList state used by the compute path.
    ComputePipeline* currentPipeline = nullptr;
    Buffer* currentIndirectParams = nullptr;
    std::vector<RhiResource*> currentBindings; // BindingSet or DescriptorTable
    std::vector<VolatileConstantBufferBinding> currentComputeVolatileCBs;
    std::unordered_map<Buffer*, D3D12_GPU_VIRTUAL_ADDRESS> volatileConstantBufferAddresses;
    bool anyVolatileBufferWrites = false;

    // Mirrors NVRHI CommandList state used by the graphics path.
    // UnityRHI deviation: SetGraphicsState always rebinds the full state
    // (streams are short-lived), so only the volatile CBs need tracking.
    GraphicsPipeline* currentGraphicsPipeline = nullptr;
    Framebuffer* currentFramebuffer = nullptr;
    Buffer* currentGraphicsIndirectParams = nullptr;
    Buffer* currentGraphicsIndirectCountBuffer = nullptr;
    std::vector<VolatileConstantBufferBinding> currentGraphicsVolatileCBs;
    bool graphicsStateActive = false; // last-set state (push constants routing)

    // Mirrors NVRHI CommandList state used by the ray-tracing path.
    ShaderTable* currentShaderTable = nullptr;
    std::vector<RhiResource*> currentRayTracingBindings; // BindingSet or DescriptorTable
    std::unordered_map<ShaderTable*, ShaderTableState> uncachedShaderTableStates;
    bool rayTracingStateActive = false; // last-set state (push constants routing)

    // Reused by CommandStream.cpp while decoding variable-length payloads.
    // Keeping these on the replay context avoids per-command heap allocation;
    // currentBindings/currentRayTracingBindings remain the persistent NVRHI state.
    std::vector<RhiResource*> decodedBindings;
    std::vector<VertexBufferBinding> decodedVertexBuffers;
    std::vector<RhiRtGeometryDesc> decodedGeometries;
    std::vector<RhiRtInstanceDesc> decodedInstances;
    // Filled by the command-stream loop so backend validation can identify the
    // exact wire command that supplied a bad handle.
    uint32_t diagnosticCommandIndex = UINT32_MAX;
    bool diagnosticReuseBindings = false;

    ID3D12DescriptorHeap* currentHeapSRVetc = nullptr;
    ID3D12DescriptorHeap* currentHeapSamplers = nullptr;

    // Ports of the nvrhi CommandList methods of the same names, with bodies in
    // the files matching their NVRHI location. Signatures deviate where NVRHI
    // takes desc structs (ComputeState etc.) - the C ABI flattens those.

    // d3d12-state-tracking.cpp. UnityRHI deviation: resources imported from
    // Unity are owned by Unity's state tracker, so require* forwards their
    // state requests to it immediately instead of entering our tracker.
    void requireTextureState(Texture* texture, TextureSubresourceSet subresources, ResourceStates state);
    void requireBufferState(Buffer* buffer, ResourceStates state);
    void commitBarriers();
    void setResourceStatesForBindingSet(BindingSet* bindingSet);
    void beginTrackingTextureState(Texture* texture, TextureSubresourceSet subresources, ResourceStates state);
    void beginTrackingBufferState(Buffer* buffer, ResourceStates state);
    void setBufferState(Buffer* buffer, ResourceStates state);
    void setTextureState(Texture* texture, ResourceStates state);
    void setTextureState(Texture* texture, TextureSubresourceSet subresources, ResourceStates state);
    void setPermanentTextureState(Texture* texture, ResourceStates state);
    void setPermanentBufferState(Buffer* buffer, ResourceStates state);
    void setEnableUavBarriersForTexture(Texture* texture, bool enable);
    void setEnableUavBarriersForBuffer(Buffer* buffer, bool enable);
    void clearState();

    // CommandStream.cpp (the d3d12-commandlist.cpp role).
    bool commitDescriptorHeaps();

    // d3d12-resource-bindings.cpp.
    void setResourceStatesForDescriptorTable(DescriptorTable* descriptorTable);
    bool setComputeBindings(const std::vector<RhiResource*>& bindings, uint32_t bindingUpdateMask,
        Buffer* indirectParams, bool updateIndirectParams, const RootSignature* rootSignature,
        bool graphics = false);

    // d3d12-buffer.cpp.
    bool writeBuffer(Buffer* buffer, UploadTicket* upload, uint64_t destOffsetBytes);
    bool copyBuffer(Buffer* dest, uint64_t destOffsetBytes, Buffer* src, uint64_t srcOffsetBytes, uint64_t dataSizeBytes);
    bool clearBufferUInt(Buffer* buffer, uint32_t clearValue);

    // d3d12-texture.cpp.
    bool writeTexture(Texture* dest, uint32_t arraySlice, uint32_t mipLevel, UploadTicket* upload);
    bool copyTexture(Buffer* dest, uint64_t destOffsetBytes, Texture* src, uint32_t arraySlice, uint32_t mipLevel);
    bool copyTexture(Texture* dest, const RhiTextureSlice& destSlice,
        Texture* src, const RhiTextureSlice& srcSlice);
    bool copyTexture(Texture* dest, const RhiTextureSlice& destSlice,
        StagingTexture* src, const RhiTextureSlice& srcSlice);
    bool copyTexture(StagingTexture* dest, const RhiTextureSlice& destSlice,
        Texture* src, const RhiTextureSlice& srcSlice);
    bool resolveTexture(Texture* dest, TextureSubresourceSet destSubresources,
        Texture* src, TextureSubresourceSet srcSubresources);
    bool clearTextureFloat(Texture* texture, const float color[4]);
    bool clearTextureFloat(Texture* texture, TextureSubresourceSet subresources, const float color[4]);
    bool clearDepthStencilTexture(Texture* texture, bool clearDepth, float depth, bool clearStencil, uint8_t stencil);
    bool clearDepthStencilTexture(Texture* texture, TextureSubresourceSet subresources,
        bool clearDepth, float depth, bool clearStencil, uint8_t stencil);
    bool clearTextureUInt(Texture* texture, TextureSubresourceSet subresources, uint32_t clearColor);

    // d3d12-compute.cpp.
    bool setComputeState(ComputePipeline* pipeline, Buffer* indirectParams,
        const std::vector<RhiResource*>& bindings);
    void updateComputeVolatileBuffers();
    void dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ);
    bool dispatchIndirect(uint64_t offsetBytes);

    // d3d12-graphics.cpp.
    bool setGraphicsState(GraphicsPipeline* pipeline, Framebuffer* framebuffer, const RhiViewport& viewport,
        const float blendConstantColor[4], Buffer* indexBuffer, Format indexFormat, uint64_t indexBufferOffset,
        Buffer* indirectParams, Buffer* indirectCountBuffer,
        const VertexBufferBinding* vertexBuffers, uint32_t vertexBufferCount,
        const std::vector<RhiResource*>& bindings);
    void updateGraphicsVolatileBuffers();
    bool draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance);
    bool drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex,
        uint32_t baseVertex, uint32_t startInstance);
    bool drawIndirect(uint64_t offsetBytes, uint32_t drawCount, bool indexed,
        uint64_t countOffsetBytes = 0, bool useCountBuffer = false);

    // d3d12-raytracing.cpp.
    ShaderTableState& getShaderTableState(ShaderTable* shaderTable);
    bool setRayTracingState(ShaderTable* shaderTable, const std::vector<RhiResource*>& bindings);
    bool dispatchRays(uint32_t width, uint32_t height, uint32_t depth);
    bool buildBottomLevelAccelStruct(AccelStruct* as, const RhiRtGeometryDesc* geometries,
        uint32_t geometryCount, RhiRtAccelStructBuildFlags buildFlags);
    bool buildTopLevelAccelStructInternal(AccelStruct* as, D3D12_GPU_VIRTUAL_ADDRESS instanceData,
        uint32_t numInstances, RhiRtAccelStructBuildFlags buildFlags);
    bool buildTopLevelAccelStruct(AccelStruct* as, const RhiRtInstanceDesc* instances,
        uint32_t instanceCount, RhiRtAccelStructBuildFlags buildFlags);
    bool buildTopLevelAccelStructFromBuffer(AccelStruct* as, Buffer* instanceBuffer,
        uint64_t instanceBufferOffset, uint32_t instanceCount, RhiRtAccelStructBuildFlags buildFlags);
};

// Shared by the compute and ray-tracing state setters when determining which
// binding slots need to be rebound.
uint32_t arrayDifferenceMask(const std::vector<RhiResource*>& a, const std::vector<RhiResource*>& b);

// UnityRHI diagnostics: process-wide count of UAV barriers recorded at replay
// (read via ReplayUavBarrierCount, CommandStream.h). Defined in
// d3d12-state-tracking.cpp.
extern std::atomic<uint64_t> g_uavBarrierCount;

// Wraps Unity's ID3D12Device. Mirrors the resource-creation subset of
// nvrhi::d3d12::Device; command recording/execution is replaced by the
// command-stream replayer (CommandStream.cpp) targeting Unity's list.
class Device
{
public:
    // Created on kUnityGfxDeviceEventInitialize, destroyed on shutdown.
    static Device* Get() { return s_instance; }
    static void Create(ID3D12Device* device, UnityD3D12Interface* unityD3D12);
    static void Destroy(); // logs leaks

    Buffer* createBuffer(const BufferDesc& d);
    Buffer* createBufferFromNativeResource(ID3D12Resource* resource, const BufferDesc& desc);
    Heap* createHeap(const HeapDesc& d);
    MemoryRequirements getBufferMemoryRequirements(Buffer* buffer) const;
    MemoryRequirements getTextureMemoryRequirements(Texture* texture) const;
    MemoryRequirements getAccelStructMemoryRequirements(AccelStruct* accelStruct) const;
    bool bindBufferMemory(Buffer* buffer, Heap* heap, uint64_t offset);
    bool bindTextureMemory(Texture* texture, Heap* heap, uint64_t offset);
    bool bindAccelStructMemory(AccelStruct* accelStruct, Heap* heap, uint64_t offset);
    Texture* createTexture(const TextureDesc& d);
    Texture* createTextureFromNativeResource(ID3D12Resource* resource, const TextureDesc& desc);
    StagingTexture* createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess);
    void* mapStagingTexture(StagingTexture* texture, const RhiTextureSlice& slice,
        CpuAccessMode cpuAccess, uint64_t* outRowPitch);
    void unmapStagingTexture(StagingTexture* texture);
    bool waitForLifetimeInstance(uint32_t instance);
    uint64_t getNativeObject(RhiObjectType objectType) const;
    bool getTextureTiling(Texture* texture, uint32_t* numTiles, RhiPackedMipDesc* packedMipDesc,
        RhiTileShape* tileShape, uint32_t* subresourceTilingCount,
        RhiSubresourceTiling* subresourceTilings) const;
    bool updateTextureTileMappings(Texture* texture, Heap* heap,
        const RhiTiledTextureCoordinate* coordinates, const RhiTiledTextureRegion* regions,
        const uint64_t* byteOffsets, uint32_t regionCount, ID3D12CommandQueue* queue = nullptr);
    Sampler* createSampler(const RhiSamplerDesc& desc, const char* debugName);
    Shader* createShader(const ShaderDesc& d, const void* binary, size_t binarySize);
    BindingLayout* createBindingLayout(const RhiBindingLayoutDesc& desc,
        const RhiBindingLayoutItem* items, uint32_t itemCount, const char* debugName);
    BindingSet* createBindingSet(
        BindingLayout* layout, const RhiBindingSetItem* items, uint32_t itemCount, const char* debugName);
    ComputePipeline* createComputePipeline(
        Shader* shader, BindingLayout* const* layouts, uint32_t layoutCount, const char* debugName);

    // Ports of nvrhi createInputLayout / createFramebuffer /
    // createGraphicsPipeline (d3d12-shader.cpp / d3d12-graphics.cpp).
    InputLayout* createInputLayout(
        const RhiVertexAttributeDesc* attributes, uint32_t attributeCount, const char* debugName);
    Framebuffer* createFramebuffer(Texture* const* colorTextures, uint32_t colorCount,
        Texture* depthTexture, const char* debugName);
    GraphicsPipeline* createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc,
        Shader* vertexShader, Shader* pixelShader, InputLayout* inputLayout,
        BindingLayout* const* layouts, uint32_t layoutCount,
        Framebuffer* framebuffer, const char* debugName);

    // Port of nvrhi createShaderLibrary; createLibraryShader is the C ABI form
    // of IShaderLibrary::getShader (the entry keeps the library's blob alive).
    ShaderLibrary* createShaderLibrary(const void* bytecode, uint64_t byteSize, const char* debugName);
    Shader* createLibraryShader(ShaderLibrary* library, const char* entryName, ShaderType shaderType);

    EventQuery* createEventQuery();
    void setEventQuery(EventQuery* query);
    bool pollEventQuery(EventQuery* query) const;
    void waitEventQuery(EventQuery* query);
    void resetEventQuery(EventQuery* query);
    TimerQuery* createTimerQuery();
    bool pollTimerQuery(TimerQuery* query) const;
    float getTimerQueryTime(TimerQuery* query);
    void resetTimerQuery(TimerQuery* query);

    // Port of nvrhi createBindlessLayout (d3d12-resource-bindings.cpp).
    BindingLayout* createBindlessLayout(const RhiBindlessLayoutDesc& desc,
        const RhiBindingLayoutItem* registerSpaces, uint32_t registerSpaceCount, const char* debugName);

    // Ports of nvrhi createDescriptorTable / resizeDescriptorTable /
    // writeDescriptorTable (d3d12-resource-bindings.cpp).
    DescriptorTable* createDescriptorTable(BindingLayout* layout, const char* debugName);
    void resizeDescriptorTable(DescriptorTable* descriptorTable, uint32_t newSize, bool keepContents);
    bool writeDescriptorTable(DescriptorTable* descriptorTable, const RhiBindingSetItem& binding);
    uint32_t getDescriptorTableFirstDescriptorIndexInHeap(DescriptorTable* descriptorTable) const;

    // Ports of nvrhi createAccelStruct / createRayTracingPipeline /
    // RayTracingPipeline::createShaderTable (d3d12-raytracing.cpp).
    AccelStruct* createAccelStruct(const AccelStructDesc& desc);
    RayTracingPipeline* createRayTracingPipeline(const RhiRtPipelineDesc& desc,
        const RhiRtPipelineShaderDesc* shaders, uint32_t shaderCount,
        const RhiRtPipelineHitGroupDesc* hitGroups, uint32_t hitGroupCount,
        BindingLayout* const* globalLayouts, uint32_t globalLayoutCount, const char* debugName);
    ShaderTable* createShaderTable(RayTracingPipeline* pipeline,
        const RhiRtShaderTableDesc& desc, const char* debugName);

    // Port of nvrhi Device::getAccelStructPreBuildInfo (d3d12-raytracing.cpp).
    bool getAccelStructPreBuildInfo(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& outPreBuildInfo,
        const AccelStructDesc& desc) const;

    // Port of nvrhi buildRootSignature/getRootSignature (with cache).
    std::shared_ptr<RootSignature> getRootSignature(
        BindingLayout* const* layouts, uint32_t layoutCount, bool allowInputLayout = false);

    void* mapBuffer(Buffer* buffer);
    void unmapBuffer(Buffer* buffer);

    // Adds one native reference for every resource in a submitted command
    // stream. The operation is all-or-nothing so a disposed handle cannot
    // leave a partially retained submission behind.
    bool RetainResources(RhiResource* const* resources, uint32_t count);
    bool RetainSubmissionResources(RhiResource* const* resources, uint32_t count);
    void ReleaseSubmissionResources(RhiResource* const* resources, uint32_t count);

    // Safe opaque-handle diagnostics. Consult the live-resource registry before
    // dereferencing a possibly stale command-stream pointer.
    bool IsLiveResource(const RhiResource* resource);
    void LogResourceDiagnostic(const char* role, uint32_t index,
        const RhiResource* resource);

    // Releases one native reference. When the last reference is gone, destroys
    // immediately if the GPU never used the resource; otherwise defers until
    // the resource's last-use recording instance retires.
    void ReleaseResource(RhiResource* resource);

    // Destroys deferred resources whose recording instance has retired.
    // Called from render events and explicitly from C#.
    void GarbageCollect();

    // Logs every live resource and pending release (diagnostics; destroys
    // nothing). Exposed to C# for domain-reload/leak investigations.
    void ReportLiveResources();

    // Returns S_OK while the device is healthy. On removal, logs the reason
    // and the breadcrumb trail once, then keeps returning the failure HRESULT.
    HRESULT CheckDeviceRemoved();

    // Call from a failed D3D12 operation that may be the first observable
    // symptom of asynchronous device removal. Ordinary failures are ignored;
    // a removed device is captured immediately before Unity's Present path
    // terminates the process.
    void HandleDeviceError(HRESULT operationResult, const char* operation);

    void GetStats(RhiDeviceStats& outStats);
    uint32_t GetResourceSnapshot(RhiResourceInfo* outResources, uint32_t capacity);
    bool QueryFeatureSupport(RhiFeature feature);
    FormatSupport QueryFormatSupport(Format format);
    bool HasUnityResourceStateTracking() const { return m_unityD3D12 != nullptr; }
    // `unityCommandList` is the list the state is being requested for. Passing
    // it explicitly keeps the 2021.3 bridge off CommandRecordingState in the
    // per-resource hot path; on Unity 6 it is unused.
    void RequestResourceState(ID3D12GraphicsCommandList* unityCommandList, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES state);
    void NotifyResourceState(ID3D12GraphicsCommandList* unityCommandList, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES state, bool uavAccess);

    // ---- GPU lifetime tracking (NVRHI's queue-instance model, adapted) ----
    // Unity's frame fence is NOT a retire marker for injected work: measured,
    // GetNextFrameFenceValue() runs only ~1.3 frames ahead of completed while
    // Unity keeps 2-3 frames in flight, so tagging lifetimes with it frees
    // memory the GPU still reads (docs/rt-sbt-upload-corruption-report.md).
    // Neither is a CPU-enqueued queue->Signal from a flush-configured plugin
    // event: Unity can run that callback while replay-only command lists are
    // still stranded unsubmitted, putting the signal on the queue ahead of
    // the work it is meant to cover (observed as a GPU page fault from
    // early deferred destruction).
    //
    // Instead every replayed command stream gets a monotonically increasing
    // recording instance - the value flowing into BufferChunk::version and
    // RhiResource::lastUseFenceValue - and the retire marker travels WITH the
    // GPU work: RecordLifetimeMarker records a 32-bit WriteBufferImmediate
    // MARKER_OUT into a plugin-owned readback buffer at the end of the replay,
    // in the same command list. MARKER_OUT cannot become visible until every
    // preceding GPU command has completed. Stranded lists merely delay the
    // marker; an instance whose marker has not landed is never reclaimable.
    uint32_t BeginRecordingInstance();          // lifetime tag for one replay
    uint32_t CurrentRecordingInstance() const
    {
        return m_RecordingInstance.load(std::memory_order_relaxed);
    }
    void RecordLifetimeMarker(ID3D12GraphicsCommandList* commandList, uint32_t instance);
    uint32_t CompletedLifetimeInstance() const; // retire authority for all lifetime tags

    // GPU sync points (kUnityRhiEvent_FlushAndSignalSyncPoint). Unlike the
    // rest of the backend these use a fence of our own: Unity's frame fence
    // only signals at end of frame, which never happens while a test blocks
    // the main thread. CreateSyncPoint allocates the value on the main thread
    // at record time; SignalSyncPoint runs on Unity's submission thread after
    // pending command lists were flushed; WaitSyncPoint blocks the CPU.
    uint64_t CreateSyncPoint();
    void SignalSyncPoint(uint64_t value);
    bool WaitSyncPoint(uint64_t value, uint32_t timeoutMs);

    const Context& context() const { return m_Context; }
    DeviceResources& resources() { return m_Resources; }
    UploadManager& uploadManager() { return m_UploadManager; }
    // Port of nvrhi CommandList::m_DxrScratchManager (AS build scratch memory).
    UploadManager& scratchManager() { return m_DxrScratchManager; }
    ID3D12Device* GetD3DDevice() const { return m_Context.device.Get(); }
    ID3D12Device5* GetD3DDevice5() const { return m_Context.device5.Get(); }
    ID3D12Device10* GetD3DDevice10() const { return m_Context.device10.Get(); }
    ID3D12CommandSignature* GetDispatchIndirectSignature() const
    {
        return m_Context.dispatchIndirectSignature.Get();
    }
    ID3D12CommandSignature* GetDrawIndirectSignature(bool indexed) const
    {
        return indexed ? m_Context.drawIndexedIndirectSignature.Get() : m_Context.drawIndirectSignature.Get();
    }
    bool IsHeapDirectlyIndexedEnabled() const { return m_HeapDirectlyIndexedEnabled; }
    bool GetEnhancedBarriersSupported() const { return m_EnhancedBarriersSupported; }
#if UNITYRHI_WITH_NVAPI
    bool IsRayTracingValidationEnabled() const { return m_RayTracingValidationEnabled; }
    // Total ray tracing validation messages received over the process
    // lifetime (diagnostics / native tests).
    static uint32_t RayTracingValidationMessageCount();
#endif

private:
    Device(ID3D12Device* device, UnityD3D12Interface* unityD3D12);
    ~Device();

    std::shared_ptr<RootSignature> buildRootSignature(
        BindingLayout* const* layouts, uint32_t layoutCount, bool isLocal = false,
        bool allowInputLayout = false);
    ComPtr<ID3D12PipelineState> createPipelineState(Shader* shader, RootSignature* rootSignature);

    void RegisterResource(RhiResource* resource);
    void DestroyResource(RhiResource* resource);

    static Device* s_instance;

    Context m_Context;
    DeviceResources m_Resources;
    UploadManager m_UploadManager;
    UploadManager m_DxrScratchManager;
    UnityD3D12Interface* m_unityD3D12 = nullptr;

    ComPtr<ID3D12Fence> m_syncPointFence;
    HANDLE m_syncPointEvent = nullptr;
    std::mutex m_syncPointMutex;
    std::atomic<uint64_t> m_nextSyncPointValue{0};
    std::atomic<bool> m_deviceRemovedLogged{false};

    // Lifetime tracking (see BeginRecordingInstance): WriteBufferImmediate
    // writes the 32-bit instance directly into a persistently mapped readback
    // slot with MARKER_OUT ordering; the CPU polls the ring for the newest
    // completed instance.
    static constexpr uint32_t kLifetimeMarkerSlotCount = 256;
    ComPtr<ID3D12Resource> m_lifetimeMarkerReadback; // readback heap, persistently mapped
    const uint32_t* m_lifetimeMarkerReadbackData = nullptr;
    uint32_t m_lifetimeMarkerNextSlot = 0;
    uint32_t m_lifetimeMarkerLastWritten[kLifetimeMarkerSlotCount] = {};
    std::atomic<uint32_t> m_RecordingInstance{0};
    mutable std::mutex m_lifetimeMutex;
    mutable uint32_t m_completedInstanceWatermark = 0;

    std::mutex m_Mutex;
    std::unordered_set<RhiResource*> m_liveResources;
    std::vector<std::pair<uint32_t, RhiResource*>> m_pendingReleases; // (instance, resource)
    uint32_t m_liveCounts[uint32_t(RhiResource::Kind::Count)] = {}; // indexed by Kind
    uint64_t m_nextBindingLayoutId = 1;
    bool m_HeapDirectlyIndexedEnabled = false;
    bool m_EnhancedBarriersSupported = false;
#if UNITYRHI_WITH_NVAPI
    // Driver-level ray tracing validation (NVAPI), opt-in at launch via
    // NV_ALLOW_RAYTRACING_VALIDATION=1. Mirrors nvrhi::d3d12::Device.
    bool m_NvapiInitialized = false;
    bool m_RayTracingValidationEnabled = false;
    void* m_RtValidationCallbackHandle = nullptr;
#endif
};

// Mirrors nvrhi::d3d12::calcSubresource.
inline uint32_t calcSubresource(uint32_t MipSlice, uint32_t ArraySlice, uint32_t PlaneSlice, uint32_t MipLevels, uint32_t ArraySize)
{
    return MipSlice + (ArraySlice * MipLevels) + (PlaneSlice * MipLevels * ArraySize);
}

inline D3D12_RESOURCE_DESC1 convertResourceDesc1(const D3D12_RESOURCE_DESC& desc)
{
    D3D12_RESOURCE_DESC1 result{};
    result.Dimension = desc.Dimension;
    result.Alignment = desc.Alignment;
    result.Width = desc.Width;
    result.Height = desc.Height;
    result.DepthOrArraySize = desc.DepthOrArraySize;
    result.MipLevels = desc.MipLevels;
    result.Format = desc.Format;
    result.SampleDesc = desc.SampleDesc;
    result.Layout = desc.Layout;
    result.Flags = desc.Flags;
    return result;
}

// d3d12-constants.cpp - nvrhi enum -> D3D12 enum converters.
D3D_PRIMITIVE_TOPOLOGY convertPrimitiveType(PrimitiveType primType);
D3D12_SHADER_VISIBILITY convertShaderStage(ShaderType s);
D3D12_TEXTURE_ADDRESS_MODE convertSamplerAddressMode(SamplerAddressMode mode);
UINT convertSamplerReductionType(SamplerReductionType reductionType);
D3D12_RESOURCE_STATES convertResourceStates(ResourceStates stateBits);

struct EnhancedResourceStateMapping
{
    ResourceStates nvrhiState = ResourceStates::Unknown;
    D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
    D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_NO_ACCESS;
    D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_COMMON;
};
EnhancedResourceStateMapping convertResourceStatesForEnhancedBarriers(ResourceStates state, bool isTexture);

// d3d12-raytracing.cpp - geometry-desc translation shared with the
// command-stream replayer (port of nvrhi's fillD3dGeometryDesc).
void fillD3dGeometryDesc(D3D12_RAYTRACING_GEOMETRY_DESC& outD3dGeometryDesc,
    const RhiRtGeometryDesc& geometryDesc, D3D12_GPU_VIRTUAL_ADDRESS transform4x4);
} // namespace unityrhi
