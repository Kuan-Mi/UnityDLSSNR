#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace unityrhi
{
namespace wire
{
struct DlrrDispatchPayload;
}

bool InitializeDlrr(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue);
void ShutdownDlrr();
int32_t CreateDlrrInstance();
void DestroyDlrrInstance(int32_t instanceId);
bool DispatchDlrr(const wire::DlrrDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList);
}
