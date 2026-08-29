// Compute pipeline creation ported from NVRHI's d3d12-compute.cpp
// (External/nvrhi/src/d3d12). Copyright (c) 2014-2021, NVIDIA CORPORATION.
// MIT license (see ThirdParty/NVRHI-LICENSE.txt).
//
// ReplayContext methods execute at replay time on Unity's command list;
// CommandStream.cpp is responsible for decoding their flattened payloads.

#include "d3d12-backend.h"

#include "UnityRhiLog.h"

namespace unityrhi
{
ComPtr<ID3D12PipelineState> Device::createPipelineState(Shader* shader, RootSignature* rootSignature)
{
    ComPtr<ID3D12PipelineState> pipelineState;

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};

    desc.pRootSignature = rootSignature->handle.Get();
    shader->getBytecode(&desc.CS.pShaderBytecode, &desc.CS.BytecodeLength);

    const HRESULT hr = m_Context.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelineState));

    if (FAILED(hr))
    {
        LogError("[UnityRHI] Failed to create a compute pipeline state object (hr=0x%08X).",
            static_cast<unsigned>(hr));
        return nullptr;
    }

    return pipelineState;
}

ComputePipeline* Device::createComputePipeline(
    Shader* shader, BindingLayout* const* layouts, uint32_t layoutCount, const char* debugName)
{
    if (!shader || (layoutCount > 0 && !layouts))
    {
        LogError("[UnityRHI] createComputePipeline '%s': shader is required and the layout array must be valid.",
            debugName ? debugName : "");
        return nullptr;
    }

    std::shared_ptr<RootSignature> pRS = getRootSignature(layouts, layoutCount);
    if (!pRS)
        return nullptr;

    ComPtr<ID3D12PipelineState> pPSO = createPipelineState(shader, pRS.get());

    if (pPSO == nullptr)
        return nullptr;

    if (debugName && debugName[0])
    {
        wchar_t wide[256];
        int written = MultiByteToWideChar(CP_UTF8, 0, debugName, -1, wide, 255);
        wide[written > 0 ? written : 0] = L'\0';
        pPSO->SetName(wide);
    }

    auto* pso = new ComputePipeline();
    pso->debugName = debugName ? debugName : "";
    pso->shader = shader;
    pso->layouts.assign(layouts, layouts + layoutCount);
    pso->rootSignature = pRS;
    pso->pipelineState = pPSO;
    RegisterResource(pso);
    return pso;
}
// Port of nvrhi CommandList::setComputeState. The binding-set handles were
// decoded from the command stream by the replayer (CommandStream.cpp).
bool ReplayContext::setComputeState(ComputePipeline* pipeline, Buffer* indirectParams,
    const std::vector<RhiResource*>& bindings)
{
    if (!pipeline || !pipeline->rootSignature || !pipeline->pipelineState)
        return false;

    // Mirrors m_CurrentComputeStateValid. Graphics and ray-tracing both use
    // the same D3D12 pipeline/root-binding slots, so returning to compute must
    // rebind even when the ComputePipeline pointer itself did not change.
    const bool computeStateValid = currentPipeline && !graphicsStateActive && !rayTracingStateActive;
    const bool updateRootSignature = !computeStateValid ||
        currentPipeline->rootSignature != pipeline->rootSignature;
    const bool updatePipeline = !computeStateValid || currentPipeline != pipeline;

    uint32_t bindingUpdateMask = 0;
    if (!computeStateValid || updateRootSignature)
        bindingUpdateMask = ~0u;

    if (commitDescriptorHeaps())
        bindingUpdateMask = ~0u;

    if (bindingUpdateMask == 0)
        bindingUpdateMask = arrayDifferenceMask(currentBindings, bindings);

    if (updateRootSignature)
        commandList->SetComputeRootSignature(pipeline->rootSignature->handle.Get());

    if (updatePipeline)
    {
        commandList->SetPipelineState(pipeline->pipelineState.Get());
        pipeline->lastUseFenceValue = fenceValue;
    }

    if (!setComputeBindings(bindings, bindingUpdateMask, indirectParams,
            indirectParams != currentIndirectParams, pipeline->rootSignature.get()))
        return false;

    currentPipeline = pipeline;
    currentIndirectParams = indirectParams;
    // Version 14 may pass currentBindings itself when the wire stream reuses
    // the previous binding handles. Avoid a self-range assign and its copy.
    if (&bindings != &currentBindings)
        currentBindings.assign(bindings.begin(), bindings.end());
    graphicsStateActive = false;
    rayTracingStateActive = false;
    bindingStatesDirty = false;

    commitBarriers();
    return true;
}

// Port of nvrhi CommandList::updateComputeVolatileBuffers.
void ReplayContext::updateComputeVolatileBuffers()
{
    // If there are some volatile buffers bound, and they have been written
    // into since the last dispatch or setComputeState, patch their views.
    if (!anyVolatileBufferWrites)
        return;

    for (VolatileConstantBufferBinding& parameter : currentComputeVolatileCBs)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS currentGpuVA = volatileConstantBufferAddresses[parameter.buffer];

        if (currentGpuVA != parameter.address)
        {
            commandList->SetComputeRootConstantBufferView(parameter.bindingPoint, currentGpuVA);
            parameter.address = currentGpuVA;
        }
    }

    anyVolatileBufferWrites = false;
}

// Port of nvrhi CommandList::dispatch.
void ReplayContext::dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
{
    updateComputeVolatileBuffers();

    commandList->Dispatch(groupsX, groupsY, groupsZ);
}

// Port of nvrhi CommandList::dispatchIndirect: the argument buffer comes from
// the current ComputeState.indirectParams.
bool ReplayContext::dispatchIndirect(uint64_t offsetBytes)
{
    Buffer* indirectParams = currentIndirectParams;
    if (!indirectParams || !indirectParams->resource)
    {
        LogError("[UnityRHI] DispatchIndirect requires SetComputeState with indirectParams.");
        return false;
    }
    ID3D12CommandSignature* signature = Device::Get() ? Device::Get()->GetDispatchIndirectSignature() : nullptr;
    if (!signature)
        return false;

    updateComputeVolatileBuffers();

    commandList->ExecuteIndirect(signature, 1, indirectParams->resource.Get(), offsetBytes, nullptr, 0);
    indirectParams->lastUseFenceValue = fenceValue;
    return true;
}
} // namespace unityrhi
