#include "DredDiagnostics.h"

#include <directx/d3d12.h>
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace unityrhi
{
namespace
{
class ReportFile
{
public:
    ReportFile()
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return;
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash)
            return;
        *slash = L'\0';
        if (wcslen(path) + wcslen(L"\\UnityRHI-Dred.log") + 1 > MAX_PATH)
            return;
        wcscat_s(path, L"\\UnityRHI-Dred.log");
        m_file = CreateFileW(path, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    ~ReportFile()
    {
        if (m_file != INVALID_HANDLE_VALUE)
        {
            FlushFileBuffers(m_file);
            CloseHandle(m_file);
        }
    }

    bool Valid() const { return m_file != INVALID_HANDLE_VALUE; }

    void Line(const char* format, ...)
    {
        if (!Valid())
            return;
        char line[2048];
        va_list args;
        va_start(args, format);
        const int result = vsnprintf(line, sizeof line - 3, format, args);
        va_end(args);
        if (result < 0)
            return;
        size_t length = strnlen(line, sizeof line - 3);
        line[length++] = '\r';
        line[length++] = '\n';
        DWORD written = 0;
        WriteFile(m_file, line, static_cast<DWORD>(length), &written, nullptr);
    }

private:
    HANDLE m_file = INVALID_HANDLE_VALUE;
};

const char* DeviceStateName(D3D12_DRED_DEVICE_STATE state)
{
    switch (state)
    {
    case D3D12_DRED_DEVICE_STATE_UNKNOWN: return "UNKNOWN";
    case D3D12_DRED_DEVICE_STATE_HUNG: return "HUNG";
    case D3D12_DRED_DEVICE_STATE_FAULT: return "FAULT";
    case D3D12_DRED_DEVICE_STATE_PAGEFAULT: return "PAGEFAULT";
    default: return "?";
    }
}

const char* OpName(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op)
    {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SETMARKER";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BEGINEVENT";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "ENDEVENT";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DRAWINSTANCED";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DRAWINDEXEDINSTANCED";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "EXECUTEINDIRECT";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "DISPATCH";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "COPYBUFFERREGION";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "COPYTEXTUREREGION";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "COPYRESOURCE";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "RESOLVESUBRESOURCE";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "CLEAR_RTV";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "CLEAR_UAV";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "CLEAR_DSV";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "RESOURCEBARRIER";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "PRESENT";
    case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "WRITEBUFFERIMMEDIATE";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BUILD_RAYTRACING_AS";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "COPY_RAYTRACING_AS";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DISPATCH_RAYS";
    case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "BARRIER";
    default: return "OTHER";
    }
}

const char* AllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type)
{
    switch (type)
    {
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "COMMAND_QUEUE";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "COMMAND_ALLOCATOR";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "PIPELINE_STATE";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "COMMAND_LIST";
    case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "FENCE";
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "DESCRIPTOR_HEAP";
    case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "HEAP";
    case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "QUERY_HEAP";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "COMMAND_SIGNATURE";
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "RESOURCE";
    case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT: return "STATE_OBJECT";
    case D3D12_DRED_ALLOCATION_TYPE_METACOMMAND: return "METACOMMAND";
    default: return "OTHER";
    }
}

void WriteBreadcrumbs(ReportFile& report, const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1& output)
{
    constexpr uint32_t kMaxNodes = 512;
    constexpr uint32_t kHistoryCapacity = 64 * 1024;
    uint32_t nodeIndex = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE1* node = output.pHeadAutoBreadcrumbNode;
         node && nodeIndex < kMaxNodes; node = node->pNext, ++nodeIndex)
    {
        const uint32_t completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
        const bool incomplete = node->pLastBreadcrumbValue && completed < node->BreadcrumbCount;
        report.Line("NODE[%u] %s list=%p queue=%p listNameA='%s' listNameW='%ls' "
                    "queueNameA='%s' queueNameW='%ls' completed=%u total=%u",
            nodeIndex, incomplete ? "INCOMPLETE" : "complete", static_cast<void*>(node->pCommandList),
            static_cast<void*>(node->pCommandQueue),
            node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "",
            node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"",
            node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "",
            node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"",
            completed, node->BreadcrumbCount);

        if (node->pCommandHistory && node->BreadcrumbCount > 0)
        {
            const uint32_t begin = completed > 12 ? completed - 12 : 0;
            const uint32_t end = std::min(node->BreadcrumbCount, completed + 13);
            for (uint32_t logicalIndex = begin; logicalIndex < end; ++logicalIndex)
            {
                const uint32_t historyIndex = logicalIndex % kHistoryCapacity;
                const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[historyIndex];
                report.Line("  OP[%u]%s %s(%u)", logicalIndex,
                    logicalIndex == completed ? " <-- NEXT/FAULT WINDOW" : "",
                    OpName(op), static_cast<unsigned>(op));
            }
        }

        if (node->pBreadcrumbContexts && node->BreadcrumbContextsCount)
        {
            uint32_t written = 0;
            for (uint32_t i = 0; i < node->BreadcrumbContextsCount && written < 64; ++i)
            {
                const D3D12_DRED_BREADCRUMB_CONTEXT& context = node->pBreadcrumbContexts[i];
                const uint32_t distance = context.BreadcrumbIndex > completed
                    ? context.BreadcrumbIndex - completed : completed - context.BreadcrumbIndex;
                if (distance <= 64)
                {
                    report.Line("  CONTEXT[%u] op=%u '%ls'", i, context.BreadcrumbIndex,
                        context.pContextString ? context.pContextString : L"");
                    ++written;
                }
            }
        }
    }
    report.Line("Breadcrumb nodes written: %u%s", nodeIndex,
        nodeIndex == kMaxNodes ? " (TRUNCATED)" : "");
}

void WriteAllocationList(ReportFile& report, const char* label,
    const D3D12_DRED_ALLOCATION_NODE1* head)
{
    constexpr uint32_t kMaxAllocations = 512;
    uint32_t count = 0;
    for (const D3D12_DRED_ALLOCATION_NODE1* node = head;
         node && count < kMaxAllocations; node = node->pNext, ++count)
    {
        report.Line("  %s[%u] type=%s(%u) object=%p nameA='%s' nameW='%ls'",
            label, count, AllocationTypeName(node->AllocationType),
            static_cast<unsigned>(node->AllocationType), static_cast<const void*>(node->pObject),
            node->ObjectNameA ? node->ObjectNameA : "", node->ObjectNameW ? node->ObjectNameW : L"");
    }
    report.Line("%s allocations written: %u%s", label, count,
        count == kMaxAllocations ? " (TRUNCATED)" : "");
}
} // namespace

void WriteDredReport(ID3D12Device* device, HRESULT removedReason)
{
    ReportFile report;
    if (!report.Valid())
        return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    report.Line("UnityRHI DRED report %04u-%02u-%02u %02u:%02u:%02u.%03u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    report.Line("ID3D12Device=%p GetDeviceRemovedReason=0x%08X", static_cast<void*>(device),
        static_cast<unsigned>(removedReason));
    if (!device)
    {
        report.Line("No device available.");
        return;
    }

    ID3D12DeviceRemovedExtendedData2* dred = nullptr;
    const HRESULT qiHr = device->QueryInterface(IID_PPV_ARGS(&dred));
    report.Line("QI ID3D12DeviceRemovedExtendedData2: hr=0x%08X interface=%p",
        static_cast<unsigned>(qiHr), static_cast<void*>(dred));
    if (FAILED(qiHr) || !dred)
        return;

    const D3D12_DRED_DEVICE_STATE state = dred->GetDeviceState();
    report.Line("DeviceState=%s(%u)", DeviceStateName(state), static_cast<unsigned>(state));

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    const HRESULT breadcrumbHr = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
    report.Line("GetAutoBreadcrumbsOutput1: hr=0x%08X head=%p",
        static_cast<unsigned>(breadcrumbHr), static_cast<const void*>(breadcrumbs.pHeadAutoBreadcrumbNode));
    if (SUCCEEDED(breadcrumbHr))
        WriteBreadcrumbs(report, breadcrumbs);

    D3D12_DRED_PAGE_FAULT_OUTPUT2 pageFault{};
    const HRESULT pageFaultHr = dred->GetPageFaultAllocationOutput2(&pageFault);
    report.Line("GetPageFaultAllocationOutput2: hr=0x%08X VA=0x%016llX flags=0x%X",
        static_cast<unsigned>(pageFaultHr), static_cast<unsigned long long>(pageFault.PageFaultVA),
        static_cast<unsigned>(pageFault.PageFaultFlags));
    if (SUCCEEDED(pageFaultHr))
    {
        WriteAllocationList(report, "EXISTING", pageFault.pHeadExistingAllocationNode);
        WriteAllocationList(report, "RECENT_FREED", pageFault.pHeadRecentFreedAllocationNode);
    }

    dred->Release();
    report.Line("END DRED report");
}
} // namespace unityrhi
