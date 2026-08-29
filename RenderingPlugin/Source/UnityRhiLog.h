#pragma once

#include <cstdarg>
#include <cstdio>

#include "IUnityLog.h"

namespace unityrhi
{
// Set/cleared by PluginMain on UnityPluginLoad/Unload. May be null (e.g. when the
// plugin is loaded outside Unity); all logging must tolerate that.
extern IUnityLog* g_Log;

inline void LogVFormat(UnityLogType type, const char* format, va_list args)
{
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    buffer[sizeof(buffer) - 1] = '\0';
    if (g_Log)
        g_Log->Log(type, buffer, "UnityRHI", 0);
    else
        fprintf(stderr, "%s\n", buffer); // standalone (native unit tests)
}

inline void LogInfo(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogVFormat(kUnityLogTypeLog, format, args);
    va_end(args);
}

inline void LogWarning(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogVFormat(kUnityLogTypeWarning, format, args);
    va_end(args);
}

inline void LogError(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogVFormat(kUnityLogTypeError, format, args);
    va_end(args);
}
} // namespace unityrhi
