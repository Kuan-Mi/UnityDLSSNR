#pragma once

// Phase 5: marker breadcrumbs for device-removed diagnostics. The replayer
// pushes one short line per notable command; when the D3D12 device is removed
// (TDR, driver reset, invalid call) the ring is dumped so the log shows what
// the GPU was executing last. Writes are a handful per replayed stream and
// reads happen only on device removal, so a mutex is plenty.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace unityrhi
{
class Breadcrumbs
{
public:
    static constexpr size_t kEntryChars = 120;
    static constexpr size_t kEntryCount = 64;

    static Breadcrumbs& Get()
    {
        static Breadcrumbs instance;
        return instance;
    }

    void Push(const char* format, ...)
    {
        char line[kEntryChars];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof line, format, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(m_mutex);
        std::memcpy(m_entries[m_next % kEntryCount], line, sizeof line);
        m_entries[m_next % kEntryCount][kEntryChars - 1] = '\0';
        ++m_next;
    }

    void PushDispatch(uint32_t x, uint32_t y, uint32_t z, const char* pipeline)
    {
#if defined(_DEBUG)
        Push("dispatch %ux%ux%u pipeline='%s'", x, y, z, pipeline);
#else
        (void)x;
        (void)y;
        (void)z;
        (void)pipeline;
#endif
    }

    // Invokes sink(sequenceNumber, line) for each retained entry, oldest first.
    template <typename Sink>
    void Dump(Sink&& sink)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint64_t begin = m_next > kEntryCount ? m_next - kEntryCount : 0;
        for (uint64_t i = begin; i < m_next; ++i)
            sink(i, m_entries[i % kEntryCount]);
    }

private:
    Breadcrumbs() = default;

    std::mutex m_mutex;
    char m_entries[kEntryCount][kEntryChars] = {};
    uint64_t m_next = 0;
};
} // namespace unityrhi
