// Replay-time resource-state methods ported from NVRHI's
// d3d12-state-tracking.cpp (External/nvrhi/src/d3d12).
// Copyright (c) 2014-2021, NVIDIA CORPORATION. MIT license
// (see ThirdParty/NVRHI-LICENSE.txt).
//
// The nvrhi CommandList methods live here as ReplayContext methods; the
// command-stream replayer (CommandStream.cpp) decodes opcodes and calls them.

#include "d3d12-backend.h"

#include "UnityRhiLog.h"
#include "UnityRhiProfiler.h"

namespace unityrhi
{
std::atomic<uint64_t> g_uavBarrierCount{0};

// Port of nvrhi CommandList::setResourceStatesForBindingSet.
void ReplayContext::setResourceStatesForBindingSet(BindingSet* bindingSet)
{
    ResourceStates const shaderResourceState = getShaderResourceStateForBindingLayout(bindingSet->layout);

    for (uint16_t bindingIndex : bindingSet->bindingsThatNeedTransitions)
    {
        const RhiBindingSetItem& binding = bindingSet->bindings[bindingIndex];
        auto* resource = static_cast<RhiResource*>(binding.resource);
        if (!resource)
            continue;

        switch (binding.type)
        {
        case ResourceType::Texture_SRV:
            requireTextureState(static_cast<Texture*>(resource), binding.subresources(), shaderResourceState);
            break;

        case ResourceType::Texture_UAV:
            requireTextureState(static_cast<Texture*>(resource), binding.subresources(), ResourceStates::UnorderedAccess);
            break;

        case ResourceType::TypedBuffer_SRV:
        case ResourceType::StructuredBuffer_SRV:
        case ResourceType::RawBuffer_SRV:
            requireBufferState(static_cast<Buffer*>(resource), shaderResourceState);
            break;

        case ResourceType::TypedBuffer_UAV:
        case ResourceType::StructuredBuffer_UAV:
        case ResourceType::RawBuffer_UAV:
            requireBufferState(static_cast<Buffer*>(resource), ResourceStates::UnorderedAccess);
            break;

        case ResourceType::ConstantBuffer:
            requireBufferState(static_cast<Buffer*>(resource), ResourceStates::ConstantBuffer);
            break;

        case ResourceType::RayTracingAccelStruct:
        {
            auto* accelStruct = static_cast<AccelStruct*>(resource);
            requireBufferState(accelStruct->dataBuffer, ResourceStates::AccelStructRead);
            if (accelStruct->dataBuffer)
                accelStruct->dataBuffer->lastUseFenceValue = fenceValue;
            break;
        }

        default:
            // do nothing
            break;
        }

        resource->lastUseFenceValue = fenceValue;
    }
}

// Port of nvrhi CommandList::requireTextureState. UnityRHI deviation:
// resources imported from Unity are owned by Unity's own state tracker, so
// their state requests are forwarded to it immediately instead of entering
// our tracker. The request is repeated even when our knowledge says the state
// matches, because URP may have changed it between plugin events.
void ReplayContext::requireTextureState(Texture* texture, TextureSubresourceSet subresources, ResourceStates state)
{
    if (!texture)
        return;

    if (texture->unityOwnedResource)
    {
        Device* device = Device::Get();
        if (device && texture->resource)
        {
            const D3D12_RESOURCE_STATES after = convertResourceStates(state);
            device->RequestResourceState(commandList, texture->resource.Get(), after);
        }
        return;
    }

    stateTracker.requireTextureState(texture, subresources, state);
}

// Port of nvrhi CommandList::requireBufferState (same deviation as above).
void ReplayContext::requireBufferState(Buffer* buffer, ResourceStates state)
{
    if (!buffer)
        return;

    if (buffer->unityOwnedResource)
    {
        Device* device = Device::Get();
        if (device && buffer->resource)
        {
            const D3D12_RESOURCE_STATES after = convertResourceStates(state);
            device->RequestResourceState(commandList, buffer->resource.Get(), after);
        }
        return;
    }

    stateTracker.requireBufferState(buffer, state);
}

// Port of nvrhi CommandList::commitBarriers. Plugin-owned resources use the
// same enhanced-barrier path as RTXPT when the device and Unity command list
// expose the required interfaces. Unity-owned resources never enter this
// tracker and remain under Unity's resource-state API.
void ReplayContext::commitBarriers()
{
    NativeProfileScope profile("UnityRHI.CommitBarriers");
    const auto& textureBarriers = stateTracker.getTextureBarriers();
    const auto& bufferBarriers = stateTracker.getBufferBarriers();
    const size_t barrierCount = textureBarriers.size() + bufferBarriers.size();
    if (barrierCount == 0)
        return;

    Device* device = Device::Get();
    if (device && device->GetEnhancedBarriersSupported() && commandList7)
    {
        d3dTextureBarriers.clear();
        d3dBufferBarriers.clear();
        d3dTextureBarriers.reserve(textureBarriers.size());
        d3dBufferBarriers.reserve(bufferBarriers.size());

        for (const auto& barrier : textureBarriers)
        {
            const auto* texture = static_cast<const Texture*>(barrier.texture);
            const EnhancedResourceStateMapping before =
                convertResourceStatesForEnhancedBarriers(barrier.stateBefore, true);
            const EnhancedResourceStateMapping after =
                convertResourceStatesForEnhancedBarriers(barrier.stateAfter, true);

            D3D12_TEXTURE_BARRIER d3dBarrier{};
            d3dBarrier.SyncBefore = before.sync;
            d3dBarrier.SyncAfter = after.sync;
            d3dBarrier.AccessBefore = before.access;
            d3dBarrier.AccessAfter = after.access;
            d3dBarrier.LayoutBefore = before.layout;
            d3dBarrier.LayoutAfter = after.layout;
            d3dBarrier.pResource = texture->resource.Get();
            if (barrier.entireTexture)
            {
                d3dBarrier.Subresources.NumArraySlices = texture->desc.arraySize;
                d3dBarrier.Subresources.NumMipLevels = texture->desc.mipLevels;
            }
            else
            {
                d3dBarrier.Subresources.FirstArraySlice = barrier.arraySlice;
                d3dBarrier.Subresources.IndexOrFirstMipLevel = barrier.mipLevel;
                d3dBarrier.Subresources.NumArraySlices = 1;
                d3dBarrier.Subresources.NumMipLevels = 1;
            }
            d3dBarrier.Subresources.NumPlanes = texture->planeCount;
            d3dTextureBarriers.push_back(d3dBarrier);

            if (barrier.stateBefore == barrier.stateAfter &&
                (barrier.stateAfter & ResourceStates::UnorderedAccess) != 0)
                g_uavBarrierCount.fetch_add(1, std::memory_order_relaxed);
        }

        for (const auto& barrier : bufferBarriers)
        {
            const auto* buffer = static_cast<const Buffer*>(barrier.buffer);
            const EnhancedResourceStateMapping before =
                convertResourceStatesForEnhancedBarriers(barrier.stateBefore, false);
            const EnhancedResourceStateMapping after =
                convertResourceStatesForEnhancedBarriers(barrier.stateAfter, false);

            D3D12_BUFFER_BARRIER d3dBarrier{};
            d3dBarrier.SyncBefore = before.sync;
            d3dBarrier.SyncAfter = after.sync;
            d3dBarrier.AccessBefore = before.access;
            d3dBarrier.AccessAfter = after.access;
            d3dBarrier.pResource = buffer->resource.Get();
            d3dBarrier.Size = buffer->desc.byteSize;
            d3dBufferBarriers.push_back(d3dBarrier);

            if (barrier.stateBefore == barrier.stateAfter &&
                (barrier.stateAfter & ResourceStates::UnorderedAccess) != 0)
                g_uavBarrierCount.fetch_add(1, std::memory_order_relaxed);
        }

        D3D12_BARRIER_GROUP groups[2]{};
        uint32_t groupCount = 0;
        if (!d3dTextureBarriers.empty())
        {
            auto& group = groups[groupCount++];
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = uint32_t(d3dTextureBarriers.size());
            group.pTextureBarriers = d3dTextureBarriers.data();
        }
        if (!d3dBufferBarriers.empty())
        {
            auto& group = groups[groupCount++];
            group.Type = D3D12_BARRIER_TYPE_BUFFER;
            group.NumBarriers = uint32_t(d3dBufferBarriers.size());
            group.pBufferBarriers = d3dBufferBarriers.data();
        }

        commandList7->Barrier(groupCount, groups);
        stateTracker.clearBarriers();
        return;
    }

    // Allocate vector space for the barriers assuming 1:1 translation.
    // For partial transitions on multi-plane textures, original barriers may translate
    // into more than 1 barrier each, but that's relatively rare.
    d3dBarriers.clear();
    d3dBarriers.reserve(barrierCount);

    // Convert the texture barriers into D3D equivalents
    for (const auto& barrier : textureBarriers)
    {
        const Texture* texture = static_cast<const Texture*>(barrier.texture);
        ID3D12Resource* resource = texture->resource.Get();

        D3D12_RESOURCE_BARRIER d3dbarrier{};
        const D3D12_RESOURCE_STATES stateBefore = convertResourceStates(barrier.stateBefore);
        const D3D12_RESOURCE_STATES stateAfter = convertResourceStates(barrier.stateAfter);
        if (stateBefore != stateAfter)
        {
            d3dbarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            d3dbarrier.Transition.StateBefore = stateBefore;
            d3dbarrier.Transition.StateAfter = stateAfter;
            d3dbarrier.Transition.pResource = resource;
            if (barrier.entireTexture)
            {
                d3dbarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                d3dBarriers.push_back(d3dbarrier);
            }
            else
            {
                for (uint8_t plane = 0; plane < texture->planeCount; plane++)
                {
                    d3dbarrier.Transition.Subresource = calcSubresource(barrier.mipLevel, barrier.arraySlice, plane, texture->desc.mipLevels, texture->desc.arraySize);
                    d3dBarriers.push_back(d3dbarrier);
                }
            }
        }
        else if (stateAfter & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            d3dbarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            d3dbarrier.UAV.pResource = resource;
            d3dBarriers.push_back(d3dbarrier);
            g_uavBarrierCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Convert the buffer barriers into D3D equivalents
    for (const auto& barrier : bufferBarriers)
    {
        const Buffer* buffer = static_cast<const Buffer*>(barrier.buffer);

        D3D12_RESOURCE_BARRIER d3dbarrier{};
        const D3D12_RESOURCE_STATES stateBefore = convertResourceStates(barrier.stateBefore);
        const D3D12_RESOURCE_STATES stateAfter = convertResourceStates(barrier.stateAfter);
        if (stateBefore != stateAfter &&
            (stateBefore & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) == 0 &&
            (stateAfter & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) == 0)
        {
            d3dbarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            d3dbarrier.Transition.StateBefore = stateBefore;
            d3dbarrier.Transition.StateAfter = stateAfter;
            d3dbarrier.Transition.pResource = buffer->resource.Get();
            d3dbarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            d3dBarriers.push_back(d3dbarrier);
        }
        else if ((barrier.stateBefore == ResourceStates::AccelStructWrite && (barrier.stateAfter & (ResourceStates::AccelStructRead | ResourceStates::AccelStructBuildBlas)) != 0) ||
            (barrier.stateAfter == ResourceStates::AccelStructWrite && (barrier.stateBefore & (ResourceStates::AccelStructRead | ResourceStates::AccelStructBuildBlas)) != 0) ||
            (barrier.stateBefore == ResourceStates::OpacityMicromapWrite && (barrier.stateAfter & ResourceStates::AccelStructBuildInput) != 0) ||
            (stateAfter & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0)
        {
            d3dbarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            d3dbarrier.UAV.pResource = buffer->resource.Get();
            d3dBarriers.push_back(d3dbarrier);
            g_uavBarrierCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (d3dBarriers.size() > 0)
        commandList->ResourceBarrier(uint32_t(d3dBarriers.size()), d3dBarriers.data());

    stateTracker.clearBarriers();
}

void ReplayContext::setEnableUavBarriersForTexture(Texture* texture, bool enable)
{
    if (texture && !texture->unityOwnedResource)
        stateTracker.setEnableUavBarriersForTexture(texture, enable);
}

void ReplayContext::setEnableUavBarriersForBuffer(Buffer* buffer, bool enable)
{
    if (buffer && !buffer->unityOwnedResource)
        stateTracker.setEnableUavBarriersForBuffer(buffer, enable);
}

void ReplayContext::beginTrackingTextureState(Texture* texture,
    TextureSubresourceSet subresources, ResourceStates state)
{
    stateTracker.beginTrackingTextureState(texture, subresources, state);
}

void ReplayContext::beginTrackingBufferState(Buffer* buffer, ResourceStates state)
{
    stateTracker.beginTrackingBufferState(buffer, state);
}

// Port of nvrhi CommandList::setTextureState (same deviation as setBufferState).
void ReplayContext::setTextureState(Texture* texture, ResourceStates state)
{
    setTextureState(texture, AllSubresources, state);
}

void ReplayContext::setTextureState(Texture* texture, TextureSubresourceSet subresources, ResourceStates state)
{
    if (!texture)
        return;
    const char* rejected = nullptr;
    if ((state & ResourceStates::UnorderedAccess) != 0 && !texture->desc.isUAV)
        rejected = "UnorderedAccess (created without isUAV)";
    else if ((state & ResourceStates::RenderTarget) != 0 && !texture->desc.isRenderTarget)
        rejected = "RenderTarget (created without isRenderTarget)";
    else if ((state & (ResourceStates::DepthWrite | ResourceStates::DepthRead)) != 0 &&
        !texture->desc.isRenderTarget)
        rejected = "DepthWrite/DepthRead (created without isRenderTarget)";
    else if ((state & ResourceStates::ShaderResource) != 0 && !texture->desc.isShaderResource)
        rejected = "ShaderResource (created without isShaderResource)";
    if (rejected)
    {
        LogError("[UnityRHI] SetTextureState: texture '%s': ignoring illegal state request %s.",
            texture->debugName.c_str(), rejected);
        return;
    }

    requireTextureState(texture, subresources, state);
    texture->lastUseFenceValue = fenceValue;
}

// Port of nvrhi CommandList::setBufferState: the state is required but the
// barrier is committed by the next command that commits barriers.
// UnityRHI deviation: D3D12 rejects barriers into states the resource was not
// created for; recording one anyway fails commandList->Close() and poisons
// the whole submission, so illegal requests are dropped instead.
void ReplayContext::setBufferState(Buffer* buffer, ResourceStates state)
{
    if (!buffer)
        return;
    if ((state & ResourceStates::UnorderedAccess) != 0 && !buffer->desc.canHaveUAVs)
    {
        LogError("[UnityRHI] SetBufferState: buffer '%s' was created without canHaveUAVs; "
                 "ignoring the UnorderedAccess state request.",
            buffer->debugName.c_str());
        return;
    }

    requireBufferState(buffer, state);
    buffer->lastUseFenceValue = fenceValue;
}

void ReplayContext::setPermanentTextureState(Texture* texture, ResourceStates state)
{
    if (!texture)
        return;
    if (texture->unityOwnedResource)
    {
        LogError("[UnityRHI] SetPermanentTextureState cannot be used with a Unity-owned texture.");
        return;
    }
    stateTracker.setPermanentTextureState(texture, AllSubresources, state);
    texture->lastUseFenceValue = fenceValue;
}

void ReplayContext::setPermanentBufferState(Buffer* buffer, ResourceStates state)
{
    if (!buffer)
        return;
    if (buffer->unityOwnedResource)
    {
        LogError("[UnityRHI] SetPermanentBufferState cannot be used with a Unity-owned buffer.");
        return;
    }
    stateTracker.setPermanentBufferState(buffer, state);
    buffer->lastUseFenceValue = fenceValue;
}

} // namespace unityrhi
