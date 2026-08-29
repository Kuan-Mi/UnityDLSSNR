#include "NrdInstance.h"
#include "RenderSystem.h"
#include "D3D12HeapHook.h"

#undef  max
#undef  min
#include "NRDIntegration.hpp"

#define LOG(msg) UNITY_LOG(s_Log, msg)

NrdInstance::NrdInstance(IUnityInterfaces* interfaces, int instanceId, const NrdDenoiserDesc* denoisers, int denoiserCount)
{
    s_d3d12 = interfaces->Get<IUnityGraphicsD3D12v8>();
    s_Log = interfaces->Get<IUnityLog>();
    id = instanceId;

    if (denoisers == nullptr || denoiserCount <= 0)
    {
        LOG(("[NRD Native] id:" + std::to_string(id) + " - NrdInstance created with no denoisers.").c_str());
    }
    else
    {
        if (denoiserCount > static_cast<int>(kMaxDenoisersPerInstance))
        {
            LOG(("[NRD Native] id:" + std::to_string(id) + " - denoiserCount exceeds kMaxDenoisersPerInstance, clamping.").c_str());
            denoiserCount = static_cast<int>(kMaxDenoisersPerInstance);
        }

        m_Denoisers.assign(denoisers, denoisers + denoiserCount);
        m_DenoiserIds.reserve(denoiserCount);
        for (const auto& d : m_Denoisers)
            m_DenoiserIds.push_back(static_cast<nrd::Identifier>(d.identifier));
    }

    initialize_and_create_resources();
}

NrdInstance::~NrdInstance()
{
    release_resources();
}

static bool IsUAVAccess(nri::AccessBits access)
{
    // 定义所有映射到 D3D12_RESOURCE_STATE_UNORDERED_ACCESS 的 NRI 位
    constexpr nri::AccessBits uavBits =
        nri::AccessBits::SHADER_RESOURCE_STORAGE |
        nri::AccessBits::SCRATCH_BUFFER |
        nri::AccessBits::CLEAR_STORAGE |
        nri::AccessBits::ACCELERATION_STRUCTURE_READ |
        nri::AccessBits::ACCELERATION_STRUCTURE_WRITE |
        nri::AccessBits::MICROMAP_READ |
        nri::AccessBits::MICROMAP_WRITE;

    return (access & uavBits) != 0;
}

static D3D12_RESOURCE_STATES GetResourceStates(nri::AccessBits accessBits, D3D12_COMMAND_LIST_TYPE commandListType)
{
    D3D12_RESOURCE_STATES resourceStates = D3D12_RESOURCE_STATE_COMMON;

    if (accessBits & nri::AccessBits::INDEX_BUFFER)
        resourceStates |= D3D12_RESOURCE_STATE_INDEX_BUFFER;

    if (accessBits & (nri::AccessBits::CONSTANT_BUFFER | nri::AccessBits::VERTEX_BUFFER))
        resourceStates |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;

    if (accessBits & nri::AccessBits::ARGUMENT_BUFFER)
        resourceStates |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;

    if (accessBits & nri::AccessBits::COLOR_ATTACHMENT)
        resourceStates |= D3D12_RESOURCE_STATE_RENDER_TARGET;

    if (accessBits & nri::AccessBits::SHADING_RATE_ATTACHMENT)
        resourceStates |= D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;

    if (accessBits & nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_READ)
        resourceStates |= D3D12_RESOURCE_STATE_DEPTH_READ;

    if (accessBits & nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE)
        resourceStates |= D3D12_RESOURCE_STATE_DEPTH_WRITE;

    if (accessBits & (nri::AccessBits::ACCELERATION_STRUCTURE_READ | nri::AccessBits::MICROMAP_READ))
        resourceStates |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (accessBits & (nri::AccessBits::ACCELERATION_STRUCTURE_WRITE | nri::AccessBits::MICROMAP_WRITE))
        resourceStates |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (accessBits & nri::AccessBits::SHADER_RESOURCE)
    {
        resourceStates |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (commandListType == D3D12_COMMAND_LIST_TYPE_DIRECT)
            resourceStates |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    if (accessBits & nri::AccessBits::SHADER_BINDING_TABLE)
        resourceStates |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (accessBits & (nri::AccessBits::SHADER_RESOURCE_STORAGE | nri::AccessBits::SCRATCH_BUFFER | nri::AccessBits::CLEAR_STORAGE))
        resourceStates |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (accessBits & nri::AccessBits::COPY_SOURCE)
        resourceStates |= D3D12_RESOURCE_STATE_COPY_SOURCE;

    if (accessBits & nri::AccessBits::COPY_DESTINATION)
        resourceStates |= D3D12_RESOURCE_STATE_COPY_DEST;

    if (accessBits & nri::AccessBits::RESOLVE_SOURCE)
        resourceStates |= D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

    if (accessBits & nri::AccessBits::RESOLVE_DESTINATION)
        resourceStates |= D3D12_RESOURCE_STATE_RESOLVE_DEST;

    return resourceStates;
}


void NrdInstance::DispatchCompute(FrameData* data)
{
    if (data == nullptr)
        return;
    if (data->width == 0 || data->height == 0)
    {
        LOG(("[NRD Native] id:" + std::to_string(id) + " - Invalid texture size, skipping dispatch.").c_str());
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


    if (TextureWidth != data->width || TextureHeight != data->height)
    {
        if (TextureWidth == 0 || TextureHeight == 0)
        {
            LOG(("[NRD Native] id:" + std::to_string(id) + " - Creating NRD instance for the first time.").c_str());
        }
        else
        {
            LOG(("[NRD Native] id:" + std::to_string(id) + " - Texture size changed, recreating NRD instance.").c_str());
        }

        TextureWidth = data->width;
        TextureHeight = data->height;

        CreateNrd();
        frameIndex = 0;
    }

    // LOG(("[NRD Native] id:" + std::to_string(id) + " - Dispatching NRD compute for frame index " + std::to_string(data->commonSettings.frameIndex) + ".").c_str());


    data->commonSettings.frameIndex = frameIndex;
    frameIndex++;

    m_NrdIntegration.SetCommonSettings(data->commonSettings);

    // Fan out per-denoiser settings from FrameData entries[].
    const uint32_t entryCount = data->denoiserCount > kMaxDenoisersPerInstance
                                    ? kMaxDenoisersPerInstance
                                    : data->denoiserCount;
    for (uint32_t i = 0; i < entryCount; ++i)
    {
        const DenoiserSettingsEntry& e = data->entries[i];
        m_NrdIntegration.SetDenoiserSettings(static_cast<nrd::Identifier>(e.identifier), e.settings);
    }

    m_NrdIntegration.NewFrame();

    nrd::ResourceSnapshot snapshot = {};

    for (const auto& input : m_CachedResources)
    {
        if (input.texture == nullptr) continue;

        uint64_t nativeHandle = RenderSystem::Get().GetNriCore().GetTextureNativeObject(input.texture);
        ID3D12Resource* rawResource = reinterpret_cast<ID3D12Resource*>(nativeHandle);
        auto state = GetResourceStates(input.state.accessBits, D3D12_COMMAND_LIST_TYPE_DIRECT);
        s_d3d12->RequestResourceState(rawResource, state);

        nrd::Resource r = {};
        r.nri.texture = input.texture;
        r.state.access = input.state.accessBits;
        r.state.layout = static_cast<nri::Layout>(input.state.layout);
        r.state.stages = input.state.stageBits;

        snapshot.SetResource(input.type, r);
    }

    const nrd::Identifier* denoiserIds  = m_DenoiserIds.empty() ? nullptr : m_DenoiserIds.data();
    const uint32_t         denoiserNums = static_cast<uint32_t>(m_DenoiserIds.size());

    D3D12HeapHook::BeginPluginDispatch();
    m_NrdIntegration.Denoise(denoiserIds, denoiserNums, *nriCmdBuffer, snapshot);
    D3D12HeapHook::EndPluginDispatch();
    D3D12HeapHook::RestoreUnityHeaps(recording_state.commandList);

    for (size_t i = 0; i < snapshot.uniqueNum; i++)
    {
        nrd::Resource& res = snapshot.unique[i];
        uint64_t nativeHandle = RenderSystem::Get().GetNriCore().GetTextureNativeObject(res.nri.texture);
        ID3D12Resource* rawResource = reinterpret_cast<ID3D12Resource*>(nativeHandle);
        auto state = GetResourceStates(res.state.access, D3D12_COMMAND_LIST_TYPE_DIRECT);

        bool isUAV = IsUAVAccess(res.state.access);

        s_d3d12->NotifyResourceState(rawResource, state, isUAV);
    }

    RenderSystem::Get().GetNriCore().DestroyCommandBuffer(nriCmdBuffer);
}

void NrdInstance::UpdateResources(const NrdResourceInput* resources, int count)
{
    if (!resources || count <= 0)
    {
        m_CachedResources.clear();
        return;
    }

    m_CachedResources.resize(count);

    memcpy(m_CachedResources.data(), resources, count * sizeof(NrdResourceInput));
}

void NrdInstance::CreateNrd()
{
    m_NrdIntegration.Destroy();

    if (m_Denoisers.empty())
    {
        LOG(("[NRD Native] id:" + std::to_string(id) + " - CreateNrd() called with empty denoiser list, skipping.").c_str());
        return;
    }

    // 1. 配置 NRD Integration
    nrd::IntegrationCreationDesc integrationDesc = {};
    integrationDesc.resourceWidth = static_cast<uint16_t>(TextureWidth);
    integrationDesc.resourceHeight = static_cast<uint16_t>(TextureHeight);
    integrationDesc.queuedFrameNum = kMaxFramesInFlight;
    integrationDesc.demoteFloat32to16 = true; // 可选优化
    integrationDesc.autoWaitForIdle = false;
    integrationDesc.enableWholeLifetimeDescriptorCaching = true; // 推荐开启以提高性能

    // 2. 按 C# 传入的列表构造 DenoiserDesc
    std::vector<nrd::DenoiserDesc> denoiserDescs;
    denoiserDescs.reserve(m_Denoisers.size());
    std::string summary;
    for (const auto& d : m_Denoisers)
    {
        nrd::DenoiserDesc dd{};
        dd.identifier = static_cast<nrd::Identifier>(d.identifier);
        dd.denoiser   = static_cast<nrd::Denoiser>(d.denoiser);
        denoiserDescs.push_back(dd);

        summary += " (id=" + std::to_string(d.identifier) + ", type=" + std::to_string(d.denoiser) + ")";
    }

    nrd::InstanceCreationDesc instanceDesc = {};
    instanceDesc.denoisers = denoiserDescs.data();
    instanceDesc.denoisersNum = static_cast<uint32_t>(denoiserDescs.size());

    nrd::Result result = m_NrdIntegration.Recreate(integrationDesc, instanceDesc, RenderSystem::Get().GetNriDevice());

    if (result != nrd::Result::SUCCESS)
    {
        LOG(("[NRD Native] id:" + std::to_string(id) + " - NRD Integration Init Failed, result=" + std::to_string(static_cast<int>(result)) + ".").c_str());
        throw std::runtime_error("NRD Integration Init Failed");
    }

    LOG(("[NRD Native] id:" + std::to_string(id)
        + " - NRD Instance Created/Updated with " + std::to_string(m_Denoisers.size())
        + " denoiser(s):" + summary).c_str());
}

void NrdInstance::initialize_and_create_resources()
{
    if (m_are_resources_initialized)
        return;
    m_are_resources_initialized = true;
}

void NrdInstance::release_resources()
{
    if (!m_are_resources_initialized)
        return;

    m_NrdIntegration.Destroy();

    m_are_resources_initialized = false;

    LOG(("[NRD Native] id:" + std::to_string(id) + " - NRD Instance Released.").c_str());
}
