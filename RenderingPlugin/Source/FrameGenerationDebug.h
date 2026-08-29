#pragma once

#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;
struct IDXGISwapChain3;

namespace unityrhi
{
#pragma pack(push, 8)
// Blittable ABI submitted synchronously from RtxptEngine. Resource pointers are
// raw ID3D12Resource pointers; SubmitFrameGenerationInputs retains them.
struct FrameGenerationInputs
{
    ID3D12Resource* depth;
    ID3D12Resource* motionVectors;
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t colorWidth;
    uint32_t colorHeight;
    uint32_t depthState;
    uint32_t motionVectorsState;
    uint64_t frameId;
    float cameraViewToClip[16];
    float clipToCameraView[16];
    float clipToPrevClip[16];
    float prevClipToClip[16];
    float cameraPos[3];
    float cameraUp[3];
    float cameraRight[3];
    float cameraFwd[3];
    float jitterX;
    float jitterY;
    float motionVectorScaleX;
    float motionVectorScaleY;
    float cameraNear;
    float cameraFar;
    float cameraFov;
    float cameraAspect;
    uint32_t depthInverted;
    uint32_t cameraMotionIncluded;
    uint32_t reset;
    uint32_t colorBuffersHdr;
};
#pragma pack(pop)

static_assert(sizeof(FrameGenerationInputs) == 400);

bool InitializeFrameGeneration(ID3D12Device* device);
void ShutdownFrameGeneration();
void SetFrameGenerationEnabled(bool enabled);
bool IsFrameGenerationEnabled();
void SubmitFrameGenerationInputs(const FrameGenerationInputs& inputs);
void NoteDisplayedPresent();
void NoteGeneratedPresent();
void NoteRealPresent();
uint64_t GetDisplayedPresentCount();
uint64_t GetGeneratedPresentCount();
uint64_t GetRealPresentCount();

enum class FrameGenerationPresentAction
{
    None = 0,
    Replace = 1,
    Insert = 2,
};

// presentSwapChain is the real DXGI chain we Present onto.
// colorBuffer is Unity's current rendered image (proxy GetBuffer). When null,
// the real chain's current backbuffer is used (replace-only fallback).
FrameGenerationPresentAction EvaluateFrameGeneration(
    IDXGISwapChain3* presentSwapChain,
    ID3D12CommandQueue* presentQueue,
    ID3D12Resource* colorBuffer);

// Copies the retained real frame from the given ping-pong slot into the next
// backbuffer. Does not CPU-wait; Present on the same queue orders after the copy.
bool CopyFrameGenerationRetainedReal(
    IDXGISwapChain3* swapChain, ID3D12CommandQueue* presentQueue, uint32_t realSlot);

// Slot written by the most recent successful Evaluate (for the pacer thread).
uint32_t ConsumeFrameGenerationPacerRealSlot();
}
