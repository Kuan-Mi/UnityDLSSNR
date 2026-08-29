// Event and timer queries, ported from NVRHI's d3d12-queries.cpp.
// Copyright (c) 2014-2021, NVIDIA CORPORATION. MIT license
// (see ThirdParty/NVRHI-LICENSE.txt).

#include "d3d12-backend.h"

#include <dxgi.h>

#include "IUnityGraphicsD3D12.h"
#include "UnityRhiLog.h"

namespace unityrhi
{
EventQuery* Device::createEventQuery()
{
    auto* query = new EventQuery();
    query->debugName = "EventQuery";
    RegisterResource(query);
    return query;
}

void Device::setEventQuery(EventQuery* query)
{
    if (!query || !m_unityD3D12)
        return;
    query->fence = m_unityD3D12->GetFrameFence();
    query->fenceValue = m_unityD3D12->GetNextFrameFenceValue();
    query->started = query->fence != nullptr;
}

bool Device::pollEventQuery(EventQuery* query) const
{
    return query && query->started && query->fence &&
        query->fence->GetCompletedValue() >= query->fenceValue;
}

void Device::waitEventQuery(EventQuery* query)
{
    if (!query || !query->started || !query->fence || pollEventQuery(query))
        return;
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle)
        return;
    if (SUCCEEDED(query->fence->SetEventOnCompletion(query->fenceValue, eventHandle)))
        WaitForSingleObject(eventHandle, INFINITE);
    CloseHandle(eventHandle);
}

void Device::resetEventQuery(EventQuery* query)
{
    if (!query)
        return;
    query->started = false;
    query->fenceValue = 0;
    query->fence.Reset();
}

TimerQuery* Device::createTimerQuery()
{
    auto* query = new TimerQuery();
    query->debugName = "TimerQuery";

    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = 2;
    HRESULT hr = m_Context.device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&query->queryHeap));
    if (FAILED(hr))
    {
        LogError("[UnityRHI] createTimerQuery: CreateQueryHeap failed (hr=0x%08X).",
            static_cast<unsigned>(hr));
        delete query;
        return nullptr;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(uint64_t) * 2;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = m_Context.device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
        &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&query->resolveBuffer));
    if (FAILED(hr))
    {
        LogError("[UnityRHI] createTimerQuery: resolve buffer creation failed (hr=0x%08X).",
            static_cast<unsigned>(hr));
        delete query;
        return nullptr;
    }

    RegisterResource(query);
    return query;
}

bool Device::pollTimerQuery(TimerQuery* query) const
{
    if (!query)
        return false;
    const uint32_t completionInstance =
        query->completionInstance.load(std::memory_order_acquire);
    return completionInstance != 0 && CompletedLifetimeInstance() >= completionInstance;
}

float Device::getTimerQueryTime(TimerQuery* query)
{
    if (!query)
        return 0.f;
    // Timer queries are deliberately asynchronous. The completion marker is
    // recorded after ResolveQueryData in the same replayed command list, so a
    // visible instance guarantees the persistently ordered GPU write is done.
    // Callers that need to wait must poll; strict CPU waits remain available
    // through RhiCore.WaitSyncPoint for tests/readback/teardown.
    if (!pollTimerQuery(query))
        return 0.f;
    if (query->resolved)
        return query->time;

    ID3D12CommandQueue* queue = m_unityD3D12 ? m_unityD3D12->GetCommandQueue() : nullptr;
    uint64_t frequency = 0;
    if (!queue || FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0)
        return 0.f;

    D3D12_RANGE readRange{0, sizeof(uint64_t) * 2};
    uint64_t* timestamps = nullptr;
    if (FAILED(query->resolveBuffer->Map(0, &readRange, reinterpret_cast<void**>(&timestamps))))
        return 0.f;
    query->time = float(double(timestamps[1] - timestamps[0]) / double(frequency));
    D3D12_RANGE noWrite{};
    query->resolveBuffer->Unmap(0, &noWrite);
    query->resolved = true;
    return query->time;
}

void Device::resetTimerQuery(TimerQuery* query)
{
    if (!query)
        return;
    query->started = false;
    query->resolved = false;
    query->time = 0.f;
    query->lastUseFenceValue = 0;
    query->completionInstance.store(0, std::memory_order_release);
}
} // namespace unityrhi
