/**
 * @file asmacoit.cpp
 * @brief AyaneStorm Mac OIT: order-independent transparency built only from
 *        OpenGL 4.1 blending, for macOS and every other platform.
 * @author chanayane@firestorm
 *
 * Frame structure (post-water alpha pool, main camera only):
 *
 *   depth copy   opaque depth -> private DEPTH24_STENCIL8, stencil cleared
 *   KEYS         key0 (MIN), farthest depth (MIN of -depth), stencil += 1
 *                per fragment
 *   PEEL k       k = 1..K-1: key k (MIN), stencil test count > k; the last
 *                peel also accumulates the tail's optical depth and moments
 *   merge        one fullscreen pass packs keys, moments and per-pixel
 *                precomputation into the W x 3H state texture
 *   COLOR        exact/tail weighted color, weight and optical depth (ADD)
 *   resolve      (finishFrame) normalized color over opaque * T, by
 *                dual-source blending into the screen
 *
 * Every geometry pass reuses LLDrawPoolAlpha::forwardRender() through the
 * dispatcher, with the Mac OIT program variants selected while capturing. The
 * material shaders expose their weighted-OIT hook under the AVBOIT define
 * (alpha-only early return unless avboitRasterPass == 2, then avboit_store()),
 * which the Mac OIT capture library implements, so no shared shader is edited.
 *
 * Nothing is read back to the CPU per frame. The only readback is the one-shot
 * blending self-test at allocation.
 */

#include "llviewerprecompiledheaders.h"

#include "asmacoit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "asbackgroundisolate.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "lldrawpoolalpha.h"
#include "llimagegl.h"
#include "llmaterial.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llsd.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
// ASRenderOITMode value selecting this renderer.
constexpr S32 MAC_OIT_MODE = 4;

// avboitRasterPass values, fixed by the shared material shaders' hook: every
// pass except COLOR stops after alpha.
constexpr GLint PASS_KEYS = 0;
constexpr GLint PASS_PEEL = 1;
constexpr GLint PASS_COLOR = 2;

// Rows of the merged state texture: keys, moments, precomputed factors.
constexpr U32 STATE_LAYERS = 3;

// Cleared value of every key slot; must match MACOIT_EMPTY in the shaders
// (FLT_MAX: the MIN-neutral value).
constexpr F32 EMPTY_KEY = 3.4028234e38f;

const char* const CAPTURE_LIBRARY = "deferred/asMacOITCaptureF.glsl";
const char* const MOMENTS_LIBRARY = "deferred/asMacOITMomentsF.glsl";
const char* const EMISSIVE_TERMINAL = "deferred/asMacOITEmissiveF.glsl";
const char* const PBR_GLOW_TERMINAL = "deferred/asMacOITPbrGlowF.glsl";
const char* const FULLSCREEN_VERTEX = "deferred/postDeferredNoTCV.glsl";

S32 exactLayers()
{
    // The tail's moment anchor is layer K-2, so K >= 2. Four keys keep hair
    // exact behind a thick two-face pane (doc/ayanestorm-oit-avboit-glass-
    // darkening.md); scripts/testing/macoit_reference.py measures each K.
    static LLCachedControl<S32> layers(gSavedSettings, "ASRenderMacOITExactLayers", 4);
    return llclamp(S32(layers), 2, 4);
}

// Debug mode 5 renders normally and logs a capture trace (see CaptureTrace).
constexpr S32 DEBUG_TRACE = 5;

S32 debugMode()
{
    static LLCachedControl<S32> mode(gSavedSettings, "ASRenderMacOITDebugMode", 0);
    return llclamp(S32(mode), 0, DEBUG_TRACE);
}

F32 momentBias()
{
    static LLCachedControl<F32> bias(gSavedSettings, "ASRenderMacOITMomentBias", 5.0e-7f);
    return llclamp(F32(bias), 0.f, 0.01f);
}

LLGLSLShader gMacOITAlphaProgram;
LLGLSLShader gMacOITSkinnedAlphaProgram;
LLGLSLShader gMacOITPBRAlphaProgram;
LLGLSLShader gMacOITSkinnedPBRAlphaProgram;
LLGLSLShader gMacOITFullbrightAlphaProgram;
LLGLSLShader gMacOITSkinnedFullbrightAlphaProgram;
LLGLSLShader gMacOITEmissiveProgram;
LLGLSLShader gMacOITSkinnedEmissiveProgram;
LLGLSLShader gMacOITPBRGlowProgram;
LLGLSLShader gMacOITSkinnedPBRGlowProgram;
LLGLSLShader gMacOITMaterialAlphaProgram[LLMaterial::SHADER_COUNT * 2];
// Holds only mGLTFVariants; the parent program itself is never linked.
LLGLSLShader gMacOITGLTFProgram;
LLGLSLShader gMacOITDepthCopyProgram;
LLGLSLShader gMacOITMergeProgram;
LLGLSLShader gMacOITResolveProgram;
LLGLSLShader gMacOITProbeProgram;

// Per capture program: cached uniform locations and the texture unit the
// linker assigned to macoitState (it differs per program, since each
// material program has its own sampler set). Rebuilt on every load, so a
// shader reload can never leave stale locations behind.
struct CaptureProgram
{
    LLGLSLShader* shader = nullptr;
    GLint pass = -1;
    GLint readChannel = -1;
    GLint writeChannel = -1;
    GLint momentPass = -1;
    GLint exactLayers = -1;
    GLint layerRows = -1;
    GLint depth = -1;
    GLint stateUnit = -1;
};

std::vector<CaptureProgram> sCapturePrograms;
std::unordered_map<const LLGLSLShader*, size_t> sCaptureProgramIndex;
// When no capture program reaches the last texture unit, every program's
// macoitState is pointed at it: the state texture is then bound once per
// pass, never per draw, and no material sampler can see it. Otherwise -1,
// and each program samples it from its own linker-assigned unit.
GLint sSharedStateUnit = -1;

struct Resources
{
    // RGBA32F: key0, key2, -farthest depth, tail optical depth.
    GLuint keysEven = 0;
    // RGBA32F: key1, key3, -farthest depth, tail optical depth.
    // COLOR pass: sum of weight, sum of optical depth.
    GLuint keysOdd = 0;
    // RGBA32F: tail optical depth * (z, z^2, z^3, z^4).
    // COLOR pass: sum of color * weight, sum of glow.
    GLuint moments = 0;
    // RGBA32F, W x 3H: the merged per-pixel state.
    GLuint state = 0;
    GLuint depthStencil = 0;
    GLuint depthFBO = 0;   // depth-stencil only
    GLuint keysFBO = 0;    // keysEven
    GLuint peelOddFBO = 0; // keysOdd, moments
    GLuint peelEvenFBO = 0;// keysEven, moments
    GLuint mergeFBO = 0;   // state
    GLuint colorFBO = 0;   // moments, keysOdd
    U32 width = 0;
    U32 height = 0;
    bool available = false;
};

Resources sResources;
bool sCaptureActive = false;
bool sCaptureCompleted = false;
bool sFrameReady = false;
// Set once per session if the driver fails the blending self-test.
bool sProbeFailed = false;
bool sProbed = false;
// avboitRasterPass of the geometry pass in flight.
GLint sPass = PASS_KEYS;
// The texture macoitState samples in the current pass, 0 in KEYS.
GLuint sStateTexture = 0;
// Texture units that received sStateTexture this frame, unbound afterwards so
// LLTexUnit never caches a name this module may delete.
U64 sStateUnits = 0;

bool cloneCaptureProgram(LLGLSLShader& destination, const LLGLSLShader& source,
                         const std::string& name, const char* terminal)
{
    destination.mName = name;
    destination.mFeatures = source.mFeatures;
    // Source programs expose their post-link draw features. Capture variants
    // must not attach an additional lighting fragment object at link time.
    destination.mFeatures.calculatesLighting = false;
    destination.mFeatures.hasLighting = false;
    destination.mDefines = source.mDefines;
    destination.mShaderFiles = source.mShaderFiles;
    if (terminal)
    {
        // Glow terminals replace the source's fragment shader entirely.
        destination.mShaderFiles.erase(
            std::remove_if(destination.mShaderFiles.begin(), destination.mShaderFiles.end(),
                           [](const auto& file) { return file.second == GL_FRAGMENT_SHADER; }),
            destination.mShaderFiles.end());
        destination.mShaderFiles.emplace_back(terminal, GL_FRAGMENT_SHADER);
    }
    destination.mShaderFiles.emplace_back(CAPTURE_LIBRARY, GL_FRAGMENT_SHADER);
    destination.mShaderFiles.emplace_back(MOMENTS_LIBRARY, GL_FRAGMENT_SHADER);
    destination.mShaderLevel = source.mShaderLevel;
    destination.mShaderGroup = source.mShaderGroup;
    // Selects the material shaders' weighted-OIT hook (see file comment).
    destination.addPermutation("AVBOIT", "1");
    destination.addPermutation("MACOIT", "1");
    return destination.createShader();
}

bool cloneCapturePair(LLGLSLShader& destination, LLGLSLShader& rigged_destination,
                      const LLGLSLShader& source, const std::string& name,
                      const char* terminal)
{
    if (!source.mRiggedVariant ||
        !cloneCaptureProgram(rigged_destination, *source.mRiggedVariant,
                             "Skinned " + name, terminal))
    {
        return false;
    }
    destination.mRiggedVariant = &rigged_destination;
    return cloneCaptureProgram(destination, source, name, terminal);
}

bool createFullscreenProgram(LLGLSLShader& program, const std::string& name,
                             const char* fragment, S32 shader_level,
                             const char* library = nullptr)
{
    program.mName = name;
    program.mFeatures.attachNothing = true;
    program.mShaderFiles.clear();
    program.mShaderFiles.emplace_back(FULLSCREEN_VERTEX, GL_VERTEX_SHADER);
    program.mShaderFiles.emplace_back(fragment, GL_FRAGMENT_SHADER);
    if (library)
    {
        program.mShaderFiles.emplace_back(library, GL_FRAGMENT_SHADER);
    }
    program.mShaderLevel = shader_level;
    program.clearPermutations();
    return program.createShader();
}

template <typename F>
void forEachCaptureProgram(F&& visit)
{
    visit(gMacOITAlphaProgram);
    visit(gMacOITSkinnedAlphaProgram);
    visit(gMacOITPBRAlphaProgram);
    visit(gMacOITSkinnedPBRAlphaProgram);
    visit(gMacOITFullbrightAlphaProgram);
    visit(gMacOITSkinnedFullbrightAlphaProgram);
    visit(gMacOITEmissiveProgram);
    visit(gMacOITSkinnedEmissiveProgram);
    visit(gMacOITPBRGlowProgram);
    visit(gMacOITSkinnedPBRGlowProgram);
    for (LLGLSLShader& program : gMacOITMaterialAlphaProgram)
    {
        visit(program);
    }
    for (LLGLSLShader& program : gMacOITGLTFProgram.mGLTFVariants)
    {
        visit(program);
    }
}

void indexCapturePrograms()
{
    sCapturePrograms.clear();
    sCaptureProgramIndex.clear();
    S32 max_channels = 0;
    forEachCaptureProgram([&max_channels](LLGLSLShader& program)
    {
        const GLuint object = program.mProgramObject;
        if (!object)
        {
            return;
        }
        CaptureProgram entry;
        entry.shader = &program;
        entry.pass = glGetUniformLocation(object, "avboitRasterPass");
        entry.readChannel = glGetUniformLocation(object, "macoitReadChannel");
        entry.writeChannel = glGetUniformLocation(object, "macoitWriteChannel");
        entry.momentPass = glGetUniformLocation(object, "macoitMomentPass");
        entry.exactLayers = glGetUniformLocation(object, "macoitExactLayers");
        entry.layerRows = glGetUniformLocation(object, "macoitLayerRows");
        entry.depth = glGetUniformLocation(object, "macoitDepth");
        const GLint state = glGetUniformLocation(object, "macoitState");
        if (state >= 0)
        {
            // LLGLSLShader::mapUniforms() gave every sampler its own unit.
            glGetUniformiv(object, state, &entry.stateUnit);
        }
        max_channels = llmax(max_channels, program.mActiveTextureChannels);
        sCaptureProgramIndex[&program] = sCapturePrograms.size();
        sCapturePrograms.push_back(entry);
    });

    // Linked samplers occupy units 0..channels-1, so the last unit is free in
    // every program when all of them stay below the limit.
    const S32 units = gGLManager.mNumTextureImageUnits;
    sSharedStateUnit = max_channels < units ? units - 1 : -1;
    if (sSharedStateUnit >= 0)
    {
        for (CaptureProgram& entry : sCapturePrograms)
        {
            const GLint state = glGetUniformLocation(entry.shader->mProgramObject, "macoitState");
            if (state >= 0)
            {
                glProgramUniform1i(entry.shader->mProgramObject, state, sSharedStateUnit);
                entry.stateUnit = sSharedStateUnit;
            }
        }
    }
    LL_INFOS("MacOIT") << "Mac OIT capture programs use up to " << max_channels << " of "
                       << units << " texture units; state sampled from "
                       << (sSharedStateUnit >= 0 ? "shared unit " : "per-program units")
                       << (sSharedStateUnit >= 0 ? llformat("%d", sSharedStateUnit) : "")
                       << LL_ENDL;
}

void bindStateUnit(GLint unit)
{
    if (unit >= 0 && unit < 64)
    {
        gGL.getTexUnit(unit)->bindManual(LLTexUnit::TT_TEXTURE, sStateTexture);
        sStateUnits |= U64(1) << unit;
    }
}

void releaseStateUnits()
{
    for (S32 unit = 0; sStateUnits; ++unit, sStateUnits >>= 1)
    {
        if (sStateUnits & 1)
        {
            gGL.getTexUnit(unit)->unbind(LLTexUnit::TT_TEXTURE);
        }
    }
}

// Starts a geometry pass: uploads its uniforms to every capture program and
// binds the texture macoitState samples (0 for none). glProgramUniform needs
// no bind, so this is a handful of calls per program per pass, not per draw.
// The previous pass's state texture is unbound first: it may be this pass's
// render target.
void configurePass(GLint pass, GLint read_channel, GLint write_channel, bool moment_pass,
                   GLuint state_texture)
{
    releaseStateUnits();
    sPass = pass;
    sStateTexture = state_texture;
    if (sStateTexture && sSharedStateUnit >= 0)
    {
        bindStateUnit(sSharedStateUnit);
    }

    const LLViewerCamera& camera = *LLViewerCamera::getInstance();
    const F32 near_depth = llmax(camera.getNear(), 0.0001f);
    const F32 far_depth = llmax(camera.getFar(), near_depth * 2.f);
    const F32 inverse_log_range = 1.f / std::log2(far_depth / near_depth);
    const GLint layers = exactLayers();
    const GLint rows = GLint(sResources.height);
    for (const CaptureProgram& entry : sCapturePrograms)
    {
        const GLuint object = entry.shader->mProgramObject;
        if (entry.pass >= 0) glProgramUniform1i(object, entry.pass, pass);
        if (entry.readChannel >= 0) glProgramUniform1i(object, entry.readChannel, read_channel);
        if (entry.writeChannel >= 0) glProgramUniform1i(object, entry.writeChannel, write_channel);
        if (entry.momentPass >= 0) glProgramUniform1i(object, entry.momentPass, moment_pass ? 1 : 0);
        if (entry.exactLayers >= 0) glProgramUniform1i(object, entry.exactLayers, layers);
        if (entry.layerRows >= 0) glProgramUniform1i(object, entry.layerRows, rows);
        if (entry.depth >= 0)
        {
            glProgramUniform3f(object, entry.depth, near_depth, far_depth, inverse_log_range);
        }
    }
}

// Fallback path only (no shared unit): binds the pass's state texture to the
// unit the given program samples it from. Units differ between programs and
// a unit that is ours in one program may hold a material texture in the
// next, so this runs per draw; LLTexUnit skips the GL call when the unit
// already holds the texture.
void bindState(const LLGLSLShader* shader)
{
    if (!sStateTexture || !shader || sSharedStateUnit >= 0)
    {
        return;
    }
    const auto found = sCaptureProgramIndex.find(shader);
    if (found != sCaptureProgramIndex.end())
    {
        bindStateUnit(sCapturePrograms[found->second].stateUnit);
    }
}

GLuint createTexture(GLenum format, U32 width, U32 height)
{
    GLuint texture = 0;
    LLImageGL::generateTextures(1, &texture);
    LLTexUnit* unit = gGL.getTexUnit(0);
    unit->bindManual(LLTexUnit::TT_TEXTURE, texture);
    // glTexStorage2D is OpenGL 4.2; macOS stops at 4.1.
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    unit->unbind(LLTexUnit::TT_TEXTURE);
    return texture;
}

GLuint createFramebuffer(std::initializer_list<GLuint> colors, bool depth_stencil)
{
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLenum buffers[4] = { GL_NONE, GL_NONE, GL_NONE, GL_NONE };
    GLsizei count = 0;
    for (GLuint color : colors)
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + count,
                               GL_TEXTURE_2D, color, 0);
        buffers[count] = GL_COLOR_ATTACHMENT0 + count;
        ++count;
    }
    if (depth_stencil)
    {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, sResources.depthStencil);
    }
    if (count)
    {
        glDrawBuffers(count, buffers);
    }
    else
    {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
    if (!complete)
    {
        glDeleteFramebuffers(1, &fbo);
        return 0;
    }
    return fbo;
}

void bindFramebuffer(GLuint fbo, U32 width, U32 height)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
}

void drawFullscreen()
{
    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
}

// Debug mode 5: reads the capture targets back after each stage and logs how
// many pixels each stage produced, plus the first GL error per stage. Runs at
// most once every two seconds; the readbacks stall, so it is diagnostic only.
class CaptureTrace
{
public:
    explicit CaptureTrace(U32 width, U32 height) : mWidth(width), mHeight(height)
    {
        static LLFrameTimer timer;
        mActive = debugMode() == DEBUG_TRACE && timer.getElapsedTimeF32() >= 2.f;
        if (mActive)
        {
            timer.reset();
            while (glGetError() != GL_NO_ERROR)
            {
            }
        }
    }

    ~CaptureTrace()
    {
        if (mActive)
        {
            LL_INFOS("MacOIT") << "Mac OIT trace " << mWidth << "x" << mHeight << ":"
                               << mText << LL_ENDL;
        }
    }

    bool active() const { return mActive; }

    void error(const char* stage)
    {
        const GLenum code = glGetError();
        if (code != GL_NO_ERROR)
        {
            mText += llformat(" [GL error 0x%04x after %s]", code, stage);
        }
    }

    // Opaque depth copied into the private depth-stencil target.
    void depth(GLuint fbo)
    {
        std::vector<F32> values(size_t(mWidth) * mHeight);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, mWidth, mHeight, GL_DEPTH_COMPONENT, GL_FLOAT, values.data());
        U32 below_far = 0;
        F32 lowest = 1.f;
        for (F32 d : values)
        {
            below_far += d < 1.f ? 1 : 0;
            lowest = llmin(lowest, d);
        }
        mText += llformat(" depth<1 %u (min %.6f)", below_far, lowest);
    }

    // Fragments counted by the KEYS pass stencil.
    void stencil(GLuint fbo)
    {
        std::vector<U8> stencil(size_t(mWidth) * mHeight);
        GLint alignment = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, mWidth, mHeight, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil.data());
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        U32 covered = 0;
        U32 deepest = 0;
        for (U8 count : stencil)
        {
            covered += count ? 1 : 0;
            deepest = llmax(deepest, U32(count));
        }
        mText += llformat(" stencil>0 %u (max %u)", covered, deepest);
    }

    // Pixels whose channel of color attachment `index` satisfies `test`.
    template <typename Test>
    void channel(const char* label, GLuint fbo, S32 index, S32 component, Test test)
    {
        std::vector<F32> texels(size_t(mWidth) * mHeight * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0 + index);
        glReadPixels(0, 0, mWidth, mHeight, GL_RGBA, GL_FLOAT, texels.data());
        U32 count = 0;
        for (size_t i = component; i < texels.size(); i += 4)
        {
            count += test(texels[i]) ? 1 : 0;
        }
        mText += llformat(" %s %u", label, count);
    }

private:
    U32 mWidth;
    U32 mHeight;
    bool mActive = false;
    std::string mText;
};

// Binds a peel target with its moment attachment enabled or not: only the
// last peel pass writes moments.
void bindPeelFramebuffer(GLuint fbo, GLsizei draw_buffers, U32 width, U32 height)
{
    static const GLenum buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    bindFramebuffer(fbo, width, height);
    glDrawBuffers(draw_buffers, buffers);
}
}

const char* ASMacOIT::shaderCacheRevision()
{
    return "Mac OIT shader revision v1";
}

bool ASMacOIT::supported()
{
    return gGLManager.mGLVersion >= 4.09f &&
        (gGLManager.mGLSLVersionMajor > 4 ||
         (gGLManager.mGLSLVersionMajor == 4 && gGLManager.mGLSLVersionMinor >= 10));
}

bool ASMacOIT::requested()
{
    static LLCachedControl<S32> mode(gSavedSettings, "ASRenderOITMode", -1);
    return S32(mode) == MAC_OIT_MODE && !sProbeFailed && supported();
}

bool ASMacOIT::shadersReady()
{
    return gMacOITAlphaProgram.mProgramObject &&
        gMacOITPBRAlphaProgram.mProgramObject &&
        gMacOITFullbrightAlphaProgram.mProgramObject &&
        gMacOITEmissiveProgram.mProgramObject &&
        gMacOITPBRGlowProgram.mProgramObject &&
        gMacOITDepthCopyProgram.mProgramObject &&
        gMacOITMergeProgram.mProgramObject &&
        gMacOITResolveProgram.mProgramObject &&
        !sCapturePrograms.empty();
}

void ASMacOIT::loadShaders(S32 shader_level)
{
    sCapturePrograms.clear();
    sCaptureProgramIndex.clear();
    if (!supported())
    {
        return;
    }

    bool success =
        createFullscreenProgram(gMacOITDepthCopyProgram, "Mac OIT Depth Copy",
                                "deferred/asMacOITDepthCopyF.glsl", shader_level) &&
        createFullscreenProgram(gMacOITMergeProgram, "Mac OIT Merge",
                                "deferred/asMacOITMergeF.glsl", shader_level,
                                MOMENTS_LIBRARY) &&
        createFullscreenProgram(gMacOITResolveProgram, "Mac OIT Resolve",
                                "deferred/asMacOITResolveF.glsl", shader_level) &&
        createFullscreenProgram(gMacOITProbeProgram, "Mac OIT Probe",
                                "deferred/asMacOITProbeF.glsl", shader_level);
    success = success && cloneCapturePair(
        gMacOITAlphaProgram, gMacOITSkinnedAlphaProgram, gDeferredAlphaProgram,
        "Deferred Alpha Mac OIT Shader", nullptr);
    success = success && cloneCapturePair(
        gMacOITPBRAlphaProgram, gMacOITSkinnedPBRAlphaProgram, gDeferredPBRAlphaProgram,
        "Deferred PBR Alpha Mac OIT Shader", nullptr);
    success = success && cloneCapturePair(
        gMacOITFullbrightAlphaProgram, gMacOITSkinnedFullbrightAlphaProgram,
        gDeferredFullbrightAlphaMaskAlphaProgram,
        "Deferred Fullbright Alpha Mac OIT Shader", nullptr);
    success = success && cloneCapturePair(
        gMacOITEmissiveProgram, gMacOITSkinnedEmissiveProgram, gDeferredEmissiveProgram,
        "Deferred Emissive Mac OIT Shader", EMISSIVE_TERMINAL);
    success = success && cloneCapturePair(
        gMacOITPBRGlowProgram, gMacOITSkinnedPBRGlowProgram, gPBRGlowProgram,
        "PBR Glow Mac OIT Shader", PBR_GLOW_TERMINAL);

    for (U32 i = 0; i < LLMaterial::SHADER_COUNT * 2 && success; ++i)
    {
        if ((i & 0x3u) != LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
        {
            continue;
        }
        success = cloneCaptureProgram(
            gMacOITMaterialAlphaProgram[i], gDeferredMaterialProgram[i],
            llformat("Material Mac OIT Shader %u", i), nullptr);
        if (i < LLMaterial::SHADER_COUNT)
        {
            gMacOITMaterialAlphaProgram[i].mRiggedVariant =
                &gMacOITMaterialAlphaProgram[i + LLMaterial::SHADER_COUNT];
        }
    }

    if (success)
    {
        gMacOITGLTFProgram.mName = "Mac OIT GLTF PBR Metallic Roughness Shader";
        gMacOITGLTFProgram.mGLTFVariants.resize(
            gGLTFPBRMetallicRoughnessProgram.mGLTFVariants.size());
        for (U32 i = 0; i < gGLTFPBRMetallicRoughnessProgram.mGLTFVariants.size() && success; ++i)
        {
            success = cloneCaptureProgram(
                gMacOITGLTFProgram.mGLTFVariants[i],
                gGLTFPBRMetallicRoughnessProgram.mGLTFVariants[i],
                "Mac OIT GLTF PBR Metallic Roughness Variant", nullptr);
        }
    }

    if (success)
    {
        indexCapturePrograms();
        LL_INFOS("MacOIT") << "Mac OIT shaders loaded (" << sCapturePrograms.size()
                           << " capture programs)" << LL_ENDL;
    }
    else
    {
        unloadShaders();
        LL_WARNS("MacOIT") << "Mac OIT shaders unavailable; dispatcher fallback remains active"
                           << LL_ENDL;
    }
}

void ASMacOIT::registerShaders(std::vector<LLGLSLShader*>& shader_list)
{
    shader_list.push_back(&gMacOITDepthCopyProgram);
    shader_list.push_back(&gMacOITMergeProgram);
    shader_list.push_back(&gMacOITResolveProgram);
    shader_list.push_back(&gMacOITProbeProgram);
    shader_list.push_back(&gMacOITGLTFProgram);
    shader_list.push_back(&gMacOITAlphaProgram);
    shader_list.push_back(&gMacOITSkinnedAlphaProgram);
    shader_list.push_back(&gMacOITPBRAlphaProgram);
    shader_list.push_back(&gMacOITSkinnedPBRAlphaProgram);
    shader_list.push_back(&gMacOITFullbrightAlphaProgram);
    shader_list.push_back(&gMacOITSkinnedFullbrightAlphaProgram);
    shader_list.push_back(&gMacOITEmissiveProgram);
    shader_list.push_back(&gMacOITSkinnedEmissiveProgram);
    shader_list.push_back(&gMacOITPBRGlowProgram);
    shader_list.push_back(&gMacOITSkinnedPBRGlowProgram);
    for (U32 i = 0; i < LLMaterial::SHADER_COUNT; ++i)
    {
        if ((i & 0x3u) == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
        {
            shader_list.push_back(&gMacOITMaterialAlphaProgram[i]);
        }
    }
}

void ASMacOIT::unloadShaders()
{
    sCapturePrograms.clear();
    sCaptureProgramIndex.clear();
    forEachCaptureProgram([](LLGLSLShader& program) { program.unload(); });
    gMacOITGLTFProgram.unload();
    gMacOITDepthCopyProgram.unload();
    gMacOITMergeProgram.unload();
    gMacOITResolveProgram.unload();
    gMacOITProbeProgram.unload();
}

void ASMacOIT::beginFrame()
{
    sCaptureActive = false;
    sCaptureCompleted = false;
    sFrameReady = false;
}

bool ASMacOIT::captureActive()
{
    return sCaptureActive;
}

bool ASMacOIT::captureCompleted()
{
    return sCaptureCompleted;
}

bool ASMacOIT::renderPostDeferredCapture(
    LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign,
    LLGLSLShader*& emissive_shader, LLGLSLShader*& pbr_emissive_shader)
{
    if (!requested() || !shadersReady() ||
        pool.getType() != LLDrawPool::POOL_ALPHA_POST_WATER ||
        LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender || gCubeSnapshot ||
        !gPipeline.mRT)
    {
        return false;
    }
    const U32 width = gPipeline.mRT->screen.getWidth();
    const U32 height = gPipeline.mRT->screen.getHeight();
    const GLuint opaque_depth = gPipeline.mRT->deferredScreen.getDepth();
    if (!width || !height || !opaque_depth)
    {
        return false;
    }
    if (!sProbed)
    {
        // First capture of the session, when every pipeline resource the
        // self-test draws with is known to exist.
        sProbed = true;
        if (!probeBlending())
        {
            sProbeFailed = true;
            // ASRenderOITMode is authoritative; resetting it is what returns
            // the dispatcher to Standard.
            gSavedSettings.setS32("ASRenderOITMode", 0);
            LL_WARNS("MacOIT") << "Mac OIT disabled for this session: the driver "
                                  "failed the blending self-test" << LL_ENDL;
            return false;
        }
    }
    if (!sResources.available || sResources.width != width || sResources.height != height)
    {
        allocateResources(width, height);
        if (!sResources.available)
        {
            return false;
        }
    }

    prepare(&gMacOITAlphaProgram, true, water_sign);
    prepare(&gMacOITPBRAlphaProgram, true, water_sign);
    prepare(&gMacOITFullbrightAlphaProgram, true, water_sign);
    for (LLGLSLShader& program : gMacOITMaterialAlphaProgram)
    {
        if (program.mProgramObject)
        {
            prepare(&program, true, water_sign);
        }
    }
    prepare(&gMacOITEmissiveProgram, false, water_sign);
    prepare(&gMacOITPBRGlowProgram, false, water_sign);
    emissive_shader = &gMacOITEmissiveProgram;
    pbr_emissive_shader = &gMacOITPBRGlowProgram;
    LLGLSLShader::unbind();

    // GL state for the whole capture. Blending is tracked as disabled: every
    // forwardRender() disables GL_BLEND for its scope anyway, and each capture
    // draw re-enables it untracked (configureCapturedDrawIfActive()). The
    // blend function stays ONE, ONE for every ADD target and is ignored by
    // MIN; setting it through gGL keeps LLRender's cache truthful.
    LLGLDisable no_scissor(GL_SCISSOR_TEST);
    LLGLDisable no_blend(GL_BLEND);
    LLGLEnable stencil(GL_STENCIL_TEST);
    gGL.setColorMask(true, true);
    gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
    glStencilMask(0xFF);
    sStateUnits = 0;

    const auto render_pass = [&pool]()
    {
        sCaptureActive = true;
        pool.forwardRender(true);
        pool.forwardRender(false);
        sCaptureActive = false;
    };
    CaptureTrace trace(width, height);
    const auto valid_key = [](F32 key) { return key < EMPTY_KEY; };
    const auto positive = [](F32 value) { return value > 0.f; };
    const GLfloat empty_keys[4] = { EMPTY_KEY, EMPTY_KEY, EMPTY_KEY, 0.f };
    const GLfloat zero[4] = { 0.f, 0.f, 0.f, 0.f };

    {
        LL_PROFILE_GPU_ZONE("Mac OIT depth copy");
        bindFramebuffer(sResources.depthFBO, width, height);
        const GLint zero_stencil = 0;
        glClearBufferiv(GL_STENCIL, 0, &zero_stencil);
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);
        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        static LLStaticHashedString opaque_depth_sampler("macoitOpaqueDepth");
        gMacOITDepthCopyProgram.bind();
        gMacOITDepthCopyProgram.uniform1i(opaque_depth_sampler, 0);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, opaque_depth);
        drawFullscreen();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gMacOITDepthCopyProgram.unbind();

        if (trace.active())
        {
            trace.error("depth copy");
            trace.depth(sResources.depthFBO);
        }

        // Every peel target starts empty; the tail moments start at zero.
        bindPeelFramebuffer(sResources.peelOddFBO, 2, width, height);
        glClearBufferfv(GL_COLOR, 0, empty_keys);
        glClearBufferfv(GL_COLOR, 1, zero);
    }

    const S32 layers = exactLayers();
    {
        LL_PROFILE_GPU_ZONE("Mac OIT keys");
        bindFramebuffer(sResources.keysFBO, width, height);
        glClearBufferfv(GL_COLOR, 0, empty_keys);
        glBlendEquationSeparatei(0, GL_MIN, GL_FUNC_ADD);
        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
        configurePass(PASS_KEYS, 0, 0, false, 0);
        render_pass();
        if (trace.active())
        {
            trace.error("keys");
            trace.stencil(sResources.depthFBO);
            trace.channel("key0", sResources.keysFBO, 0, 0, valid_key);
        }
    }

    for (S32 k = 1; k < layers; ++k)
    {
        LL_PROFILE_GPU_ZONE("Mac OIT peel");
        // Keys alternate between the two targets, so each pass reads the
        // previous layer from the texture it is not writing.
        const bool to_odd = (k & 1) != 0;
        const bool moment_pass = k == layers - 1;
        // keysEven keeps key0 and the farthest depth across its later peel:
        // MIN with the neutral EMPTY key and ADD of zero leave them intact.
        bindPeelFramebuffer(to_odd ? sResources.peelOddFBO : sResources.peelEvenFBO,
                            moment_pass ? 2 : 1, width, height);
        glBlendEquationSeparatei(0, GL_MIN, GL_FUNC_ADD);
        glBlendEquationi(1, GL_FUNC_ADD);
        // Pixels with at most k fragments cannot have a layer k.
        glStencilFunc(GL_LESS, k, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        configurePass(PASS_PEEL, (k - 1) >> 1, k >> 1, moment_pass,
                      ((k - 1) & 1) ? sResources.keysOdd : sResources.keysEven);
        render_pass();
        if (trace.active())
        {
            trace.error("peel");
        }
    }

    {
        LL_PROFILE_GPU_ZONE("Mac OIT merge");
        // forwardRender() ends with alpha writes masked; the state needs all
        // four channels.
        glDisable(GL_BLEND);
        gGL.setColorMask(true, true);
        bindFramebuffer(sResources.mergeFBO, width, height * STATE_LAYERS);
        static LLStaticHashedString keys_even("macoitKeysEven");
        static LLStaticHashedString keys_odd("macoitKeysOdd");
        static LLStaticHashedString moments("macoitMoments");
        static LLStaticHashedString exact_layers("macoitExactLayers");
        static LLStaticHashedString layer_rows("macoitLayerRows");
        static LLStaticHashedString moments_in_odd("macoitMomentsInOdd");
        static LLStaticHashedString moment_bias("macoitMomentBias");
        gMacOITMergeProgram.bind();
        gMacOITMergeProgram.uniform1i(keys_even, 0);
        gMacOITMergeProgram.uniform1i(keys_odd, 1);
        gMacOITMergeProgram.uniform1i(moments, 2);
        gMacOITMergeProgram.uniform1i(exact_layers, layers);
        gMacOITMergeProgram.uniform1i(layer_rows, S32(height));
        gMacOITMergeProgram.uniform1i(moments_in_odd, ((layers - 1) & 1) ? 1 : 0);
        gMacOITMergeProgram.uniform1f(moment_bias, momentBias());
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, sResources.keysEven);
        gGL.getTexUnit(1)->bindManual(LLTexUnit::TT_TEXTURE, sResources.keysOdd);
        gGL.getTexUnit(2)->bindManual(LLTexUnit::TT_TEXTURE, sResources.moments);
        drawFullscreen();
        for (S32 unit = 0; unit < 3; ++unit)
        {
            gGL.getTexUnit(unit)->unbind(LLTexUnit::TT_TEXTURE);
        }
        gMacOITMergeProgram.unbind();
        if (trace.active())
        {
            trace.error("merge");
        }
    }

    {
        LL_PROFILE_GPU_ZONE("Mac OIT color");
        // moments and keysOdd are free after the merge: reuse them as the
        // color and weight/optical-depth accumulators.
        bindFramebuffer(sResources.colorFBO, width, height);
        gGL.setColorMask(true, true);  // clears obey the color mask
        glClearBufferfv(GL_COLOR, 0, zero);
        glClearBufferfv(GL_COLOR, 1, zero);
        glBlendEquationi(0, GL_FUNC_ADD);
        glBlendEquationi(1, GL_FUNC_ADD);
        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        configurePass(PASS_COLOR, 0, 0, false, sResources.state);
        render_pass();
        if (trace.active())
        {
            trace.error("color");
            trace.channel("weight>0", sResources.colorFBO, 1, 0, positive);
            trace.channel("depth>0", sResources.colorFBO, 1, 1, positive);
            trace.channel("glow>0", sResources.colorFBO, 0, 3, positive);
        }
    }

    // Leave GL exactly as the trackers believe it is.
    sStateTexture = 0;
    releaseStateUnits();
    glDisable(GL_BLEND);
    // Not glBlendEquation(): on Windows that GL 1.2 entry point is a
    // glh_genext pointer the viewer never loads (null).
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
    glViewport(0, 0, LLRenderTarget::sCurResX, LLRenderTarget::sCurResY);
    gGL.setColorMask(true, false);

    sCaptureCompleted = true;
    sFrameReady = true;
    return true;
}

bool ASMacOIT::configureCapturedDrawIfActive(LLGLSLShader* shader)
{
    if (!sCaptureActive)
    {
        return false;
    }
    // forwardRender()'s LLGLDisable(GL_BLEND) scope disables blending on every
    // draw buffer; the per-attachment equations survive it.
    glEnable(GL_BLEND);
    // renderAlpha() ends with setSceneBlendType(BT_ALPHA), so every pass after
    // the first would otherwise blend ADD targets with SRC_ALPHA factors and,
    // for this module's zero-alpha outputs, write nothing. MIN targets ignore
    // the factors. gGL skips the call while the cache already holds ONE, ONE.
    gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
    bindState(shader);
    return true;
}

bool ASMacOIT::handleCapturedEmissives(
    LLDrawPoolAlpha& pool, bool depth_only,
    std::vector<LLDrawInfo*>& emissives,
    std::vector<LLDrawInfo*>& pbr_emissives,
    std::vector<LLDrawInfo*>& rigged_emissives,
    std::vector<LLDrawInfo*>& pbr_rigged_emissives)
{
    if (!sCaptureActive)
    {
        return false;
    }
    // Glow is weighted only in the color pass; it never forms a layer.
    if (depth_only || sPass != PASS_COLOR)
    {
        return true;
    }

    // Particle draws always carry TYPE_EMISSIVE even with zero glow; prims set
    // mHasGlow = true (llvovolume.cpp), so only known-zero particles drop.
    const auto drop_no_glow = [](std::vector<LLDrawInfo*>& draws)
    {
        draws.erase(std::remove_if(draws.begin(), draws.end(),
                                   [](LLDrawInfo* draw) { return !draw->mHasGlow; }),
                    draws.end());
    };
    drop_no_glow(emissives);
    drop_no_glow(pbr_emissives);
    drop_no_glow(rigged_emissives);
    drop_no_glow(pbr_rigged_emissives);

    // The pool's render*Emissives() leave the glow program bound, and
    // returning true skips renderAlpha()'s own rebind of the alpha program:
    // restore it, or the next group draws through the glow shader
    // (doc/ayanestorm-oit-nvidia-tdr-crash-investigation.md, defect 3).
    LLGLSLShader* const previous = LLGLSLShader::sCurBoundShaderPtr;
    bool drawn = false;
    glEnable(GL_BLEND);
    gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
    if (!emissives.empty())
    {
        bindState(&gMacOITEmissiveProgram);
        pool.renderEmissives(emissives);
        drawn = true;
    }
    if (!pbr_emissives.empty())
    {
        bindState(&gMacOITPBRGlowProgram);
        pool.renderPbrEmissives(pbr_emissives);
        drawn = true;
    }
    if (!rigged_emissives.empty())
    {
        bindState(&gMacOITSkinnedEmissiveProgram);
        pool.renderRiggedEmissives(rigged_emissives);
        drawn = true;
    }
    if (!pbr_rigged_emissives.empty())
    {
        bindState(&gMacOITSkinnedPBRGlowProgram);
        pool.renderRiggedPbrEmissives(pbr_rigged_emissives);
        drawn = true;
    }
    if (drawn && previous && LLGLSLShader::sCurBoundShaderPtr != previous)
    {
        previous->bind();
        bindState(previous);
    }
    return true;
}

void ASMacOIT::configureGLTFCapturedDraw(LLGLSLShader& shader)
{
    configureCapturedDrawIfActive(&shader);
}

bool ASMacOIT::finishFrame(LLPipeline& pipeline, LLRenderTarget& screen)
{
    if (!sFrameReady)
    {
        return false;
    }
    sFrameReady = false;

    {
        LL_PROFILE_GPU_ZONE("Mac OIT resolve");
        static LLStaticHashedString color_glow("macoitColorGlow");
        static LLStaticHashedString weight_depth("macoitWeightDepth");
        static LLStaticHashedString state("macoitState");
        static LLStaticHashedString exact_layers("macoitExactLayers");
        static LLStaticHashedString debug_mode("macoitDebugMode");

        // Self-lighting floater isolate mode: covered pixels write a near
        // depth so the later backdrop pass keeps them. Otherwise the scene
        // depth is untouched.
        const bool isolate = ASBackgroundIsolate::isActive();
        LLGLDepthTest depth(isolate ? GL_TRUE : GL_FALSE, isolate ? GL_TRUE : GL_FALSE,
                            GL_ALWAYS);
        LLGLEnable blend(GL_BLEND);
        gGL.setColorMask(true, true);
        gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
        // dst = src0 + dst * src1, with src1 = T: LLRender has no SRC1 factor,
        // so set it directly and restore the cached ONE, ONE right after.
        glBlendFunc(GL_ONE, GL_SRC1_COLOR);

        gMacOITResolveProgram.bind();
        gMacOITResolveProgram.uniform1i(color_glow, 0);
        gMacOITResolveProgram.uniform1i(weight_depth, 1);
        gMacOITResolveProgram.uniform1i(state, 2);
        gMacOITResolveProgram.uniform1i(exact_layers, exactLayers());
        const S32 mode = debugMode();
        gMacOITResolveProgram.uniform1i(debug_mode, mode == DEBUG_TRACE ? 0 : mode);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, sResources.moments);
        gGL.getTexUnit(1)->bindManual(LLTexUnit::TT_TEXTURE, sResources.keysOdd);
        gGL.getTexUnit(2)->bindManual(LLTexUnit::TT_TEXTURE, sResources.state);
        drawFullscreen();
        for (S32 unit = 0; unit < 3; ++unit)
        {
            gGL.getTexUnit(unit)->unbind(LLTexUnit::TT_TEXTURE);
        }
        gMacOITResolveProgram.unbind();

        glBlendFunc(GL_ONE, GL_ONE);
        gGL.setColorMask(true, false);
    }

    for (LLDrawPool* pool : pipeline.mPools)
    {
        if (pool->getType() == LLDrawPool::POOL_ALPHA_POST_WATER)
        {
            static_cast<LLDrawPoolAlpha*>(pool)->renderDebugAlpha();
            break;
        }
    }
    return true;
}

LLGLSLShader& ASMacOIT::gltfProgram(LLGLSLShader& ordinary_program)
{
    return sCaptureActive ? gMacOITGLTFProgram : ordinary_program;
}

LLGLSLShader* ASMacOIT::alphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gMacOITAlphaProgram : ordinary;
}

LLGLSLShader* ASMacOIT::pbrAlphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gMacOITPBRAlphaProgram : ordinary;
}

LLGLSLShader* ASMacOIT::fullbrightAlphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gMacOITFullbrightAlphaProgram : ordinary;
}

LLGLSLShader* ASMacOIT::materialAlphaShader(U32 mask, LLGLSLShader* ordinary)
{
    LLGLSLShader& program = gMacOITMaterialAlphaProgram[mask];
    return sCaptureActive && program.mProgramObject ? &program : ordinary;
}

// One-shot driver self-test: MIN blending on R32F must return an operand
// bit-exactly (the layer keys depend on it, down to keys next to the
// smallest normal float), and ADD blending on RGBA32F must accumulate.
bool ASMacOIT::probeBlending()
{
    GLuint key_target = createTexture(GL_R32F, 1, 1);
    GLuint sum_target = createTexture(GL_RGBA32F, 1, 1);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, key_target, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sum_target, 0);
    static const GLenum buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, buffers);

    bool passed = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    F32 key = 0.f;
    U32 key_bits = 0;
    F32 sum[4] = { 0.f, 0.f, 0.f, 0.f };
    const U32 high_bits = 0x407FFFFFu;  // largest key
    const U32 low_bits = 0x00800101u;   // a key just above the smallest normal
    F32 high = 0.f;
    F32 low = 0.f;
    memcpy(&high, &high_bits, sizeof(high));
    memcpy(&low, &low_bits, sizeof(low));
    if (passed)
    {
        LLGLDisable no_scissor(GL_SCISSOR_TEST);
        LLGLDepthTest no_depth(GL_FALSE, GL_FALSE);
        LLGLEnable blend(GL_BLEND);
        gGL.setColorMask(true, true);
        gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
        glViewport(0, 0, 1, 1);
        const GLfloat empty[4] = { EMPTY_KEY, EMPTY_KEY, EMPTY_KEY, EMPTY_KEY };
        const GLfloat zero[4] = { 0.f, 0.f, 0.f, 0.f };
        glClearBufferfv(GL_COLOR, 0, empty);
        glClearBufferfv(GL_COLOR, 1, zero);
        glBlendEquationi(0, GL_MIN);
        glBlendEquationi(1, GL_FUNC_ADD);
        static LLStaticHashedString value("macoitProbeValue");
        gMacOITProbeProgram.bind();
        for (F32 v : { high, low, high })
        {
            gMacOITProbeProgram.uniform4f(value, v, v, v, v);
            drawFullscreen();
        }
        gMacOITProbeProgram.unbind();
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, 1, 1, GL_RED, GL_FLOAT, &key);
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, sum);
        memcpy(&key_bits, &key, sizeof(key));
        const F32 expected = high + high + low;
        passed = key_bits == low_bits &&
            fabsf(sum[0] - expected) <= expected * 1.0e-6f &&
            fabsf(sum[3] - expected) <= expected * 1.0e-6f &&
            glGetError() == GL_NO_ERROR;
        gGL.setColorMask(true, false);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
    glViewport(0, 0, LLRenderTarget::sCurResX, LLRenderTarget::sCurResY);
    glDeleteFramebuffers(1, &fbo);
    LLImageGL::deleteTextures(1, &key_target);
    LLImageGL::deleteTextures(1, &sum_target);

    GLint draw_buffers = 0;
    GLint texture_units = 0;
    GLint texture_size = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &draw_buffers);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &texture_units);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texture_size);
    LL_INFOS("MacOIT") << "Mac OIT blending self-test " << (passed ? "passed" : "FAILED")
                       << " (MIN key bits " << std::hex << key_bits << std::dec << ", ADD " << sum[0] << "); draw buffers " << draw_buffers
                       << ", fragment texture units " << texture_units
                       << ", max texture size " << texture_size << LL_ENDL;
    return passed;
}

bool ASMacOIT::allocate(U32 width, U32 height)
{
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    if (height * STATE_LAYERS > U32(max_size))
    {
        LL_WARNS("MacOIT") << "Mac OIT state texture " << width << "x"
                           << height * STATE_LAYERS << " exceeds GL_MAX_TEXTURE_SIZE "
                           << max_size << LL_ENDL;
        return false;
    }
    while (glGetError() != GL_NO_ERROR)
    {
    }
    sResources.width = width;
    sResources.height = height;
    sResources.keysEven = createTexture(GL_RGBA32F, width, height);
    sResources.keysOdd = createTexture(GL_RGBA32F, width, height);
    sResources.moments = createTexture(GL_RGBA32F, width, height);
    sResources.state = createTexture(GL_RGBA32F, width, height * STATE_LAYERS);
    glGenRenderbuffers(1, &sResources.depthStencil);
    glBindRenderbuffer(GL_RENDERBUFFER, sResources.depthStencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    sResources.depthFBO = createFramebuffer({}, true);
    sResources.keysFBO = createFramebuffer({ sResources.keysEven }, true);
    sResources.peelOddFBO = createFramebuffer({ sResources.keysOdd, sResources.moments }, true);
    sResources.peelEvenFBO = createFramebuffer({ sResources.keysEven, sResources.moments }, true);
    sResources.mergeFBO = createFramebuffer({ sResources.state }, false);
    sResources.colorFBO = createFramebuffer({ sResources.moments, sResources.keysOdd }, true);
    return glGetError() == GL_NO_ERROR &&
        sResources.depthFBO && sResources.keysFBO && sResources.peelOddFBO &&
        sResources.peelEvenFBO && sResources.mergeFBO && sResources.colorFBO;
}

void ASMacOIT::allocateResources(U32 width, U32 height)
{
    releaseResources();
    // Until the first capture has run the blending self-test, allocation is
    // deferred to that capture.
    if (!requested() || !shadersReady() || !sProbed)
    {
        return;
    }
    sResources.available = allocate(width, height);
    if (!sResources.available)
    {
        releaseResources();
        LL_WARNS("MacOIT") << "Mac OIT resources unavailable at " << width << "x" << height
                           << "; dispatcher fallback remains active" << LL_ENDL;
    }
}

void ASMacOIT::releaseResources()
{
    for (GLuint* fbo : { &sResources.depthFBO, &sResources.keysFBO, &sResources.peelOddFBO,
                         &sResources.peelEvenFBO, &sResources.mergeFBO, &sResources.colorFBO })
    {
        if (*fbo)
        {
            glDeleteFramebuffers(1, fbo);
        }
    }
    for (GLuint* texture : { &sResources.keysEven, &sResources.keysOdd,
                             &sResources.moments, &sResources.state })
    {
        if (*texture)
        {
            LLImageGL::deleteTextures(1, texture);
        }
    }
    if (sResources.depthStencil)
    {
        glDeleteRenderbuffers(1, &sResources.depthStencil);
    }
    sResources = Resources();
    sFrameReady = false;
}

void ASMacOIT::appendDiagnostics(LLSD& info)
{
    const U64 pixels = U64(sResources.width) * sResources.height;
    // Four RGBA32F targets (the state texture holds three layers) plus
    // DEPTH24_STENCIL8.
    const U64 bytes = pixels * (16u * (3u + STATE_LAYERS) + 4u);
    info["MACOIT_AVAILABLE"] = sResources.available;
    info["MACOIT_EXACT_LAYERS"] = LLSD::Integer(exactLayers());
    info["MACOIT_WIDTH"] = LLSD::Integer(sResources.width);
    info["MACOIT_HEIGHT"] = LLSD::Integer(sResources.height);
    info["MACOIT_MB"] = LLSD::Integer(bytes / (1024ull * 1024ull));
    info["MACOIT_STATUS"] = !supported() ? "Unavailable: OpenGL 4.1 is required" :
        sProbeFailed ? "Unavailable: blending self-test failed" :
        !sResources.available ? "Unavailable or disabled; dispatcher fallback active" :
        "Available";
}
