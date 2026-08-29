#include "FrameGenerationHooks.h"

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <mutex>
#include <algorithm>
#include <cstdint>

#include "FrameGenerationDebug.h"
#include "FrameGenerationSwapChain.h"
#include "UnityRhiLog.h"

namespace unityrhi
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr size_t kFactoryCreateSwapChain = 10;
constexpr size_t kFactoryCreateSwapChainForHwnd = 15;
constexpr size_t kSwapChainPresent = 8;
constexpr size_t kSwapChainPresent1 = 22;
constexpr UINT kMinRealSwapChainBuffers = 4;

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

struct HookState
{
    std::mutex mutex;
    void** createSwapChainSlot = nullptr;
    void** createSwapChainForHwndSlot = nullptr;
    void** presentSlot = nullptr;
    void** present1Slot = nullptr;
    CreateSwapChainFn createSwapChain = nullptr;
    CreateSwapChainForHwndFn createSwapChainForHwnd = nullptr;
    PresentFn present = nullptr;
    Present1Fn present1 = nullptr;
    ComPtr<ID3D12CommandQueue> queue;
    bool factoryInstalled = false;
};

HookState g_Hooks;

UINT RealBufferCount(UINT unityBufferCount)
{
    return std::max(unityBufferCount * 2u, kMinRealSwapChainBuffers);
}

bool CanWrapSwapChain(const DXGI_SWAP_CHAIN_DESC1& desc)
{
    if (desc.Stereo)
        return false;
    if (desc.SampleDesc.Count > 1)
        return false;
    return desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
           desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
}

bool WriteSlot(void** slot, void* replacement)
{
    if (!slot)
        return false;
    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;
    InterlockedExchangePointer(slot, replacement);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

template <typename T>
void RestoreSlot(void**& slot, T original, void* hook)
{
    if (slot && *slot == hook && original)
        WriteSlot(slot, reinterpret_cast<void*>(original));
    slot = nullptr;
}

HRESULT CallOriginalPresent(
    IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
    PresentFn present, Present1Fn present1,
    const DXGI_PRESENT_PARAMETERS* parameters)
{
    HRESULT hr = E_FAIL;
    if (present)
        hr = present(swapChain, syncInterval, flags);
    else
    {
        ComPtr<IDXGISwapChain1> swapChain1;
        if (present1 && SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1))))
            hr = present1(swapChain1.Get(), syncInterval, flags, parameters);
    }
    if (SUCCEEDED(hr) && (flags & DXGI_PRESENT_TEST) == 0)
        NoteDisplayedPresent();
    return hr;
}

// Fallback only: Unity already owns this swapchain. Never extra-Present here —
// that desyncs GetCurrentBackBufferIndex / BufferCount and hangs the device.
HRESULT PresentWithoutInsertion(
    IDXGISwapChain* swapChain, ID3D12CommandQueue* queue,
    UINT syncInterval, UINT flags,
    PresentFn present, Present1Fn present1,
    const DXGI_PRESENT_PARAMETERS* parameters)
{
    ComPtr<IDXGISwapChain3> swapChain3;
    FrameGenerationPresentAction action = FrameGenerationPresentAction::None;
    if (queue && SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))))
        action = EvaluateFrameGeneration(swapChain3.Get(), queue, nullptr);

    if (action == FrameGenerationPresentAction::Insert)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LogWarning("[UnityRHI][DLSS-G] 2x insertion requires the FG swapchain proxy; "
                "falling back to single Present (replace).");
        }
        action = FrameGenerationPresentAction::Replace;
    }

    const HRESULT result = CallOriginalPresent(
        swapChain, syncInterval, flags, present, present1, parameters);
    if (FAILED(result))
        LogError("[UnityRHI][DLSS-G] Fallback swapchain Present failed (0x%08X).",
            static_cast<unsigned>(result));
    return result;
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
HRESULT STDMETHODCALLTYPE HookedPresent1(
    IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters);

void PatchPresentLocked(IDXGISwapChain* swapChain)
{
    if (!swapChain)
        return;
    void** vtable = *reinterpret_cast<void***>(swapChain);
    void** presentSlot = &vtable[kSwapChainPresent];
    if (*presentSlot != reinterpret_cast<void*>(&HookedPresent))
    {
        if (g_Hooks.presentSlot && g_Hooks.presentSlot != presentSlot)
            RestoreSlot(g_Hooks.presentSlot, g_Hooks.present,
                reinterpret_cast<void*>(&HookedPresent));
        g_Hooks.present = reinterpret_cast<PresentFn>(*presentSlot);
        if (WriteSlot(presentSlot, reinterpret_cast<void*>(&HookedPresent)))
            g_Hooks.presentSlot = presentSlot;
    }

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1))))
        return;
    vtable = *reinterpret_cast<void***>(swapChain1.Get());
    void** present1Slot = &vtable[kSwapChainPresent1];
    if (*present1Slot != reinterpret_cast<void*>(&HookedPresent1))
    {
        if (g_Hooks.present1Slot && g_Hooks.present1Slot != present1Slot)
            RestoreSlot(g_Hooks.present1Slot, g_Hooks.present1,
                reinterpret_cast<void*>(&HookedPresent1));
        g_Hooks.present1 = reinterpret_cast<Present1Fn>(*present1Slot);
        if (WriteSlot(present1Slot, reinterpret_cast<void*>(&HookedPresent1)))
            g_Hooks.present1Slot = present1Slot;
    }
}

void AdoptFallback(IDXGISwapChain* swapChain, IUnknown* queueObject)
{
    if (!swapChain || !queueObject)
        return;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(queueObject->QueryInterface(IID_PPV_ARGS(&queue))))
        return;
    std::lock_guard lock(g_Hooks.mutex);
    const bool firstPresentHook = !g_Hooks.presentSlot;
    g_Hooks.queue = queue;
    PatchPresentLocked(swapChain);
    if (firstPresentHook && g_Hooks.presentSlot)
        LogInfo("[UnityRHI][DLSS-G] Hooked IDXGISwapChain::Present/Present1 (no extra Present; "
            "2x insertion needs the FG proxy created at swapchain construction).");
}

HRESULT TryWrapCreatedSwapChain1(
    IDXGISwapChain1* created, IUnknown* queueObject, UINT unityBufferCount,
    IDXGISwapChain1** wrapped)
{
    const HRESULT hr = WrapFrameGenerationSwapChain1(
        created, queueObject, unityBufferCount, wrapped);
    if (FAILED(hr) || !wrapped || !*wrapped)
    {
        LogError("[UnityRHI][DLSS-G] Failed to wrap swapchain as FG proxy (0x%08X).",
            static_cast<unsigned>(hr));
        return FAILED(hr) ? hr : E_FAIL;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** swapChain)
{
    CreateSwapChainFn original;
    {
        std::lock_guard lock(g_Hooks.mutex);
        original = g_Hooks.createSwapChain;
    }
    if (!original)
        return E_FAIL;
    if (!desc || !swapChain)
        return original(factory, device, desc, swapChain);

    DXGI_SWAP_CHAIN_DESC1 desc1{};
    desc1.Width = desc->BufferDesc.Width;
    desc1.Height = desc->BufferDesc.Height;
    desc1.Format = desc->BufferDesc.Format;
    desc1.Stereo = FALSE;
    desc1.SampleDesc = desc->SampleDesc;
    desc1.BufferUsage = desc->BufferUsage;
    desc1.BufferCount = desc->BufferCount;
    desc1.SwapEffect = desc->SwapEffect;
    desc1.Flags = desc->Flags;
    if (!CanWrapSwapChain(desc1))
        return original(factory, device, desc, swapChain);

    const UINT unityCount = desc->BufferCount;
    DXGI_SWAP_CHAIN_DESC realDesc = *desc;
    realDesc.BufferCount = RealBufferCount(unityCount);
    IDXGISwapChain* created = nullptr;
    HRESULT hr = original(factory, device, &realDesc, &created);
    if (SUCCEEDED(hr) && created)
    {
        IDXGISwapChain* wrapped = nullptr;
        if (SUCCEEDED(WrapFrameGenerationSwapChain(
                created, device, unityCount, &wrapped)) && wrapped)
        {
            created->Release();
            *swapChain = wrapped;
            return S_OK;
        }
        created->Release();
    }

    LogWarning("[UnityRHI][DLSS-G] FG proxy wrap failed for CreateSwapChain; "
        "creating Unity's original BufferCount=%u.", unityCount);
    return original(factory, device, desc, swapChain);
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
    IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
    IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain)
{
    CreateSwapChainForHwndFn original;
    {
        std::lock_guard lock(g_Hooks.mutex);
        original = g_Hooks.createSwapChainForHwnd;
    }
    if (!original)
        return E_FAIL;
    if (!desc || !swapChain)
        return original(factory, device, hwnd, desc, fullscreenDesc, restrictToOutput, swapChain);
    if (!CanWrapSwapChain(*desc))
        return original(factory, device, hwnd, desc, fullscreenDesc, restrictToOutput, swapChain);

    const UINT unityCount = desc->BufferCount;
    DXGI_SWAP_CHAIN_DESC1 realDesc = *desc;
    realDesc.BufferCount = RealBufferCount(unityCount);
    LogInfo("[UnityRHI][DLSS-G] CreateSwapChainForHwnd: Unity BufferCount=%u, "
        "real swapchain BufferCount=%u (hidden from Unity).",
        unityCount, realDesc.BufferCount);

    IDXGISwapChain1* created = nullptr;
    HRESULT hr = original(factory, device, hwnd, &realDesc, fullscreenDesc,
        restrictToOutput, &created);
    if (SUCCEEDED(hr) && created)
    {
        IDXGISwapChain1* wrapped = nullptr;
        if (SUCCEEDED(TryWrapCreatedSwapChain1(created, device, unityCount, &wrapped)))
        {
            created->Release();
            *swapChain = wrapped;
            return S_OK;
        }
        created->Release();
    }

    LogWarning("[UnityRHI][DLSS-G] FG proxy wrap failed; creating Unity's original BufferCount=%u.",
        unityCount);
    return original(factory, device, hwnd, desc, fullscreenDesc, restrictToOutput, swapChain);
}

HRESULT STDMETHODCALLTYPE HookedPresent(
    IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    PresentFn original;
    Present1Fn original1;
    ComPtr<ID3D12CommandQueue> queue;
    {
        std::lock_guard lock(g_Hooks.mutex);
        original = g_Hooks.present;
        original1 = g_Hooks.present1;
        queue = g_Hooks.queue;
    }
    return PresentWithoutInsertion(
        swapChain, queue.Get(), syncInterval, flags, original, original1, nullptr);
}

HRESULT STDMETHODCALLTYPE HookedPresent1(
    IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters)
{
    PresentFn original;
    Present1Fn original1;
    ComPtr<ID3D12CommandQueue> queue;
    {
        std::lock_guard lock(g_Hooks.mutex);
        original = g_Hooks.present;
        original1 = g_Hooks.present1;
        queue = g_Hooks.queue;
    }
    return PresentWithoutInsertion(
        swapChain, queue.Get(), syncInterval, flags, original, original1, parameters);
}
} // namespace

bool InstallFrameGenerationHooks()
{
    std::lock_guard lock(g_Hooks.mutex);
    if (g_Hooks.factoryInstalled)
        return true;

    ComPtr<IDXGIFactory2> factory;
    const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        LogError("[UnityRHI][DLSS-G] CreateDXGIFactory1 for early hook failed (0x%08X).",
            static_cast<unsigned>(hr));
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(factory.Get());
    g_Hooks.createSwapChainSlot = &vtable[kFactoryCreateSwapChain];
    g_Hooks.createSwapChainForHwndSlot = &vtable[kFactoryCreateSwapChainForHwnd];
    g_Hooks.createSwapChain =
        reinterpret_cast<CreateSwapChainFn>(*g_Hooks.createSwapChainSlot);
    g_Hooks.createSwapChainForHwnd =
        reinterpret_cast<CreateSwapChainForHwndFn>(*g_Hooks.createSwapChainForHwndSlot);

    if (!WriteSlot(g_Hooks.createSwapChainSlot,
            reinterpret_cast<void*>(&HookedCreateSwapChain)) ||
        !WriteSlot(g_Hooks.createSwapChainForHwndSlot,
            reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd)))
    {
        RestoreSlot(g_Hooks.createSwapChainSlot, g_Hooks.createSwapChain,
            reinterpret_cast<void*>(&HookedCreateSwapChain));
        RestoreSlot(g_Hooks.createSwapChainForHwndSlot,
            g_Hooks.createSwapChainForHwnd,
            reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd));
        LogError("[UnityRHI][DLSS-G] Failed to patch DXGI factory vtable.");
        return false;
    }

    g_Hooks.factoryInstalled = true;
    LogInfo("[UnityRHI][DLSS-G] Installed early DXGI factory hooks (Player only).");
    return true;
}

void AdoptFrameGenerationSwapChain(
    IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue)
{
    if (!swapChain)
        return;
    if (IsFrameGenerationSwapChainProxy(swapChain))
    {
        SetFrameGenerationProxyPresentQueue(swapChain, presentQueue);
        LogInfo("[UnityRHI][DLSS-G] Adopted FG proxy swapchain (Unity Present stays 1:1).");
        return;
    }
    AdoptFallback(swapChain, presentQueue);
}

void ResetFrameGenerationHookDeviceObjects()
{
    std::lock_guard lock(g_Hooks.mutex);
    g_Hooks.queue.Reset();
}

void ShutdownFrameGenerationHooks()
{
    std::lock_guard lock(g_Hooks.mutex);
    g_Hooks.queue.Reset();
    RestoreSlot(g_Hooks.presentSlot, g_Hooks.present,
        reinterpret_cast<void*>(&HookedPresent));
    RestoreSlot(g_Hooks.present1Slot, g_Hooks.present1,
        reinterpret_cast<void*>(&HookedPresent1));
    RestoreSlot(g_Hooks.createSwapChainSlot, g_Hooks.createSwapChain,
        reinterpret_cast<void*>(&HookedCreateSwapChain));
    RestoreSlot(g_Hooks.createSwapChainForHwndSlot,
        g_Hooks.createSwapChainForHwnd,
        reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd));
    g_Hooks.factoryInstalled = false;
    g_Hooks.createSwapChain = nullptr;
    g_Hooks.createSwapChainForHwnd = nullptr;
    g_Hooks.present = nullptr;
    g_Hooks.present1 = nullptr;
}

bool IsFrameGenerationPresentHookInstalled()
{
    if (HasFrameGenerationSwapChainProxy())
        return true;
    std::lock_guard lock(g_Hooks.mutex);
    return g_Hooks.presentSlot &&
           *g_Hooks.presentSlot == reinterpret_cast<void*>(&HookedPresent);
}
} // namespace unityrhi
