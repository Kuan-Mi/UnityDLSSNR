#include "DLSRInstance.h"

#include "RenderSystem.h"
#include "DLSRFrameData.h"


#define LOG(msg) UNITY_LOG(s_Log, msg)

static inline uint64_t CreateDLSRDescriptorKey(uint64_t texturePtr, bool isStorage)
{
    return (uint64_t(isStorage ? 1 : 0) << 63ull) | (texturePtr & 0x7FFFFFFFFFFFFFFFull);
}

DLSRInstance::DLSRInstance(IUnityInterfaces* interfaces, int instanceId)
{
    s_d3d12 = interfaces->Get<IUnityGraphicsD3D12v8>();
    s_Log   = interfaces->Get<IUnityLog>();
    id      = instanceId;

    initialize_and_create_resources();
}

DLSRInstance::~DLSRInstance()
{
    release_resources();
}

nri::Descriptor* DLSRInstance::GetOrCreateDescriptor(nri::Texture* texture, bool isStorage)
{
    if (!texture) return nullptr;

    auto& nriCore = RenderSystem::Get().GetNriCore();

    uint64_t nativeHandle = nriCore.GetTextureNativeObject(texture);
    uint64_t key = CreateDLSRDescriptorKey(nativeHandle, isStorage);

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

nri::UpscalerResource DLSRInstance::GetPair(nri::Texture* texture, bool isUAV)
{
    nri::Descriptor* desc = GetOrCreateDescriptor(texture, isUAV);
    return {texture, desc};
}

void DLSRInstance::DispatchCompute(DLSRFrameData* data)
{
    if (data == nullptr)
        return;

    if (data->outputWidth == 0 || data->outputHeight == 0)
    {
        LOG(("[DLSR] id:" + std::to_string(id) + " - Invalid texture size, skipping dispatch.").c_str());
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
                     || (TextureHeight != data->outputHeight)
                     || (upscalerMode  != data->upscalerMode)
                     || (preset        != data->preset);

    if (needRecreate)
    {
        if (TextureWidth == 0 || TextureHeight == 0)
            LOG(("[DLSR] id:" + std::to_string(id) + " - Creating DLSR instance for the first time.").c_str());
        else
            LOG(("[DLSR] id:" + std::to_string(id) + " - Parameters changed, recreating DLSR instance.").c_str());

        TextureWidth  = data->outputWidth;
        TextureHeight = data->outputHeight;
        upscalerMode  = data->upscalerMode;
        preset        = data->preset;

        if (m_DLSR)
        {
            RenderSystem::Get().GetNriUpScaler().DestroyUpscaler(m_DLSR);
            m_DLSR = nullptr;
        }

        RenderSystem& rs = RenderSystem::Get();

        nri::UpscalerBits upscalerFlags = nri::UpscalerBits::DEPTH_INFINITE;
        upscalerFlags |= nri::UpscalerBits::HDR;
        upscalerFlags |= nri::UpscalerBits::DEPTH_INVERTED;

        nri::UpscalerDesc upscalerDesc = {};
        upscalerDesc.upscaleResolution = {(nri::Dim_t)TextureWidth, (nri::Dim_t)TextureHeight};
        upscalerDesc.type              = nri::UpscalerType::DLSR;
        upscalerDesc.mode              = upscalerMode;
        upscalerDesc.flags             = upscalerFlags;
        upscalerDesc.preset            = preset;
        upscalerDesc.commandBuffer     = nriCmdBuffer;

        nri::Result r = rs.GetNriUpScaler().CreateUpscaler(*rs.GetNriDevice(), upscalerDesc, m_DLSR);
        if (r != nri::Result::SUCCESS)
        {
            LOG(("[DLSR] Failed to create DLSR Upscaler. Error code: " + std::to_string(static_cast<int>(r))).c_str());
        }
        else
        {
            LOG("[DLSR] DLSR Upscaler created successfully.");
        }

        nri::UpscalerProps upscalerProps = {};
        rs.GetNriUpScaler().GetUpscalerProps(*m_DLSR, upscalerProps);

        LOG(("[DLSR] id:" + std::to_string(id) + " - Render resolution: " +
             std::to_string(upscalerProps.renderResolution.w) + "x" + std::to_string(upscalerProps.renderResolution.h) +
             ", Upscale resolution: " + std::to_string(upscalerProps.upscaleResolution.w) + "x" + std::to_string(upscalerProps.upscaleResolution.h) +
             ", MipBias: " + std::to_string(upscalerProps.mipBias))
            .c_str());
    }

    nri::DispatchUpscaleDesc dispatchUpscaleDesc = {};
    dispatchUpscaleDesc.input  = GetPair(data->inputTex,  false);
    dispatchUpscaleDesc.output = GetPair(data->outputTex, true);

    dispatchUpscaleDesc.currentResolution = {(nri::Dim_t)data->currentWidth, (nri::Dim_t)data->currentHeight};
    dispatchUpscaleDesc.cameraJitter      = {-data->cameraJitter[0], -data->cameraJitter[1]};
    dispatchUpscaleDesc.mvScale           = {data->mvScale[0], data->mvScale[1]};

    dispatchUpscaleDesc.flags = data->resetHistory
                              ? nri::DispatchUpscaleBits::RESET_HISTORY
                              : nri::DispatchUpscaleBits::NONE;

    dispatchUpscaleDesc.guides.upscaler.mv    = GetPair(data->mvTex,    false);
    dispatchUpscaleDesc.guides.upscaler.depth = GetPair(data->depthTex, false);

    // Optional guides — only set if textures provided
    if (data->exposureTex)
        dispatchUpscaleDesc.guides.upscaler.exposure  = GetPair(data->exposureTex,  false);
    if (data->reactiveTex)
        dispatchUpscaleDesc.guides.upscaler.reactive  = GetPair(data->reactiveTex,  false);

    RenderSystem::Get().GetNriUpScaler().CmdDispatchUpscale(*nriCmdBuffer, *m_DLSR, dispatchUpscaleDesc);

    RenderSystem::Get().GetNriCore().DestroyCommandBuffer(nriCmdBuffer);
}

void DLSRInstance::initialize_and_create_resources()
{
    if (m_are_resources_initialized)
        return;
    m_are_resources_initialized = true;
}

void DLSRInstance::release_resources()
{
    if (!m_are_resources_initialized)
        return;

    if (m_DLSR)
    {
        RenderSystem::Get().GetNriUpScaler().DestroyUpscaler(m_DLSR);
        m_DLSR = nullptr;
    }

    for (auto& [key, desc] : m_DescriptorCache)
        RenderSystem::Get().GetNriCore().DestroyDescriptor(desc);
    m_DescriptorCache.clear();

    m_are_resources_initialized = false;

    LOG(("[DLSR] id:" + std::to_string(id) + " - DLSR Instance Released.").c_str());
}
