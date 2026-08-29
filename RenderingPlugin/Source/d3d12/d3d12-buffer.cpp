// Buffer creation and views ported from NVRHI's d3d12-buffer.cpp
// (External/nvrhi/src/d3d12). Copyright (c) 2014-2021, NVIDIA CORPORATION.
// MIT license (see ThirdParty/NVRHI-LICENSE.txt).

#include "d3d12-backend.h"

#include <cassert>
#include <sstream>

#include "common/dxgi-format.h"
#include "common/format-info.h"
#include "UnityRhiLog.h"

namespace unityrhi
{
Buffer::~Buffer()
{
    if (sharedHandle)
    {
        CloseHandle(sharedHandle);
        sharedHandle = nullptr;
    }
    if (m_ClearUAV != c_InvalidDescriptorIndex)
    {
        m_Resources.shaderResourceViewHeap.releaseDescriptor(m_ClearUAV);
        m_ClearUAV = c_InvalidDescriptorIndex;
    }
}

Buffer* Device::createBuffer(const BufferDesc& d)
{
    BufferDesc desc = d;
    if (desc.isConstantBuffer)
    {
        desc.byteSize = align(d.byteSize, 256ull);
    }

    Buffer* buffer = new Buffer(m_Context, m_Resources, desc);
    buffer->debugName = desc.debugName;

    if (d.isVolatile)
    {
        // Do not create any resources for volatile buffers. Done.
        RegisterResource(buffer);
        return buffer;
    }

    D3D12_RESOURCE_DESC& resourceDesc = buffer->resourceDesc;
    resourceDesc.Width = buffer->desc.byteSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (buffer->desc.canHaveUAVs)
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Acceleration-structure storage needs the dedicated resource flag in
    // addition to ALLOW_UNORDERED_ACCESS. Keep it in the cached description
    // for virtual buffers too, so a later placed-resource binding sees the
    // same requirements as a committed resource.
    if (d.isAccelStructStorage)
    {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
    }

    if (d.isVirtual)
    {
        RegisterResource(buffer);
        return buffer;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;

    const bool isShared = d.sharedResourceFlags != SharedResourceFlags::None;
    if ((d.sharedResourceFlags & SharedResourceFlags::Shared) != 0)
    {
        heapFlags |= D3D12_HEAP_FLAG_SHARED;
    }
    if ((d.sharedResourceFlags & SharedResourceFlags::Shared_CrossAdapter) != 0)
    {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        heapFlags |= D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    }

    switch (buffer->desc.cpuAccess)
    {
    case CpuAccessMode::None:
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        initialState = convertResourceStates(d.initialState);
        if (initialState != D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
            initialState = D3D12_RESOURCE_STATE_COMMON;
        break;

    case CpuAccessMode::Read:
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        initialState = D3D12_RESOURCE_STATE_COPY_DEST;
        break;

    case CpuAccessMode::Write:
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        break;
    }

    // Allow readback buffers to be used as resolve destination targets
    if ((buffer->desc.cpuAccess == CpuAccessMode::Read) && (d.initialState == ResourceStates::ResolveDest))
    {
        heapProps.Type = D3D12_HEAP_TYPE_CUSTOM;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        initialState = D3D12_RESOURCE_STATE_COMMON;
    }

    HRESULT res;
    if (m_EnhancedBarriersSupported)
    {
        const D3D12_RESOURCE_DESC1 desc1 = convertResourceDesc1(resourceDesc);
        res = m_Context.device10->CreateCommittedResource3(
            &heapProps, heapFlags, &desc1, D3D12_BARRIER_LAYOUT_UNDEFINED,
            nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&buffer->resource));
    }
    else
    {
        res = m_Context.device->CreateCommittedResource(
            &heapProps, heapFlags, &resourceDesc, initialState, nullptr,
            IID_PPV_ARGS(&buffer->resource));
    }

    if (FAILED(res))
    {
        LogError("[UnityRHI] CreateCommittedResource call failed for buffer '%s' (hr=0x%08X, %llu bytes).",
            buffer->debugName.c_str(), static_cast<unsigned>(res),
            static_cast<unsigned long long>(desc.byteSize));
        HandleDeviceError(res, "Create buffer resource");
        delete buffer;
        return nullptr;
    }

    if (isShared)
    {
        res = m_Context.device->CreateSharedHandle(
            buffer->resource.Get(), nullptr, GENERIC_ALL, nullptr, &buffer->sharedHandle);
        if (FAILED(res))
        {
            LogError("[UnityRHI] CreateSharedHandle failed for buffer '%s' (hr=0x%08X).",
                buffer->debugName.c_str(), unsigned(res));
            HandleDeviceError(res, "Create buffer shared handle");
            delete buffer;
            return nullptr;
        }
    }

    buffer->postCreate();
    RegisterResource(buffer);
    return buffer;
}

void Buffer::postCreate()
{
    gpuVA = resource->GetGPUVirtualAddress();

    if (!desc.debugName.empty())
    {
        wchar_t wide[256];
        int written = MultiByteToWideChar(CP_UTF8, 0, desc.debugName.c_str(), -1, wide, 255);
        wide[written > 0 ? written : 0] = L'\0';
        resource->SetName(wide);
    }
}

DescriptorIndex Buffer::getClearUAV()
{
    assert(desc.canHaveUAVs);

    if (m_ClearUAV != c_InvalidDescriptorIndex)
        return m_ClearUAV;

    m_ClearUAV = m_Resources.shaderResourceViewHeap.allocateDescriptor();
    createUAV(m_Resources.shaderResourceViewHeap.getCpuHandle(m_ClearUAV).ptr, Format::R32_UINT,
        EntireBuffer, ResourceType::TypedBuffer_UAV);
    m_Resources.shaderResourceViewHeap.copyToShaderVisibleHeap(m_ClearUAV);
    return m_ClearUAV;
}

void* Device::mapBuffer(Buffer* buffer)
{
    void* mappedBuffer = nullptr;
    D3D12_RANGE range{};
    const HRESULT res = buffer->resource->Map(0, &range, &mappedBuffer);

    if (FAILED(res))
    {
        LogError("[UnityRHI] Map call failed for buffer '%s' (hr=0x%08X).",
            buffer->debugName.c_str(), static_cast<unsigned>(res));
        HandleDeviceError(res, "Map buffer");
        return nullptr;
    }

    return mappedBuffer;
}

void Device::unmapBuffer(Buffer* buffer)
{
    buffer->resource->Unmap(0, nullptr);
}

MemoryRequirements Device::getBufferMemoryRequirements(Buffer* buffer) const
{
    MemoryRequirements memReq{};

    const D3D12_RESOURCE_ALLOCATION_INFO allocInfo = m_Context.device->GetResourceAllocationInfo(
        1, 1, &buffer->resourceDesc);

    memReq.alignment = allocInfo.Alignment;
    memReq.size = allocInfo.SizeInBytes;

    return memReq;
}

bool Device::bindBufferMemory(Buffer* buffer, Heap* heap, uint64_t offset)
{
    if (buffer->resource)
        return false; // already bound

    if (!buffer->desc.isVirtual)
        return false; // not supported

    // Port of nvrhi's validation layer (DeviceWrapper::bindBufferMemory);
    // UnityRHI has no separate validation wrapper, so the range/alignment
    // checks live here.
    const MemoryRequirements memReq = getBufferMemoryRequirements(buffer);
    if (offset + memReq.size > heap->desc.capacity ||
        (memReq.alignment != 0 && (offset % memReq.alignment) != 0))
    {
        LogError("[UnityRHI] bindBufferMemory '%s': invalid heap range/alignment.",
            buffer->debugName.c_str());
        return false;
    }

    D3D12_RESOURCE_STATES initialState = convertResourceStates(buffer->desc.initialState);
    if (initialState != D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
        initialState = D3D12_RESOURCE_STATE_COMMON;
    HRESULT hr;
    if (m_EnhancedBarriersSupported)
    {
        const D3D12_RESOURCE_DESC1 desc1 = convertResourceDesc1(buffer->resourceDesc);
        hr = m_Context.device10->CreatePlacedResource2(heap->heap.Get(), offset,
            &desc1, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr,
            0, nullptr, IID_PPV_ARGS(&buffer->resource));
    }
    else
    {
        hr = m_Context.device->CreatePlacedResource(heap->heap.Get(), offset,
            &buffer->resourceDesc, initialState, nullptr, IID_PPV_ARGS(&buffer->resource));
    }
    if (FAILED(hr))
    {
        LogError("[UnityRHI] bindBufferMemory '%s' failed (hr=0x%08X).",
            buffer->debugName.c_str(), unsigned(hr));
        return false;
    }
    buffer->heap = heap;
    buffer->postCreate();
    return true;
}

Buffer* Device::createBufferFromNativeResource(ID3D12Resource* resource, const BufferDesc& desc)
{
    if (!resource)
        return nullptr;
    auto* buffer = new Buffer(m_Context, m_Resources, desc);
    buffer->debugName = desc.debugName;
    buffer->resource = resource;
    buffer->resourceDesc = resource->GetDesc();
    buffer->unityOwnedResource = true;
    buffer->postCreate();
    RegisterResource(buffer);
    return buffer;
}

void Buffer::createCBV(size_t descriptor, BufferRange range) const
{
    assert(desc.isConstantBuffer);

    range = range.resolve(desc);
    assert(range.byteSize <= UINT_MAX);

    D3D12_CONSTANT_BUFFER_VIEW_DESC viewDesc;
    viewDesc.BufferLocation = resource->GetGPUVirtualAddress() + range.byteOffset;
    viewDesc.SizeInBytes = align(UINT(range.byteSize), c_ConstantBufferOffsetSizeAlignment);
    m_Context.device->CreateConstantBufferView(&viewDesc, {descriptor});
}

void Buffer::createNullSRV(size_t descriptor, Format format, const Context& context)
{
    const DxgiFormatMapping& mapping = getDxgiFormatMapping(format == Format::UNKNOWN ? Format::R32_UINT : format);

    D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
    viewDesc.Format = mapping.srvFormat;
    viewDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    context.device->CreateShaderResourceView(nullptr, &viewDesc, {descriptor});
}

void Buffer::createSRV(size_t descriptor, Format format, BufferRange range, ResourceType type) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};

    viewDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    if (format == Format::UNKNOWN)
    {
        format = desc.format;
    }

    range = range.resolve(desc);

    switch (type)
    {
    case ResourceType::StructuredBuffer_SRV:
        assert(desc.structStride != 0);
        viewDesc.Format = DXGI_FORMAT_UNKNOWN;
        viewDesc.Buffer.FirstElement = range.byteOffset / desc.structStride;
        viewDesc.Buffer.NumElements = UINT(range.byteSize / desc.structStride);
        viewDesc.Buffer.StructureByteStride = desc.structStride;
        break;

    case ResourceType::RawBuffer_SRV:
        viewDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        viewDesc.Buffer.FirstElement = range.byteOffset / 4;
        viewDesc.Buffer.NumElements = UINT(range.byteSize / 4);
        viewDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        break;

    case ResourceType::TypedBuffer_SRV:
    {
        assert(format != Format::UNKNOWN);
        const DxgiFormatMapping& mapping = getDxgiFormatMapping(format);
        const FormatInfo& formatInfo = getFormatInfo(format);

        viewDesc.Format = mapping.srvFormat;
        viewDesc.Buffer.FirstElement = range.byteOffset / formatInfo.bytesPerBlock;
        viewDesc.Buffer.NumElements = UINT(range.byteSize / formatInfo.bytesPerBlock);
        break;
    }

    default:
        LogError("[UnityRHI] Buffer::createSRV: unsupported ResourceType %u.", uint32_t(type));
        return;
    }

    m_Context.device->CreateShaderResourceView(resource.Get(), &viewDesc, {descriptor});
}

void Buffer::createNullUAV(size_t descriptor, Format format, const Context& context)
{
    const DxgiFormatMapping& mapping = getDxgiFormatMapping(format == Format::UNKNOWN ? Format::R32_UINT : format);

    D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};
    viewDesc.Format = mapping.srvFormat;
    viewDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    context.device->CreateUnorderedAccessView(nullptr, nullptr, &viewDesc, {descriptor});
}

void Buffer::createUAV(size_t descriptor, Format format, BufferRange range, ResourceType type) const
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};

    viewDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;

    if (format == Format::UNKNOWN)
    {
        format = desc.format;
    }

    range = range.resolve(desc);

    switch (type)
    {
    case ResourceType::StructuredBuffer_UAV:
        assert(desc.structStride != 0);
        viewDesc.Format = DXGI_FORMAT_UNKNOWN;
        viewDesc.Buffer.FirstElement = range.byteOffset / desc.structStride;
        viewDesc.Buffer.NumElements = UINT(range.byteSize / desc.structStride);
        viewDesc.Buffer.StructureByteStride = desc.structStride;
        break;

    case ResourceType::RawBuffer_UAV:
        viewDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        viewDesc.Buffer.FirstElement = range.byteOffset / 4;
        viewDesc.Buffer.NumElements = UINT(range.byteSize / 4);
        viewDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        break;

    case ResourceType::TypedBuffer_UAV:
    {
        assert(format != Format::UNKNOWN);
        const DxgiFormatMapping& mapping = getDxgiFormatMapping(format);
        const FormatInfo& formatInfo = getFormatInfo(format);

        viewDesc.Format = mapping.srvFormat;
        viewDesc.Buffer.FirstElement = range.byteOffset / formatInfo.bytesPerBlock;
        viewDesc.Buffer.NumElements = UINT(range.byteSize / formatInfo.bytesPerBlock);
        break;
    }

    default:
        LogError("[UnityRHI] Buffer::createUAV: unsupported ResourceType %u.", uint32_t(type));
        return;
    }

    m_Context.device->CreateUnorderedAccessView(resource.Get(), nullptr, &viewDesc, {descriptor});
}

// Port of nvrhi CommandList::writeBuffer. The memcpy into the upload heap now
// happens on the recording thread; replay only records the GPU copy.
bool ReplayContext::writeBuffer(Buffer* buffer, UploadTicket* upload, uint64_t destOffsetBytes)
{
    Device* device = Device::Get();
    if (!buffer || !device)
        return false;

    D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
    ID3D12Resource* uploadBuffer = nullptr;
    uint64_t offsetInUploadBuffer = 0;
    if (!device->uploadManager().resolveTicket(upload, fenceValue, &uploadBuffer, &offsetInUploadBuffer, &gpuVA))
    {
        LogError("[UnityRHI] WriteBuffer received an invalid record-time upload ticket");
        return false;
    }

    if (buffer->desc.isVolatile)
    {
        volatileConstantBufferAddresses[buffer] = gpuVA;
        anyVolatileBufferWrites = true;
    }
    else
    {
        if (!buffer->resource)
            return false;
        if (enableAutomaticBarriers)
        {
            requireBufferState(buffer, ResourceStates::CopyDest);
            bindingStatesDirty = true;
        }
        commitBarriers();

        commandList->CopyBufferRegion(buffer->resource.Get(), destOffsetBytes, uploadBuffer,
            offsetInUploadBuffer, upload->size);
    }

    buffer->lastUseFenceValue = fenceValue;
    return true;
}

// Port of nvrhi CommandList::clearBufferUInt.
bool ReplayContext::clearBufferUInt(Buffer* buffer, uint32_t clearValue)
{
    Device* device = Device::Get();
    if (!buffer || !buffer->resource || !device)
        return false;

    if (!buffer->desc.canHaveUAVs)
    {
        LogError("[UnityRHI] Cannot clear buffer '%s' because it was created with canHaveUAVs = false",
            buffer->debugName.c_str());
        return false;
    }

    if (enableAutomaticBarriers)
    {
        requireBufferState(buffer, ResourceStates::UnorderedAccess);
        bindingStatesDirty = true;
    }
    commitBarriers();

    commitDescriptorHeaps();

    DescriptorIndex clearUAV = buffer->getClearUAV();
    if (clearUAV == c_InvalidDescriptorIndex)
        return false;

    // The clear UAV may have just been created; make sure the shader-visible
    // heap pointer bound above is still current after a potential Grow.
    commitDescriptorHeaps();

    const uint32_t values[4] = {clearValue, clearValue, clearValue, clearValue};
    StaticDescriptorHeap& heap = device->resources().shaderResourceViewHeap;
    commandList->ClearUnorderedAccessViewUint(
        heap.getGpuHandle(clearUAV),
        heap.getCpuHandle(clearUAV),
        buffer->resource.Get(), values, 0, nullptr);
    buffer->lastUseFenceValue = fenceValue;
    return true;
}

// Port of nvrhi CommandList::copyBuffer.
bool ReplayContext::copyBuffer(Buffer* dest, uint64_t destOffsetBytes, Buffer* src, uint64_t srcOffsetBytes, uint64_t dataSizeBytes)
{
    if (!dest || !src || !dest->resource || !src->resource)
        return false;

    if (enableAutomaticBarriers)
    {
        requireBufferState(dest, ResourceStates::CopyDest);
        requireBufferState(src, ResourceStates::CopySource);
        bindingStatesDirty = true;
    }
    commitBarriers();

    commandList->CopyBufferRegion(dest->resource.Get(), destOffsetBytes, src->resource.Get(), srcOffsetBytes, dataSizeBytes);
    dest->lastUseFenceValue = src->lastUseFenceValue = fenceValue;
    return true;
}
} // namespace unityrhi
