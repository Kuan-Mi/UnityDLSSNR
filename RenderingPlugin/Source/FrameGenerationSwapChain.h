#pragma once

#include <cstdint>
#include <windows.h>

struct ID3D12CommandQueue;
struct IDXGISwapChain;
struct IDXGISwapChain1;
struct IUnknown;

namespace unityrhi
{
// COM proxy that Streamline's slUpgradeInterface would otherwise provide:
// Unity sees the requested BufferCount / GetBuffer / GetCurrentBackBufferIndex
// 1:1, while the plugin owns the real DXGI swapchain and can Present twice.
HRESULT WrapFrameGenerationSwapChain(
    IDXGISwapChain* real,
    IUnknown* queueObject,
    uint32_t unityBufferCount,
    IDXGISwapChain** wrapped);

HRESULT WrapFrameGenerationSwapChain1(
    IDXGISwapChain1* real,
    IUnknown* queueObject,
    uint32_t unityBufferCount,
    IDXGISwapChain1** wrapped);

bool IsFrameGenerationSwapChainProxy(IUnknown* swapChain);
void SetFrameGenerationProxyPresentQueue(IUnknown* swapChain, ID3D12CommandQueue* queue);
bool HasFrameGenerationSwapChainProxy();

// Clears CPU pacing EMA so toggling FG does not inherit a stale interval.
void ResetFrameGenerationPacing();
}
