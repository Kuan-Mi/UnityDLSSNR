// Unity 2021.3.18f1 does not expose IUnityGraphicsD3D12v8, so the plugin cannot
// reach RequestResourceState / NotifyResourceState through the public plugin API.
// The underlying implementation, however, already exists in 2021.3.18: the v8
// entry point is a thin wrapper over D3D12CommandList::RequestResourceState,
// which carries an identical mangled signature in both 2021.3.18 and 6000.3.
//
// This header binds to that internal directly, keyed on the host module's
// CodeView PDB GUID so a mismatched build fails closed instead of corrupting
// resource state. RVAs and member offsets were extracted from the PDBs Unity
// ships with the editor (Editor/unity_x64.pdb) and with each player variation.
//
// Everything here is unsupported and valid only for 2021.3.18f1 exactly.
#pragma once

#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace unityrhi::unity2021
{
struct BuildBinding
{
    const char* label;
    GUID        pdbGuid;          // CodeView RSDS GUID of the host module
    uint32_t    requestResourceState;
    uint32_t    setResourceBarrier;
    uint32_t    markUAVResource;
    uint32_t    getCmdListAndMakeActive;
    uint32_t    gfxDeviceGlobal;  // RVA of the GfxDevice* that GetRealGfxDevice returns
    // GfxDeviceD3D12 -> current D3D12CommandList* (Unity's wrapper). Confirmed
    // at runtime: GetPluginCommandRecordingState reads this slot, and the
    // public v7 CommandRecordingState converts it via GetCmdListAndMakeActive
    // before handing the raw list to plugins.
    uint32_t    deviceCmdListSlot;
    uint32_t    listRawCmdList;   // D3D12CommandList -> ID3D12GraphicsCommandList*
};

// Generated from the shipped PDBs; see scratchpad/gentable.py.
inline constexpr BuildBinding kBuilds[] = {
    {"2021.3.18f1 editor (Unity.exe)",
     {0x50E22447, 0x3848, 0x46D5, {0xA7, 0xBE, 0x98, 0xB3, 0x72, 0xF8, 0xF0, 0x69}},
     0x010CC670, 0x010CE500, 0x010CC220, 0x010C8EE0, 0x04102E00, 0x3B50, 0x10},
    {"2021.3.18f1 player dev il2cpp",
     {0x64C8CF26, 0xF048, 0x4E74, {0x81, 0xB0, 0x82, 0xE7, 0xCD, 0x06, 0x8E, 0xB5}},
     0x0103E960, 0x01040680, 0x0103E5E0, 0x0103B940, 0x02D5D588, 0x3B50, 0x10},
    {"2021.3.18f1 player dev mono",
     {0x43A87C43, 0x2A3F, 0x4CE5, {0x9D, 0xA2, 0x8F, 0x38, 0x8D, 0xCC, 0xF4, 0x7D}},
     0x01046EB0, 0x01048BD0, 0x01046B30, 0x01043EA0, 0x02D8A290, 0x3B50, 0x10},
    {"2021.3.18f1 player il2cpp",
     {0xC3067F92, 0x0A9B, 0x4E68, {0xB4, 0x83, 0x22, 0x9E, 0x9E, 0xD5, 0x1C, 0x50}},
     0x00783B40, 0x007856B0, 0x00783740, 0x00780FB0, 0x01B0C1A0, 0x3AF8, 0x10},
    {"2021.3.18f1 player mono",
     {0x31071DF7, 0xC3F5, 0x4B5E, {0x8A, 0x4E, 0xC3, 0xF5, 0xE3, 0x42, 0xFB, 0xAB}},
     0x00784850, 0x007863C0, 0x00784450, 0x00781CC0, 0x01B0F030, 0x3AF8, 0x10},
};

using RequestResourceStateFn = bool (*)(void* self, ID3D12Resource*, D3D12_RESOURCE_STATES);
using SetResourceBarrierFn = bool (*)(void* self, ID3D12Resource*, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after, UINT subresource);
using MarkUAVResourceFn = void (*)(void* self, ID3D12Resource*);

class Bridge
{
public:
    // Resolve the host module and match it against the table. Safe to call more
    // than once; only the first call does work.
    static bool Initialize();
    static bool Available() { return s_binding != nullptr; }
    static const char* BuildLabel() { return s_binding ? s_binding->label : "<unbound>"; }

    // Bind the D3D12CommandList wrapper for the duration of one plugin event.
    // Unity's command list cannot change inside a single event callback, so the
    // pointer walk - and the VirtualQuery validation that guards it - runs once
    // here instead of once per resource-state request. Returns false when the
    // wrapper cannot be resolved; the failure is cached too, so a build whose
    // layout no longer matches degrades to a no-op rather than re-running the
    // calibration scan for every binding in the frame.
    static bool BeginEvent(ID3D12GraphicsCommandList* unityCommandList);
    static void EndEvent();

    // Mirrors IUnityGraphicsD3D12v8::RequestResourceState. `unityCommandList` is
    // the ID3D12GraphicsCommandList* handed back by CommandRecordingState; it is
    // used to locate and validate Unity's D3D12CommandList wrapper.
    static bool RequestResourceState(
        ID3D12GraphicsCommandList* unityCommandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES state);

    // The closest 2021.3 analogue of the v8 NotifyResourceState UAV flag.
    static bool MarkUAVResource(ID3D12GraphicsCommandList* unityCommandList, ID3D12Resource* resource);

    // Intermediate values behind RequestResourceState, so a caller can verify
    // each hop (global -> GfxDeviceD3D12 -> D3D12CommandList) instead of only
    // seeing a pass/fail at the end.
    struct Diagnostics
    {
        const char* buildLabel;
        void*       gfxDevice;
        void*       deviceCmdListSlot;  // raw contents of GfxDeviceD3D12 + deviceCmdListSlot
        void*       wrapper;            // D3D12CommandList*
        void*       wrapperRawCmdList;  // wrapper + listRawCmdList (must equal the passed list)
        uint32_t    wrapperState;       // 0 fresh / 1 active / 2 closed
        uint32_t    wrapperOffset;
        bool        wrapperOffsetKnown;
    };
    static Diagnostics Describe(ID3D12GraphicsCommandList* unityCommandList);

private:
    // Walks the GfxDeviceD3D12 instance looking for the D3D12CommandList whose
    // raw-list member equals `raw`. The hit offset is cached, then revalidated
    // on every call, so a layout change degrades to "unavailable" rather than a
    // wild write.
    static void* FindCommandListWrapper(ID3D12GraphicsCommandList* raw);
    static bool ReadableQword(const void* p);

    // Hot path: the wrapper bound by BeginEvent, or a one-shot resolve when a
    // caller reaches us outside an event. Never runs the calibration scan more
    // than once per (event, raw list) pair.
    static void* EventWrapper(ID3D12GraphicsCommandList* raw);

    static inline const BuildBinding* s_binding = nullptr;
    static inline uint8_t* s_base = nullptr;
    static inline RequestResourceStateFn s_request = nullptr;
    static inline MarkUAVResourceFn s_markUav = nullptr;
    static inline uint32_t s_wrapperOffset = 0;   // byte offset into GfxDeviceD3D12
    static inline bool s_wrapperOffsetKnown = false;
    // Resolved once per plugin event. s_eventRaw non-null with a null
    // s_eventWrapper is a cached failure, not "not yet resolved".
    static inline ID3D12GraphicsCommandList* s_eventRaw = nullptr;
    static inline void* s_eventWrapper = nullptr;
};

inline bool Bridge::ReadableQword(const void* p)
{
    if (!p || (reinterpret_cast<uintptr_t>(p) & 7u) != 0)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readable) == 0 || (mbi.Protect & PAGE_GUARD) != 0)
        return false;
    // Ensure the whole qword sits inside the same committed region.
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(p) + sizeof(void*) <= end;
}

inline bool Bridge::Initialize()
{
    if (s_binding)
        return true;

    // Player builds host the graphics backend in UnityPlayer.dll; the editor
    // links it straight into Unity.exe.
    HMODULE host = GetModuleHandleW(L"UnityPlayer.dll");
    if (!host)
        host = GetModuleHandleW(nullptr);
    if (!host)
        return false;

    auto* base = reinterpret_cast<uint8_t*>(host);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress == 0 || dir.Size == 0)
        return false;

    struct CvInfoRsds
    {
        DWORD signature;  // 'RSDS'
        GUID  guid;
        DWORD age;
        char  pdbPath[1];
    };

    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    const size_t count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (size_t i = 0; i < count; ++i)
    {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || entries[i].AddressOfRawData == 0)
            continue;
        const auto* cv = reinterpret_cast<const CvInfoRsds*>(base + entries[i].AddressOfRawData);
        if (cv->signature != 0x53445352u)  // 'RSDS'
            continue;

        for (const BuildBinding& b : kBuilds)
        {
            if (std::memcmp(&b.pdbGuid, &cv->guid, sizeof(GUID)) != 0)
                continue;
            s_base = base;
            s_binding = &b;
            s_request = reinterpret_cast<RequestResourceStateFn>(base + b.requestResourceState);
            s_markUav = reinterpret_cast<MarkUAVResourceFn>(base + b.markUAVResource);
            return true;
        }
        break;  // CodeView entry found but unrecognised build: fail closed.
    }
    return false;
}

inline void* Bridge::FindCommandListWrapper(ID3D12GraphicsCommandList* raw)
{
    if (!s_binding || !raw)
        return nullptr;

    auto* devSlot = reinterpret_cast<uint8_t**>(s_base + s_binding->gfxDeviceGlobal);
    if (!ReadableQword(devSlot))
        return nullptr;
    uint8_t* device = *devSlot;
    if (!device)
        return nullptr;

    // Cheap path: the offset found last time still points at this raw list.
    auto probe = [&](uint32_t off) -> void* {
        auto** slot = reinterpret_cast<void**>(device + off);
        if (!ReadableQword(slot))
            return nullptr;
        auto* wrapper = static_cast<uint8_t*>(*slot);
        if (!wrapper)
            return nullptr;
        auto** rawSlot = reinterpret_cast<void**>(wrapper + s_binding->listRawCmdList);
        if (!ReadableQword(rawSlot))
            return nullptr;
        return (*rawSlot == raw) ? wrapper : nullptr;
    };

    if (s_wrapperOffsetKnown)
    {
        if (void* hit = probe(s_wrapperOffset))
            return hit;
        s_wrapperOffsetKnown = false;  // layout moved; fall through and re-scan
    }

    // The slot GetPluginCommandRecordingState reads holds the wrapper directly,
    // so try it before scanning.
    if (void* hit = probe(s_binding->deviceCmdListSlot))
    {
        s_wrapperOffset = s_binding->deviceCmdListSlot;
        s_wrapperOffsetKnown = true;
        return hit;
    }

    // Fallback calibration scan, in case a build stores it elsewhere.
    constexpr uint32_t kWindow = 0x400;
    const uint32_t centre = s_binding->deviceCmdListSlot;
    const uint32_t begin = centre > kWindow ? centre - kWindow : 0;
    for (uint32_t off = begin; off <= centre + kWindow; off += sizeof(void*))
    {
        if (void* hit = probe(off))
        {
            s_wrapperOffset = off;
            s_wrapperOffsetKnown = true;
            return hit;
        }
    }
    return nullptr;
}

inline Bridge::Diagnostics Bridge::Describe(ID3D12GraphicsCommandList* unityCommandList)
{
    Diagnostics d{};
    d.buildLabel = BuildLabel();
    if (!s_binding)
        return d;

    auto* devSlot = reinterpret_cast<uint8_t**>(s_base + s_binding->gfxDeviceGlobal);
    if (ReadableQword(devSlot))
        d.gfxDevice = *devSlot;
    if (d.gfxDevice)
    {
        auto** slot = reinterpret_cast<void**>(
            static_cast<uint8_t*>(d.gfxDevice) + s_binding->deviceCmdListSlot);
        if (ReadableQword(slot))
            d.deviceCmdListSlot = *slot;
    }

    d.wrapper = FindCommandListWrapper(unityCommandList);
    if (d.wrapper)
    {
        auto* w = static_cast<uint8_t*>(d.wrapper);
        d.wrapperRawCmdList = *reinterpret_cast<void**>(w + s_binding->listRawCmdList);
        d.wrapperState = *reinterpret_cast<uint32_t*>(w + 0x18);
    }
    d.wrapperOffset = s_wrapperOffset;
    d.wrapperOffsetKnown = s_wrapperOffsetKnown;
    return d;
}

inline bool Bridge::BeginEvent(ID3D12GraphicsCommandList* unityCommandList)
{
    s_eventRaw = unityCommandList;
    s_eventWrapper = unityCommandList ? FindCommandListWrapper(unityCommandList) : nullptr;
    return s_eventWrapper != nullptr;
}

inline void Bridge::EndEvent()
{
    s_eventRaw = nullptr;
    s_eventWrapper = nullptr;
}

inline void* Bridge::EventWrapper(ID3D12GraphicsCommandList* raw)
{
    if (raw && raw == s_eventRaw)
        return s_eventWrapper;  // may be null: cached resolve failure
    // Outside an event (native tests, external-heap dispatches): resolve and
    // cache under the same rule so a repeated caller pays the walk only once.
    return BeginEvent(raw) ? s_eventWrapper : nullptr;
}

inline bool Bridge::RequestResourceState(
    ID3D12GraphicsCommandList* unityCommandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
{
    if (!s_request || !resource)
        return false;
    void* wrapper = EventWrapper(unityCommandList);
    if (!wrapper)
        return false;
    return s_request(wrapper, resource, state);
}

inline bool Bridge::MarkUAVResource(ID3D12GraphicsCommandList* unityCommandList, ID3D12Resource* resource)
{
    if (!s_markUav || !resource)
        return false;
    void* wrapper = EventWrapper(unityCommandList);
    if (!wrapper)
        return false;
    s_markUav(wrapper, resource);
    return true;
}
} // namespace unityrhi::unity2021
