#pragma once

#include <atomic>
#include <iostream>
#include "d3dx12.h"
#include "dxgi.h"

#include "NRI.h"
#include "Extensions/NRIWrapperD3D12.h"
#include "Extensions/NRIUpscaler.h"

#include "IUnityGraphicsD3D12.h"
#include "IUnityGraphics.h"
#include "IUnityLog.h"

class RenderSystem
{
public:
    static RenderSystem& Get();

    void Initialize(IUnityInterfaces* interfaces);
    void Shutdown();
    void Release(nri::Texture* texture);

    RenderSystem();
    ~RenderSystem();

    ID3D12Device* GetDevice() const { return device; }
    nri::Device* GetNriDevice() const { return m_NriDevice; }
    nri::CoreInterface& GetNriCore() { return m_NriCore; }
    nri::UpscalerInterface& GetNriUpScaler() { return m_NriUpScaler; }
    nri::WrapperD3D12Interface& GetNriWrapper() { return m_NriWrapper; }
    IUnityGraphicsD3D12v7* GetD3D12() const { return s_d3d12; }

    void ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces);
    nri::Texture* WrapD3D12Texture(ID3D12Resource* resource, DXGI_FORMAT format) const;

private:
    static constexpr int kMaxFramesInFlight = 3;

    IUnityInterfaces* m_UnityInterfaces = nullptr;
    IUnityGraphicsD3D12v7* s_d3d12 = nullptr;
    IUnityLog* s_Log = nullptr;

    ID3D12Device* device;

    // NRI
    nri::Device* m_NriDevice = nullptr;
    
    

    nri::CoreInterface m_NriCore = {};
    nri::UpscalerInterface m_NriUpScaler = {};
    nri::WrapperD3D12Interface m_NriWrapper = {};

    std::atomic<bool> m_are_resources_initialized{false};
};
