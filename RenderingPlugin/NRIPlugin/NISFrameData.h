#pragma once
#include <cstdint>
#include <NRIDescs.h>

#pragma pack(push, 1)

struct NISFrameData
{
    nri::Texture* inputTex;
    nri::Texture* outputTex;

    uint16_t outputWidth;
    uint16_t outputHeight;
    uint16_t currentWidth;
    uint16_t currentHeight;
    float    sharpness;    // [0, 1]; 0 = no sharpening, 1 = maximum

    int instanceId;
};

#pragma pack(pop)
