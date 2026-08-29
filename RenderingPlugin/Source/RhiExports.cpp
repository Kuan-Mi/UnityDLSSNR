// C ABI resource exports consumed by com.unityrhi (NativeMethods.cs).
// Handles are opaque RhiResource pointers owned by the managed wrappers.

#include "CommandStream.h"
#include "d3d12/d3d12-backend.h"
#include "d3d12/GpuDumps.h"
#include "IUnityInterface.h"
#include "RhiTypes.h"
#include "CommandSubmission.h"
#include "ShaderCompiler.h"

using namespace unityrhi;

extern "C"
{
    UNITY_INTERFACE_EXPORT uint64_t UNITY_INTERFACE_API UnityRhiGetDeviceNativeObject(RhiObjectType objectType)
    {
        Device* device = Device::Get();
        return device ? device->getNativeObject(objectType) : 0;
    }

    UNITY_INTERFACE_EXPORT uint64_t UNITY_INTERFACE_API
    UnityRhiGetNativeObject(void* handle, RhiObjectType objectType)
    {
        auto* resource = static_cast<RhiResource*>(handle);
        if (!resource)
            return 0;
        switch (resource->kind)
        {
        case RhiResource::Kind::Buffer:
        {
            auto* buffer = static_cast<Buffer*>(resource);
            if (objectType == RhiObjectType::D3D12_Resource)
                return reinterpret_cast<uint64_t>(buffer->resource.Get());
            if (objectType == RhiObjectType::SharedHandle)
                return reinterpret_cast<uint64_t>(buffer->sharedHandle);
            break;
        }
        case RhiResource::Kind::Texture:
        {
            auto* texture = static_cast<Texture*>(resource);
            if (objectType == RhiObjectType::D3D12_Resource)
                return reinterpret_cast<uint64_t>(texture->resource.Get());
            if (objectType == RhiObjectType::SharedHandle)
                return reinterpret_cast<uint64_t>(texture->sharedHandle);
            break;
        }
        case RhiResource::Kind::StagingTexture:
        {
            auto* staging = static_cast<StagingTexture*>(resource);
            if (objectType == RhiObjectType::D3D12_Resource && staging->buffer)
                return reinterpret_cast<uint64_t>(staging->buffer->resource.Get());
            break;
        }
        case RhiResource::Kind::AccelStruct:
        {
            auto* accelStruct = static_cast<AccelStruct*>(resource);
            if (objectType == RhiObjectType::D3D12_Resource && accelStruct->dataBuffer)
                return reinterpret_cast<uint64_t>(accelStruct->dataBuffer->resource.Get());
            break;
        }
        case RhiResource::Kind::ComputePipeline:
        {
            auto* pipeline = static_cast<ComputePipeline*>(resource);
            if (objectType == RhiObjectType::D3D12_RootSignature && pipeline->rootSignature)
                return reinterpret_cast<uint64_t>(pipeline->rootSignature->handle.Get());
            if (objectType == RhiObjectType::D3D12_PipelineState)
                return reinterpret_cast<uint64_t>(pipeline->pipelineState.Get());
            break;
        }
        case RhiResource::Kind::GraphicsPipeline:
        {
            auto* pipeline = static_cast<GraphicsPipeline*>(resource);
            if (objectType == RhiObjectType::D3D12_RootSignature && pipeline->rootSignature)
                return reinterpret_cast<uint64_t>(pipeline->rootSignature->handle.Get());
            if (objectType == RhiObjectType::D3D12_PipelineState)
                return reinterpret_cast<uint64_t>(pipeline->pipelineState.Get());
            break;
        }
        default:
            break;
        }
        return 0;
    }

    UNITY_INTERFACE_EXPORT uint64_t UNITY_INTERFACE_API UnityRhiGetTextureNativeView(
        void* handle, RhiObjectType objectType, Format format,
        const TextureSubresourceSet* subresources, TextureDimension dimension, uint32_t isReadOnlyDSV)
    {
        auto* texture = static_cast<Texture*>(handle);
        return texture && subresources
            ? texture->getNativeView(objectType, format, *subresources, dimension, isReadOnlyDSV != 0)
            : 0;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetTextureTiling(
        void* handle, uint32_t* numTiles, RhiPackedMipDesc* packedMipDesc,
        RhiTileShape* tileShape, uint32_t* subresourceTilingCount,
        RhiSubresourceTiling* subresourceTilings)
    {
        Device* device = Device::Get();
        return device && device->getTextureTiling(static_cast<Texture*>(handle), numTiles,
            packedMipDesc, tileShape, subresourceTilingCount, subresourceTilings);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiUpdateTextureTileMappings(
        void* texture, void* heap, const RhiTiledTextureCoordinate* coordinates,
        const RhiTiledTextureRegion* regions, const uint64_t* byteOffsets, uint32_t regionCount)
    {
        Device* device = Device::Get();
        return device && device->updateTextureTileMappings(static_cast<Texture*>(texture),
            static_cast<Heap*>(heap), coordinates, regions, byteOffsets, regionCount);
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API
    UnityRhiCreateBuffer(const RhiBufferDesc* desc, const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc ? device->createBuffer(BufferDesc(*desc, debugName)) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API
    UnityRhiCreateTexture(const RhiTextureDesc* desc, const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc ? device->createTexture(TextureDesc(*desc, debugName)) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API
    UnityRhiCreateTextureFromNativeResource(void* nativeResource, const RhiTextureDesc* desc, const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc
            ? device->createTextureFromNativeResource(
                  static_cast<ID3D12Resource*>(nativeResource), TextureDesc(*desc, debugName))
            : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiGetTextureNativeResource(void* handle)
    {
        auto* texture = static_cast<Texture*>(handle);
        return texture && texture->resource ? texture->resource.Get() : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateStagingTexture(
        const RhiTextureDesc* desc, CpuAccessMode cpuAccess, const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc && cpuAccess != CpuAccessMode::None
            ? device->createStagingTexture(TextureDesc(*desc, debugName), cpuAccess)
            : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiMapStagingTexture(
        void* handle, const RhiTextureSlice* slice, CpuAccessMode cpuAccess, uint64_t* outRowPitch)
    {
        Device* device = Device::Get();
        return device && slice
            ? device->mapStagingTexture(static_cast<StagingTexture*>(handle), *slice, cpuAccess, outRowPitch)
            : nullptr;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiUnmapStagingTexture(void* handle)
    {
        if (Device* device = Device::Get())
            device->unmapStagingTexture(static_cast<StagingTexture*>(handle));
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API
    UnityRhiCreateSampler(const RhiSamplerDesc* desc, const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc ? device->createSampler(*desc, debugName) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiReleaseResource(void* handle)
    {
        if (Device* device = Device::Get())
            device->ReleaseResource(static_cast<RhiResource*>(handle));
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiRetainResources(
        void* const* handles, uint32_t count)
    {
        Device* device = Device::Get();
        return device && device->RetainResources(
            reinterpret_cast<RhiResource* const*>(handles), count) ? 1 : 0;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateCommandSubmission(
        const void* stream, uint32_t byteSize, void* const* resources, uint32_t resourceCount,
        void* const* uploadTickets, uint32_t uploadTicketCount)
    {
        return CreateCommandSubmission(stream, byteSize,
            reinterpret_cast<RhiResource* const*>(resources), resourceCount,
            reinterpret_cast<UploadTicket* const*>(uploadTickets), uploadTicketCount);
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiDestroyCommandSubmission(void* submission)
    {
        DestroyCommandSubmission(static_cast<CommandSubmission*>(submission));
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiGarbageCollect()
    {
        if (Device* device = Device::Get())
            device->GarbageCollect();
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiGetDeviceStats(RhiDeviceStats* outStats)
    {
        if (!outStats)
            return;
        *outStats = RhiDeviceStats{};
        if (Device* device = Device::Get())
            device->GetStats(*outStats);
    }

    UNITY_INTERFACE_EXPORT uint32_t UNITY_INTERFACE_API UnityRhiGetResourceSnapshot(
        RhiResourceInfo* outResources, uint32_t capacity)
    {
        Device* device = Device::Get();
        return device ? device->GetResourceSnapshot(outResources, capacity) : 0;
    }

    // ---- Phase 2: shader compilation (device-independent) ----

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCompileShader(
        const char* source, const char* sourceName, const char* entryPoint,
        const char* targetProfile, const char* defines, const char* includeDirs,
        uint32_t flags)
    {
        ShaderCompileArgs args;
        args.source = source;
        args.sourceName = sourceName;
        args.entryPoint = entryPoint;
        args.targetProfile = targetProfile;
        args.defines = defines;
        args.includeDirs = includeDirs;
        args.flags = flags;
        return CompileShader(args);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiShaderCompileGetSucceeded(void* result)
    {
        return result && static_cast<ShaderCompileResult*>(result)->succeeded ? 1 : 0;
    }

    UNITY_INTERFACE_EXPORT const void* UNITY_INTERFACE_API
    UnityRhiShaderCompileGetBytecode(void* result, uint64_t* outSize)
    {
        auto* compileResult = static_cast<ShaderCompileResult*>(result);
        if (!compileResult || compileResult->bytecode.empty())
        {
            if (outSize)
                *outSize = 0;
            return nullptr;
        }
        if (outSize)
            *outSize = compileResult->bytecode.size();
        return compileResult->bytecode.data();
    }

    UNITY_INTERFACE_EXPORT const char* UNITY_INTERFACE_API UnityRhiShaderCompileGetErrors(void* result)
    {
        auto* compileResult = static_cast<ShaderCompileResult*>(result);
        return compileResult ? compileResult->errors.c_str() : "";
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiShaderCompileDestroy(void* result)
    {
        delete static_cast<ShaderCompileResult*>(result);
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateShader(
        uint32_t shaderType, const char* entryName,
        const void* bytecode, uint64_t byteSize, const char* debugName)
    {
        Device* device = Device::Get();
        if (!device || !bytecode || byteSize == 0)
            return nullptr;
        ShaderDesc desc;
        desc.shaderType = ShaderType(shaderType);
        desc.debugName = debugName ? debugName : "";
        desc.entryName = entryName ? entryName : "";
        return device->createShader(desc, bytecode, size_t(byteSize));
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateShaderLibrary(
        const void* bytecode, uint64_t byteSize, const char* debugName)
    {
        Device* device = Device::Get();
        return device ? device->createShaderLibrary(bytecode, byteSize, debugName) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateLibraryShader(
        void* library, uint32_t shaderType, const char* entryName, const char* debugName)
    {
        Device* device = Device::Get();
        (void)debugName; // the entry name doubles as the debug name, as in nvrhi
        return device ? device->createLibraryShader(
                            static_cast<ShaderLibrary*>(library), entryName, ShaderType(shaderType))
                      : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateBindingLayout(
        const RhiBindingLayoutDesc* desc, const RhiBindingLayoutItem* items, uint32_t itemCount,
        const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc ? device->createBindingLayout(*desc, items, itemCount, debugName) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateBindlessLayout(
        const RhiBindlessLayoutDesc* desc, const RhiBindingLayoutItem* registerSpaces, uint32_t registerSpaceCount,
        const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc ? device->createBindlessLayout(*desc, registerSpaces, registerSpaceCount, debugName) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateBindingSet(
        void* layout, const RhiBindingSetItem* items, uint32_t itemCount, const char* debugName)
    {
        Device* device = Device::Get();
        return device ? device->createBindingSet(static_cast<BindingLayout*>(layout), items, itemCount, debugName) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateComputePipeline(
        void* shader, void** layouts, uint32_t layoutCount, const char* debugName)
    {
        Device* device = Device::Get();
        return device ? device->createComputePipeline(static_cast<Shader*>(shader),
                            reinterpret_cast<BindingLayout* const*>(layouts), layoutCount, debugName)
                      : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateInputLayout(
        const RhiVertexAttributeDesc* attributes, uint32_t attributeCount, const char* debugName)
    {
        Device* device = Device::Get();
        return device ? device->createInputLayout(attributes, attributeCount, debugName) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateFramebuffer(
        void** colorTextures, uint32_t colorCount, void* depthTexture, const char* debugName)
    {
        Device* device = Device::Get();
        return device ? device->createFramebuffer(
                            reinterpret_cast<Texture* const*>(colorTextures), colorCount,
                            static_cast<Texture*>(depthTexture), debugName)
                      : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateGraphicsPipeline(
        const RhiGraphicsPipelineDesc* desc,
        void* vertexShader, void* pixelShader, void* inputLayout,
        void** layouts, uint32_t layoutCount,
        void* framebuffer, const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc
            ? device->createGraphicsPipeline(*desc,
                  static_cast<Shader*>(vertexShader), static_cast<Shader*>(pixelShader),
                  static_cast<InputLayout*>(inputLayout),
                  reinterpret_cast<BindingLayout* const*>(layouts), layoutCount,
                  static_cast<Framebuffer*>(framebuffer), debugName)
            : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateDescriptorTable(
        void* layout, const char* debugName)
    {
        Device* device = Device::Get();
        return device ? device->createDescriptorTable(static_cast<BindingLayout*>(layout), debugName) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API
    UnityRhiCreateBufferFromNativeResource(void* nativeResource, const RhiBufferDesc* desc, const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc
            ? device->createBufferFromNativeResource(
                  static_cast<ID3D12Resource*>(nativeResource), BufferDesc(*desc, debugName))
            : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiGetBufferNativeResource(void* handle)
    {
        auto* buffer = static_cast<Buffer*>(handle);
        return buffer && buffer->resource ? buffer->resource.Get() : nullptr;
    }

    UNITY_INTERFACE_EXPORT uint64_t UNITY_INTERFACE_API UnityRhiGetBufferGpuVirtualAddress(void* handle)
    {
        auto* buffer = static_cast<Buffer*>(handle);
        return buffer ? buffer->gpuVA : 0;
    }

    UNITY_INTERFACE_EXPORT uint64_t UNITY_INTERFACE_API UnityRhiGetAccelStructGpuVirtualAddress(void* handle)
    {
        auto* accelStruct = static_cast<AccelStruct*>(handle);
        return accelStruct ? accelStruct->getDeviceAddress() : 0;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API
    UnityRhiCreateHeap(const RhiHeapDesc* desc, const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc && desc->capacity != 0
            ? device->createHeap(HeapDesc(*desc, debugName))
            : nullptr;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
    UnityRhiGetBufferMemoryRequirements(void* handle, MemoryRequirements* outRequirements)
    {
        if (!outRequirements)
            return;
        Device* device = Device::Get();
        *outRequirements = device && handle
            ? device->getBufferMemoryRequirements(static_cast<Buffer*>(handle))
            : MemoryRequirements{};
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
    UnityRhiGetTextureMemoryRequirements(void* handle, MemoryRequirements* outRequirements)
    {
        if (!outRequirements)
            return;
        Device* device = Device::Get();
        *outRequirements = device
            ? device->getTextureMemoryRequirements(static_cast<Texture*>(handle))
            : MemoryRequirements{};
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
    UnityRhiGetAccelStructMemoryRequirements(void* handle, MemoryRequirements* outRequirements)
    {
        if (!outRequirements)
            return;
        Device* device = Device::Get();
        *outRequirements = device
            ? device->getAccelStructMemoryRequirements(static_cast<AccelStruct*>(handle))
            : MemoryRequirements{};
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiBindBufferMemory(void* buffer, void* heap, uint64_t offset)
    {
        Device* device = Device::Get();
        auto* h = static_cast<Heap*>(heap);
        return device && buffer && h && h->heap &&
            device->bindBufferMemory(static_cast<Buffer*>(buffer), h, offset);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiBindTextureMemory(void* texture, void* heap, uint64_t offset)
    {
        Device* device = Device::Get();
        return device && device->bindTextureMemory(static_cast<Texture*>(texture), static_cast<Heap*>(heap), offset);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiBindAccelStructMemory(void* accelStruct, void* heap, uint64_t offset)
    {
        Device* device = Device::Get();
        return device && device->bindAccelStructMemory(
            static_cast<AccelStruct*>(accelStruct), static_cast<Heap*>(heap), offset);
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiResizeDescriptorTable(
        void* descriptorTable, uint32_t newSize, uint32_t keepContents)
    {
        if (Device* device = Device::Get())
            device->resizeDescriptorTable(static_cast<DescriptorTable*>(descriptorTable), newSize, keepContents != 0);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiWriteDescriptorTable(
        void* descriptorTable, const RhiBindingSetItem* binding)
    {
        Device* device = Device::Get();
        return device && binding
            ? (device->writeDescriptorTable(static_cast<DescriptorTable*>(descriptorTable), *binding) ? 1 : 0)
            : 0;
    }

    UNITY_INTERFACE_EXPORT uint32_t UNITY_INTERFACE_API UnityRhiGetDescriptorTableFirstDescriptorIndexInHeap(
        void* descriptorTable)
    {
        Device* device = Device::Get();
        return device ? device->getDescriptorTableFirstDescriptorIndexInHeap(
                            static_cast<DescriptorTable*>(descriptorTable))
                      : c_InvalidDescriptorIndex;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateAccelStruct(
        const RhiRtAccelStructDesc* desc, const RhiRtGeometryDesc* geometries, uint32_t geometryCount,
        const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc
            ? device->createAccelStruct(AccelStructDesc(*desc, geometries, geometryCount, debugName))
            : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateRayTracingPipeline(
        const RhiRtPipelineDesc* desc,
        const RhiRtPipelineShaderDesc* shaders, uint32_t shaderCount,
        const RhiRtPipelineHitGroupDesc* hitGroups, uint32_t hitGroupCount,
        void** globalLayouts, uint32_t globalLayoutCount,
        const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc
            ? device->createRayTracingPipeline(*desc, shaders, shaderCount, hitGroups, hitGroupCount,
                  reinterpret_cast<BindingLayout* const*>(globalLayouts), globalLayoutCount, debugName)
            : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateShaderTable(
        void* pipeline, const RhiRtShaderTableDesc* desc, const char* debugName)
    {
        Device* device = Device::Get();
        return device && desc ? device->createShaderTable(static_cast<RayTracingPipeline*>(pipeline), *desc, debugName)
                              : nullptr;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiShaderTableSetRayGenerationShader(
        void* shaderTable, const char* exportName, void* bindings)
    {
        if (shaderTable)
            static_cast<ShaderTable*>(shaderTable)->setRayGenerationShader(exportName, static_cast<BindingSet*>(bindings));
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiShaderTableAddMissShader(
        void* shaderTable, const char* exportName, void* bindings)
    {
        return shaderTable
            ? static_cast<ShaderTable*>(shaderTable)->addMissShader(exportName, static_cast<BindingSet*>(bindings))
            : -1;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiShaderTableAddHitGroup(
        void* shaderTable, const char* exportName, void* bindings)
    {
        return shaderTable
            ? static_cast<ShaderTable*>(shaderTable)->addHitGroup(exportName, static_cast<BindingSet*>(bindings))
            : -1;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiShaderTableAddCallableShader(
        void* shaderTable, const char* exportName, void* bindings)
    {
        return shaderTable
            ? static_cast<ShaderTable*>(shaderTable)->addCallableShader(exportName, static_cast<BindingSet*>(bindings))
            : -1;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiShaderTableClearMissShaders(void* shaderTable)
    {
        if (shaderTable)
            static_cast<ShaderTable*>(shaderTable)->clearMissShaders();
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiShaderTableClearHitShaders(void* shaderTable)
    {
        if (shaderTable)
            static_cast<ShaderTable*>(shaderTable)->clearHitShaders();
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiShaderTableClearCallableShaders(void* shaderTable)
    {
        if (shaderTable)
            static_cast<ShaderTable*>(shaderTable)->clearCallableShaders();
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiMapBuffer(void* buffer)
    {
        Device* device = Device::Get();
        auto* b = static_cast<Buffer*>(buffer);
        return device && b && b->resource ? device->mapBuffer(b) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiUnmapBuffer(void* buffer)
    {
        Device* device = Device::Get();
        auto* b = static_cast<Buffer*>(buffer);
        if (device && b && b->resource)
            device->unmapBuffer(b);
    }

    // Record-time upload reservations. The returned ticket is embedded in the
    // compact command stream and released with its managed stream allocation.
    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API
    UnityRhiStageBufferUpload(const void* data, uint64_t byteSize)
    {
        Device* device = Device::Get();
        return device ? device->uploadManager().stageBuffer(
            data, byteSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiStageTextureUpload(
        void* textureHandle, uint32_t arraySlice, uint32_t mipLevel,
        const void* data, uint64_t dataSize, uint64_t rowPitch, uint64_t depthPitch)
    {
        Device* device = Device::Get();
        return device ? device->uploadManager().stageTexture(
            static_cast<Texture*>(textureHandle), arraySlice, mipLevel,
            data, dataSize, rowPitch, depthPitch) : nullptr;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiReleaseUploadTicket(void* ticket)
    {
        if (Device* device = Device::Get())
            device->uploadManager().releaseTicket(static_cast<UploadTicket*>(ticket));
    }

    UNITY_INTERFACE_EXPORT uint64_t UNITY_INTERFACE_API UnityRhiGetUploadTicketSize(void* ticket)
    {
        const auto* upload = static_cast<const UploadTicket*>(ticket);
        return upload ? upload->size : 0;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiQueryFeatureSupport(uint32_t feature)
    {
        Device* device = Device::Get();
        return device && device->QueryFeatureSupport(static_cast<RhiFeature>(feature)) ? 1 : 0;
    }

    // ---- Phase 6: wire-format test hook ----

    // Dry-run decode of a recorded command stream; requires no device. Returns
    // 1 and fills `outInfo` when the stream is structurally valid.
    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
    UnityRhiDecodeCommandStream(const void* data, CommandStreamDecodeInfo* outInfo)
    {
        if (!outInfo)
            return 0;
        return DecodeCommandStream(data, *outInfo) ? 1 : 0;
    }

    UNITY_INTERFACE_EXPORT uint32_t UNITY_INTERFACE_API UnityRhiQueryFormatSupport(uint32_t format)
    {
        Device* device = Device::Get();
        return device ? uint32_t(device->QueryFormatSupport(static_cast<Format>(format))) : 0;
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateEventQuery()
    {
        Device* device = Device::Get();
        return device ? device->createEventQuery() : nullptr;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiSetEventQuery(void* query)
    {
        if (Device* device = Device::Get()) device->setEventQuery(static_cast<EventQuery*>(query));
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiPollEventQuery(void* query)
    {
        Device* device = Device::Get();
        return device && device->pollEventQuery(static_cast<EventQuery*>(query)) ? 1 : 0;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiWaitEventQuery(void* query)
    {
        if (Device* device = Device::Get()) device->waitEventQuery(static_cast<EventQuery*>(query));
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiResetEventQuery(void* query)
    {
        if (Device* device = Device::Get()) device->resetEventQuery(static_cast<EventQuery*>(query));
    }

    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API UnityRhiCreateTimerQuery()
    {
        Device* device = Device::Get();
        return device ? device->createTimerQuery() : nullptr;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiPollTimerQuery(void* query)
    {
        Device* device = Device::Get();
        return device && device->pollTimerQuery(static_cast<TimerQuery*>(query)) ? 1 : 0;
    }

    UNITY_INTERFACE_EXPORT float UNITY_INTERFACE_API UnityRhiGetTimerQueryTime(void* query)
    {
        Device* device = Device::Get();
        return device ? device->getTimerQueryTime(static_cast<TimerQuery*>(query)) : 0.f;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiResetTimerQuery(void* query)
    {
        if (Device* device = Device::Get()) device->resetTimerQuery(static_cast<TimerQuery*>(query));
    }

    // ---- Phase 5: robustness diagnostics ----

    // 0 (S_OK) while the device is healthy; otherwise the DXGI removal HRESULT.
    // The first failing call also logs the reason and the breadcrumb trail.
    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetDeviceRemovedReason()
    {
        Device* device = Device::Get();
        return device ? static_cast<int>(device->CheckDeviceRemoved()) : 0;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiReportLiveResources()
    {
        if (Device* device = Device::Get())
            device->ReportLiveResources();
    }

    // ---- GPU dump-file diagnostics (Agility SDK preview) ----

    // Non-zero when the preview dump-file interface was acquired. Fills the
    // out-params with D3D12_FEATURE_DUMP_FILE results (any may be null).
    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
    UnityRhiGpuDumpQuerySupport(int* supportedByOS, int* driverTier, unsigned* driverOptionsMask)
    {
        GpuDumps& dumps = GpuDumps::Get();
        if (supportedByOS) *supportedByOS = dumps.SupportedByOS() ? 1 : 0;
        if (driverTier) *driverTier = static_cast<int>(dumps.DriverTier());
        if (driverOptionsMask) *driverOptionsMask = dumps.DriverOptionsMask();
        return dumps.Available() ? 1 : 0;
    }

    // Re-run ConfigureDumpFile with a D3D12_DUMP_FILE_DRIVER_OPTION_* mask.
    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGpuDumpSetOptions(unsigned optionsMask)
    {
        return GpuDumps::Get().SetOptions(optionsMask) ? 1 : 0;
    }

    // D3D12_DEVICE_ERROR_CODE from the preview device (0 when healthy/unavailable).
    UNITY_INTERFACE_EXPORT unsigned UNITY_INTERFACE_API UnityRhiGpuDumpGetDeviceErrorCode()
    {
        return GpuDumps::Get().DeviceErrorCode();
    }

    // Copies the most recent dump path (UTF-16) into buffer; returns its length.
    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
    UnityRhiGpuDumpGetLastPath(wchar_t* buffer, int capacity)
    {
        return GpuDumps::Get().GetLastDumpPath(buffer, capacity);
    }

    // ---- GPU sync points (see kUnityRhiEvent_FlushAndSignalSyncPoint) ----

    UNITY_INTERFACE_EXPORT uint64_t UNITY_INTERFACE_API UnityRhiCreateSyncPoint()
    {
        Device* device = Device::Get();
        return device ? device->CreateSyncPoint() : 0;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
    UnityRhiWaitSyncPoint(uint64_t value, uint32_t timeoutMs)
    {
        Device* device = Device::Get();
        return device && device->WaitSyncPoint(value, timeoutMs) ? 1 : 0;
    }
}
