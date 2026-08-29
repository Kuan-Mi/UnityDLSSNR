#pragma once
#include <cstdint>
#include <NRIDescs.h>

#pragma pack(push, 1)

struct DLSRFrameData
{
    nri::Texture* inputTex;
    nri::Texture* outputTex;
    nri::Texture* mvTex;
    nri::Texture* depthTex;
    nri::Texture* exposureTex;   // optional, may be null
    nri::Texture* reactiveTex;   // optional, may be null

    uint16_t outputWidth;
    uint16_t outputHeight;
    uint16_t currentWidth;
    uint16_t currentHeight;
    float    cameraJitter[2];
    float    mvScale[2];

    int instanceId;
    nri::UpscalerMode upscalerMode;
    uint8_t  preset;        // 0 = default
    uint8_t  resetHistory;  // non-zero → RESET_HISTORY
};

#pragma pack(pop)
