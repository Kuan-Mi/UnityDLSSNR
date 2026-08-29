#include <cassert>
#include <mutex>
#include <unordered_map>

#include "DLRRInstance.h"
#include "DLSRInstance.h"
#include "NISInstance.h"
#include "RenderSystem.h"
#include "NrdInstance.h"
#include "RRFrameData.h"
#include "DLSRFrameData.h"
#include "NISFrameData.h"
#include "IUnityLog.h"


#define LOG(msg) UNITY_LOG(s_Logger, msg)


namespace
{
    IUnityInterfaces* s_UnityInterfaces = nullptr;
    IUnityGraphics* s_Graphics = nullptr;
    IUnityLog* s_Logger = nullptr;

    // Cache for DLRR_QueryOptimalRenderSize — keyed by (outputWidth, outputHeight, mode).
    // A temporary upscaler is created once per unique key to query the NGX-recommended
    // render resolution, matching RTXPT's QueryDLSSOptimalSettings path.
    struct OptimalResKey
    {
        uint32_t outputWidth;
        uint32_t outputHeight;
        uint8_t  mode;
        bool operator==(const OptimalResKey& o) const
        {
            return outputWidth == o.outputWidth && outputHeight == o.outputHeight && mode == o.mode;
        }
    };
    struct OptimalResKeyHash
    {
        size_t operator()(const OptimalResKey& k) const
        {
            uint64_t v = ((uint64_t)k.outputWidth << 24) | ((uint64_t)k.outputHeight << 8) | k.mode;
            return std::hash<uint64_t>()(v);
        }
    };
    std::unordered_map<OptimalResKey, nri::Dim2_t, OptimalResKeyHash> g_OptimalResCache;
    std::mutex g_OptimalResCacheMutex;

    std::unordered_map<int32_t, NrdInstance*> g_NrdInstances;
    std::mutex g_NrdInstanceMutex;
    int32_t g_NrdNextInstanceId = 1;

    std::unordered_map<int32_t, DLRRInstance*> g_DLRRInstances;
    std::mutex g_DLRRInstanceMutex;
    int32_t g_DLRRNextInstanceId = 1;

    std::unordered_map<int32_t, DLSRInstance*> g_DLSRInstances;
    std::mutex g_DLSRInstanceMutex;
    int32_t g_DLSRNextInstanceId = 1;

    std::unordered_map<int32_t, NISInstance*> g_NISInstances;
    std::mutex g_NISInstanceMutex;
    int32_t g_NISNextInstanceId = 1;


    // 图形设备事件回调
    void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
    {
        // 初始化时，创建图形API
        if (eventType == kUnityGfxDeviceEventInitialize)
        {
            RenderSystem::Get().Initialize(s_UnityInterfaces);
        }

        // 让图形API处理与设备相关的事件
        RenderSystem::Get().ProcessDeviceEvent(eventType, s_UnityInterfaces);

        // 在关闭时清理图形API
        if (eventType == kUnityGfxDeviceEventShutdown)
        {
            std::scoped_lock lock(g_NrdInstanceMutex);
            for (auto& pair : g_NrdInstances) delete pair.second;
            g_NrdInstances.clear();

            RenderSystem::Get().Shutdown();
        }
    }

    // 渲染事件和数据的回调
    void UNITY_INTERFACE_API OnRenderEventAndData(int eventID, void* data)
    {
        if (eventID == 1)
        {
            FrameData* frameData = static_cast<FrameData*>(data);

            std::scoped_lock lock(g_NrdInstanceMutex);
            auto it = g_NrdInstances.find(frameData->instanceId);
            if (it != g_NrdInstances.end())
            {
                it->second->DispatchCompute(frameData);
            }
        }
        else if (eventID == 2)
        {
            // DLRR 事件处理
            RRFrameData* frameData = static_cast<RRFrameData*>(data);

            std::scoped_lock lock(g_DLRRInstanceMutex);
            auto it = g_DLRRInstances.find(frameData->instanceId);
            if (it != g_DLRRInstances.end())
            {
                it->second->DispatchCompute(frameData);
            }
        }
        else if (eventID == 3)
        {
            // DLSR 事件处理
            DLSRFrameData* frameData = static_cast<DLSRFrameData*>(data);

            std::scoped_lock lock(g_DLSRInstanceMutex);
            auto it = g_DLSRInstances.find(frameData->instanceId);
            if (it != g_DLSRInstances.end())
            {
                it->second->DispatchCompute(frameData);
            }
        }
        else if (eventID == 4)
        {
            // NIS 事件处理
            NISFrameData* frameData = static_cast<NISFrameData*>(data);

            std::scoped_lock lock(g_NISInstanceMutex);
            auto it = g_NISInstances.find(frameData->instanceId);
            if (it != g_NISInstances.end())
            {
                it->second->DispatchCompute(frameData);
            }
        }
    }
}

// 加载Unity插件
extern "C" {
void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
    s_UnityInterfaces = unityInterfaces;
    // 获取IUnityGraphics接口
    s_Graphics = s_UnityInterfaces->Get<IUnityGraphics>();
    s_Logger = s_UnityInterfaces->Get<IUnityLog>();
    // 注册回调以接收图形设备事件
    s_Graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);

    // 在插件加载时手动运行OnGraphicsDeviceEvent（initialize）
    OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);

    LOG("[NRD Native] UnityPluginLoad completed.");
}

// 卸载Unity插件
void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
    // 取消注册图形设备事件回调
    s_Graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
    LOG("[NRD Native] UnityPluginUnload completed.");
}

// 获取渲染事件和数据的函数指针
UnityRenderingEventAndData UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetRenderEventAndDataFunc()
{
    return OnRenderEventAndData;
}

// C# 构造时调用
UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API CreateDenoiserInstance(NrdDenoiserDesc* denoisers, int count)
{
    std::scoped_lock lock(g_NrdInstanceMutex);
    int id = g_NrdNextInstanceId++;
    g_NrdInstances[id] = new NrdInstance(s_UnityInterfaces, id, denoisers, count);
    return id;
}

UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API CreateDLRRInstance()
{
    std::scoped_lock lock(g_NrdInstanceMutex);
    int id = g_NrdNextInstanceId++;
    g_DLRRInstances[id] = new DLRRInstance(s_UnityInterfaces, id);
    return id;
}

// C# Dispose 时调用
UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API DestroyDenoiserInstance(int id)
{
    std::scoped_lock lock(g_NrdInstanceMutex);
    auto it = g_NrdInstances.find(id);
    if (it != g_NrdInstances.end())
    {
        delete it->second;
        g_NrdInstances.erase(it);
    }
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API DestroyDLRRInstance(int id)
{
    std::scoped_lock lock(g_DLRRInstanceMutex);
    auto it = g_DLRRInstances.find(id);
    if (it != g_DLRRInstances.end())
    {
        delete it->second;
        g_DLRRInstances.erase(it);
    }
}

UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API CreateDLSRInstance()
{
    std::scoped_lock lock(g_DLSRInstanceMutex);
    int id = g_DLSRNextInstanceId++;
    g_DLSRInstances[id] = new DLSRInstance(s_UnityInterfaces, id);
    return id;
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API DestroyDLSRInstance(int id)
{
    std::scoped_lock lock(g_DLSRInstanceMutex);
    auto it = g_DLSRInstances.find(id);
    if (it != g_DLSRInstances.end())
    {
        delete it->second;
        g_DLSRInstances.erase(it);
    }
}

UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API CreateNISInstance()
{
    std::scoped_lock lock(g_NISInstanceMutex);
    int id = g_NISNextInstanceId++;
    g_NISInstances[id] = new NISInstance(s_UnityInterfaces, id);
    return id;
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API DestroyNISInstance(int id)
{
    std::scoped_lock lock(g_NISInstanceMutex);
    auto it = g_NISInstances.find(id);
    if (it != g_NISInstances.end())
    {
        delete it->second;
        g_NISInstances.erase(it);
    }
}

// C# Dispose 时调用
UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API WrapD3D12Texture(ID3D12Resource* resource, DXGI_FORMAT format)
{
    return RenderSystem::Get().WrapD3D12Texture(resource, format);
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API ReleaseTexture(nri::Texture* nriTex)
{
    RenderSystem::Get().Release(nriTex);
}

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UpdateDenoiserResources(
    int instanceId,
    NrdResourceInput* resources,
    int count)
{
    std::scoped_lock lock(g_NrdInstanceMutex);
    auto it = g_NrdInstances.find(instanceId);
    if (it != g_NrdInstances.end())
    {
        it->second->UpdateResources(resources, count);
    }
}

// Query the NGX/Streamline-recommended render resolution for a given output resolution
// and DLSS mode, matching RTXPT Sample.cpp QueryDLSSOptimalSettings().
// Creates a temporary DLRR upscaler (without a command buffer, which triggers a GPU
// wait-for-idle) on the first call per unique (outputWidth, outputHeight, mode);
// subsequent calls return immediately from cache.
// mode matches nri::UpscalerMode: 0=NATIVE,1=ULTRA_QUALITY,2=QUALITY,3=BALANCED,
//                                  4=PERFORMANCE,5=ULTRA_PERFORMANCE.
UNITY_INTERFACE_EXPORT bool UNITY_INTERFACE_API DLRR_QueryOptimalRenderSize(
    uint32_t  outputWidth,
    uint32_t  outputHeight,
    uint8_t   mode,
    uint32_t* outRenderWidth,
    uint32_t* outRenderHeight)
{
    if (!outRenderWidth || !outRenderHeight)
        return false;

    // NATIVE (mode 0): render resolution equals output resolution.
    if (mode == 0)
    {
        *outRenderWidth  = outputWidth;
        *outRenderHeight = outputHeight;
        return true;
    }

    OptimalResKey key{ outputWidth, outputHeight, mode };

    {
        std::scoped_lock lock(g_OptimalResCacheMutex);
        auto it = g_OptimalResCache.find(key);
        if (it != g_OptimalResCache.end())
        {
            *outRenderWidth  = it->second.w;
            *outRenderHeight = it->second.h;
            return true;
        }
    }

    // Cache miss: create a temporary upscaler to ask NGX for the optimal resolution.
    RenderSystem& rs = RenderSystem::Get();
    if (!rs.GetNriDevice())
        return false;

    nri::UpscalerDesc desc = {};
    desc.upscaleResolution = { (nri::Dim_t)outputWidth, (nri::Dim_t)outputHeight };
    desc.type  = nri::UpscalerType::DLRR;
    desc.mode  = (nri::UpscalerMode)mode;
    desc.flags = nri::UpscalerBits::DEPTH_INFINITE | nri::UpscalerBits::HDR | nri::UpscalerBits::DEPTH_INVERTED;
    desc.commandBuffer = nullptr; // no command buffer → NRI blocks until GPU is idle

    nri::Upscaler* upscaler = nullptr;
    nri::Result r = rs.GetNriUpScaler().CreateUpscaler(*rs.GetNriDevice(), desc, upscaler);
    if (r != nri::Result::SUCCESS || !upscaler)
        return false;

    nri::UpscalerProps props = {};
    rs.GetNriUpScaler().GetUpscalerProps(*upscaler, props);
    rs.GetNriUpScaler().DestroyUpscaler(upscaler);

    {
        std::scoped_lock lock(g_OptimalResCacheMutex);
        g_OptimalResCache[key] = props.renderResolution;
    }

    *outRenderWidth  = props.renderResolution.w;
    *outRenderHeight = props.renderResolution.h;
    return true;
}
}
