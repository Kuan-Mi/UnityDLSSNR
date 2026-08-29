#pragma once
#include <cstdint>
#include <d3d12.h>
#include <NRD.h>
#include <NRDSettings.h>
#include <NRIDescs.h>

namespace nri
{
    struct Texture;
}

// Maximum number of NRD denoisers that can be created per NrdInstance.
// Keep in sync with C# side (NrdFrameData.cs).
constexpr uint32_t kMaxDenoisersPerInstance = 4;

// Maximum size (in bytes) of any single NRD denoiser settings struct.
// Keep in sync with C# side (NrdFrameData.cs).
constexpr uint32_t kMaxDenoiserSettingsSize = 256;

static_assert(sizeof(nrd::SigmaSettings)     <= kMaxDenoiserSettingsSize, "Bump kMaxDenoiserSettingsSize");
static_assert(sizeof(nrd::ReblurSettings)    <= kMaxDenoiserSettingsSize, "Bump kMaxDenoiserSettingsSize");
static_assert(sizeof(nrd::RelaxSettings)     <= kMaxDenoiserSettingsSize, "Bump kMaxDenoiserSettingsSize");
static_assert(sizeof(nrd::ReferenceSettings) <= kMaxDenoiserSettingsSize, "Bump kMaxDenoiserSettingsSize");

#pragma pack(push, 1)

// Creation-time descriptor: bound to an identifier at NrdInstance construction.
struct NrdDenoiserDesc
{
    uint32_t identifier;
    uint32_t denoiser; // nrd::Denoiser
};

// Per-frame settings for a single denoiser (tag + opaque blob).
struct DenoiserSettingsEntry
{
    uint32_t identifier;
    uint32_t denoiser; // nrd::Denoiser tag; tells native how to interpret the blob
    uint8_t  settings[kMaxDenoiserSettingsSize];
};

struct FrameData
{
    nrd::CommonSettings commonSettings;

    uint32_t              denoiserCount;
    DenoiserSettingsEntry entries[kMaxDenoisersPerInstance];

    uint16_t width;
    uint16_t height;

    int instanceId;
};

struct NriResourceState
{
    nri::AccessBits accessBits;
    uint32_t layout;
    nri::StageBits stageBits;
};

struct NrdResourceInput
{
    nrd::ResourceType type;
    nri::Texture* texture;
    NriResourceState state;
};
#pragma pack(pop)
