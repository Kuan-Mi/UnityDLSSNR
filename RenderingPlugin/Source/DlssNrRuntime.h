#pragma once

#include <cstdint>
#include <mutex>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace unityrhi
{
bool InitializeDlssNrRuntime(ID3D12Device* device);
void ShutdownDlssNrRuntime();
bool IsDlssNrRuntimeAvailable();
int32_t DlssNrRuntimeInitResult();
std::mutex& DlssNrMutex();

NVSDK_NGX_Parameter* AllocateDlssNrParameters();
void DestroyDlssNrParameters(NVSDK_NGX_Parameter* parameters);
int32_t CreateDlssNrFeature(ID3D12GraphicsCommandList* commandList,
    NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle);
int32_t EvaluateDlssNrFeature(ID3D12GraphicsCommandList* commandList,
    const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* parameters);
int32_t ReleaseDlssNrFeature(NVSDK_NGX_Handle* handle);
}
