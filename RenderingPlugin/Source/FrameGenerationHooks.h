#pragma once

struct ID3D12CommandQueue;
struct IDXGISwapChain;

namespace unityrhi
{
// Player-only DXGI interception. Install must run from UnityPluginLoad while
// UnityRHI is preloaded, before Unity creates its swapchain.
bool InstallFrameGenerationHooks();

// Fallback/adoption path for Unity versions which expose an already-created
// swapchain through IUnityGraphicsD3D12v7/v8.
void AdoptFrameGenerationSwapChain(
    IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue);

// Releases device-owned references but leaves the early factory hook active so
// a subsequent D3D12 device/swapchain recreation can be captured.
void ResetFrameGenerationHookDeviceObjects();

// Restores every patched vtable slot. Only call when the plugin unloads.
void ShutdownFrameGenerationHooks();

bool IsFrameGenerationPresentHookInstalled();
}
