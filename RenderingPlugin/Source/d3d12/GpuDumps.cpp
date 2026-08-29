#include "GpuDumps.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "UnityRhiLog.h"

namespace unityrhi
{
namespace
{
// Tag passed to AddBlobToDumpFile so our journal blob is identifiable in the
// .dxdmp ("URHJ" = UnityRHI Journal, little-endian).
constexpr UINT64 kJournalBlobTag = 0x4A485255ULL; // 'U''R''H''J'

// Unity's fatal crash handler can stop normal logging (and suspend other
// threads) as soon as Present observes device removal. This allocation-free
// trace goes directly beside the Player executable so callback timing remains
// visible even if Player.log never receives the final messages.
void DiagnosticTrace(const char* format, ...)
{
    char message[1024];
    const int prefix = snprintf(message, sizeof message, "[%llu ms tid=%lu] ",
        static_cast<unsigned long long>(GetTickCount64()),
        static_cast<unsigned long>(GetCurrentThreadId()));
    if (prefix < 0 || static_cast<size_t>(prefix) >= sizeof message)
        return;

    va_list args;
    va_start(args, format);
    const int body = vsnprintf(message + prefix, sizeof message - static_cast<size_t>(prefix), format, args);
    va_end(args);
    if (body < 0)
        return;

    size_t length = strnlen(message, sizeof message - 3);
    message[length++] = '\r';
    message[length++] = '\n';
    message[length] = '\0';
    OutputDebugStringA(message);

    wchar_t path[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    *slash = L'\0';
    if (wcslen(path) + wcslen(L"\\UnityRHI-GpuDumpTrace.log") + 1 > MAX_PATH)
        return;
    wcscat_s(path, L"\\UnityRHI-GpuDumpTrace.log");

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(length), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

const char* DriverTierName(uint32_t tier)
{
    switch (tier)
    {
    case D3D12_DUMP_FILE_DRIVER_TIER_NOT_SUPPORTED: return "NOT_SUPPORTED";
    case D3D12_DUMP_FILE_DRIVER_TIER_1: return "TIER_1";
    case D3D12_DUMP_FILE_DRIVER_TIER_2: return "TIER_2";
    default: return "unknown";
    }
}
} // namespace

GpuDumps& GpuDumps::Get()
{
    static GpuDumps instance;
    return instance;
}

void GpuDumps::Initialize(ID3D12Device* device)
{
    if (m_device || !device)
        return;

    m_beginCalled.store(false, std::memory_order_release);
    m_endCalled.store(false, std::memory_order_release);
    m_lastDumpPath[0] = L'\0';
    DiagnosticTrace("Initialize begin: ID3D12Device=%p", static_cast<void*>(device));

    IUnityRhiDevicePreview* preview = nullptr;
    if (FAILED(device->QueryInterface(
            __uuidof(IUnityRhiDevicePreview), reinterpret_cast<void**>(&preview))) ||
        !preview)
    {
        LogInfo("[UnityRHI] GPU dump-file API unavailable (stock D3D12 runtime); GPU dumps disabled.");
        return;
    }
    m_device = preview; // QueryInterface AddRef'd; released in Shutdown
    DiagnosticTrace("Preview interface acquired: preview=%p", static_cast<void*>(preview));

    if (!m_blobBuffer)
        m_blobBuffer = new char[kBlobCap];

    D3D12_FEATURE_DATA_DUMP_FILE data{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_DUMP_FILE, &data, sizeof data)))
    {
        m_supportedByOS = data.SupportedByOS != FALSE;
        m_driverTier = static_cast<uint32_t>(data.DumpFileDriverTier);
        m_driverOptionsMask = data.DumpFileDriverOptionsMask;
        LogInfo("[UnityRHI] GPU dump-file: SupportedByOS=%d driverTier=%s(%u) driverOptionsMask=0x%X",
            m_supportedByOS ? 1 : 0, DriverTierName(m_driverTier), m_driverTier, m_driverOptionsMask);
    }
    else
    {
        LogInfo("[UnityRHI] GPU dump-file: CheckFeatureSupport(DUMP_FILE) failed; preview interface present.");
    }

    // Default: cheapest useful dump. RESOURCES / SHADER_REGISTERS are driver-tier
    // dependent, so they are opt-in via SetOptions / the C# settings toggle.
    // All these bits are DRIVER options: ConfigureDumpFile only succeeds on a
    // driver that reports a supported tier, so skip it on NOT_SUPPORTED - the
    // callbacks below still enable OS-level dumps + the application journal blob.
    constexpr uint32_t kDefaultOptions =
        D3D12_DUMP_FILE_DRIVER_OPTION_NO_OVERHEAD | D3D12_DUMP_FILE_DRIVER_OPTION_EVENT_MARKERS;
    if (m_driverTier == D3D12_DUMP_FILE_DRIVER_TIER_NOT_SUPPORTED)
    {
        LogInfo("[UnityRHI] GPU dump-file: driver tier NOT_SUPPORTED; skipping driver ConfigureDumpFile. "
                "OS-level dumps + the application journal blob remain available.");
    }
    else
    {
        const uint32_t effectiveOptions = kDefaultOptions & m_driverOptionsMask;
        if (effectiveOptions != kDefaultOptions)
            LogInfo("[UnityRHI] GPU dump-file: driver supports options 0x%X; requested 0x%X reduced to 0x%X.",
                m_driverOptionsMask, kDefaultOptions, effectiveOptions);
        const HRESULT hrCfg =
            m_device->ConfigureDumpFile(static_cast<D3D12_DUMP_FILE_DRIVER_OPTIONS>(effectiveOptions));
        if (FAILED(hrCfg))
            LogWarning("[UnityRHI] GPU dump-file: ConfigureDumpFile(0x%X) failed (0x%08X).",
                effectiveOptions, static_cast<unsigned>(hrCfg));
    }

    m_device->SetDumpFileCallbacks(&GpuDumps::BeginTrampoline, &GpuDumps::EndTrampoline);
    // Always retain while this diagnostics integration is enabled. In Release,
    // making this conditional silently removes the dump after End returns.
    m_device->RetainDumpFile(TRUE);
    DiagnosticTrace("Callbacks installed: begin=%p end=%p RetainDumpFile(TRUE)",
        reinterpret_cast<void*>(&GpuDumps::BeginTrampoline),
        reinterpret_cast<void*>(&GpuDumps::EndTrampoline));

    LogInfo("[UnityRHI] GPU dump-file callbacks installed (default options NO_OVERHEAD | EVENT_MARKERS).");
    RecordEvent("gpu-dumps initialized: os=%d tier=%s driverOptionsMask=0x%X",
        m_supportedByOS ? 1 : 0, DriverTierName(m_driverTier), m_driverOptionsMask);
}

void GpuDumps::Shutdown()
{
    DiagnosticTrace("Shutdown: beginCalled=%d endCalled=%d", BeginCalled() ? 1 : 0, EndCalled() ? 1 : 0);
    if (m_device)
    {
        m_device->Release();
        m_device = nullptr;
    }
    delete[] m_blobBuffer;
    m_blobBuffer = nullptr;
}

bool GpuDumps::SetOptions(uint32_t optionsMask)
{
    if (!m_device)
        return false;
    const HRESULT hr = m_device->ConfigureDumpFile(static_cast<D3D12_DUMP_FILE_DRIVER_OPTIONS>(optionsMask));
    if (SUCCEEDED(hr))
    {
        m_driverOptionsMask = optionsMask;
        RecordEvent("gpu-dumps options set to 0x%X", optionsMask);
    }
    else
    {
        LogWarning("[UnityRHI] GPU dump-file: ConfigureDumpFile(0x%X) failed (0x%08X).",
            optionsMask, static_cast<unsigned>(hr));
    }
    return SUCCEEDED(hr);
}

uint32_t GpuDumps::DeviceErrorCode()
{
    return m_device ? static_cast<uint32_t>(m_device->GetDeviceErrorCode()) : 0u;
}

int GpuDumps::GetLastDumpPath(wchar_t* buffer, int cap) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const int len = static_cast<int>(wcslen(m_lastDumpPath));
    if (buffer && cap > 0)
    {
        const int n = len < cap - 1 ? len : cap - 1;
        std::memcpy(buffer, m_lastDumpPath, static_cast<size_t>(n) * sizeof(wchar_t));
        buffer[n] = L'\0';
    }
    return len;
}

bool GpuDumps::WaitForDumpCompletion(uint32_t timeoutMs)
{
    if (!m_device)
        return false;

    constexpr DWORD kPollMs = 50;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    DiagnosticTrace("WaitForDump begin: timeoutMs=%u beginCalled=%d endCalled=%d errorCode=0x%X",
        timeoutMs, BeginCalled() ? 1 : 0, EndCalled() ? 1 : 0,
        static_cast<unsigned>(m_device->GetDeviceErrorCode()));

    uint32_t pollCount = 0;
    while (!EndCalled() && GetTickCount64() < deadline)
    {
        Sleep(kPollMs);
        ++pollCount;
        if ((pollCount % 10) == 0)
        {
            DiagnosticTrace("WaitForDump poll: beginCalled=%d endCalled=%d errorCode=0x%X",
                BeginCalled() ? 1 : 0, EndCalled() ? 1 : 0,
                static_cast<unsigned>(m_device->GetDeviceErrorCode()));
        }
    }

    DiagnosticTrace("WaitForDump end: beginCalled=%d endCalled=%d errorCode=0x%X",
        BeginCalled() ? 1 : 0, EndCalled() ? 1 : 0,
        static_cast<unsigned>(m_device->GetDeviceErrorCode()));
    return EndCalled();
}

void GpuDumps::TraceDeviceIdentity(ID3D12Device* device, const char* label)
{
    if (!device)
    {
        DiagnosticTrace("Device identity [%s]: null ID3D12Device", label ? label : "?");
        return;
    }

    IUnknown* deviceIdentity = nullptr;
    IUnknown* cachedIdentity = nullptr;
    IUnknown* freshIdentity = nullptr;
    IUnityRhiDevicePreview* freshPreview = nullptr;
    const HRESULT freshHr = device->QueryInterface(
        __uuidof(IUnityRhiDevicePreview), reinterpret_cast<void**>(&freshPreview));
    device->QueryInterface(IID_PPV_ARGS(&deviceIdentity));
    if (m_device)
        m_device->QueryInterface(IID_PPV_ARGS(&cachedIdentity));
    if (freshPreview)
        freshPreview->QueryInterface(IID_PPV_ARGS(&freshIdentity));

    const LUID luid = device->GetAdapterLuid();
    const bool sameAsCached = deviceIdentity && cachedIdentity && deviceIdentity == cachedIdentity;
    const bool freshMatches = deviceIdentity && freshIdentity && deviceIdentity == freshIdentity;
    DiagnosticTrace(
        "Device identity [%s]: device=%p deviceIUnknown=%p cachedPreview=%p cachedIUnknown=%p "
        "freshQI=0x%08X freshPreview=%p freshIUnknown=%p sameCached=%d freshMatches=%d "
        "adapterLuid=%08lX:%08lX",
        label ? label : "?", static_cast<void*>(device), static_cast<void*>(deviceIdentity),
        static_cast<void*>(m_device), static_cast<void*>(cachedIdentity),
        static_cast<unsigned>(freshHr), static_cast<void*>(freshPreview),
        static_cast<void*>(freshIdentity), sameAsCached ? 1 : 0, freshMatches ? 1 : 0,
        static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart));

    if (freshIdentity) freshIdentity->Release();
    if (cachedIdentity) cachedIdentity->Release();
    if (deviceIdentity) deviceIdentity->Release();
    if (freshPreview) freshPreview->Release();
}

// ---- journal ----

void GpuDumps::PushLine(const char* line)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    char* slot = m_lines[m_nextLine % kLineCount];
    size_t len = std::strlen(line);
    if (len > kLineChars - 1)
        len = kLineChars - 1;
    std::memcpy(slot, line, len);
    slot[len] = '\0';
    ++m_nextLine;
}

void GpuDumps::RecordEvent(const char* format, ...)
{
    char line[kLineChars];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    line[kLineChars - 1] = '\0';
    PushLine(line);
}

void GpuDumps::RecordFrame(uint64_t frameIndex, uint64_t completedFence, uint64_t nextFrameFence)
{
    RecordEvent("frame %llu: completedFence=%llu nextFrameFence=%llu",
        static_cast<unsigned long long>(frameIndex),
        static_cast<unsigned long long>(completedFence),
        static_cast<unsigned long long>(nextFrameFence));
}

void GpuDumps::RecordSyncPoint(const char* action, uint64_t value)
{
    RecordEvent("syncPoint %s value=%llu", action, static_cast<unsigned long long>(value));
}

size_t GpuDumps::SerializeJournal(char* out, size_t cap) const
{
    if (!out || cap == 0)
        return 0;
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t off = 0;
    auto append = [&](const char* text) {
        while (*text && off < cap - 1)
            out[off++] = *text++;
    };

    char header[128];
    snprintf(header, sizeof header,
        "UnityRHI GPU-dump journal\ntotalRecords=%llu retained=%zu\n----\n",
        static_cast<unsigned long long>(m_nextLine),
        m_nextLine < kLineCount ? static_cast<size_t>(m_nextLine) : kLineCount);
    append(header);

    const uint64_t begin = m_nextLine > kLineCount ? m_nextLine - kLineCount : 0;
    for (uint64_t i = begin; i < m_nextLine && off < cap - 1; ++i)
    {
        append(m_lines[i % kLineCount]);
        if (off < cap - 1)
            out[off++] = '\n';
    }
    out[off] = '\0';
    return off;
}

// ---- device-error callbacks ----

UINT GpuDumps::BeginTrampoline(UINT64 flags)
{
    return Get().OnDumpBegin(flags);
}

void GpuDumps::EndTrampoline(const wchar_t* path)
{
    Get().OnDumpEnd(path);
}

UINT GpuDumps::OnDumpBegin(uint64_t flags)
{
    m_beginCalled.store(true, std::memory_order_release);
    DiagnosticTrace("BEGIN callback entered: flags=0x%llX", static_cast<unsigned long long>(flags));
    // Runs on the device-error path: no allocation (m_blobBuffer is preallocated).
    if (m_device && m_blobBuffer)
    {
        const size_t n = SerializeJournal(m_blobBuffer, kBlobCap);
        if (n > 0)
        {
            const HRESULT hr =
                m_device->AddBlobToDumpFile(m_blobBuffer, static_cast<UINT>(n), kJournalBlobTag);
            DiagnosticTrace("BEGIN callback AddBlobToDumpFile: bytes=%zu hr=0x%08X",
                n, static_cast<unsigned>(hr));
        }
    }
    DiagnosticTrace("BEGIN callback returning allow=1");
    return 1; // allow the dump to proceed
}

void GpuDumps::OnDumpEnd(const wchar_t* path)
{
    DiagnosticTrace("END callback entered: pathPresent=%d", path ? 1 : 0);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (path)
        {
            constexpr size_t cap = sizeof m_lastDumpPath / sizeof(wchar_t);
            size_t len = wcslen(path);
            if (len > cap - 1)
                len = cap - 1;
            std::memcpy(m_lastDumpPath, path, len * sizeof(wchar_t));
            m_lastDumpPath[len] = L'\0';
        }
    }
    m_endCalled.store(true, std::memory_order_release);
    DiagnosticTrace("END callback dump state committed");
    if (path)
        LogError("[UnityRHI] GPU DUMP WRITTEN: %ls", path);
    else
        LogError("[UnityRHI] GPU dump written (path unavailable).");
    DiagnosticTrace("END callback returning");
}
} // namespace unityrhi
