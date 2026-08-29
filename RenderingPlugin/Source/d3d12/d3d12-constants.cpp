// Enum converters ported from NVRHI's d3d12-constants.cpp
// (External/nvrhi/src/d3d12). Copyright (c) 2014-2021, NVIDIA CORPORATION.
// MIT license (see ThirdParty/NVRHI-LICENSE.txt).
//
// Converters for types the plugin does not expose yet (blend, stencil,
// comparison, primitive topology, shading rate, coopvec) are omitted and
// will be ported together with their features.

#include "d3d12-backend.h"

#include <cassert>

namespace unityrhi
{
D3D12_SHADER_VISIBILITY convertShaderStage(ShaderType s)
{
    switch (s)
    {
    case ShaderType::Vertex:
        return D3D12_SHADER_VISIBILITY_VERTEX;
    case ShaderType::Hull:
        return D3D12_SHADER_VISIBILITY_HULL;
    case ShaderType::Domain:
        return D3D12_SHADER_VISIBILITY_DOMAIN;
    case ShaderType::Geometry:
        return D3D12_SHADER_VISIBILITY_GEOMETRY;
    case ShaderType::Pixel:
        return D3D12_SHADER_VISIBILITY_PIXEL;
    case ShaderType::Amplification:
        return D3D12_SHADER_VISIBILITY_AMPLIFICATION;
    case ShaderType::Mesh:
        return D3D12_SHADER_VISIBILITY_MESH;

    default:
        // catch-all case - actually some of the bitfield combinations are unrepresentable in DX12
        return D3D12_SHADER_VISIBILITY_ALL;
    }
}

D3D_PRIMITIVE_TOPOLOGY convertPrimitiveType(PrimitiveType primType)
{
    switch (primType)
    {
    case PrimitiveType::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case PrimitiveType::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveType::LineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case PrimitiveType::TriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveType::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PrimitiveType::TriangleListWithAdjacency: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
    case PrimitiveType::TriangleStripWithAdjacency: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
    default: return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

D3D12_TEXTURE_ADDRESS_MODE convertSamplerAddressMode(SamplerAddressMode mode)
{
    switch (mode)
    {
    case SamplerAddressMode::Clamp:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case SamplerAddressMode::Wrap:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case SamplerAddressMode::Border:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case SamplerAddressMode::Mirror:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case SamplerAddressMode::MirrorOnce:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

UINT convertSamplerReductionType(SamplerReductionType reductionType)
{
    switch (reductionType)
    {
    case SamplerReductionType::Standard:
        return D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    case SamplerReductionType::Comparison:
        return D3D12_FILTER_REDUCTION_TYPE_COMPARISON;
    case SamplerReductionType::Minimum:
        return D3D12_FILTER_REDUCTION_TYPE_MINIMUM;
    case SamplerReductionType::Maximum:
        return D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
    default:
        return D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    }
}

D3D12_RESOURCE_STATES convertResourceStates(ResourceStates stateBits)
{
    if (stateBits == ResourceStates::Common)
        return D3D12_RESOURCE_STATE_COMMON;

    D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON; // also 0

    if ((stateBits & ResourceStates::ConstantBuffer) != 0) result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if ((stateBits & ResourceStates::VertexBuffer) != 0) result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if ((stateBits & ResourceStates::IndexBuffer) != 0) result |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    if ((stateBits & ResourceStates::IndirectArgument) != 0) result |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if ((stateBits & ResourceStates::PixelShaderResource) != 0) result |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if ((stateBits & ResourceStates::NonPixelShaderResource) != 0) result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if ((stateBits & ResourceStates::UnorderedAccess) != 0) result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if ((stateBits & ResourceStates::RenderTarget) != 0) result |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if ((stateBits & ResourceStates::DepthWrite) != 0) result |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if ((stateBits & ResourceStates::DepthRead) != 0) result |= D3D12_RESOURCE_STATE_DEPTH_READ;
    if ((stateBits & ResourceStates::StreamOut) != 0) result |= D3D12_RESOURCE_STATE_STREAM_OUT;
    if ((stateBits & ResourceStates::CopyDest) != 0) result |= D3D12_RESOURCE_STATE_COPY_DEST;
    if ((stateBits & ResourceStates::CopySource) != 0) result |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if ((stateBits & ResourceStates::ResolveDest) != 0) result |= D3D12_RESOURCE_STATE_RESOLVE_DEST;
    if ((stateBits & ResourceStates::ResolveSource) != 0) result |= D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    if ((stateBits & ResourceStates::Present) != 0) result |= D3D12_RESOURCE_STATE_PRESENT;
    if ((stateBits & ResourceStates::AccelStructRead) != 0) result |= D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    if ((stateBits & ResourceStates::AccelStructWrite) != 0) result |= D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    if ((stateBits & ResourceStates::AccelStructBuildInput) != 0) result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if ((stateBits & ResourceStates::AccelStructBuildBlas) != 0) result |= D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    if ((stateBits & ResourceStates::ShadingRateSurface) != 0) result |= D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    if ((stateBits & ResourceStates::OpacityMicromapBuildInput) != 0) result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if ((stateBits & ResourceStates::OpacityMicromapWrite) != 0) result |= D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    if ((stateBits & ResourceStates::ConvertCoopVecMatrixInput) != 0) result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if ((stateBits & ResourceStates::ConvertCoopVecMatrixOutput) != 0) result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    return result;
}

EnhancedResourceStateMapping convertResourceStatesForEnhancedBarriers(ResourceStates state, bool isTexture)
{
    EnhancedResourceStateMapping result{};
    if (state != ResourceStates::Unknown)
        result.access = D3D12_BARRIER_ACCESS_COMMON;
    uint32_t remaining = uint32_t(state);

    for (uint32_t bitIndex = 0; remaining != 0 && bitIndex < 26; ++bitIndex)
    {
        const uint32_t bit = 1u << bitIndex;
        if ((remaining & bit) == 0)
            continue;

        EnhancedResourceStateMapping mapping{};
        mapping.access = D3D12_BARRIER_ACCESS_COMMON;
        mapping.nvrhiState = ResourceStates(bit);
        switch (ResourceStates(bit))
        {
        case ResourceStates::Common:
            mapping.sync = D3D12_BARRIER_SYNC_ALL; mapping.access = D3D12_BARRIER_ACCESS_COMMON; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::ConstantBuffer:
            mapping.sync = D3D12_BARRIER_SYNC_ALL_SHADING; mapping.access = D3D12_BARRIER_ACCESS_CONSTANT_BUFFER; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::VertexBuffer:
            mapping.sync = D3D12_BARRIER_SYNC_ALL_SHADING; mapping.access = D3D12_BARRIER_ACCESS_VERTEX_BUFFER; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::IndexBuffer:
            mapping.sync = D3D12_BARRIER_SYNC_INDEX_INPUT; mapping.access = D3D12_BARRIER_ACCESS_INDEX_BUFFER; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::IndirectArgument:
            mapping.sync = D3D12_BARRIER_SYNC_EXECUTE_INDIRECT; mapping.access = D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::PixelShaderResource:
            mapping.sync = D3D12_BARRIER_SYNC_PIXEL_SHADING; mapping.access = D3D12_BARRIER_ACCESS_SHADER_RESOURCE; mapping.layout = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE; break;
        case ResourceStates::NonPixelShaderResource:
            mapping.sync = D3D12_BARRIER_SYNC_NON_PIXEL_SHADING; mapping.access = D3D12_BARRIER_ACCESS_SHADER_RESOURCE; mapping.layout = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE; break;
        case ResourceStates::UnorderedAccess:
            mapping.sync = D3D12_BARRIER_SYNC_ALL_SHADING; mapping.access = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS; mapping.layout = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS; break;
        case ResourceStates::RenderTarget:
            mapping.sync = D3D12_BARRIER_SYNC_RENDER_TARGET; mapping.access = D3D12_BARRIER_ACCESS_RENDER_TARGET; mapping.layout = D3D12_BARRIER_LAYOUT_RENDER_TARGET; break;
        case ResourceStates::DepthWrite:
            mapping.sync = D3D12_BARRIER_SYNC_DEPTH_STENCIL; mapping.access = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE; mapping.layout = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE; break;
        case ResourceStates::DepthRead:
            mapping.sync = D3D12_BARRIER_SYNC_DEPTH_STENCIL; mapping.access = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ; mapping.layout = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ; break;
        case ResourceStates::StreamOut:
            mapping.sync = D3D12_BARRIER_SYNC_VERTEX_SHADING; mapping.access = D3D12_BARRIER_ACCESS_STREAM_OUTPUT; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::CopyDest:
            mapping.sync = D3D12_BARRIER_SYNC_COPY; mapping.access = D3D12_BARRIER_ACCESS_COPY_DEST; mapping.layout = D3D12_BARRIER_LAYOUT_COPY_DEST; break;
        case ResourceStates::CopySource:
            mapping.sync = D3D12_BARRIER_SYNC_COPY; mapping.access = D3D12_BARRIER_ACCESS_COPY_SOURCE; mapping.layout = D3D12_BARRIER_LAYOUT_COPY_SOURCE; break;
        case ResourceStates::ResolveDest:
            mapping.sync = D3D12_BARRIER_SYNC_RESOLVE; mapping.access = D3D12_BARRIER_ACCESS_RESOLVE_DEST; mapping.layout = D3D12_BARRIER_LAYOUT_RESOLVE_DEST; break;
        case ResourceStates::ResolveSource:
            mapping.sync = D3D12_BARRIER_SYNC_RESOLVE; mapping.access = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE; mapping.layout = D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE; break;
        case ResourceStates::Present:
            mapping.sync = D3D12_BARRIER_SYNC_ALL; mapping.access = D3D12_BARRIER_ACCESS_COPY_SOURCE; mapping.layout = D3D12_BARRIER_LAYOUT_PRESENT; break;
        case ResourceStates::AccelStructRead:
            mapping.sync = D3D12_BARRIER_SYNC_ALL_SHADING; mapping.access = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::AccelStructWrite:
            mapping.sync = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE; mapping.access = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::AccelStructBuildInput:
            mapping.sync = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE; mapping.access = D3D12_BARRIER_ACCESS_SHADER_RESOURCE; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::AccelStructBuildBlas:
            mapping.sync = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE; mapping.access = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::ShadingRateSurface:
            mapping.sync = D3D12_BARRIER_SYNC_ALL_SHADING; mapping.access = D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE; mapping.layout = D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE; break;
        case ResourceStates::OpacityMicromapWrite:
            mapping.sync = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE; mapping.access = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE; mapping.layout = D3D12_BARRIER_LAYOUT_COMMON; break;
        case ResourceStates::OpacityMicromapBuildInput:
            mapping.sync = D3D12_BARRIER_SYNC_ALL_SHADING; mapping.access = D3D12_BARRIER_ACCESS_SHADER_RESOURCE; mapping.layout = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE; break;
        case ResourceStates::ConvertCoopVecMatrixInput:
            mapping.sync = D3D12_BARRIER_SYNC_CONVERT_LINEAR_ALGEBRA_MATRIX; mapping.access = D3D12_BARRIER_ACCESS_SHADER_RESOURCE; mapping.layout = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE; break;
        case ResourceStates::ConvertCoopVecMatrixOutput:
            mapping.sync = D3D12_BARRIER_SYNC_CONVERT_LINEAR_ALGEBRA_MATRIX; mapping.access = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS; mapping.layout = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS; break;
        default:
            assert(false && "Unsupported enhanced-barrier resource state"); break;
        }

        result.nvrhiState = result.nvrhiState | mapping.nvrhiState;
        result.sync |= mapping.sync;
        result.access |= mapping.access;
        if (isTexture)
        {
            if (result.layout == D3D12_BARRIER_LAYOUT_COMMON)
                result.layout = mapping.layout;
            else
                assert(mapping.layout == D3D12_BARRIER_LAYOUT_COMMON || result.layout == mapping.layout);
        }
        remaining &= ~bit;
    }

    assert(result.nvrhiState == state);
    return result;
}
} // namespace unityrhi
