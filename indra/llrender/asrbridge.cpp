/**
 * @file asrbridge.cpp
 * @brief See asrbridge.h.
 */
#include "linden_common.h"

#include "asrbridge.h"

#include "llglheaders.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"

#include "glm/gtc/type_ptr.hpp"
#include "glm/mat4x4.hpp"

#include <unordered_set>
#include <vector>

namespace
{

using ASR::PixelLayout;

ASR::RendererLink* sLink = nullptr;
bool sRecording = false;

// Bits of the sampler index, which is the order of asr::UISampler.
constexpr U8 SAMPLER_WRAP = 1;
constexpr U8 SAMPLER_NEAREST = 2;

struct Texture
{
    U32 slot = 0;
    S32 width = 0;
    S32 height = 0;
    PixelLayout layout = PixelLayout::RGBA;
    U8 sampler = SAMPLER_WRAP; // GL's default for a new texture
    bool mips = false;
    U8 storage = 0; // RendererLink::chooseStorage()'s, 0 when kept as its layout
};

thread_local U32 sCompressionScopes = 0;
thread_local const void* sCompressionOwner = nullptr;
thread_local const ASR::EncodedImage* sEncodedImage = nullptr;

// Indexed by name minus FIRST_TEXTURE_NAME, so finding a texture at draw time is
// one array read. Names are reused, so this stays as long as the most textures
// alive at once. All of this is main thread only: LLImageGL's threads are off.
constexpr U32 FIRST_TEXTURE_NAME = 1000;
std::vector<Texture> sTextures;
std::vector<U32> sFreeNames;
const Texture sNoTexture;

bool sWatchingImages = false;
std::vector<const void*> sChangedImages;

// GL's defaults: blending off, and factors that replace the target.
bool sBlendEnabled = false;
U8 sBlendFactors[4] = { 0, 1, 0, 1 };
bool sBlendDirty = true;

bool sScissorEnabled = false;
S32 sScissor[4] = { 0, 0, 0, 0 };
bool sScissorDirty = true;

// GL's defaults: no depth test, GL_LESS, depth writes on, no culling, back faces.
bool sDepthTest = false;
bool sDepthWrite = true;
U8 sDepthCompare = 1;
bool sCull = false;
U8 sCullFace = 1;
bool sDepthCullDirty = true;

S32 sViewport[4] = { 0, 0, 0, 0 };
bool sViewportDirty = false;

// See ASR::beginUIWorld().
glm::mat4 sWorldProjection(1.f);
bool sWorldActive = false;

// See ASR::addUniformColorProgram(): a handful, with their last diffuse colour (RGBA8).
struct UniformColorProgram
{
    const LLGLSLShader* shader;
    U32 color;
};
std::vector<UniformColorProgram> sUniformColorPrograms;

UniformColorProgram* findUniformColorProgram(const LLGLSLShader* shader)
{
    for (UniformColorProgram& program : sUniformColorPrograms)
    {
        if (program.shader == shader)
        {
            return &program;
        }
    }
    return nullptr;
}

const Texture& findTexture(U32 name)
{
    const U32 index = name - FIRST_TEXTURE_NAME;
    return index < sTextures.size() ? sTextures[index] : sNoTexture;
}

Texture* changeTexture(U32 name)
{
    const U32 index = name - FIRST_TEXTURE_NAME;
    return index < sTextures.size() ? &sTextures[index] : nullptr;
}

bool toPixelLayout(U32 format, U32 type, PixelLayout& layout)
{
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_INT_8_8_8_8_REV)
    {
        return false;
    }
    switch (format)
    {
        case GL_RGBA:            layout = PixelLayout::RGBA; return true;
        case GL_BGRA:            layout = PixelLayout::BGRA; return true;
        case GL_RGB:             layout = PixelLayout::RGB; return true;
        case GL_BGR:             layout = PixelLayout::BGR; return true;
        case GL_RED:             layout = PixelLayout::Red; return true;
        case GL_RG:              layout = PixelLayout::RG; return true;
        case GL_ALPHA:           layout = PixelLayout::Alpha; return true;
        case GL_LUMINANCE:       layout = PixelLayout::Luminance; return true;
        case GL_LUMINANCE_ALPHA: layout = PixelLayout::LuminanceAlpha; return true;
        default:                 return false;
    }
}

// Every level down to 1 pixel, as glGenerateMipmap makes.
U32 fullMipCount(S32 width, S32 height)
{
    U32 levels = 1;
    for (S32 size = llmax(width, height); size > 1; size /= 2)
    {
        ++levels;
    }
    return levels;
}

void reportLeftOut(const LLGLSLShader* shader)
{
    static const LLGLSLShader* last = nullptr;
    if (shader != last)
    {
        last = shader;
        static std::unordered_set<const LLGLSLShader*> reported;
        if (reported.insert(shader).second)
        {
            LL_INFOS("ASRenderer") << "left out of the UI: geometry drawn with " << (shader ? shader->mName : std::string("no program")) << LL_ENDL;
        }
    }
}

void sendChangedState()
{
    if (sBlendDirty)
    {
        static const U8 NO_BLENDING[4] = { 0, 1, 0, 1 };
        sLink->setUIBlend(sBlendEnabled ? sBlendFactors : NO_BLENDING);
        sBlendDirty = false;
    }
    if (sScissorDirty)
    {
        sLink->setUIScissor(sScissorEnabled, sScissor[0], sScissor[1], sScissor[2], sScissor[3]);
        sScissorDirty = false;
    }
    if (sDepthCullDirty)
    {
        sLink->setUIDepthCull(sDepthTest, sDepthWrite, sDepthCompare, sCull, sCullFace);
        sDepthCullDirty = false;
    }
    if (sViewportDirty)
    {
        sLink->setUIViewport(sViewport[0], sViewport[1], sViewport[2], sViewport[3]);
        sViewportDirty = false;
    }
}

} // namespace

void ASR::setRendererLink(RendererLink* link)
{
    sLink = link;
    sRecording = false;
}

void ASR::beginUI()
{
    if (sLink)
    {
        sLink->beginUI();
        sRecording = true;
        // A new list starts from its defaults, not from where the last one ended.
        sBlendDirty = true;
        sScissorDirty = true;
        sDepthCullDirty = true;
        sViewportDirty = sViewport[2] != 0;
    }
    sWorldActive = false;
}

void ASR::endUI()
{
    if (sLink && sRecording)
    {
        sLink->publishUI();
    }
    sRecording = false;
    sWorldActive = false;
}

void ASR::drawUI(U8 mode, const F32* positions, const F32* uvs, const U32* colors, U32 count,
                 const glm::mat4& modelview, const glm::mat4& projection, const glm::mat4& texture)
{
    if (!sRecording || LLRenderTarget::sCurFBO != 0)
    {
        return;
    }

    const LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    const bool solidColor = shader == &gSolidColorProgram;
    const UniformColorProgram* uniformColor = solidColor || shader == &gUIProgram ? nullptr : findUniformColorProgram(shader);
    if (!solidColor && shader != &gUIProgram && !uniformColor)
    {
        reportLeftOut(shader);
        return;
    }

    sendChangedState();

    const Texture& bound = findTexture(gGL.getTexUnit(0)->getCurrTexture());
    const bool world = sWorldActive && projection == sWorldProjection;
    const glm::mat4 matrix = world ? modelview : projection * modelview;

    UIDrawData data;
    data.positions = positions;
    data.uvs = uvs;
    data.colors = colors;
    data.matrix = glm::value_ptr(matrix);
    data.world = world;
    // Only gUIProgram's vertex shader reads the texture matrix.
    data.uvMatrix = (solidColor || texture == glm::mat4(1.f)) ? nullptr : glm::value_ptr(texture);
    data.count = count;
    data.slot = bound.slot;
    data.mode = mode;
    data.sampler = bound.sampler;
    data.solidColor = solidColor;
    if (uniformColor)
    {
        // Flat in its colour, untextured, as these programs draw.
        data.uniformColor = true;
        data.singleColor = uniformColor->color;
        data.slot = 0;
        data.uvMatrix = nullptr;
    }
    sLink->drawUI(data);
}

namespace
{
U32 sCreated = 0;
U32 sBlank = 0;
U32 sFilled = 0;
U32 sSubImages = 0;
U32 sShrunk = 0;

U64 pixelBytes(PixelLayout layout)
{
    switch (layout)
    {
        case PixelLayout::Red:
        case PixelLayout::Alpha:
        case PixelLayout::Luminance:
            return 1;
        case PixelLayout::RG:
        case PixelLayout::LuminanceAlpha:
            return 2;
        default:
            return 4; // GPUs have no three-channel format
    }
}
}

ASR::CompressionScope::CompressionScope(const void* owner) : mPreviousOwner(sCompressionOwner)
{
    ++sCompressionScopes;
    sCompressionOwner = owner;
}

ASR::CompressionScope::~CompressionScope()
{
    --sCompressionScopes;
    sCompressionOwner = mPreviousOwner;
}

bool ASR::CompressionScope::open()
{
    return sCompressionScopes != 0;
}

const void* ASR::CompressionScope::owner()
{
    return sCompressionOwner;
}

ASR::EncodedImageScope::EncodedImageScope(const EncodedImage& image)
{
    sEncodedImage = &image;
}

ASR::EncodedImageScope::~EncodedImageScope()
{
    sEncodedImage = nullptr;
}

const ASR::EncodedImage* ASR::EncodedImageScope::current()
{
    return sEncodedImage;
}

void ASR::setTextureImage(U32 name, S32 width, S32 height, U32 format, U32 type, const void* pixels, bool mips, bool compressible)
{
    if (!sLink)
    {
        // Would show as white with nothing to explain it.
        LL_WARNS_ONCE("ASRenderer") << "a texture arrived before the renderer started" << LL_ENDL;
        return;
    }

    Texture* texture = changeTexture(name);
    if (!texture || width <= 0 || height <= 0)
    {
        return;
    }

    PixelLayout layout;
    if (!toPixelLayout(format, type, layout))
    {
        // Compressed and floating point images are not UI. World textures get their
        // own streaming path.
        LL_WARNS_ONCE("ASRenderer") << std::hex << "texture format 0x" << format << " type 0x" << type << std::dec
                                    << " has no native path yet, it shows as white" << LL_ENDL;
        return;
    }

    // A compressed image is encoded once from whole pixels, so new pixels always make a
    // new image, which the slot shows once it is encoded.
    // Blocks from the disk cache come in place of the pixels, already encoded.
    const EncodedImage* encoded = EncodedImageScope::current();
    const U32 mipLevels = mips ? fullMipCount(width, height) : 1;
    if (encoded && (encoded->storage == 0 || encoded->mipLevels != mipLevels))
    {
        encoded = nullptr; // not this image's: the pixels, if any, are used
    }
    const U8 storage = encoded ? encoded->storage
                     : compressible && pixels && CompressionScope::open() ? sLink->chooseStorage(width, height, layout, pixels) : 0;
    bool replacing = false;
    if (texture->slot == 0 || texture->width != width || texture->height != height || texture->layout != layout || texture->mips != mips ||
        texture->storage != storage || storage != 0)
    {
        if (texture->slot == 0)
        {
            const U32 slot = encoded ? sLink->createEncodedTexture(width, height, mipLevels, layout, storage)
                                     : sLink->createTexture(width, height, mipLevels, layout, storage);
            if (slot == 0)
            {
                LL_WARNS_ONCE("ASRenderer") << "no texture slot left for a new image" << LL_ENDL;
                return;
            }
            texture->slot = slot;
        }
        else
        {
            // Another size: the slot keeps showing the old image until the new one is
            // filled, so a texture loaded sharper or smaller never flashes white, and what
            // names the slot never has to change.
            if (encoded)
            {
                sLink->replaceEncodedTexture(texture->slot, width, height, mipLevels, layout, storage);
            }
            else
            {
                sLink->replaceTexture(texture->slot, width, height, mipLevels, layout, 0, storage);
            }
            replacing = true;
        }
        ++sCreated;
        sBlank += pixels ? 0 : 1;
        texture->width = width;
        texture->height = height;
        texture->layout = layout;
        texture->mips = mips;
        texture->storage = storage;
    }

    if (encoded)
    {
        ++sFilled;
        for (U32 level = 0; level < mipLevels; ++level)
        {
            sLink->uploadBlocks(texture->slot, width, height, mipLevels, storage, level, encoded->blocks + encoded->mipOffsets[level]);
        }
        sLink->generateMips(texture->slot, nullptr); // puts them in place; nothing to store again
    }
    else if (pixels)
    {
        ++sFilled;
        sLink->uploadTexture(texture->slot, 0, 0, width, height, layout, pixels, width, storage);
        if (mips || storage != 0)
        {
            // Encodes a compressed one; the disk cache may keep its blocks.
            sLink->generateMips(texture->slot, storage != 0 ? CompressionScope::owner() : nullptr);
        }
    }
    if (replacing)
    {
        sLink->publishTexture(texture->slot); // after its pixels and mips, in order
    }
}

void ASR::setTextureSubImage(U32 name, S32 x, S32 y, S32 width, S32 height, const void* pixels, S32 rowPixels)
{
    const Texture& texture = findTexture(name);
    if (sLink && texture.slot != 0 && texture.storage != 0)
    {
        // Encoded once, whole: CompressionScope keeps images that take sub-images out.
        LL_WARNS_ONCE("ASRenderer") << "a sub-image for a compressed texture was left out" << LL_ENDL;
    }
    else if (sLink && texture.slot != 0)
    {
        ++sSubImages;
        sLink->uploadTexture(texture.slot, x, y, width, height, texture.layout, pixels, rowPixels, 0);
    }
}

bool ASR::shrinkTexture(U32 name, S32 mips)
{
    Texture* texture = changeTexture(name);
    if (!sLink || !texture || texture->slot == 0 || !texture->mips || mips <= 0)
    {
        return false;
    }

    const S32 width = llmax(texture->width >> mips, 1);
    const S32 height = llmax(texture->height >> mips, 1);
    // Made on the GPU from the image's own mips, and shown in its place once made.
    sLink->replaceTexture(texture->slot, width, height, fullMipCount(width, height), texture->layout, U32(mips), texture->storage);
    sLink->publishTexture(texture->slot);
    texture->width = width;
    texture->height = height;
    ++sShrunk;
    return true;
}

void ASR::getTextureMemory(U64& used, U64& limit, U64* hardLimit)
{
    used = 0;
    limit = 0;
    U64 hard = 0;
    if (sLink)
    {
        sLink->getTextureMemory(used, limit, hard);
    }
    if (hardLimit)
    {
        *hardLimit = hard;
    }
}

// Against the hard limit, not the steering one: an image refused here leaves its texture at
// the image it had, so this is only for what would push memory out of VRAM.
bool ASR::textureFits(S32 width, S32 height, S32 components, bool mips)
{
    U64 used = 0;
    U64 steering = 0;
    U64 limit = 0;
    getTextureMemory(used, steering, &limit);
    const U64 channels = components == 3 ? 4 : components;
    if (width <= 0 || height <= 0 || components <= 0 || used > limit) return false;
    U64 bytes = 0;
    // Count the full chain, including the long tail of rectangular images.
    for (;;)
    {
        bytes += U64(width) * height * channels;
        if (!mips || (width == 1 && height == 1)) break;
        width = llmax(width / 2, 1);
        height = llmax(height / 2, 1);
    }
    return bytes <= limit - used;
}

U32 ASR::freeTextureSlots()
{
    return sLink ? sLink->freeTextureSlots() : 0;
}

U32 ASR::firstTextureMip(U32 width, U32 height, U32 components, bool mips)
{
    return sLink ? sLink->firstTextureMip(width, height, components, mips) : 0;
}

U32 ASR::textureSlot(U32 name)
{
    return findTexture(name).slot;
}

void ASR::watchImages(bool watch)
{
    sWatchingImages = watch;
    sChangedImages.clear();
}

void ASR::imageChanged(const void* image)
{
    if (sWatchingImages)
    {
        sChangedImages.push_back(image);
    }
}

void ASR::takeChangedImages(std::vector<const void*>& images)
{
    images.clear();
    images.swap(sChangedImages);
}

void ASR::getTextureCounts(U32& created, U32& blank, U32& filled, U32& subImages)
{
    created = sCreated;
    blank = sBlank;
    filled = sFilled;
    subImages = sSubImages;
}

void ASR::getLiveTextures(U32& count, U64& bytes, U32& shrunk)
{
    count = 0;
    bytes = 0;
    shrunk = sShrunk;
    for (const Texture& texture : sTextures)
    {
        if (texture.slot != 0)
        {
            const U64 base = U64(texture.width) * texture.height * pixelBytes(texture.layout);
            ++count;
            bytes += texture.mips ? base + base / 3 : base;
        }
    }
}

void ASR::setTextureWrap(U32 name, bool wrap)
{
    if (Texture* texture = changeTexture(name))
    {
        texture->sampler = U8(wrap ? texture->sampler | SAMPLER_WRAP : texture->sampler & ~SAMPLER_WRAP);
    }
}

void ASR::setTextureFilter(U32 name, bool linear)
{
    if (Texture* texture = changeTexture(name))
    {
        texture->sampler = U8(linear ? texture->sampler & ~SAMPLER_NEAREST : texture->sampler | SAMPLER_NEAREST);
    }
}

void ASR::genTextureNames(S32 count, U32* names)
{
    for (S32 i = 0; i < count; ++i)
    {
        if (!sFreeNames.empty())
        {
            names[i] = sFreeNames.back();
            sFreeNames.pop_back();
        }
        else
        {
            names[i] = FIRST_TEXTURE_NAME + U32(sTextures.size());
            sTextures.emplace_back();
        }
    }
}

void ASR::moveTexture(U32 from, U32 to)
{
    Texture* source = changeTexture(from);
    Texture* target = changeTexture(to);
    if (!source || !target || from == to || source->slot == 0 || target->slot != 0)
    {
        return;
    }
    *target = *source;
    source->slot = 0; // deleting the old name no longer takes the slot with it
}

void ASR::deleteTextureNames(S32 count, const U32* names)
{
    for (S32 i = 0; i < count; ++i)
    {
        if (Texture* texture = changeTexture(names[i]))
        {
            if (sLink && texture->slot != 0)
            {
                sLink->destroyTexture(texture->slot);
            }
            *texture = Texture();
            sFreeNames.push_back(names[i]);
        }
    }
}

void ASR::setBlendEnabled(bool enabled)
{
    sBlendEnabled = enabled;
    sBlendDirty = true;
}

void ASR::setBlendFactors(U8 colorSource, U8 colorDest, U8 alphaSource, U8 alphaDest)
{
    sBlendFactors[0] = colorSource;
    sBlendFactors[1] = colorDest;
    sBlendFactors[2] = alphaSource;
    sBlendFactors[3] = alphaDest;
    sBlendDirty = true;
}

void ASR::setScissorEnabled(bool enabled)
{
    sScissorEnabled = enabled;
    sScissorDirty = true;
}

void ASR::setScissorRect(S32 x, S32 y, S32 width, S32 height)
{
    sScissor[0] = x;
    sScissor[1] = y;
    sScissor[2] = width;
    sScissor[3] = height;
    sScissorDirty = true;
}

void ASR::setDepthTestEnabled(bool enabled)
{
    sDepthTest = enabled;
    sDepthCullDirty = true;
}

void ASR::setDepthCompare(U8 compare)
{
    sDepthCompare = compare;
    sDepthCullDirty = true;
}

void ASR::setDepthWrite(bool enabled)
{
    sDepthWrite = enabled;
    sDepthCullDirty = true;
}

void ASR::setCullEnabled(bool enabled)
{
    sCull = enabled;
    sDepthCullDirty = true;
}

void ASR::setCullFace(U8 face)
{
    sCullFace = face;
    sDepthCullDirty = true;
}

void ASR::setViewport(S32 x, S32 y, S32 width, S32 height)
{
    sViewport[0] = x;
    sViewport[1] = y;
    sViewport[2] = width;
    sViewport[3] = height;
    sViewportDirty = true;
}

// In order with the draws, as glClear is.
void ASR::clearDepth()
{
    if (sLink && sRecording)
    {
        sLink->clearUIDepth();
    }
}

void ASR::beginUIWorld()
{
    if (sLink && sRecording)
    {
        sWorldProjection = gGL.getProjectionMatrix(); // what LLRender hands drawUI()
        sWorldActive = true;
        sLink->beginUIWorld();
    }
}

void ASR::addUniformColorProgram(const LLGLSLShader* shader)
{
    if (!findUniformColorProgram(shader))
    {
        sUniformColorPrograms.push_back({ shader, 0xFFFFFFFF });
    }
}

void ASR::setDiffuseColor(const LLGLSLShader* shader, const F32 color[4])
{
    if (UniformColorProgram* program = findUniformColorProgram(shader))
    {
        U32 packed = 0;
        for (U32 i = 0; i < 4; ++i)
        {
            packed |= U32(llclamp(color[i], 0.f, 1.f) * 255.f + 0.5f) << (i * 8);
        }
        program->color = packed;
    }
}

void ASR::markUIHud()
{
    if (sLink && sRecording)
    {
        sLink->markUIHud();
    }
}
