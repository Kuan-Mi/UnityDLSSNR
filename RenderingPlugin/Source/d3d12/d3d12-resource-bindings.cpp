// Binding layouts, binding sets, and root-signature construction, ported from
// NVRHI's d3d12-resource-bindings.cpp (External/nvrhi/src/d3d12).
// Copyright (c) 2014-2021, NVIDIA CORPORATION. MIT license
// (see ThirdParty/NVRHI-LICENSE.txt).
//
// Layouts precompute their root-parameter segment: one CBV/SRV/UAV descriptor
// table and one sampler table per layout (with merged ranges), root descriptors
// for volatile CBs, root constants for push constants. Binding sets stage
// their descriptors into the CPU heaps at creation and copy them to the
// shader-visible mirrors; binding = SetComputeRootDescriptorTable at replay.

#include "d3d12-backend.h"

#include <cassert>

#include "UnityRhiLog.h"

namespace unityrhi
{
uint32_t arrayDifferenceMask(const std::vector<RhiResource*>& a, const std::vector<RhiResource*>& b)
{
    const uint32_t count = uint32_t(std::max(a.size(), b.size()));
    uint32_t mask = 0;
    for (uint32_t i = 0; i < count && i < 32; ++i)
    {
        RhiResource* av = i < a.size() ? a[i] : nullptr;
        RhiResource* bv = i < b.size() ? b[i] : nullptr;
        if (av != bv)
            mask |= 1u << i;
    }
    return mask;
}
static ResourceType GetNormalizedResourceType(ResourceType type)
{
    switch (type)
    {
    case ResourceType::StructuredBuffer_UAV:
    case ResourceType::RawBuffer_UAV:
        return ResourceType::TypedBuffer_UAV;
    case ResourceType::StructuredBuffer_SRV:
    case ResourceType::RawBuffer_SRV:
        return ResourceType::TypedBuffer_SRV;
    default:
        return type;
    }
}

static bool AreResourceTypesCompatible(ResourceType a, ResourceType b)
{
    if (a == b)
        return true;

    a = GetNormalizedResourceType(a);
    b = GetNormalizedResourceType(b);

    if ((a == ResourceType::TypedBuffer_SRV && b == ResourceType::Texture_SRV) ||
        (b == ResourceType::TypedBuffer_SRV && a == ResourceType::Texture_SRV) ||
        (a == ResourceType::TypedBuffer_SRV && b == ResourceType::RayTracingAccelStruct) ||
        (a == ResourceType::Texture_SRV && b == ResourceType::RayTracingAccelStruct) ||
        (b == ResourceType::TypedBuffer_SRV && a == ResourceType::RayTracingAccelStruct) ||
        (b == ResourceType::Texture_SRV && a == ResourceType::RayTracingAccelStruct))
        return true;

    if ((a == ResourceType::TypedBuffer_UAV && b == ResourceType::Texture_UAV) ||
        (b == ResourceType::TypedBuffer_UAV && a == ResourceType::Texture_UAV))
        return true;

    return false;
}

bool BindingSet::createDescriptors()
{
    // Process the volatile constant buffers: they occupy one root parameter each
    for (const std::pair<RootParameterIndex, D3D12_ROOT_DESCRIPTOR1>& parameter : layout->rootParametersVolatileCB)
    {
        Buffer* foundBuffer = nullptr;

        RootParameterIndex rootParameterIndex = parameter.first;
        const D3D12_ROOT_DESCRIPTOR1& rootDescriptor = parameter.second;

        for (const RhiBindingSetItem& binding : bindings)
        {
            if (binding.type == ResourceType::VolatileConstantBuffer &&
                binding.slot == rootDescriptor.ShaderRegister)
            {
                auto* bindingResource = static_cast<RhiResource*>(binding.resource);
                if (!bindingResource || bindingResource->kind != Kind::Buffer)
                {
                    m_Context.error("BindingSet: volatile constant buffer binding at slot b" +
                        std::to_string(binding.slot) + " is not a buffer.");
                    return false;
                }
                foundBuffer = static_cast<Buffer*>(bindingResource);
                break;
            }
        }

        // Add an entry to the binding set's array, whether we found the buffer in the binding set or not.
        // Even if not found, the command list still has to bind something to the root parameter.
        rootParametersVolatileCB.push_back(std::make_pair(rootParameterIndex, foundBuffer));
    }

    if (layout->descriptorTableSizeSamplers > 0)
    {
        DescriptorIndex descriptorTableBaseIndex =
            m_Resources.samplerHeap.allocateDescriptors(layout->descriptorTableSizeSamplers);
        if (descriptorTableBaseIndex == c_InvalidDescriptorIndex)
            return false;
        descriptorTableSamplers = descriptorTableBaseIndex;
        descriptorTableSizeSamplers = layout->descriptorTableSizeSamplers;
        rootParameterIndexSamplers = layout->rootParameterSamplers;
        descriptorTableValidSamplers = true;

        for (const auto& range : layout->descriptorRangesSamplers)
        {
            for (uint32_t itemInRange = 0; itemInRange < range.NumDescriptors; itemInRange++)
            {
                uint32_t slot = range.BaseShaderRegister + itemInRange;
                bool found = false;
                D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle = m_Resources.samplerHeap.getCpuHandle(
                    descriptorTableBaseIndex + range.OffsetInDescriptorsFromTableStart + itemInRange);

                for (const RhiBindingSetItem& binding : bindings)
                {
                    if (binding.type == ResourceType::Sampler &&
                        binding.slot + binding.arrayElement == slot)
                    {
                        auto* bindingResource = static_cast<RhiResource*>(binding.resource);
                        if (!bindingResource || bindingResource->kind != Kind::Sampler)
                        {
                            m_Context.error("BindingSet: sampler binding at slot s" +
                                std::to_string(binding.slot) + " is not a sampler.");
                            return false;
                        }
                        static_cast<Sampler*>(bindingResource)->createDescriptor(descriptorHandle.ptr);
                        found = true;
                        break;
                    }
                }

                if (!found)
                {
                    // Create a default sampler
                    D3D12_SAMPLER_DESC samplerDesc = {};
                    m_Context.device->CreateSampler(&samplerDesc, descriptorHandle);
                }
            }
        }

        m_Resources.samplerHeap.copyToShaderVisibleHeap(descriptorTableBaseIndex, layout->descriptorTableSizeSamplers);
    }

    if (layout->descriptorTableSizeSRVetc > 0)
    {
        DescriptorIndex descriptorTableBaseIndex =
            m_Resources.shaderResourceViewHeap.allocateDescriptors(layout->descriptorTableSizeSRVetc);
        if (descriptorTableBaseIndex == c_InvalidDescriptorIndex)
            return false;
        descriptorTableSRVetc = descriptorTableBaseIndex;
        descriptorTableSizeSRVetc = layout->descriptorTableSizeSRVetc;
        rootParameterIndexSRVetc = layout->rootParameterSRVetc;
        descriptorTableValidSRVetc = true;

        for (const auto& range : layout->descriptorRangesSRVetc)
        {
            for (uint32_t itemInRange = 0; itemInRange < range.NumDescriptors; itemInRange++)
            {
                uint32_t slot = range.BaseShaderRegister + itemInRange;
                bool found = false;
                D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle = m_Resources.shaderResourceViewHeap.getCpuHandle(
                    descriptorTableBaseIndex + range.OffsetInDescriptorsFromTableStart + itemInRange);

                for (size_t bindingIndex = 0; bindingIndex < bindings.size(); bindingIndex++)
                {
                    const RhiBindingSetItem& binding = bindings[bindingIndex];

                    if (binding.slot + binding.arrayElement != slot)
                        continue;

                    const auto bindingType = GetNormalizedResourceType(binding.type);
                    auto* bindingResource = static_cast<RhiResource*>(binding.resource);

                    if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
                        bindingType == ResourceType::TypedBuffer_SRV)
                    {
                        if (bindingResource)
                        {
                            if (bindingResource->kind != Kind::Buffer)
                                break;
                            auto* buffer = static_cast<Buffer*>(bindingResource);
                            buffer->createSRV(descriptorHandle.ptr, binding.format, binding.range(), binding.type);
                            bindingsThatNeedTransitions.push_back(uint16_t(bindingIndex));
                        }
                        else
                        {
                            Buffer::createNullSRV(descriptorHandle.ptr, binding.format, m_Context);
                        }

                        found = true;
                        break;
                    }
                    else if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
                        bindingType == ResourceType::TypedBuffer_UAV)
                    {
                        if (bindingResource)
                        {
                            if (bindingResource->kind != Kind::Buffer)
                                break;
                            auto* buffer = static_cast<Buffer*>(bindingResource);
                            buffer->createUAV(descriptorHandle.ptr, binding.format, binding.range(), binding.type);
                            bindingsThatNeedTransitions.push_back(uint16_t(bindingIndex));
                        }
                        else
                        {
                            Buffer::createNullUAV(descriptorHandle.ptr, binding.format, m_Context);
                        }

                        hasUavBindings = true;
                        found = true;
                        break;
                    }
                    else if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
                        bindingType == ResourceType::Texture_SRV)
                    {
                        if (!bindingResource || bindingResource->kind != Kind::Texture)
                            break;
                        auto* texture = static_cast<Texture*>(bindingResource);
                        texture->createSRV(descriptorHandle.ptr, binding.format, binding.dimension, binding.subresources());
                        bindingsThatNeedTransitions.push_back(uint16_t(bindingIndex));

                        found = true;
                        break;
                    }
                    else if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
                        bindingType == ResourceType::Texture_UAV)
                    {
                        if (!bindingResource || bindingResource->kind != Kind::Texture)
                            break;
                        auto* texture = static_cast<Texture*>(bindingResource);
                        texture->createUAV(descriptorHandle.ptr, binding.format, binding.dimension, binding.subresources());
                        bindingsThatNeedTransitions.push_back(uint16_t(bindingIndex));

                        hasUavBindings = true;
                        found = true;
                        break;
                    }
                    else if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV &&
                        bindingType == ResourceType::ConstantBuffer)
                    {
                        if (!bindingResource || bindingResource->kind != Kind::Buffer)
                            break;
                        auto* buffer = static_cast<Buffer*>(bindingResource);

                        if (buffer->desc.isVolatile)
                        {
                            m_Context.error("Attempted to bind a volatile constant buffer '" +
                                buffer->debugName + "' to a non-volatile CB layout at slot b" +
                                std::to_string(binding.slot));
                            return false;
                        }

                        buffer->createCBV(descriptorHandle.ptr, binding.range());
                        bindingsThatNeedTransitions.push_back(uint16_t(bindingIndex));

                        found = true;
                        break;
                    }
                    else if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
                        binding.type == ResourceType::RayTracingAccelStruct)
                    {
                        if (!bindingResource || bindingResource->kind != Kind::AccelStruct)
                            break;
                        auto* accelStruct = static_cast<AccelStruct*>(bindingResource);
                        accelStruct->createSRV(descriptorHandle.ptr);
                        bindingsThatNeedTransitions.push_back(uint16_t(bindingIndex));

                        found = true;
                        break;
                    }
                    // SamplerFeedbackTexture_UAV bindings are not ported.
                }

                if (!found)
                {
                    // Create a null SRV, UAV, or CBV

                    switch (range.RangeType)
                    {
                    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
                        Buffer::createNullSRV(descriptorHandle.ptr, Format::UNKNOWN, m_Context);
                        break;

                    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
                        Buffer::createNullUAV(descriptorHandle.ptr, Format::UNKNOWN, m_Context);
                        break;

                    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
                        m_Context.device->CreateConstantBufferView(nullptr, descriptorHandle);
                        break;

                    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
                    default:
                        break;
                    }
                }
            }
        }

        m_Resources.shaderResourceViewHeap.copyToShaderVisibleHeap(descriptorTableBaseIndex, layout->descriptorTableSizeSRVetc);
    }

    return true;
}

BindingLayout* Device::createBindingLayout(const RhiBindingLayoutDesc& desc,
    const RhiBindingLayoutItem* items, uint32_t itemCount, const char* debugName)
{
    auto* layout = new BindingLayout(desc, items, itemCount);
    layout->debugName = debugName ? debugName : "";
    layout->uniqueId = m_nextBindingLayoutId++;
    RegisterResource(layout);
    return layout;
}

// Port of nvrhi Device::createBindlessLayout.
BindingLayout* Device::createBindlessLayout(const RhiBindlessLayoutDesc& desc,
    const RhiBindingLayoutItem* registerSpaces, uint32_t registerSpaceCount, const char* debugName)
{
    const RhiBindlessLayoutType layoutType = RhiBindlessLayoutType(desc.layoutType);

    if (desc.visibility == ShaderType::None)
    {
        LogError("[UnityRHI] createBindlessLayout '%s': visibility must not be None.",
            debugName ? debugName : "");
        return nullptr;
    }

    if (desc.maxCapacity == 0)
    {
        LogError("[UnityRHI] createBindlessLayout '%s': maxCapacity must be non-zero.",
            debugName ? debugName : "");
        return nullptr;
    }

    if (layoutType == RhiBindlessLayoutType::Immutable)
    {
        if (!registerSpaces || registerSpaceCount == 0)
        {
            LogError("[UnityRHI] createBindlessLayout '%s': immutable layouts require at least one register space.",
                debugName ? debugName : "");
            return nullptr;
        }
    }
    else
    {
        if (registerSpaceCount != 0)
        {
            LogError("[UnityRHI] createBindlessLayout '%s': mutable layouts must not define register spaces.",
                debugName ? debugName : "");
            return nullptr;
        }
    }

    auto* layout = new BindingLayout(desc, registerSpaces, registerSpaceCount);
    layout->debugName = debugName ? debugName : "";
    layout->uniqueId = m_nextBindingLayoutId++;
    RegisterResource(layout);
    return layout;
}

BindingSet* Device::createBindingSet(
    BindingLayout* layout, const RhiBindingSetItem* items, uint32_t itemCount, const char* debugName)
{
    if (!layout || !items || itemCount == 0)
    {
        LogError("[UnityRHI] createBindingSet '%s': layout and items are required.",
            debugName ? debugName : "");
        return nullptr;
    }

    if (layout->isBindless)
    {
        LogError("[UnityRHI] createBindingSet '%s': bindless layouts take descriptor tables, "
                 "not binding sets (use createDescriptorTable).",
            debugName ? debugName : "");
        return nullptr;
    }

    auto* set = new BindingSet(m_Context, m_Resources);
    set->debugName = debugName ? debugName : "";
    set->layout = layout;
    set->bindings.assign(items, items + itemCount);

    if (!set->createDescriptors())
    {
        LogError("[UnityRHI] createBindingSet '%s' failed.", set->debugName.c_str());
        delete set;
        return nullptr;
    }

    RegisterResource(set);
    return set;
}

// Port of nvrhi Device::createDescriptorTable.
DescriptorTable* Device::createDescriptorTable(BindingLayout* layout, const char* debugName)
{
    if (!layout || !layout->isBindless)
    {
        LogError("[UnityRHI] createDescriptorTable '%s': a bindless layout is required.",
            debugName ? debugName : "");
        return nullptr;
    }

    auto* ret = new DescriptorTable(m_Resources);
    ret->debugName = debugName ? debugName : "";
    ret->capacity = 0;
    ret->firstDescriptor = 0;
    ret->layout = layout;

    RegisterResource(ret);
    return ret;
}

BindingSet::~BindingSet()
{
    if (descriptorTableValidSRVetc)
        m_Resources.shaderResourceViewHeap.releaseDescriptors(descriptorTableSRVetc, descriptorTableSizeSRVetc);

    if (descriptorTableValidSamplers)
        m_Resources.samplerHeap.releaseDescriptors(descriptorTableSamplers, descriptorTableSizeSamplers);
}

bool DescriptorTable::isSamplerTable() const
{
    return layout && RhiBindlessLayoutType(layout->bindlessDesc.layoutType) ==
        RhiBindlessLayoutType::MutableSampler;
}

StaticDescriptorHeap& DescriptorTable::getDescriptorHeap() const
{
    return isSamplerTable() ? m_Resources.samplerHeap : m_Resources.shaderResourceViewHeap;
}

DescriptorTable::~DescriptorTable()
{
    getDescriptorHeap().releaseDescriptors(firstDescriptor, capacity);
}

BindingLayout::BindingLayout(const RhiBindingLayoutDesc& _desc,
    const RhiBindingLayoutItem* items, uint32_t itemCount)
    : RhiResource(Kind::BindingLayout)
    , desc(_desc)
{
    if (items && itemCount > 0)
        bindings.assign(items, items + itemCount);

    // Start with some invalid values, to make sure that we start a new range on the first binding
    ResourceType currentType = ResourceType(~0u);
    uint32_t currentSlot = ~0u;

    D3D12_ROOT_CONSTANTS rootConstants = {};

    for (const RhiBindingLayoutItem& binding : bindings)
    {
        if (binding.type == ResourceType::VolatileConstantBuffer)
        {
            D3D12_ROOT_DESCRIPTOR1 rootDescriptor;
            rootDescriptor.ShaderRegister = binding.slot;
            rootDescriptor.RegisterSpace = desc.registerSpace;

            // Volatile CBs are static descriptors, however strange that may seem.
            // A volatile CB can only be bound to a command list after it's been written into, and
            // after that the data will not change until the command list has finished executing.
            // Subsequent writes will be made into a newly allocated portion of an upload buffer.
            rootDescriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

            rootParametersVolatileCB.push_back(std::make_pair(c_InvalidRootParameterIndex, rootDescriptor));
        }
        else if (binding.type == ResourceType::PushConstants)
        {
            pushConstantByteSize = binding.size;
            rootConstants.ShaderRegister = binding.slot;
            rootConstants.RegisterSpace = desc.registerSpace;
            rootConstants.Num32BitValues = binding.size / 4;
        }
        else if (!AreResourceTypesCompatible(binding.type, currentType) || binding.slot != currentSlot + 1)
        {
            // Start a new range

            if (binding.type == ResourceType::Sampler)
            {
                descriptorRangesSamplers.resize(descriptorRangesSamplers.size() + 1);
                D3D12_DESCRIPTOR_RANGE1& range = descriptorRangesSamplers[descriptorRangesSamplers.size() - 1];

                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                range.NumDescriptors = binding.size;
                range.BaseShaderRegister = binding.slot;
                range.RegisterSpace = desc.registerSpace;
                range.OffsetInDescriptorsFromTableStart = descriptorTableSizeSamplers;
                range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

                descriptorTableSizeSamplers += binding.size;
            }
            else
            {
                descriptorRangesSRVetc.resize(descriptorRangesSRVetc.size() + 1);
                D3D12_DESCRIPTOR_RANGE1& range = descriptorRangesSRVetc[descriptorRangesSRVetc.size() - 1];

                switch (binding.type)
                {
                case ResourceType::Texture_SRV:
                case ResourceType::TypedBuffer_SRV:
                case ResourceType::StructuredBuffer_SRV:
                case ResourceType::RawBuffer_SRV:
                case ResourceType::RayTracingAccelStruct:
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                    break;

                case ResourceType::Texture_UAV:
                case ResourceType::TypedBuffer_UAV:
                case ResourceType::StructuredBuffer_UAV:
                case ResourceType::RawBuffer_UAV:
                case ResourceType::SamplerFeedbackTexture_UAV:
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                    break;

                case ResourceType::ConstantBuffer:
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                    break;

                default:
                    LogError("[UnityRHI] BindingLayout: invalid resource type %u.", uint32_t(binding.type));
                    continue;
                }
                range.NumDescriptors = binding.size;
                range.BaseShaderRegister = binding.slot;
                range.RegisterSpace = desc.registerSpace;
                range.OffsetInDescriptorsFromTableStart = descriptorTableSizeSRVetc;

                // We don't know how apps will use resources referenced in a binding set. They may bind
                // a buffer to the command list and then copy data into it.
                range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

                descriptorTableSizeSRVetc += binding.size;

                bindingLayoutsSRVetc.push_back(binding);
            }

            currentType = binding.type;
            currentSlot = binding.slot;
        }
        else
        {
            // Extend the current range

            if (binding.type == ResourceType::Sampler)
            {
                assert(!descriptorRangesSamplers.empty());
                D3D12_DESCRIPTOR_RANGE1& range = descriptorRangesSamplers[descriptorRangesSamplers.size() - 1];

                range.NumDescriptors += binding.size;
                descriptorTableSizeSamplers += binding.size;
            }
            else
            {
                assert(!descriptorRangesSRVetc.empty());
                D3D12_DESCRIPTOR_RANGE1& range = descriptorRangesSRVetc[descriptorRangesSRVetc.size() - 1];

                range.NumDescriptors += binding.size;
                descriptorTableSizeSRVetc += binding.size;

                bindingLayoutsSRVetc.push_back(binding);
            }

            currentSlot = binding.slot;
        }
    }

    // A layout occupies a contiguous segment of a root signature.
    // The root parameter indices stored here are relative to the beginning of that segment.

    rootParameters.resize(0);

    if (rootConstants.Num32BitValues)
    {
        D3D12_ROOT_PARAMETER1& param = rootParameters.emplace_back();

        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.ShaderVisibility = convertShaderStage(desc.visibility);
        param.Constants = rootConstants;

        rootParameterPushConstants = RootParameterIndex(rootParameters.size() - 1);
    }

    for (std::pair<RootParameterIndex, D3D12_ROOT_DESCRIPTOR1>& rootParameterVolatileCB : rootParametersVolatileCB)
    {
        rootParameters.resize(rootParameters.size() + 1);
        D3D12_ROOT_PARAMETER1& param = rootParameters[rootParameters.size() - 1];

        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.ShaderVisibility = convertShaderStage(desc.visibility);
        param.Descriptor = rootParameterVolatileCB.second;

        rootParameterVolatileCB.first = RootParameterIndex(rootParameters.size() - 1);
    }

    if (descriptorTableSizeSamplers > 0)
    {
        rootParameters.resize(rootParameters.size() + 1);
        D3D12_ROOT_PARAMETER1& param = rootParameters[rootParameters.size() - 1];

        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.ShaderVisibility = convertShaderStage(desc.visibility);
        param.DescriptorTable.NumDescriptorRanges = UINT(descriptorRangesSamplers.size());
        param.DescriptorTable.pDescriptorRanges = &descriptorRangesSamplers[0];

        rootParameterSamplers = RootParameterIndex(rootParameters.size() - 1);
    }

    if (descriptorTableSizeSRVetc > 0)
    {
        rootParameters.resize(rootParameters.size() + 1);
        D3D12_ROOT_PARAMETER1& param = rootParameters[rootParameters.size() - 1];

        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.ShaderVisibility = convertShaderStage(desc.visibility);
        param.DescriptorTable.NumDescriptorRanges = UINT(descriptorRangesSRVetc.size());
        param.DescriptorTable.pDescriptorRanges = &descriptorRangesSRVetc[0];

        rootParameterSRVetc = RootParameterIndex(rootParameters.size() - 1);
    }
}

// Port of nvrhi BindlessLayout::BindlessLayout (d3d12-resource-bindings.cpp):
// one unbounded descriptor range per register space, all sharing the table's
// base slot; the whole layout is a single root parameter.
BindingLayout::BindingLayout(const RhiBindlessLayoutDesc& _desc,
    const RhiBindingLayoutItem* items, uint32_t itemCount)
    : RhiResource(Kind::BindingLayout)
    , isBindless(true)
    , bindlessDesc(_desc)
{
    if (items && itemCount > 0)
        bindings.assign(items, items + itemCount);
    bindlessRanges.reserve(itemCount);

    for (const RhiBindingLayoutItem& item : bindings)
    {
        D3D12_DESCRIPTOR_RANGE_TYPE rangeType;

        switch (item.type)
        {
        case ResourceType::Texture_SRV:
        case ResourceType::TypedBuffer_SRV:
        case ResourceType::StructuredBuffer_SRV:
        case ResourceType::RawBuffer_SRV:
        case ResourceType::RayTracingAccelStruct:
            rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            break;

        case ResourceType::ConstantBuffer:
            rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            break;

        case ResourceType::Texture_UAV:
        case ResourceType::TypedBuffer_UAV:
        case ResourceType::StructuredBuffer_UAV:
        case ResourceType::RawBuffer_UAV:
        case ResourceType::SamplerFeedbackTexture_UAV:
            rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            break;

        case ResourceType::Sampler:
            rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            break;

        default:
            LogError("[UnityRHI] BindlessLayout: invalid register-space resource type %u.",
                uint32_t(item.type));
            continue;
        }

        D3D12_DESCRIPTOR_RANGE1& descriptorRange = bindlessRanges.emplace_back();

        descriptorRange.RangeType = rangeType;
        descriptorRange.NumDescriptors = ~0u; // unbounded
        descriptorRange.BaseShaderRegister = bindlessDesc.firstSlot;
        descriptorRange.RegisterSpace = item.slot;
        descriptorRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        descriptorRange.OffsetInDescriptorsFromTableStart = 0;
    }

    if (RhiBindlessLayoutType(bindlessDesc.layoutType) == RhiBindlessLayoutType::Immutable)
    {
        bindlessRootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        bindlessRootParameter.ShaderVisibility = convertShaderStage(bindlessDesc.visibility);
        bindlessRootParameter.DescriptorTable.NumDescriptorRanges = uint32_t(bindlessRanges.size());
        bindlessRootParameter.DescriptorTable.pDescriptorRanges = &bindlessRanges[0];
    }
}

// Port of nvrhi Device::buildRootSignature.
std::shared_ptr<RootSignature> Device::buildRootSignature(
    BindingLayout* const* layouts, uint32_t layoutCount, bool isLocal, bool allowInputLayout)
{
    auto rootsig = std::make_shared<RootSignature>();

    // Assemble the root parameter table from the pipeline binding layouts
    // Also attach the root parameter offsets to the pipeline layouts

    std::vector<D3D12_ROOT_PARAMETER1> rootParameters;
    bool usesResourceDescriptorHeap = false;
    bool usesSamplerDescriptorHeap = false;

    for (uint32_t layoutIndex = 0; layoutIndex < layoutCount; layoutIndex++)
    {
        BindingLayout* layout = layouts[layoutIndex];
        RootParameterIndex rootParameterOffset = RootParameterIndex(rootParameters.size());

        if (!layout->isBindless)
        {
            rootsig->pipelineLayouts.push_back(std::make_pair(layout, rootParameterOffset));

            rootParameters.insert(rootParameters.end(), layout->rootParameters.begin(), layout->rootParameters.end());

            if (layout->pushConstantByteSize)
            {
                rootsig->pushConstantByteSize = layout->pushConstantByteSize;
                rootsig->rootParameterPushConstants = layout->rootParameterPushConstants + rootParameterOffset;
            }
        }
        else
        {
            const RhiBindlessLayoutType layoutType = RhiBindlessLayoutType(layout->bindlessDesc.layoutType);
            if (layoutType != RhiBindlessLayoutType::Immutable)
            {
                // Mutable layouts use ResourceDescriptorHeap/SamplerDescriptorHeap,
                // so there is no root parameter for this layout.
                rootsig->pipelineLayouts.push_back(std::make_pair(layout, c_InvalidRootParameterIndex));
                if (layoutType == RhiBindlessLayoutType::MutableSampler)
                    usesSamplerDescriptorHeap = true;
                else
                    usesResourceDescriptorHeap = true;
            }
            else
            {
                // Immutable bindless layout: a single descriptor-table root
                // parameter with unbounded ranges.
                rootsig->pipelineLayouts.push_back(std::make_pair(layout, rootParameterOffset));
                rootParameters.push_back(layout->bindlessRootParameter);
            }
        }
    }

    // Build the description structure

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;

    if (isLocal)
    {
        rsDesc.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    }

    if (allowInputLayout)
    {
        rsDesc.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    }

    if (m_HeapDirectlyIndexedEnabled)
    {
        if (usesSamplerDescriptorHeap)
            rsDesc.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
        if (usesResourceDescriptorHeap)
            rsDesc.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    }

    if (!rootParameters.empty())
    {
        rsDesc.Desc_1_1.pParameters = rootParameters.data();
        rsDesc.Desc_1_1.NumParameters = UINT(rootParameters.size());
    }

    // Serialize the root signature

    ComPtr<ID3DBlob> rsBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT res = D3D12SerializeVersionedRootSignature(&rsDesc, &rsBlob, &errorBlob);

    if (FAILED(res))
    {
        LogError("[UnityRHI] D3D12SerializeVersionedRootSignature call failed (hr=0x%08X): %s",
            static_cast<unsigned>(res),
            errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "(no message)");
        return nullptr;
    }

    // Create the RS object

    res = m_Context.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS(&rootsig->handle));

    if (FAILED(res))
    {
        LogError("[UnityRHI] CreateRootSignature call failed (hr=0x%08X).", static_cast<unsigned>(res));
        return nullptr;
    }

    return rootsig;
}

// Port of nvrhi Device::getRootSignature; the cache is keyed on the layout
// pointer combination.
std::shared_ptr<RootSignature> Device::getRootSignature(
    BindingLayout* const* layouts, uint32_t layoutCount, bool allowInputLayout)
{
    size_t hash = allowInputLayout ? 1 : 0;
    for (uint32_t i = 0; i < layoutCount; ++i)
        hash = hash * 31 + std::hash<uint64_t>()(layouts[i] ? layouts[i]->uniqueId : 0);

    const auto range = m_Resources.rootsigCache.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (std::shared_ptr<RootSignature> cached = it->second.lock())
        {
            if (cached->allowInputLayout != allowInputLayout ||
                cached->pipelineLayouts.size() != layoutCount)
                continue;

            bool equal = true;
            for (uint32_t index = 0; index < layoutCount; ++index)
            {
                if (cached->pipelineLayouts[index].first != layouts[index])
                {
                    equal = false;
                    break;
                }
            }
            if (equal)
                return cached;
        }
    }

    std::shared_ptr<RootSignature> rootsig = buildRootSignature(layouts, layoutCount, false, allowInputLayout);
    if (!rootsig)
        return nullptr;

    rootsig->hash = hash;
    rootsig->allowInputLayout = allowInputLayout;
    rootsig->m_Resources = &m_Resources;
    m_Resources.rootsigCache.emplace(hash, rootsig);
    return rootsig;
}

RootSignature::~RootSignature()
{
    // Remove the root signature from the cache
    if (m_Resources)
    {
        const auto range = m_Resources->rootsigCache.equal_range(hash);
        for (auto it = range.first; it != range.second;)
        {
            std::shared_ptr<RootSignature> cached = it->second.lock();
            if (!cached || cached.get() == this)
                it = m_Resources->rootsigCache.erase(it);
            else
                ++it;
        }
    }
}

// Port of nvrhi Device::writeDescriptorTable.
bool Device::writeDescriptorTable(DescriptorTable* descriptorTable, const RhiBindingSetItem& binding)
{
    if (!descriptorTable)
        return false;

    if (binding.slot >= descriptorTable->capacity)
        return false;

    StaticDescriptorHeap& heap = descriptorTable->getDescriptorHeap();
    D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle =
        heap.getCpuHandle(descriptorTable->firstDescriptor + binding.slot);

    auto* resource = static_cast<RhiResource*>(binding.resource);

    if (descriptorTable->isSamplerTable() && binding.type != ResourceType::Sampler)
    {
        LogError("[UnityRHI] writeDescriptorTable '%s': mutable sampler tables accept only samplers.",
            descriptorTable->debugName.c_str());
        return false;
    }
    if (!descriptorTable->isSamplerTable() && binding.type == ResourceType::Sampler)
    {
        LogError("[UnityRHI] writeDescriptorTable '%s': samplers require a mutable sampler layout.",
            descriptorTable->debugName.c_str());
        return false;
    }

    switch (binding.type)
    {
    case ResourceType::None:
        Buffer::createNullSRV(descriptorHandle.ptr, Format::UNKNOWN, m_Context);
        break;
    case ResourceType::Texture_SRV: {
        if (!resource || resource->kind != RhiResource::Kind::Texture)
            return false;
        auto* texture = static_cast<Texture*>(resource);
        texture->createSRV(descriptorHandle.ptr, binding.format, binding.dimension, binding.subresources());
        break;
    }
    case ResourceType::Texture_UAV: {
        if (!resource || resource->kind != RhiResource::Kind::Texture)
            return false;
        auto* texture = static_cast<Texture*>(resource);
        texture->createUAV(descriptorHandle.ptr, binding.format, binding.dimension, binding.subresources());
        break;
    }
    case ResourceType::TypedBuffer_SRV:
    case ResourceType::StructuredBuffer_SRV:
    case ResourceType::RawBuffer_SRV: {
        if (!resource || resource->kind != RhiResource::Kind::Buffer)
            return false;
        auto* buffer = static_cast<Buffer*>(resource);
        buffer->createSRV(descriptorHandle.ptr, binding.format, binding.range(), binding.type);
        break;
    }
    case ResourceType::TypedBuffer_UAV:
    case ResourceType::StructuredBuffer_UAV:
    case ResourceType::RawBuffer_UAV: {
        if (!resource || resource->kind != RhiResource::Kind::Buffer)
            return false;
        auto* buffer = static_cast<Buffer*>(resource);
        buffer->createUAV(descriptorHandle.ptr, binding.format, binding.range(), binding.type);
        break;
    }
    case ResourceType::ConstantBuffer: {
        if (!resource || resource->kind != RhiResource::Kind::Buffer)
            return false;
        auto* buffer = static_cast<Buffer*>(resource);
        buffer->createCBV(descriptorHandle.ptr, binding.range());
        break;
    }
    case ResourceType::RayTracingAccelStruct: {
        if (!resource || resource->kind != RhiResource::Kind::AccelStruct)
            return false;
        auto* accelStruct = static_cast<AccelStruct*>(resource);
        accelStruct->createSRV(descriptorHandle.ptr);
        break;
    }

    case ResourceType::VolatileConstantBuffer:
        m_Context.error("Attempted to bind a volatile constant buffer to a bindless set.");
        return false;

    case ResourceType::Sampler: {
        if (!resource || resource->kind != RhiResource::Kind::Sampler)
            return false;
        static_cast<Sampler*>(resource)->createDescriptor(descriptorHandle.ptr);
        break;
    }
    case ResourceType::SamplerFeedbackTexture_UAV:
    case ResourceType::PushConstants:
    default:
        LogError("[UnityRHI] writeDescriptorTable: invalid resource type %u.", uint32_t(binding.type));
        return false;
    }

    // Descriptor tables normally do not retain resource handles or participate
    // in NVRHI automatic barriers. Preserve only Unity-owned entries: unlike
    // plugin resources, they must be transitioned through Unity's state tracker
    // on the render thread before the table is consumed.
    if (binding.slot < descriptorTable->unityResourceBindings.size())
    {
        descriptorTable->unityResourceBindings[binding.slot] =
            resource && resource->unityOwnedResource ? binding : RhiBindingSetItem{};
    }

    heap.copyToShaderVisibleHeap(descriptorTable->firstDescriptor + binding.slot, 1);
    return true;
}

// Port of nvrhi Device::resizeDescriptorTable.
void Device::resizeDescriptorTable(DescriptorTable* descriptorTable, uint32_t newSize, bool keepContents)
{
    if (!descriptorTable)
        return;

    if (newSize == descriptorTable->capacity)
        return;

    if (newSize > descriptorTable->layout->bindlessDesc.maxCapacity)
    {
        LogError("[UnityRHI] resizeDescriptorTable '%s': requested size %u exceeds layout capacity %u.",
            descriptorTable->debugName.c_str(), newSize, descriptorTable->layout->bindlessDesc.maxCapacity);
        return;
    }

    StaticDescriptorHeap& heap = descriptorTable->getDescriptorHeap();
    const D3D12_DESCRIPTOR_HEAP_TYPE heapType = heap.getHeapType();

    if (newSize < descriptorTable->capacity)
    {
        heap.releaseDescriptors(
            descriptorTable->firstDescriptor + newSize, descriptorTable->capacity - newSize);
        descriptorTable->unityResourceBindings.resize(newSize);
        descriptorTable->capacity = newSize;
        return;
    }

    uint32_t originalFirst = descriptorTable->firstDescriptor;
    if (!keepContents && descriptorTable->capacity > 0)
    {
        heap.releaseDescriptors(
            descriptorTable->firstDescriptor, descriptorTable->capacity);
    }

    descriptorTable->firstDescriptor = heap.allocateDescriptors(newSize);

    if (keepContents && descriptorTable->capacity > 0)
    {
        m_Context.device->CopyDescriptorsSimple(descriptorTable->capacity,
            heap.getCpuHandle(descriptorTable->firstDescriptor),
            heap.getCpuHandle(originalFirst),
            heapType);

        m_Context.device->CopyDescriptorsSimple(descriptorTable->capacity,
            heap.getCpuHandleShaderVisible(descriptorTable->firstDescriptor),
            heap.getCpuHandle(originalFirst),
            heapType);

        heap.releaseDescriptors(originalFirst, descriptorTable->capacity);
    }

    if (keepContents)
        descriptorTable->unityResourceBindings.resize(newSize);
    else
        descriptorTable->unityResourceBindings.assign(newSize, RhiBindingSetItem{});

    descriptorTable->capacity = newSize;
}

uint32_t Device::getDescriptorTableFirstDescriptorIndexInHeap(DescriptorTable* descriptorTable) const
{
    return descriptorTable ? descriptorTable->firstDescriptor : c_InvalidDescriptorIndex;
}

void ReplayContext::setResourceStatesForDescriptorTable(DescriptorTable* descriptorTable)
{
    if (!descriptorTable || descriptorTable->unityResourceBindings.empty())
        return;

    const ResourceStates shaderResourceState =
        getShaderResourceStateForBindingLayout(descriptorTable->layout);

    for (const RhiBindingSetItem& binding : descriptorTable->unityResourceBindings)
    {
        auto* resource = static_cast<RhiResource*>(binding.resource);
        if (!resource || !resource->unityOwnedResource)
            continue;

        switch (binding.type)
        {
        case ResourceType::Texture_SRV:
            requireTextureState(static_cast<Texture*>(resource), binding.subresources(), shaderResourceState);
            break;
        case ResourceType::Texture_UAV:
            requireTextureState(static_cast<Texture*>(resource), binding.subresources(), ResourceStates::UnorderedAccess);
            break;
        case ResourceType::TypedBuffer_SRV:
        case ResourceType::StructuredBuffer_SRV:
        case ResourceType::RawBuffer_SRV:
            requireBufferState(static_cast<Buffer*>(resource), shaderResourceState);
            break;
        case ResourceType::TypedBuffer_UAV:
        case ResourceType::StructuredBuffer_UAV:
        case ResourceType::RawBuffer_UAV:
            requireBufferState(static_cast<Buffer*>(resource), ResourceStates::UnorderedAccess);
            break;
        case ResourceType::ConstantBuffer:
            requireBufferState(static_cast<Buffer*>(resource), ResourceStates::ConstantBuffer);
            break;
        default:
            continue;
        }

        resource->lastUseFenceValue = fenceValue;
    }
}

// Port of nvrhi CommandList::setComputeBindings / setGraphicsBindings;
// `graphics` selects the root-parameter API.
bool ReplayContext::setComputeBindings(const std::vector<RhiResource*>& bindings, uint32_t bindingUpdateMask,
    Buffer* indirectParams, bool updateIndirectParams, const RootSignature* rootSignature, bool graphics)
{
    Device* device = Device::Get();
    if (!device || !rootSignature)
        return false;

    auto setRootCBV = [&](RootParameterIndex index, D3D12_GPU_VIRTUAL_ADDRESS va) {
        if (graphics)
            commandList->SetGraphicsRootConstantBufferView(index, va);
        else
            commandList->SetComputeRootConstantBufferView(index, va);
    };
    auto setRootTable = [&](RootParameterIndex index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
        if (graphics)
            commandList->SetGraphicsRootDescriptorTable(index, handle);
        else
            commandList->SetComputeRootDescriptorTable(index, handle);
    };
    std::vector<VolatileConstantBufferBinding>& currentVolatileCBs =
        graphics ? currentGraphicsVolatileCBs : currentComputeVolatileCBs;

    if (bindingUpdateMask)
    {
        std::vector<VolatileConstantBufferBinding> newVolatileCBs;

        for (uint32_t bindingSetIndex = 0; bindingSetIndex < bindings.size(); ++bindingSetIndex)
        {
            RhiResource* binding = bindings[bindingSetIndex];
            if (!binding)
                continue;
            Device* liveDevice = Device::Get();
            if (!liveDevice || !liveDevice->IsLiveResource(binding))
            {
                LogError("[UnityRHI] Binding %u is null, disposed, or absent from the live-resource "
                         "registry (command=%u, reuseBindings=%u, handle=%p).",
                    bindingSetIndex, diagnosticCommandIndex,
                    diagnosticReuseBindings ? 1u : 0u, binding);
                if (liveDevice)
                    liveDevice->LogResourceDiagnostic("binding", bindingSetIndex, binding);
                return false;
            }

            if (bindingSetIndex >= rootSignature->pipelineLayouts.size())
            {
                LogError("[UnityRHI] Binding index %u exceeds root signature layout count.", bindingSetIndex);
                return false;
            }

            const bool updateThisSet = (bindingUpdateMask & (1u << bindingSetIndex)) != 0;
            const auto& layoutAndOffset = rootSignature->pipelineLayouts[bindingSetIndex];
            const RootParameterIndex rootParameterOffset = layoutAndOffset.second;

            if (binding->kind == RhiResource::Kind::BindingSet)
            {
                auto* bindingSet = static_cast<BindingSet*>(binding);
                if (layoutAndOffset.first != bindingSet->layout)
                {
                    LogError("[UnityRHI] Binding set %u does not match the pipeline's binding layout.", bindingSetIndex);
                    return false;
                }

                for (size_t volatileCbIndex = 0; volatileCbIndex < bindingSet->rootParametersVolatileCB.size(); ++volatileCbIndex)
                {
                    const auto& parameter = bindingSet->rootParametersVolatileCB[volatileCbIndex];
                    const RootParameterIndex rootParameterIndex = rootParameterOffset + parameter.first;

                    if (parameter.second)
                    {
                        Buffer* buffer = parameter.second;

                        if (buffer->desc.isVolatile)
                        {
                            const auto found = volatileConstantBufferAddresses.find(buffer);
                            const D3D12_GPU_VIRTUAL_ADDRESS volatileData =
                                found != volatileConstantBufferAddresses.end() ? found->second : 0;

                            if (!volatileData)
                            {
                                LogError("[UnityRHI] Attempted use of a volatile constant buffer '%s' before it was written into",
                                    buffer->debugName.c_str());
                                continue;
                            }

                            // NVRHI indexes the old array by newVolatileCBs.size(),
                            // because volatileCbIndex restarts for each binding set.
                            const size_t currentIndex = newVolatileCBs.size();
                            const bool addressChanged = currentIndex >= currentVolatileCBs.size() ||
                                currentVolatileCBs[currentIndex].address != volatileData;
                            if (updateThisSet || addressChanged)
                                setRootCBV(rootParameterIndex, volatileData);

                            newVolatileCBs.push_back({rootParameterIndex, buffer, volatileData});
                        }
                        else if (updateThisSet)
                        {
                            setRootCBV(rootParameterIndex, buffer->gpuVA);
                        }

                        buffer->lastUseFenceValue = fenceValue;
                    }
                    else if (updateThisSet)
                    {
                        setRootCBV(rootParameterIndex, 0);
                    }
                }

                if (updateThisSet)
                {
                    if (bindingSet->descriptorTableValidSamplers)
                    {
                        setRootTable(rootParameterOffset + bindingSet->rootParameterIndexSamplers,
                            device->resources().samplerHeap.getGpuHandle(bindingSet->descriptorTableSamplers));
                    }

                    if (bindingSet->descriptorTableValidSRVetc)
                    {
                        setRootTable(rootParameterOffset + bindingSet->rootParameterIndexSRVetc,
                            device->resources().shaderResourceViewHeap.getGpuHandle(bindingSet->descriptorTableSRVetc));
                    }
                }

                // UAV bindings may place UAV barriers on the same binding set
                if (enableAutomaticBarriers &&
                    (bindingStatesDirty || updateThisSet || bindingSet->hasUavBindings))
                {
                    setResourceStatesForBindingSet(bindingSet);
                }

                bindingSet->lastUseFenceValue = fenceValue;
            }
            else if (binding->kind == RhiResource::Kind::DescriptorTable)
            {
                auto* descriptorTable = static_cast<DescriptorTable*>(binding);
                if (layoutAndOffset.first != descriptorTable->layout)
                {
                    LogError("[UnityRHI] Descriptor table %u does not match the pipeline's bindless layout.",
                        bindingSetIndex);
                    return false;
                }

                if (rootParameterOffset != c_InvalidRootParameterIndex && updateThisSet)
                {
                    setRootTable(rootParameterOffset,
                        descriptorTable->getDescriptorHeap().getGpuHandle(descriptorTable->firstDescriptor));
                }
                if (enableAutomaticBarriers && updateThisSet)
                    setResourceStatesForDescriptorTable(descriptorTable);
                // Mutable layouts have no root table, but their heap indices
                // are still consumed by the GPU and must remain live.
                descriptorTable->lastUseFenceValue = fenceValue;
            }
            else
            {
                LogError("[UnityRHI] Binding %u is not a BindingSet or DescriptorTable.", bindingSetIndex);
                return false;
            }
        }

        currentVolatileCBs = std::move(newVolatileCBs);
    }

    if (indirectParams && updateIndirectParams)
    {
        if (enableAutomaticBarriers)
            requireBufferState(indirectParams, ResourceStates::IndirectArgument);
        indirectParams->lastUseFenceValue = fenceValue;
    }

    const uint32_t bindingMask = bindings.size() >= 32 ? ~0u : ((1u << uint32_t(bindings.size())) - 1u);
    if ((bindingUpdateMask & bindingMask) == bindingMask)
        anyVolatileBufferWrites = false;

    return true;
}
} // namespace unityrhi
