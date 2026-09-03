// Command-stream replay: decode the C#-recorded opcode stream onto Unity's
// live ID3D12GraphicsCommandList. Resource and pipeline work lives in the
// NVRHI-ported d3d12 backend; this file is UnityRHI's decoder.

#include "CommandStream.h"
#include "CommandStreamWire.h"
#include "Dlrr.h"
#include "Dlss.h"
#include "DlssNr.h"
#include "UnityRhiProfiler.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Breadcrumbs.h"
#include "d3d12/d3d12-backend.h"
#include "common/dxgi-format.h"
#include "UnityRhiLog.h"

namespace unityrhi
{
bool ReplayContext::commitDescriptorHeaps()
{
    Device* device = Device::Get();
    if (!device)
        return false;

    ID3D12DescriptorHeap* heapSRVetc = device->resources().shaderResourceViewHeap.getShaderVisibleHeap();
    ID3D12DescriptorHeap* heapSamplers = device->resources().samplerHeap.getShaderVisibleHeap();
    if (heapSRVetc == currentHeapSRVetc && heapSamplers == currentHeapSamplers)
        return false;

    ID3D12DescriptorHeap* heaps[2] = {heapSRVetc, heapSamplers};
    commandList->SetDescriptorHeaps(2, heaps);
    currentHeapSRVetc = heapSRVetc;
    currentHeapSamplers = heapSamplers;
    return true;
}

void ReplayContext::clearState()
{
    commandList->ClearState(nullptr);
    anyVolatileBufferWrites = false;
    currentPipeline = nullptr;
    currentIndirectParams = nullptr;
    currentBindings.clear();
    currentComputeVolatileCBs.clear();
    currentGraphicsPipeline = nullptr;
    currentFramebuffer = nullptr;
    currentGraphicsIndirectParams = nullptr;
    currentGraphicsIndirectCountBuffer = nullptr;
    currentGraphicsVolatileCBs.clear();
    graphicsStateActive = false;
    currentShaderTable = nullptr;
    currentRayTracingBindings.clear();
    rayTracingStateActive = false;
    currentHeapSRVetc = nullptr;
    currentHeapSamplers = nullptr;
    commitDescriptorHeaps();
}

uint64_t ReplayUavBarrierCount()
{
    // Counter maintained by d3d12/d3d12-state-tracking.cpp.
    return g_uavBarrierCount.load(std::memory_order_relaxed);
}

namespace
{
class Reader
{
public:
    Reader(const uint8_t* begin, const uint8_t* end)
        : m_pos(begin)
        , m_end(end)
    {
    }

    template <typename T>
    bool Read(T& value)
    {
        if (size_t(m_end - m_pos) < sizeof(T))
            return false;
        std::memcpy(&value, m_pos, sizeof(T));
        m_pos += sizeof(T);
        return true;
    }

    template <typename T>
    bool ReadSpan(T* values, size_t count)
    {
        if (count > SIZE_MAX / sizeof(T))
            return false;
        const size_t size = count * sizeof(T);
        if (size_t(m_end - m_pos) < size)
            return false;
        if (size != 0)
            std::memcpy(values, m_pos, size);
        m_pos += size;
        return true;
    }

    bool Skip(size_t size) { return ReadBytes(size) != nullptr; }

    template <typename T>
    bool SkipSpan(size_t count)
    {
        return count <= SIZE_MAX / sizeof(T) && Skip(count * sizeof(T));
    }

    // Returns a pointer into the stream and advances past `size` bytes.
    const uint8_t* ReadBytes(size_t size)
    {
        if (size_t(m_end - m_pos) < size)
            return nullptr;
        const uint8_t* result = m_pos;
        m_pos += size;
        return result;
    }

    const uint8_t* Position() const { return m_pos; }

private:
    const uint8_t* m_pos;
    const uint8_t* m_end;
};

bool DecodeBindings(Reader& reader, uint32_t count, std::vector<RhiResource*>& bindings)
{
    static_assert(sizeof(RhiResource*) == sizeof(uint64_t),
        "The Version 14 wire format requires 64-bit resource handles.");
    bindings.resize(count);
    return reader.ReadSpan(bindings.data(), bindings.size());
}

// Port of nvrhi CommandList::commitDescriptorHeaps: rebind when the heaps
// changed (e.g. grew on the main thread between events).


// Explicit UAV barrier (UavBarrier opcode). NVRHI has no direct equivalent
// command-list method; pending tracked barriers are committed first so the
// explicit barrier lands in stream order.
void UavBarrier(ReplayContext& context, RhiResource* resource)
{
    if (!resource || !resource->resource)
        return;

    context.commitBarriers();

    Device* device = Device::Get();
    if (!resource->unityOwnedResource && device && device->GetEnhancedBarriersSupported() && context.commandList7)
    {
        if (resource->kind == RhiResource::Kind::Texture)
        {
            auto* texture = static_cast<Texture*>(resource);
            // Enhanced texture barriers must name the actual layout of every
            // covered subresource. A mip-generation texture can simultaneously
            // have source mips in SRV layout and destination mips in UAV layout,
            // so an all-subresources UAV->UAV barrier is invalid. Emit the
            // explicit UAV ordering barrier only for the subresources that are
            // currently tracked as UAVs.
            context.d3dTextureBarriers.clear();
            context.d3dTextureBarriers.reserve(
                size_t(texture->desc.mipLevels) * texture->desc.arraySize);
            for (uint32_t arraySlice = 0; arraySlice < texture->desc.arraySize; ++arraySlice)
            {
                for (uint32_t mipLevel = 0; mipLevel < texture->desc.mipLevels; ++mipLevel)
                {
                    const ResourceStates state = texture->permanentState != ResourceStates::Unknown
                        ? texture->permanentState
                        : context.stateTracker.getTextureSubresourceState(texture, arraySlice, mipLevel);
                    if ((state & ResourceStates::UnorderedAccess) == 0)
                        continue;

                    D3D12_TEXTURE_BARRIER barrier{};
                    barrier.SyncBefore = D3D12_BARRIER_SYNC_ALL_SHADING;
                    barrier.SyncAfter = D3D12_BARRIER_SYNC_ALL_SHADING;
                    barrier.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                    barrier.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                    barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
                    barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
                    barrier.pResource = resource->resource.Get();
                    barrier.Subresources.IndexOrFirstMipLevel = mipLevel;
                    barrier.Subresources.NumMipLevels = 1;
                    barrier.Subresources.FirstArraySlice = arraySlice;
                    barrier.Subresources.NumArraySlices = 1;
                    barrier.Subresources.NumPlanes = texture->planeCount;
                    context.d3dTextureBarriers.push_back(barrier);
                }
            }

            if (context.d3dTextureBarriers.empty())
                return;

            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = uint32_t(context.d3dTextureBarriers.size());
            group.pTextureBarriers = context.d3dTextureBarriers.data();
            context.commandList7->Barrier(1, &group);
        }
        else if (resource->kind == RhiResource::Kind::Buffer)
        {
            const auto* buffer = static_cast<const Buffer*>(resource);
            D3D12_BUFFER_BARRIER barrier{};
            barrier.SyncBefore = D3D12_BARRIER_SYNC_ALL_SHADING;
            barrier.SyncAfter = D3D12_BARRIER_SYNC_ALL_SHADING;
            barrier.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            barrier.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            barrier.pResource = resource->resource.Get();
            barrier.Size = buffer->desc.byteSize;
            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_BUFFER;
            group.NumBarriers = 1;
            group.pBufferBarriers = &barrier;
            context.commandList7->Barrier(1, &group);
        }
        else
        {
            return;
        }
        g_uavBarrierCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource->resource.Get();
    context.commandList->ResourceBarrier(1, &barrier);
    g_uavBarrierCount.fetch_add(1, std::memory_order_relaxed);
}



// Port of nvrhi CommandList::setComputeBindings / setGraphicsBindings
// (d3d12-resource-bindings.cpp); `graphics` selects the root-parameter API.


bool ExecuteCopyBuffer(Reader& reader, ReplayContext& context)
{
    wire::CopyBufferPayload command{};
    if (!reader.Read(command))
        return false;
    return context.copyBuffer(reinterpret_cast<Buffer*>(uintptr_t(command.dest)), command.destOffset,
        reinterpret_cast<Buffer*>(uintptr_t(command.src)), command.srcOffset, command.byteSize);
}

// Upload bytes are already in the native upload heap at record time.
bool ExecuteWriteBuffer(Reader& reader, ReplayContext& context)
{
    NativeProfileScope profile("UnityRHI.WriteBuffer");
    wire::WriteBufferPayload command{};
    if (!reader.Read(command))
        return false;
    return context.writeBuffer(reinterpret_cast<Buffer*>(uintptr_t(command.buffer)),
        reinterpret_cast<UploadTicket*>(uintptr_t(command.uploadTicket)), command.destOffset);
}

// Texels are laid out in the native upload heap at record time.
bool ExecuteWriteTexture(Reader& reader, ReplayContext& context)
{
    wire::WriteTexturePayload command{};
    if (!reader.Read(command))
        return false;
    return context.writeTexture(reinterpret_cast<Texture*>(uintptr_t(command.texture)),
        command.arraySlice, command.mipLevel,
        reinterpret_cast<UploadTicket*>(uintptr_t(command.uploadTicket)));
}

bool ExecuteCopyTextureToBuffer(Reader& reader, ReplayContext& context)
{
    wire::CopyTextureToBufferPayload command{};
    if (!reader.Read(command))
        return false;
    return context.copyTexture(reinterpret_cast<Buffer*>(uintptr_t(command.dest)), command.destOffset,
        reinterpret_cast<Texture*>(uintptr_t(command.src)), command.arraySlice, command.mipLevel);
}

// Decode the flattened compute state and delegate execution to the backend.
bool ExecuteSetComputeState(Reader& reader, ReplayContext& context)
{
    NativeProfileScope profile("UnityRHI.Diagnostic.SetComputeState");
    wire::StatePayload command{};
    if (!reader.Read(command))
        return false;
    if ((command.flags & ~wire::StateFlagReuseBindings) != 0)
        return false;
    const bool reuseBindings = (command.flags & wire::StateFlagReuseBindings) != 0;
    context.diagnosticReuseBindings = reuseBindings;
    bool ok = false;
    if (reuseBindings)
    {
        if (context.currentBindings.size() != command.bindingCount)
            return false;
        ok = context.setComputeState(reinterpret_cast<ComputePipeline*>(uintptr_t(command.object)),
            reinterpret_cast<Buffer*>(uintptr_t(command.indirectParams)), context.currentBindings);
    }
    else
    {
        if (!DecodeBindings(reader, command.bindingCount, context.decodedBindings))
            return false;
        ok = context.setComputeState(reinterpret_cast<ComputePipeline*>(uintptr_t(command.object)),
            reinterpret_cast<Buffer*>(uintptr_t(command.indirectParams)), context.decodedBindings);
    }

    if (!ok)
    {
        const std::vector<RhiResource*>& bindings = reuseBindings
            ? context.currentBindings : context.decodedBindings;
        LogError("[UnityRHI] SetComputeState diagnostic: command=%u, reuseBindings=%u, "
                 "bindingCount=%u, pipeline=%p, indirectParams=%p.",
            context.diagnosticCommandIndex, reuseBindings ? 1u : 0u,
            command.bindingCount, reinterpret_cast<void*>(uintptr_t(command.object)),
            reinterpret_cast<void*>(uintptr_t(command.indirectParams)));
        if (Device* device = Device::Get())
        {
            device->LogResourceDiagnostic("pipeline", 0,
                reinterpret_cast<RhiResource*>(uintptr_t(command.object)));
            if (command.indirectParams != 0)
                device->LogResourceDiagnostic("indirect", 0,
                    reinterpret_cast<RhiResource*>(uintptr_t(command.indirectParams)));
            for (uint32_t i = 0; i < bindings.size(); ++i)
                device->LogResourceDiagnostic("binding", i, bindings[i]);
        }
    }
    return ok;
}

// Decode the flattened graphics state and delegate execution to the backend.
bool ExecuteSetGraphicsState(Reader& reader, ReplayContext& context)
{
    wire::GraphicsStatePayload command{};
    if (!reader.Read(command))
        return false;
    context.decodedVertexBuffers.resize(command.vertexBufferCount);
    for (uint32_t i = 0; i < command.vertexBufferCount; ++i)
    {
        wire::VertexBufferBinding binding{};
        if (!reader.Read(binding))
            return false;
        context.decodedVertexBuffers[i].buffer = reinterpret_cast<Buffer*>(uintptr_t(binding.buffer));
        context.decodedVertexBuffers[i].slot = binding.slot;
        context.decodedVertexBuffers[i].offset = binding.offset;
    }
    uint32_t bindingCount = 0;
    if (!reader.Read(bindingCount))
        return false;
    if (!DecodeBindings(reader, bindingCount, context.decodedBindings))
        return false;
    RhiViewport viewport{command.viewport.minX, command.viewport.maxX,
        command.viewport.minY, command.viewport.maxY, command.viewport.minZ, command.viewport.maxZ};
    const float blendConstantColor[4] = {command.blendR, command.blendG, command.blendB, command.blendA};
    return context.setGraphicsState(reinterpret_cast<GraphicsPipeline*>(uintptr_t(command.pipeline)),
        reinterpret_cast<Framebuffer*>(uintptr_t(command.framebuffer)), viewport, blendConstantColor,
        reinterpret_cast<Buffer*>(uintptr_t(command.indexBuffer)), Format(command.indexFormat), command.indexOffset,
        reinterpret_cast<Buffer*>(uintptr_t(command.indirectParams)),
        reinterpret_cast<Buffer*>(uintptr_t(command.indirectCountBuffer)),
        context.decodedVertexBuffers.data(), uint32_t(context.decodedVertexBuffers.size()),
        context.decodedBindings);
}

// Port of nvrhi CommandList::updateGraphicsVolatileBuffers.


// Ports of nvrhi CommandList::draw / drawIndexed (d3d12-graphics.cpp).
bool ExecuteDraw(Reader& reader, ReplayContext& context)
{
    wire::DrawPayload command{};
    return reader.Read(command) && context.draw(command.a, command.b, command.c, command.d);
}

bool ExecuteDrawIndexed(Reader& reader, ReplayContext& context)
{
    wire::DrawIndexedPayload command{};
    return reader.Read(command) &&
        context.drawIndexed(command.a, command.b, command.c, command.d, command.e);
}

// Port of nvrhi CommandList::clearTextureFloat (render-target path; UAV-based
// texture clears are not ported).
bool ExecuteClearTextureFloat(Reader& reader, ReplayContext& context)
{
    wire::ClearTextureFloatPayload command{};
    const bool read = reader.Read(command);
    const float color[4] = {command.r, command.g, command.b, command.a};
    return read && context.clearTextureFloat(
        reinterpret_cast<Texture*>(uintptr_t(command.texture)), color);
}

bool ExecuteClearTextureFloatSubresources(Reader& reader, ReplayContext& context)
{
    wire::ClearTextureFloatSubresourcesPayload command{};
    const bool read = reader.Read(command);
    TextureSubresourceSet subresources{command.subresources.baseMipLevel, command.subresources.numMipLevels,
        command.subresources.baseArraySlice, command.subresources.numArraySlices};
    const float color[4] = {command.r, command.g, command.b, command.a};
    return read && context.clearTextureFloat(
        reinterpret_cast<Texture*>(uintptr_t(command.texture)), subresources, color);
}

// Port of nvrhi CommandList::clearDepthStencilTexture.
bool ExecuteClearDepthStencilTexture(Reader& reader, ReplayContext& context)
{
    wire::ClearDepthStencilPayload command{};
    return reader.Read(command) && context.clearDepthStencilTexture(
        reinterpret_cast<Texture*>(uintptr_t(command.texture)), command.clearDepth != 0,
        command.depth, command.clearStencil != 0, uint8_t(command.stencil));
}

bool ExecuteClearDepthStencilTextureSubresources(Reader& reader, ReplayContext& context)
{
    wire::ClearDepthStencilSubresourcesPayload command{};
    if (!reader.Read(command))
        return false;
    TextureSubresourceSet subresources{command.subresources.baseMipLevel, command.subresources.numMipLevels,
        command.subresources.baseArraySlice, command.subresources.numArraySlices};
    return context.clearDepthStencilTexture(reinterpret_cast<Texture*>(uintptr_t(command.texture)), subresources,
        command.clearDepth != 0, command.depth, command.clearStencil != 0, uint8_t(command.stencil));
}

bool ExecuteClearTextureUInt(Reader& reader, ReplayContext& context)
{
    wire::ClearTextureUIntPayload command{};
    if (!reader.Read(command))
        return false;
    TextureSubresourceSet subresources{command.subresources.baseMipLevel, command.subresources.numMipLevels,
        command.subresources.baseArraySlice, command.subresources.numArraySlices};
    return context.clearTextureUInt(
        reinterpret_cast<Texture*>(uintptr_t(command.texture)), subresources, command.value);
}

// Port of nvrhi CommandList::updateComputeVolatileBuffers.


// Port of nvrhi CommandList::setPushConstants (d3d12-commandlist.cpp).
bool ExecuteSetPushConstants(Reader& reader, ReplayContext& context)
{
    wire::MarkerPayload command{};
    if (!reader.Read(command))
        return false;
    const uint32_t byteSize = command.byteSize;
    const uint8_t* data = reader.ReadBytes(command.byteSize);
    if (!data)
        return false;

    const bool graphics = context.graphicsStateActive;
    const bool rayTracing = context.rayTracingStateActive;
    if (!graphics && !rayTracing && !context.currentPipeline)
    {
        LogError("[UnityRHI] SetPushConstants requires SetComputeState, SetGraphicsState, or SetRayTracingState first.");
        return false;
    }

    const RootSignature* rootSignature = graphics
        ? context.currentGraphicsPipeline->rootSignature.get()
        : rayTracing
            ? context.currentShaderTable->pipeline->globalRootSignature.get()
            : context.currentPipeline->rootSignature.get();
    if (rootSignature->rootParameterPushConstants == c_InvalidRootParameterIndex ||
        rootSignature->pushConstantByteSize != byteSize)
    {
        LogError("[UnityRHI] SetPushConstants: the layout expects %u bytes of push constants, got %u.",
            rootSignature->pushConstantByteSize, byteSize);
        return false;
    }

    if (graphics)
        context.commandList->SetGraphicsRoot32BitConstants(
            rootSignature->rootParameterPushConstants, byteSize / 4, data, 0);
    else
        context.commandList->SetComputeRoot32BitConstants(
            rootSignature->rootParameterPushConstants, byteSize / 4, data, 0);
    return true;
}

bool ExecuteBeginMarker(Reader& reader, ReplayContext& context)
{
    wire::MarkerPayload command{};
    if (!reader.Read(command))
        return false;
    const uint32_t byteLength = command.byteSize;
    const uint8_t* bytes = reader.ReadBytes(byteLength);
    if (!bytes)
        return false;

    // Null-terminate locally: the stream stores length-prefixed UTF8.
    char name[128];
    const uint32_t copyLength = byteLength < sizeof(name) - 1 ? byteLength : uint32_t(sizeof(name) - 1);
    std::memcpy(name, bytes, copyLength);
    name[copyLength] = '\0';

    // Metadata 1 = ANSI string event (PIX/RenderDoc-visible).
    {
        NativeProfileScope profile("UnityRHI.D3D12BeginEvent");
        context.commandList->BeginEvent(1, name, copyLength + 1);
    }
    BeginCommandMarkerProfile(name);
    Breadcrumbs::Get().Push("marker '%s'", name);
    return true;
}

bool ExecuteSetBufferState(Reader& reader, ReplayContext& context)
{
        NativeProfileScope profile("UnityRHI.SetBufferState");
    wire::HandleUInt32Payload command{};
    if (!reader.Read(command))
        return false;
    auto* buffer = reinterpret_cast<Buffer*>(uintptr_t(command.handle));
    if (!buffer)
        return false;

    context.setBufferState(buffer, ResourceStates(command.value));
    return true;
}

bool ExecuteSetTextureState(Reader& reader, ReplayContext& context)
{
        NativeProfileScope profile("UnityRHI.SetTextureState");
    wire::HandleUInt32Payload command{};
    if (!reader.Read(command))
        return false;
    auto* texture = reinterpret_cast<Texture*>(uintptr_t(command.handle));
    if (!texture)
        return false;

    context.setTextureState(texture, ResourceStates(command.value));
    return true;
}

bool ExecuteDrawIndirect(Reader& reader, ReplayContext& context, CommandOpcode opcode)
{
    const bool useCount = opcode == CommandOpcode::DrawIndexedIndirectCount;
    if (useCount)
    {
        wire::DrawIndirectCountPayload command{};
        return reader.Read(command) && context.drawIndirect(command.paramsOffset,
            command.maxDrawCount, true, command.countOffset, true);
    }
    wire::DrawIndirectPayload command{};
    return reader.Read(command) && context.drawIndirect(command.offset, command.count,
        opcode != CommandOpcode::DrawIndirect, 0, false);
}

bool ExecuteCopyTexture(Reader& reader, ReplayContext& context)
{
    wire::TextureCopyPayload command{};
    if (!reader.Read(command))
        return false;
    RhiTextureSlice destSlice{}, srcSlice{};
    static_assert(sizeof(destSlice) == sizeof(command.destSlice));
    std::memcpy(&destSlice, &command.destSlice, sizeof(destSlice));
    std::memcpy(&srcSlice, &command.srcSlice, sizeof(srcSlice));
    return context.copyTexture(reinterpret_cast<Texture*>(uintptr_t(command.dest)), destSlice,
        reinterpret_cast<Texture*>(uintptr_t(command.src)), srcSlice);
}

bool ExecuteCopyStagingTexture(Reader& reader, ReplayContext& context, CommandOpcode opcode)
{
    wire::TextureCopyPayload command{};
    if (!reader.Read(command))
        return false;
    RhiTextureSlice destSlice{}, srcSlice{};
    static_assert(sizeof(destSlice) == sizeof(command.destSlice));
    std::memcpy(&destSlice, &command.destSlice, sizeof(destSlice));
    std::memcpy(&srcSlice, &command.srcSlice, sizeof(srcSlice));
    if (opcode == CommandOpcode::CopyTextureFromStaging)
        return context.copyTexture(reinterpret_cast<Texture*>(uintptr_t(command.dest)), destSlice,
            reinterpret_cast<StagingTexture*>(uintptr_t(command.src)), srcSlice);
    return context.copyTexture(reinterpret_cast<StagingTexture*>(uintptr_t(command.dest)), destSlice,
        reinterpret_cast<Texture*>(uintptr_t(command.src)), srcSlice);
}

bool ExecuteResolveTexture(Reader& reader, ReplayContext& context)
{
    wire::ResolveTexturePayload command{};
    if (!reader.Read(command))
        return false;
    TextureSubresourceSet destSubresources{command.destSubresources.baseMipLevel,
        command.destSubresources.numMipLevels, command.destSubresources.baseArraySlice,
        command.destSubresources.numArraySlices};
    TextureSubresourceSet srcSubresources{command.srcSubresources.baseMipLevel,
        command.srcSubresources.numMipLevels, command.srcSubresources.baseArraySlice,
        command.srcSubresources.numArraySlices};
    return context.resolveTexture(reinterpret_cast<Texture*>(uintptr_t(command.dest)), destSubresources,
        reinterpret_cast<Texture*>(uintptr_t(command.src)), srcSubresources);
}

bool ExecuteBeginTrackingBufferState(Reader& reader, ReplayContext& context)
{
    wire::HandleUInt32Payload command{};
    if (!reader.Read(command))
        return false;
    auto* buffer = reinterpret_cast<Buffer*>(uintptr_t(command.handle));
    if (!buffer)
        return false;

    context.beginTrackingBufferState(buffer, ResourceStates(command.value));
    return true;
}

bool ExecuteBeginTrackingTextureState(Reader& reader, ReplayContext& context)
{
    wire::TextureSubresourceStatePayload command{};
    if (!reader.Read(command))
        return false;
    auto* texture = reinterpret_cast<Texture*>(uintptr_t(command.texture));
    if (!texture)
        return false;
    TextureSubresourceSet subresources{command.subresources.baseMipLevel, command.subresources.numMipLevels,
        command.subresources.baseArraySlice, command.subresources.numArraySlices};
    context.beginTrackingTextureState(texture, subresources, ResourceStates(command.state));
    return true;
}

bool ExecuteSetTextureSubresourceState(Reader& reader, ReplayContext& context)
{
    wire::TextureSubresourceStatePayload command{};
    if (!reader.Read(command))
        return false;
    auto* texture = reinterpret_cast<Texture*>(uintptr_t(command.texture));
    if (!texture)
        return false;
    TextureSubresourceSet subresources{command.subresources.baseMipLevel, command.subresources.numMipLevels,
        command.subresources.baseArraySlice, command.subresources.numArraySlices};
    context.setTextureState(texture, subresources, ResourceStates(command.state));
    return true;
}

bool ExecuteResourceStateToggle(Reader& reader, ReplayContext& context, CommandOpcode opcode)
{
    wire::HandleUInt32Payload command{};
    if (!reader.Read(command) || !command.handle)
        return false;

    switch (opcode)
    {
    case CommandOpcode::SetPermanentTextureState:
        context.setPermanentTextureState(reinterpret_cast<Texture*>(uintptr_t(command.handle)), ResourceStates(command.value));
        break;
    case CommandOpcode::SetPermanentBufferState:
        context.setPermanentBufferState(reinterpret_cast<Buffer*>(uintptr_t(command.handle)), ResourceStates(command.value));
        break;
    case CommandOpcode::SetEnableUavBarriersForTexture:
        context.setEnableUavBarriersForTexture(reinterpret_cast<Texture*>(uintptr_t(command.handle)), command.value != 0);
        break;
    case CommandOpcode::SetEnableUavBarriersForBuffer:
        context.setEnableUavBarriersForBuffer(reinterpret_cast<Buffer*>(uintptr_t(command.handle)), command.value != 0);
        break;
    default:
        return false;
    }
    return true;
}

// Port of nvrhi CommandList::dispatchIndirect (d3d12-compute.cpp): the
// argument buffer comes from the current ComputeState.indirectParams.
bool ExecuteDispatchIndirect(Reader& reader, ReplayContext& context)
{
    wire::HandlePayload command{};
    return reader.Read(command) && context.dispatchIndirect(command.handle);
}

bool ExecuteUavBarrier(Reader& reader, ReplayContext& context)
{
        NativeProfileScope profile("UnityRHI.UavBarrier");
    wire::HandlePayload command{};
    if (!reader.Read(command))
        return false;
    auto* resource = reinterpret_cast<RhiResource*>(uintptr_t(command.handle));
    if (!resource || !resource->resource)
        return false;

    UavBarrier(context, resource);
    resource->lastUseFenceValue = context.fenceValue;
    return true;
}

bool ExecuteTimerQuery(Reader& reader, ReplayContext& context, CommandOpcode opcode)
{
    wire::HandlePayload command{};
    if (!reader.Read(command) || !command.handle)
        return false;
    auto* query = reinterpret_cast<TimerQuery*>(uintptr_t(command.handle));
    if (query->kind != RhiResource::Kind::TimerQuery || !query->queryHeap || !query->resolveBuffer)
        return false;

    if (opcode == CommandOpcode::BeginTimerQuery)
    {
        query->started = true;
        query->resolved = false;
        query->time = 0.f;
        query->completionInstance.store(0, std::memory_order_release);
        context.commandList->EndQuery(query->queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    }
    else
    {
        if (!query->started)
            return false;
        context.commandList->EndQuery(query->queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        context.commandList->ResolveQueryData(query->queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            0, 2, query->resolveBuffer.Get(), 0);
        query->lastUseFenceValue = context.fenceValue;
        // PluginMain records the lifetime marker at the tail of this replay.
        // Publishing the instance only after ResolveQueryData has been
        // recorded lets PollTimerQuery use that in-list GPU marker as the
        // completion authority without flushing Unity's command buffers.
        query->completionInstance.store(context.fenceValue, std::memory_order_release);
    }
    return true;
}

// Port of nvrhi CommandList::clearBufferUInt (d3d12-buffer.cpp).
bool ExecuteClearBufferUInt(Reader& reader, ReplayContext& context)
{
    wire::HandleUInt32Payload command{};
    return reader.Read(command) &&
        context.clearBufferUInt(reinterpret_cast<Buffer*>(uintptr_t(command.handle)), command.value);
}



bool ExecuteSetRayTracingState(Reader& reader, ReplayContext& context)
{
    wire::BindingStatePayload command{};
    if (!reader.Read(command))
        return false;
    if (!DecodeBindings(reader, command.bindingCount, context.decodedBindings))
        return false;
    return context.setRayTracingState(
        reinterpret_cast<ShaderTable*>(uintptr_t(command.object)), context.decodedBindings);
}

bool ExecuteDispatchRays(Reader& reader, ReplayContext& context)
{
    wire::DispatchPayload command{};
    return reader.Read(command) && context.dispatchRays(command.x, command.y, command.z);
}

bool ExecuteBuildBottomLevelAccelStruct(Reader& reader, ReplayContext& context)
{
    wire::AccelStructBuildPayload command{};
    if (!reader.Read(command))
        return false;
    context.decodedGeometries.resize(command.elementCount);
    if (!reader.ReadSpan(context.decodedGeometries.data(), context.decodedGeometries.size()))
        return false;
    return context.buildBottomLevelAccelStruct(
        reinterpret_cast<AccelStruct*>(uintptr_t(command.accelStruct)), context.decodedGeometries.data(),
        command.elementCount, RhiRtAccelStructBuildFlags(command.buildFlags));
}



bool ExecuteBuildTopLevelAccelStruct(Reader& reader, ReplayContext& context)
{
    wire::AccelStructBuildPayload command{};
    if (!reader.Read(command))
        return false;
    context.decodedInstances.resize(command.elementCount);
    if (!reader.ReadSpan(context.decodedInstances.data(), context.decodedInstances.size()))
        return false;
    return context.buildTopLevelAccelStruct(
        reinterpret_cast<AccelStruct*>(uintptr_t(command.accelStruct)), context.decodedInstances.data(),
        command.elementCount, RhiRtAccelStructBuildFlags(command.buildFlags));
}

bool ExecuteBuildTopLevelAccelStructFromBuffer(Reader& reader, ReplayContext& context)
{
    wire::TlasFromBufferPayload command{};
    return reader.Read(command) && context.buildTopLevelAccelStructFromBuffer(
        reinterpret_cast<AccelStruct*>(uintptr_t(command.accelStruct)),
        reinterpret_cast<Buffer*>(uintptr_t(command.instanceBuffer)), command.instanceBufferOffset,
        command.numInstances, RhiRtAccelStructBuildFlags(command.buildFlags));
}
} // namespace

bool ReplayCommandStream(const void* data, ID3D12GraphicsCommandList* commandList, uint32_t fenceValue)
{
    Device* device = Device::Get();
    if (!device)
        return false;
    ReplayContext context(&device->context());
    return ReplayCommandStream(data, commandList, fenceValue, context);
}

namespace
{
struct CommandMarkerProfileGuard
{
    CommandMarkerProfileGuard() { CloseCommandMarkerProfiles(); }
    ~CommandMarkerProfileGuard() { CloseCommandMarkerProfiles(); }
};

void ResetReplayState(ReplayContext& context)
{
    context.d3dBarriers.clear();
    context.d3dTextureBarriers.clear();
    context.d3dBufferBarriers.clear();
    context.enableAutomaticBarriers = true;
    context.bindingStatesDirty = false;
    context.currentPipeline = nullptr;
    context.currentIndirectParams = nullptr;
    context.currentBindings.clear();
    context.currentComputeVolatileCBs.clear();
    context.volatileConstantBufferAddresses.clear();
    context.anyVolatileBufferWrites = false;
    context.currentGraphicsPipeline = nullptr;
    context.currentFramebuffer = nullptr;
    context.currentGraphicsIndirectParams = nullptr;
    context.currentGraphicsIndirectCountBuffer = nullptr;
    context.currentGraphicsVolatileCBs.clear();
    context.graphicsStateActive = false;
    context.currentShaderTable = nullptr;
    context.currentRayTracingBindings.clear();
    context.uncachedShaderTableStates.clear();
    context.rayTracingStateActive = false;
    context.decodedBindings.clear();
    context.decodedVertexBuffers.clear();
    context.decodedGeometries.clear();
    context.decodedInstances.clear();
    context.currentHeapSRVetc = nullptr;
    context.currentHeapSamplers = nullptr;
}
} // namespace

bool ReplayCommandStream(const void* data, ID3D12GraphicsCommandList* commandList, uint32_t fenceValue,
    ReplayContext& context)
{
    CommandMarkerProfileGuard commandMarkerProfileGuard;
    if (!data || !commandList)
        return false;

    auto* header = static_cast<const CommandStreamHeader*>(data);
    if (header->magic != kCommandStreamMagic || header->version != kCommandStreamVersion ||
        header->byteSize < sizeof(CommandStreamHeader))
    {
        LogError("[UnityRHI] Invalid command stream header.");
        return false;
    }

    Device* device = Device::Get();
    if (!device)
        return false;

    const auto* begin = reinterpret_cast<const uint8_t*>(data) + sizeof(CommandStreamHeader);
    const auto* end = reinterpret_cast<const uint8_t*>(data) + header->byteSize;
    Reader reader(begin, end);
    ID3D12GraphicsCommandList* previousCommandList = context.commandList;
    ResetReplayState(context);
    context.commandList = commandList;
    context.fenceValue = fenceValue;
    // DXR commands require ID3D12GraphicsCommandList4. Acquire it once for
    // the replay context; non-DXR devices leave this null and the individual
    // ray-tracing methods report failure when invoked.
    if (previousCommandList != commandList)
    {
        context.commandList4.Reset();
        context.commandList7.Reset();
    }
    if (device->GetD3DDevice5() && !context.commandList4)
    {
        const HRESULT hr = commandList->QueryInterface(IID_PPV_ARGS(&context.commandList4));
        if (FAILED(hr))
            LogError("[UnityRHI] ID3D12GraphicsCommandList4 is unavailable (hr=0x%08X).",
                static_cast<unsigned>(hr));
    }
    if (device->GetEnhancedBarriersSupported() && !context.commandList7)
    {
        const HRESULT hr = commandList->QueryInterface(IID_PPV_ARGS(&context.commandList7));
        if (FAILED(hr))
            LogError("[UnityRHI] ID3D12GraphicsCommandList7 is unavailable (hr=0x%08X); enhanced barriers cannot be recorded.",
                static_cast<unsigned>(hr));
    }

    Breadcrumbs::Get().Push("replay begin: %u command(s), fence=%llu",
        header->commandCount, static_cast<unsigned long long>(fenceValue));

    for (uint32_t i = 0; i < header->commandCount; ++i)
    {
        context.diagnosticCommandIndex = i;
        CommandOpcode opcode{};
        if (!reader.Read(opcode))
            return false;

        bool ok = false;
        switch (opcode)
        {
        case CommandOpcode::CopyBuffer:
            ok = ExecuteCopyBuffer(reader, context);
            break;
        case CommandOpcode::SetComputeState:
            ok = ExecuteSetComputeState(reader, context);
            break;
        case CommandOpcode::Dispatch:
        {
            NativeProfileScope profile("UnityRHI.Diagnostic.Dispatch");
            wire::DispatchPayload command{};
            ok = reader.Read(command);
            if (ok)
            {
                Breadcrumbs::Get().PushDispatch(
                    command.x, command.y, command.z,
                    context.currentPipeline ? context.currentPipeline->debugName.c_str() : "?");
                context.dispatch(command.x, command.y, command.z);
            }
            break;
        }
        case CommandOpcode::BeginMarker:
            ok = ExecuteBeginMarker(reader, context);
            break;
        case CommandOpcode::EndMarker:
            EndCommandMarkerProfile();
            {
                NativeProfileScope profile("UnityRHI.D3D12EndEvent");
                context.commandList->EndEvent();
            }
            ok = true;
            break;
        case CommandOpcode::SetEnableAutomaticBarriers:
        {
            wire::UInt32Payload command{};
            ok = reader.Read(command);
            context.enableAutomaticBarriers = command.value != 0;
            break;
        }
        case CommandOpcode::SetBufferState:
            ok = ExecuteSetBufferState(reader, context);
            break;
        case CommandOpcode::DispatchIndirect:
            ok = ExecuteDispatchIndirect(reader, context);
            break;
        case CommandOpcode::UavBarrier:
            ok = ExecuteUavBarrier(reader, context);
            break;
        case CommandOpcode::ClearBufferUInt:
            ok = ExecuteClearBufferUInt(reader, context);
            break;
        case CommandOpcode::WriteBuffer:
            ok = ExecuteWriteBuffer(reader, context);
            break;
        case CommandOpcode::SetPushConstants:
            ok = ExecuteSetPushConstants(reader, context);
            break;
        case CommandOpcode::SetTextureState:
            ok = ExecuteSetTextureState(reader, context);
            break;
        case CommandOpcode::BuildBottomLevelAccelStruct:
            ok = ExecuteBuildBottomLevelAccelStruct(reader, context);
            break;
        case CommandOpcode::BuildTopLevelAccelStruct:
            ok = ExecuteBuildTopLevelAccelStruct(reader, context);
            break;
        case CommandOpcode::BuildTopLevelAccelStructFromBuffer:
            ok = ExecuteBuildTopLevelAccelStructFromBuffer(reader, context);
            break;
        case CommandOpcode::SetRayTracingState:
            ok = ExecuteSetRayTracingState(reader, context);
            break;
        case CommandOpcode::DispatchRays:
            ok = ExecuteDispatchRays(reader, context);
            break;
        case CommandOpcode::WriteTexture:
            ok = ExecuteWriteTexture(reader, context);
            break;
        case CommandOpcode::CopyTextureToBuffer:
            ok = ExecuteCopyTextureToBuffer(reader, context);
            break;
        case CommandOpcode::BeginTrackingTextureState:
            ok = ExecuteBeginTrackingTextureState(reader, context);
            break;
        case CommandOpcode::BeginTrackingBufferState:
            ok = ExecuteBeginTrackingBufferState(reader, context);
            break;
        case CommandOpcode::ClearState:
            context.clearState();
            ok = true;
            break;
        case CommandOpcode::CommitBarriers:
            context.commitBarriers();
            ok = true;
            break;
        case CommandOpcode::SetPermanentTextureState:
        case CommandOpcode::SetPermanentBufferState:
        case CommandOpcode::SetEnableUavBarriersForTexture:
        case CommandOpcode::SetEnableUavBarriersForBuffer:
            ok = ExecuteResourceStateToggle(reader, context, opcode);
            break;
        case CommandOpcode::SetTextureSubresourceState:
            ok = ExecuteSetTextureSubresourceState(reader, context);
            break;
        case CommandOpcode::CopyTexture:
            ok = ExecuteCopyTexture(reader, context);
            break;
        case CommandOpcode::CopyTextureFromStaging:
        case CommandOpcode::CopyTextureToStaging:
            ok = ExecuteCopyStagingTexture(reader, context, opcode);
            break;
        case CommandOpcode::BeginTimerQuery:
        case CommandOpcode::EndTimerQuery:
            ok = ExecuteTimerQuery(reader, context, opcode);
            break;
        case CommandOpcode::ResolveTexture:
            ok = ExecuteResolveTexture(reader, context);
            break;
        case CommandOpcode::SetGraphicsState:
            ok = ExecuteSetGraphicsState(reader, context);
            break;
        case CommandOpcode::Draw:
            ok = ExecuteDraw(reader, context);
            break;
        case CommandOpcode::DrawIndexed:
            ok = ExecuteDrawIndexed(reader, context);
            break;
        case CommandOpcode::DrawIndirect:
        case CommandOpcode::DrawIndexedIndirect:
        case CommandOpcode::DrawIndexedIndirectCount:
            ok = ExecuteDrawIndirect(reader, context, opcode);
            break;
        case CommandOpcode::ClearTextureFloat:
            ok = ExecuteClearTextureFloat(reader, context);
            break;
        case CommandOpcode::ClearDepthStencilTexture:
            ok = ExecuteClearDepthStencilTexture(reader, context);
            break;
        case CommandOpcode::ClearTextureFloatSubresources:
            ok = ExecuteClearTextureFloatSubresources(reader, context);
            break;
        case CommandOpcode::ClearDepthStencilTextureSubresources:
            ok = ExecuteClearDepthStencilTextureSubresources(reader, context);
            break;
        case CommandOpcode::ClearTextureUInt:
            ok = ExecuteClearTextureUInt(reader, context);
            break;
        case CommandOpcode::DispatchDlrr:
        {
            wire::DlrrDispatchPayload command{};
            ok = reader.Read(command);
            if (ok)
            {
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.input)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.motionVectors)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.depth)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.diffuseAlbedo)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.specularAlbedo)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.normalRoughness)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.specularMotion)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.output)),
                    ResourceStates::UnorderedAccess);
                context.commitBarriers();
                ok = DispatchDlrr(command, context.commandList);
                if (ok)
                    context.clearState();
            }
            break;
        }
        case CommandOpcode::DispatchDlssNr:
        {
            wire::DlssNrDispatchPayload command{};
            ok = reader.Read(command);
            if (ok)
            {
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.color)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.motionVectors)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.depth)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.output)),
                    ResourceStates::UnorderedAccess);
                context.commitBarriers();
                ok = DispatchDlssNr(command, context.commandList);
                if (ok)
                    context.clearState();
            }
            break;
        }
        case CommandOpcode::DispatchDlss:
        {
            wire::DlssDispatchPayload command{};
            ok = reader.Read(command);
            if (ok)
            {
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.input)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.motionVectors)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.depth)),
                    ResourceStates::ShaderResource);
                context.setTextureState(
                    reinterpret_cast<Texture*>(uintptr_t(command.output)),
                    ResourceStates::UnorderedAccess);
                context.commitBarriers();
                ok = DispatchDlss(command, context.commandList);
                if (ok)
                    context.clearState();
            }
            break;
        }
        default:
            LogError("[UnityRHI] Unknown command opcode %u.", uint32_t(opcode));
            return false;
        }

        if (!ok)
        {
            LogError("[UnityRHI] Failed to replay command %u (opcode %u).", i, uint32_t(opcode));
            Breadcrumbs::Get().Push("replay FAILED at command %u (opcode %u)", i, uint32_t(opcode));
            return false;
        }
    }

    // Port of nvrhi CommandList::close() + executed(): restore keepInitialState
    // resources, flush the pending barriers, then commit permanent states and
    // persist the final tracked states for the next stream.
    {
        NativeProfileScope profile("UnityRHI.FinalizeReplay");
        {
            NativeProfileScope step("UnityRHI.KeepBufferInitialStates");
            context.stateTracker.keepBufferInitialStates();
        }
        {
            NativeProfileScope step("UnityRHI.KeepTextureInitialStates");
            context.stateTracker.keepTextureInitialStates();
        }
        {
            NativeProfileScope step("UnityRHI.CommitFinalBarriers");
            context.commitBarriers();
        }
        {
            NativeProfileScope step("UnityRHI.StateTrackerSubmitted");
            context.stateTracker.commandListSubmitted();
        }
    }

    Breadcrumbs::Get().Push("replay end");
    // Match NVRHI close()/executed(): discard per-list state while retaining
    // vector capacities and unordered-map buckets for the next replay.
    ResetReplayState(context);
    return true;
}

namespace
{
// Canonical payload layout per opcode, independent of the replay functions.
// Returns false on truncation or malformed variable-length fields.
bool SkipCommandPayload(Reader& reader, CommandOpcode opcode)
{
    switch (opcode)
    {
    case CommandOpcode::CopyBuffer: // dst, dstOffset, src, srcOffset, byteSize
        return reader.Skip(sizeof(wire::CopyBufferPayload));
    case CommandOpcode::SetComputeState: // pipeline, indirectParams, count, flags, optional count*set
    {
        wire::StatePayload command{};
        if (!reader.Read(command))
            return false;
        if ((command.flags & ~wire::StateFlagReuseBindings) != 0)
            return false;
        return (command.flags & wire::StateFlagReuseBindings) != 0 ||
            reader.SkipSpan<uint64_t>(command.bindingCount);
    }
    case CommandOpcode::Dispatch: // x, y, z
        return reader.Skip(sizeof(wire::DispatchPayload));
    case CommandOpcode::BeginMarker: // length + utf8 bytes
    {
        wire::MarkerPayload command{};
        return reader.Read(command) && reader.Skip(command.byteSize);
    }
    case CommandOpcode::EndMarker:
        return true;
    case CommandOpcode::SetEnableAutomaticBarriers: // enable
        return reader.Skip(sizeof(wire::UInt32Payload));
    case CommandOpcode::SetBufferState:  // buffer, state
    case CommandOpcode::SetTextureState: // texture, state
    case CommandOpcode::BeginTrackingBufferState: // buffer, state
    case CommandOpcode::SetPermanentTextureState: // texture, state
    case CommandOpcode::SetPermanentBufferState: // buffer, state
    case CommandOpcode::SetEnableUavBarriersForTexture: // texture, enable
    case CommandOpcode::SetEnableUavBarriersForBuffer: // buffer, enable
    case CommandOpcode::ClearBufferUInt: // buffer, clearValue
        return reader.Skip(sizeof(wire::HandleUInt32Payload));
    case CommandOpcode::BeginTrackingTextureState: // texture, subresources, state
    case CommandOpcode::SetTextureSubresourceState: // texture, subresources, state
        return reader.Skip(sizeof(wire::TextureSubresourceStatePayload));
    case CommandOpcode::ClearState:
    case CommandOpcode::CommitBarriers:
        return true;
    case CommandOpcode::CopyTexture:
    case CommandOpcode::CopyTextureFromStaging:
    case CommandOpcode::CopyTextureToStaging:
        return reader.Skip(sizeof(wire::TextureCopyPayload));
    case CommandOpcode::ResolveTexture:
        return reader.Skip(sizeof(wire::ResolveTexturePayload));
    case CommandOpcode::DispatchIndirect: // offset
    case CommandOpcode::UavBarrier:       // resource
    case CommandOpcode::BeginTimerQuery:  // query
    case CommandOpcode::EndTimerQuery:    // query
        return reader.Skip(sizeof(wire::HandlePayload));
    case CommandOpcode::WriteBuffer: // buffer, destOffset, upload ticket
        return reader.Skip(sizeof(wire::WriteBufferPayload));
    case CommandOpcode::SetPushConstants: // size + data bytes
    {
        wire::MarkerPayload command{};
        return reader.Read(command) && reader.Skip(command.byteSize);
    }
    case CommandOpcode::BuildBottomLevelAccelStruct:
    {
        wire::AccelStructBuildPayload command{};
        if (!reader.Read(command))
            return false;
        return reader.SkipSpan<RhiRtGeometryDesc>(command.elementCount);
    }
    case CommandOpcode::BuildTopLevelAccelStruct:
    {
        wire::AccelStructBuildPayload command{};
        if (!reader.Read(command))
            return false;
        return reader.SkipSpan<RhiRtInstanceDesc>(command.elementCount);
    }
    case CommandOpcode::BuildTopLevelAccelStructFromBuffer:
        return reader.Skip(sizeof(wire::TlasFromBufferPayload));
    case CommandOpcode::SetRayTracingState:
    {
        wire::BindingStatePayload command{};
        if (!reader.Read(command))
            return false;
        return reader.SkipSpan<uint64_t>(command.bindingCount);
    }
    case CommandOpcode::DispatchRays:
        return reader.Skip(sizeof(wire::DispatchPayload));
    case CommandOpcode::WriteTexture: // texture, arraySlice, mip, upload ticket
        return reader.Skip(sizeof(wire::WriteTexturePayload));
    case CommandOpcode::CopyTextureToBuffer: // buffer, offset, texture, arraySlice, mip
        return reader.Skip(sizeof(wire::CopyTextureToBufferPayload));
    case CommandOpcode::SetGraphicsState:
    {
        wire::GraphicsStatePayload command{};
        if (!reader.Read(command) ||
            !reader.SkipSpan<wire::VertexBufferBinding>(command.vertexBufferCount))
            return false;
        uint32_t bindingCount = 0;
        if (!reader.Read(bindingCount))
            return false;
        return reader.SkipSpan<uint64_t>(bindingCount);
    }
    case CommandOpcode::Draw: // vertexCount, instanceCount, startVertex, startInstance
        return reader.Skip(sizeof(wire::DrawPayload));
    case CommandOpcode::DrawIndexed: // indexCount, instanceCount, startIndex, baseVertex, startInstance
        return reader.Skip(sizeof(wire::DrawIndexedPayload));
    case CommandOpcode::DrawIndirect:
    case CommandOpcode::DrawIndexedIndirect:
        return reader.Skip(sizeof(wire::DrawIndirectPayload));
    case CommandOpcode::DrawIndexedIndirectCount:
        return reader.Skip(sizeof(wire::DrawIndirectCountPayload));
    case CommandOpcode::ClearTextureFloat: // texture, rgba
        return reader.Skip(sizeof(wire::ClearTextureFloatPayload));
    case CommandOpcode::ClearDepthStencilTexture: // texture, clearDepth, depth, clearStencil, stencil
        return reader.Skip(sizeof(wire::ClearDepthStencilPayload));
    case CommandOpcode::ClearTextureFloatSubresources:
        return reader.Skip(sizeof(wire::ClearTextureFloatSubresourcesPayload));
    case CommandOpcode::ClearDepthStencilTextureSubresources:
        return reader.Skip(sizeof(wire::ClearDepthStencilSubresourcesPayload));
    case CommandOpcode::ClearTextureUInt:
        return reader.Skip(sizeof(wire::ClearTextureUIntPayload));
    case CommandOpcode::DispatchDlrr:
        return reader.Skip(sizeof(wire::DlrrDispatchPayload));
    case CommandOpcode::DispatchDlssNr:
        return reader.Skip(sizeof(wire::DlssNrDispatchPayload));
    case CommandOpcode::DispatchDlss:
        return reader.Skip(sizeof(wire::DlssDispatchPayload));
    default:
        return false;
    }
}

} // namespace

bool DecodeCommandStream(const void* data, CommandStreamDecodeInfo& outInfo)
{
    outInfo = CommandStreamDecodeInfo{};
    if (!data)
        return false;

    auto* header = static_cast<const CommandStreamHeader*>(data);
    if (header->magic != kCommandStreamMagic || header->version != kCommandStreamVersion ||
        header->byteSize < sizeof(CommandStreamHeader))
        return false;

    outInfo.byteSize = header->byteSize;
    const auto* begin = reinterpret_cast<const uint8_t*>(data) + sizeof(CommandStreamHeader);
    const auto* end = reinterpret_cast<const uint8_t*>(data) + header->byteSize;
    Reader reader(begin, end);

    for (uint32_t i = 0; i < header->commandCount; ++i)
    {
        const uint8_t* commandBegin = reader.Position();
        CommandOpcode opcode{};
        if (!reader.Read(opcode) || !SkipCommandPayload(reader, opcode))
            return false;
        if (uint32_t(opcode) < CommandStreamDecodeInfo::kMaxOpcode)
        {
            outInfo.opcodeCounts[uint32_t(opcode)]++;
            outInfo.opcodeBytes[uint32_t(opcode)] +=
                static_cast<uint32_t>(reader.Position() - commandBegin);
        }
        outInfo.commandCount++;
    }

    // The writer's byteSize must land exactly on the last payload byte;
    // trailing garbage means the writer and this table disagree.
    if (reader.ReadBytes(1) != nullptr)
        return false;

    outInfo.ok = 1;
    return true;
}

} // namespace unityrhi
