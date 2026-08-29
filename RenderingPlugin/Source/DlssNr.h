#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace unityrhi
{
namespace wire { struct DlssNrDispatchPayload; }

bool InitializeDlssNr(ID3D12Device* device);
void ShutdownDlssNr();
void TickDlssNr();
int32_t CreateDlssNrInstance();
void DestroyDlssNrInstance(int32_t instanceId);
bool DispatchDlssNr(const wire::DlssNrDispatchPayload& command,
    ID3D12GraphicsCommandList* commandList);
int32_t DlssNrLastCreateResult();
int32_t DlssNrLastEvaluateResult();
}
