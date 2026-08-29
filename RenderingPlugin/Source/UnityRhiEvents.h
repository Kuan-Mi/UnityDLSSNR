#pragma once

// Render-event IDs shared between the native plugin and the C# runtime
// (mirrored in Packages/com.unityrhi/Runtime/Interop/NativeMethods.cs).
// These IDs are passed to CommandBuffer.IssuePluginEventAndData and arrive
// in OnRenderEvent on Unity's render thread.
enum UnityRhiEventId
{
    // Phase 3+: execute a native-owned CommandSubmission. `data` points to the
    // submission object, which is destroyed after replay. ID 1 is retired;
    // existing IDs must not be renumbered.
    kUnityRhiEvent_ExecuteCommandStream = 2,

    // Configured with queue access + FlushCommandBuffers: Unity submits every
    // command list recorded so far (including replayed command streams), then
    // the plugin signals its sync-point fence on Unity's graphics queue.
    // `data` carries the fence value from UnityRhiCreateSyncPoint. The CPU
    // waits with UnityRhiWaitSyncPoint.
    kUnityRhiEvent_FlushAndSignalSyncPoint = 3,

    // Bracket an external native plugin (for example NRI/DLSS-RR) that binds
    // private descriptor heaps but does not use D3D12HeapHook itself. The
    // begin event prevents those heaps from replacing the cached Unity heaps;
    // the end event restores Unity's heaps on the active command list.
    kUnityRhiEvent_BeginExternalHeapDispatch = 4,
    kUnityRhiEvent_EndExternalHeapDispatch = 5,
};
