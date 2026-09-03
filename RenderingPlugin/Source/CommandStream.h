#pragma once

#include <cstdint>

struct ID3D12GraphicsCommandList;

namespace unityrhi
{
struct ReplayContext;
constexpr uint32_t kCommandStreamMagic = 0x31494852; // RHI1
constexpr uint32_t kCommandStreamVersion = 14;

enum class CommandOpcode : uint32_t
{
    CopyBuffer = 1,
    SetComputeState = 2,
    Dispatch = 3,
    BeginMarker = 4,
    EndMarker = 5,
    SetEnableAutomaticBarriers = 6,
    SetBufferState = 7,
    DispatchIndirect = 8,
    UavBarrier = 9,
    ClearBufferUInt = 10,
    WriteBuffer = 11,
    SetPushConstants = 12,
    SetTextureState = 13,

    // Ray tracing (Phase 7).
    BuildBottomLevelAccelStruct = 14,
    BuildTopLevelAccelStruct = 15,
    BuildTopLevelAccelStructFromBuffer = 16,
    SetRayTracingState = 17,
    DispatchRays = 18,

    // Raster path (Phase 8).
    SetGraphicsState = 19,
    Draw = 20,
    DrawIndexed = 21,
    ClearTextureFloat = 22,
    ClearDepthStencilTexture = 23,

    // 24..25 remain reserved for the raster path (DrawIndirect, ResolveTexture).

    WriteTexture = 26,
    CopyTextureToBuffer = 27,
    BeginTrackingTextureState = 28,
    BeginTrackingBufferState = 29,
    ClearState = 30,
    CommitBarriers = 31,
    SetPermanentTextureState = 32,
    SetPermanentBufferState = 33,
    SetEnableUavBarriersForTexture = 34,
    SetEnableUavBarriersForBuffer = 35,
    SetTextureSubresourceState = 36,
    CopyTexture = 37,
    ResolveTexture = 38,
    DrawIndirect = 39,
    DrawIndexedIndirect = 40,
    DrawIndexedIndirectCount = 41,
    ClearTextureFloatSubresources = 42,
    ClearDepthStencilTextureSubresources = 43,
    ClearTextureUInt = 44,
    CopyTextureFromStaging = 45,
    CopyTextureToStaging = 46,
    BeginTimerQuery = 47,
    EndTimerQuery = 48,
    DispatchDlrr = 49,
    DispatchDlssNr = 50,
    DispatchDlss = 51,
};

struct CommandStreamHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t byteSize;
    uint32_t commandCount;
};

bool ReplayCommandStream(const void* data, ID3D12GraphicsCommandList* commandList, uint32_t fenceValue);
bool ReplayCommandStream(const void* data, ID3D12GraphicsCommandList* commandList, uint32_t fenceValue,
    ReplayContext& context);

// Total D3D12 UAV barriers recorded by the replayer since process start
// (same-state UnorderedAccess requests, explicit UavBarrier commands, and the
// barriers after acceleration-structure builds). Diagnostics and tests only.
uint64_t ReplayUavBarrierCount();

// Phase 6 test hook: decode statistics produced by DecodeCommandStream.
// Mirrored in C# as CommandStreamInfo (CommandList.Validate).
struct CommandStreamDecodeInfo
{
    static constexpr uint32_t kMaxOpcode = 64;

    uint32_t ok;           // 1 when the whole stream decodes and ends exactly at byteSize
    uint32_t commandCount; // commands walked (== header count on success)
    uint32_t byteSize;     // header byteSize
    uint32_t opcodeCounts[kMaxOpcode]; // histogram indexed by CommandOpcode
    uint32_t opcodeBytes[kMaxOpcode];  // encoded bytes, including each opcode field
};

// Dry-run decoder: walks the stream against the canonical wire layout without
// dereferencing any resource handle or needing a device/command list. This is
// deliberately a second, independent decoder - it exists to catch drift
// between the C# writer and the replayer's expectations.
bool DecodeCommandStream(const void* data, CommandStreamDecodeInfo& outInfo);
} // namespace unityrhi
