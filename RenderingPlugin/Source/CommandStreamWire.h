#pragma once

#include <cstdint>

namespace unityrhi::wire
{
#pragma pack(push, 1)
struct UInt32Payload { uint32_t value; };
struct HandlePayload { uint64_t handle; };
struct HandleUInt32Payload { uint64_t handle; uint32_t value; };
struct CopyBufferPayload { uint64_t dest, destOffset, src, srcOffset, byteSize; };
struct CopyTextureToBufferPayload
{
    uint64_t dest, destOffset, src; uint32_t arraySlice, mipLevel;
};
struct MarkerPayload { uint32_t byteSize; };
struct TextureSubresources { uint32_t baseMipLevel, numMipLevels, baseArraySlice, numArraySlices; };
struct TextureSlice { uint32_t x, y, z, width, height, depth, mipLevel, arraySlice; };
struct TextureCopyPayload
{
    uint64_t dest; TextureSlice destSlice; uint64_t src; TextureSlice srcSlice;
};
struct ResolveTexturePayload
{
    uint64_t dest; TextureSubresources destSubresources;
    uint64_t src; TextureSubresources srcSubresources;
};
struct TextureSubresourceStatePayload
{
    uint64_t texture; TextureSubresources subresources; uint32_t state;
};
struct WriteBufferPayload { uint64_t buffer, destOffset, uploadTicket; };
struct WriteTexturePayload
{
    uint64_t texture; uint32_t arraySlice, mipLevel;
    uint64_t uploadTicket;
};
constexpr uint32_t StateFlagReuseBindings = 1u;
struct StatePayload
{
    uint64_t object, indirectParams; uint32_t bindingCount, flags;
};
struct BindingStatePayload { uint64_t object; uint32_t bindingCount; };
struct AccelStructBuildPayload
{
    uint64_t accelStruct; uint32_t elementCount, buildFlags;
};
struct TlasFromBufferPayload
{
    uint64_t accelStruct, instanceBuffer, instanceBufferOffset;
    uint32_t numInstances, buildFlags;
};
struct DispatchPayload { uint32_t x, y, z; };
struct DrawPayload { uint32_t a, b, c, d; };
struct DrawIndexedPayload { uint32_t a, b, c, d, e; };
struct DrawIndirectPayload { uint64_t offset; uint32_t count; };
struct DrawIndirectCountPayload
{
    uint64_t paramsOffset; uint32_t maxDrawCount; uint64_t countOffset;
};
struct ClearTextureFloatPayload
{
    uint64_t texture; float r, g, b, a;
};
struct ClearTextureFloatSubresourcesPayload
{
    uint64_t texture; TextureSubresources subresources; float r, g, b, a;
};
struct ClearDepthStencilPayload
{
    uint64_t texture; uint32_t clearDepth; float depth; uint32_t clearStencil, stencil;
};
struct ClearDepthStencilSubresourcesPayload
{
    uint64_t texture; TextureSubresources subresources;
    uint32_t clearDepth; float depth; uint32_t clearStencil, stencil;
};
struct ClearTextureUIntPayload
{
    uint64_t texture; TextureSubresources subresources; uint32_t value;
};
struct Viewport { float minX, maxX, minY, maxY, minZ, maxZ; };
struct GraphicsStatePayload
{
    uint64_t pipeline, framebuffer; Viewport viewport;
    float blendR, blendG, blendB, blendA; uint64_t indexBuffer; uint32_t indexFormat;
    uint64_t indexOffset, indirectParams, indirectCountBuffer; uint32_t vertexBufferCount;
};
struct VertexBufferBinding { uint64_t buffer; uint32_t slot; uint64_t offset; };
struct DlrrDispatchPayload
{
    uint64_t input, output, motionVectors, depth;
    uint64_t diffuseAlbedo, specularAlbedo, normalRoughness, specularMotion;
    float worldToViewMatrix[16];
    float viewToClipMatrix[16];
    uint16_t outputWidth, outputHeight, currentWidth, currentHeight;
    float cameraJitterX, cameraJitterY;
    int32_t instanceId;
    uint8_t useSpecularMotionVector, upscalerMode, preset;
};
struct DlssNrDispatchPayload
{
    uint64_t color, output, motionVectors, depth;
    uint16_t inputWidth, inputHeight, outputWidth, outputHeight;
    float motionVectorScaleX, motionVectorScaleY;
    float intensity, localToneStrength, localStructureStrength, skinStructureStrength;
    int32_t instanceId;
    uint8_t depthInverted, reset, useAutoMask, uiCorrection;
    uint8_t upscaling, preset, style;
};
#pragma pack(pop)

static_assert(sizeof(UInt32Payload) == 4);
static_assert(sizeof(HandlePayload) == 8);
static_assert(sizeof(HandleUInt32Payload) == 12);
static_assert(sizeof(CopyBufferPayload) == 40);
static_assert(sizeof(CopyTextureToBufferPayload) == 32);
static_assert(sizeof(MarkerPayload) == 4);
static_assert(sizeof(TextureSubresources) == 16);
static_assert(sizeof(TextureSlice) == 32);
static_assert(sizeof(TextureCopyPayload) == 80);
static_assert(sizeof(ResolveTexturePayload) == 48);
static_assert(sizeof(TextureSubresourceStatePayload) == 28);
static_assert(sizeof(WriteBufferPayload) == 24);
static_assert(sizeof(WriteTexturePayload) == 24);
static_assert(sizeof(StatePayload) == 24);
static_assert(sizeof(BindingStatePayload) == 12);
static_assert(sizeof(AccelStructBuildPayload) == 16);
static_assert(sizeof(TlasFromBufferPayload) == 32);
static_assert(sizeof(DispatchPayload) == 12);
static_assert(sizeof(DrawPayload) == 16);
static_assert(sizeof(DrawIndexedPayload) == 20);
static_assert(sizeof(DrawIndirectPayload) == 12);
static_assert(sizeof(DrawIndirectCountPayload) == 20);
static_assert(sizeof(ClearTextureFloatPayload) == 24);
static_assert(sizeof(ClearTextureFloatSubresourcesPayload) == 40);
static_assert(sizeof(ClearDepthStencilPayload) == 24);
static_assert(sizeof(ClearDepthStencilSubresourcesPayload) == 40);
static_assert(sizeof(ClearTextureUIntPayload) == 28);
static_assert(sizeof(Viewport) == 24);
static_assert(sizeof(GraphicsStatePayload) == 96);
static_assert(sizeof(VertexBufferBinding) == 20);
static_assert(sizeof(DlrrDispatchPayload) == 215);
static_assert(sizeof(DlssNrDispatchPayload) == 75);
}
