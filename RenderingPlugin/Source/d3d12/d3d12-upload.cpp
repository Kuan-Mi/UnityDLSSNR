// Upload-buffer suballocator ported from NVRHI's d3d12-upload.cpp
// (External/nvrhi/src/d3d12). Copyright (c) 2014-2021, NVIDIA CORPORATION.
// MIT license (see ThirdParty/NVRHI-LICENSE.txt).
//
// UnityRHI deviations: chunk versions are plugin-owned recording instances
// (Device::BeginRecordingInstance, one per replayed command stream) instead
// of NVRHI's packed (queue, instance, submitted) versions. The single
// graphics queue makes the queue field unnecessary, and the submitted flag is
// implicit: Device::CompletedLifetimeInstance() only advances when a
// MARKER_OUT write recorded at the replay tail reaches the readback ring, so
// an instance that was never submitted can never compare as retired.
// (An earlier revision tagged chunks with Unity frame-fence values on the
// premise that recording and submission coincide; they do not - see
// docs/rt-sbt-upload-corruption-report.md.)

#include "d3d12-backend.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>

#include "UnityRhiLog.h"

namespace unityrhi
{
BufferChunk::~BufferChunk()
{
    if (buffer && cpuVA)
    {
        buffer->Unmap(0, nullptr);
        cpuVA = nullptr;
    }
}

std::shared_ptr<BufferChunk> UploadManager::createChunk(uint64_t size)
{
    auto chunk = std::make_shared<BufferChunk>();

    size = align(size, BufferChunk::c_sizeAlignment);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = m_IsScratchBuffer ? D3D12_HEAP_TYPE_DEFAULT : D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = size;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (m_IsScratchBuffer) bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = m_Context.device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        m_IsScratchBuffer ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&chunk->buffer));

    if (FAILED(hr))
    {
        m_Parent->HandleDeviceError(hr,
            m_IsScratchBuffer ? "Create scratch upload chunk" : "Create upload chunk");
        return nullptr;
    }

    if (!m_IsScratchBuffer)
    {
        hr = chunk->buffer->Map(0, nullptr, &chunk->cpuVA);

        if (FAILED(hr))
        {
            m_Parent->HandleDeviceError(hr, "Map upload chunk");
            return nullptr;
        }
    }

    chunk->bufferSize = size;
    chunk->gpuVA = chunk->buffer->GetGPUVirtualAddress();
    chunk->identifier = uint32_t(m_ChunkPool.size());

    wchar_t name[64];
    if (m_IsScratchBuffer)
        swprintf_s(name, L"UnityRHI DXR Scratch Buffer %u", chunk->identifier);
    else
        swprintf_s(name, L"UnityRHI Upload Buffer %u", chunk->identifier);
    chunk->buffer->SetName(name);

    m_AllocatedMemory += size;

    return chunk;
}

bool UploadManager::suballocateBuffer(uint64_t size, ID3D12GraphicsCommandList* pCommandList,
    ID3D12Resource** pBuffer, uint64_t* pOffset,
    void** pCpuVA, D3D12_GPU_VIRTUAL_ADDRESS* pGpuVA, uint32_t currentVersion, uint32_t alignment,
    std::shared_ptr<BufferChunk>* pTicketChunk)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    // Scratch allocations need a command list, upload ones don't
    if (m_IsScratchBuffer && !pCommandList)
        return false;

    std::shared_ptr<BufferChunk> chunkToRetire;

    // Try to allocate from the current chunk first
    if (m_CurrentChunk != nullptr)
    {
        uint64_t alignedOffset = align(m_CurrentChunk->writePointer, uint64_t(alignment));
        uint64_t endOfDataInChunk = alignedOffset + size;

        if (endOfDataInChunk <= m_CurrentChunk->bufferSize)
        {
            // The buffer can fit into the current chunk - great, we're done
            m_CurrentChunk->writePointer = endOfDataInChunk;
            // Recording instances grow monotonically; keep the newest so the
            // chunk is not reclaimed before its latest use retires.
            m_CurrentChunk->version = std::max(m_CurrentChunk->version, currentVersion);

            if (pBuffer) *pBuffer = m_CurrentChunk->buffer.Get();
            if (pOffset) *pOffset = alignedOffset;
            if (pCpuVA && m_CurrentChunk->cpuVA)
                *pCpuVA = static_cast<char*>(m_CurrentChunk->cpuVA) + alignedOffset;
            if (pGpuVA && m_CurrentChunk->gpuVA)
                *pGpuVA = m_CurrentChunk->gpuVA + alignedOffset;
            if (pTicketChunk)
            {
                ++m_CurrentChunk->pendingUploadTickets;
                *pTicketChunk = m_CurrentChunk;
            }

            return true;
        }

        chunkToRetire = m_CurrentChunk;
        m_CurrentChunk.reset();
    }

    const uint32_t completedInstance = m_Parent->CompletedLifetimeInstance();

    // Try to find a chunk in the pool that's no longer used and is large enough to allocate our buffer
    for (auto it = m_ChunkPool.begin(); it != m_ChunkPool.end(); ++it)
    {
        std::shared_ptr<BufferChunk> chunk = *it;

        if (chunk->version != 0 && chunk->version <= completedInstance)
        {
            chunk->version = 0;
        }

        if (chunk->pendingUploadTickets == 0 && chunk->version == 0 && chunk->bufferSize >= size)
        {
            m_ChunkPool.erase(it);
            m_CurrentChunk = chunk;
            break;
        }
    }

    if (chunkToRetire)
    {
        m_ChunkPool.push_back(chunkToRetire);
    }

    if (!m_CurrentChunk)
    {
        uint64_t sizeToAllocate = align(std::max(size, m_DefaultChunkSize), BufferChunk::c_sizeAlignment);

        // See if we're allowed to allocate more memory
        if ((m_MemoryLimit > 0) && (m_AllocatedMemory + sizeToAllocate > m_MemoryLimit))
        {
            if (m_IsScratchBuffer)
            {
                // Nope, need to reuse something.
                // Find the largest least recently used chunk that can fit our buffer.
                // UnityRHI deviation: NVRHI compares packed (submitted, instance)
                // versions; the plain recording instances here order the same way.

                std::shared_ptr<BufferChunk> bestChunk;
                for (const auto& candidateChunk : m_ChunkPool)
                {
                    if (candidateChunk->pendingUploadTickets == 0 && candidateChunk->bufferSize >= sizeToAllocate)
                    {
                        // Pick the first fitting chunk if we have nothing so far
                        if (!bestChunk)
                        {
                            bestChunk = candidateChunk;
                            continue;
                        }

                        // Compare chunks: old is better than new, large is better than small
                        if (candidateChunk->version < bestChunk->version ||
                            (candidateChunk->version == bestChunk->version
                                && candidateChunk->bufferSize > bestChunk->bufferSize))
                        {
                            bestChunk = candidateChunk;
                        }
                    }
                }

                if (!bestChunk)
                {
                    // No chunk found that can be reused. And we can't allocate. :(
                    return false;
                }

                // Move the found chunk from the pool to the current chunk
                m_ChunkPool.erase(std::find(m_ChunkPool.begin(), m_ChunkPool.end(), bestChunk));
                m_CurrentChunk = bestChunk;

                // Place a UAV barrier on the chunk.
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.UAV.pResource = bestChunk->buffer.Get();
                pCommandList->ResourceBarrier(1, &barrier);
            }
            else // !m_IsScratchBuffer
            {
                // Can't reuse in-flight buffers for uploads.
                // But uploads have no memory limit, so this should never execute.
                return false;
            }
        }
        else
        {
            m_CurrentChunk = createChunk(sizeToAllocate);
            if (!m_CurrentChunk)
            {
                LogError("[UnityRHI] Couldn't allocate an upload buffer chunk (%llu bytes).",
                    static_cast<unsigned long long>(size));
                return false;
            }
        }
    }

    m_CurrentChunk->version = currentVersion;
    // Reusing a chunk restarts it at offset 0, overwriting whatever it held.
    m_CurrentChunk->writePointer = size;

    if (pBuffer) *pBuffer = m_CurrentChunk->buffer.Get();
    if (pOffset) *pOffset = 0;
    if (pCpuVA) *pCpuVA = m_CurrentChunk->cpuVA;
    if (pGpuVA) *pGpuVA = m_CurrentChunk->gpuVA;
    if (pTicketChunk)
    {
        ++m_CurrentChunk->pendingUploadTickets;
        *pTicketChunk = m_CurrentChunk;
    }

    return true;
}

UploadTicket* UploadManager::stageBuffer(const void* data, uint64_t size, uint32_t alignment)
{
    if (!data || size == 0 || m_IsScratchBuffer)
        return nullptr;

    void* cpuVA = nullptr;
    UploadTicket* ticket = allocateTicket(size, alignment, &cpuVA);
    if (ticket)
        std::memcpy(cpuVA, data, size_t(size));
    return ticket;
}

UploadTicket* UploadManager::stageTexture(Texture* texture, uint32_t arraySlice, uint32_t mipLevel,
    const void* data, uint64_t dataSize, uint64_t rowPitch, uint64_t depthPitch)
{
    if (!texture || !texture->resource || !data ||
        mipLevel >= texture->desc.mipLevels || arraySlice >= texture->desc.arraySize)
        return nullptr;

    const uint32_t subresource = calcSubresource(
        mipLevel, arraySlice, 0, texture->desc.mipLevels, texture->desc.arraySize);
    const D3D12_RESOURCE_DESC desc = texture->resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    UINT64 totalBytes = 0;
    m_Context.device->GetCopyableFootprints(
        &desc, subresource, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);
    if (rowPitch == 0)
        rowPitch = rowSize;
    // Preserve NVRHI's writeTexture semantics exactly: depthPitch is used as
    // supplied. It is irrelevant for 2D textures and required for distinct 3D slices.
    const uint64_t sourceDepthPitch = depthPitch;
    const uint64_t requiredBytes = numRows == 0 ? 0 :
        sourceDepthPitch * (footprint.Footprint.Depth - 1) +
        rowPitch * (uint64_t(numRows) - 1) + std::min(rowPitch, rowSize);
    if (requiredBytes > dataSize)
        return nullptr;

    void* cpuVA = nullptr;
    UploadTicket* ticket = allocateTicket(
        totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &cpuVA);
    if (!ticket)
        return nullptr;
    ticket->footprint = footprint;
    ticket->footprint.Offset = ticket->offset;
    for (uint32_t z = 0; z < footprint.Footprint.Depth; ++z)
    {
        for (uint32_t row = 0; row < numRows; ++row)
        {
            void* dst = static_cast<uint8_t*>(cpuVA) +
                uint64_t(footprint.Footprint.RowPitch) * (row + uint64_t(z) * numRows);
            const void* src = static_cast<const uint8_t*>(data) +
                rowPitch * row + sourceDepthPitch * z;
            std::memcpy(dst, src, size_t(std::min(rowPitch, rowSize)));
        }
    }
    return ticket;
}

UploadTicket* UploadManager::allocateTicket(uint64_t size, uint32_t alignment, void** outCpuVA)
{
    if (!outCpuVA || size == 0 || m_IsScratchBuffer)
        return nullptr;

    void* cpuVA = nullptr;
    uint64_t offset = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
    std::shared_ptr<BufferChunk> chunk;
    if (!suballocateBuffer(size, nullptr, nullptr, &offset, &cpuVA, &gpuVA, 0, alignment, &chunk))
        return nullptr;

    auto* ticket = new (std::nothrow) UploadTicket{};
    if (!ticket)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        --chunk->pendingUploadTickets;
        return nullptr;
    }
    ticket->chunk = std::move(chunk);
    ticket->offset = offset;
    ticket->size = size;
    ticket->gpuVA = gpuVA;
    *outCpuVA = cpuVA;
    return ticket;
}

bool UploadManager::resolveTicket(UploadTicket* ticket, uint32_t fenceValue, ID3D12Resource** buffer,
    uint64_t* offset, D3D12_GPU_VIRTUAL_ADDRESS* gpuVA)
{
    if (!ticket || !ticket->chunk)
        return false;
    std::lock_guard<std::mutex> lock(m_Mutex);
    ticket->chunk->version = std::max(ticket->chunk->version, fenceValue);
    if (buffer) *buffer = ticket->chunk->buffer.Get();
    if (offset) *offset = ticket->offset;
    if (gpuVA) *gpuVA = ticket->gpuVA;
    return true;
}

void UploadManager::releaseTicket(UploadTicket* ticket)
{
    if (!ticket)
        return;
    if (ticket->referenceCount.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (ticket->chunk && ticket->chunk->pendingUploadTickets != 0)
            --ticket->chunk->pendingUploadTickets;
    }
    delete ticket;
}

void UploadManager::retainTicket(UploadTicket* ticket)
{
    if (ticket)
        ticket->referenceCount.fetch_add(1, std::memory_order_relaxed);
}
} // namespace unityrhi
