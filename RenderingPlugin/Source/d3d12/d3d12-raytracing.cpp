// Acceleration structures, ray-tracing pipelines, and shader tables ported
// from NVRHI's d3d12-raytracing.cpp (External/nvrhi/src/d3d12).
// Copyright (c) 2014-2021, NVIDIA CORPORATION. MIT license
// (see ThirdParty/NVRHI-LICENSE.txt).
//
// UnityRHI deviations:
//  - NVAPI paths (opacity micromaps, spheres, LSS, clusters) and RTXMU are
//    not ported; the plain D3D12 code paths remain, which is exactly what the
//    original compiles to with those options off. Without NVAPI the
//    D3D12RaytracingGeometryDesc wrapper collapses to
//    D3D12_RAYTRACING_GEOMETRY_DESC, so build inputs use
//    D3D12_ELEMENTS_LAYOUT_ARRAY instead of ARRAY_OF_POINTERS.
//  - CommandList::build*/setRayTracingState/dispatchRays are ReplayContext
//    methods here; CommandStream.cpp decodes their flattened payloads.

#include "d3d12-backend.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "common/dxgi-format.h"
#include "UnityRhiLog.h"

namespace unityrhi
{
static D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS convertAccelStructBuildFlags(
    RhiRtAccelStructBuildFlags flags)
{
    // AllowEmptyInstances is an NVRHI validation-only flag. Its numeric value
    // aliases a newer D3D12 OMM flag and must never be passed to D3D12.
    const uint32_t nativeFlags = uint32_t(flags) &
        ~uint32_t(RhiRtAccelStructBuildFlags::AllowEmptyInstances);
    return D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(nativeFlags);
}

static void fillD3dGeometryTrianglesDesc(D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& outDxrTriangles,
    const RhiRtGeometryDesc& geometryDesc, D3D12_GPU_VIRTUAL_ADDRESS transform4x4)
{
    const auto& triangles = geometryDesc.geometryData.triangles;

    auto* indexBuffer = reinterpret_cast<Buffer*>(uintptr_t(triangles.indexBuffer));
    auto* vertexBuffer = reinterpret_cast<Buffer*>(uintptr_t(triangles.vertexBuffer));

    if (indexBuffer)
        outDxrTriangles.IndexBuffer = indexBuffer->gpuVA + triangles.indexOffset;
    else
        outDxrTriangles.IndexBuffer = 0;

    if (vertexBuffer)
        outDxrTriangles.VertexBuffer.StartAddress = vertexBuffer->gpuVA + triangles.vertexOffset;
    else
        outDxrTriangles.VertexBuffer.StartAddress = 0;

    outDxrTriangles.VertexBuffer.StrideInBytes = triangles.vertexStride;
    outDxrTriangles.IndexFormat = getDxgiFormatMapping(triangles.indexFormat).srvFormat;
    outDxrTriangles.VertexFormat = getDxgiFormatMapping(triangles.vertexFormat).srvFormat;
    outDxrTriangles.IndexCount = triangles.indexCount;
    outDxrTriangles.VertexCount = triangles.vertexCount;
    outDxrTriangles.Transform3x4 = transform4x4;
}

static void fillD3dAABBDesc(D3D12_RAYTRACING_GEOMETRY_AABBS_DESC& outDxrAABB,
    const RhiRtGeometryDesc& geometryDesc)
{
    const auto& aabbs = geometryDesc.geometryData.aabbs;

    auto* buffer = reinterpret_cast<Buffer*>(uintptr_t(aabbs.buffer));

    if (buffer)
        outDxrAABB.AABBs.StartAddress = buffer->gpuVA + aabbs.offset;
    else
        outDxrAABB.AABBs.StartAddress = 0;

    outDxrAABB.AABBs.StrideInBytes = aabbs.stride;
    outDxrAABB.AABBCount = aabbs.count;
}

void fillD3dGeometryDesc(D3D12_RAYTRACING_GEOMETRY_DESC& outD3dGeometryDesc,
    const RhiRtGeometryDesc& geometryDesc, D3D12_GPU_VIRTUAL_ADDRESS transform4x4)
{
    outD3dGeometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAGS(geometryDesc.flags);

    if (geometryDesc.geometryType == RhiRtGeometryType::Triangles)
    {
        outD3dGeometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC dxrTriangles = {};
        fillD3dGeometryTrianglesDesc(dxrTriangles, geometryDesc, transform4x4);
        outD3dGeometryDesc.Triangles = dxrTriangles;
    }
    else
    {
        outD3dGeometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        D3D12_RAYTRACING_GEOMETRY_AABBS_DESC dxrAABBs = {};
        fillD3dAABBDesc(dxrAABBs, geometryDesc);
        outD3dGeometryDesc.AABBs = dxrAABBs;
    }
}

// Port of nvrhi's fillAsInputDescForPreBuildInfo.
static void fillAsInputDescForPreBuildInfo(
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& outASInputs,
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& outGeometryDescs,
    const AccelStructDesc& desc, const RhiRtGeometryDesc* geometries, uint32_t geometryCount)
{
    outASInputs = {};
    if (desc.isTopLevel)
    {
        outASInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        outASInputs.Flags = convertAccelStructBuildFlags(desc.buildFlags);
        outASInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        outASInputs.InstanceDescs = D3D12_GPU_VIRTUAL_ADDRESS{0};
        outASInputs.NumDescs = UINT(desc.topLevelMaxInstances);
    }
    else
    {
        outASInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        outASInputs.Flags = convertAccelStructBuildFlags(desc.buildFlags);
        outASInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        outGeometryDescs.resize(geometryCount);
        for (uint32_t i = 0; i < geometryCount; i++)
        {
            const RhiRtGeometryDesc& srcDesc = geometries[i];
            // useTransform sets a non-null dummy GPU VA. The reason is explained in the spec:
            // "It (read: GetRaytracingAccelerationStructurePrebuildInfo) may not inspect/dereference
            // any GPU virtual addresses, other than to assert to see if a pointer is NULL or not,
            // such as the optional Transform in D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC, without dereferencing it."
            // Omitting this here will trigger a gpu hang due to incorrect memory calculation.
            D3D12_GPU_VIRTUAL_ADDRESS transform4x4 = srcDesc.useTransform ? 16 : 0;
            fillD3dGeometryDesc(outGeometryDescs[i], srcDesc, transform4x4);
        }
        outASInputs.NumDescs = geometryCount;
        outASInputs.pGeometryDescs = outGeometryDescs.data();
    }
}

uint32_t ShaderTable::getNumEntries() const
{
    return 1 + // rayGeneration
        uint32_t(missShaders.size()) +
        uint32_t(hitGroups.size()) +
        uint32_t(callableShaders.size());
}

bool ShaderTable::verifyExport(const RayTracingPipeline::ExportTableEntry* pExport, BindingSet* bindings) const
{
    if (!pExport)
    {
        m_Context.error("Couldn't find a DXR PSO export with a given name");
        return false;
    }

    if (pExport->bindingLayout && !bindings)
    {
        m_Context.error("A shader table entry does not provide required local bindings");
        return false;
    }

    if (!pExport->bindingLayout && bindings)
    {
        m_Context.error("A shader table entry provides local bindings, but none are required");
        return false;
    }

    if (bindings && (bindings->layout != pExport->bindingLayout))
    {
        m_Context.error("A shader table entry provides local bindings that do not match the expected layout");
        return false;
    }

    return true;
}

void ShaderTable::setRayGenerationShader(const char* exportName, BindingSet* bindings /*= nullptr*/)
{
    const RayTracingPipeline::ExportTableEntry* pipelineExport = pipeline->getExport(exportName);

    if (verifyExport(pipelineExport, bindings))
    {
        rayGenerationShader.pShaderIdentifier = pipelineExport->pShaderIdentifier;
        rayGenerationShader.localBindings = bindings;

        ++version;
    }
}

int ShaderTable::addMissShader(const char* exportName, BindingSet* bindings /*= nullptr*/)
{
    const RayTracingPipeline::ExportTableEntry* pipelineExport = pipeline->getExport(exportName);

    if (verifyExport(pipelineExport, bindings))
    {
        Entry entry;
        entry.pShaderIdentifier = pipelineExport->pShaderIdentifier;
        entry.localBindings = bindings;
        missShaders.push_back(entry);

        ++version;

        return int(missShaders.size()) - 1;
    }

    return -1;
}

int ShaderTable::addHitGroup(const char* exportName, BindingSet* bindings /*= nullptr*/)
{
    const RayTracingPipeline::ExportTableEntry* pipelineExport = pipeline->getExport(exportName);

    if (verifyExport(pipelineExport, bindings))
    {
        Entry entry;
        entry.pShaderIdentifier = pipelineExport->pShaderIdentifier;
        entry.localBindings = bindings;
        hitGroups.push_back(entry);

        ++version;

        return int(hitGroups.size()) - 1;
    }

    return -1;
}

int ShaderTable::addCallableShader(const char* exportName, BindingSet* bindings /*= nullptr*/)
{
    const RayTracingPipeline::ExportTableEntry* pipelineExport = pipeline->getExport(exportName);

    if (verifyExport(pipelineExport, bindings))
    {
        Entry entry;
        entry.pShaderIdentifier = pipelineExport->pShaderIdentifier;
        entry.localBindings = bindings;
        callableShaders.push_back(entry);

        ++version;

        return int(callableShaders.size()) - 1;
    }

    return -1;
}

void ShaderTable::clearMissShaders()
{
    missShaders.clear();
    ++version;
}

void ShaderTable::clearHitShaders()
{
    hitGroups.clear();
    ++version;
}

void ShaderTable::clearCallableShaders()
{
    callableShaders.clear();
    ++version;
}

const RayTracingPipeline::ExportTableEntry* RayTracingPipeline::getExport(const char* name)
{
    const auto exportEntryIt = exports.find(name);
    if (exportEntryIt == exports.end())
    {
        return nullptr;
    }

    return &exportEntryIt->second;
}

// Port of nvrhi RayTracingPipeline::createShaderTable.
ShaderTable* Device::createShaderTable(RayTracingPipeline* pipeline,
    const RhiRtShaderTableDesc& desc, const char* debugName)
{
    if (!pipeline)
        return nullptr;

    Buffer* cache = nullptr;
    if (desc.isCached)
    {
        if (desc.maxEntries == 0)
        {
            m_Context.error("maxEntries must be nonzero for a cached ShaderTable");
            return nullptr;
        }

        BufferDesc bufferDesc{};
        bufferDesc.byteSize = uint64_t(pipeline->getShaderTableEntrySize()) * desc.maxEntries;
        bufferDesc.isShaderBindingTable = 1;
        bufferDesc.initialState = ResourceStates::ShaderResource;
        bufferDesc.keepInitialState = 1;
        bufferDesc.debugName = debugName ? debugName : "";

        cache = createBuffer(bufferDesc);
        if (!cache)
            return nullptr;
    }

    auto* shaderTable = new ShaderTable(m_Context, pipeline, desc);
    shaderTable->debugName = debugName ? debugName : "";
    shaderTable->cache = cache;
    if (cache)
    {
    }

    RegisterResource(shaderTable);
    return shaderTable;
}

uint32_t RayTracingPipeline::getShaderTableEntrySize() const
{
    uint32_t requiredSize =
        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + uint32_t(sizeof(uint64_t)) * maxLocalRootParameters;
    return align(requiredSize, uint32_t(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT));
}

uint64_t AccelStruct::getDeviceAddress() const
{
    return dataBuffer ? dataBuffer->gpuVA : 0;
}

bool Device::getAccelStructPreBuildInfo(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& outPreBuildInfo,
    const AccelStructDesc& desc) const
{
    if (!m_Context.device5)
        return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
    fillAsInputDescForPreBuildInfo(inputs, geometryDescs, desc,
        desc.bottomLevelGeometries.data(), uint32_t(desc.bottomLevelGeometries.size()));

    m_Context.device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &outPreBuildInfo);
    return true;
}

// Port of nvrhi Device::createAccelStruct.
AccelStruct* Device::createAccelStruct(const AccelStructDesc& desc)
{
    if (!m_Context.device5)
    {
        LogError("[UnityRHI] createAccelStruct '%s': ray tracing is not supported on this device.",
            desc.debugName.c_str());
        return nullptr;
    }

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ASPreBuildInfo = {};
    if (!getAccelStructPreBuildInfo(ASPreBuildInfo, desc))
        return nullptr;

    auto* as = new AccelStruct(m_Context);
    as->debugName = desc.debugName;
    as->desc = desc;
    as->allowUpdate = (desc.buildFlags & RhiRtAccelStructBuildFlags::AllowUpdate) != 0;

    {
        BufferDesc bufferDesc{};
        bufferDesc.canHaveUAVs = 1;
        bufferDesc.byteSize = ASPreBuildInfo.ResultDataMaxSizeInBytes;
        bufferDesc.initialState =
            desc.isTopLevel ? ResourceStates::AccelStructRead : ResourceStates::AccelStructBuildBlas;
        bufferDesc.keepInitialState = 1;
        bufferDesc.isAccelStructStorage = 1;
        bufferDesc.isVirtual = desc.isVirtual;
        bufferDesc.debugName = desc.debugName;
        Buffer* buffer = createBuffer(bufferDesc);
        if (!buffer)
        {
            delete as;
            return nullptr;
        }
        as->dataBuffer = buffer;
    }

    // Sanitize the geometry data to avoid dangling pointers, we don't need these buffers in the desc
    for (auto& geometry : as->desc.bottomLevelGeometries)
    {
        static_assert(offsetof(RhiRtGeometryTriangles, indexBuffer)
            == offsetof(RhiRtGeometryAABBs, buffer));
        static_assert(offsetof(RhiRtGeometryTriangles, vertexBuffer)
            == offsetof(RhiRtGeometryAABBs, unused));

        // Clear only the triangles' data, because the other types' data is aliased to triangles (verified above)
        geometry.geometryData.triangles.indexBuffer = 0;
        geometry.geometryData.triangles.vertexBuffer = 0;
    }

    RegisterResource(as);
    return as;
}

MemoryRequirements Device::getAccelStructMemoryRequirements(AccelStruct* accelStruct) const
{
    return accelStruct ? getBufferMemoryRequirements(accelStruct->dataBuffer) : MemoryRequirements{};
}

bool Device::bindAccelStructMemory(AccelStruct* accelStruct, Heap* heap, uint64_t offset)
{
    return accelStruct && bindBufferMemory(accelStruct->dataBuffer, heap, offset);
}

void AccelStruct::createSRV(size_t descriptor) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = dataBuffer->gpuVA;

    m_Context.device->CreateShaderResourceView(nullptr, &srvDesc, {descriptor});
}

// ---------------------------------------------------------------- pipeline

// Port of nvrhi Device::createRayTracingPipeline.
RayTracingPipeline* Device::createRayTracingPipeline(const RhiRtPipelineDesc& desc,
    const RhiRtPipelineShaderDesc* shaders, uint32_t shaderCount,
    const RhiRtPipelineHitGroupDesc* hitGroups, uint32_t hitGroupCount,
    BindingLayout* const* globalLayouts, uint32_t globalLayoutCount, const char* debugName)
{
    if (!m_Context.device5)
    {
        LogError("[UnityRHI] createRayTracingPipeline '%s': ray tracing is not supported on this device.",
            debugName ? debugName : "");
        return nullptr;
    }

    if (desc.hlslExtensionsUAV >= 0)
    {
        // NVAPI is not ported; the SER/extension slot cannot be honored.
        LogError("[UnityRHI] createRayTracingPipeline '%s': hlslExtensionsUAV requires NVAPI (not supported).",
            debugName ? debugName : "");
        return nullptr;
    }

    auto* pso = new RayTracingPipeline(m_Context);
    pso->debugName = debugName ? debugName : "";
    pso->maxLocalRootParameters = 0;

    // Collect all DXIL libraries that are referenced in `desc`, and enumerate their exports.
    // Build local root signatures for all referenced local binding layouts.
    // Convert the export names to wstring.

    struct Library
    {
        const void* pBlob = nullptr;
        size_t blobSize = 0;
        std::vector<std::pair<std::wstring, std::wstring>> exports; // vector(originalName, newName)
        std::vector<D3D12_EXPORT_DESC> d3dExports;
    };

    // Go through the individual shaders first.

    // Match NVRHI's state-object construction exactly. In particular, DXIL
    // library subobjects are emitted in the implementation's unordered-map
    // iteration order rather than in first-use order.
    std::unordered_map<const void*, Library> dxilLibraries;

    for (uint32_t shaderIndex = 0; shaderIndex < shaderCount; shaderIndex++)
    {
        const RhiRtPipelineShaderDesc& shaderDesc = shaders[shaderIndex];
        auto* shader = reinterpret_cast<Shader*>(uintptr_t(shaderDesc.shader));
        auto* bindingLayout = reinterpret_cast<BindingLayout*>(uintptr_t(shaderDesc.bindingLayout));
        if (!shader)
        {
            LogError("[UnityRHI] createRayTracingPipeline '%s': shader %u is null.",
                pso->debugName.c_str(), shaderIndex);
            delete pso;
            return nullptr;
        }

        const void* pBlob = nullptr;
        size_t blobSize = 0;
        shader->getBytecode(&pBlob, &blobSize);

        // Assuming that no shader is referenced twice, we just add every shader to its library export list.

        Library& library = dxilLibraries[pBlob];
        library.pBlob = pBlob;
        library.blobSize = blobSize;

        std::string originalShaderName = shader->desc.entryName;
        std::string newShaderName = (!shaderDesc.exportName || !shaderDesc.exportName[0])
            ? originalShaderName
            : shaderDesc.exportName;

        library.exports.push_back(std::make_pair<std::wstring, std::wstring>(
            std::wstring(originalShaderName.begin(), originalShaderName.end()),
            std::wstring(newShaderName.begin(), newShaderName.end())));

        // Build a local root signature for the shader, if needed.

        if (bindingLayout)
        {
            std::shared_ptr<RootSignature>& localRS = pso->localRootSignatures[bindingLayout];
            if (!localRS)
            {
                BindingLayout* layouts[] = {bindingLayout};
                localRS = buildRootSignature(layouts, 1, /*isLocal =*/true);
                if (!localRS)
                {
                    delete pso;
                    return nullptr;
                }

                pso->maxLocalRootParameters =
                    std::max(pso->maxLocalRootParameters, uint32_t(bindingLayout->rootParameters.size()));
            }
        }
    }

    // Still in the collection phase - go through the hit groups.
    // Rename all exports used in the hit groups to avoid collisions between different libraries.

    std::vector<D3D12_HIT_GROUP_DESC> d3dHitGroups;
    std::unordered_map<Shader*, std::wstring> hitGroupShaderNames;
    std::vector<std::wstring> hitGroupExportNames;
    hitGroupExportNames.reserve(hitGroupCount);

    for (uint32_t hitGroupIndex = 0; hitGroupIndex < hitGroupCount; hitGroupIndex++)
    {
        const RhiRtPipelineHitGroupDesc& hitGroupDesc = hitGroups[hitGroupIndex];
        auto* closestHitShader = reinterpret_cast<Shader*>(uintptr_t(hitGroupDesc.closestHitShader));
        auto* anyHitShader = reinterpret_cast<Shader*>(uintptr_t(hitGroupDesc.anyHitShader));
        auto* intersectionShader = reinterpret_cast<Shader*>(uintptr_t(hitGroupDesc.intersectionShader));
        auto* bindingLayout = reinterpret_cast<BindingLayout*>(uintptr_t(hitGroupDesc.bindingLayout));

        for (Shader* shader : {closestHitShader, anyHitShader, intersectionShader})
        {
            if (!shader)
                continue;

            std::wstring& newName = hitGroupShaderNames[shader];

            // See if we've encountered this particular shader before...

            if (newName.empty())
            {
                // No - add it to the corresponding library, come up with a new name for it.

                const void* pBlob = nullptr;
                size_t blobSize = 0;
                shader->getBytecode(&pBlob, &blobSize);

                Library& library = dxilLibraries[pBlob];
                library.pBlob = pBlob;
                library.blobSize = blobSize;

                std::string originalShaderName = shader->desc.entryName;
                std::string newShaderName = originalShaderName + std::to_string(hitGroupShaderNames.size());

                library.exports.push_back(std::make_pair<std::wstring, std::wstring>(
                    std::wstring(originalShaderName.begin(), originalShaderName.end()),
                    std::wstring(newShaderName.begin(), newShaderName.end())));

                newName = std::wstring(newShaderName.begin(), newShaderName.end());
            }
        }

        // Build a local root signature for the hit group, if needed.

        if (bindingLayout)
        {
            std::shared_ptr<RootSignature>& localRS = pso->localRootSignatures[bindingLayout];
            if (!localRS)
            {
                BindingLayout* layouts[] = {bindingLayout};
                localRS = buildRootSignature(layouts, 1, /*isLocal =*/true);
                if (!localRS)
                {
                    delete pso;
                    return nullptr;
                }

                pso->maxLocalRootParameters =
                    std::max(pso->maxLocalRootParameters, uint32_t(bindingLayout->rootParameters.size()));
            }
        }

        // Create a hit group descriptor and store the new export names in it.

        D3D12_HIT_GROUP_DESC d3dHitGroupDesc = {};
        if (anyHitShader)
            d3dHitGroupDesc.AnyHitShaderImport = hitGroupShaderNames[anyHitShader].c_str();
        if (closestHitShader)
            d3dHitGroupDesc.ClosestHitShaderImport = hitGroupShaderNames[closestHitShader].c_str();
        if (intersectionShader)
            d3dHitGroupDesc.IntersectionShaderImport = hitGroupShaderNames[intersectionShader].c_str();

        if (hitGroupDesc.isProceduralPrimitive)
            d3dHitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        else
            d3dHitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;

        std::string exportNameA = hitGroupDesc.exportName ? hitGroupDesc.exportName : "";
        std::wstring hitGroupExportName = std::wstring(exportNameA.begin(), exportNameA.end());
        hitGroupExportNames.push_back(hitGroupExportName); // store the wstring so that it's not deallocated
        d3dHitGroupDesc.HitGroupExport = hitGroupExportNames[hitGroupExportNames.size() - 1].c_str();
        d3dHitGroups.push_back(d3dHitGroupDesc);
    }

    // Create descriptors for DXIL libraries, enumerate the exports used from each library.

    std::vector<D3D12_DXIL_LIBRARY_DESC> d3dDxilLibraries;
    d3dDxilLibraries.reserve(dxilLibraries.size());
    for (auto& it : dxilLibraries)
    {
        Library& library = it.second;
        for (const std::pair<std::wstring, std::wstring>& exportNames : library.exports)
        {
            D3D12_EXPORT_DESC d3dExportDesc = {};
            d3dExportDesc.ExportToRename = exportNames.first.c_str();
            d3dExportDesc.Name = exportNames.second.c_str();
            d3dExportDesc.Flags = D3D12_EXPORT_FLAG_NONE;
            library.d3dExports.push_back(d3dExportDesc);
        }

        D3D12_DXIL_LIBRARY_DESC d3dLibraryDesc = {};
        d3dLibraryDesc.DXILLibrary.pShaderBytecode = library.pBlob;
        d3dLibraryDesc.DXILLibrary.BytecodeLength = library.blobSize;
        d3dLibraryDesc.NumExports = UINT(library.d3dExports.size());
        d3dLibraryDesc.pExports = library.d3dExports.data();

        d3dDxilLibraries.push_back(d3dLibraryDesc);
    }

    // Start building the D3D state subobject array.

    std::vector<D3D12_STATE_SUBOBJECT> d3dSubobjects;

    // Same subobject is reused multiple times and copied to the vector each time.
    D3D12_STATE_SUBOBJECT d3dSubobject = {};

    // Subobject: Shader config

    D3D12_RAYTRACING_SHADER_CONFIG d3dShaderConfig = {};
    d3dShaderConfig.MaxAttributeSizeInBytes = desc.maxAttributeSize;
    d3dShaderConfig.MaxPayloadSizeInBytes = desc.maxPayloadSize;

    d3dSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    d3dSubobject.pDesc = &d3dShaderConfig;
    d3dSubobjects.push_back(d3dSubobject);

    // Subobject: Pipeline config

    d3dSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1;
    D3D12_RAYTRACING_PIPELINE_CONFIG1 d3dPipelineConfig = {};

    d3dPipelineConfig.MaxTraceRecursionDepth = desc.maxRecursionDepth;
    if (desc.allowOpacityMicromaps)
        d3dPipelineConfig.Flags = D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS;
    d3dSubobject.pDesc = &d3dPipelineConfig;
    d3dSubobjects.push_back(d3dSubobject);

    // Subobjects: DXIL libraries

    for (const D3D12_DXIL_LIBRARY_DESC& d3dLibraryDesc : d3dDxilLibraries)
    {
        d3dSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        d3dSubobject.pDesc = &d3dLibraryDesc;
        d3dSubobjects.push_back(d3dSubobject);
    }

    // Subobjects: hit groups

    for (const D3D12_HIT_GROUP_DESC& d3dHitGroupDesc : d3dHitGroups)
    {
        d3dSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        d3dSubobject.pDesc = &d3dHitGroupDesc;
        d3dSubobjects.push_back(d3dSubobject);
    }

    // Subobject: global root signature

    D3D12_GLOBAL_ROOT_SIGNATURE d3dGlobalRootSignature = {};

    if (globalLayoutCount > 0)
    {
        pso->globalRootSignature = buildRootSignature(globalLayouts, globalLayoutCount, /*isLocal =*/false);
        if (!pso->globalRootSignature)
        {
            delete pso;
            return nullptr;
        }
        d3dGlobalRootSignature.pGlobalRootSignature = pso->globalRootSignature->handle.Get();

        d3dSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        d3dSubobject.pDesc = &d3dGlobalRootSignature;
        d3dSubobjects.push_back(d3dSubobject);
    }

    // Subobjects: local root signatures

    // Make sure that adding local root signatures does not resize the arrays,
    // because we need to store pointers to array elements there.
    std::vector<D3D12_LOCAL_ROOT_SIGNATURE> d3dLocalRootSignatures;
    std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> d3dAssociations;
    d3dLocalRootSignatures.reserve(pso->localRootSignatures.size());
    d3dAssociations.reserve(pso->localRootSignatures.size());
    d3dSubobjects.reserve(d3dSubobjects.size() + pso->localRootSignatures.size() * 2);

    // Same - pre-allocate the arrays to avoid resizing them
    size_t numAssociations = size_t(shaderCount) + hitGroupCount;
    std::vector<std::wstring> d3dAssociationExports;
    std::vector<LPCWSTR> d3dAssociationExportsCStr;
    d3dAssociationExports.reserve(numAssociations);
    d3dAssociationExportsCStr.reserve(numAssociations);

    for (const auto& it : pso->localRootSignatures)
    {
        D3D12_LOCAL_ROOT_SIGNATURE* d3dLocalRootSignature = &d3dLocalRootSignatures.emplace_back();
        d3dLocalRootSignature->pLocalRootSignature = it.second->handle.Get();

        d3dSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
        d3dSubobject.pDesc = d3dLocalRootSignature;
        d3dSubobjects.push_back(d3dSubobject);

        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION* d3dAssociation = &d3dAssociations.emplace_back();
        d3dAssociation->pSubobjectToAssociate = &d3dSubobjects[d3dSubobjects.size() - 1];
        d3dAssociation->NumExports = 0;
        size_t firstExportIndex = d3dAssociationExportsCStr.size();

        for (uint32_t shaderIndex = 0; shaderIndex < shaderCount; shaderIndex++)
        {
            const RhiRtPipelineShaderDesc& shaderDesc = shaders[shaderIndex];
            if (reinterpret_cast<BindingLayout*>(uintptr_t(shaderDesc.bindingLayout)) == it.first)
            {
                auto* shader = reinterpret_cast<Shader*>(uintptr_t(shaderDesc.shader));
                std::string exportName = (!shaderDesc.exportName || !shaderDesc.exportName[0])
                    ? shader->desc.entryName
                    : shaderDesc.exportName;
                std::wstring exportNameW = std::wstring(exportName.begin(), exportName.end());
                d3dAssociationExports.push_back(exportNameW);
                d3dAssociationExportsCStr.push_back(d3dAssociationExports[d3dAssociationExports.size() - 1].c_str());
                d3dAssociation->NumExports += 1;
            }
        }

        for (uint32_t hitGroupIndex = 0; hitGroupIndex < hitGroupCount; hitGroupIndex++)
        {
            const RhiRtPipelineHitGroupDesc& hitGroupDesc = hitGroups[hitGroupIndex];
            if (reinterpret_cast<BindingLayout*>(uintptr_t(hitGroupDesc.bindingLayout)) == it.first)
            {
                std::string exportNameA = hitGroupDesc.exportName ? hitGroupDesc.exportName : "";
                std::wstring exportNameW = std::wstring(exportNameA.begin(), exportNameA.end());
                d3dAssociationExports.push_back(exportNameW);
                d3dAssociationExportsCStr.push_back(d3dAssociationExports[d3dAssociationExports.size() - 1].c_str());
                d3dAssociation->NumExports += 1;
            }
        }

        d3dAssociation->pExports = &d3dAssociationExportsCStr[firstExportIndex];

        d3dSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
        d3dSubobject.pDesc = d3dAssociation;
        d3dSubobjects.push_back(d3dSubobject);
    }

    // Top-level PSO descriptor structure

    D3D12_STATE_OBJECT_DESC pipelineDesc = {};
    pipelineDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    pipelineDesc.NumSubobjects = static_cast<UINT>(d3dSubobjects.size());
    pipelineDesc.pSubobjects = d3dSubobjects.data();

    HRESULT hr = m_Context.device5->CreateStateObject(&pipelineDesc, IID_PPV_ARGS(&pso->pipelineState));

    if (FAILED(hr))
    {
        m_Context.error("Failed to create a DXR pipeline state object");
        delete pso;
        return nullptr;
    }

    hr = pso->pipelineState->QueryInterface(IID_PPV_ARGS(&pso->pipelineInfo));
    if (FAILED(hr))
    {
        m_Context.error("Failed to get a DXR pipeline info interface from a PSO");
        delete pso;
        return nullptr;
    }

    for (uint32_t shaderIndex = 0; shaderIndex < shaderCount; shaderIndex++)
    {
        const RhiRtPipelineShaderDesc& shaderDesc = shaders[shaderIndex];
        auto* shader = reinterpret_cast<Shader*>(uintptr_t(shaderDesc.shader));
        auto* bindingLayout = reinterpret_cast<BindingLayout*>(uintptr_t(shaderDesc.bindingLayout));

        std::string exportName = (shaderDesc.exportName && shaderDesc.exportName[0])
            ? shaderDesc.exportName
            : shader->desc.entryName;
        std::wstring exportNameW = std::wstring(exportName.begin(), exportName.end());
        const void* pShaderIdentifier = pso->pipelineInfo->GetShaderIdentifier(exportNameW.c_str());

        if (pShaderIdentifier == nullptr)
        {
            m_Context.error("Failed to get an identifier for a shader in a fresh DXR PSO");
            delete pso;
            return nullptr;
        }

        pso->exports[exportName] = RayTracingPipeline::ExportTableEntry{bindingLayout, pShaderIdentifier};
    }

    for (uint32_t hitGroupIndex = 0; hitGroupIndex < hitGroupCount; hitGroupIndex++)
    {
        const RhiRtPipelineHitGroupDesc& hitGroupDesc = hitGroups[hitGroupIndex];
        auto* bindingLayout = reinterpret_cast<BindingLayout*>(uintptr_t(hitGroupDesc.bindingLayout));

        std::string exportNameA = hitGroupDesc.exportName ? hitGroupDesc.exportName : "";
        std::wstring exportNameW = std::wstring(exportNameA.begin(), exportNameA.end());
        const void* pShaderIdentifier = pso->pipelineInfo->GetShaderIdentifier(exportNameW.c_str());

        if (pShaderIdentifier == nullptr)
        {
            m_Context.error("Failed to get an identifier for a hit group in a fresh DXR PSO");
            delete pso;
            return nullptr;
        }

        pso->exports[exportNameA] = RayTracingPipeline::ExportTableEntry{bindingLayout, pShaderIdentifier};
    }

    RegisterResource(pso);
    return pso;
}

// ------------------------------------------------------------- shader table

// Port of nvrhi ShaderTable::bake.
void ShaderTable::bake(uint8_t* cpuVA, D3D12_GPU_VIRTUAL_ADDRESS gpuVA, DeviceResources& resources,
    ShaderTableState& state)
{
    uint32_t const entrySize = pipeline->getShaderTableEntrySize();

    auto writeEntry = [this, &resources, entrySize, &cpuVA, &gpuVA](const ShaderTable::Entry& entry)
    {
        memcpy(cpuVA, entry.pShaderIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

        if (entry.localBindings)
        {
            BindingSet* bindingSet = entry.localBindings;
            BindingLayout* layout = bindingSet->layout;

            if (layout->descriptorTableSizeSamplers > 0)
            {
                auto pTable = reinterpret_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(cpuVA
                    + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + layout->rootParameterSamplers * sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));
                *pTable = resources.samplerHeap.getGpuHandle(bindingSet->descriptorTableSamplers);
            }

            if (layout->descriptorTableSizeSRVetc > 0)
            {
                auto pTable = reinterpret_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(cpuVA
                    + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + layout->rootParameterSRVetc * sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));
                *pTable = resources.shaderResourceViewHeap.getGpuHandle(bindingSet->descriptorTableSRVetc);
            }

            if (!layout->rootParametersVolatileCB.empty())
            {
                m_Context.error("Cannot use Volatile CBs in a shader binding table");
                return;
            }
        }

        cpuVA += entrySize;
        gpuVA += entrySize;
    };

    D3D12_DISPATCH_RAYS_DESC& drd = state.dispatchRaysTemplate;
    memset(&drd, 0, sizeof(D3D12_DISPATCH_RAYS_DESC));

    drd.RayGenerationShaderRecord.StartAddress = gpuVA;
    drd.RayGenerationShaderRecord.SizeInBytes = entrySize;
    writeEntry(rayGenerationShader);

    if (!missShaders.empty())
    {
        drd.MissShaderTable.StartAddress = gpuVA;
        drd.MissShaderTable.StrideInBytes = (missShaders.size() == 1) ? 0 : entrySize;
        drd.MissShaderTable.SizeInBytes = uint32_t(missShaders.size()) * entrySize;

        for (auto& entry : missShaders)
            writeEntry(entry);
    }

    if (!hitGroups.empty())
    {
        drd.HitGroupTable.StartAddress = gpuVA;
        drd.HitGroupTable.StrideInBytes = (hitGroups.size() == 1) ? 0 : entrySize;
        drd.HitGroupTable.SizeInBytes = uint32_t(hitGroups.size()) * entrySize;

        for (auto& entry : hitGroups)
            writeEntry(entry);
    }

    if (!callableShaders.empty())
    {
        drd.CallableShaderTable.StartAddress = gpuVA;
        drd.CallableShaderTable.StrideInBytes = (callableShaders.size() == 1) ? 0 : entrySize;
        drd.CallableShaderTable.SizeInBytes = uint32_t(callableShaders.size()) * entrySize;

        for (auto& entry : callableShaders)
            writeEntry(entry);
    }

    state.committedVersion = version;
    if (pipeline->hasLocalResources())
    {
        state.descriptorHeapSRV = resources.shaderResourceViewHeap.getShaderVisibleHeap();
        state.descriptorHeapSamplers = resources.samplerHeap.getShaderVisibleHeap();
    }
    else
    {
        state.descriptorHeapSRV = nullptr;
        state.descriptorHeapSamplers = nullptr;
    }
}

bool ShaderTable::isStateValid(ShaderTableState const& state, DeviceResources const& resources) const
{
    if (pipeline->hasLocalResources())
    {
        return state.committedVersion == version &&
            state.descriptorHeapSRV == resources.shaderResourceViewHeap.getShaderVisibleHeap() &&
            state.descriptorHeapSamplers == resources.samplerHeap.getShaderVisibleHeap();
    }
    else
    {
        return state.committedVersion == version;
    }
}
// Port of nvrhi CommandList::getShaderTableState.
ShaderTableState& ReplayContext::getShaderTableState(ShaderTable* shaderTable)
{
    if (shaderTable->desc.isCached)
        return shaderTable->cacheState;

    return uncachedShaderTableStates[shaderTable];
}

// Port of nvrhi CommandList::setRayTracingState. The binding-set handles are
// decoded and commandList4 is acquired by the replayer (CommandStream.cpp).
bool ReplayContext::setRayTracingState(ShaderTable* shaderTable, const std::vector<RhiResource*>& bindings)
{
    Device* device = Device::Get();
    if (!shaderTable || !shaderTable->pipeline || !device || !commandList4)
        return false;

    RayTracingPipeline* pso = shaderTable->pipeline;

    // Rebuild the SBT if it's uncached and we're using it for the first time
    // in this command list, or if it's been changed since the previous build,
    // or if the resource or sampler heaps have changed.
    const bool shaderTableCached = shaderTable->desc.isCached != 0;
    ShaderTableState& shaderTableState = getShaderTableState(shaderTable);
    const bool rebuildShaderTable = !shaderTable->isStateValid(shaderTableState, device->resources());

    if (rebuildShaderTable)
    {
        const size_t shaderTableSize = shaderTable->getUploadSize();
        if (shaderTableCached && (!shaderTable->cache || shaderTableSize > shaderTable->cache->desc.byteSize))
        {
            LogError("[UnityRHI] Required shader table size is larger than ShaderTableDesc::maxEntries.");
            return false;
        }

        ID3D12Resource* uploadBuffer = nullptr;
        uint64_t uploadOffset = 0;
        uint8_t* uploadCpuVA = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS uploadGpuVA = 0;
        if (!device->uploadManager().suballocateBuffer(shaderTableSize, nullptr, &uploadBuffer, &uploadOffset,
                reinterpret_cast<void**>(&uploadCpuVA), &uploadGpuVA, fenceValue,
                D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT))
        {
            LogError("[UnityRHI] Couldn't suballocate an upload buffer for a shader table.");
            return false;
        }

        const D3D12_GPU_VIRTUAL_ADDRESS effectiveGpuVA = shaderTableCached ? shaderTable->cache->gpuVA : uploadGpuVA;
        shaderTable->bake(uploadCpuVA, effectiveGpuVA, device->resources(), shaderTableState);

        if (shaderTableCached)
        {
            // Not conditional on enableAutomaticBarriers because the cache is
            // an internal object, invisible to the application (nvrhi
            // setRayTracingState does the same).
            requireBufferState(shaderTable->cache, ResourceStates::CopyDest);
            commitBarriers();

            commandList->CopyBufferRegion(shaderTable->cache->resource.Get(), 0, uploadBuffer,
                uploadOffset, shaderTableSize);
        }
    }

    if (shaderTableCached)
    {
        // Ensure that the cache buffer is in the right state (see above);
        // committed together with the binding-set barriers below.
        requireBufferState(shaderTable->cache, ResourceStates::ShaderResource);
    }

    const bool updateRootSignature = !rayTracingStateActive || currentShaderTable == nullptr ||
        currentShaderTable->pipeline->globalRootSignature != pso->globalRootSignature;
    const bool updatePipeline = !rayTracingStateActive || currentShaderTable == nullptr ||
        currentShaderTable->pipeline != pso;

    uint32_t bindingUpdateMask = 0;
    if (updateRootSignature)
        bindingUpdateMask = ~0u;

    if (commitDescriptorHeaps())
        bindingUpdateMask = ~0u;

    if (bindingUpdateMask == 0)
        bindingUpdateMask = arrayDifferenceMask(currentRayTracingBindings, bindings);

    if (updateRootSignature)
    {
        if (!pso->globalRootSignature)
        {
            LogError("[UnityRHI] Ray tracing pipeline has no global root signature.");
            return false;
        }
        commandList4->SetComputeRootSignature(pso->globalRootSignature->handle.Get());
    }

    if (updatePipeline)
    {
        commandList4->SetPipelineState1(pso->pipelineState.Get());
        pso->lastUseFenceValue = fenceValue;
    }

    if (!setComputeBindings(bindings, bindingUpdateMask, nullptr, false, pso->globalRootSignature.get()))
        return false;

    shaderTable->lastUseFenceValue = fenceValue;
    currentPipeline = nullptr;
    graphicsStateActive = false;
    currentShaderTable = shaderTable;
    currentRayTracingBindings.assign(bindings.begin(), bindings.end());
    rayTracingStateActive = true;
    bindingStatesDirty = false;

    commitBarriers();
    return true;
}

// Port of nvrhi CommandList::dispatchRays.
bool ReplayContext::dispatchRays(uint32_t width, uint32_t height, uint32_t depth)
{
    if (!commandList4 || !currentShaderTable || !rayTracingStateActive)
    {
        LogError("[UnityRHI] DispatchRays requires SetRayTracingState first.");
        return false;
    }

    updateComputeVolatileBuffers();

    ShaderTableState& shaderTableState = getShaderTableState(currentShaderTable);
    D3D12_DISPATCH_RAYS_DESC desc = shaderTableState.dispatchRaysTemplate;
    desc.Width = width;
    desc.Height = height;
    desc.Depth = depth;

    commandList4->DispatchRays(&desc);
    return true;
}

// Port of nvrhi CommandList::buildBottomLevelAccelStruct: the build-input
// states are required for all geometries first and committed in one batch.
bool ReplayContext::buildBottomLevelAccelStruct(AccelStruct* as, const RhiRtGeometryDesc* geometries,
    uint32_t geometryCount, RhiRtAccelStructBuildFlags buildFlags)
{
    Device* device = Device::Get();
    if (!as || !as->dataBuffer || !device || !device->GetD3DDevice5() || !commandList4)
        return false;

    const bool performUpdate = (buildFlags & RhiRtAccelStructBuildFlags::PerformUpdate) != 0;

    for (uint32_t i = 0; i < geometryCount; ++i)
    {
        const RhiRtGeometryDesc& geometry = geometries[i];
        if (geometry.geometryType == RhiRtGeometryType::Triangles)
        {
            auto* indexBuffer = reinterpret_cast<Buffer*>(uintptr_t(geometry.geometryData.triangles.indexBuffer));
            auto* vertexBuffer = reinterpret_cast<Buffer*>(uintptr_t(geometry.geometryData.triangles.vertexBuffer));
            if (enableAutomaticBarriers)
            {
                requireBufferState(indexBuffer, ResourceStates::AccelStructBuildInput);
                requireBufferState(vertexBuffer, ResourceStates::AccelStructBuildInput);
            }
            if (indexBuffer)
                indexBuffer->lastUseFenceValue = fenceValue;
            if (vertexBuffer)
                vertexBuffer->lastUseFenceValue = fenceValue;
        }
        else
        {
            auto* buffer = reinterpret_cast<Buffer*>(uintptr_t(geometry.geometryData.aabbs.buffer));
            if (enableAutomaticBarriers)
                requireBufferState(buffer, ResourceStates::AccelStructBuildInput);
            if (buffer)
                buffer->lastUseFenceValue = fenceValue;
        }
    }

    if (enableAutomaticBarriers)
        bindingStatesDirty = true;
    commitBarriers();

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> d3dGeometryDescs(geometryCount);
    for (uint32_t i = 0; i < geometryCount; ++i)
    {
        const RhiRtGeometryDesc& geometry = geometries[i];

        D3D12_GPU_VIRTUAL_ADDRESS transformGpuVA = 0;
        if (geometry.useTransform)
        {
            void* cpuVA = nullptr;
            ID3D12Resource* uploadBuffer = nullptr;
            uint64_t uploadOffset = 0;
            if (!device->uploadManager().suballocateBuffer(sizeof(geometry.transform), nullptr, &uploadBuffer,
                    &uploadOffset, &cpuVA, &transformGpuVA, fenceValue,
                    D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT))
                return false;
            std::memcpy(cpuVA, geometry.transform, sizeof(geometry.transform));
        }

        fillD3dGeometryDesc(d3dGeometryDescs[i], geometry, transformGpuVA);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = convertAccelStructBuildFlags(buildFlags);
    if (as->allowUpdate)
        inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = geometryCount;
    inputs.pGeometryDescs = d3dGeometryDescs.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetD3DDevice5()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    if (prebuild.ResultDataMaxSizeInBytes > as->dataBuffer->desc.byteSize)
    {
        LogError("[UnityRHI] BLAS '%s' build requires %llu bytes, allocated %llu.",
            as->debugName.c_str(),
            static_cast<unsigned long long>(prebuild.ResultDataMaxSizeInBytes),
            static_cast<unsigned long long>(as->dataBuffer->desc.byteSize));
        return false;
    }

    const uint64_t scratchSize = performUpdate ? prebuild.UpdateScratchDataSizeInBytes : prebuild.ScratchDataSizeInBytes;
    D3D12_GPU_VIRTUAL_ADDRESS scratchGpuVA = 0;
    if (!device->scratchManager().suballocateBuffer(scratchSize, commandList, nullptr, nullptr, nullptr,
            &scratchGpuVA, fenceValue, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT))
        return false;

    if (enableAutomaticBarriers)
    {
        requireBufferState(as->dataBuffer, ResourceStates::AccelStructWrite);
        bindingStatesDirty = true;
    }
    commitBarriers();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData = scratchGpuVA;
    buildDesc.DestAccelerationStructureData = as->dataBuffer->gpuVA;
    buildDesc.SourceAccelerationStructureData = performUpdate ? as->dataBuffer->gpuVA : 0;
    commandList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    as->lastUseFenceValue = as->dataBuffer->lastUseFenceValue = fenceValue;
    return true;
}

// Port of nvrhi CommandList::buildTopLevelAccelStructInternal.
bool ReplayContext::buildTopLevelAccelStructInternal(AccelStruct* as, D3D12_GPU_VIRTUAL_ADDRESS instanceData,
    uint32_t numInstances, RhiRtAccelStructBuildFlags buildFlags)
{
    Device* device = Device::Get();
    if (!device || !device->GetD3DDevice5() || !commandList4 || !as || !as->dataBuffer)
        return false;

    const bool performUpdate = (buildFlags & RhiRtAccelStructBuildFlags::PerformUpdate) != 0;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.InstanceDescs = instanceData;
    inputs.NumDescs = numInstances;
    inputs.Flags = convertAccelStructBuildFlags(buildFlags);
    if (as->allowUpdate)
        inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetD3DDevice5()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    if (prebuild.ResultDataMaxSizeInBytes > as->dataBuffer->desc.byteSize)
        return false;

    const uint64_t scratchSize = performUpdate ? prebuild.UpdateScratchDataSizeInBytes : prebuild.ScratchDataSizeInBytes;
    D3D12_GPU_VIRTUAL_ADDRESS scratchGpuVA = 0;
    if (!device->scratchManager().suballocateBuffer(scratchSize, commandList, nullptr, nullptr, nullptr,
            &scratchGpuVA, fenceValue, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT))
        return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData = scratchGpuVA;
    buildDesc.DestAccelerationStructureData = as->dataBuffer->gpuVA;
    buildDesc.SourceAccelerationStructureData = performUpdate ? as->dataBuffer->gpuVA : 0;
    commandList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    return true;
}

// Port of nvrhi CommandList::buildTopLevelAccelStruct.
bool ReplayContext::buildTopLevelAccelStruct(AccelStruct* as, const RhiRtInstanceDesc* instances,
    uint32_t instanceCount, RhiRtAccelStructBuildFlags buildFlags)
{
    Device* device = Device::Get();
    if (!as || !device || !commandList4)
        return false;

    // Keep the dxrInstances array in the AS object to avoid reallocating it
    // on the next update.
    as->dxrInstances.resize(instanceCount);
    for (uint32_t i = 0; i < instanceCount; ++i)
    {
        static_assert(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) == sizeof(RhiRtInstanceDesc));
        std::memcpy(&as->dxrInstances[i], &instances[i], sizeof(RhiRtInstanceDesc));

        auto* blas = reinterpret_cast<AccelStruct*>(uintptr_t(instances[i].bottomLevelAS));
        if (blas && blas->dataBuffer)
        {
            as->dxrInstances[i].AccelerationStructure = blas->dataBuffer->gpuVA;
            if (enableAutomaticBarriers)
                requireBufferState(blas->dataBuffer, ResourceStates::AccelStructBuildBlas);
            blas->lastUseFenceValue = blas->dataBuffer->lastUseFenceValue = fenceValue;
        }
        else
        {
            as->dxrInstances[i].AccelerationStructure = 0;
        }
    }

    // Copy the instance array to the GPU.
    D3D12_RAYTRACING_INSTANCE_DESC* cpuVA = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
    const size_t uploadSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * as->dxrInstances.size();
    if (!device->uploadManager().suballocateBuffer(uploadSize, nullptr, nullptr, nullptr,
            reinterpret_cast<void**>(&cpuVA), &gpuVA, fenceValue, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
        return false;
    std::memcpy(cpuVA, as->dxrInstances.data(), uploadSize);

    if (enableAutomaticBarriers)
    {
        requireBufferState(as->dataBuffer, ResourceStates::AccelStructWrite);
        bindingStatesDirty = true;
    }
    commitBarriers();

    if (!buildTopLevelAccelStructInternal(as, gpuVA, instanceCount, buildFlags))
        return false;

    as->lastUseFenceValue = as->dataBuffer->lastUseFenceValue = fenceValue;
    return true;
}

// Port of nvrhi CommandList::buildTopLevelAccelStructFromBuffer.
bool ReplayContext::buildTopLevelAccelStructFromBuffer(AccelStruct* as, Buffer* instanceBuffer,
    uint64_t instanceBufferOffset, uint32_t instanceCount, RhiRtAccelStructBuildFlags buildFlags)
{
    if (!as || !instanceBuffer || !instanceBuffer->resource || !commandList4)
        return false;

    if (enableAutomaticBarriers)
    {
        requireBufferState(as->dataBuffer, ResourceStates::AccelStructWrite);
        requireBufferState(instanceBuffer, ResourceStates::AccelStructBuildInput);
        bindingStatesDirty = true;
    }
    commitBarriers();

    if (!buildTopLevelAccelStructInternal(as, instanceBuffer->gpuVA + instanceBufferOffset, instanceCount, buildFlags))
        return false;

    as->lastUseFenceValue = as->dataBuffer->lastUseFenceValue = instanceBuffer->lastUseFenceValue = fenceValue;
    return true;
}
} // namespace unityrhi
