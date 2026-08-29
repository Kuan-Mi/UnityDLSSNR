// Shader objects, mirroring NVRHI's d3d12-shader.cpp (External/nvrhi/src/d3d12).
// Copyright (c) 2014-2021, NVIDIA CORPORATION. MIT license
// (see ThirdParty/NVRHI-LICENSE.txt).
//
// Shader compilation itself lives in ShaderCompiler.cpp (DXC).

#include "d3d12-backend.h"

#include "common/dxgi-format.h"
#include "UnityRhiLog.h"

namespace unityrhi
{
Shader* Device::createShader(const ShaderDesc& d, const void* binary, size_t binarySize)
{
    if (binarySize == 0)
        return nullptr;

    auto* shader = new Shader();
    shader->desc = d;
    shader->debugName = d.debugName;
    const auto* data = static_cast<const uint8_t*>(binary);
    shader->bytecode = std::make_shared<std::vector<uint8_t>>(data, data + binarySize);
    RegisterResource(shader);
    return shader;
}

// Port of nvrhi Device::createShaderLibrary (d3d12-shader.cpp).
ShaderLibrary* Device::createShaderLibrary(const void* bytecode, uint64_t byteSize, const char* debugName)
{
    if (!bytecode || byteSize == 0)
    {
        LogError("[UnityRHI] createShaderLibrary '%s': bytecode must be non-empty.",
            debugName ? debugName : "");
        return nullptr;
    }

    auto* shaderLibrary = new ShaderLibrary();
    shaderLibrary->debugName = debugName ? debugName : "";
    const auto* data = static_cast<const uint8_t*>(bytecode);
    shaderLibrary->bytecode = std::make_shared<std::vector<uint8_t>>(data, data + byteSize);
    RegisterResource(shaderLibrary);
    return shaderLibrary;
}

// Port of nvrhi Device::createInputLayout. UnityRHI receives the already
// compiled layout through its C ABI, so arraySize expansion is not exposed.
InputLayout* Device::createInputLayout(
    const RhiVertexAttributeDesc* attributes, uint32_t attributeCount, const char* debugName)
{
    if (!attributes || attributeCount == 0)
    {
        LogError("[UnityRHI] createInputLayout '%s': at least one attribute is required.",
            debugName ? debugName : "");
        return nullptr;
    }

    auto* layout = new InputLayout();
    layout->debugName = debugName ? debugName : "";
    layout->semanticNames.reserve(attributeCount);

    for (uint32_t index = 0; index < attributeCount; ++index)
    {
        const RhiVertexAttributeDesc& attr = attributes[index];
        layout->semanticNames.emplace_back(attr.name ? attr.name : "");

        D3D12_INPUT_ELEMENT_DESC desc{};
        desc.SemanticName = layout->semanticNames.back().c_str();
        desc.SemanticIndex = attr.semanticIndex;
        desc.Format = getDxgiFormatMapping(attr.format).srvFormat;
        desc.InputSlot = attr.bufferIndex;
        desc.AlignedByteOffset = attr.offset;
        desc.InputSlotClass = attr.isInstanced
            ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
            : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        desc.InstanceDataStepRate = attr.isInstanced ? 1 : 0;
        layout->inputElements.push_back(desc);

        if (layout->elementStrides.find(attr.bufferIndex) == layout->elementStrides.end())
            layout->elementStrides[attr.bufferIndex] = attr.elementStride;
    }

    RegisterResource(layout);
    return layout;
}

// Port of nvrhi ShaderLibrary::getShader / ShaderLibraryEntry. UnityRHI
// deviation: the entry is a plain Shader sharing the library's blob (a
// shared_ptr replaces NVRHI's RefCountPtr back-reference).
Shader* Device::createLibraryShader(ShaderLibrary* library, const char* entryName, ShaderType shaderType)
{
    if (!library || !library->bytecode)
        return nullptr;

    auto* shader = new Shader();
    shader->desc.shaderType = shaderType;
    shader->desc.entryName = entryName ? entryName : "";
    shader->desc.debugName = shader->desc.entryName;
    shader->debugName = shader->desc.debugName;
    shader->bytecode = library->bytecode;
    RegisterResource(shader);
    return shader;
}
} // namespace unityrhi
