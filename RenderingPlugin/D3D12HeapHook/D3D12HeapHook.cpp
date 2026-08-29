// D3D12HeapHook.cpp — see header.

#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

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
    // ID3D12GraphicsCommandList::SetDescriptorHeaps vtable index.
    // 9 base methods + 19 cmd-list methods before SetDescriptorHeaps = index 28.
    constexpr UINT kSetDescriptorHeapsVTIdx = 28;

    using PFN_SetDescriptorHeaps = void (STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);

    std::atomic<bool>        g_Installed{false};
    PFN_SetDescriptorHeaps   g_OrigSetDescriptorHeaps = nullptr;
    D3D12HeapHook::LogFn     g_Logger = nullptr;

    constexpr size_t kMaxCachedHeaps = 8;

    // Per-command-list cache of the most recent SetDescriptorHeaps() arguments.
    // Keyed by cmdList pointer — descriptor-heap bindings are command-list-local
    // state in D3D12, so restoring must use exactly the same cmd list's values.
    struct HeapBinding
    {
        ID3D12DescriptorHeap* heaps[kMaxCachedHeaps];
        UINT                  num;
    };

    std::mutex                                                     g_CacheMutex;
    std::unordered_map<ID3D12GraphicsCommandList*, HeapBinding>    g_Cache;

    thread_local uint64_t              tl_CaptureCount  = 0;
    thread_local uint64_t              tl_RestoreCount  = 0;
    thread_local int                   tl_InPluginDispatch = 0; // nesting depth

    std::atomic<uint64_t>  g_TotalCaptures{0};
    std::atomic<uint64_t>  g_TotalRestores{0};

    void Logf(int level, const char* fmt, ...)
    {
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

        {
            std::lock_guard<std::mutex> lock(g_CacheMutex);
            g_Cache[This] = bind;
        }

        ++tl_CaptureCount;
        const uint64_t total = g_TotalCaptures.fetch_add(1, std::memory_order_relaxed) + 1;

        if (tl_CaptureCount <= 4 || (tl_CaptureCount & 0xFF) == 0)
        {
            ID3D12DescriptorHeap* h0 = (n > 0) ? bind.heaps[0] : nullptr;
            ID3D12DescriptorHeap* h1 = (n > 1) ? bind.heaps[1] : nullptr;
            Logf(0,
                 "Capture SetDescriptorHeaps: cmdList=%p num=%u (raw=%u) heap0=%p heap1=%p "
                 "(thread cnt=%llu total=%llu)",
                 (void*)This, n, NumDescriptorHeaps, (void*)h0, (void*)h1,
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

        void** vtable = *reinterpret_cast<void***>(cmdList);
        void** slot   = vtable + kSetDescriptorHeapsVTIdx;

        if (!Unprotect(slot))
        {
            Logf(2, "InstallHook: VirtualProtect failed (GetLastError=%lu) on slot=%p",
                 (unsigned long)GetLastError(), (void*)slot);
            g_Installed.store(false, std::memory_order_release);
            return;
        }

        g_OrigSetDescriptorHeaps =
            reinterpret_cast<PFN_SetDescriptorHeaps>(*slot);
        *slot = reinterpret_cast<void*>(&Hooked_SetDescriptorHeaps);

        Logf(0, "InstallHook: SUCCESS. vtable=%p slot=%p orig=%p new=%p",
             (void*)vtable, (void*)slot,
             (void*)g_OrigSetDescriptorHeaps,
             (void*)&Hooked_SetDescriptorHeaps);
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
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_CacheMutex);
            auto it = g_Cache.find(cmdList);
            if (it != g_Cache.end())
            {
                bind  = it->second;
                found = true;
            }
        }

        if (!found || bind.num == 0)
        {
            Logf(1, "RestoreUnityHeaps: no cached heaps for cmdList=%p, skipping",
                 (void*)cmdList);
            return;
        }

        ++tl_RestoreCount;
        const uint64_t total = g_TotalRestores.fetch_add(1, std::memory_order_relaxed) + 1;

        if (tl_RestoreCount <= 4 || (tl_RestoreCount & 0xFF) == 0)
        {
            ID3D12DescriptorHeap* h0 = bind.heaps[0];
            ID3D12DescriptorHeap* h1 = (bind.num > 1) ? bind.heaps[1] : nullptr;
            Logf(0,
                 "Restore Unity heaps: cmdList=%p num=%u heap0=%p heap1=%p "
                 "(thread cnt=%llu total=%llu)",
                 (void*)cmdList, bind.num, (void*)h0, (void*)h1,
                 (unsigned long long)tl_RestoreCount, (unsigned long long)total);
        }

        g_OrigSetDescriptorHeaps(cmdList, bind.num, bind.heaps);
    }
}
