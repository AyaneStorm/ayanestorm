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
};

// Indexed by name minus FIRST_TEXTURE_NAME, so finding a texture at draw time is
// one array read. Names are reused, so this stays as long as the most textures
// alive at once. All of this is main thread only: LLImageGL's threads are off.
constexpr U32 FIRST_TEXTURE_NAME = 1000;
std::vector<Texture> sTextures;
std::vector<U32> sFreeNames;
const Texture sNoTexture;

// GL's defaults: blending off, and factors that replace the target.
bool sBlendEnabled = false;
U8 sBlendFactors[4] = { 0, 1, 0, 1 };
bool sBlendDirty = true;

bool sScissorEnabled = false;
S32 sScissor[4] = { 0, 0, 0, 0 };
bool sScissorDirty = true;

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
    }
}

void ASR::endUI()
{
    if (sLink && sRecording)
    {
        sLink->publishUI();
    }
    sRecording = false;
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
    if (!solidColor && shader != &gUIProgram)
    {
        reportLeftOut(shader);
        return;
    }

    sendChangedState();

    const Texture& bound = findTexture(gGL.getTexUnit(0)->getCurrTexture());
    const glm::mat4 matrix = projection * modelview;

    UIDrawData data;
    data.positions = positions;
    data.uvs = uvs;
    data.colors = colors;
    data.matrix = glm::value_ptr(matrix);
    // Only gUIProgram's vertex shader reads the texture matrix.
    data.uvMatrix = (solidColor || texture == glm::mat4(1.f)) ? nullptr : glm::value_ptr(texture);
    data.count = count;
    data.slot = bound.slot;
    data.mode = mode;
    data.sampler = bound.sampler;
    data.solidColor = solidColor;
    sLink->drawUI(data);
}

void ASR::setTextureImage(U32 name, S32 width, S32 height, U32 format, U32 type, const void* pixels, bool mips)
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

    if (texture->slot == 0 || texture->width != width || texture->height != height || texture->layout != layout || texture->mips != mips)
    {
        if (texture->slot != 0)
        {
            sLink->destroyTexture(texture->slot);
        }
        texture->slot = sLink->createTexture(width, height, mips ? fullMipCount(width, height) : 1, layout);
        texture->width = width;
        texture->height = height;
        texture->layout = layout;
        texture->mips = mips;
    }

    if (pixels)
    {
        sLink->uploadTexture(texture->slot, 0, 0, width, height, layout, pixels, width);
        if (mips)
        {
            sLink->generateMips(texture->slot);
        }
    }
}

void ASR::setTextureSubImage(U32 name, S32 x, S32 y, S32 width, S32 height, const void* pixels, S32 rowPixels)
{
    const Texture& texture = findTexture(name);
    if (sLink && texture.slot != 0)
    {
        sLink->uploadTexture(texture.slot, x, y, width, height, texture.layout, pixels, rowPixels);
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
