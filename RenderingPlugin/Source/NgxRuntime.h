#pragma once

#include <cstdint>
#include <mutex>

struct ID3D12Device;
struct NVSDK_NGX_Parameter;

namespace unityrhi
{
// NGX is process-global and its API is not thread-safe. DLSS, DLRR and the
// frame-generation present bridge share this owner and serialize every NGX
// create/evaluate/release operation through NgxMutex().
bool InitializeNgx(ID3D12Device* device);
void ShutdownNgx();
bool IsNgxInitialized();
std::mutex& NgxMutex();

// Each feature instance gets its own capability parameter map. This keeps the
// stateful helper parameters of DLRR and DLSS-G isolated while retaining the
// driver-provided callbacks stored in capability maps.
NVSDK_NGX_Parameter* CreateNgxFeatureParameters();
void DestroyNgxFeatureParameters(NVSDK_NGX_Parameter* parameters);

int32_t NgxInitResult();
int32_t NgxDlssAvailable();
int32_t NgxDlssInitResult();
int32_t NgxDlrrAvailable();
int32_t NgxDlrrInitResult();
int32_t NgxFrameGenerationAvailable();
int32_t NgxFrameGenerationInitResult();
uint32_t NgxFrameGenerationMultiFrameCountMax();
}
