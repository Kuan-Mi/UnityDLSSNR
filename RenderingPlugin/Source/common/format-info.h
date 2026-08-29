#pragma once

#include "RhiTypes.h"

namespace unityrhi
{
// API-neutral format information. Ported from NVRHI (struct FormatInfo in
// include/nvrhi/nvrhi.h + src/common/format-info.cpp), Copyright (c) 2021
// NVIDIA CORPORATION, MIT license. NVRHI declares these in its public header;
// we keep them in a separate file because RhiTypes.h is the ABI contract.

enum class FormatKind : uint8_t
{
    Integer,
    Normalized,
    Float,
    DepthStencil
};

struct FormatInfo
{
    Format format;
    const char* name;
    uint8_t bytesPerBlock;
    uint8_t blockSize;
    FormatKind kind;
    bool hasRed : 1;
    bool hasGreen : 1;
    bool hasBlue : 1;
    bool hasAlpha : 1;
    bool hasDepth : 1;
    bool hasStencil : 1;
    bool isSigned : 1;
    bool isSRGB : 1;
};

const FormatInfo& getFormatInfo(Format format);
} // namespace unityrhi
