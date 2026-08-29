#pragma once

struct IUnityInterfaces;

namespace unityrhi
{
void InitializeNativeProfiler(IUnityInterfaces* interfaces);
void ShutdownNativeProfiler();

class NativeProfileScope
{
public:
    explicit NativeProfileScope(const char* name);
    ~NativeProfileScope();

    NativeProfileScope(const NativeProfileScope&) = delete;
    NativeProfileScope& operator=(const NativeProfileScope&) = delete;

private:
    const void* m_Marker = nullptr;
};

// Mirrors command-list BeginMarker/EndMarker pairs into Unity's CPU profiler.
// A separate stack lets a malformed stream close all still-open samples.
void BeginCommandMarkerProfile(const char* name);
void EndCommandMarkerProfile();
void CloseCommandMarkerProfiles();
} // namespace unityrhi
