#include "RenderSystem.h"
#include "D3D12HeapHook.h"

#define LOG(msg) UNITY_LOG(s_Log, msg)

RenderSystem& RenderSystem::Get()
{
    static RenderSystem instance;
    return instance;
}

void RenderSystem::Initialize(IUnityInterfaces* interfaces)
{
    if (m_are_resources_initialized)
        return;

    m_UnityInterfaces = interfaces;
    s_d3d12 = interfaces->Get<IUnityGraphicsD3D12v7>();
    s_Log = interfaces->Get<IUnityLog>();

    if (!s_d3d12)
    {
        LOG("[NRD Native] IUnityGraphicsD3D12v7 not available, skipping initialization.");
        return;
    }
    device = s_d3d12->GetDevice();

    // Install the SetDescriptorHeaps vtable hook so we can restore Unity's
    // heaps after NRD/DLRR dispatches. Idempotent — safe even if
    // NativeRenderPlugin already installed it (shared DLL, single globals).
    D3D12HeapHook::InstallHookFromDevice(device);

    // Provide Unity's graphics command queue to NRI so GetQueue(GRAPHICS) works
    ID3D12CommandQueue* d3d12GraphicsQueue = s_d3d12->GetCommandQueue();

    nri::QueueFamilyD3D12Desc queueFamily = {};
    queueFamily.d3d12Queues = &d3d12GraphicsQueue;
    queueFamily.queueNum    = 1;
    queueFamily.queueType   = nri::QueueType::GRAPHICS;

    nri::DeviceCreationD3D12Desc deviceDesc = {};
    deviceDesc.d3d12Device     = device;
    deviceDesc.queueFamilies   = &queueFamily;
    deviceDesc.queueFamilyNum  = 1;
    deviceDesc.disableD3D12EnhancedBarriers = true;
    deviceDesc.enableNRIValidation = true;


    nri::Result result = nriCreateDeviceFromD3D12Device(deviceDesc, m_NriDevice);
    if (result != nri::Result::SUCCESS)
    {
        LOG("[NRD Native] Failed to create NRI device from D3D12");
        return;
    }

    nriGetInterface(*m_NriDevice, NRI_INTERFACE(nri::CoreInterface), &m_NriCore);
    nriGetInterface(*m_NriDevice, NRI_INTERFACE(nri::WrapperD3D12Interface), &m_NriWrapper);
    nriGetInterface(*m_NriDevice, NRI_INTERFACE(nri::UpscalerInterface), &m_NriUpScaler);

    UnityGraphicsD3D12PhysicalVideoMemoryControlValues control_values;
    control_values.reservation = 64000000;
    control_values.systemMemoryThreshold = 64000000;
    control_values.residencyHysteresisThreshold = 128000000;
    control_values.nonEvictableRelativeThreshold = 0.25;
    s_d3d12->SetPhysicalVideoMemoryControlValues(&control_values);

    m_are_resources_initialized = true;

    LOG("[NRD Native] RenderSystem Initialized.");
}

void RenderSystem::Shutdown()
{
    if (!m_are_resources_initialized)
        return;

    if (m_NriDevice)
    {
        nriDestroyDevice(m_NriDevice);
        m_NriDevice = nullptr;
    }

    m_NriCore = {};
    m_NriWrapper = {};

    m_are_resources_initialized = false;

    LOG("[NRD Native] RenderSystem Shutdown completed.");
}

void RenderSystem::Release(nri::Texture* texture)
{
    if (texture)
    {
        m_NriCore.DestroyTexture(texture);
    }
}

RenderSystem::RenderSystem()
= default;

RenderSystem::~RenderSystem()
{
    // DestroyNrd();
}

// 处理设备事件
void RenderSystem::ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces)
{
    switch (type)
    {
    case kUnityGfxDeviceEventInitialize:
        s_d3d12 = interfaces->Get<IUnityGraphicsD3D12v7>();
        s_Log = interfaces->Get<IUnityLog>();

        LOG("[NRD Native] ProcessDeviceEvent kUnityGfxDeviceEventInitialize");

        UnityD3D12PluginEventConfig config_1;
        config_1.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
        config_1.flags = kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
        config_1.ensureActiveRenderTextureIsBound = true;
        
        s_d3d12->ConfigureEvent(0, &config_1);
        s_d3d12->ConfigureEvent(1, &config_1);
        s_d3d12->ConfigureEvent(2, &config_1);
        s_d3d12->ConfigureEvent(3, &config_1);
        s_d3d12->ConfigureEvent(4, &config_1);

        // initialize_and_create_resources();
        break;
    case kUnityGfxDeviceEventShutdown:
        LOG("[NRD Native] ProcessDeviceEvent kUnityGfxDeviceEventShutdown");
        // release_resources();
        break;
    case kUnityGfxDeviceEventBeforeReset:
    case kUnityGfxDeviceEventAfterReset:
        break;
    }
}

nri::Texture* RenderSystem::WrapD3D12Texture(ID3D12Resource* resource, DXGI_FORMAT format) const
{
    nri::TextureD3D12Desc desc;
    desc.d3d12Resource = resource;
    desc.format = format;

    nri::Texture* nriTexture = nullptr;
    m_NriWrapper.CreateTextureD3D12(*RenderSystem::Get().GetNriDevice(), desc, nriTexture);
    return nriTexture;
}
