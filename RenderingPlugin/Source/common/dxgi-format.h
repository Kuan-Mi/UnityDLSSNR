#pragma once

#include <dxgiformat.h>

#include "RhiTypes.h"

namespace unityrhi
{
// A mapping from unityrhi::Format to the DXGI formats used for resource
// creation and views. Ported from NVRHI (External/nvrhi/src/common/
// dxgi-format.cpp/.h), Copyright (c) 2014-2021 NVIDIA CORPORATION, MIT license.
struct DxgiFormatMapping
{
    Format abstractFormat;
    DXGI_FORMAT resourceFormat; // typeless where applicable
    DXGI_FORMAT srvFormat;
    DXGI_FORMAT rtvFormat;
};

const DxgiFormatMapping& getDxgiFormatMapping(Format format);
} // namespace unityrhi
