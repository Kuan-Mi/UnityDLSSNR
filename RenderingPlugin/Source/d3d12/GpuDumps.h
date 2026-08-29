#pragma once

// D3D12 Agility SDK GPU dump-file integration (preview). QueryInterfaces the
// preview device, configures dump options, installs the device-error
// begin/end callbacks, and - the real payoff for this project's
// DEVICE_HUNG-class bugs - serializes an always-on ring journal of the
// frame-fence / sync-point state into the dump blob. Entirely no-op (one log
// line) on a stock D3D12 runtime where the preview interface is absent.
//
// See GpuDumpPreview.h for why we mirror the preview vtable under our own name,
// and https://microsoft.github.io/DirectX-Specs/d3d/D3D12GpuDumps.html

#include <cstdarg>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "GpuDumpPreview.h"

struct ID3D12Device;

namespace unityrhi
{
class GpuDumps
{
public:
    static GpuDumps& Get();

    // QI the preview device; query D3D12_FEATURE_DUMP_FILE and log the
    // OS/driver support; ConfigureDumpFile with the default mask; install the
    // callbacks; and (dev builds) RetainDumpFile. Single log line + no-op when
    // the runtime is stock. Idempotent.
    void Initialize(ID3D12Device* device);
    void Shutdown();

    bool Available() const { return m_device != nullptr; }
    bool SupportedByOS() const { return m_supportedByOS; }
    uint32_t DriverTier() const { return m_driverTier; }
    uint32_t DriverOptionsMask() const { return m_driverOptionsMask; }

    // Re-run ConfigureDumpFile with a new option mask (D3D12_DUMP_FILE_DRIVER_OPTION_*
    // bits). False if the preview interface is unavailable or the call fails.
    bool SetOptions(uint32_t optionsMask);

    // D3D12_DEVICE_ERROR_CODE from the preview device (0 / NONE when healthy or
    // unavailable). Folded into Device::CheckDeviceRemoved.
    uint32_t DeviceErrorCode();

    // UTF-16 path of the most recent dump ("" if none). Copies up to cap-1
    // chars and always null-terminates. Returns the full string length.
    int GetLastDumpPath(wchar_t* buffer, int cap) const;

    // Device removal can be followed immediately by Unity's fatal crash
    // handler, which suspends the runtime worker before it finishes the dump.
    // Keep the render event alive briefly and expose callback progress so the
    // failure mode is observable even when Player.log stops abruptly.
    bool WaitForDumpCompletion(uint32_t timeoutMs);
    bool BeginCalled() const { return m_beginCalled.load(std::memory_order_acquire); }
    bool EndCalled() const { return m_endCalled.load(std::memory_order_acquire); }

    // Re-query the preview interface from a device and record COM identity.
    // This is deliberately read-only: SetDumpFileCallbacks is called exactly
    // once during Initialize, before any plugin GPU work is submitted.
    void TraceDeviceIdentity(ID3D12Device* device, const char* label);

    // ---- Phase 3: application journal ----
    // Records are the exact frame-fence / sync-point state we have had to
    // reconstruct by hand for past fence/flush bugs. Cheap, always-on, and
    // serialized into the dump blob on the device-error path.
    void RecordFrame(uint64_t frameIndex, uint64_t completedFence, uint64_t nextFrameFence);
    void RecordSyncPoint(const char* action, uint64_t value);
    void RecordEvent(const char* format, ...);

private:
    GpuDumps() = default;

    UINT OnDumpBegin(uint64_t flags);
    void OnDumpEnd(const wchar_t* path);
    // Match D3D12_DUMPFILEBEGINCALLBACK / D3D12_DUMPFILEENDCALLBACK exactly.
    static UINT BeginTrampoline(UINT64 flags);
    static void EndTrampoline(const wchar_t* path);

    void PushLine(const char* line);
    // Allocation-free: writes at most cap bytes into out, returns bytes written.
    size_t SerializeJournal(char* out, size_t cap) const;

    IUnityRhiDevicePreview* m_device = nullptr; // AddRef'd; released in Shutdown
    bool m_supportedByOS = false;
    uint32_t m_driverTier = 0;
    uint32_t m_driverOptionsMask = 0;
    std::atomic<bool> m_beginCalled{false};
    std::atomic<bool> m_endCalled{false};

    static constexpr size_t kLineChars = 128;
    static constexpr size_t kLineCount = 256;
    mutable std::mutex m_mutex;
    char m_lines[kLineCount][kLineChars] = {};
    uint64_t m_nextLine = 0;

    // Preallocated at Initialize so the begin callback (device-error path)
    // never allocates. Target 64 KB, well under the 2 MB blob cap.
    static constexpr size_t kBlobCap = 64 * 1024;
    char* m_blobBuffer = nullptr;

    wchar_t m_lastDumpPath[520] = {};
};
} // namespace unityrhi
