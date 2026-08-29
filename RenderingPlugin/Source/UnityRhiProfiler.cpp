#include "UnityRhiProfiler.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "IUnityProfiler.h"

namespace unityrhi
{
namespace
{
IUnityProfiler* g_Profiler = nullptr;
bool g_ProfilerAvailable = false;
uint64_t g_ProfilerGeneration = 1;
struct TransparentStringHash
{
    using is_transparent = void;
    size_t operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }
};
std::unordered_map<std::string, const UnityProfilerMarkerDesc*,
    TransparentStringHash, std::equal_to<>> g_Markers;
std::vector<const UnityProfilerMarkerDesc*> g_CommandMarkerStack;

const UnityProfilerMarkerDesc* GetMarker(const char* name)
{
    if (!g_ProfilerAvailable || !name || !*name)
        return nullptr;

    const auto found = g_Markers.find(std::string_view(name));
    if (found != g_Markers.end())
        return found->second;

    const UnityProfilerMarkerDesc* marker = nullptr;
    if (g_Profiler->CreateMarker(&marker, name, kUnityProfilerCategoryRender,
            kUnityProfilerMarkerFlagDefault, 0) != 0)
        marker = nullptr;
    g_Markers.emplace(name, marker);
    return marker;
}

void EndSample(const UnityProfilerMarkerDesc* marker)
{
    if (marker && g_ProfilerAvailable)
        g_Profiler->EndSample(marker);
}
} // namespace

void InitializeNativeProfiler(IUnityInterfaces* interfaces)
{
    ++g_ProfilerGeneration;
    g_Profiler = interfaces ? interfaces->Get<IUnityProfiler>() : nullptr;
    g_ProfilerAvailable = g_Profiler && g_Profiler->IsAvailable() != 0;
}

void ShutdownNativeProfiler()
{
    ++g_ProfilerGeneration;
    CloseCommandMarkerProfiles();
    g_Markers.clear();
    g_ProfilerAvailable = false;
    g_Profiler = nullptr;
}

NativeProfileScope::NativeProfileScope(const char* name)
{
    // Native scopes overwhelmingly use string literals. Avoid hashing the same
    // marker name for every replayed command while retaining the string-keyed
    // registry for command markers and first use. The generation prevents a
    // cached descriptor from surviving profiler shutdown/reinitialization.
    struct CacheEntry
    {
        const char* name = nullptr;
        const UnityProfilerMarkerDesc* marker = nullptr;
        uint64_t generation = 0;
    };
    static thread_local CacheEntry cache[32];
    const uintptr_t key = reinterpret_cast<uintptr_t>(name);
    CacheEntry& entry = cache[(key >> 4u) & 31u];
    if (entry.name != name || entry.generation != g_ProfilerGeneration)
    {
        entry.name = name;
        entry.marker = GetMarker(name);
        entry.generation = g_ProfilerGeneration;
    }
    const UnityProfilerMarkerDesc* marker = entry.marker;
    m_Marker = marker;
    if (marker)
        g_Profiler->BeginSample(marker);
}

NativeProfileScope::~NativeProfileScope()
{
    EndSample(static_cast<const UnityProfilerMarkerDesc*>(m_Marker));
}

void BeginCommandMarkerProfile(const char* name)
{
    const UnityProfilerMarkerDesc* marker = GetMarker(name);
    g_CommandMarkerStack.push_back(marker);
    if (marker)
        g_Profiler->BeginSample(marker);
}

void EndCommandMarkerProfile()
{
    if (g_CommandMarkerStack.empty())
        return;
    const UnityProfilerMarkerDesc* marker = g_CommandMarkerStack.back();
    g_CommandMarkerStack.pop_back();
    EndSample(marker);
}

void CloseCommandMarkerProfiles()
{
    while (!g_CommandMarkerStack.empty())
        EndCommandMarkerProfile();
}
} // namespace unityrhi
