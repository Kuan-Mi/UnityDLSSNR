// Single point of divergence between the Unity 6 and Unity 2021.3 paths.
//
// Unity 6 exposes IUnityGraphicsD3D12v8, whose RequestResourceState /
// NotifyResourceState let a plugin hand Unity-owned resources back to Unity's
// own state tracker. Unity 2021.3.18 tops out at v7 and has neither entry
// point, even though the underlying implementation
// (D3D12CommandList::RequestResourceState) is already present in the binary.
// The two missing calls are routed through UnityInternals2021::Bridge instead.
//
// This selection is made at *runtime*, so one DLL serves both editors:
//
//   * IUnityGraphicsD3D12v7 is a byte-exact prefix of v8 -- identical members
//     in identical order, with v8 appending only the two state calls. So a v7
//     pointer can be stored as a v8 pointer provided the last two members are
//     never touched, which is exactly what kUsingLegacyStateApi guards.
//   * IUnityInterfaces::Get matches on an exact interface GUID and returns
//     null for one it does not know, so asking a 2021.3 editor for v8 cleanly
//     reports "not available" rather than handing back a v7 in disguise.
//   * unity2021::Bridge is header-only and entirely lazy: every static starts
//     null and nothing resolves until Initialize() runs. Compiling it into the
//     Unity 6 build therefore costs nothing and does nothing.
#pragma once

// IUnityGraphicsD3D12.h declares ID3D12*/IDXGISwapChain members without
// including anything itself, so it only compiles if these come first.
#include <d3d12.h>
#include <dxgi.h>

#include "IUnityGraphicsD3D12.h"
#include "UnityInternals2021.h"

namespace unityrhi
{
// Always the widest interface; on 2021.3 this points at a v7 whose trailing two
// members must not be dereferenced. See s_nativeStateApi.
using UnityD3D12Interface = IUnityGraphicsD3D12v8;

// True once AcquireUnityD3D12 has found a real v8, i.e. the resource-state
// calls are part of the public plugin API and the 2021.3 bridge is not needed.
// Set before InitializeUnityStateForwarding runs, which is the only ordering
// this file requires of its caller.
inline bool s_nativeStateApi = false;

inline bool UnityD3D12StateApiIsNative() { return s_nativeStateApi; }

// Acquire the newest D3D12 interface this Unity build offers, preferring the
// public state API and falling back to v7 + the internals bridge.
inline UnityD3D12Interface* AcquireUnityD3D12(IUnityInterfaces* interfaces)
{
    if (!interfaces)
        return nullptr;

    if (IUnityGraphicsD3D12v8* v8 = interfaces->Get<IUnityGraphicsD3D12v8>())
    {
        s_nativeStateApi = true;
        return v8;
    }

    // 2021.3: v7 is layout-compatible with the prefix of v8 that this plugin
    // calls. RequestResourceState/NotifyResourceState are past the end of the
    // real object and are never reached while s_nativeStateApi is false.
    s_nativeStateApi = false;
    return reinterpret_cast<UnityD3D12Interface*>(interfaces->Get<IUnityGraphicsD3D12v7>());
}

// Initialise anything the state-forwarding path needs. With the public v8 API
// this is a no-op; on 2021.3 it binds the internals bridge.
inline bool InitializeUnityStateForwarding()
{
    if (s_nativeStateApi)
        return true;
    return unity2021::Bridge::Initialize();
}

inline const char* UnityStateForwardingDescription()
{
    if (s_nativeStateApi)
        return "IUnityGraphicsD3D12v8";
    return unity2021::Bridge::Available() ? unity2021::Bridge::BuildLabel()
                                          : "<2021 bridge unbound>";
}

// Bind the state-forwarding path to the command list a plugin event records
// into. On 2021.3 this resolves Unity's D3D12CommandList wrapper once for the
// whole event; without it every forwarded state would re-enter
// CommandRecordingState and re-walk the wrapper, which is the dominant cost of
// replaying a frame. Unity 6 addresses resources through the device, so there
// is nothing to bind.
// Returns false when the 2021.3 wrapper could not be resolved, in which case
// state forwarding is a no-op for the rest of the event.
inline bool BeginUnityStateForwardingEvent(ID3D12GraphicsCommandList* unityCommandList)
{
    if (s_nativeStateApi)
    {
        (void)unityCommandList;
        return true;
    }
    return unity2021::Bridge::BeginEvent(unityCommandList);
}

inline void EndUnityStateForwardingEvent()
{
    if (!s_nativeStateApi)
        unity2021::Bridge::EndEvent();
}

// Forward a Unity-owned resource's required state to Unity's tracker.
// `unityCommandList` is the list the caller is recording into; on 2021.3 it
// selects the wrapper bound by BeginUnityStateForwardingEvent.
inline void ForwardRequestResourceState(UnityD3D12Interface* unityD3D12,
    ID3D12GraphicsCommandList* unityCommandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
{
    if (!unityD3D12 || !resource)
        return;
    if (s_nativeStateApi)
    {
        (void)unityCommandList;
        unityD3D12->RequestResourceState(resource, state);
        return;
    }
    unity2021::Bridge::RequestResourceState(unityCommandList, resource, state);
}

// Tell Unity what state we left a resource in. 2021.3 has no direct
// NotifyResourceState equivalent; MarkUAVResource covers the UAV-hazard half of
// it, which is the part that matters for compute dispatches.
inline void ForwardNotifyResourceState(UnityD3D12Interface* unityD3D12,
    ID3D12GraphicsCommandList* unityCommandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
    bool uavAccess)
{
    if (!unityD3D12 || !resource)
        return;
    if (s_nativeStateApi)
    {
        (void)unityCommandList;
        unityD3D12->NotifyResourceState(resource, state, uavAccess);
        return;
    }
    // Re-request the post-state so Unity's tracker agrees with what we left.
    unity2021::Bridge::RequestResourceState(unityCommandList, resource, state);
    if (uavAccess)
        unity2021::Bridge::MarkUAVResource(unityCommandList, resource);
}
} // namespace unityrhi
