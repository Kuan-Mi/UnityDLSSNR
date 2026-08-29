#include "DLRRInstance.h"

#include "RenderSystem.h"
#include "RRFrameData.h"


#define LOG(msg) UNITY_LOG(s_Log, msg)

// 模仿源码中的 Key 创建逻辑
static inline uint64_t CreateDescriptorKey(uint64_t texturePtr, bool isStorage)
{
    return (uint64_t(isStorage ? 1 : 0) << 63ull) | (texturePtr & 0x7FFFFFFFFFFFFFFFull);
}

DLRRInstance::DLRRInstance(IUnityInterfaces* interfaces, int instanceId)
{
    s_d3d12 = interfaces->Get<IUnityGraphicsD3D12v8>();
    s_Log = interfaces->Get<IUnityLog>();
    id = instanceId;

    initialize_and_create_resources();
}

DLRRInstance::~DLRRInstance()
{
    release_resources();
}

nri::Descriptor* DLRRInstance::GetOrCreateDescriptor(nri::Texture* texture, bool isStorage)
{
    if (!texture) return nullptr;

    auto& nriCore = RenderSystem::Get().GetNriCore();

    uint64_t nativeHandle = nriCore.GetTextureNativeObject(texture);
    uint64_t key = CreateDescriptorKey(nativeHandle, isStorage);

    // 2. 查找缓存
    auto it = m_DescriptorCache.find(key);
    if (it != m_DescriptorCache.end())
    {
        return it->second;
    }

    // 3. 缓存未命中，创建新的视图
    const nri::TextureDesc& texDesc = nriCore.GetTextureDesc(*texture);

    nri::TextureViewDesc viewDesc = {};
    viewDesc.texture = texture;
    // 根据是否是存储纹理（UAV）选择类型
    viewDesc.type = isStorage
                            ? nri::TextureView::STORAGE_TEXTURE
                            : nri::TextureView::TEXTURE;
    viewDesc.format = texDesc.format;
    viewDesc.mipOffset = 0;
    viewDesc.mipNum = 1;

    nri::Descriptor* descriptor = nullptr;
    nri::Result res = nriCore.CreateTextureView(viewDesc, descriptor);

    if (res == nri::Result::SUCCESS)
    {
        m_DescriptorCache[key] = descriptor;
        return descriptor;
    }

    return nullptr;
}

nri::UpscalerResource DLRRInstance::GetPair(nri::Texture* texture, bool isUAV)
{
    nri::Descriptor* desc = GetOrCreateDescriptor(texture, isUAV);
    return {texture, desc};
}


void DLRRInstance::DispatchCompute(RRFrameData* data)
{
    if (data == nullptr)
        return;

    if (data->outputWidth == 0 || data->outputHeight == 0)
    {
        LOG(("[DLRR] id:" + std::to_string(id) + " - Invalid texture size, skipping dispatch.").c_str());
        return;
    }

    UnityGraphicsD3D12RecordingState recording_state;
    if (!s_d3d12->CommandRecordingState(&recording_state))
        return;


    nri::CommandBufferD3D12Desc cmdDesc;
    cmdDesc.d3d12CommandList = recording_state.commandList;
    cmdDesc.d3d12CommandAllocator = nullptr;

    nri::CommandBuffer* nriCmdBuffer = nullptr;
    RenderSystem::Get().GetNriWrapper().CreateCommandBufferD3D12(*RenderSystem::Get().GetNriDevice(), cmdDesc, nriCmdBuffer);


    if (TextureWidth != data->outputWidth || TextureHeight != data->outputHeight  || upscalerMode != data->upscalerMode || preset != data->preset)
    {
        if (TextureWidth == 0 || TextureHeight == 0)
        {
            LOG(("[DLRR] id:" + std::to_string(id) + " - Creating DLRR instance for the first time.").c_str());
        }
        else
        {
            LOG(("[DLRR] id:" + std::to_string(id) + " - Texture size changed, recreating DLRR instance.").c_str());
        }

        TextureWidth = data->outputWidth;
        TextureHeight = data->outputHeight;
        upscalerMode = data->upscalerMode;
        preset = data->preset;

        if (m_DLRR)
        {
            RenderSystem::Get().GetNriUpScaler().DestroyUpscaler(m_DLRR);
            m_DLRR = nullptr;
        }

        RenderSystem& rs = RenderSystem::Get();

        // nri::UpscalerMode mode = nri::UpscalerMode::NATIVE;
        nri::UpscalerMode mode = upscalerMode;
        nri::UpscalerBits upscalerFlags = nri::UpscalerBits::DEPTH_INFINITE;
        upscalerFlags |= nri::UpscalerBits::HDR;
        upscalerFlags |= nri::UpscalerBits::DEPTH_INVERTED;

        nri::UpscalerDesc upscalerDesc = {};
        upscalerDesc.upscaleResolution = {(nri::Dim_t)TextureWidth, (nri::Dim_t)TextureHeight};
        upscalerDesc.type = nri::UpscalerType::DLRR;
        upscalerDesc.mode = mode;
        upscalerDesc.flags = upscalerFlags;
        upscalerDesc.preset = preset;
        upscalerDesc.commandBuffer = nriCmdBuffer;

        nri::Result r = rs.GetNriUpScaler().CreateUpscaler(*rs.GetNriDevice(), upscalerDesc, m_DLRR);
        if (r != nri::Result::SUCCESS)
        {
            LOG(("[DLRR] Failed to create DLRR Upscaler . Error code: " + std::to_string(static_cast<int>(r))).c_str());
        }
        else
        {
            LOG("[DLRR] DLRR Upscaler created successfully.");
        }
        
        
        nri::UpscalerProps upscalerProps = {};
        rs.GetNriUpScaler().GetUpscalerProps(*m_DLRR, upscalerProps);
        
        LOG(("[DLRR] id:" + std::to_string(id) + " - DLRR Upscaler created with render resolution: " +
             std::to_string(upscalerProps.renderResolution.w) + "x" + std::to_string(upscalerProps.renderResolution.h) +
             ", upscale resolution: " + std::to_string(upscalerProps.upscaleResolution.w) + "x" + std::to_string(upscalerProps.upscaleResolution.h))
            .c_str());
        
        
    }

    nri::DispatchUpscaleDesc dispatchUpscaleDesc = {};
    dispatchUpscaleDesc.input = GetPair(data->inputTex, false);
    dispatchUpscaleDesc.output = GetPair(data->outputTex, true);


    dispatchUpscaleDesc.currentResolution = {(nri::Dim_t)(data->currentWidth), (nri::Dim_t)(data->currentHeight)};

    dispatchUpscaleDesc.cameraJitter = {-data->cameraJitter[0], -data->cameraJitter[1]};
    dispatchUpscaleDesc.mvScale = {1.0f, 1.0f};
    dispatchUpscaleDesc.flags = nri::DispatchUpscaleBits::NONE;
    if (data->useSpecularMotionVector) {
        dispatchUpscaleDesc.flags |= nri::DispatchUpscaleBits::USE_SPECULAR_MOTION;
    }

    dispatchUpscaleDesc.guides.denoiser.mv = GetPair(data->mvTex, false);
    dispatchUpscaleDesc.guides.denoiser.depth = GetPair(data->depthTex, false);
    dispatchUpscaleDesc.guides.denoiser.diffuseAlbedo = GetPair(data->diffuseAlbedoTex, false);
    dispatchUpscaleDesc.guides.denoiser.specularAlbedo = GetPair(data->specularAlbedoTex, false);
    dispatchUpscaleDesc.guides.denoiser.normalRoughness = GetPair(data->normalRoughnessTex, false);
    dispatchUpscaleDesc.guides.denoiser.specularMvOrHitT = GetPair(data->specularMvOrHitTex, false);

    memcpy(&dispatchUpscaleDesc.settings.dlrr.worldToViewMatrix, &data->worldToViewMatrix, sizeof(data->worldToViewMatrix));
    memcpy(&dispatchUpscaleDesc.settings.dlrr.viewToClipMatrix, &data->viewToClipMatrix, sizeof(data->viewToClipMatrix));

    RenderSystem::Get().GetNriUpScaler().CmdDispatchUpscale(*nriCmdBuffer, *m_DLRR, dispatchUpscaleDesc);

    RenderSystem::Get().GetNriCore().DestroyCommandBuffer(nriCmdBuffer);
}

void DLRRInstance::initialize_and_create_resources()
{
    if (m_are_resources_initialized)
        return;
    m_are_resources_initialized = true;
}

void DLRRInstance::release_resources()
{
    if (!m_are_resources_initialized)
        return;

    if (m_DLRR)
    {
        RenderSystem::Get().GetNriUpScaler().DestroyUpscaler(m_DLRR);
        m_DLRR = nullptr;
    }

    m_are_resources_initialized = false;

    LOG(("[DLRR] id:" + std::to_string(id) + " - DLRR Instance Released.").c_str());
}
