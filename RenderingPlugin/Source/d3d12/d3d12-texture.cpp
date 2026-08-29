// Texture creation and views ported from NVRHI's d3d12-texture.cpp
// (External/nvrhi/src/d3d12). Copyright (c) 2014-2021, NVIDIA CORPORATION.
// MIT license (see ThirdParty/NVRHI-LICENSE.txt).

#include "d3d12-backend.h"

#include <cassert>

#include <algorithm>
#include <cstring>

#include "common/dxgi-format.h"
#include "common/format-info.h"
#include "UnityRhiLog.h"

namespace unityrhi
{
uint64_t Texture::getNativeView(RhiObjectType objectType, Format format,
    TextureSubresourceSet subresources, TextureDimension dimension, bool isReadOnlyDSV)
{
    if (!resource)
        return 0;
    subresources = subresources.resolve(desc,
        objectType == RhiObjectType::D3D12_RenderTargetViewDescriptor ||
        objectType == RhiObjectType::D3D12_DepthStencilViewDescriptor ||
        objectType == RhiObjectType::D3D12_UnorderedAccessViewGpuDescriptor);
    if (dimension == TextureDimension::Unknown)
        dimension = desc.dimension;

    std::string key;
    auto append = [&key](const auto& value) {
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    append(subresources.baseMipLevel);
    append(subresources.numMipLevels);
    append(subresources.baseArraySlice);
    append(subresources.numArraySlices);
    append(format);
    append(dimension);
    append(isReadOnlyDSV);

    std::unordered_map<std::string, DescriptorIndex>* cache = nullptr;
    StaticDescriptorHeap* descriptorHeap = nullptr;
    bool gpuDescriptor = false;
    switch (objectType)
    {
    case RhiObjectType::D3D12_ShaderResourceViewGpuDescriptor:
        cache = &m_NativeSRVs;
        descriptorHeap = &m_Resources.shaderResourceViewHeap;
        gpuDescriptor = true;
        break;
    case RhiObjectType::D3D12_UnorderedAccessViewGpuDescriptor:
        cache = &m_NativeUAVs;
        descriptorHeap = &m_Resources.shaderResourceViewHeap;
        gpuDescriptor = true;
        break;
    case RhiObjectType::D3D12_RenderTargetViewDescriptor:
        cache = &m_NativeRTVs;
        descriptorHeap = &m_Resources.renderTargetViewHeap;
        break;
    case RhiObjectType::D3D12_DepthStencilViewDescriptor:
        cache = &m_NativeDSVs;
        descriptorHeap = &m_Resources.depthStencilViewHeap;
        break;
    default:
        return 0;
    }

    DescriptorIndex descriptor = c_InvalidDescriptorIndex;
    if (const auto found = cache->find(key); found != cache->end())
        descriptor = found->second;
    else
    {
        descriptor = descriptorHeap->allocateDescriptor();
        if (descriptor == c_InvalidDescriptorIndex)
            return 0;
        const size_t cpuHandle = descriptorHeap->getCpuHandle(descriptor).ptr;
        switch (objectType)
        {
        case RhiObjectType::D3D12_ShaderResourceViewGpuDescriptor:
            createSRV(cpuHandle, format, dimension, subresources);
            descriptorHeap->copyToShaderVisibleHeap(descriptor);
            break;
        case RhiObjectType::D3D12_UnorderedAccessViewGpuDescriptor:
            createUAV(cpuHandle, format, dimension, subresources);
            descriptorHeap->copyToShaderVisibleHeap(descriptor);
            break;
        case RhiObjectType::D3D12_RenderTargetViewDescriptor:
            createRTV(cpuHandle, format, subresources);
            break;
        case RhiObjectType::D3D12_DepthStencilViewDescriptor:
            createDSV(cpuHandle, subresources, isReadOnlyDSV);
            break;
        default:
            break;
        }
        cache->emplace(std::move(key), descriptor);
    }
    return gpuDescriptor
        ? descriptorHeap->getGpuHandle(descriptor).ptr
        : descriptorHeap->getCpuHandle(descriptor).ptr;
}

Texture::~Texture()
{
    if (sharedHandle)
    {
        CloseHandle(sharedHandle);
        sharedHandle = nullptr;
    }
    if (m_ClearRTV != c_InvalidDescriptorIndex)
    {
        m_Resources.renderTargetViewHeap.releaseDescriptor(m_ClearRTV);
        m_ClearRTV = c_InvalidDescriptorIndex;
    }
    if (m_ClearDSV != c_InvalidDescriptorIndex)
    {
        m_Resources.depthStencilViewHeap.releaseDescriptor(m_ClearDSV);
        m_ClearDSV = c_InvalidDescriptorIndex;
    }
    for (const auto& entry : m_ClearRTVs)
        m_Resources.renderTargetViewHeap.releaseDescriptor(entry.second);
    for (const auto& entry : m_ClearDSVs)
        m_Resources.depthStencilViewHeap.releaseDescriptor(entry.second);
    for (const auto& entry : m_ClearUAVs)
        m_Resources.shaderResourceViewHeap.releaseDescriptor(entry.second);
    for (const auto& entry : m_NativeSRVs)
        m_Resources.shaderResourceViewHeap.releaseDescriptor(entry.second);
    for (const auto& entry : m_NativeUAVs)
        m_Resources.shaderResourceViewHeap.releaseDescriptor(entry.second);
    for (const auto& entry : m_NativeRTVs)
        m_Resources.renderTargetViewHeap.releaseDescriptor(entry.second);
    for (const auto& entry : m_NativeDSVs)
        m_Resources.depthStencilViewHeap.releaseDescriptor(entry.second);
}

StagingTexture::SliceRegion StagingTexture::getSliceRegion(
    ID3D12Device* device, const RhiTextureSlice& slice) const
{
    SliceRegion result{};
    if (!device || slice.mipLevel >= desc.mipLevels || slice.arraySlice >= desc.arraySize)
        return result;
    const uint32_t subresource = calcSubresource(
        slice.mipLevel, slice.arraySlice, 0, desc.mipLevels, desc.arraySize);
    if (subresource >= subresourceOffsets.size())
        return result;
    device->GetCopyableFootprints(&resourceDesc, subresource, 1,
        subresourceOffsets[subresource], &result.footprint, nullptr, nullptr, &result.size);
    result.offset = result.footprint.Offset;
    return result;
}

uint64_t StagingTexture::getSizeInBytes(ID3D12Device* device) const
{
    if (!device || subresourceOffsets.empty())
        return 0;
    const uint32_t lastSubresource = calcSubresource(
        desc.mipLevels - 1, desc.arraySize - 1, 0, desc.mipLevels, desc.arraySize);
    uint64_t lastSubresourceSize = 0;
    device->GetCopyableFootprints(
        &resourceDesc, lastSubresource, 1, 0, nullptr, nullptr, nullptr, &lastSubresourceSize);
    return subresourceOffsets[lastSubresource] + lastSubresourceSize;
}

void StagingTexture::computeSubresourceOffsets(ID3D12Device* device)
{
    const uint32_t subresourceCount = desc.mipLevels * desc.arraySize;
    subresourceOffsets.resize(subresourceCount);
    uint64_t baseOffset = 0;
    for (uint32_t subresource = 0; subresource < subresourceCount; ++subresource)
    {
        uint64_t subresourceSize = 0;
        device->GetCopyableFootprints(
            &resourceDesc, subresource, 1, 0, nullptr, nullptr, nullptr, &subresourceSize);
        subresourceOffsets[subresource] = baseOffset;
        baseOffset = align(baseOffset + subresourceSize, uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
    }
}

static D3D12_RESOURCE_DESC convertTextureDesc(const TextureDesc& d)
{
    const auto& formatMapping = getDxgiFormatMapping(d.format);
    const FormatInfo& formatInfo = getFormatInfo(d.format);

    D3D12_RESOURCE_DESC desc = {};
    desc.Width = d.width;
    desc.Height = d.height;
    desc.MipLevels = UINT16(d.mipLevels);
    desc.Format = d.isTypeless ? formatMapping.resourceFormat : formatMapping.rtvFormat;
    desc.SampleDesc.Count = d.sampleCount;
    desc.SampleDesc.Quality = d.sampleQuality;

    switch (d.dimension)
    {
    case TextureDimension::Texture1D:
    case TextureDimension::Texture1DArray:
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        desc.DepthOrArraySize = UINT16(d.arraySize);
        break;
    case TextureDimension::Texture2D:
    case TextureDimension::Texture2DArray:
    case TextureDimension::TextureCube:
    case TextureDimension::TextureCubeArray:
    case TextureDimension::Texture2DMS:
    case TextureDimension::Texture2DMSArray:
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.DepthOrArraySize = UINT16(d.arraySize);
        break;
    case TextureDimension::Texture3D:
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        desc.DepthOrArraySize = UINT16(d.depth);
        break;
    case TextureDimension::Unknown:
    default:
        LogError("[UnityRHI] convertTextureDesc: invalid dimension %u.", uint32_t(d.dimension));
        break;
    }

    if (!d.isShaderResource)
        desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    if (d.isRenderTarget)
    {
        if (formatInfo.hasDepth || formatInfo.hasStencil)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        else
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }

    if (d.isUAV)
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    return desc;
}

static D3D12_CLEAR_VALUE convertTextureClearValue(const TextureDesc& d)
{
    const auto& formatMapping = getDxgiFormatMapping(d.format);
    const FormatInfo& formatInfo = getFormatInfo(d.format);
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = formatMapping.rtvFormat;
    if (formatInfo.hasDepth || formatInfo.hasStencil)
    {
        clearValue.DepthStencil.Depth = d.clearValue.r;
        clearValue.DepthStencil.Stencil = UINT8(d.clearValue.g);
    }
    else
    {
        clearValue.Color[0] = d.clearValue.r;
        clearValue.Color[1] = d.clearValue.g;
        clearValue.Color[2] = d.clearValue.b;
        clearValue.Color[3] = d.clearValue.a;
    }

    return clearValue;
}

Texture* Device::createTexture(const TextureDesc& d)
{
    D3D12_RESOURCE_DESC rd = convertTextureDesc(d);
    D3D12_HEAP_PROPERTIES heapProps = {};
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;

    const bool isShared = d.sharedResourceFlags != SharedResourceFlags::None;
    if ((d.sharedResourceFlags & SharedResourceFlags::Shared) != 0)
    {
        heapFlags |= D3D12_HEAP_FLAG_SHARED;
    }
    if ((d.sharedResourceFlags & SharedResourceFlags::Shared_CrossAdapter) != 0)
    {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        heapFlags |= D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    }
    if (d.isTiled)
    {
        rd.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    }

    Texture* texture = new Texture(m_Context, m_Resources, d);
    texture->debugName = d.debugName;
    texture->resourceDesc = rd;

    D3D12_CLEAR_VALUE clearValue = convertTextureClearValue(d);
    HRESULT hr = S_OK;

    if (d.isVirtual)
    {
        // The resource is created later by bindTextureMemory.
        RegisterResource(texture);
        return texture;
    }

    const D3D12_RESOURCE_STATES initialState = convertResourceStates(d.initialState);
    const D3D12_BARRIER_LAYOUT initialLayout =
        convertResourceStatesForEnhancedBarriers(d.initialState, true).layout;
    const D3D12_RESOURCE_DESC1 rd1 = convertResourceDesc1(rd);

    if (d.isTiled)
    {
        if (m_EnhancedBarriersSupported)
            hr = m_Context.device10->CreateReservedResource2(
                &rd, initialLayout, d.useClearValue ? &clearValue : nullptr,
                nullptr, 0, nullptr, IID_PPV_ARGS(&texture->resource));
        else
            hr = m_Context.device->CreateReservedResource(
                &rd, initialState, d.useClearValue ? &clearValue : nullptr,
                IID_PPV_ARGS(&texture->resource));
    }
    else
    {
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (m_EnhancedBarriersSupported)
            hr = m_Context.device10->CreateCommittedResource3(
                &heapProps, heapFlags, &rd1, initialLayout,
                d.useClearValue ? &clearValue : nullptr,
                nullptr, 0, nullptr, IID_PPV_ARGS(&texture->resource));
        else
            hr = m_Context.device->CreateCommittedResource(
                &heapProps, heapFlags, &rd, initialState,
                d.useClearValue ? &clearValue : nullptr,
                IID_PPV_ARGS(&texture->resource));
    }

    if (FAILED(hr))
    {
        LogError("[UnityRHI] Failed to create texture '%s' (hr=0x%08X, %ux%u fmt=%u).",
            texture->debugName.c_str(), static_cast<unsigned>(hr),
            d.width, d.height, uint32_t(d.format));
        HandleDeviceError(hr, "Create texture resource");
        delete texture;
        return nullptr;
    }

    if (isShared)
    {
        hr = m_Context.device->CreateSharedHandle(
            texture->resource.Get(), nullptr, GENERIC_ALL, nullptr, &texture->sharedHandle);
        if (FAILED(hr))
        {
            LogError("[UnityRHI] CreateSharedHandle failed for texture '%s' (hr=0x%08X).",
                texture->debugName.c_str(), unsigned(hr));
            HandleDeviceError(hr, "Create texture shared handle");
            delete texture;
            return nullptr;
        }
    }

    texture->postCreate();

    RegisterResource(texture);
    return texture;
}

MemoryRequirements Device::getTextureMemoryRequirements(Texture* texture) const
{
    MemoryRequirements memReq{};

    const D3D12_RESOURCE_ALLOCATION_INFO allocInfo = m_Context.device->GetResourceAllocationInfo(
        1, 1, &texture->resourceDesc);

    memReq.alignment = allocInfo.Alignment;
    memReq.size = allocInfo.SizeInBytes;

    return memReq;
}

bool Device::bindTextureMemory(Texture* texture, Heap* heap, uint64_t offset)
{
    if (texture->resource)
        return false; // already bound

    if (!texture->desc.isVirtual)
        return false; // not supported

    // Port of nvrhi's validation layer (DeviceWrapper::bindTextureMemory);
    // UnityRHI has no separate validation wrapper, so the range/alignment
    // checks live here.
    const MemoryRequirements memReq = getTextureMemoryRequirements(texture);
    if (offset + memReq.size > heap->desc.capacity ||
        (memReq.alignment != 0 && (offset % memReq.alignment) != 0))
    {
        LogError("[UnityRHI] bindTextureMemory '%s': invalid heap range/alignment.",
            texture->debugName.c_str());
        return false;
    }

    const D3D12_CLEAR_VALUE clearValue = convertTextureClearValue(texture->desc);
    const D3D12_RESOURCE_STATES initialState = convertResourceStates(texture->desc.initialState);

    HRESULT hr;
    if (m_EnhancedBarriersSupported)
    {
        const D3D12_RESOURCE_DESC1 desc1 = convertResourceDesc1(texture->resourceDesc);
        const D3D12_BARRIER_LAYOUT initialLayout =
            convertResourceStatesForEnhancedBarriers(texture->desc.initialState, true).layout;
        hr = m_Context.device10->CreatePlacedResource2(
            heap->heap.Get(), offset, &desc1, initialLayout,
            texture->desc.useClearValue ? &clearValue : nullptr,
            0, nullptr, IID_PPV_ARGS(&texture->resource));
    }
    else
    {
        hr = m_Context.device->CreatePlacedResource(
            heap->heap.Get(), offset, &texture->resourceDesc, initialState,
            texture->desc.useClearValue ? &clearValue : nullptr,
            IID_PPV_ARGS(&texture->resource));
    }

    if (FAILED(hr))
    {
        LogError("[UnityRHI] Failed to create placed texture '%s' (hr=0x%08X).",
            texture->debugName.c_str(), unsigned(hr));
        return false;
    }

    texture->heap = heap;
    texture->postCreate();

    return true;
}

Texture* Device::createTextureFromNativeResource(ID3D12Resource* resource, const TextureDesc& desc)
{
    if (!resource)
        return nullptr;

    Texture* texture = new Texture(m_Context, m_Resources, desc);
    texture->debugName = desc.debugName;
    texture->resource = resource;
    texture->resourceDesc = resource->GetDesc();
    texture->unityOwnedResource = true; // states handled by Unity's tracker, not ours

    texture->postCreate();
    // Unity-created resources may use formats whose plane-count query fails;
    // fall back to one plane so subresource math stays sane.
    if (texture->planeCount == 0)
        texture->planeCount = 1;

    RegisterResource(texture);
    return texture;
}

// Port of nvrhi Texture::postCreate (minus Aftermath and the per-mip clear
// UAV cache, which is not ported).
void Texture::postCreate()
{
    if (!desc.debugName.empty())
    {
        wchar_t wide[256];
        int written = MultiByteToWideChar(CP_UTF8, 0, desc.debugName.c_str(), -1, wide, 255);
        wide[written > 0 ? written : 0] = L'\0';
        resource->SetName(wide);
    }

    planeCount = m_Resources.getFormatPlaneCount(resourceDesc.Format);
}

DescriptorIndex Texture::getClearRTV()
{
    if (m_ClearRTV != c_InvalidDescriptorIndex)
        return m_ClearRTV;

    if (!desc.isRenderTarget)
        return c_InvalidDescriptorIndex;

    m_ClearRTV = m_Resources.renderTargetViewHeap.allocateDescriptor();
    if (m_ClearRTV == c_InvalidDescriptorIndex)
        return c_InvalidDescriptorIndex;

    TextureSubresourceSet all{};
    all.numMipLevels = 1;
    all.numArraySlices = desc.arraySize;
    createRTV(m_Resources.renderTargetViewHeap.getCpuHandle(m_ClearRTV).ptr, Format::UNKNOWN, all);
    return m_ClearRTV;
}

DescriptorIndex Texture::getClearDSV()
{
    if (m_ClearDSV != c_InvalidDescriptorIndex)
        return m_ClearDSV;

    if (!desc.isRenderTarget)
        return c_InvalidDescriptorIndex;

    m_ClearDSV = m_Resources.depthStencilViewHeap.allocateDescriptor();
    if (m_ClearDSV == c_InvalidDescriptorIndex)
        return c_InvalidDescriptorIndex;

    TextureSubresourceSet all{};
    all.numMipLevels = 1;
    all.numArraySlices = desc.arraySize;
    createDSV(m_Resources.depthStencilViewHeap.getCpuHandle(m_ClearDSV).ptr, all, false);
    return m_ClearDSV;
}

uint64_t makeClearViewKey(MipLevel mipLevel, ArraySlice baseArraySlice, ArraySlice arraySize, Format format)
{
    return uint64_t(mipLevel) | (uint64_t(baseArraySlice) << 16) |
        (uint64_t(arraySize) << 32) | (uint64_t(format) << 48);
}

DescriptorIndex Texture::getClearRTV(TextureSubresourceSet subresources)
{
    if (subresources.baseMipLevel == 0 && subresources.baseArraySlice == 0 &&
        subresources.numArraySlices == desc.arraySize)
        return getClearRTV();
    const uint64_t key = makeClearViewKey(subresources.baseMipLevel,
        subresources.baseArraySlice, subresources.numArraySlices, Format::UNKNOWN);
    if (const auto found = m_ClearRTVs.find(key); found != m_ClearRTVs.end())
        return found->second;
    const DescriptorIndex descriptor = m_Resources.renderTargetViewHeap.allocateDescriptor();
    if (descriptor == c_InvalidDescriptorIndex)
        return descriptor;
    subresources.numMipLevels = 1;
    createRTV(m_Resources.renderTargetViewHeap.getCpuHandle(descriptor).ptr,
        Format::UNKNOWN, subresources);
    m_ClearRTVs.emplace(key, descriptor);
    return descriptor;
}

DescriptorIndex Texture::getClearDSV(TextureSubresourceSet subresources)
{
    if (subresources.baseMipLevel == 0 && subresources.baseArraySlice == 0 &&
        subresources.numArraySlices == desc.arraySize)
        return getClearDSV();
    const uint64_t key = makeClearViewKey(subresources.baseMipLevel,
        subresources.baseArraySlice, subresources.numArraySlices, Format::UNKNOWN);
    if (const auto found = m_ClearDSVs.find(key); found != m_ClearDSVs.end())
        return found->second;
    const DescriptorIndex descriptor = m_Resources.depthStencilViewHeap.allocateDescriptor();
    if (descriptor == c_InvalidDescriptorIndex)
        return descriptor;
    subresources.numMipLevels = 1;
    createDSV(m_Resources.depthStencilViewHeap.getCpuHandle(descriptor).ptr, subresources, false);
    m_ClearDSVs.emplace(key, descriptor);
    return descriptor;
}

DescriptorIndex Texture::getClearUAV(MipLevel mipLevel, ArraySlice baseArraySlice,
    ArraySlice arraySize, Format format)
{
    const uint64_t key = makeClearViewKey(mipLevel, baseArraySlice, arraySize, format);
    if (const auto found = m_ClearUAVs.find(key); found != m_ClearUAVs.end())
        return found->second;
    const DescriptorIndex descriptor = m_Resources.shaderResourceViewHeap.allocateDescriptor();
    if (descriptor == c_InvalidDescriptorIndex)
        return descriptor;
    const TextureSubresourceSet subresources{mipLevel, 1, baseArraySlice, arraySize};
    createUAV(m_Resources.shaderResourceViewHeap.getCpuHandle(descriptor).ptr, format,
        TextureDimension::Unknown, subresources);
    m_Resources.shaderResourceViewHeap.copyToShaderVisibleHeap(descriptor);
    m_ClearUAVs.emplace(key, descriptor);
    return descriptor;
}

StagingTexture* Device::createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess)
{
    assert(cpuAccess != CpuAccessMode::None);

    auto* ret = new StagingTexture();
    ret->debugName = d.debugName;
    ret->desc = d;
    ret->resourceDesc = convertTextureDesc(d);
    ret->computeSubresourceOffsets(m_Context.device.Get());

    BufferDesc bufferDesc;
    bufferDesc.byteSize = ret->getSizeInBytes(m_Context.device.Get());
    bufferDesc.structStride = 0;
    bufferDesc.debugName = d.debugName;
    bufferDesc.cpuAccess = cpuAccess;

    ret->buffer = createBuffer(bufferDesc);
    if (!ret->buffer)
    {
        delete ret;
        return nullptr;
    }

    ret->cpuAccess = cpuAccess;
    RegisterResource(ret);
    return ret;
}

// Port of nvrhi DeviceResources::getFormatPlaneCount.
uint8_t DeviceResources::getFormatPlaneCount(DXGI_FORMAT format)
{
    uint8_t& planeCount = m_DxgiFormatPlaneCounts[format];
    if (planeCount == 0)
    {
        D3D12_FEATURE_DATA_FORMAT_INFO formatInfo = { format, 1 };
        if (FAILED(m_Context.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo))))
        {
            // Format not supported - store a special value in the cache to avoid querying later
            planeCount = 255;
        }
        else
        {
            // Format supported - store the plane count in the cache
            planeCount = formatInfo.PlaneCount;
        }
    }

    if (planeCount == 255)
        return 0;

    return planeCount;
}

void Texture::createSRV(size_t descriptor, Format format, TextureDimension dimension, TextureSubresourceSet subresources) const
{
    subresources = subresources.resolve(desc, false);

    if (dimension == TextureDimension::Unknown)
        dimension = desc.dimension;

    D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};

    viewDesc.Format = getDxgiFormatMapping(format == Format::UNKNOWN ? desc.format : format).srvFormat;
    viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    uint32_t planeSlice = (viewDesc.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT) ? 1 : 0;

    switch (dimension)
    {
    case TextureDimension::Texture1D:
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        viewDesc.Texture1D.MostDetailedMip = subresources.baseMipLevel;
        viewDesc.Texture1D.MipLevels = subresources.numMipLevels;
        break;
    case TextureDimension::Texture1DArray:
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        viewDesc.Texture1DArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture1DArray.ArraySize = subresources.numArraySlices;
        viewDesc.Texture1DArray.MostDetailedMip = subresources.baseMipLevel;
        viewDesc.Texture1DArray.MipLevels = subresources.numMipLevels;
        break;
    case TextureDimension::Texture2D:
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MostDetailedMip = subresources.baseMipLevel;
        viewDesc.Texture2D.MipLevels = subresources.numMipLevels;
        viewDesc.Texture2D.PlaneSlice = planeSlice;
        break;
    case TextureDimension::Texture2DArray:
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture2DArray.ArraySize = subresources.numArraySlices;
        viewDesc.Texture2DArray.MostDetailedMip = subresources.baseMipLevel;
        viewDesc.Texture2DArray.MipLevels = subresources.numMipLevels;
        viewDesc.Texture2DArray.PlaneSlice = planeSlice;
        break;
    case TextureDimension::TextureCube:
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        viewDesc.TextureCube.MostDetailedMip = subresources.baseMipLevel;
        viewDesc.TextureCube.MipLevels = subresources.numMipLevels;
        break;
    case TextureDimension::TextureCubeArray:
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        viewDesc.TextureCubeArray.First2DArrayFace = subresources.baseArraySlice;
        viewDesc.TextureCubeArray.NumCubes = subresources.numArraySlices / 6;
        viewDesc.TextureCubeArray.MostDetailedMip = subresources.baseMipLevel;
        viewDesc.TextureCubeArray.MipLevels = subresources.numMipLevels;
        break;
    case TextureDimension::Texture2DMS:
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        break;
    case TextureDimension::Texture2DMSArray:
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
        viewDesc.Texture2DMSArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture2DMSArray.ArraySize = subresources.numArraySlices;
        break;
    case TextureDimension::Texture3D:
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        viewDesc.Texture3D.MostDetailedMip = subresources.baseMipLevel;
        viewDesc.Texture3D.MipLevels = subresources.numMipLevels;
        break;
    case TextureDimension::Unknown:
    default:
        LogError("[UnityRHI] Texture::createSRV: invalid dimension %u.", uint32_t(dimension));
        return;
    }

    m_Context.device->CreateShaderResourceView(resource.Get(), &viewDesc, {descriptor});
}

void Texture::createUAV(size_t descriptor, Format format, TextureDimension dimension, TextureSubresourceSet subresources) const
{
    subresources = subresources.resolve(desc, true);

    if (dimension == TextureDimension::Unknown)
        dimension = desc.dimension;

    D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};

    viewDesc.Format = getDxgiFormatMapping(format == Format::UNKNOWN ? desc.format : format).srvFormat;

    // D3D12 forbids sRGB UAVs; demote to the UNORM sibling like NRI does for
    // storage views. Shaders read/write raw encoded bytes through the UAV.
    switch (viewDesc.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: viewDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: viewDesc.Format = DXGI_FORMAT_B8G8R8X8_UNORM; break;
    default: break;
    }

    switch (dimension)
    {
    case TextureDimension::Texture1D:
        viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
        viewDesc.Texture1D.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture1DArray:
        viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        viewDesc.Texture1DArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture1DArray.ArraySize = subresources.numArraySlices;
        viewDesc.Texture1DArray.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture2D:
        viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture2DArray:
    case TextureDimension::TextureCube:
    case TextureDimension::TextureCubeArray:
        viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture2DArray.ArraySize = subresources.numArraySlices;
        viewDesc.Texture2DArray.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture3D:
        viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        viewDesc.Texture3D.FirstWSlice = 0;
        viewDesc.Texture3D.WSize = desc.depth;
        viewDesc.Texture3D.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture2DMS:
    case TextureDimension::Texture2DMSArray:
        LogError("[UnityRHI] Texture '%s' has unsupported dimension for UAV.", debugName.c_str());
        return;
    case TextureDimension::Unknown:
    default:
        LogError("[UnityRHI] Texture::createUAV: invalid dimension %u.", uint32_t(dimension));
        return;
    }

    m_Context.device->CreateUnorderedAccessView(resource.Get(), nullptr, &viewDesc, {descriptor});
}

void Texture::createRTV(size_t descriptor, Format format, TextureSubresourceSet subresources) const
{
    subresources = subresources.resolve(desc, true);

    D3D12_RENDER_TARGET_VIEW_DESC viewDesc = {};

    viewDesc.Format = getDxgiFormatMapping(format == Format::UNKNOWN ? desc.format : format).rtvFormat;

    switch (desc.dimension)
    {
    case TextureDimension::Texture1D:
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
        viewDesc.Texture1D.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture1DArray:
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
        viewDesc.Texture1DArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture1DArray.ArraySize = subresources.numArraySlices;
        viewDesc.Texture1DArray.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture2D:
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture2DArray:
    case TextureDimension::TextureCube:
    case TextureDimension::TextureCubeArray:
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.ArraySize = subresources.numArraySlices;
        viewDesc.Texture2DArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture2DArray.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture2DMS:
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        break;
    case TextureDimension::Texture2DMSArray:
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
        viewDesc.Texture2DMSArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture2DMSArray.ArraySize = subresources.numArraySlices;
        break;
    case TextureDimension::Texture3D:
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        viewDesc.Texture3D.FirstWSlice = subresources.baseArraySlice;
        viewDesc.Texture3D.WSize = subresources.numArraySlices;
        viewDesc.Texture3D.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Unknown:
    default:
        LogError("[UnityRHI] Texture::createRTV: invalid dimension %u.", uint32_t(desc.dimension));
        return;
    }

    m_Context.device->CreateRenderTargetView(resource.Get(), &viewDesc, {descriptor});
}

void Texture::createDSV(size_t descriptor, TextureSubresourceSet subresources, bool isReadOnly) const
{
    subresources = subresources.resolve(desc, true);

    D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc = {};

    viewDesc.Format = getDxgiFormatMapping(desc.format).rtvFormat;

    if (isReadOnly)
    {
        viewDesc.Flags |= D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        if (viewDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || viewDesc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
            viewDesc.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    }

    switch (desc.dimension)
    {
    case TextureDimension::Texture1D:
        viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
        viewDesc.Texture1D.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture1DArray:
        viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
        viewDesc.Texture1DArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture1DArray.ArraySize = subresources.numArraySlices;
        viewDesc.Texture1DArray.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture2D:
        viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture2DArray:
    case TextureDimension::TextureCube:
    case TextureDimension::TextureCubeArray:
        viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.ArraySize = subresources.numArraySlices;
        viewDesc.Texture2DArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture2DArray.MipSlice = subresources.baseMipLevel;
        break;
    case TextureDimension::Texture2DMS:
        viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        break;
    case TextureDimension::Texture2DMSArray:
        viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
        viewDesc.Texture2DMSArray.FirstArraySlice = subresources.baseArraySlice;
        viewDesc.Texture2DMSArray.ArraySize = subresources.numArraySlices;
        break;
    case TextureDimension::Texture3D:
    case TextureDimension::Unknown:
    default:
        LogError("[UnityRHI] Texture '%s': unsupported dimension for DSV.", debugName.c_str());
        return;
    }

    m_Context.device->CreateDepthStencilView(resource.Get(), &viewDesc, {descriptor});
}

void* Device::mapStagingTexture(StagingTexture* texture, const RhiTextureSlice& slice,
    CpuAccessMode cpuAccess, uint64_t* outRowPitch)
{
    if (outRowPitch)
        *outRowPitch = 0;
    if (!texture || !texture->buffer || !texture->buffer->resource || !outRowPitch ||
        cpuAccess == CpuAccessMode::None || cpuAccess != texture->cpuAccess ||
        texture->mappedAccess != CpuAccessMode::None ||
        slice.x != 0 || slice.y != 0 || slice.z != 0)
        return nullptr;

    const StagingTexture::SliceRegion region = texture->getSliceRegion(m_Context.device.Get(), slice);
    if (region.size == 0)
        return nullptr;

    // Match NVRHI's map behavior: wait until the staging texture's last-use
    // recording instance retires on the lifetime fence. Native tests have no
    // Unity interface and explicitly wait on their own queue before mapping.
    if (!waitForLifetimeInstance(texture->lastUseFenceValue))
        return nullptr;

    D3D12_RANGE readRange{};
    if (cpuAccess == CpuAccessMode::Read)
    {
        readRange.Begin = size_t(region.offset);
        readRange.End = size_t(region.offset + region.size);
    }
    uint8_t* mapped = nullptr;
    const HRESULT hr = texture->buffer->resource->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr))
        return nullptr;

    texture->mappedRegion = region;
    texture->mappedAccess = cpuAccess;
    *outRowPitch = region.footprint.Footprint.RowPitch;
    return mapped + region.offset;
}

void Device::unmapStagingTexture(StagingTexture* texture)
{
    if (!texture || !texture->buffer || !texture->buffer->resource ||
        texture->mappedAccess == CpuAccessMode::None || texture->mappedRegion.size == 0)
        return;
    D3D12_RANGE writtenRange{};
    if (texture->mappedAccess == CpuAccessMode::Write)
    {
        writtenRange.Begin = size_t(texture->mappedRegion.offset);
        writtenRange.End = size_t(texture->mappedRegion.offset + texture->mappedRegion.size);
    }
    texture->buffer->resource->Unmap(0, &writtenRange);
    texture->mappedRegion = {};
    texture->mappedAccess = CpuAccessMode::None;
}

namespace
{
}

bool ReplayContext::clearTextureFloat(Texture* texture, const float color[4])
{
    return clearTextureFloat(texture, AllSubresources, color);
}

// Port of nvrhi CommandList::clearTextureFloat, including per-subresource RTV
// and UAV paths.
bool ReplayContext::clearTextureFloat(Texture* texture, TextureSubresourceSet subresources, const float color[4])
{
    Device* device = Device::Get();
    if (!texture || !texture->resource || !device)
        return false;

    if (!texture->desc.isRenderTarget && !texture->desc.isUAV)
    {
        LogError("[UnityRHI] ClearTextureFloat: texture '%s' is neither a render target nor a UAV.",
            texture->debugName.c_str());
        return false;
    }

    subresources = subresources.resolve(texture->desc, false);

    const ResourceStates clearState = texture->desc.isRenderTarget
        ? ResourceStates::RenderTarget : ResourceStates::UnorderedAccess;
    if (enableAutomaticBarriers)
    {
        requireTextureState(texture, subresources, clearState);
        bindingStatesDirty = true;
    }
    commitBarriers();

    for (MipLevel mip = subresources.baseMipLevel;
         mip < subresources.baseMipLevel + subresources.numMipLevels; ++mip)
    {
        TextureSubresourceSet oneMip = subresources;
        oneMip.baseMipLevel = mip;
        oneMip.numMipLevels = 1;
        if (texture->desc.isRenderTarget)
        {
            const DescriptorIndex rtv = texture->getClearRTV(oneMip);
            if (rtv == c_InvalidDescriptorIndex)
                return false;
            commandList->ClearRenderTargetView(
                device->resources().renderTargetViewHeap.getCpuHandle(rtv), color, 0, nullptr);
        }
        else
        {
            commitDescriptorHeaps();
            const DescriptorIndex uav = texture->getClearUAV(
                mip, subresources.baseArraySlice, subresources.numArraySlices);
            if (uav == c_InvalidDescriptorIndex)
                return false;
            commandList->ClearUnorderedAccessViewFloat(
                device->resources().shaderResourceViewHeap.getGpuHandle(uav),
                device->resources().shaderResourceViewHeap.getCpuHandle(uav),
                texture->resource.Get(), color, 0, nullptr);
        }
    }
    texture->lastUseFenceValue = fenceValue;
    return true;
}

// Port of nvrhi CommandList::clearDepthStencilTexture.
bool ReplayContext::clearDepthStencilTexture(Texture* texture, bool clearDepth, float depth, bool clearStencil, uint8_t stencil)
{
    return clearDepthStencilTexture(texture, AllSubresources, clearDepth, depth, clearStencil, stencil);
}

bool ReplayContext::clearDepthStencilTexture(Texture* texture, TextureSubresourceSet subresources,
    bool clearDepth, float depth, bool clearStencil, uint8_t stencil)
{
    Device* device = Device::Get();
    if (!texture || !texture->resource || !device)
        return false;

    if (!clearDepth && !clearStencil)
        return true;

    if (!texture->desc.isRenderTarget)
    {
        LogError("[UnityRHI] ClearDepthStencilTexture: texture '%s' is not a render target.",
            texture->debugName.c_str());
        return false;
    }

    subresources = subresources.resolve(texture->desc, false);

    if (enableAutomaticBarriers)
    {
        requireTextureState(texture, subresources, ResourceStates::DepthWrite);
        bindingStatesDirty = true;
    }
    commitBarriers();

    D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAGS(0);
    if (clearDepth)
        flags |= D3D12_CLEAR_FLAG_DEPTH;
    if (clearStencil)
        flags |= D3D12_CLEAR_FLAG_STENCIL;

    for (MipLevel mip = subresources.baseMipLevel;
         mip < subresources.baseMipLevel + subresources.numMipLevels; ++mip)
    {
        TextureSubresourceSet oneMip = subresources;
        oneMip.baseMipLevel = mip;
        oneMip.numMipLevels = 1;
        const DescriptorIndex dsv = texture->getClearDSV(oneMip);
        if (dsv == c_InvalidDescriptorIndex)
            return false;
        commandList->ClearDepthStencilView(
            device->resources().depthStencilViewHeap.getCpuHandle(dsv), flags, depth, stencil, 0, nullptr);
    }
    texture->lastUseFenceValue = fenceValue;
    return true;
}

bool ReplayContext::clearTextureUInt(Texture* texture, TextureSubresourceSet subresources, uint32_t clearColor)
{
    Device* device = Device::Get();
    if (!texture || !texture->resource || !device || (!texture->desc.isUAV && !texture->desc.isRenderTarget))
        return false;

    subresources = subresources.resolve(texture->desc, false);

    uint32_t values[4] = {clearColor, clearColor, clearColor, clearColor};
    if (texture->desc.isUAV)
    {
        if (enableAutomaticBarriers)
        {
            requireTextureState(texture, subresources, ResourceStates::UnorderedAccess);
            bindingStatesDirty = true;
        }
        commitBarriers();
        commitDescriptorHeaps();
        for (MipLevel mip = subresources.baseMipLevel;
             mip < subresources.baseMipLevel + subresources.numMipLevels; ++mip)
        {
            const DescriptorIndex uav = texture->getClearUAV(
                mip, subresources.baseArraySlice, subresources.numArraySlices);
            if (uav == c_InvalidDescriptorIndex)
                return false;
            commandList->ClearUnorderedAccessViewUint(
                device->resources().shaderResourceViewHeap.getGpuHandle(uav),
                device->resources().shaderResourceViewHeap.getCpuHandle(uav),
                texture->resource.Get(), values, 0, nullptr);
        }
    }
    else
    {
        const float value = float(clearColor);
        const float floatValues[4] = {value, value, value, value};
        return clearTextureFloat(texture, subresources, floatValues);
    }

    texture->lastUseFenceValue = fenceValue;
    return true;
}

// Port of nvrhi CommandList::copyTexture (texture-to-buffer overload).
bool ReplayContext::copyTexture(Buffer* dest, uint64_t destOffsetBytes, Texture* src, uint32_t arraySlice, uint32_t mipLevel)
{
    Device* device = Device::Get();
    if (!dest || !dest->resource || dest->desc.cpuAccess != CpuAccessMode::Read ||
        !src || !src->resource || !device)
        return false;
    if ((destOffsetBytes % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) != 0 ||
        mipLevel >= src->desc.mipLevels || arraySlice >= src->desc.arraySize)
        return false;

    const uint32_t subresource = calcSubresource(mipLevel, arraySlice, 0, src->desc.mipLevels, src->desc.arraySize);
    const D3D12_RESOURCE_DESC resourceDesc = src->resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    device->GetD3DDevice()->GetCopyableFootprints(
        &resourceDesc, subresource, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
    if (destOffsetBytes + totalBytes > dest->desc.byteSize)
        return false;
    footprint.Offset = destOffsetBytes;

    if (enableAutomaticBarriers)
    {
        requireTextureState(src, TextureSubresourceSet{mipLevel, 1, arraySlice, 1}, ResourceStates::CopySource);
        requireBufferState(dest, ResourceStates::CopyDest); // no-op: readback buffers are CPU-visible
        bindingStatesDirty = true;
    }
    commitBarriers();

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = dest->resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = src->resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = subresource;

    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    dest->lastUseFenceValue = src->lastUseFenceValue = fenceValue;
    return true;
}

bool ReplayContext::copyTexture(Texture* dest, const RhiTextureSlice& destSlice,
    Texture* src, const RhiTextureSlice& srcSlice)
{
    if (!dest || !dest->resource || !src || !src->resource)
        return false;
    if (destSlice.mipLevel >= dest->desc.mipLevels || destSlice.arraySlice >= dest->desc.arraySize ||
        srcSlice.mipLevel >= src->desc.mipLevels || srcSlice.arraySlice >= src->desc.arraySize ||
        srcSlice.width == 0 || srcSlice.height == 0 || srcSlice.depth == 0)
        return false;

    if (enableAutomaticBarriers)
    {
        requireTextureState(dest,
            TextureSubresourceSet{destSlice.mipLevel, 1, destSlice.arraySlice, 1}, ResourceStates::CopyDest);
        requireTextureState(src,
            TextureSubresourceSet{srcSlice.mipLevel, 1, srcSlice.arraySlice, 1}, ResourceStates::CopySource);
        bindingStatesDirty = true;
    }
    commitBarriers();

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = dest->resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = calcSubresource(
        destSlice.mipLevel, destSlice.arraySlice, 0, dest->desc.mipLevels, dest->desc.arraySize);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = src->resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = calcSubresource(
        srcSlice.mipLevel, srcSlice.arraySlice, 0, src->desc.mipLevels, src->desc.arraySize);

    D3D12_BOX sourceBox{};
    sourceBox.left = srcSlice.x;
    sourceBox.top = srcSlice.y;
    sourceBox.front = srcSlice.z;
    sourceBox.right = srcSlice.x + srcSlice.width;
    sourceBox.bottom = srcSlice.y + srcSlice.height;
    sourceBox.back = srcSlice.z + srcSlice.depth;

    commandList->CopyTextureRegion(&destination, destSlice.x, destSlice.y, destSlice.z, &source, &sourceBox);
    dest->lastUseFenceValue = src->lastUseFenceValue = fenceValue;
    return true;
}

bool ReplayContext::copyTexture(Texture* dest, const RhiTextureSlice& destSlice,
    StagingTexture* src, const RhiTextureSlice& srcSlice)
{
    Device* device = Device::Get();
    if (!dest || !dest->resource || !src || !src->buffer || !src->buffer->resource || !device ||
        src->cpuAccess != CpuAccessMode::Write ||
        destSlice.mipLevel >= dest->desc.mipLevels || destSlice.arraySlice >= dest->desc.arraySize ||
        srcSlice.mipLevel >= src->desc.mipLevels || srcSlice.arraySlice >= src->desc.arraySize ||
        srcSlice.width == 0 || srcSlice.height == 0 || srcSlice.depth == 0)
        return false;

    if (enableAutomaticBarriers)
    {
        requireTextureState(dest,
            TextureSubresourceSet{destSlice.mipLevel, 1, destSlice.arraySlice, 1}, ResourceStates::CopyDest);
        requireBufferState(src->buffer, ResourceStates::CopySource);
        bindingStatesDirty = true;
    }
    commitBarriers();

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = dest->resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = calcSubresource(
        destSlice.mipLevel, destSlice.arraySlice, 0, dest->desc.mipLevels, dest->desc.arraySize);

    const StagingTexture::SliceRegion sourceRegion =
        src->getSliceRegion(device->GetD3DDevice(), srcSlice);
    if (sourceRegion.size == 0)
        return false;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = src->buffer->resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = sourceRegion.footprint;

    D3D12_BOX sourceBox{};
    sourceBox.left = srcSlice.x;
    sourceBox.top = srcSlice.y;
    sourceBox.front = srcSlice.z;
    sourceBox.right = srcSlice.x + srcSlice.width;
    sourceBox.bottom = srcSlice.y + srcSlice.height;
    sourceBox.back = srcSlice.z + srcSlice.depth;
    commandList->CopyTextureRegion(
        &destination, destSlice.x, destSlice.y, destSlice.z, &source, &sourceBox);
    dest->lastUseFenceValue = src->lastUseFenceValue = src->buffer->lastUseFenceValue = fenceValue;
    return true;
}

bool ReplayContext::copyTexture(StagingTexture* dest, const RhiTextureSlice& destSlice,
    Texture* src, const RhiTextureSlice& srcSlice)
{
    Device* device = Device::Get();
    if (!dest || !dest->buffer || !dest->buffer->resource || !src || !src->resource || !device ||
        dest->cpuAccess != CpuAccessMode::Read ||
        destSlice.mipLevel >= dest->desc.mipLevels || destSlice.arraySlice >= dest->desc.arraySize ||
        srcSlice.mipLevel >= src->desc.mipLevels || srcSlice.arraySlice >= src->desc.arraySize ||
        srcSlice.width == 0 || srcSlice.height == 0 || srcSlice.depth == 0)
        return false;

    if (enableAutomaticBarriers)
    {
        requireBufferState(dest->buffer, ResourceStates::CopyDest);
        requireTextureState(src,
            TextureSubresourceSet{srcSlice.mipLevel, 1, srcSlice.arraySlice, 1}, ResourceStates::CopySource);
        bindingStatesDirty = true;
    }
    commitBarriers();

    const StagingTexture::SliceRegion destinationRegion =
        dest->getSliceRegion(device->GetD3DDevice(), destSlice);
    if (destinationRegion.size == 0)
        return false;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = dest->buffer->resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = destinationRegion.footprint;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = src->resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = calcSubresource(
        srcSlice.mipLevel, srcSlice.arraySlice, 0, src->desc.mipLevels, src->desc.arraySize);

    D3D12_BOX sourceBox{};
    sourceBox.left = srcSlice.x;
    sourceBox.top = srcSlice.y;
    sourceBox.front = srcSlice.z;
    sourceBox.right = srcSlice.x + srcSlice.width;
    sourceBox.bottom = srcSlice.y + srcSlice.height;
    sourceBox.back = srcSlice.z + srcSlice.depth;
    commandList->CopyTextureRegion(
        &destination, destSlice.x, destSlice.y, destSlice.z, &source, &sourceBox);
    dest->lastUseFenceValue = dest->buffer->lastUseFenceValue = src->lastUseFenceValue = fenceValue;
    return true;
}

// Port of nvrhi CommandList::writeTexture. Record time performs the footprint
// layout and memcpy; replay only records CopyTextureRegion.
bool ReplayContext::writeTexture(Texture* dest, uint32_t arraySlice, uint32_t mipLevel, UploadTicket* upload)
{
    Device* device = Device::Get();
    if (!dest || !dest->resource || !device)
        return false;

    if (mipLevel >= dest->desc.mipLevels || arraySlice >= dest->desc.arraySize)
    {
        LogError("[UnityRHI] WriteTexture: subresource (mip %u, slice %u) out of range for '%s'.",
            mipLevel, arraySlice, dest->debugName.c_str());
        return false;
    }

    if (enableAutomaticBarriers)
    {
        requireTextureState(dest, TextureSubresourceSet{mipLevel, 1, arraySlice, 1}, ResourceStates::CopyDest);
        bindingStatesDirty = true;
    }
    commitBarriers();

    const uint32_t subresource = calcSubresource(mipLevel, arraySlice, 0, dest->desc.mipLevels, dest->desc.arraySize);

    ID3D12Resource* uploadBuffer = nullptr;
    uint64_t offsetInUploadBuffer = 0;
    if (!device->uploadManager().resolveTicket(upload, fenceValue, &uploadBuffer, &offsetInUploadBuffer, nullptr))
    {
        LogError("[UnityRHI] WriteTexture received an invalid record-time upload ticket");
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION destCopyLocation{};
    destCopyLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destCopyLocation.SubresourceIndex = subresource;
    destCopyLocation.pResource = dest->resource.Get();

    D3D12_TEXTURE_COPY_LOCATION srcCopyLocation{};
    srcCopyLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcCopyLocation.PlacedFootprint = upload->footprint;
    srcCopyLocation.PlacedFootprint.Offset = offsetInUploadBuffer;
    srcCopyLocation.pResource = uploadBuffer;

    commandList->CopyTextureRegion(&destCopyLocation, 0, 0, 0, &srcCopyLocation, nullptr);
    dest->lastUseFenceValue = fenceValue;
    return true;
}

bool ReplayContext::resolveTexture(Texture* dest, TextureSubresourceSet destSubresources,
    Texture* src, TextureSubresourceSet srcSubresources)
{
    if (!dest || !dest->resource || !src || !src->resource ||
        destSubresources.numArraySlices != srcSubresources.numArraySlices ||
        destSubresources.numMipLevels != srcSubresources.numMipLevels)
        return false;

    if (enableAutomaticBarriers)
    {
        requireTextureState(dest, destSubresources, ResourceStates::ResolveDest);
        requireTextureState(src, srcSubresources, ResourceStates::ResolveSource);
        bindingStatesDirty = true;
    }
    commitBarriers();

    const DXGI_FORMAT format = getDxgiFormatMapping(dest->desc.format).rtvFormat;
    for (uint32_t plane = 0; plane < dest->planeCount; ++plane)
    {
        for (uint32_t arrayIndex = 0; arrayIndex < destSubresources.numArraySlices; ++arrayIndex)
        {
            for (uint32_t mipIndex = 0; mipIndex < destSubresources.numMipLevels; ++mipIndex)
            {
                const uint32_t destSubresource = calcSubresource(
                    destSubresources.baseMipLevel + mipIndex,
                    destSubresources.baseArraySlice + arrayIndex,
                    plane, dest->desc.mipLevels, dest->desc.arraySize);
                const uint32_t srcSubresource = calcSubresource(
                    srcSubresources.baseMipLevel + mipIndex,
                    srcSubresources.baseArraySlice + arrayIndex,
                    plane, src->desc.mipLevels, src->desc.arraySize);
                commandList->ResolveSubresource(
                    dest->resource.Get(), destSubresource, src->resource.Get(), srcSubresource, format);
            }
        }
    }
    dest->lastUseFenceValue = src->lastUseFenceValue = fenceValue;
    return true;
}


} // namespace unityrhi
