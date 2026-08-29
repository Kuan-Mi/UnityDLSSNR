#pragma once

#include <atomic>
#include <unordered_map>
#include <iostream>
#include <dxgi1_6.h>
#include <d3d12.h>
#include "d3dx12.h"
#include "FrameData.h"

#include "NRD.h"
#include "NRDDescs.h"
#include "NRI.h"
#include "Extensions/NRIHelper.h" 
#include "Extensions/NRIWrapperD3D12.h"
#include "Extensions/NRIUpscaler.h"
#include "NRDIntegration.h"

#include "dxgi.h"
#include "RRFrameData.h"
#include "IUnityGraphicsD3D12.h"
#include "IUnityLog.h"
class DLRRInstance
{
public:
    DLRRInstance(IUnityInterfaces* interfaces,int instanceId);
    ~DLRRInstance();
    nri::Descriptor* GetOrCreateDescriptor(nri::Texture* texture, bool isStorage);
    nri::UpscalerResource GetPair(nri::Texture* texture, bool cond);  
    void DispatchCompute(RRFrameData* data);
    void initialize_and_create_resources();
    void release_resources();

private:
    IUnityGraphicsD3D12v8* s_d3d12 = nullptr;
    IUnityLog* s_Log = nullptr;
    int id;
    std::atomic<bool> m_are_resources_initialized{false};
    
    UINT TextureWidth = 0;
    UINT TextureHeight = 0;
    nri::UpscalerMode upscalerMode;
    uint8_t preset = 0;
    std::unordered_map<uint64_t, nri::Descriptor*> m_DescriptorCache;
    nri::Upscaler* m_DLRR = nullptr;
};
