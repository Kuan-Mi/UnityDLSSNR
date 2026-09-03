#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "FrameGenerationSwapChain.h"

#include "FrameGenerationDebug.h"
#include "UnityRhiLog.h"

namespace unityrhi
{
namespace
{
using Microsoft::WRL::ComPtr;

// {B3E91C40-7A12-4F8D-9C55-1E8A6D4B2F70}
static const GUID IID_IUnityRhiFgSwapChain =
{ 0xb3e91c40, 0x7a12, 0x4f8d, { 0x9c, 0x55, 0x1e, 0x8a, 0x6d, 0x4b, 0x2f, 0x70 } };

std::atomic<int> g_ActiveProxies{0};

struct PacingState
{
    int64_t frequency = 0;
    // QPC when the previous PresentInternal returned to Unity.
    int64_t lastPresentEndQpc = 0;
    // EMA of (Unity work gap + Evaluate duration). Pacing must NOT include
    // WaitForPacer / WaitUntilQpc, or Present-to-Present feedback collapses FPS.
    int64_t frameWorkEma = 0;
    uint64_t logCounter = 0;
};

PacingState g_Pacing;

void EnsurePacingFrequency()
{
    if (g_Pacing.frequency != 0)
        return;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_Pacing.frequency = frequency.QuadPart;
}

int64_t QpcNow()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double TicksToMs(int64_t ticks)
{
    if (g_Pacing.frequency <= 0 || ticks <= 0)
        return 0.0;
    return (1000.0 * static_cast<double>(ticks)) /
        static_cast<double>(g_Pacing.frequency);
}

void NoteFrameWorkSample(int64_t unityGapTicks, int64_t evaluateTicks)
{
    EnsurePacingFrequency();
    int64_t work = unityGapTicks + evaluateTicks;
    if (work < 0)
        work = 0;
    if (g_Pacing.frameWorkEma <= 0)
        g_Pacing.frameWorkEma = work;
    else
        g_Pacing.frameWorkEma = (g_Pacing.frameWorkEma * 3 + work) / 4;
}

int64_t HalfFrameTicks()
{
    EnsurePacingFrequency();
    if (g_Pacing.frequency <= 0)
        return 0;
    // Default ~8.3ms half-interval until the EMA has a real sample.
    int64_t half = g_Pacing.frameWorkEma > 0 ?
        g_Pacing.frameWorkEma / 2 : g_Pacing.frequency / 120;
    const int64_t minTicks = g_Pacing.frequency / 2000; // 0.5ms
    // Cap at 100ms half so a deliberate low render rate (e.g. ~10 FPS sleep)
    // still paces generated and real evenly. The old 33ms ceiling made the
    // generated frame flash for ~1/3 of a 100ms Unity frame.
    const int64_t maxTicks = g_Pacing.frequency / 10; // 100ms
    if (half < minTicks)
        half = minTicks;
    if (half > maxTicks)
        half = maxTicks;
    return half;
}

void WaitUntilQpc(int64_t target, const std::atomic<bool>* stop)
{
    EnsurePacingFrequency();
    if (g_Pacing.frequency <= 0 || target <= 0)
        return;
    for (;;)
    {
        if (stop && stop->load(std::memory_order_acquire))
            return;
        const int64_t now = QpcNow();
        if (now >= target)
            return;
        const int64_t remainMs = (target - now) * 1000 / g_Pacing.frequency;
        if (remainMs >= 2)
            Sleep(static_cast<DWORD>(remainMs - 1));
        else
            SwitchToThread();
    }
}

bool CanUseClearValue(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

D3D12_RESOURCE_BARRIER Transition(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

class SwapChainProxy final : public IDXGISwapChain4
{
public:
    SwapChainProxy()
    {
        g_ActiveProxies.fetch_add(1, std::memory_order_relaxed);
    }

    ~SwapChainProxy()
    {
        StopPacer();
        WaitGpu();
        if (fenceEvent_)
            CloseHandle(fenceEvent_);
        g_ActiveProxies.fetch_sub(1, std::memory_order_relaxed);
    }

    HRESULT Initialize(
        IDXGISwapChain* real, ID3D12CommandQueue* queue, UINT unityBufferCount)
    {
        if (!real || !queue || unityBufferCount == 0)
            return E_INVALIDARG;

        queue_ = queue;
        HRESULT hr = queue_->GetDevice(IID_PPV_ARGS(&device_));
        if (FAILED(hr) || !device_)
            return FAILED(hr) ? hr : E_FAIL;

        real_ = real;
        real_->QueryInterface(IID_PPV_ARGS(&real1_));
        real_->QueryInterface(IID_PPV_ARGS(&real2_));
        real_->QueryInterface(IID_PPV_ARGS(&real3_));
        real_->QueryInterface(IID_PPV_ARGS(&real4_));
        if (!real1_ || !real3_)
            return E_NOINTERFACE;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        hr = real1_->GetDesc1(&desc);
        if (FAILED(hr))
            return hr;

        unityBufferCount_ = unityBufferCount;
        unityIndex_ = 0;
        unityPresentCount_ = 0;

        hr = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyAllocator_));
        if (FAILED(hr))
            return hr;
        hr = device_->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyAllocator_.Get(), nullptr,
            IID_PPV_ARGS(&copyList_));
        if (FAILED(hr))
            return hr;
        copyList_->Close();
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(hr))
            return hr;
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_)
            return HRESULT_FROM_WIN32(GetLastError());

        if (real2_)
            real2_->SetMaximumFrameLatency(std::max(3u, unityBufferCount_));

        hr = CreateUnityBuffers(desc.Width, desc.Height, desc.Format);
        if (FAILED(hr))
            return hr;

        try
        {
            pacerThread_ = std::thread(&SwapChainProxy::PacerLoop, this);
        }
        catch (...)
        {
            LogError("[UnityRHI][DLSS-G] Failed to start FG present pacer thread.");
            return E_FAIL;
        }

        LogInfo("[UnityRHI][DLSS-G] Unity now presents through FG proxy "
            "(Unity BufferCount=%u, real BufferCount=%u, %ux%u).",
            unityBufferCount_, desc.BufferCount, desc.Width, desc.Height);
        LogInfo("[UnityRHI][DLSS-G] FG presenter thread started; generated and retained "
            "frames Present asynchronously from Unity's Present.");
        return S_OK;
    }

    void SetQueue(ID3D12CommandQueue* queue)
    {
        if (queue)
            queue_ = queue;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject)
            return E_POINTER;
        if (riid == IID_IUnknown ||
            riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) ||
            riid == __uuidof(IDXGISwapChain1) ||
            riid == __uuidof(IDXGISwapChain2) ||
            riid == __uuidof(IDXGISwapChain3) ||
            riid == __uuidof(IDXGISwapChain4) ||
            riid == IID_IUnityRhiFgSwapChain)
        {
            *ppvObject = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining =
            static_cast<ULONG>(refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(
        REFGUID name, UINT dataSize, const void* pData) override
    {
        return real_->SetPrivateData(name, dataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
        REFGUID name, const IUnknown* pUnknown) override
    {
        return real_->SetPrivateDataInterface(name, pUnknown);
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(
        REFGUID name, UINT* pDataSize, void* pData) override
    {
        return real_->GetPrivateData(name, pDataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override
    {
        return real_->GetParent(riid, ppParent);
    }

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override
    {
        return real_->GetDevice(riid, ppDevice);
    }

    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override
    {
        return PresentInternal(syncInterval, flags, nullptr);
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void** ppSurface) override
    {
        if (!ppSurface)
            return E_POINTER;
        std::lock_guard lock(mutex_);
        if (buffer >= unityBuffers_.size() || !unityBuffers_[buffer])
            return DXGI_ERROR_INVALID_CALL;
        return unityBuffers_[buffer]->QueryInterface(riid, ppSurface);
    }

    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* pTarget) override
    {
        WaitForPacer();
        return real_->SetFullscreenState(fullscreen, pTarget);
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override
    {
        return real_->GetFullscreenState(pFullscreen, ppTarget);
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override
    {
        const HRESULT hr = real_->GetDesc(pDesc);
        if (SUCCEEDED(hr) && pDesc)
            pDesc->BufferCount = unityBufferCount_;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(
        UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT flags) override
    {
        WaitForPacer();
        std::lock_guard lock(mutex_);
        WaitGpu();
        const UINT unityCount = bufferCount == 0 ? unityBufferCount_ : bufferCount;
        DXGI_SWAP_CHAIN_DESC1 current{};
        real1_->GetDesc1(&current);
        const UINT realCount = current.BufferCount == 0 ? unityCount * 2 : current.BufferCount;
        const HRESULT hr = real_->ResizeBuffers(realCount, width, height, newFormat, flags);
        if (FAILED(hr))
            return hr;
        unityBufferCount_ = unityCount;
        unityIndex_ = 0;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        real1_->GetDesc1(&desc);
        return CreateUnityBuffers(desc.Width, desc.Height, desc.Format);
    }

    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override
    {
        return real_->ResizeTarget(pNewTargetParameters);
    }

    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override
    {
        return real_->GetContainingOutput(ppOutput);
    }

    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override
    {
        const HRESULT hr = real_->GetFrameStatistics(pStats);
        if (SUCCEEDED(hr) && pStats)
            pStats->PresentCount = unityPresentCount_;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override
    {
        if (!pLastPresentCount)
            return E_POINTER;
        *pLastPresentCount = unityPresentCount_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override
    {
        const HRESULT hr = real1_->GetDesc1(pDesc);
        if (SUCCEEDED(hr) && pDesc)
            pDesc->BufferCount = unityBufferCount_;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override
    {
        return real1_->GetFullscreenDesc(pDesc);
    }

    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override
    {
        return real1_->GetHwnd(pHwnd);
    }

    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override
    {
        return real1_->GetCoreWindow(refiid, ppUnk);
    }

    HRESULT STDMETHODCALLTYPE Present1(
        UINT syncInterval, UINT presentFlags,
        const DXGI_PRESENT_PARAMETERS* pPresentParameters) override
    {
        return PresentInternal(syncInterval, presentFlags, pPresentParameters);
    }

    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override
    {
        return real1_->IsTemporaryMonoSupported();
    }

    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override
    {
        return real1_->GetRestrictToOutput(ppRestrictToOutput);
    }

    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override
    {
        return real1_->SetBackgroundColor(pColor);
    }

    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override
    {
        return real1_->GetBackgroundColor(pColor);
    }

    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override
    {
        return real1_->SetRotation(rotation);
    }

    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override
    {
        return real1_->GetRotation(pRotation);
    }

    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override
    {
        return real2_ ? real2_->SetSourceSize(width, height) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override
    {
        return real2_ ? real2_->GetSourceSize(pWidth, pHeight) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT maxLatency) override
    {
        return real2_ ? real2_->SetMaximumFrameLatency(maxLatency) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override
    {
        return real2_ ? real2_->GetMaximumFrameLatency(pMaxLatency) : E_NOINTERFACE;
    }

    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
    {
        return real2_ ? real2_->GetFrameLatencyWaitableObject() : nullptr;
    }

    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override
    {
        return real2_ ? real2_->SetMatrixTransform(pMatrix) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override
    {
        return real2_ ? real2_->GetMatrixTransform(pMatrix) : E_NOINTERFACE;
    }

    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override
    {
        std::lock_guard lock(mutex_);
        return unityIndex_;
    }

    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(
        DXGI_COLOR_SPACE_TYPE colorSpace, UINT* pColorSpaceSupport) override
    {
        return real3_->CheckColorSpaceSupport(colorSpace, pColorSpaceSupport);
    }

    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE colorSpace) override
    {
        return real3_->SetColorSpace1(colorSpace);
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers1(
        UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT swapChainFlags,
        const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) override
    {
        WaitForPacer();
        std::lock_guard lock(mutex_);
        WaitGpu();
        const UINT unityCount = bufferCount == 0 ? unityBufferCount_ : bufferCount;
        DXGI_SWAP_CHAIN_DESC1 current{};
        real1_->GetDesc1(&current);
        const UINT realCount = current.BufferCount == 0 ? unityCount * 2 : current.BufferCount;
        const HRESULT hr = real3_->ResizeBuffers1(
            realCount, width, height, format, swapChainFlags, pCreationNodeMask, ppPresentQueue);
        if (FAILED(hr))
            return hr;
        if (ppPresentQueue && ppPresentQueue[0])
        {
            ComPtr<ID3D12CommandQueue> queue;
            if (SUCCEEDED(ppPresentQueue[0]->QueryInterface(IID_PPV_ARGS(&queue))) && queue)
                queue_ = queue;
        }
        unityBufferCount_ = unityCount;
        unityIndex_ = 0;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        real1_->GetDesc1(&desc);
        return CreateUnityBuffers(desc.Width, desc.Height, desc.Format);
    }

    HRESULT STDMETHODCALLTYPE SetHDRMetaData(
        DXGI_HDR_METADATA_TYPE type, UINT size, void* pMetaData) override
    {
        return real4_ ? real4_->SetHDRMetaData(type, size, pMetaData) : E_NOINTERFACE;
    }

private:
    HRESULT CreateUnityBuffers(UINT width, UINT height, DXGI_FORMAT format)
    {
        unityBuffers_.clear();
        unityBuffers_.resize(unityBufferCount_);
        if (width == 0 || height == 0)
            return E_INVALIDARG;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        const D3D12_CLEAR_VALUE* pClear = CanUseClearValue(format) ? &clear : nullptr;

        for (UINT i = 0; i < unityBufferCount_; ++i)
        {
            const HRESULT hr = device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                pClear, IID_PPV_ARGS(&unityBuffers_[i]));
            if (FAILED(hr))
            {
                LogError("[UnityRHI][DLSS-G] Failed to create FG proxy backbuffer %u (0x%08X).",
                    i, static_cast<unsigned>(hr));
                unityBuffers_.clear();
                return hr;
            }
            wchar_t name[64];
            swprintf_s(name, L"UnityRHI FG UnityBackbuffer %u", i);
            unityBuffers_[i]->SetName(name);
        }
        return S_OK;
    }

    void WaitGpu()
    {
        if (!queue_ || !fence_)
            return;
        const uint64_t value = ++fenceValue_;
        if (FAILED(queue_->Signal(fence_.Get(), value)))
            return;
        if (fence_->GetCompletedValue() >= value)
            return;
        if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_)))
            return;
        WaitForSingleObject(fenceEvent_, 5000);
    }

    HRESULT BlitColorToReal(ID3D12Resource* color)
    {
        if (!color || !queue_ || !real3_ || !copyList_ || !copyAllocator_)
            return E_FAIL;
        if (fence_->GetCompletedValue() < fenceValue_)
        {
            if (FAILED(fence_->SetEventOnCompletion(fenceValue_, fenceEvent_)))
                return E_FAIL;
            if (WaitForSingleObject(fenceEvent_, 5000) != WAIT_OBJECT_0)
                return E_FAIL;
        }

        ComPtr<ID3D12Resource> dest;
        const UINT index = real3_->GetCurrentBackBufferIndex();
        HRESULT hr = real3_->GetBuffer(index, IID_PPV_ARGS(&dest));
        if (FAILED(hr) || !dest)
            return FAILED(hr) ? hr : E_FAIL;

        auto srcDesc = color->GetDesc();
        auto dstDesc = dest->GetDesc();
        if (srcDesc.Width != dstDesc.Width ||
            srcDesc.Height != dstDesc.Height ||
            srcDesc.Format != dstDesc.Format)
        {
            LogError("[UnityRHI][DLSS-G] FG blit size/format mismatch (%ux%u %u -> %ux%u %u).",
                static_cast<unsigned>(srcDesc.Width), srcDesc.Height,
                static_cast<unsigned>(srcDesc.Format),
                static_cast<unsigned>(dstDesc.Width), dstDesc.Height,
                static_cast<unsigned>(dstDesc.Format));
            return E_INVALIDARG;
        }

        hr = copyAllocator_->Reset();
        if (SUCCEEDED(hr))
            hr = copyList_->Reset(copyAllocator_.Get(), nullptr);
        if (FAILED(hr))
            return hr;

        D3D12_RESOURCE_BARRIER before[] = {
            Transition(color, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(dest.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        copyList_->ResourceBarrier(2, before);
        copyList_->CopyResource(dest.Get(), color);
        D3D12_RESOURCE_BARRIER after[] = {
            Transition(dest.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
            Transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
        };
        copyList_->ResourceBarrier(2, after);
        hr = copyList_->Close();
        if (FAILED(hr))
            return hr;

        ID3D12CommandList* lists[] = {copyList_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const uint64_t value = ++fenceValue_;
        return queue_->Signal(fence_.Get(), value);
    }

    enum class PresentKind
    {
        Other = 0,
        Generated = 1,
        Real = 2,
    };

    HRESULT CallRealPresent(
        UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters,
        PresentKind kind = PresentKind::Other)
    {
        HRESULT hr;
        if (parameters && real1_)
            hr = real1_->Present1(syncInterval, flags, parameters);
        else
            hr = real_->Present(syncInterval, flags);
        if (SUCCEEDED(hr) && (flags & DXGI_PRESENT_TEST) == 0)
        {
            if (kind == PresentKind::Generated)
                NoteGeneratedPresent();
            else if (kind == PresentKind::Real)
                NoteRealPresent();
            else
                NoteDisplayedPresent();
        }
        return hr;
    }

    HRESULT PresentInternal(
        UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
    {
        if (flags & DXGI_PRESENT_TEST)
            return CallRealPresent(syncInterval, flags, parameters);

        EnsurePacingFrequency();
        const int64_t presentEntryQpc = QpcNow();
        const int64_t unityGapTicks = (g_Pacing.lastPresentEndQpc > 0) ?
            (presentEntryQpc - g_Pacing.lastPresentEndQpc) : 0;

        // Finish the previous retained Present before Evaluate writes the next
        // generated frame onto the real backbuffer (and before overwriting OutputReal).
        const int64_t waitPacerStart = QpcNow();
        WaitForPacer();
        const int64_t waitPacerTicks = QpcNow() - waitPacerStart;

        ID3D12Resource* color = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (unityIndex_ < unityBuffers_.size())
                color = unityBuffers_[unityIndex_].Get();
        }

        FrameGenerationPresentAction action = FrameGenerationPresentAction::None;
        FrameGenerationPresentInfo presentInfo{};
        int64_t evaluateTicks = 0;
        if (color && queue_ && real3_)
        {
            const int64_t evaluateStart = QpcNow();
            action = EvaluateFrameGeneration(real3_.Get(), queue_.Get(), color, &presentInfo);
            evaluateTicks = QpcNow() - evaluateStart;
        }

        HRESULT result = E_FAIL;
        int64_t halfTicks = 0;
        if (action == FrameGenerationPresentAction::Insert)
        {
            static bool loggedSyncOverride = false;
            if (syncInterval != 0 && !loggedSyncOverride)
            {
                loggedSyncOverride = true;
                LogInfo("[UnityRHI][DLSS-G] 2x insertion forcing syncInterval=0 (Unity requested %u).",
                    syncInterval);
            }

            if (presentInfo.realSlot == UINT32_MAX || presentInfo.readyFenceValue == 0)
            {
                LogError("[UnityRHI][DLSS-G] Evaluate succeeded without presenter synchronization data.");
                g_Pacing.lastPresentEndQpc = QpcNow();
                return E_FAIL;
            }

            NoteFrameWorkSample(unityGapTicks, evaluateTicks);
            halfTicks = HalfFrameTicks();
            SchedulePacer(halfTicks, flags, presentInfo.realSlot,
                presentInfo.readyFenceValue);
            result = S_OK;
        }
        else
        {
            if (action != FrameGenerationPresentAction::Replace && color)
            {
                const HRESULT blit = BlitColorToReal(color);
                if (FAILED(blit))
                    LogError("[UnityRHI][DLSS-G] Failed to blit Unity color onto the real swapchain (0x%08X).",
                        static_cast<unsigned>(blit));
            }
            result = CallRealPresent(syncInterval, flags, parameters);
            if (FAILED(result))
                LogError("[UnityRHI][DLSS-G] Swapchain Present failed (0x%08X).",
                    static_cast<unsigned>(result));
        }

        {
            std::lock_guard lock(mutex_);
            if (unityBufferCount_ > 0)
                unityIndex_ = (unityIndex_ + 1) % unityBufferCount_;
            ++unityPresentCount_;
        }

        if (action == FrameGenerationPresentAction::Insert &&
            (++g_Pacing.logCounter % 60) == 0)
        {
            const uint64_t gen = GetGeneratedPresentCount();
            const uint64_t real = GetRealPresentCount();
            const double ratio = (real > 0) ? (static_cast<double>(gen) / static_cast<double>(real)) : 0.0;
            LogInfo(
                "[UnityRHI][DLSS-G] pace: unity=%.2fms eval=%.2fms waitPacer=%.2fms "
                "half=%.2fms ema=%.2fms gen=%llu real=%llu ratio=%.2f",
                TicksToMs(unityGapTicks),
                TicksToMs(evaluateTicks),
                TicksToMs(waitPacerTicks),
                TicksToMs(halfTicks),
                TicksToMs(g_Pacing.frameWorkEma),
                static_cast<unsigned long long>(gen),
                static_cast<unsigned long long>(real),
                ratio);
        }

        g_Pacing.lastPresentEndQpc = QpcNow();
        return result;
    }

    void WaitForPacer()
    {
        std::unique_lock lock(pacerMutex_);
        pacerDoneCv_.wait(lock, [this] {
            return !pacerBusy_ || pacerStop_.load(std::memory_order_acquire);
        });
    }

    void SchedulePacer(
        int64_t halfTicks, UINT flags, uint32_t realSlot,
        uint64_t readyFenceValue)
    {
        {
            std::lock_guard lock(pacerMutex_);
            pacerHalfTicks_ = halfTicks;
            pacerFlags_ = flags;
            pacerRealSlot_ = realSlot;
            pacerReadyFenceValue_ = readyFenceValue;
            pacerPending_ = true;
            pacerBusy_ = true;
        }
        pacerCv_.notify_one();
    }

    void StopPacer()
    {
        {
            std::lock_guard lock(pacerMutex_);
            pacerStop_.store(true, std::memory_order_release);
        }
        pacerCv_.notify_all();
        pacerDoneCv_.notify_all();
        if (pacerThread_.joinable())
            pacerThread_.join();
        pacerBusy_ = false;
        pacerPending_ = false;
    }

    void PacerLoop()
    {
        for (;;)
        {
            int64_t halfTicks = 0;
            UINT flags = 0;
            uint32_t realSlot = UINT32_MAX;
            uint64_t readyFenceValue = 0;
            {
                std::unique_lock lock(pacerMutex_);
                pacerCv_.wait(lock, [this] { return pacerStop_.load(std::memory_order_acquire) || pacerPending_; });
                if (pacerStop_.load(std::memory_order_acquire) && !pacerPending_)
                    break;
                halfTicks = pacerHalfTicks_;
                flags = pacerFlags_;
                realSlot = pacerRealSlot_;
                readyFenceValue = pacerReadyFenceValue_;
                pacerPending_ = false;
            }

            if (!WaitForFrameGenerationFence(readyFenceValue))
            {
                LogError("[UnityRHI][DLSS-G] Timed out waiting for generated-frame GPU work.");
            }
            const HRESULT generated =
                CallRealPresent(0, flags, nullptr, PresentKind::Generated);
            int64_t target = QpcNow();
            if (FAILED(generated))
            {
                LogError("[UnityRHI][DLSS-G] Present of generated frame failed (0x%08X).",
                    static_cast<unsigned>(generated));
            }
            else
            {
                const int64_t genFlipQpc = QpcNow();
                target = genFlipQpc + halfTicks;
            }

            WaitUntilQpc(target, &pacerStop_);
            bool copied = false;
            if (realSlot != UINT32_MAX)
                copied = CopyFrameGenerationRetainedReal(real3_.Get(), queue_.Get(), realSlot);
            if (!copied)
            {
                LogError("[UnityRHI][DLSS-G] Failed to copy retained real frame into the next backbuffer.");
            }
            else
            {
                const HRESULT hr = CallRealPresent(0, flags, nullptr, PresentKind::Real);
                if (FAILED(hr))
                {
                    LogError("[UnityRHI][DLSS-G] Present of retained real frame failed (0x%08X).",
                        static_cast<unsigned>(hr));
                }
            }

            {
                std::lock_guard lock(pacerMutex_);
                pacerBusy_ = false;
            }
            pacerDoneCv_.notify_all();
            if (pacerStop_.load(std::memory_order_acquire))
                break;
        }
    }

    std::mutex mutex_;
    std::atomic<ULONG> refCount_{1};
    ComPtr<IDXGISwapChain> real_;
    ComPtr<IDXGISwapChain1> real1_;
    ComPtr<IDXGISwapChain2> real2_;
    ComPtr<IDXGISwapChain3> real3_;
    ComPtr<IDXGISwapChain4> real4_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> copyAllocator_;
    ComPtr<ID3D12GraphicsCommandList> copyList_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    uint64_t fenceValue_ = 0;
    std::vector<ComPtr<ID3D12Resource>> unityBuffers_;
    UINT unityBufferCount_ = 0;
    UINT unityIndex_ = 0;
    UINT unityPresentCount_ = 0;

    std::mutex pacerMutex_;
    std::condition_variable pacerCv_;
    std::condition_variable pacerDoneCv_;
    std::thread pacerThread_;
    std::atomic<bool> pacerStop_{false};
    bool pacerPending_ = false;
    bool pacerBusy_ = false;
    int64_t pacerHalfTicks_ = 0;
    UINT pacerFlags_ = 0;
    uint32_t pacerRealSlot_ = UINT32_MAX;
    uint64_t pacerReadyFenceValue_ = 0;
};
} // namespace

HRESULT WrapFrameGenerationSwapChain(
    IDXGISwapChain* real,
    IUnknown* queueObject,
    uint32_t unityBufferCount,
    IDXGISwapChain** wrapped)
{
    if (!real || !queueObject || !wrapped || unityBufferCount == 0)
        return E_INVALIDARG;
    *wrapped = nullptr;

    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(queueObject->QueryInterface(IID_PPV_ARGS(&queue))) || !queue)
        return E_NOINTERFACE;

    auto* proxy = new (std::nothrow) SwapChainProxy();
    if (!proxy)
        return E_OUTOFMEMORY;
    const HRESULT hr = proxy->Initialize(real, queue.Get(), unityBufferCount);
    if (FAILED(hr))
    {
        proxy->Release();
        return hr;
    }
    *wrapped = proxy;
    return S_OK;
}

HRESULT WrapFrameGenerationSwapChain1(
    IDXGISwapChain1* real,
    IUnknown* queueObject,
    uint32_t unityBufferCount,
    IDXGISwapChain1** wrapped)
{
    if (!wrapped)
        return E_POINTER;
    *wrapped = nullptr;
    IDXGISwapChain* wrapped0 = nullptr;
    const HRESULT hr = WrapFrameGenerationSwapChain(
        real, queueObject, unityBufferCount, &wrapped0);
    if (FAILED(hr) || !wrapped0)
        return FAILED(hr) ? hr : E_FAIL;
    const HRESULT qi = wrapped0->QueryInterface(IID_PPV_ARGS(wrapped));
    wrapped0->Release();
    return qi;
}

bool IsFrameGenerationSwapChainProxy(IUnknown* swapChain)
{
    if (!swapChain)
        return false;
    ComPtr<IUnknown> probe;
    return SUCCEEDED(swapChain->QueryInterface(IID_IUnityRhiFgSwapChain, &probe));
}

void SetFrameGenerationProxyPresentQueue(IUnknown* swapChain, ID3D12CommandQueue* queue)
{
    if (!swapChain || !queue)
        return;
    ComPtr<IDXGISwapChain4> as4;
    if (FAILED(swapChain->QueryInterface(IID_IUnityRhiFgSwapChain, &as4)) || !as4)
        return;
    static_cast<SwapChainProxy*>(as4.Get())->SetQueue(queue);
}

bool HasFrameGenerationSwapChainProxy()
{
    return g_ActiveProxies.load(std::memory_order_relaxed) > 0;
}

void ResetFrameGenerationPacing()
{
    g_Pacing.lastPresentEndQpc = 0;
    g_Pacing.frameWorkEma = 0;
    g_Pacing.logCounter = 0;
}
}
