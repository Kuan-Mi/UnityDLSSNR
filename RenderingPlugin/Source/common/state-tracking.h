#pragma once

// Per-subresource resource-state tracker ported from NVRHI's
// common/state-tracking.h (External/nvrhi/src/common).
// Copyright (c) 2021, NVIDIA CORPORATION. MIT license
// (see ThirdParty/NVRHI-LICENSE.txt).
//
// UnityRHI deviations:
//  - IMessageCallback is replaced by the backend Context (error()).
//  - The desc types are the C-ABI BufferDesc/TextureDesc; they carry no
//    debugName, so the extension structs hold a pointer to the owning
//    RhiResource's name for error messages.

#include <string>
#include <unordered_map>
#include <vector>

#include "RhiTypes.h"

namespace unityrhi
{
struct Context;

struct BufferStateExtension
{
    const BufferDesc& descRef;
    ResourceStates permanentState = ResourceStates::Unknown;

    const std::string* debugNameRef = nullptr;

    explicit BufferStateExtension(const BufferDesc& desc)
        : descRef(desc)
    { }
};

struct TextureStateExtension
{
    const TextureDesc& descRef;
    ResourceStates permanentState = ResourceStates::Unknown;
    bool stateInitialized = false;
    bool isSamplerFeedback = false; // sampler-feedback textures are not ported

    const std::string* debugNameRef = nullptr;

    explicit TextureStateExtension(const TextureDesc& desc)
        : descRef(desc)
    { }
};

struct TextureState
{
    std::vector<ResourceStates> subresourceStates;
    ResourceStates state = ResourceStates::Unknown;
    bool enableUavBarriers = true;
    bool firstUavBarrierPlaced = false;
    bool permanentTransition = false;
};

struct BufferState
{
    ResourceStates state = ResourceStates::Unknown;
    bool enableUavBarriers = true;
    bool firstUavBarrierPlaced = false;
    bool permanentTransition = false;
};

struct TextureBarrier
{
    TextureStateExtension* texture = nullptr;
    MipLevel mipLevel = 0;
    ArraySlice arraySlice = 0;
    bool entireTexture = false;
    ResourceStates stateBefore = ResourceStates::Unknown;
    ResourceStates stateAfter = ResourceStates::Unknown;
};

struct BufferBarrier
{
    BufferStateExtension* buffer = nullptr;
    ResourceStates stateBefore = ResourceStates::Unknown;
    ResourceStates stateAfter = ResourceStates::Unknown;
};

class CommandListResourceStateTracker
{
public:
    explicit CommandListResourceStateTracker(const Context* context)
        : m_Context(context)
    { }

    // ICommandList-like interface

    void setEnableUavBarriersForTexture(TextureStateExtension* texture, bool enableBarriers);
    void setEnableUavBarriersForBuffer(BufferStateExtension* buffer, bool enableBarriers);

    void beginTrackingTextureState(TextureStateExtension* texture, TextureSubresourceSet subresources, ResourceStates stateBits);
    void beginTrackingBufferState(BufferStateExtension* buffer, ResourceStates stateBits);

    void setPermanentTextureState(TextureStateExtension* texture, TextureSubresourceSet subresources, ResourceStates stateBits);
    void setPermanentBufferState(BufferStateExtension* buffer, ResourceStates stateBits);

    ResourceStates getTextureSubresourceState(TextureStateExtension* texture, ArraySlice arraySlice, MipLevel mipLevel);
    ResourceStates getBufferState(BufferStateExtension* buffer);

    // Internal interface

    void requireTextureState(TextureStateExtension* texture, TextureSubresourceSet subresources, ResourceStates state);
    void requireBufferState(BufferStateExtension* buffer, ResourceStates state);

    void keepBufferInitialStates();
    void keepTextureInitialStates();
    void commandListSubmitted();

    [[nodiscard]] const std::vector<TextureBarrier>& getTextureBarriers() const { return m_TextureBarriers; }
    [[nodiscard]] const std::vector<BufferBarrier>& getBufferBarriers() const { return m_BufferBarriers; }
    void clearBarriers() { m_TextureBarriers.clear(); m_BufferBarriers.clear(); }

private:
    const Context* m_Context;

    // NVRHI stores each state in a separate unique_ptr. Replay creates and
    // clears this table for every submitted stream, so that shape adds one
    // extra heap allocation/free per tracked resource. Values are never kept
    // across map mutations; storing them inline preserves the tracking logic
    // while removing that render-thread allocator traffic.
    std::unordered_map<TextureStateExtension*, TextureState> m_TextureStates;
    std::unordered_map<BufferStateExtension*, BufferState> m_BufferStates;

    // Deferred transitions of textures and buffers to permanent states.
    // They are executed only when the command list is executed, not when the app calls setPermanentTextureState or setPermanentBufferState.
    std::vector<std::pair<TextureStateExtension*, ResourceStates>> m_PermanentTextureStates;
    std::vector<std::pair<BufferStateExtension*, ResourceStates>> m_PermanentBufferStates;

    std::vector<TextureBarrier> m_TextureBarriers;
    std::vector<BufferBarrier> m_BufferBarriers;

    TextureState* getTextureStateTracking(TextureStateExtension* texture, bool allowCreate);
    BufferState* getBufferStateTracking(BufferStateExtension* buffer, bool allowCreate);
};

bool verifyPermanentResourceState(ResourceStates permanentState, ResourceStates requiredState, bool isTexture, const std::string& debugName, const Context* context);

// Derives the state for SRV type resources used in binding sets.
// We don't want to include the PixelShaderResource bit everywhere because it cannot be used in a compute queue.
// So, use the binding layout's visibility mask to determine if a pixel shader might be necessary.
struct BindingLayout;
ResourceStates getShaderResourceStateForBindingLayout(BindingLayout* bindingLayout);

} // namespace unityrhi
