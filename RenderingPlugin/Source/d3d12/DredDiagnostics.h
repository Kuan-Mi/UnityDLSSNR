#pragma once

#include <windows.h>

struct ID3D12Device;

namespace unityrhi
{
// Query DRED after device removal and write a standalone report beside the
// Player executable. DRED must have been enabled before device creation (the
// deployment workflow uses d3dconfig for that).
void WriteDredReport(ID3D12Device* device, HRESULT removedReason);
}
