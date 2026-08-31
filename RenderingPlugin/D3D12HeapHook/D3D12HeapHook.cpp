// D3D12HeapHook.cpp — see header.

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "D3D12HeapHook.h"

// ---------------------------------------------------------------------------
// Log verbosity control
//
//   NR_HEAPHOOK_LOG_LEVEL
//     0 = silent (no logs at all; Logf compiles to a no-op)
//     1 = errors + warnings only (install failures, missing cache, etc.)
//     2 = + info, throttled (default): first 4 captures/restores per thread,
//         then every 256th; every Restore and every SKIP shown
//     3 = everything, no throttling
//
// Override via compiler flag, e.g. /DNR_HEAPHOOK_LOG_LEVEL=0
// ---------------------------------------------------------------------------
#ifndef NR_HEAPHOOK_LOG_LEVEL
#define NR_HEAPHOOK_LOG_LEVEL 2
#endif

namespace
{
    // ID3D12GraphicsCommandList vtable indices. Close is observational.
    // Reset/ClearState also invalidate cached state from the previous
    // command-list recording epoch.
    constexpr UINT kCloseVTIdx              = 9;
    constexpr UINT kResetVTIdx              = 10;
    constexpr UINT kClearStateVTIdx         = 11;
    // 9 base methods + 19 cmd-list methods before SetDescriptorHeaps = 28.
    constexpr UINT kSetDescriptorHeapsVTIdx = 28;

    using PFN_Close = HRESULT (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
    using PFN_Reset = HRESULT (STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
    using PFN_ClearState = void (STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*, ID3D12PipelineState*);
    using PFN_SetDescriptorHeaps = void (STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);

    std::atomic<bool>        g_Installed{false};
    PFN_Close                g_OrigClose = nullptr;
    PFN_Reset                g_OrigReset = nullptr;
    PFN_ClearState           g_OrigClearState = nullptr;
    PFN_SetDescriptorHeaps   g_OrigSetDescriptorHeaps = nullptr;
    D3D12HeapHook::LogFn     g_Logger = nullptr;

    constexpr size_t kMaxCachedHeaps = 8;

    // Per-command-list cache of the most recent SetDescriptorHeaps() arguments.
    // Keyed by cmdList pointer — descriptor-heap bindings are command-list-local
    // state in D3D12, so restoring must use exactly the same cmd list's values.
    struct HeapBinding
    {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heaps[kMaxCachedHeaps];
        UINT num = 0;
    };

    struct CommandListObservation
    {
        uint64_t epoch = 0;
        uint64_t captureEpoch = 0;
        uint64_t captureSerial = 0;
        uint64_t lastRestoreSerial = 0;
        uint64_t lastRestoreEpoch = 0;
        uint64_t lastRestoreCaptureEpoch = 0;
        DWORD captureThread = 0;
        DWORD lastRestoreThread = 0;
        bool hasCapture = false;
        bool lastRestoreHadBinding = false;
    };

    std::mutex                                                     g_CacheMutex;
    std::unordered_map<ID3D12GraphicsCommandList*, HeapBinding>    g_Cache;
    std::unordered_map<ID3D12GraphicsCommandList*, CommandListObservation>
                                                                    g_Observations;

    thread_local uint64_t              tl_CaptureCount  = 0;
    thread_local uint64_t              tl_RestoreCount  = 0;
    thread_local int                   tl_InPluginDispatch = 0; // nesting depth

    std::atomic<uint64_t>  g_TotalCaptures{0};
    std::atomic<uint64_t>  g_TotalRestores{0};
    std::atomic<uint64_t>  g_TotalResets{0};
    std::atomic<uint64_t>  g_TotalClearStates{0};

    void Logf(int level, const char* fmt, ...)
    {
#if NR_HEAPHOOK_LOG_LEVEL == 0
        (void)level;
        (void)fmt;
        return;
#else
        // Level 1 keeps warnings/errors; level 2 additionally enables the
        // throttled informational capture/restore trace.
        if (level == 0 && NR_HEAPHOOK_LOG_LEVEL < 2)
            return;

        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n < 0) { buf[sizeof(buf) - 1] = '\0'; }

        const char* tag = (level == 2) ? "[NR/HeapHook ERR] "
                         : (level == 1) ? "[NR/HeapHook WRN] "
                                        : "[NR/HeapHook] ";

        char prefixed[560];
        _snprintf_s(prefixed, sizeof(prefixed), _TRUNCATE, "%s%s", tag, buf);

        if (g_Logger)
        {
            g_Logger(level, prefixed);
        }
        else
        {
            // Fallback — visible in DebugView / VS output.
            char withNl[576];
            _snprintf_s(withNl, sizeof(withNl), _TRUNCATE, "%s\n", prefixed);
            OutputDebugStringA(withNl);
        }
#endif
    }

    void ObserveEpochAdvance(ID3D12GraphicsCommandList* cmdList,
                             const char* reason,
                             uint64_t operationSerial)
    {
        CommandListObservation observation{};
        bool hadCachedBinding = false;
        HeapBinding invalidatedBinding{};
        {
            std::lock_guard<std::mutex> lock(g_CacheMutex);
            CommandListObservation& live = g_Observations[cmdList];
            ++live.epoch;

            auto bindingIt = g_Cache.find(cmdList);
            if (bindingIt != g_Cache.end())
            {
                hadCachedBinding = true;
                invalidatedBinding = std::move(bindingIt->second);
                g_Cache.erase(bindingIt);
            }

            // Keep the last capture metadata for diagnostics, but it is no
            // longer a valid capture for the new recording epoch.
            live.hasCapture = false;
            observation = live;
        }

        // Release heap references after dropping the cache lock.
        invalidatedBinding = HeapBinding{};

        if (operationSerial <= 8 || (operationSerial & 0xFF) == 0 ||
            NR_HEAPHOOK_LOG_LEVEL >= 3)
        {
            Logf(0,
                 "%s: cmdList=%p epoch=%llu cacheInvalidated=%d captureEpoch=%llu "
                 "captureSerial=%llu (op=%llu)",
                 reason, (void*)cmdList,
                 (unsigned long long)observation.epoch,
                 hadCachedBinding ? 1 : 0,
                 (unsigned long long)observation.captureEpoch,
                 (unsigned long long)observation.captureSerial,
                 (unsigned long long)operationSerial);
        }
    }

    HRESULT STDMETHODCALLTYPE Hooked_Close(ID3D12GraphicsCommandList* This)
    {
        const HRESULT hr = g_OrigClose(This);
        if (FAILED(hr))
        {
            CommandListObservation observation{};
            HeapBinding binding{};
            bool foundBinding = false;
            {
                std::lock_guard<std::mutex> lock(g_CacheMutex);
                auto observationIt = g_Observations.find(This);
                if (observationIt != g_Observations.end())
                    observation = observationIt->second;
                auto bindingIt = g_Cache.find(This);
                if (bindingIt != g_Cache.end())
                {
                    binding = bindingIt->second;
                    foundBinding = true;
                }
            }

            Logf(2,
                 "Close FAILED: cmdList=%p hr=0x%08lX epoch=%llu "
                 "captureEpoch=%llu captureSerial=%llu lastRestore=%llu "
                 "restoreEpoch=%llu restoreCaptureEpoch=%llu "
                 "heap0=%p heap1=%p captureTid=%lu restoreTid=%lu",
                 (void*)This, (unsigned long)hr,
                 (unsigned long long)observation.epoch,
                 (unsigned long long)observation.captureEpoch,
                 (unsigned long long)observation.captureSerial,
                 (unsigned long long)observation.lastRestoreSerial,
                 (unsigned long long)observation.lastRestoreEpoch,
                 (unsigned long long)observation.lastRestoreCaptureEpoch,
                 foundBinding && binding.num > 0 ? (void*)binding.heaps[0].Get() : nullptr,
                 foundBinding && binding.num > 1 ? (void*)binding.heaps[1].Get() : nullptr,
                 (unsigned long)observation.captureThread,
                 (unsigned long)observation.lastRestoreThread);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Hooked_Reset(
        ID3D12GraphicsCommandList* This,
        ID3D12CommandAllocator* allocator,
        ID3D12PipelineState* initialState)
    {
        const HRESULT hr = g_OrigReset(This, allocator, initialState);
        if (SUCCEEDED(hr))
        {
            const uint64_t serial =
                g_TotalResets.fetch_add(1, std::memory_order_relaxed) + 1;
            ObserveEpochAdvance(This, "Reset observed", serial);
        }
        else
        {
            Logf(2, "Reset FAILED: cmdList=%p hr=0x%08lX",
                 (void*)This, (unsigned long)hr);
        }
        return hr;
    }

    void STDMETHODCALLTYPE Hooked_ClearState(
        ID3D12GraphicsCommandList* This,
        ID3D12PipelineState* pipelineState)
    {
        g_OrigClearState(This, pipelineState);

        // A ClearState performed by the external plugin is part of the state
        // that RestoreUnityHeaps is expected to repair. Only Unity-side
        // ClearState calls advance the observed Unity recording epoch.
        if (tl_InPluginDispatch == 0)
        {
            const uint64_t serial =
                g_TotalClearStates.fetch_add(1, std::memory_order_relaxed) + 1;
            ObserveEpochAdvance(This, "ClearState observed", serial);
        }
    }

    void STDMETHODCALLTYPE Hooked_SetDescriptorHeaps(
        ID3D12GraphicsCommandList* This,
        UINT NumDescriptorHeaps,
        ID3D12DescriptorHeap* const* ppDescriptorHeaps)
    {
        // When the plugin itself is binding heaps, pass through without
        // updating the cache — otherwise we would "restore" our own heaps.
        if (tl_InPluginDispatch > 0)
        {
            if (tl_CaptureCount <= 4 || (tl_CaptureCount & 0xFF) == 0)
            {
                ID3D12DescriptorHeap* h0 = (NumDescriptorHeaps > 0 && ppDescriptorHeaps)
                                            ? ppDescriptorHeaps[0] : nullptr;
                Logf(0,
                     "SKIP (plugin-owned) SetDescriptorHeaps: cmdList=%p num=%u heap0=%p depth=%d",
                     (void*)This, NumDescriptorHeaps, (void*)h0, tl_InPluginDispatch);
            }
            g_OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
            return;
        }

        UINT n = NumDescriptorHeaps;
        if (n > kMaxCachedHeaps) n = static_cast<UINT>(kMaxCachedHeaps);
        if (ppDescriptorHeaps == nullptr) n = 0;

        HeapBinding bind{};
        bind.num = n;
        for (UINT i = 0; i < n; ++i)
            bind.heaps[i] = ppDescriptorHeaps[i];

        const uint64_t total =
            g_TotalCaptures.fetch_add(1, std::memory_order_relaxed) + 1;
        uint64_t captureEpoch = 0;
        {
            std::lock_guard<std::mutex> lock(g_CacheMutex);
            g_Cache[This] = bind;
            CommandListObservation& observation = g_Observations[This];
            observation.captureEpoch = observation.epoch;
            observation.captureSerial = total;
            observation.captureThread = GetCurrentThreadId();
            observation.hasCapture = true;
            captureEpoch = observation.captureEpoch;
        }

        ++tl_CaptureCount;

        if (tl_CaptureCount <= 4 || (tl_CaptureCount & 0xFF) == 0 ||
            NR_HEAPHOOK_LOG_LEVEL >= 3)
        {
            ID3D12DescriptorHeap* h0 = (n > 0) ? bind.heaps[0].Get() : nullptr;
            ID3D12DescriptorHeap* h1 = (n > 1) ? bind.heaps[1].Get() : nullptr;
            Logf(0,
                 "Capture SetDescriptorHeaps: cmdList=%p num=%u (raw=%u) heap0=%p heap1=%p "
                 "epoch=%llu (thread cnt=%llu total=%llu)",
                 (void*)This, n, NumDescriptorHeaps, (void*)h0, (void*)h1,
                 (unsigned long long)captureEpoch,
                 (unsigned long long)tl_CaptureCount, (unsigned long long)total);
        }

        g_OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
    }

    bool Unprotect(void* addr)
    {
        constexpr SIZE_T pageSize = 4096;
        DWORD oldProtect = 0;
        void* page = reinterpret_cast<void*>(
            (reinterpret_cast<size_t>(addr) / pageSize) * pageSize);
        return VirtualProtect(page, pageSize, PAGE_READWRITE, &oldProtect) != 0;
    }

    void InstallHook(ID3D12GraphicsCommandList* cmdList)
    {
        if (!cmdList) return;
        if (g_Installed.load(std::memory_order_acquire)) return;

        // Race-guard: only the first caller patches.
        bool expected = false;
        if (!g_Installed.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel))
            return;

        Logf(0, "InstallHook: patching vtable on cmdList=%p (slot idx %u)",
             (void*)cmdList, kSetDescriptorHeapsVTIdx);

        void** vtable              = *reinterpret_cast<void***>(cmdList);
        void** closeSlot            = vtable + kCloseVTIdx;
        void** resetSlot            = vtable + kResetVTIdx;
        void** clearStateSlot       = vtable + kClearStateVTIdx;
        void** setDescriptorSlot    = vtable + kSetDescriptorHeapsVTIdx;

        if (!Unprotect(closeSlot) || !Unprotect(resetSlot) ||
            !Unprotect(clearStateSlot) || !Unprotect(setDescriptorSlot))
        {
            Logf(2, "InstallHook: VirtualProtect failed (GetLastError=%lu) on vtable=%p",
                 (unsigned long)GetLastError(), (void*)vtable);
            g_Installed.store(false, std::memory_order_release);
            return;
        }

        g_OrigClose = reinterpret_cast<PFN_Close>(*closeSlot);
        g_OrigReset = reinterpret_cast<PFN_Reset>(*resetSlot);
        g_OrigClearState = reinterpret_cast<PFN_ClearState>(*clearStateSlot);
        g_OrigSetDescriptorHeaps =
            reinterpret_cast<PFN_SetDescriptorHeaps>(*setDescriptorSlot);
        *closeSlot = reinterpret_cast<void*>(&Hooked_Close);
        *resetSlot = reinterpret_cast<void*>(&Hooked_Reset);
        *clearStateSlot = reinterpret_cast<void*>(&Hooked_ClearState);
        *setDescriptorSlot = reinterpret_cast<void*>(&Hooked_SetDescriptorHeaps);

        Logf(0,
             "InstallHook: SUCCESS. vtable=%p Close=%p Reset=%p ClearState=%p "
             "SetDescriptorHeaps=%p",
             (void*)vtable, (void*)g_OrigClose, (void*)g_OrigReset,
             (void*)g_OrigClearState, (void*)g_OrigSetDescriptorHeaps);
    }

    struct RestoreExceptionContext
    {
        ID3D12GraphicsCommandList* cmdList;
        ID3D12DescriptorHeap* heap0;
        ID3D12DescriptorHeap* heap1;
        UINT num;
        uint64_t restoreSerial;
        uint64_t currentEpoch;
        uint64_t captureEpoch;
        uint64_t captureSerial;
        DWORD captureThread;
        DWORD restoreThread;
    };

    LONG LogRestoreException(EXCEPTION_POINTERS* exception,
                             const RestoreExceptionContext* context)
    {
        const DWORD code = exception && exception->ExceptionRecord
            ? exception->ExceptionRecord->ExceptionCode : 0;
        void* address = exception && exception->ExceptionRecord
            ? exception->ExceptionRecord->ExceptionAddress : nullptr;
        Logf(2,
             "RESTORE CPU EXCEPTION: code=0x%08lX address=%p cmdList=%p "
             "num=%u heap0=%p heap1=%p restore=%llu epoch=%llu "
             "captureEpoch=%llu captureSerial=%llu "
             "captureTid=%lu restoreTid=%lu; continuing exception search",
             (unsigned long)code, address, (void*)context->cmdList,
             context->num, (void*)context->heap0, (void*)context->heap1,
             (unsigned long long)context->restoreSerial,
             (unsigned long long)context->currentEpoch,
             (unsigned long long)context->captureEpoch,
             (unsigned long long)context->captureSerial,
             (unsigned long)context->captureThread,
             (unsigned long)context->restoreThread);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Keep SEH in a POD-only helper so MSVC does not reject __try in the
    // RestoreUnityHeaps function, which uses C++ lock guards.
    void InvokeRestoreWithDiagnostics(PFN_SetDescriptorHeaps function,
                                      ID3D12GraphicsCommandList* cmdList,
                                      UINT num,
                                      ID3D12DescriptorHeap* const* heaps,
                                      const RestoreExceptionContext* context)
    {
        __try
        {
            function(cmdList, num, heaps);
        }
        __except (LogRestoreException(GetExceptionInformation(), context))
        {
            // LogRestoreException returns EXCEPTION_CONTINUE_SEARCH, so this
            // block is intentionally unreachable and the original crash is
            // delivered unchanged to Unity's crash handler.
        }
    }
}

namespace D3D12HeapHook
{
    void SetLogger(LogFn fn)
    {
        g_Logger = fn;
        Logf(0, "Logger attached");
    }

    void BeginPluginDispatch()
    {
        ++tl_InPluginDispatch;
    }

    void EndPluginDispatch()
    {
        if (tl_InPluginDispatch > 0)
            --tl_InPluginDispatch;
    }

    bool InstallHookFromDevice(ID3D12Device* device)
    {
        if (!device)
        {
            Logf(2, "InstallHookFromDevice: null device");
            return false;
        }
        if (g_Installed.load(std::memory_order_acquire))
        {
            Logf(0, "InstallHookFromDevice: already installed, skipping");
            return true;
        }

        // Create a throwaway direct command allocator + command list just to
        // obtain the vtable. The vtable is shared by all cmd lists of this
        // interface type, so patching it once covers every cmd list Unity
        // creates later (and any already created).
        ID3D12CommandAllocator*    alloc = nullptr;
        ID3D12GraphicsCommandList* list  = nullptr;

        HRESULT hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (FAILED(hr) || !alloc)
        {
            Logf(2, "InstallHookFromDevice: CreateCommandAllocator failed hr=0x%08lx",
                 (unsigned long)hr);
            return false;
        }

        hr = device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
            IID_PPV_ARGS(&list));
        if (FAILED(hr) || !list)
        {
            Logf(2, "InstallHookFromDevice: CreateCommandList failed hr=0x%08lx",
                 (unsigned long)hr);
            alloc->Release();
            return false;
        }

        InstallHook(list);

        // Close & release — we never submit this list.
        list->Close();
        list->Release();
        alloc->Release();

        return g_Installed.load(std::memory_order_acquire);
    }

    void RestoreUnityHeaps(ID3D12GraphicsCommandList* cmdList)
    {
        if (!cmdList)
        {
            Logf(1, "RestoreUnityHeaps: null cmdList, skipping");
            return;
        }
        if (!g_OrigSetDescriptorHeaps)
        {
            Logf(1, "RestoreUnityHeaps: hook not installed yet, skipping (cmdList=%p)",
                 (void*)cmdList);
            return;
        }

        HeapBinding bind{};
        CommandListObservation observation{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_CacheMutex);
            auto it = g_Cache.find(cmdList);
            if (it != g_Cache.end())
            {
                bind  = it->second;
                found = true;
            }
            auto observationIt = g_Observations.find(cmdList);
            if (observationIt != g_Observations.end())
                observation = observationIt->second;
        }

        if (!found || bind.num == 0)
        {
            Logf(1,
                 "RestoreUnityHeaps: no current-epoch heaps for cmdList=%p, "
                 "skipping (epoch=%llu captureEpoch=%llu captureSerial=%llu "
                 "hasCurrentCapture=%d)",
                 (void*)cmdList,
                 (unsigned long long)observation.epoch,
                 (unsigned long long)observation.captureEpoch,
                 (unsigned long long)observation.captureSerial,
                 observation.hasCapture ? 1 : 0);
            return;
        }

        ++tl_RestoreCount;
        const uint64_t total = g_TotalRestores.fetch_add(1, std::memory_order_relaxed) + 1;
        const DWORD restoreThread = GetCurrentThreadId();

        {
            std::lock_guard<std::mutex> lock(g_CacheMutex);
            CommandListObservation& live = g_Observations[cmdList];
            live.lastRestoreSerial = total;
            live.lastRestoreEpoch = observation.epoch;
            live.lastRestoreCaptureEpoch = observation.captureEpoch;
            live.lastRestoreThread = restoreThread;
            live.lastRestoreHadBinding = true;
        }

        ID3D12DescriptorHeap* heaps[kMaxCachedHeaps]{};
        for (UINT i = 0; i < bind.num; ++i)
            heaps[i] = bind.heaps[i].Get();

        if (tl_RestoreCount <= 4 ||
            (tl_RestoreCount & 0xFF) == 0 ||
            NR_HEAPHOOK_LOG_LEVEL >= 3)
        {
            ID3D12DescriptorHeap* h0 = heaps[0];
            ID3D12DescriptorHeap* h1 = (bind.num > 1) ? heaps[1] : nullptr;
            Logf(0,
                 "Restore Unity heaps: cmdList=%p num=%u heap0=%p heap1=%p "
                 "epoch=%llu captureEpoch=%llu captureSerial=%llu "
                 "(thread cnt=%llu total=%llu)",
                 (void*)cmdList, bind.num, (void*)h0, (void*)h1,
                 (unsigned long long)observation.epoch,
                 (unsigned long long)observation.captureEpoch,
                 (unsigned long long)observation.captureSerial,
                 (unsigned long long)tl_RestoreCount, (unsigned long long)total);
        }

        RestoreExceptionContext exceptionContext{};
        exceptionContext.cmdList = cmdList;
        exceptionContext.heap0 = bind.num > 0 ? heaps[0] : nullptr;
        exceptionContext.heap1 = bind.num > 1 ? heaps[1] : nullptr;
        exceptionContext.num = bind.num;
        exceptionContext.restoreSerial = total;
        exceptionContext.currentEpoch = observation.epoch;
        exceptionContext.captureEpoch = observation.captureEpoch;
        exceptionContext.captureSerial = observation.captureSerial;
        exceptionContext.captureThread = observation.captureThread;
        exceptionContext.restoreThread = restoreThread;
        InvokeRestoreWithDiagnostics(g_OrigSetDescriptorHeaps, cmdList,
                                     bind.num, heaps, &exceptionContext);
    }
}
