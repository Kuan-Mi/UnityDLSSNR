// UnityRHI native plugin entry point.
//
// All state lives in a single resettable struct because the editor never
// unloads native DLLs: UnityPluginLoad/Unload can run many times per process.

#include <directx/d3d12.h>
#include <dxgi.h>
#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D12.h"
#include "IUnityInterface.h"
#include "IUnityLog.h"

#include "d3d12/AgilitySdk.h"
#include "d3d12/d3d12-backend.h"
#include "d3d12/GpuDumps.h"
#include "CommandStream.h"
#include "CommandSubmission.h"
#include "Dlrr.h"
#include "Dlss.h"
#include "DlssNr.h"
#include "DlssNrRuntime.h"
#include "FrameGenerationDebug.h"
#include "FrameGenerationHooks.h"
#include "NgxRuntime.h"
#include "UnityRhiEvents.h"
#include "UnityRhiLog.h"
#include "UnityRhiProfiler.h"

namespace unityrhi
{
IUnityLog* g_Log = nullptr;

namespace
{
// D3D12HeapHook.dll is shared with NRIPlugin. Resolve its stable export
// ordinals dynamically so both plugins use the same vtable hook/cache without
// adding a second hook implementation to UnityRHI.
namespace HeapHook
{
using InstallFn = bool (*)(ID3D12Device*);
using DispatchFn = void (*)();
using RestoreFn = void (*)(ID3D12GraphicsCommandList*);

InstallFn install = nullptr;
DispatchFn begin = nullptr;
DispatchFn end = nullptr;
RestoreFn restore = nullptr;

template <typename T>
T ExportByOrdinal(HMODULE module, WORD ordinal)
{
    return reinterpret_cast<T>(GetProcAddress(module, MAKEINTRESOURCEA(ordinal)));
}

bool Initialize(ID3D12Device* device)
{
    HMODULE module = GetModuleHandleW(L"D3D12HeapHook.dll");
    if (!module)
    {
        // Unity loads native plugins by absolute path without adding the
        // package directory to the process DLL search path. Resolve the hook
        // next to this module explicitly.
        HMODULE self = nullptr;
        wchar_t selfPath[MAX_PATH] = {};
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&Initialize), &self) &&
            GetModuleFileNameW(self, selfPath, MAX_PATH) != 0)
        {
            std::wstring hookPath(selfPath);
            const size_t slash = hookPath.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                hookPath.resize(slash + 1);
                hookPath += L"D3D12HeapHook.dll";
                module = LoadLibraryW(hookPath.c_str());
            }
        }
    }
    if (!module)
    {
        LogError("[UnityRHI] Failed to load D3D12HeapHook.dll (error=%lu).",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    // Export ordinals from the shared hook: Begin=1, End=2, Install=3,
    // Restore=4. Ordinals avoid coupling to MSVC C++ decorated names.
    begin = ExportByOrdinal<DispatchFn>(module, 1);
    end = ExportByOrdinal<DispatchFn>(module, 2);
    install = ExportByOrdinal<InstallFn>(module, 3);
    restore = ExportByOrdinal<RestoreFn>(module, 4);
    if (!begin || !end || !install || !restore)
    {
        LogError("[UnityRHI] D3D12HeapHook.dll has an incompatible export table.");
        begin = nullptr;
        end = nullptr;
        install = nullptr;
        restore = nullptr;
        return false;
    }

    if (!install(device))
    {
        LogError("[UnityRHI] D3D12 descriptor-heap hook installation failed.");
        return false;
    }
    return true;
}
} // namespace HeapHook

struct PluginState
{
    IUnityInterfaces* interfaces = nullptr;
    IUnityGraphics* graphics = nullptr;
    unityrhi::UnityD3D12Interface* d3d12 = nullptr;

    bool d3d12Active = false;
    bool eventsConfigured = false;
    // Render-thread-owned and reused across streams. On a failed replay it is
    // discarded so partially decoded/tracked state cannot leak forward.
    std::unique_ptr<ReplayContext> replayContext;

    // Written on the render thread, read from C# via exports.
    std::atomic<uint64_t> commandStreamEventCount{0};
    std::atomic<uint64_t> droppedCommandStreamCount{0};
    std::atomic<bool> loggedRecordingUnavailable{false};
    std::atomic<bool> loggedStateForwardingUnbound{false};
    std::atomic<bool> tracedCommandListDevice{false};

    void Reset()
    {
        replayContext.reset();
        interfaces = nullptr;
        graphics = nullptr;
        d3d12 = nullptr;
        d3d12Active = false;
        eventsConfigured = false;
        commandStreamEventCount.store(0);
        droppedCommandStreamCount.store(0);
        loggedRecordingUnavailable.store(false);
        loggedStateForwardingUnbound.store(false);
        tracedCommandListDevice.store(false);
    }
};

PluginState g_State;

bool IsStandalonePlayer()
{
    wchar_t executable[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, executable, MAX_PATH) == 0)
        return false;
    const wchar_t* name = executable;
    if (const wchar_t* slash = wcsrchr(executable, L'\\'))
        name = slash + 1;
    return _wcsicmp(name, L"Unity.exe") != 0;
}

void ConfigureRenderEvents()
{
    if (g_State.eventsConfigured || !g_State.d3d12)
        return;

    // DontCare enables CommandRecordingState and guarantees our callbacks run
    // on the render thread. ModifiesCommandBuffersState tells Unity we dirty
    // the command list state (PSO, root signature, descriptor heaps) so it
    // rebinds its own state after our event.
    UnityD3D12PluginEventConfig config{};
    config.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
    config.flags = kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
    config.ensureActiveRenderTextureIsBound = false;

    g_State.d3d12->ConfigureEvent(kUnityRhiEvent_ExecuteCommandStream, &config);
    g_State.d3d12->ConfigureEvent(kUnityRhiEvent_BeginExternalHeapDispatch, &config);
    g_State.d3d12->ConfigureEvent(kUnityRhiEvent_EndExternalHeapDispatch, &config);

    // The sync-point event needs the opposite configuration: queue access (it
    // signals a fence on Unity's graphics queue, so it runs on the submission
    // thread) and FlushCommandBuffers so every command list recorded before it
    // - including replayed command streams - is submitted before the signal.
    UnityD3D12PluginEventConfig syncConfig{};
    syncConfig.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_Allow;
    syncConfig.flags = kUnityD3D12EventConfigFlag_FlushCommandBuffers |
                       kUnityD3D12EventConfigFlag_SyncWorkerThreads;
    syncConfig.ensureActiveRenderTextureIsBound = false;
    g_State.d3d12->ConfigureEvent(kUnityRhiEvent_FlushAndSignalSyncPoint, &syncConfig);
    g_State.eventsConfigured = true;
}

void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
    switch (eventType)
    {
    case kUnityGfxDeviceEventInitialize:
    {
        UnityGfxRenderer renderer = g_State.graphics->GetRenderer();
        if (renderer == kUnityGfxRendererNull)
            return; // Called from UnityPluginLoad before the device exists; wait for the real init.

        if (renderer != kUnityGfxRendererD3D12)
        {
            LogError(
                "[UnityRHI] Graphics API is not Direct3D12 (renderer=%d). "
                "Set Graphics APIs for Windows to Direct3D12 in Player Settings and restart the editor.",
                static_cast<int>(renderer));
            return;
        }

        g_State.d3d12 = unityrhi::AcquireUnityD3D12(g_State.interfaces);
        if (!g_State.d3d12)
        {
            LogError("[UnityRHI] Unity D3D12 plugin interface unavailable; this Unity version is too old.");
            return;
        }

        // On 2021.3 the resource-state calls are not part of the public plugin
        // API and are bound to Unity's internals instead; refuse to run half-
        // wired rather than silently skipping every state handoff.
        if (!unityrhi::InitializeUnityStateForwarding())
        {
            LogError("[UnityRHI] Unity resource-state forwarding unavailable (%s); "
                     "this Unity build is not supported.",
                unityrhi::UnityStateForwardingDescription());
            return;
        }

        ID3D12Device* device = g_State.d3d12->GetDevice();
        HeapHook::Initialize(device);
        ConfigureRenderEvents();
        Device::Create(device, g_State.d3d12);
        InitializeNgx(device);
        InitializeFrameGeneration(device);
        InitializeDlrr(device, g_State.d3d12->GetCommandQueue());
        InitializeDlss(device);
        InitializeDlssNr(device);
        if (IDXGISwapChain* swapChain = g_State.d3d12->GetSwapChain())
            AdoptFrameGenerationSwapChain(
                swapChain, g_State.d3d12->GetCommandQueue());
        g_State.d3d12Active = true;
        LogInfo("[UnityRHI] Initialized on D3D12. ID3D12Device=%p state-forwarding=%s",
            static_cast<void*>(device), unityrhi::UnityStateForwardingDescription());
        // Which D3D12 runtime actually won. SetSDKVersion returning S_OK does
        // not guarantee the core loaded: preview SDKs additionally require
        // Windows Developer Mode, and fall back to the OS runtime silently.
        LogInfo("[UnityRHI] D3D12Core: %s", unityrhi::ResolvedD3D12CorePath());
        break;
    }
    case kUnityGfxDeviceEventShutdown:
        g_State.replayContext.reset();
        ShutdownDlssNr();
        ShutdownDlss();
        ShutdownDlrr();
        ShutdownFrameGeneration();
        ResetFrameGenerationHookDeviceObjects();
        ShutdownNgx();
        DestroyAllCommandSubmissions();
        Device::Destroy();
        g_State.d3d12 = nullptr;
        g_State.d3d12Active = false;
        g_State.eventsConfigured = false;
        break;
    default:
        break;
    }
}

void UNITY_INTERFACE_API OnRenderEvent(int eventId, void* data)
{
    (void)data;

    // Opportunistic cleanup of fence-expired deferred releases.
    {
        NativeProfileScope profile("UnityRHI.GarbageCollect");
        if (Device* device = Device::Get())
            device->GarbageCollect();
        TickDlssNr();
    }

    switch (eventId)
    {
    case kUnityRhiEvent_ExecuteCommandStream:
    {
        NativeProfileScope eventProfile("UnityRHI.ExecuteCommandStream");
        std::unique_ptr<CommandSubmission, decltype(&DestroyCommandSubmission)> submission(
            static_cast<CommandSubmission*>(data), &DestroyCommandSubmission);
        const void* stream = GetCommandSubmissionStream(submission.get());
        if (!stream)
            return;
        if (!g_State.d3d12)
            return;
        UnityGraphicsD3D12RecordingState recordingState{};
        {
            NativeProfileScope profile("UnityRHI.CommandRecordingState");
            if (!g_State.d3d12->CommandRecordingState(&recordingState) || !recordingState.commandList)
            {
                LogWarning("[UnityRHI] CommandRecordingState unavailable during command-stream event; stream dropped.");
                g_State.droppedCommandStreamCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        Device* device = Device::Get();
        if (!device)
            return;
        if (!g_State.tracedCommandListDevice.exchange(true))
        {
            ID3D12Device* commandListDevice = nullptr;
            const HRESULT parentHr = recordingState.commandList->GetDevice(
                IID_PPV_ARGS(&commandListDevice));
            if (SUCCEEDED(parentHr) && commandListDevice)
            {
                GpuDumps::Get().TraceDeviceIdentity(commandListDevice, "first command list parent");
                commandListDevice->Release();
            }
            else
            {
                LogWarning("[UnityRHI] Failed to query the command list parent device (0x%08X).",
                    static_cast<unsigned>(parentHr));
            }
        }
        // Resolve Unity's resource-state entry point once for this event. Every
        // Unity-owned resource the replay touches forwards its state through it,
        // so leaving it unbound costs a wrapper walk per binding per dispatch.
        {
            NativeProfileScope profile("UnityRHI.BindStateForwarding");
            if (!unityrhi::BeginUnityStateForwardingEvent(recordingState.commandList) &&
                !g_State.loggedStateForwardingUnbound.exchange(true))
            {
                // Not fatal - the replay still records - but every Unity-owned
                // resource silently keeps whatever state Unity last left it in.
                LogWarning("[UnityRHI] Could not bind Unity's command-list wrapper for this event; "
                           "resource-state forwarding is inactive for Unity-owned resources.");
            }
        }
        {
            NativeProfileScope profile("UnityRHI.HeapHookBegin");
            if (HeapHook::begin)
                HeapHook::begin();
        }
        if (!g_State.replayContext)
            g_State.replayContext = std::make_unique<ReplayContext>(&device->context());
        bool replayed = false;
        {
            NativeProfileScope profile("UnityRHI.ReplayCommandStream");
            // Lifetime tag for everything this stream touches: a fresh
            // recording instance, retired only when the marker recorded at
            // the tail of this replay lands in the readback ring - i.e. when
            // the GPU has executed past the replay. Never a Unity frame-fence
            // value: that fence is not a retire marker for injected work
            // (docs/rt-sbt-upload-corruption-report.md).
            const uint32_t instance = device->BeginRecordingInstance();
            replayed = ReplayCommandStream(
                stream, recordingState.commandList, instance, *g_State.replayContext);
            // Record even for a failed replay: commands recorded up to the
            // failure point still reference this instance's allocations, and
            // the marker covers exactly what was recorded before it.
            device->RecordLifetimeMarker(recordingState.commandList, instance);
        }
        if (!replayed)
            g_State.replayContext.reset();
        {
            NativeProfileScope profile("UnityRHI.HeapHookEndRestore");
            if (HeapHook::end)
                HeapHook::end();
            if (HeapHook::restore)
                HeapHook::restore(recordingState.commandList);
        }

        if (replayed)
            g_State.commandStreamEventCount.fetch_add(1, std::memory_order_relaxed);
        else
            g_State.droppedCommandStreamCount.fetch_add(1, std::memory_order_relaxed);
        {
            NativeProfileScope profile("UnityRHI.CheckDeviceRemoved");
            if (Device* activeDevice = Device::Get())
                activeDevice->CheckDeviceRemoved();
        }
        // Unity recycles command lists between events, so the resolved wrapper
        // must not outlive the event that produced it.
        unityrhi::EndUnityStateForwardingEvent();
        break;
    }
    case kUnityRhiEvent_FlushAndSignalSyncPoint:
    {
        if (Device* device = Device::Get())
        {
            device->SignalSyncPoint(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data)));
            device->CheckDeviceRemoved();
        }
        break;
    }
    case kUnityRhiEvent_BeginExternalHeapDispatch:
        if (HeapHook::begin)
            HeapHook::begin();
        break;
    case kUnityRhiEvent_EndExternalHeapDispatch:
    {
        if (HeapHook::end)
            HeapHook::end();
        if (!g_State.d3d12 || !HeapHook::restore)
            break;
        UnityGraphicsD3D12RecordingState recordingState{};
        if (g_State.d3d12->CommandRecordingState(&recordingState) && recordingState.commandList)
            HeapHook::restore(recordingState.commandList);
        else
            LogWarning("[UnityRHI] CommandRecordingState unavailable while restoring external plugin heaps.");
        break;
    }
    default:
        break;
    }
}
} // namespace
} // namespace unityrhi

extern "C"
{
    void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
    {
        using namespace unityrhi;
        g_State.Reset();
        InitializeNativeProfiler(unityInterfaces);
        g_State.interfaces = unityInterfaces;
        g_Log = unityInterfaces->Get<IUnityLog>();
        // Before anything can create a device. Only actually early enough when
        // this plugin is marked "Load on startup" (isPreloaded: 1), which puts
        // this call in Unity's preload step ahead of "GfxDevice: creating
        // device"; otherwise it detects the loaded core and does nothing.
        EnableAgilitySdkIfHostDoesNot();
        if (IsStandalonePlayer())
            InstallFrameGenerationHooks();
        g_State.graphics = unityInterfaces->Get<IUnityGraphics>();
        if (!g_State.graphics)
            return;
        g_State.graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
        // Per Unity convention the device may already exist when the plugin loads.
        OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
    }

    void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
    {
        using namespace unityrhi;
        g_State.replayContext.reset();
        ShutdownDlssNr();
        ShutdownDlss();
        ShutdownDlrr();
        ShutdownFrameGeneration();
        ShutdownNgx();
        ShutdownFrameGenerationHooks();
        Device::Destroy();
        if (g_State.graphics)
            g_State.graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
        g_State.Reset();
        g_Log = nullptr;
        ShutdownNativeProfiler();
    }

    // ---- Exports consumed by com.unityrhi (NativeMethods.cs) ----

    UNITY_INTERFACE_EXPORT UnityRenderingEventAndData UNITY_INTERFACE_API
    UnityRhiGetRenderEventAndDataFunc()
    {
        return unityrhi::OnRenderEvent;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetApiVersion()
    {
        // Version 10 exposes upload-ticket sizes for frame capture tooling.
        // Stale managed/native plugin pairs must not claim ABI compatibility.
        return 10;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiIsD3D12Active()
    {
        return unityrhi::g_State.d3d12Active ? 1 : 0;
    }

    UNITY_INTERFACE_EXPORT unsigned long long UNITY_INTERFACE_API UnityRhiGetCommandStreamEventCount()
    {
        return unityrhi::g_State.commandStreamEventCount.load(std::memory_order_relaxed);
    }

    UNITY_INTERFACE_EXPORT unsigned long long UNITY_INTERFACE_API UnityRhiGetDroppedCommandStreamCount()
    {
        return unityrhi::g_State.droppedCommandStreamCount.load(std::memory_order_relaxed);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiCreateDlrrInstance()
    {
        return unityrhi::CreateDlrrInstance();
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiDestroyDlrrInstance(int instanceId)
    {
        unityrhi::DestroyDlrrInstance(instanceId);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiCreateDlssNrInstance()
    {
        return unityrhi::CreateDlssNrInstance();
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiDestroyDlssNrInstance(int instanceId)
    {
        unityrhi::DestroyDlssNrInstance(instanceId);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetDlssNrAvailable()
    {
        return unityrhi::IsDlssNrRuntimeAvailable() ? 1 : 0;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetDlssNrInitResult()
    {
        return unityrhi::DlssNrRuntimeInitResult();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetDlssNrLastCreateResult()
    {
        return unityrhi::DlssNrLastCreateResult();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetDlssNrLastEvaluateResult()
    {
        return unityrhi::DlssNrLastEvaluateResult();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiCreateDlssInstance()
    {
        return unityrhi::CreateDlssInstance();
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiDestroyDlssInstance(int instanceId)
    {
        unityrhi::DestroyDlssInstance(instanceId);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetNgxDlssAvailable()
    {
        return unityrhi::NgxDlssAvailable();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetNgxDlssInitResult()
    {
        return unityrhi::NgxDlssInitResult();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetDlssLastCreateResult()
    {
        return unityrhi::DlssLastCreateResult();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetDlssLastEvaluateResult()
    {
        return unityrhi::DlssLastEvaluateResult();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiQueryDlssOptimalSettings(
        unsigned int outputWidth, unsigned int outputHeight, unsigned char upscalerMode,
        unsigned int* renderWidth, unsigned int* renderHeight)
    {
        return unityrhi::QueryDlssOptimalSettings(outputWidth, outputHeight, upscalerMode,
            renderWidth, renderHeight);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetNgxInitResult()
    {
        return unityrhi::NgxInitResult();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetNgxDlrrAvailable()
    {
        return unityrhi::NgxDlrrAvailable();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetNgxDlrrInitResult()
    {
        return unityrhi::NgxDlrrInitResult();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetNgxFrameGenerationAvailable()
    {
        return unityrhi::NgxFrameGenerationAvailable();
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API UnityRhiGetNgxFrameGenerationInitResult()
    {
        return unityrhi::NgxFrameGenerationInitResult();
    }

    UNITY_INTERFACE_EXPORT unsigned UNITY_INTERFACE_API UnityRhiGetNgxFrameGenerationMultiFrameCountMax()
    {
        return unityrhi::NgxFrameGenerationMultiFrameCountMax();
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiSetFrameGenerationEnabled(int enabled)
    {
        unityrhi::SetFrameGenerationEnabled(enabled != 0);
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityRhiSubmitFrameGenerationInputs(
        const unityrhi::FrameGenerationInputs* inputs)
    {
        if (inputs)
            unityrhi::SubmitFrameGenerationInputs(*inputs);
    }

    UNITY_INTERFACE_EXPORT unsigned long long UNITY_INTERFACE_API UnityRhiGetDisplayedPresentCount()
    {
        return unityrhi::GetDisplayedPresentCount();
    }
}
