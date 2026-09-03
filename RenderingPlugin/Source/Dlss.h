#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace unityrhi
{
namespace wire
{
struct DlssDispatchPayload;
}

bool InitializeDlss(ID3D12Device* device);
void ShutdownDlss();
int32_t CreateDlssInstance();
void DestroyDlssInstance(int32_t instanceId);
bool DispatchDlss(const wire::DlssDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList);
int32_t DlssLastCreateResult();
int32_t DlssLastEvaluateResult();
// Query NGX optimal render size for a display resolution + quality mode.
// Returns 1 on success and writes renderWidth/renderHeight; 0 on failure.
int32_t QueryDlssOptimalSettings(uint32_t outputWidth, uint32_t outputHeight,
    uint8_t upscalerMode, uint32_t* renderWidth, uint32_t* renderHeight);
}
