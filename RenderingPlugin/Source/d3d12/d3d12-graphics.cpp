// Graphics (raster) pipeline objects, ported from NVRHI's d3d12-graphics.cpp
// (External/nvrhi/src/d3d12).
// Copyright (c) 2014-2021, NVIDIA CORPORATION. MIT license
// (see ThirdParty/NVRHI-LICENSE.txt).
//
// UnityRHI deviations: the render state is the flattened single-target
// RhiGraphicsPipelineDesc (blend/comparison values are D3D12 enums passed
// through), and framebuffers take plain attachment lists instead of nvrhi's
// FramebufferDesc subresource bindings (whole mip 0 of each attachment).

#include "d3d12-backend.h"

#include "common/dxgi-format.h"
#include "common/format-info.h"
#include "UnityRhiLog.h"

namespace unityrhi
{
static D3D12_PRIMITIVE_TOPOLOGY_TYPE convertPrimitiveTopologyType(PrimitiveType primType)
{
    switch (primType)
    {
    case PrimitiveType::PointList:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case PrimitiveType::LineList:
    case PrimitiveType::LineStrip:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case PrimitiveType::TriangleList:
    case PrimitiveType::TriangleStrip:
    case PrimitiveType::TriangleFan:
    case PrimitiveType::TriangleListWithAdjacency:
    case PrimitiveType::TriangleStripWithAdjacency:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    case PrimitiveType::PatchList:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    default:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    }
}

// Port of nvrhi Device::createGraphicsPipeline + createPipelineState
// (d3d12-graphics.cpp), against the flattened render state.
GraphicsPipeline* Device::createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc,
    Shader* vertexShader, Shader* pixelShader, InputLayout* inputLayout,
    BindingLayout* const* layouts, uint32_t layoutCount,
    Framebuffer* framebuffer, const char* debugName)
{
    if (!vertexShader || !framebuffer)
    {
        LogError("[UnityRHI] createGraphicsPipeline '%s': vertex shader and framebuffer are required.",
            debugName ? debugName : "");
        return nullptr;
    }

    std::shared_ptr<RootSignature> rootSignature = getRootSignature(layouts, layoutCount, inputLayout != nullptr);
    if (!rootSignature)
        return nullptr;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature->handle.Get();
    psoDesc.VS = {vertexShader->bytecode->data(), vertexShader->bytecode->size()};
    if (pixelShader)
        psoDesc.PS = {pixelShader->bytecode->data(), pixelShader->bytecode->size()};

    // Blend state (single target; nvrhi TranslateBlendState reduced).
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& blend = psoDesc.BlendState.RenderTarget[0];
    blend.BlendEnable = desc.blendEnable ? TRUE : FALSE;
    blend.SrcBlend = desc.srcBlend ? D3D12_BLEND(desc.srcBlend) : D3D12_BLEND_ONE;
    blend.DestBlend = desc.destBlend ? D3D12_BLEND(desc.destBlend) : D3D12_BLEND_ZERO;
    blend.BlendOp = desc.blendOp ? D3D12_BLEND_OP(desc.blendOp) : D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = desc.srcBlendAlpha ? D3D12_BLEND(desc.srcBlendAlpha) : D3D12_BLEND_ONE;
    blend.DestBlendAlpha = desc.destBlendAlpha ? D3D12_BLEND(desc.destBlendAlpha) : D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = desc.blendOpAlpha ? D3D12_BLEND_OP(desc.blendOpAlpha) : D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask =
        UINT8(desc.colorWriteMask ? desc.colorWriteMask : D3D12_COLOR_WRITE_ENABLE_ALL);

    // Depth-stencil state (nvrhi TranslateDepthStencilState; stencil not ported).
    const bool hasDepthAttachment = framebuffer->depthTexture != nullptr;
    psoDesc.DepthStencilState.DepthEnable = (desc.depthTestEnable && hasDepthAttachment) ? TRUE : FALSE;
    psoDesc.DepthStencilState.DepthWriteMask =
        desc.depthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc =
        desc.depthFunc ? D3D12_COMPARISON_FUNC(desc.depthFunc) : D3D12_COMPARISON_FUNC_LESS;
    if (desc.depthTestEnable && !hasDepthAttachment)
        LogError("[UnityRHI] createGraphicsPipeline '%s': depthTestEnable is set but no depth target is bound.",
            debugName ? debugName : "");

    // Rasterizer state (nvrhi TranslateRasterizerState).
    psoDesc.RasterizerState.FillMode =
        RasterFillMode(desc.fillMode) == RasterFillMode::Wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    switch (RasterCullMode(desc.cullMode))
    {
    case RasterCullMode::Front: psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT; break;
    case RasterCullMode::None: psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; break;
    case RasterCullMode::Back:
    default: psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK; break;
    }
    psoDesc.RasterizerState.FrontCounterClockwise = desc.frontCounterClockwise ? TRUE : FALSE;
    psoDesc.RasterizerState.DepthClipEnable = desc.depthClipEnable ? TRUE : FALSE;

    psoDesc.PrimitiveTopologyType = convertPrimitiveTopologyType(PrimitiveType(desc.primType));
    if (psoDesc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED)
    {
        LogError("[UnityRHI] createGraphicsPipeline '%s': unsupported primitive type %u.",
            debugName ? debugName : "", desc.primType);
        return nullptr;
    }

    // Formats come from the framebuffer (nvrhi FramebufferInfo).
    psoDesc.NumRenderTargets = uint32_t(framebuffer->colorTextures.size());
    for (uint32_t i = 0; i < psoDesc.NumRenderTargets; ++i)
        psoDesc.RTVFormats[i] = getDxgiFormatMapping(framebuffer->colorTextures[i]->desc.format).rtvFormat;
    if (framebuffer->depthTexture)
        psoDesc.DSVFormat = getDxgiFormatMapping(framebuffer->depthTexture->desc.format).rtvFormat;
    psoDesc.SampleDesc.Count = framebuffer->sampleCount;
    psoDesc.SampleDesc.Quality = framebuffer->sampleQuality;
    psoDesc.SampleMask = ~0u;

    if (inputLayout && !inputLayout->inputElements.empty())
    {
        psoDesc.InputLayout.NumElements = uint32_t(inputLayout->inputElements.size());
        psoDesc.InputLayout.pInputElementDescs = inputLayout->inputElements.data();
    }

    ComPtr<ID3D12PipelineState> pipelineState;
    const HRESULT hr = m_Context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
    if (FAILED(hr))
    {
        LogError("[UnityRHI] createGraphicsPipeline '%s': CreateGraphicsPipelineState failed (hr=0x%08X).",
            debugName ? debugName : "", static_cast<unsigned>(hr));
        return nullptr;
    }

    if (debugName && debugName[0])
    {
        wchar_t wide[256];
        int written = MultiByteToWideChar(CP_UTF8, 0, debugName, -1, wide, 255);
        wide[written > 0 ? written : 0] = L'\0';
        pipelineState->SetName(wide);
    }

    auto* pso = new GraphicsPipeline();
    pso->debugName = debugName ? debugName : "";
    pso->desc = desc;
    pso->inputLayout = inputLayout;
    pso->layouts.assign(layouts, layouts + layoutCount);
    pso->rootSignature = rootSignature;
    pso->pipelineState = pipelineState;
    RegisterResource(pso);
    return pso;
}

// Port of nvrhi Device::createFramebuffer (d3d12-graphics.cpp).
Framebuffer* Device::createFramebuffer(Texture* const* colorTextures, uint32_t colorCount,
    Texture* depthTexture, const char* debugName)
{
    if (colorCount == 0 && !depthTexture)
    {
        LogError("[UnityRHI] createFramebuffer '%s': at least one attachment is required.",
            debugName ? debugName : "");
        return nullptr;
    }

    auto* fb = new Framebuffer(m_Resources);
    fb->debugName = debugName ? debugName : "";

    Texture* sizingTexture = colorCount > 0 ? colorTextures[0] : depthTexture;
    fb->width = sizingTexture->desc.width;
    fb->height = sizingTexture->desc.height;
    fb->sampleCount = sizingTexture->desc.sampleCount;
    fb->sampleQuality = sizingTexture->desc.sampleQuality;

    TextureSubresourceSet wholeMip0{};

    for (uint32_t rt = 0; rt < colorCount; ++rt)
    {
        Texture* texture = colorTextures[rt];
        if (!texture || !texture->desc.isRenderTarget)
        {
            LogError("[UnityRHI] createFramebuffer '%s': color attachment %u is not a render target.",
                fb->debugName.c_str(), rt);
            delete fb;
            return nullptr;
        }

        DescriptorIndex index = m_Resources.renderTargetViewHeap.allocateDescriptor();
        texture->createRTV(m_Resources.renderTargetViewHeap.getCpuHandle(index).ptr, Format::UNKNOWN, wholeMip0);

        fb->RTVs.push_back(index);
        fb->colorTextures.push_back(texture);
    }

    if (depthTexture)
    {
        if (!depthTexture->desc.isRenderTarget)
        {
            LogError("[UnityRHI] createFramebuffer '%s': depth attachment is not a render target.",
                fb->debugName.c_str());
            delete fb;
            return nullptr;
        }

        DescriptorIndex index = m_Resources.depthStencilViewHeap.allocateDescriptor();
        depthTexture->createDSV(m_Resources.depthStencilViewHeap.getCpuHandle(index).ptr, wholeMip0, false);

        fb->DSV = index;
        fb->depthTexture = depthTexture;
    }

    RegisterResource(fb);
    return fb;
}

Framebuffer::~Framebuffer()
{
    for (DescriptorIndex rtv : RTVs)
        m_Resources.renderTargetViewHeap.releaseDescriptor(rtv);

    if (DSV != c_InvalidDescriptorIndex)
        m_Resources.depthStencilViewHeap.releaseDescriptor(DSV);
}

// Port of nvrhi CommandList::setGraphicsState.
// UnityRHI deviation: the full state is rebound on every call instead of
// diffing against the previous state - replayed streams are short.
bool ReplayContext::setGraphicsState(GraphicsPipeline* pipeline, Framebuffer* framebuffer, const RhiViewport& viewport,
    const float blendConstantColor[4], Buffer* indexBuffer, Format indexFormat, uint64_t indexBufferOffset,
    Buffer* indirectParams, Buffer* indirectCountBuffer,
    const VertexBufferBinding* vertexBuffers, uint32_t vertexBufferCount,
    const std::vector<RhiResource*>& bindings)
{
    Device* device = Device::Get();
    if (!pipeline || !pipeline->rootSignature || !pipeline->pipelineState || !framebuffer || !device)
        return false;

    commitDescriptorHeaps();

    // Port of nvrhi CommandList::bindGraphicsPipeline.
    commandList->SetGraphicsRootSignature(pipeline->rootSignature->handle.Get());
    commandList->SetPipelineState(pipeline->pipelineState.Get());
    commandList->IASetPrimitiveTopology(convertPrimitiveType(PrimitiveType(pipeline->desc.primType)));
    pipeline->lastUseFenceValue = fenceValue;

    // Port of nvrhi setResourceStatesForFramebuffer + bindFramebuffer.
    // UnityRHI deviation: framebuffers bind whole attachments (mip 0), so the
    // states are required for all subresources.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8] = {};
    const uint32_t rtvCount = uint32_t(framebuffer->RTVs.size() > 8 ? 8 : framebuffer->RTVs.size());
    for (uint32_t i = 0; i < rtvCount; ++i)
    {
        if (enableAutomaticBarriers)
            requireTextureState(framebuffer->colorTextures[i], AllSubresources, ResourceStates::RenderTarget);
        framebuffer->colorTextures[i]->lastUseFenceValue = fenceValue;
        rtvHandles[i] = device->resources().renderTargetViewHeap.getCpuHandle(framebuffer->RTVs[i]);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    if (framebuffer->depthTexture)
    {
        if (enableAutomaticBarriers)
            requireTextureState(framebuffer->depthTexture, AllSubresources, ResourceStates::DepthWrite);
        framebuffer->depthTexture->lastUseFenceValue = fenceValue;
        dsvHandle = device->resources().depthStencilViewHeap.getCpuHandle(framebuffer->DSV);
    }
    commandList->OMSetRenderTargets(rtvCount, rtvHandles, FALSE,
        framebuffer->depthTexture ? &dsvHandle : nullptr);
    framebuffer->lastUseFenceValue = fenceValue;

    if (!setComputeBindings(bindings, ~0u, nullptr, false, pipeline->rootSignature.get(), true))
        return false;

    // Index buffer.
    if (indexBuffer)
    {
        if (!indexBuffer->resource || indexBufferOffset > indexBuffer->desc.byteSize)
            return false;
        if (enableAutomaticBarriers)
            requireBufferState(indexBuffer, ResourceStates::IndexBuffer);
        indexBuffer->lastUseFenceValue = fenceValue;

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.Format = getDxgiFormatMapping(indexFormat).srvFormat;
        ibv.SizeInBytes = UINT(std::min(indexBuffer->desc.byteSize - indexBufferOffset, uint64_t(UINT_MAX)));
        ibv.BufferLocation = indexBuffer->gpuVA + indexBufferOffset;
        commandList->IASetIndexBuffer(&ibv);
    }
    else
    {
        // Full-rebind mode must clear a previously bound index buffer.
        commandList->IASetIndexBuffer(nullptr);
    }

    // Vertex buffers; strides come from the pipeline's input layout.
    constexpr uint32_t kMaxVertexBuffers = 16;
    D3D12_VERTEX_BUFFER_VIEW vbvs[kMaxVertexBuffers] = {};
    InputLayout* inputLayout = pipeline->inputLayout;
    for (uint32_t i = 0; i < vertexBufferCount; ++i)
    {
        const VertexBufferBinding& binding = vertexBuffers[i];
        if (binding.slot >= kMaxVertexBuffers || !binding.buffer || !binding.buffer->resource ||
            binding.offset > binding.buffer->desc.byteSize)
            continue;
        if (enableAutomaticBarriers)
            requireBufferState(binding.buffer, ResourceStates::VertexBuffer);
        binding.buffer->lastUseFenceValue = fenceValue;

        vbvs[binding.slot].StrideInBytes =
            inputLayout && inputLayout->elementStrides.count(binding.slot)
                ? inputLayout->elementStrides[binding.slot] : 0;
        vbvs[binding.slot].SizeInBytes = UINT(std::min(
            binding.buffer->desc.byteSize - binding.offset, uint64_t(UINT_MAX)));
        vbvs[binding.slot].BufferLocation = binding.buffer->gpuVA + binding.offset;
    }
    // Binding all slots also clears stale slots from the previous state.
    commandList->IASetVertexBuffers(0, kMaxVertexBuffers, vbvs);

    // Viewport + scissor (nvrhi convertViewportState, scissorEnable = false).
    D3D12_VIEWPORT d3dViewport{};
    d3dViewport.TopLeftX = viewport.minX;
    d3dViewport.TopLeftY = viewport.minY;
    d3dViewport.Width = viewport.maxX - viewport.minX;
    d3dViewport.Height = viewport.maxY - viewport.minY;
    d3dViewport.MinDepth = viewport.minZ;
    d3dViewport.MaxDepth = viewport.maxZ;
    commandList->RSSetViewports(1, &d3dViewport);

    D3D12_RECT scissor{};
    scissor.left = LONG(viewport.minX);
    scissor.top = LONG(viewport.minY);
    scissor.right = LONG(viewport.maxX);
    scissor.bottom = LONG(viewport.maxY);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->OMSetBlendFactor(blendConstantColor);

    currentGraphicsPipeline = pipeline;
    currentFramebuffer = framebuffer;
    currentGraphicsIndirectParams = indirectParams;
    currentGraphicsIndirectCountBuffer = indirectCountBuffer;
    if (indirectParams)
    {
        if (enableAutomaticBarriers)
            requireBufferState(indirectParams, ResourceStates::IndirectArgument);
        indirectParams->lastUseFenceValue = fenceValue;
    }
    if (indirectCountBuffer)
    {
        if (enableAutomaticBarriers)
            requireBufferState(indirectCountBuffer, ResourceStates::IndirectArgument);
        indirectCountBuffer->lastUseFenceValue = fenceValue;
    }
    graphicsStateActive = true;
    rayTracingStateActive = false;
    bindingStatesDirty = false;

    commitBarriers();
    return true;
}

// Port of nvrhi CommandList::updateGraphicsVolatileBuffers.
void ReplayContext::updateGraphicsVolatileBuffers()
{
    if (!anyVolatileBufferWrites)
        return;

    for (VolatileConstantBufferBinding& parameter : currentGraphicsVolatileCBs)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS currentGpuVA = volatileConstantBufferAddresses[parameter.buffer];

        if (currentGpuVA != parameter.address)
        {
            commandList->SetGraphicsRootConstantBufferView(parameter.bindingPoint, currentGpuVA);
            parameter.address = currentGpuVA;
        }
    }

    anyVolatileBufferWrites = false;
}

// Ports of nvrhi CommandList::draw / drawIndexed.
bool ReplayContext::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance)
{
    if (!graphicsStateActive)
    {
        LogError("[UnityRHI] Draw requires SetGraphicsState first.");
        return false;
    }

    updateGraphicsVolatileBuffers();
    commandList->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
    return true;
}

bool ReplayContext::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex,
    uint32_t baseVertex, uint32_t startInstance)
{
    if (!graphicsStateActive)
    {
        LogError("[UnityRHI] DrawIndexed requires SetGraphicsState first.");
        return false;
    }

    updateGraphicsVolatileBuffers();
    commandList->DrawIndexedInstanced(indexCount, instanceCount, startIndex, INT(baseVertex), startInstance);
    return true;
}

bool ReplayContext::drawIndirect(uint64_t offsetBytes, uint32_t drawCount, bool indexed,
    uint64_t countOffsetBytes, bool useCountBuffer)
{
    if (!graphicsStateActive || !currentGraphicsIndirectParams || !currentGraphicsIndirectParams->resource)
    {
        LogError("[UnityRHI] DrawIndirect requires SetGraphicsState with indirectParams.");
        return false;
    }
    if (useCountBuffer && (!currentGraphicsIndirectCountBuffer || !currentGraphicsIndirectCountBuffer->resource))
    {
        LogError("[UnityRHI] DrawIndexedIndirectCount requires indirectCountBuffer.");
        return false;
    }

    ID3D12CommandSignature* signature = Device::Get() ? Device::Get()->GetDrawIndirectSignature(indexed) : nullptr;
    if (!signature)
        return false;

    updateGraphicsVolatileBuffers();
    commandList->ExecuteIndirect(signature, drawCount,
        currentGraphicsIndirectParams->resource.Get(), offsetBytes,
        useCountBuffer ? currentGraphicsIndirectCountBuffer->resource.Get() : nullptr,
        useCountBuffer ? countOffsetBytes : 0);
    currentGraphicsIndirectParams->lastUseFenceValue = fenceValue;
    if (useCountBuffer)
        currentGraphicsIndirectCountBuffer->lastUseFenceValue = fenceValue;
    return true;
}
} // namespace unityrhi
