#include "NISInstance.h"

#include "RenderSystem.h"
#include "NISFrameData.h"


#define LOG(msg) UNITY_LOG(s_Log, msg)

static inline uint64_t CreateNISDescriptorKey(uint64_t texturePtr, bool isStorage)
{
    return (uint64_t(isStorage ? 1 : 0) << 63ull) | (texturePtr & 0x7FFFFFFFFFFFFFFFull);
}

NISInstance::NISInstance(IUnityInterfaces* interfaces, int instanceId)
{
    s_d3d12 = interfaces->Get<IUnityGraphicsD3D12v8>();
    s_Log   = interfaces->Get<IUnityLog>();
    id      = instanceId;

    initialize_and_create_resources();
}

NISInstance::~NISInstance()
{
    release_resources();
}

nri::Descriptor* NISInstance::GetOrCreateDescriptor(nri::Texture* texture, bool isStorage)
{
    if (!texture) return nullptr;

    auto& nriCore = RenderSystem::Get().GetNriCore();

    uint64_t nativeHandle = nriCore.GetTextureNativeObject(texture);
    uint64_t key          = CreateNISDescriptorKey(nativeHandle, isStorage);

    auto it = m_DescriptorCache.find(key);
    if (it != m_DescriptorCache.end())
        return it->second;

    const nri::TextureDesc& texDesc = nriCore.GetTextureDesc(*texture);

    nri::TextureViewDesc viewDesc = {};
    viewDesc.texture   = texture;
    viewDesc.type      = isStorage ? nri::TextureView::STORAGE_TEXTURE : nri::TextureView::TEXTURE;
    viewDesc.format    = texDesc.format;
    viewDesc.mipOffset = 0;
    viewDesc.mipNum    = 1;

    nri::Descriptor* descriptor = nullptr;
    nri::Result res = nriCore.CreateTextureView(viewDesc, descriptor);

    if (res == nri::Result::SUCCESS)
    {
        m_DescriptorCache[key] = descriptor;
        return descriptor;
    }

    return nullptr;
}

nri::UpscalerResource NISInstance::GetPair(nri::Texture* texture, bool isUAV)
{
    nri::Descriptor* desc = GetOrCreateDescriptor(texture, isUAV);
    return {texture, desc};
}

void NISInstance::DispatchCompute(NISFrameData* data)
{
    if (data == nullptr)
        return;

    if (data->outputWidth == 0 || data->outputHeight == 0)
    {
        LOG(("[NIS] id:" + std::to_string(id) + " - Invalid texture size, skipping dispatch.").c_str());
        return;
    }

    UnityGraphicsD3D12RecordingState recording_state;
    if (!s_d3d12->CommandRecordingState(&recording_state))
        return;

    nri::CommandBufferD3D12Desc cmdDesc;
    cmdDesc.d3d12CommandList      = recording_state.commandList;
    cmdDesc.d3d12CommandAllocator = nullptr;

    nri::CommandBuffer* nriCmdBuffer = nullptr;
    RenderSystem::Get().GetNriWrapper().CreateCommandBufferD3D12(*RenderSystem::Get().GetNriDevice(), cmdDesc, nriCmdBuffer);

    bool needRecreate = (TextureWidth  != data->outputWidth)
                     || (TextureHeight != data->outputHeight);

    if (needRecreate)
    {
        if (TextureWidth == 0 || TextureHeight == 0)
            LOG(("[NIS] id:" + std::to_string(id) + " - Creating NIS instance for the first time.").c_str());
        else
            LOG(("[NIS] id:" + std::to_string(id) + " - Resolution changed, recreating NIS instance.").c_str());

        TextureWidth  = data->outputWidth;
        TextureHeight = data->outputHeight;

        if (m_NIS)
        {
            RenderSystem::Get().GetNriUpScaler().DestroyUpscaler(m_NIS);
            m_NIS = nullptr;
        }

        RenderSystem& rs = RenderSystem::Get();

        nri::UpscalerDesc upscalerDesc = {};
        upscalerDesc.upscaleResolution = {(nri::Dim_t)TextureWidth, (nri::Dim_t)TextureHeight};
        upscalerDesc.type              = nri::UpscalerType::NIS;
        upscalerDesc.commandBuffer     = nriCmdBuffer;

        nri::Result r = rs.GetNriUpScaler().CreateUpscaler(*rs.GetNriDevice(), upscalerDesc, m_NIS);
        if (r != nri::Result::SUCCESS)
        {
            LOG(("[NIS] Failed to create NIS Upscaler. Error code: " + std::to_string(static_cast<int>(r))).c_str());
            RenderSystem::Get().GetNriCore().DestroyCommandBuffer(nriCmdBuffer);
            return;
        }
        else
        {
            LOG("[NIS] NIS Upscaler created successfully.");
        }
    }

    if (!m_NIS)
    {
        LOG(("[NIS] id:" + std::to_string(id) + " - m_NIS is null, skipping dispatch.").c_str());
        RenderSystem::Get().GetNriCore().DestroyCommandBuffer(nriCmdBuffer);
        return;
    }

    nri::DispatchUpscaleDesc dispatchUpscaleDesc = {};
    dispatchUpscaleDesc.input             = GetPair(data->inputTex,  false);
    dispatchUpscaleDesc.output            = GetPair(data->outputTex, true);
    dispatchUpscaleDesc.currentResolution = {(nri::Dim_t)data->currentWidth, (nri::Dim_t)data->currentHeight};
    dispatchUpscaleDesc.settings.nis.sharpness = data->sharpness;

    // Transition input to SRV, output to UAV before dispatch
    {
        auto& nriCore = RenderSystem::Get().GetNriCore();
        if (data->inputTex)
        {
            auto* inputRes = reinterpret_cast<ID3D12Resource*>(nriCore.GetTextureNativeObject(data->inputTex));
            if (inputRes)
                s_d3d12->RequestResourceState(inputRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (data->outputTex)
        {
            auto* outputRes = reinterpret_cast<ID3D12Resource*>(nriCore.GetTextureNativeObject(data->outputTex));
            if (outputRes)
                s_d3d12->RequestResourceState(outputRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    RenderSystem::Get().GetNriUpScaler().CmdDispatchUpscale(*nriCmdBuffer, *m_NIS, dispatchUpscaleDesc);

    RenderSystem::Get().GetNriCore().DestroyCommandBuffer(nriCmdBuffer);
}

void NISInstance::initialize_and_create_resources()
{
    if (m_are_resources_initialized)
        return;
    m_are_resources_initialized = true;
}

void NISInstance::release_resources()
{
    if (!m_are_resources_initialized)
        return;

    if (m_NIS)
    {
        RenderSystem::Get().GetNriUpScaler().DestroyUpscaler(m_NIS);
        m_NIS = nullptr;
    }

    for (auto& [key, desc] : m_DescriptorCache)
        RenderSystem::Get().GetNriCore().DestroyDescriptor(desc);
    m_DescriptorCache.clear();

    m_are_resources_initialized = false;

    LOG(("[NIS] id:" + std::to_string(id) + " - NIS Instance Released.").c_str());
}
