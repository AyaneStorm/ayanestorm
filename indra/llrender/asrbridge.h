/**
 * @file asrbridge.h
 * @brief What llrender hands to the new renderer, without linking to it.
 *
 * llrender is built with or without AS_RENDERER, and the renderer library links to
 * the viewer only. So llrender talks to the RendererLink interface, which
 * asrviewer.cpp fills in. Only .cpp files include this header, so changing it never
 * rebuilds the viewer.
 *
 * UI geometry and textures go through here as what they are. This is not GL
 * emulation: see section 1 of doc/ayanestorm-new-renderer-plan.md.
 */
#ifndef ASR_BRIDGE_H
#define ASR_BRIDGE_H

#include "stdtypes.h"

#include <glm/fwd.hpp>

#include <vector>

namespace ASR
{
    // How a texture's pixels arrive. Every layout has 8 bits per channel.
    enum class PixelLayout : U8
    {
        RGBA,
        BGRA,
        RGB,
        BGR,
        Red,
        RG,
        Alpha,
        Luminance,
        LuminanceAlpha
    };

    struct UIDrawData
    {
        const F32* positions = nullptr; // x, y, z, and one unused float per vertex
        const F32* uvs = nullptr;       // two floats per vertex
        const U32* colors = nullptr;    // RGBA8 per vertex
        const F32* matrix = nullptr;    // to clip space
        const F32* uvMatrix = nullptr;  // null when it is the identity
        U32 count = 0;
        U32 slot = 0;                   // where the texture is, 0 being white
        U8 mode = 0;                    // LLRender::eGeomModes
        U8 sampler = 0;                 // in the order of asr::UISampler
        bool solidColor = false;        // gSolidColorProgram, not gUIProgram
    };

    class RendererLink
    {
    public:
        virtual ~RendererLink() = default;

        // storage: 0 keeps the pixels' layout; otherwise what chooseStorage() gave, a
        // compressed format the GPU encodes the uploads into (see
        // doc/ayanestorm-new-renderer-texture-compression.md).
        virtual U32 createTexture(U32 width, U32 height, U32 mipLevels, PixelLayout layout, U8 storage) = 0;
        // rowPixels is the width of the data that pixels points into.
        virtual void uploadTexture(U32 slot, U32 x, U32 y, U32 width, U32 height, PixelLayout layout,
                                   const void* pixels, U32 rowPixels, U8 storage) = 0;
        // Makes the smaller mips; for a compressed texture, also encodes it.
        virtual void generateMips(U32 slot) = 0;
        // A new image for a slot in use, which keeps showing its old one until publishTexture(),
        // so a resize never shows white. Uploads and mips go to the new image meanwhile. With
        // fromMip > 0 it is made on the GPU from the old image's mips, from fromMip down.
        virtual void replaceTexture(U32 slot, U32 width, U32 height, U32 mipLevels, PixelLayout layout, U32 fromMip, U8 storage) = 0;
        // How a fetched image is kept, from its pixels: 0 as it is, or a compressed format.
        virtual U8 chooseStorage(U32 width, U32 height, PixelLayout layout, const void* pixels) = 0;
        virtual void publishTexture(U32 slot) = 0;
        virtual void destroyTexture(U32 slot) = 0;
        // Bytes that exist or have been asked for, how much textures should take (what the
        // discard bias steers by), and how much a new image may never push them past.
        virtual void getTextureMemory(U64& used, U64& limit, U64& hardLimit) = 0;
        // Admission for a decoded first image; a relative mip, without changing its raw data.
        virtual U32 firstTextureMip(U32 width, U32 height, U32 components, bool mips) = 0;
        // How many more textures can exist now.
        virtual U32 freeTextureSlots() = 0;

        virtual void beginUI() = 0;
        virtual void setUIBlend(const U8 factors[4]) = 0; // colour source, colour dest, alpha source, alpha dest, as LLRender::eBlendFactor
        virtual void setUIScissor(bool enabled, S32 x, S32 y, S32 width, S32 height) = 0;
        virtual void drawUI(const UIDrawData& data) = 0;
        virtual void publishUI() = 0;
    };

    // Set while the renderer runs, null otherwise.
    void setRendererLink(RendererLink* link);

    // Everything drawn between these two calls is one finished frame of UI.
    void beginUI();
    void endUI();

    // Adds to the UI being recorded. Geometry that is not UI is left out, and each
    // program this happens with is logged once.
    void drawUI(U8 mode, const F32* positions, const F32* uvs, const U32* colors, U32 count,
                const glm::mat4& modelview, const glm::mat4& projection, const glm::mat4& texture);

    // Textures, named by the name LLImageGL holds. A new image for a name replaces
    // the old one, in the same call, if its size or layout changes. With no slot free,
    // the name keeps the image it has: callers make sure of a slot first, see freeTextureSlots().
    // compressible: LLImageGL allows it, and the image was fetched (see CompressionScope);
    // the renderer may then keep it in a compressed format, which takes no sub-images.
    void setTextureImage(U32 name, S32 width, S32 height, U32 format, U32 type, const void* pixels, bool mips, bool compressible = false);

    // While one exists on a thread, images set on that thread may be compressed: only
    // LLViewerFetchedTexture::createTexture() opens one, so UI, fonts and media never are.
    class CompressionScope
    {
    public:
        CompressionScope();
        ~CompressionScope();
        static bool open();
    };
    void setTextureSubImage(U32 name, S32 x, S32 y, S32 width, S32 height, const void* pixels, S32 rowPixels);
    // Drops the given number of top mips, keeping what is left, with no decode. False
    // when the texture has no such mips, and then nothing has changed.
    bool shrinkTexture(U32 name, S32 mips);
    void setTextureWrap(U32 name, bool wrap);
    void setTextureFilter(U32 name, bool linear);

    // Where a texture is now, 0 being white. It moves when a new size replaces it.
    U32 textureSlot(U32 name);

    // Who follows slots hears of changes here, instead of asking about every texture.
    // LLImageGL reports itself when its image is replaced or deleted. The list holds
    // repeats, and pointers only meant to be compared: some may be gone by then.
    void watchImages(bool watch);
    void imageChanged(const void* image);
    void takeChangedImages(std::vector<const void*>& images);

    // What the viewer's texture budget steers by, in bytes, and the limit no image may pass.
    void getTextureMemory(U64& used, U64& limit, U64* hardLimit = nullptr);
    // Whether a new image of this size stays inside the limit.
    bool textureFits(S32 width, S32 height, S32 components, bool mips);
    U32 firstTextureMip(U32 width, U32 height, U32 components, bool mips);
    // How many more textures can exist now. Slots are a hard limit, unlike memory.
    U32 freeTextureSlots();

    // Temporary: textures created with no pixels, and images replaced with pixels.
    void getTextureCounts(U32& created, U32& blank, U32& filled, U32& subImages);
    // Temporary: textures alive now, their bytes, and how many were shrunk so far.
    void getLiveTextures(U32& count, U64& bytes, U32& shrunk);

    // For the GL table, which hands over what the viewer sets through GL.
    void genTextureNames(S32 count, U32* names);
    void deleteTextureNames(S32 count, const U32* names);
    void setBlendEnabled(bool enabled);
    void setBlendFactors(U8 colorSource, U8 colorDest, U8 alphaSource, U8 alphaDest);
    void setScissorEnabled(bool enabled);
    void setScissorRect(S32 x, S32 y, S32 width, S32 height);
}

#endif // ASR_BRIDGE_H
