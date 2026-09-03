/**
 * @file fsavboit.cpp
 * @brief Approximate adaptive voxel-based OIT resolve.
 * @author chanayane@firestorm
 */

#include "llviewerprecompiledheaders.h"

#include "fsavboit.h"

// AVBOIT depends on compute shaders and SSBO/image-load-store bindings that
// macOS's capped OpenGL 4.1 does not provide. Compile it out entirely on
// Darwin and fall back to inert stubs so every call site (pipeline.cpp,
// llviewershadermgr.cpp, etc.) keeps linking without platform-specific edits.
#if LL_DARWIN

FSAVBOIT::Resources FSAVBOIT::sResources;
S32 FSAVBOIT::sDirectRasterPass = -1;
bool FSAVBOIT::sDirectFrameReady = false;
bool FSAVBOIT::sCaptureActive = false;
bool FSAVBOIT::sCaptureCompleted = false;

const char* FSAVBOIT::shaderCacheRevision() { return "avboit-unsupported"; }
bool FSAVBOIT::supported() { return false; }
bool FSAVBOIT::requested() { return false; }
bool FSAVBOIT::available() { return false; }
void FSAVBOIT::selectVirtualDomain() {}
void FSAVBOIT::loadShaders(S32 shader_level) {}
void FSAVBOIT::registerShaders(std::vector<LLGLSLShader*>& shader_list) {}
void FSAVBOIT::unloadShaders() {}
bool FSAVBOIT::shadersReady() { return false; }
void FSAVBOIT::beginFrame() {}
bool FSAVBOIT::captureActive() { return false; }
bool FSAVBOIT::captureCompleted() { return false; }
bool FSAVBOIT::renderPostDeferredCapture(
    LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign,
    LLGLSLShader*& emissive_shader, LLGLSLShader*& pbr_emissive_shader)
{
    return false;
}
bool FSAVBOIT::configureCapturedDrawIfActive(LLGLSLShader* shader) { return false; }
bool FSAVBOIT::handleCapturedEmissives(
    LLDrawPoolAlpha& pool, bool depth_only,
    std::vector<LLDrawInfo*>& emissives,
    std::vector<LLDrawInfo*>& pbr_emissives,
    std::vector<LLDrawInfo*>& rigged_emissives,
    std::vector<LLDrawInfo*>& pbr_rigged_emissives)
{
    return false;
}
void FSAVBOIT::configureGLTFCapturedDraw(LLGLSLShader& shader) {}
bool FSAVBOIT::finishFrame(LLPipeline& pipeline, LLRenderTarget& screen) { return false; }
LLGLSLShader& FSAVBOIT::gltfProgram(LLGLSLShader& ordinary_program) { return ordinary_program; }
LLGLSLShader* FSAVBOIT::alphaShader(LLGLSLShader* ordinary) { return ordinary; }
LLGLSLShader* FSAVBOIT::pbrAlphaShader(LLGLSLShader* ordinary) { return ordinary; }
LLGLSLShader* FSAVBOIT::fullbrightAlphaShader(LLGLSLShader* ordinary) { return ordinary; }
LLGLSLShader* FSAVBOIT::materialAlphaShader(U32 mask, LLGLSLShader* ordinary) { return ordinary; }
LLGLSLShader* FSAVBOIT::emissiveShader() { return nullptr; }
LLGLSLShader* FSAVBOIT::pbrGlowShader() { return nullptr; }
bool FSAVBOIT::allocateVolume(U32 width, U32 height) { return false; }
void FSAVBOIT::allocateResources(U32 width, U32 height) {}
void FSAVBOIT::releaseResources() {}
void FSAVBOIT::appendDiagnostics(LLSD& info) {}
bool FSAVBOIT::beginDirectFrame(LLRenderTarget& screen) { return false; }
void FSAVBOIT::beginDirectRasterPass(S32 pass) {}
void FSAVBOIT::configureDirectRasterShader(LLGLSLShader* shader) {}
void FSAVBOIT::rasterizeConservativeBounds() {}
void FSAVBOIT::finishDirectOccupancy() {}
void FSAVBOIT::beginPass1() {}
void FSAVBOIT::finishDirectExtinction() {}
void FSAVBOIT::finishDirectColorRaster() {}
bool FSAVBOIT::finishDirectFrame(LLRenderTarget& screen) { return false; }
bool FSAVBOIT::directFrameReady() { return false; }

#else // !LL_DARWIN

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include "asbackgroundisolate.h"
#include "llenvironment.h"
#include "gltfscenemanager.h"
#include "llglslshader.h"
#include "lldrawpoolalpha.h"
#include "llrendertarget.h"
#include "llsd.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvieweroctree.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
constexpr U32 AVBOIT_SCALE = 8;
// Pass 1 (extinction accumulation) samples this many points per axis per
// 8x8 cell instead of one, so a cell's stored extinction is the block's
// average rather than whichever single strand or garment layer happened to
// land on one sample point -- the single-sample choice is otherwise what a
// fine per-tile depth range turns into a visible 8px block/moire pattern
// (round 3, see doc/ayanestorm-oit-avboit-hair-flicker-regression-todo.md).
// 8 is full resolution (one sample per pixel, equivalent to the reverted A8);
// 4 is a quarter of that cost (16 samples/cell vs 64) as a starting point.
constexpr U32 AVBOIT_PASS1_SUBSAMPLE = 4;
constexpr U32 AVBOIT_SLICES = 128;
// The scratch extinction volume is always allocated for the widest supported
// layout (two 16-bit lanes per word). The presentation's four-8-bit-lane layout
// needs only half of those slices, so RenderAVBOITWideExtinction can be
// switched at runtime without reallocating the volume.
constexpr U32 AVBOIT_PACKED_SLICES = AVBOIT_SLICES / 2;
// Virtual depth-slice domain. The presentation's reference configuration is
// 8192, which corresponds to about 8.9 mm of depth resolution at two metres on a
// 128 metre draw distance. Second Life clothing layers are routinely a fraction
// of a millimetre apart, so the reference domain merges them before compaction
// can order them. Resolution scales linearly with the domain size, and the
// domain costs only two U32 buffers, so a high-resolution domain is affordable:
// 1048576 slices reach about 0.07 mm at two metres for 8 MB.
//
// The baseline domain remains available as a fallback for drivers that cannot
// support the high-resolution path. See avboitVirtualSlices().
// 65536 is an eightfold increase over the reference domain, reaching about
// 1.1 mm at two metres. It is deliberately the largest domain the existing
// single-workgroup prefix scan can absorb without restructuring: the scan
// becomes 256 serial iterations per thread instead of 32. Reaching 0.07 mm
// needs 1048576, which requires replacing that scan with a multi-pass
// per-workgroup scan first.
constexpr U32 AVBOIT_VIRTUAL_SLICES_BASELINE = 8192;
constexpr U32 AVBOIT_VIRTUAL_SLICES_HIGH = 65536;

// Selected once per session so buffer allocation and shader defines can
// never disagree about the domain size.
U32 sVirtualSlices = AVBOIT_VIRTUAL_SLICES_BASELINE;

U32 avboitVirtualSlices()
{
    return sVirtualSlices;
}

// Largest power-of-two divider the compaction search may use. It must be able
// to reduce the virtual domain to the 128-slice physical budget, so it scales
// with the domain: 8192 needs 6, 65536 needs 9. A cap below this pins the
// search short of any fitting candidate and compaction never fits.
U32 avboitMaxDivider()
{
    U32 divider = 0;
    while ((avboitVirtualSlices() >> divider) > AVBOIT_SLICES)
    {
        ++divider;
    }
    return divider;
}

bool wideExtinction()
{
    static LLCachedControl<bool> wide_extinction(
        gSavedSettings, "RenderAVBOITWideExtinction", true);
    return wide_extinction;
}

// Per-tile depth ranging. The depth curve is otherwise shared by the entire
// frame, so the slice spacing every pixel receives is set by the distinct
// depths present anywhere on screen: one distant surface coarsens the spacing
// for close clothing layers as well. Ranging each screen tile to the depth
// actually occupied within it spends the 128 physical slices where the geometry
// is, which is what separates layers a fraction of a millimetre apart.
bool tileRange()
{
    // A5 revised (Option A): fed by rasterizeConservativeBounds()'s
    // exact-proxy pass, which runs full-resolution over all static and
    // rigged alpha geometry every frame -- back on by default.
    static LLCachedControl<bool> tile_range(
        gSavedSettings, "RenderAVBOITTileRange", true);
    return tile_range;
}

S32 debugMode()
{
    static LLCachedControl<S32> debug_mode(
        gSavedSettings, "RenderAVBOITDebugMode", 0);
    // A9 names debug modes 16/17 (front-key diagnostics) but they are not
    // implemented -- the resolve compute shader is already at GL's 8-image-
    // unit limit and both spare slots (6/7) are format-mismatched or
    // already spoken for; see doc/ayanestorm-oit-performance-audit-plan.md
    // A9's "Diagnostics" section and the AVBOIT hair-flicker-regression-todo
    // doc's A9 implementation notes for the binding conflict.
    return llclamp(S32(debug_mode), 0, 15);
}

// A9: per-pixel exact front-two-layer key. Without it, a hair strand or
// sheer garment layer's weight comes from the volume's per-8x8-cell summed
// extinction, which mixes in whatever else shares that cell's slices --
// the sheer-over-sheer and hair-lighter-than-vanilla bugs. With it, the two
// nearest distinct-depth fragments at a pixel get an exact source-over
// weight (see doc/ayanestorm-oit-performance-audit-plan.md A9's proof);
// only the third and deeper layers still fall back to the volume. Default
// on so the fix is live by default; exposed as a live A/B toggle since the
// plan's design keeps both code paths for exactly that comparison.
bool frontLayers()
{
    static LLCachedControl<bool> front_layers(
        gSavedSettings, "RenderAVBOITFrontLayers", true);
    return front_layers;
}

// Self-occlusion bias in virtual slices, clamped to the range the sampling
// code can represent. Zero disables the bias.
F32 samplingBias()
{
    static LLCachedControl<F32> sampling_bias(
        gSavedSettings, "RenderAVBOITSamplingBias", 1.f);
    return llclamp(F32(sampling_bias), 0.f, 8.f);
}

// Linearization factor for the presentation's log depth curve
//
//     z(x) = log2(x/a + 1) / log2(b/a + 1) * n
//
// where b is the far plane, n the slice count, and a the linearization factor.
// The slide "VBOIT : DEPTH DISTRIBUTION / LOG CURVE" gives the reference
// configuration as n = 8192/2^d and a = 16384/2^d against a far plane
// b = 32000. The key relationship is that a is proportional to the far plane:
// a/b = 16384/32000 = 0.512, so at the finest divider the distribution is
// almost uniform. That is what the companion slide means by "adjust slice count
// together with linearization factor - keeps spatial slice resolution close to
// camera ~constant". Precision is then changed by the divider, which scales n
// and a together, not by reshaping the curve at a fixed n.
//
// The previous implementation instead solved a from a requested near-plane
// thickness. That decoupled a from the far plane and could drive it arbitrarily
// small: a requested 0.001 m over a 128 m far plane produced a = 1.95, an a/b
// ratio of 0.015 against the reference 0.512, a curve roughly thirty times more
// logarithmic than the paper's finest setting. Near-plane precision improved
// only by starving everything beyond a few metres, which is why lowering the
// setting traded the close layers against distant transparency instead of
// improving both.
//
// This restores the specified proportionality. RenderAVBOITMinimumSliceThickness
// is retained as a bounded adjustment of the reference ratio rather than an
// unbounded solve, so the curve cannot leave the family the presentation uses.
F32 fittedLinearization(F32 far_depth)
{
    // Reference ratio from the slide's a = 16384, b = 32000.
    constexpr F64 reference_ratio = 16384.0 / 32000.0;
    const F64 far_value = llmax(static_cast<F64>(far_depth), 0.0001);
    const F64 requested = llmax(
        static_cast<F64>(
            gSavedSettings.getF32("RenderAVBOITMinimumSliceThickness")),
        0.00001);
    static F64 cached_far = -1.0;
    static F64 cached_requested = -1.0;
    static F32 cached_result = 1.f;
    if (far_value == cached_far && requested == cached_requested)
    {
        return cached_result;
    }

    // The reference curve's near-plane slice thickness for this far plane.
    const F64 reference_a = far_value * reference_ratio;
    const F64 reference_thickness =
        reference_a * std::log1p(far_value / reference_a) /
        static_cast<F64>(avboitVirtualSlices());

    // Scale a by the requested departure from that reference, bounded to one
    // order of magnitude either side. Smaller a concentrates precision near the
    // camera; the bound keeps the curve inside the presentation's family and
    // prevents the far plane from collapsing into the last slices.
    const F64 scale = llclamp(requested / reference_thickness, 0.1, 10.0);
    cached_far = far_value;
    cached_requested = requested;
    cached_result = static_cast<F32>(llmax(reference_a * scale, 0.0001));
    return cached_result;
}

void allocateAccumulationTexture(GLuint& texture, GLenum format,
                                  U32 width, U32 height)
{
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void configureAccumulationBlend()
{
    for (GLuint attachment = 1; attachment <= 3; ++attachment)
    {
        glEnablei(GL_BLEND, attachment);
        glBlendEquationi(attachment, GL_FUNC_ADD);
        glBlendFunci(attachment, GL_ONE, GL_ONE);
    }
}

S32 directTransmittanceTextureUnit()
{
    return llmax(0, gGLManager.mNumTextureImageUnits - 1);
}

S32 directOpaqueDepthTextureUnit()
{
    return llmax(0, gGLManager.mNumTextureImageUnits - 2);
}

LLGLSLShader gAVBOITVolumeProgram;
LLGLSLShader gAVBOITResolveProgram;
LLGLSLShader gAVBOITEarlyDepthProgram;
// <AS:Chanayane> Self-lighting floater isolate-background mode: writes
// depth for AVBOIT-resolved pixels only when isolate mode is active. See
// avboitIsolateDepthF.glsl and FSAVBOIT::finishDirectFrame().
LLGLSLShader gAVBOITIsolateDepthProgram;
// </AS:Chanayane>
// A2: per-cell farthest opaque depth, baked once before pass 1 so its
// hardware early-depth test rejects against the correct 8x8 block instead
// of a single full-res pixel. See avboitCellDepthF.glsl and
// FSAVBOIT::finishDirectOccupancy().
LLGLSLShader gAVBOITCellDepthProgram;
LLGLSLShader gAVBOITBoundsProgram;
LLGLSLShader gAVBOITSkinnedBoundsProgram;
LLGLSLShader gAVBOITGLTFProgram;
LLGLSLShader gAVBOITAlphaProgram;
LLGLSLShader gAVBOITSkinnedAlphaProgram;
LLGLSLShader gAVBOITPBRAlphaProgram;
LLGLSLShader gAVBOITSkinnedPBRAlphaProgram;
LLGLSLShader gAVBOITFullbrightAlphaProgram;
LLGLSLShader gAVBOITSkinnedFullbrightAlphaProgram;
LLGLSLShader gAVBOITEmissiveProgram;
LLGLSLShader gAVBOITSkinnedEmissiveProgram;
LLGLSLShader gAVBOITPBRGlowProgram;
LLGLSLShader gAVBOITSkinnedPBRGlowProgram;
LLGLSLShader gAVBOITMaterialAlphaProgram[LLMaterial::SHADER_COUNT * 2];
LLRenderTarget gAVBOITOpaqueTarget;
LLRenderTarget gAVBOITPrepassTarget;
// A2: volume-resolution depth-only target holding each cell's baked
// farthest opaque depth, bound during pass 1 so early_fragment_tests tests
// against the correct per-cell value. Distinct from gAVBOITPrepassTarget,
// which is an unrelated pass-0 null color sink.
LLRenderTarget gAVBOITCellDepthTarget;

bool cloneCaptureShader(LLGLSLShader& destination, const LLGLSLShader& source,
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
    if (std::strcmp(terminal, "deferred/avboitEmissiveF.glsl") == 0 ||
        std::strcmp(terminal, "deferred/avboitPbrGlowF.glsl") == 0)
    {
        destination.mShaderFiles.erase(
            std::remove_if(
                destination.mShaderFiles.begin(), destination.mShaderFiles.end(),
                [](const auto& shader_file)
                {
                    return shader_file.second == GL_FRAGMENT_SHADER;
                }),
            destination.mShaderFiles.end());
    }
    destination.mShaderFiles.emplace_back(terminal, GL_FRAGMENT_SHADER);
    destination.mShaderLevel = source.mShaderLevel;
    destination.mShaderGroup = source.mShaderGroup;
    destination.addPermutation("AVBOIT", "1");
    destination.addPermutation(
        "AVBOIT_VIRTUAL_SLICES", llformat("%u", avboitVirtualSlices()));
    destination.addPermutation(
        "AVBOIT_MAX_DIVIDER_VALUE", llformat("%u", avboitMaxDivider()));
    return destination.createShader();
}

bool cloneCapturePair(LLGLSLShader& destination, LLGLSLShader& rigged_destination,
                      const LLGLSLShader& source, const std::string& name,
                      const char* terminal)
{
    if (!source.mRiggedVariant ||
        !cloneCaptureShader(rigged_destination, *source.mRiggedVariant,
                            "Skinned " + name, terminal))
    {
        return false;
    }
    destination.mRiggedVariant = &rigged_destination;
    return cloneCaptureShader(destination, source, name, terminal);
}

void unloadMaterialShaders()
{
    gAVBOITGLTFProgram.unload();
    gAVBOITAlphaProgram.unload();
    gAVBOITSkinnedAlphaProgram.unload();
    gAVBOITPBRAlphaProgram.unload();
    gAVBOITSkinnedPBRAlphaProgram.unload();
    gAVBOITFullbrightAlphaProgram.unload();
    gAVBOITSkinnedFullbrightAlphaProgram.unload();
    gAVBOITEmissiveProgram.unload();
    gAVBOITSkinnedEmissiveProgram.unload();
    gAVBOITPBRGlowProgram.unload();
    gAVBOITSkinnedPBRGlowProgram.unload();
    for (LLGLSLShader& shader : gAVBOITMaterialAlphaProgram)
    {
        shader.unload();
    }
}
}

FSAVBOIT::Resources FSAVBOIT::sResources;
S32 FSAVBOIT::sDirectRasterPass = -1;
bool FSAVBOIT::sDirectFrameReady = false;
bool FSAVBOIT::sCaptureActive = false;
bool FSAVBOIT::sCaptureCompleted = false;

const char* FSAVBOIT::shaderCacheRevision()
{
    return "AVBOIT shader revision v135";
}

bool FSAVBOIT::supported()
{
    return gGLManager.mGLVersion >= 4.29f &&
        (gGLManager.mGLSLVersionMajor > 4 ||
         (gGLManager.mGLSLVersionMajor == 4 && gGLManager.mGLSLVersionMinor >= 30));
}

// Select the virtual depth domain for this session. The high-resolution domain
// is what allows sub-millimetre layer separation; the baseline reproduces the
// presentation's reference configuration and is the fallback.
//
// The high-resolution path does not depend on any extension beyond the 4.3
// baseline: it replaces the single-workgroup shared-memory prefix scan with a
// multi-pass scan over shader storage, so the domain is bounded by buffer size
// rather than by GL_MAX_COMPUTE_SHARED_MEMORY_SIZE. It is gated on a driver
// reporting OpenGL 4.6 and on sufficient reported video memory purely as a
// conservative measure, because the deep scan issues more dispatches and the
// domain costs two buffers of four bytes per slice.
void FSAVBOIT::selectVirtualDomain()
{
    const bool requested_high =
        gSavedSettings.getBOOL("RenderAVBOITHighDepthResolution");
    const bool driver_capable = gGLManager.mGLVersion >= 4.59f;
    // Two U32 domain buffers plus the Z-bin table; require headroom well beyond
    // that before opting in.
    const bool memory_capable = gGLManager.mVRAM >= 4096;

    sVirtualSlices = (requested_high && driver_capable && memory_capable) ?
        AVBOIT_VIRTUAL_SLICES_HIGH : AVBOIT_VIRTUAL_SLICES_BASELINE;

    static U32 logged_slices = 0;
    if (logged_slices != sVirtualSlices)
    {
        logged_slices = sVirtualSlices;
        LL_INFOS("AVBOIT") << "AVBOIT virtual depth domain " << sVirtualSlices
                           << " slices (requested high: " << requested_high
                           << ", GL " << gGLManager.mGLVersion
                           << ", VRAM " << gGLManager.mVRAM << " MB)"
                           << LL_ENDL;
    }
}

bool FSAVBOIT::requested()
{
    return gSavedSettings.getBOOL("RenderAVBOIT") && supported();
}

bool FSAVBOIT::available()
{
    return requested() && sResources.available &&
        gAVBOITVolumeProgram.mProgramObject && gAVBOITResolveProgram.mProgramObject;
}

void FSAVBOIT::loadShaders(S32 shader_level)
{
    if (!supported())
    {
        return;
    }

    // The domain must be selected before any shader is compiled: every AVBOIT
    // stage receives it as a compile-time define so the buffers, the depth
    // curve, the Z-bin table, and the prefix scan cannot disagree.
    selectVirtualDomain();

    gAVBOITVolumeProgram.mName = "AVBOIT Volume Compute";
    gAVBOITVolumeProgram.mFeatures.attachNothing = true;
    gAVBOITVolumeProgram.mShaderFiles.clear();
    gAVBOITVolumeProgram.mShaderFiles.emplace_back("deferred/avboitVolumeC.glsl", GL_COMPUTE_SHADER);
    gAVBOITVolumeProgram.mShaderLevel = shader_level;
    gAVBOITVolumeProgram.clearPermutations();
    gAVBOITVolumeProgram.addPermutation(
        "AVBOIT_VIRTUAL_SLICES", llformat("%u", avboitVirtualSlices()));
    gAVBOITVolumeProgram.addPermutation(
        "AVBOIT_MAX_DIVIDER_VALUE", llformat("%u", avboitMaxDivider()));
    gAVBOITVolumeProgram.addPermutation("AVBOIT_BUILD", "1");

    gAVBOITResolveProgram.mName = "AVBOIT Resolve Compute";
    gAVBOITResolveProgram.mFeatures.attachNothing = true;
    gAVBOITResolveProgram.mShaderFiles.clear();
    gAVBOITResolveProgram.mShaderFiles.emplace_back("deferred/avboitVolumeC.glsl", GL_COMPUTE_SHADER);
    gAVBOITResolveProgram.mShaderLevel = shader_level;
    gAVBOITResolveProgram.clearPermutations();
    gAVBOITResolveProgram.addPermutation(
        "AVBOIT_VIRTUAL_SLICES", llformat("%u", avboitVirtualSlices()));
    gAVBOITResolveProgram.addPermutation(
        "AVBOIT_MAX_DIVIDER_VALUE", llformat("%u", avboitMaxDivider()));
    gAVBOITResolveProgram.addPermutation("AVBOIT_RESOLVE", "1");

    gAVBOITEarlyDepthProgram.mName = "AVBOIT Early Depth";
    gAVBOITEarlyDepthProgram.mFeatures.attachNothing = true;
    gAVBOITEarlyDepthProgram.mShaderFiles.clear();
    gAVBOITEarlyDepthProgram.mShaderFiles.emplace_back(
        "deferred/avboitEarlyDepthV.glsl", GL_VERTEX_SHADER);
    gAVBOITEarlyDepthProgram.mShaderFiles.emplace_back(
        "deferred/avboitEarlyDepthF.glsl", GL_FRAGMENT_SHADER);
    gAVBOITEarlyDepthProgram.mShaderLevel = shader_level;
    gAVBOITEarlyDepthProgram.clearPermutations();
    gAVBOITEarlyDepthProgram.addPermutation(
        "AVBOIT_VIRTUAL_SLICES", llformat("%u", avboitVirtualSlices()));
    gAVBOITEarlyDepthProgram.addPermutation(
        "AVBOIT_MAX_DIVIDER_VALUE", llformat("%u", avboitMaxDivider()));

    // <AS:Chanayane> Self-lighting floater isolate-background mode: see
    // gAVBOITIsolateDepthProgram's declaration above and finishDirectFrame()
    // below. No AVBOIT-specific permutations needed -- it just samples the
    // plain 2D coverage textures via ordinary texelFetch.
    gAVBOITIsolateDepthProgram.mName = "AVBOIT Isolate Depth";
    gAVBOITIsolateDepthProgram.mFeatures.attachNothing = true;
    gAVBOITIsolateDepthProgram.mShaderFiles.clear();
    gAVBOITIsolateDepthProgram.mShaderFiles.emplace_back(
        "deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    gAVBOITIsolateDepthProgram.mShaderFiles.emplace_back(
        "deferred/avboitIsolateDepthF.glsl", GL_FRAGMENT_SHADER);
    gAVBOITIsolateDepthProgram.mShaderLevel = shader_level;
    gAVBOITIsolateDepthProgram.clearPermutations();
    // </AS:Chanayane>

    // A2: bakes each volume cell's farthest opaque depth once, ahead of
    // pass 1, so pass 1's hardware depth test rejects against the correct
    // 8x8 block. See avboitCellDepthF.glsl.
    gAVBOITCellDepthProgram.mName = "AVBOIT Cell Depth";
    gAVBOITCellDepthProgram.mFeatures.attachNothing = true;
    gAVBOITCellDepthProgram.mShaderFiles.clear();
    gAVBOITCellDepthProgram.mShaderFiles.emplace_back(
        "deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    gAVBOITCellDepthProgram.mShaderFiles.emplace_back(
        "deferred/avboitCellDepthF.glsl", GL_FRAGMENT_SHADER);
    gAVBOITCellDepthProgram.mShaderLevel = shader_level;
    gAVBOITCellDepthProgram.clearPermutations();

    gAVBOITBoundsProgram.mName = "AVBOIT Conservative Bounds";
    gAVBOITBoundsProgram.mFeatures.attachNothing = true;
    gAVBOITBoundsProgram.mShaderFiles.clear();
    gAVBOITBoundsProgram.mShaderFiles.emplace_back(
        "deferred/avboitBoundsV.glsl", GL_VERTEX_SHADER);
    gAVBOITBoundsProgram.mShaderFiles.emplace_back(
        "deferred/avboitBoundsF.glsl", GL_FRAGMENT_SHADER);
    gAVBOITBoundsProgram.mShaderLevel = shader_level;
    gAVBOITBoundsProgram.clearPermutations();
    gAVBOITBoundsProgram.addPermutation(
        "AVBOIT_VIRTUAL_SLICES", llformat("%u", avboitVirtualSlices()));
    gAVBOITBoundsProgram.addPermutation(
        "AVBOIT_MAX_DIVIDER_VALUE", llformat("%u", avboitMaxDivider()));
    gAVBOITBoundsProgram.addPermutation("AVBOIT", "1");

    gAVBOITSkinnedBoundsProgram.mName =
        "AVBOIT Skinned Conservative Bounds";
    gAVBOITSkinnedBoundsProgram.mFeatures.attachNothing = false;
    gAVBOITSkinnedBoundsProgram.mFeatures.hasObjectSkinning = true;
    gAVBOITSkinnedBoundsProgram.mShaderFiles =
        gAVBOITBoundsProgram.mShaderFiles;
    gAVBOITSkinnedBoundsProgram.mShaderLevel = shader_level;
    gAVBOITSkinnedBoundsProgram.clearPermutations();
    gAVBOITSkinnedBoundsProgram.addPermutation(
        "AVBOIT_VIRTUAL_SLICES", llformat("%u", avboitVirtualSlices()));
    gAVBOITSkinnedBoundsProgram.addPermutation(
        "AVBOIT_MAX_DIVIDER_VALUE", llformat("%u", avboitMaxDivider()));
    gAVBOITSkinnedBoundsProgram.addPermutation("AVBOIT", "1");
    gAVBOITSkinnedBoundsProgram.addPermutation("HAS_SKIN", "1");

    bool success = gAVBOITVolumeProgram.createShader() &&
        gAVBOITResolveProgram.createShader() &&
        gAVBOITEarlyDepthProgram.createShader() &&
        gAVBOITIsolateDepthProgram.createShader() &&
        gAVBOITCellDepthProgram.createShader() &&
        gAVBOITBoundsProgram.createShader() &&
        gAVBOITSkinnedBoundsProgram.createShader();
    success = success && cloneCapturePair(
        gAVBOITAlphaProgram, gAVBOITSkinnedAlphaProgram,
        gDeferredAlphaProgram, "Deferred Alpha AVBOIT Shader",
        "deferred/avboitCaptureF.glsl");
    success = success && cloneCapturePair(
        gAVBOITPBRAlphaProgram, gAVBOITSkinnedPBRAlphaProgram,
        gDeferredPBRAlphaProgram, "Deferred PBR Alpha AVBOIT Shader",
        "deferred/avboitCaptureF.glsl");
    success = success && cloneCapturePair(
        gAVBOITFullbrightAlphaProgram, gAVBOITSkinnedFullbrightAlphaProgram,
        gDeferredFullbrightAlphaMaskAlphaProgram,
        "Deferred Fullbright Alpha AVBOIT Shader",
        "deferred/avboitCaptureF.glsl");
    success = success && cloneCapturePair(
        gAVBOITEmissiveProgram, gAVBOITSkinnedEmissiveProgram,
        gDeferredEmissiveProgram, "Deferred Emissive AVBOIT Shader",
        "deferred/avboitEmissiveF.glsl");
    success = success && cloneCapturePair(
        gAVBOITPBRGlowProgram, gAVBOITSkinnedPBRGlowProgram,
        gPBRGlowProgram, "PBR Glow AVBOIT Shader",
        "deferred/avboitPbrGlowF.glsl");

    for (U32 i = 0; i < LLMaterial::SHADER_COUNT * 2 && success; ++i)
    {
        if ((i & 0x3u) != LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
        {
            continue;
        }
        success = cloneCaptureShader(
            gAVBOITMaterialAlphaProgram[i], gDeferredMaterialProgram[i],
            llformat("Material AVBOIT Shader %u", i),
            "deferred/avboitCaptureF.glsl");
        if (i < LLMaterial::SHADER_COUNT)
        {
            gAVBOITMaterialAlphaProgram[i].mRiggedVariant =
                &gAVBOITMaterialAlphaProgram[i + LLMaterial::SHADER_COUNT];
        }
    }

    if (success)
    {
        gAVBOITGLTFProgram.mName = "AVBOIT GLTF PBR Metallic Roughness Shader";
        gAVBOITGLTFProgram.mFeatures = gGLTFPBRMetallicRoughnessProgram.mFeatures;
        gAVBOITGLTFProgram.mDefines = gGLTFPBRMetallicRoughnessProgram.mDefines;
        gAVBOITGLTFProgram.mShaderFiles =
            gGLTFPBRMetallicRoughnessProgram.mShaderFiles;
        gAVBOITGLTFProgram.mShaderFiles.emplace_back(
            "deferred/avboitCaptureF.glsl", GL_FRAGMENT_SHADER);
        gAVBOITGLTFProgram.mShaderLevel =
            gGLTFPBRMetallicRoughnessProgram.mShaderLevel;
        gAVBOITGLTFProgram.mShaderGroup =
            gGLTFPBRMetallicRoughnessProgram.mShaderGroup;
        gAVBOITGLTFProgram.addPermutation("AVBOIT", "1");
        gAVBOITGLTFProgram.addPermutation(
            "AVBOIT_VIRTUAL_SLICES", llformat("%u", avboitVirtualSlices()));
    gAVBOITGLTFProgram.addPermutation(
        "AVBOIT_MAX_DIVIDER_VALUE", llformat("%u", avboitMaxDivider()));
        gAVBOITGLTFProgram.mGLTFVariants.resize(
            gGLTFPBRMetallicRoughnessProgram.mGLTFVariants.size());
        for (U32 i = 0;
             i < gGLTFPBRMetallicRoughnessProgram.mGLTFVariants.size() && success;
             ++i)
        {
            success = cloneCaptureShader(
                gAVBOITGLTFProgram.mGLTFVariants[i],
                gGLTFPBRMetallicRoughnessProgram.mGLTFVariants[i],
                "AVBOIT GLTF PBR Metallic Roughness Variant",
                "deferred/avboitCaptureF.glsl");
        }
    }

    if (!success)
    {
        unloadShaders();
        LL_WARNS("AVBOIT") << "AVBOIT shaders unavailable; dispatcher fallback remains active"
                            << LL_ENDL;
    }
}

void FSAVBOIT::registerShaders(std::vector<LLGLSLShader*>& shader_list)
{
    shader_list.push_back(&gAVBOITVolumeProgram);
    shader_list.push_back(&gAVBOITResolveProgram);
    shader_list.push_back(&gAVBOITEarlyDepthProgram);
    shader_list.push_back(&gAVBOITIsolateDepthProgram);
    shader_list.push_back(&gAVBOITCellDepthProgram);
    shader_list.push_back(&gAVBOITBoundsProgram);
    shader_list.push_back(&gAVBOITSkinnedBoundsProgram);
    shader_list.push_back(&gAVBOITGLTFProgram);
    shader_list.push_back(&gAVBOITAlphaProgram);
    shader_list.push_back(&gAVBOITSkinnedAlphaProgram);
    shader_list.push_back(&gAVBOITPBRAlphaProgram);
    shader_list.push_back(&gAVBOITSkinnedPBRAlphaProgram);
    shader_list.push_back(&gAVBOITFullbrightAlphaProgram);
    shader_list.push_back(&gAVBOITSkinnedFullbrightAlphaProgram);
    shader_list.push_back(&gAVBOITEmissiveProgram);
    shader_list.push_back(&gAVBOITSkinnedEmissiveProgram);
    shader_list.push_back(&gAVBOITPBRGlowProgram);
    shader_list.push_back(&gAVBOITSkinnedPBRGlowProgram);
    for (U32 i = 0; i < LLMaterial::SHADER_COUNT; ++i)
    {
        if ((i & 0x3u) == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
        {
            shader_list.push_back(&gAVBOITMaterialAlphaProgram[i]);
        }
    }
}

void FSAVBOIT::unloadShaders()
{
    gAVBOITVolumeProgram.unload();
    gAVBOITResolveProgram.unload();
    gAVBOITEarlyDepthProgram.unload();
    gAVBOITIsolateDepthProgram.unload();
    gAVBOITCellDepthProgram.unload();
    gAVBOITBoundsProgram.unload();
    gAVBOITSkinnedBoundsProgram.unload();
    unloadMaterialShaders();
}

bool FSAVBOIT::shadersReady()
{
    return gAVBOITVolumeProgram.mProgramObject &&
        gAVBOITResolveProgram.mProgramObject &&
        gAVBOITEarlyDepthProgram.mProgramObject &&
        gAVBOITBoundsProgram.mProgramObject &&
        gAVBOITSkinnedBoundsProgram.mProgramObject &&
        gAVBOITAlphaProgram.mProgramObject &&
        gAVBOITPBRAlphaProgram.mProgramObject &&
        gAVBOITFullbrightAlphaProgram.mProgramObject &&
        gAVBOITEmissiveProgram.mProgramObject &&
        gAVBOITPBRGlowProgram.mProgramObject;
}

void FSAVBOIT::beginFrame()
{
    // Mode-transition invalidation is centralized in the neutral dispatcher.
    sCaptureActive = false;
    sCaptureCompleted = false;
    sDirectRasterPass = -1;
    sDirectFrameReady = false;
}

bool FSAVBOIT::captureActive()
{
    return sCaptureActive;
}

bool FSAVBOIT::captureCompleted()
{
    return sCaptureCompleted;
}

bool FSAVBOIT::renderPostDeferredCapture(
    LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign,
    LLGLSLShader*& emissive_shader, LLGLSLShader*& pbr_emissive_shader)
{
    if (!requested() || !shadersReady() ||
        pool.getType() != LLDrawPool::POOL_ALPHA_POST_WATER ||
        LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender ||
        gCubeSnapshot || !beginDirectFrame(gPipeline.mRT->screen))
    {
        return false;
    }

    prepare(&gAVBOITAlphaProgram, true, water_sign);
    prepare(&gAVBOITPBRAlphaProgram, true, water_sign);
    prepare(&gAVBOITFullbrightAlphaProgram, true, water_sign);
    for (LLGLSLShader& shader : gAVBOITMaterialAlphaProgram)
    {
        if (shader.mProgramObject)
        {
            prepare(&shader, true, water_sign);
        }
    }
    prepare(&gAVBOITEmissiveProgram, false, water_sign);
    prepare(&gAVBOITPBRGlowProgram, false, water_sign);
    emissive_shader = emissiveShader();
    pbr_emissive_shader = pbrGlowShader();
    LLGLSLShader::unbind();

    const auto render_pass = [&pool](bool include_static)
    {
        // A3: configure every AVBOIT program's per-pass uniforms once here
        // instead of via configureCapturedDrawIfActive()/
        // configureGLTFCapturedDraw() on every draw. lldrawpoolalpha.cpp
        // resolves LLGLSLShader::mRiggedVariant to the actual bound program
        // for a rigged draw (see the `target_shader = target_shader->
        // mRiggedVariant` sites), so both halves of every base/rigged pair
        // need their own call -- configuring only the base object would
        // leave every rigged draw silently falling back to whatever stale
        // uniforms that program object last had.
        configureDirectRasterShader(&gAVBOITAlphaProgram);
        configureDirectRasterShader(&gAVBOITSkinnedAlphaProgram);
        configureDirectRasterShader(&gAVBOITPBRAlphaProgram);
        configureDirectRasterShader(&gAVBOITSkinnedPBRAlphaProgram);
        configureDirectRasterShader(&gAVBOITFullbrightAlphaProgram);
        configureDirectRasterShader(&gAVBOITSkinnedFullbrightAlphaProgram);
        for (LLGLSLShader& shader : gAVBOITMaterialAlphaProgram)
        {
            if (shader.mProgramObject)
            {
                configureDirectRasterShader(&shader);
            }
        }
        for (LLGLSLShader& variant : gAVBOITGLTFProgram.mGLTFVariants)
        {
            if (variant.mProgramObject)
            {
                configureDirectRasterShader(&variant);
            }
        }
        configureDirectRasterShader(&gAVBOITEmissiveProgram);
        configureDirectRasterShader(&gAVBOITSkinnedEmissiveProgram);
        configureDirectRasterShader(&gAVBOITPBRGlowProgram);
        configureDirectRasterShader(&gAVBOITSkinnedPBRGlowProgram);
        sCaptureActive = true;
        pool.forwardRender(true);
        if (include_static)
        {
            pool.forwardRender(false);
        }
        sCaptureActive = false;
    };

    gGL.setColorMask(false, false);
    {
        LL_PROFILE_GPU_ZONE("AVBOIT occupancy raster");
        rasterizeConservativeBounds();
        // Per-tile depth ranging is fed by rasterizeConservativeBounds()'s
        // exact-proxy pass (avboitBoundsF.glsl's avboit_reduce_tile_range()
        // call), which already runs full-resolution over all alpha
        // geometry every frame -- no extra pass needed. Two attempts to
        // feed it from a material-tested occupancy pass instead (this
        // block, briefly) both left tile-shaped corruption, root-caused to
        // two unrelated bugs in the per-tile range's consumers, not the
        // feed pass; see doc/ayanestorm-oit-avboit-hair-flicker-regression-
        // todo.md. Debug mode 6 still runs this pass, purely as a
        // proxy-vs-material occupancy comparison (avboit_compare_proxy_
        // coverage()), unrelated to feeding the range.
        const bool compare_static_proxy = debugMode() == 6;
        if (compare_static_proxy)
        {
            render_pass(true);
        }
        else
        {
            // GLTF scene geometry is traversed outside the alpha spatial-group
            // draw maps used by rasterizeConservativeBounds(). Give it the
            // same material-tested occupancy pass before building the warp.
            LLGLDepthTest depth_test(GL_TRUE, GL_FALSE, GL_LEQUAL);
            LLGLDisable blend(GL_BLEND);
            sCaptureActive = true;
            LL::GLTFSceneManager::instance().render(false, false);
            LL::GLTFSceneManager::instance().render(false, true);
            LL::GLTFSceneManager::instance().render(false, false, true);
            LL::GLTFSceneManager::instance().render(false, true, true);
            sCaptureActive = false;
        }
    }
    {
        LL_PROFILE_GPU_ZONE("AVBOIT depth warp and sparse clear");
        finishDirectOccupancy();
    }
    {
        // A9: full-resolution front-key pass. Must run before
        // finishDirectExtinction()'s conservative early-depth-tile raster
        // (dispatched from inside the following extinction-integration
        // block below), otherwise a tile could reject a fragment that is a
        // legitimate second layer. gAVBOITOpaqueTarget is already the
        // current target here (finishDirectOccupancy() leaves it bound,
        // see its trailing comment); pass 3 draws into it directly, same as
        // pass 0's occupancy raster does.
        LL_PROFILE_GPU_ZONE("AVBOIT front key raster");
        beginDirectRasterPass(3);
        render_pass(true);
        {
            // GLTF scene geometry is traversed outside the alpha spatial-
            // group draw maps render_pass() covers -- see the identical
            // block for pass 0's occupancy raster above.
            LLGLDepthTest depth_test(GL_TRUE, GL_FALSE, GL_LEQUAL);
            LLGLDisable blend(GL_BLEND);
            sCaptureActive = true;
            LL::GLTFSceneManager::instance().render(false, false);
            LL::GLTFSceneManager::instance().render(false, true);
            LL::GLTFSceneManager::instance().render(false, false, true);
            LL::GLTFSceneManager::instance().render(false, true, true);
            sCaptureActive = false;
        }
        beginPass1();
    }
    {
        LL_PROFILE_GPU_ZONE("AVBOIT extinction raster");
        render_pass(true);
    }
    {
        LL_PROFILE_GPU_ZONE("AVBOIT extinction integration");
        finishDirectExtinction();
    }
    {
        LL_PROFILE_GPU_ZONE("AVBOIT weighted color raster");
        render_pass(true);
    }
    finishDirectColorRaster();
    gGL.setColorMask(true, true);
    sCaptureCompleted = true;
    return true;
}

bool FSAVBOIT::configureCapturedDrawIfActive(LLGLSLShader* shader)
{
    if (!sCaptureActive)
    {
        return false;
    }
    // A3: per-pass uniforms (avboitViewport, avboitDepthRange, etc.) are
    // configured once per pass in renderPostDeferredCapture()'s render_pass
    // lambda instead of on every draw; oitGlow is now a shader-side literal
    // (see avboitCaptureF.glsl). LLDrawPoolAlpha disables ordinary
    // framebuffer blending for capture, and that LLGLDisable scope is per
    // forwardRender() call (i.e. per pass) rather than per draw, so the
    // independent additive accumulation blend state must still be
    // re-applied here on every draw in pass 2 -- see A3 in
    // doc/ayanestorm-oit-performance-audit-plan.md for why a per-pass-only
    // fix for this specific piece was rejected as fragile.
    if (shader && sDirectRasterPass == 2)
    {
        configureAccumulationBlend();
    }
    return true;
}

bool FSAVBOIT::handleCapturedEmissives(
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
    // Pass 1 (extinction raster) never stores glow: avboit_store_glow()
    // returns immediately for it. Skip the draws entirely instead of paying
    // for vertex transform + texture fetch just to hit that return. A9's
    // pass 3 (front key) has no glow store at all -- it only needs alpha,
    // via the shared shaders' own early-return for that pass -- so skip it
    // there too.
    if (!depth_only && sDirectRasterPass != 1 && sDirectRasterPass != 3)
    {
        if (sDirectRasterPass == 2)
        {
            configureAccumulationBlend();
        }
        if (!emissives.empty()) pool.renderEmissives(emissives);
        if (!pbr_emissives.empty()) pool.renderPbrEmissives(pbr_emissives);
        if (!rigged_emissives.empty()) pool.renderRiggedEmissives(rigged_emissives);
        if (!pbr_rigged_emissives.empty())
            pool.renderRiggedPbrEmissives(pbr_rigged_emissives);
    }
    return true;
}

void FSAVBOIT::configureGLTFCapturedDraw(LLGLSLShader& shader)
{
    // A3: per-pass uniforms are now configured once per pass in
    // renderPostDeferredCapture()'s render_pass lambda (which loops
    // gAVBOITGLTFProgram.mGLTFVariants), and oitGlow is a shader-side
    // literal (see avboitCaptureF.glsl). Nothing left to do per draw here;
    // this function is kept (rather than removed) as the dispatcher's
    // established per-GLTF-draw hook, matching configureCapturedDrawIfActive().
}

bool FSAVBOIT::finishFrame(LLPipeline& pipeline, LLRenderTarget& screen)
{
    if (!directFrameReady() || !finishDirectFrame(screen))
    {
        return false;
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

LLGLSLShader& FSAVBOIT::gltfProgram(LLGLSLShader& ordinary_program)
{
    return sCaptureActive ? gAVBOITGLTFProgram : ordinary_program;
}

LLGLSLShader* FSAVBOIT::alphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gAVBOITAlphaProgram : ordinary;
}

LLGLSLShader* FSAVBOIT::pbrAlphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gAVBOITPBRAlphaProgram : ordinary;
}

LLGLSLShader* FSAVBOIT::fullbrightAlphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gAVBOITFullbrightAlphaProgram : ordinary;
}

LLGLSLShader* FSAVBOIT::materialAlphaShader(U32 mask, LLGLSLShader* ordinary)
{
    LLGLSLShader& shader = gAVBOITMaterialAlphaProgram[mask];
    return sCaptureActive && shader.mProgramObject ? &shader : ordinary;
}

LLGLSLShader* FSAVBOIT::emissiveShader()
{
    return &gAVBOITEmissiveProgram;
}

LLGLSLShader* FSAVBOIT::pbrGlowShader()
{
    return &gAVBOITPBRGlowProgram;
}

bool FSAVBOIT::allocateVolume(U32 width, U32 height)
{
    // Judge this allocation independently of stale errors from earlier GL
    // work; failures below are still observed by the final error check.
    while (glGetError() != GL_NO_ERROR)
    {
    }
    sResources.viewportWidth = width;
    sResources.viewportHeight = height;
    sResources.volumeWidth = (width + AVBOIT_SCALE - 1u) / AVBOIT_SCALE;
    sResources.volumeHeight = (height + AVBOIT_SCALE - 1u) / AVBOIT_SCALE;

    glGenTextures(1, &sResources.extinction);
    glBindTexture(GL_TEXTURE_3D, sResources.extinction);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_R32UI,
                   sResources.volumeWidth, sResources.volumeHeight,
                   AVBOIT_PACKED_SLICES);

    glGenTextures(1, &sResources.transmittance);
    glBindTexture(GL_TEXTURE_3D, sResources.transmittance);
    // The integrated transmittance volume is the entire ordering weight for
    // blended geometry, so its precision bounds how well the approximate
    // weight can match the exact aggregate extinction used for opaque
    // geometry. R8 cannot represent the wide layout's 1/65536 effective-zero
    // endpoint at all, and quantizes the sheer range that viewer clothing
    // occupies to 1/255 steps. Revision v55 used R16F here and was reported
    // visually good; v56 returned it to R8 for storage conformance and the
    // corruption returned with it.
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_R16F,
                   sResources.volumeWidth, sResources.volumeHeight, AVBOIT_SLICES);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_3D, 0);

    glGenTextures(1, &sResources.zeroTransmittanceDepth);
    glBindTexture(GL_TEXTURE_2D, sResources.zeroTransmittanceDepth);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI,
                   sResources.volumeWidth, sResources.volumeHeight);

    glGenTextures(1, &sResources.extinctionOverflowDepth);
    glBindTexture(GL_TEXTURE_2D, sResources.extinctionOverflowDepth);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI,
                   sResources.volumeWidth, sResources.volumeHeight);

    glBindTexture(GL_TEXTURE_2D, 0);

    const U64 domain_bytes =
        static_cast<U64>(avboitVirtualSlices()) * sizeof(U32);

    glGenBuffers(1, &sResources.occupancy);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.occupancy);
    glBufferData(GL_SHADER_STORAGE_BUFFER, domain_bytes,
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sResources.warpScan);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.warpScan);
    glBufferData(GL_SHADER_STORAGE_BUFFER, domain_bytes,
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sResources.warp);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.warp);
    glBufferData(GL_SHADER_STORAGE_BUFFER, domain_bytes,
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sResources.tileOccupancy);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.tileOccupancy);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<U64>(sResources.volumeWidth) * sResources.volumeHeight *
                     (AVBOIT_SLICES / 32u) * sizeof(U32),
                 nullptr, GL_DYNAMIC_DRAW);

    const U32 tile_count =
        ((width + 15u) / 16u) * ((height + 15u) / 16u);
    const U64 work_words = 8u + AVBOIT_SLICES +
        static_cast<U64>(sResources.volumeWidth) * sResources.volumeHeight +
        static_cast<U64>(tile_count) * 4u +
        static_cast<U64>(sResources.volumeWidth) *
            sResources.volumeHeight * 5u +
        // Per-tile depth range: minimum and maximum depth key per 16x16 tile.
        static_cast<U64>(tile_count) * 2u;
    glGenBuffers(1, &sResources.work);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.work);
    glBufferData(GL_SHADER_STORAGE_BUFFER, work_words * sizeof(U32),
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sResources.diagnostics);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.diagnostics);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 16u * sizeof(U32),
                 nullptr, GL_DYNAMIC_DRAW);

    allocateAccumulationTexture(sResources.accumulatedColorGlow,
                                GL_RGBA16F, width, height);
    allocateAccumulationTexture(sResources.accumulatedWeight,
                                GL_R16F, width, height);
    allocateAccumulationTexture(sResources.accumulatedExtinction,
                                GL_R16F, width, height);
    // A9: full-resolution per-pixel front key. Cleared to 0xffffffff (no
    // key) at the start of every frame, not here -- this only sizes storage.
    allocateAccumulationTexture(sResources.frontKey0, GL_R32UI, width, height);
    allocateAccumulationTexture(sResources.frontKey1, GL_R32UI, width, height);
    // FBO for the glClearTexImage (GL 4.4) fallback path -- see
    // beginDirectRasterPass()'s pass-3 clear. Built even when the driver
    // supports glClearTexImage so a mid-session driver/context change (rare,
    // but selectVirtualDomain()-style capability checks elsewhere in this
    // file treat that as possible) still has a working fallback.
    glGenFramebuffers(1, &sResources.frontKeyFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, sResources.frontKeyFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, sResources.frontKey0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, sResources.frontKey1, 0);
    const bool front_key_fbo_complete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return glGetError() == GL_NO_ERROR && front_key_fbo_complete &&
        // A7: color is never sampled (the resolve reads screen's own texel
        // directly now) and no AVBOIT raster pass writes fragment color to
        // this target's attachment 0 either (the shared shaders' AVBOIT path
        // never declares frag_color), so a single 8-bit channel is enough to
        // keep the FBO complete for the depth test the raster passes need.
        gAVBOITOpaqueTarget.allocate(width, height, GL_R8, true) &&
        gAVBOITPrepassTarget.allocate(
            sResources.volumeWidth, sResources.volumeHeight, GL_RGBA8) &&
        // A2: depth-only, scaled by the pass-1 subsample factor (round 3) so
        // pass 1's early_fragment_tests culls at that finer granularity
        // instead of one farthest-depth sample per whole 8x8 cell. Color is
        // never read; only gl_FragDepth from avboitCellDepthF.glsl matters.
        gAVBOITCellDepthTarget.allocate(
            sResources.volumeWidth * AVBOIT_PASS1_SUBSAMPLE,
            sResources.volumeHeight * AVBOIT_PASS1_SUBSAMPLE, GL_R8, true);
}

void FSAVBOIT::allocateResources(U32 width, U32 height)
{
    releaseResources();
    if (requested())
    {
        sResources.available = allocateVolume(width, height);
        if (!sResources.available)
        {
            releaseResources();
        }
    }
}

void FSAVBOIT::releaseResources()
{
    if (sResources.extinction) glDeleteTextures(1, &sResources.extinction);
    if (sResources.transmittance) glDeleteTextures(1, &sResources.transmittance);
    if (sResources.zeroTransmittanceDepth)
        glDeleteTextures(1, &sResources.zeroTransmittanceDepth);
    if (sResources.extinctionOverflowDepth)
        glDeleteTextures(1, &sResources.extinctionOverflowDepth);
    if (sResources.occupancy) glDeleteBuffers(1, &sResources.occupancy);
    if (sResources.warp) glDeleteBuffers(1, &sResources.warp);
    if (sResources.warpScan) glDeleteBuffers(1, &sResources.warpScan);
    if (sResources.tileOccupancy) glDeleteBuffers(1, &sResources.tileOccupancy);
    if (sResources.work) glDeleteBuffers(1, &sResources.work);
    if (sResources.diagnostics) glDeleteBuffers(1, &sResources.diagnostics);
    if (sResources.accumulatedColorGlow)
        glDeleteTextures(1, &sResources.accumulatedColorGlow);
    if (sResources.accumulatedWeight)
        glDeleteTextures(1, &sResources.accumulatedWeight);
    if (sResources.accumulatedExtinction)
        glDeleteTextures(1, &sResources.accumulatedExtinction);
    if (sResources.frontKey0) glDeleteTextures(1, &sResources.frontKey0);
    if (sResources.frontKey1) glDeleteTextures(1, &sResources.frontKey1);
    if (sResources.frontKeyFBO)
        glDeleteFramebuffers(1, &sResources.frontKeyFBO);
    gAVBOITOpaqueTarget.release();
    gAVBOITPrepassTarget.release();
    gAVBOITCellDepthTarget.release();
    sDirectRasterPass = -1;
    sDirectFrameReady = false;
    sResources = Resources();
}

void FSAVBOIT::appendDiagnostics(LLSD& info)
{
    info["AVBOIT_AVAILABLE"] = available();
    info["AVBOIT_VOLUME_WIDTH"] = LLSD::Integer(sResources.volumeWidth);
    info["AVBOIT_VOLUME_HEIGHT"] = LLSD::Integer(sResources.volumeHeight);
    info["AVBOIT_SLICES"] = LLSD::Integer(AVBOIT_SLICES);
    info["AVBOIT_PACKED_EXTINCTION_MB"] = LLSD::Integer(
        (static_cast<U64>(sResources.volumeWidth) * sResources.volumeHeight *
         AVBOIT_PACKED_SLICES * sizeof(U32)) / (1024ull * 1024ull));
    // Two bytes per voxel: the transmittance volume is R16F, not R8.
    info["AVBOIT_TRANSMITTANCE_MB"] = LLSD::Integer(
        (static_cast<U64>(sResources.volumeWidth) * sResources.volumeHeight *
         AVBOIT_SLICES * 2ull) / (1024ull * 1024ull));
    info["AVBOIT_VIRTUAL_SLICES"] = LLSD::Integer(avboitVirtualSlices());
    info["AVBOIT_EXTINCTION_LANE_BITS"] = LLSD::Integer(
        wideExtinction() ? 16 : 8);
    info["AVBOIT_DIRECT_RASTER"] = sDirectRasterPass >= 0 || sDirectFrameReady;
    info["AVBOIT_ACCUMULATION_MB"] = LLSD::Integer(
        (static_cast<U64>(sResources.viewportWidth) * sResources.viewportHeight *
         12ull) / (1024ull * 1024ull));
    info["AVBOIT_STATUS"] = !supported() ? "Unavailable: OpenGL 4.3 is required" :
        !available() ? "Unavailable or disabled; dispatcher fallback active" :
        "Available";
}

bool FSAVBOIT::beginDirectFrame(LLRenderTarget& screen)
{
    const U32 width = screen.getWidth();
    const U32 height = screen.getHeight();
    if (width == 0u || height == 0u)
    {
        return false;
    }
    // screen shares deferredScreen's depth attachment but does not own the
    // texture name, so get the sampled depth from its actual owner.
    const GLuint opaque_depth = gPipeline.mRT ?
        gPipeline.mRT->deferredScreen.getDepth() : 0;
    if (requested() && (!sResources.available ||
        sResources.viewportWidth != width || sResources.viewportHeight != height))
    {
        const GLint previous_fbo = LLRenderTarget::sCurFBO;
        allocateResources(width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);
    }
    if (!available() || !sResources.accumulatedColorGlow ||
        !sResources.accumulatedWeight || !sResources.accumulatedExtinction ||
        !sResources.frontKey0 || !sResources.frontKey1 ||
        !sResources.work ||
        !opaque_depth ||
        !gAVBOITOpaqueTarget.isComplete() ||
        !gAVBOITPrepassTarget.isComplete() ||
        !gAVBOITCellDepthTarget.isComplete())
    {
        return false;
    }

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        LL_INFOS("AVBOIT") << "Using independent direct-raster AVBOIT without fragment lists"
                            << LL_ENDL;
    }

    // The resolve compute shader (pass 7) used to read the opaque colour back
    // from a copy in gAVBOITOpaqueTarget purely so it could composite it
    // under the accumulated transparency. It writes the same pixel of the
    // screen it would have copied from, and a compute invocation may
    // imageLoad then imageStore its own texel of one image with no barrier
    // (no other invocation touches that texel), so the copy is unnecessary:
    // the resolve now reads screen's own current colour directly. Depth
    // still needs its own copy: gAVBOITOpaqueTarget's private depth is the
    // early-Z target the raster passes test against, and it must stay frozen
    // at the opaque depth for the whole capture while screen's shared depth
    // moves on.
    glCopyImageSubData(opaque_depth, GL_TEXTURE_2D, 0, 0, 0, 0,
                       gAVBOITOpaqueTarget.getDepth(), GL_TEXTURE_2D,
                       0, 0, 0, 0, width, height, 1);

    const U32 zero = 0u;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.occupancy);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.tileOccupancy);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.diagnostics);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.work);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    const U32 draw_command[4] = { 6u, 0u, 0u, 0u };
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 4u * sizeof(U32),
                    sizeof(draw_command), draw_command);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glBindImageTexture(3, sResources.extinction, 0, GL_TRUE, 0,
                       GL_READ_WRITE, GL_R32UI);
    // Must match the allocated internal format selected in allocateVolume().
    glBindImageTexture(4, sResources.transmittance, 0, GL_TRUE, 0,
                       GL_READ_WRITE, GL_R16F);
    glBindImageTexture(6, sResources.zeroTransmittanceDepth, 0, GL_FALSE, 0,
                       GL_READ_WRITE, GL_R8UI);
    glBindImageTexture(7, sResources.extinctionOverflowDepth, 0, GL_FALSE, 0,
                       GL_READ_WRITE, GL_R32UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sResources.warpScan);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, sResources.occupancy);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, sResources.warp);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, sResources.tileOccupancy);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, sResources.diagnostics);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sResources.work);

    gGL.getTexUnit(directOpaqueDepthTextureUnit())->bindManual(
        LLTexUnit::TT_TEXTURE, opaque_depth);

    // Full-resolution occupancy is conservatively folded into 8x8 cells.
    // Use the private opaque-depth target so thin final-raster coverage cannot
    // disappear merely because it missed a low-resolution sample center.
    gAVBOITOpaqueTarget.bindTarget();
    sDirectFrameReady = false;
    beginDirectRasterPass(0);
    return true;
}

void FSAVBOIT::beginDirectRasterPass(S32 pass)
{
    sDirectRasterPass = pass;
    if (pass == 0 || pass == 3)
    {
        glViewport(0, 0, sResources.viewportWidth,
                   sResources.viewportHeight);
    }
    else if (pass == 1)
    {
        // Round 3: pass 1 supersamples each 8x8 cell at
        // AVBOIT_PASS1_SUBSAMPLE points per axis instead of one, so its
        // viewport (and gAVBOITCellDepthTarget, bound as this pass's render
        // target ahead of this call) is scaled up by that factor.
        glViewport(0, 0, sResources.volumeWidth * AVBOIT_PASS1_SUBSAMPLE,
                   sResources.volumeHeight * AVBOIT_PASS1_SUBSAMPLE);
    }
    if (pass == 3)
    {
        // A9: full-resolution front-key pass. Two smallest-distinct-depth
        // keys per pixel, atomically inserted by every alpha fragment
        // (avboit_store_front_key() in avboitCaptureF.glsl); pass 2 reads
        // them back to give the front two layers an exact source-over
        // weight instead of the volume's per-cell approximation. Cleared to
        // the "no key" sentinel every frame, including the first frame
        // after allocation -- avboit_front_key()'s match in pass 2 must
        // never see stale data from a previous frame.
        //
        // E11 in fsexactoit.cpp uses the identical glClearTexImage-with-
        // fallback pattern for its own R32UI head/count images; mirrored
        // here rather than assuming the GL 4.4 function is always present
        // on an AVBOIT-capable (GL 4.3 baseline) driver.
        const GLuint no_key = 0xffffffffu;
        if (glClearTexImage)
        {
            glClearTexImage(sResources.frontKey0, 0, GL_RED_INTEGER,
                            GL_UNSIGNED_INT, &no_key);
            glClearTexImage(sResources.frontKey1, 0, GL_RED_INTEGER,
                            GL_UNSIGNED_INT, &no_key);
        }
        else
        {
            const GLint previous_fbo = LLRenderTarget::sCurFBO;
            const GLuint no_key_rgba[4] = { no_key, no_key, no_key, no_key };
            glBindFramebuffer(GL_FRAMEBUFFER, sResources.frontKeyFBO);
            glClearBufferuiv(GL_COLOR, 0, no_key_rgba);
            glClearBufferuiv(GL_COLOR, 1, no_key_rgba);
            glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);
        }
        glBindImageTexture(0, sResources.frontKey0, 0, GL_FALSE, 0,
                           GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(1, sResources.frontKey1, 0, GL_FALSE, 0,
                           GL_READ_WRITE, GL_R32UI);
    }
    if (pass == 2)
    {
        glViewport(0, 0, sResources.viewportWidth, sResources.viewportHeight);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                               GL_TEXTURE_2D,
                               sResources.accumulatedColorGlow, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                               GL_TEXTURE_2D,
                               sResources.accumulatedWeight, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3,
                               GL_TEXTURE_2D,
                               sResources.accumulatedExtinction, 0);
        const GLenum draw_buffers[] = {
            GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3
        };
        glDrawBuffers(4, draw_buffers);
        glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        for (GLuint attachment = 1; attachment <= 3; ++attachment)
        {
            glColorMaski(attachment, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnablei(GL_BLEND, attachment);
            glBlendEquationi(attachment, GL_FUNC_ADD);
            glBlendFunci(attachment, GL_ONE, GL_ONE);
        }
        const GLfloat clear_value[4] = { 0.f, 0.f, 0.f, 0.f };
        glClearBufferfv(GL_COLOR, 1, clear_value);
        glClearBufferfv(GL_COLOR, 2, clear_value);
        glClearBufferfv(GL_COLOR, 3, clear_value);
        gGL.getTexUnit(directTransmittanceTextureUnit())->bindManual(
            LLTexUnit::TT_TEXTURE_3D, sResources.transmittance);
        sDirectFrameReady = true;
    }
}

void FSAVBOIT::rasterizeConservativeBounds()
{
    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    static LLStaticHashedString depth_range("avboitDepthRange");
    static LLStaticHashedString linearization("avboitLinearization");
    static LLStaticHashedString opaque_depth_sampler(
        "avboitOpaqueDepthSampler");
    static LLStaticHashedString proxy_depth_interval(
        "avboitProxyDepthInterval");
    static LLStaticHashedString exact_proxy("avboitExactProxy");
    static LLStaticHashedString tile_range_uniform("avboitTileRange");
    const U32 groups_x = (sResources.volumeWidth + 15u) / 16u;
    const U32 groups_y = (sResources.volumeHeight + 15u) / 16u;

    // Initialize the interval words with compute so the portable GL 4.3
    // baseline retains an explicit empty sentinel.
    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITVolumeProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    gAVBOITVolumeProgram.uniform1i(pass, 9);
    glDispatchCompute(groups_x, groups_y, 1u);
    // Reset the per-tile depth range before any capture pass reduces into
    // it. Pass 13, not 12: pass 12 is a different, unrelated compute step
    // (transmittance-validity diagnostic) dispatched later in
    // finishDirectExtinction() on this same program -- the two collided
    // under the shared value 12 (see avboitVolumeC.glsl's pass-13 comment),
    // which meant this reset never actually ran.
    const U32 range_groups_x =
        ((sResources.viewportWidth + 15u) / 16u + 15u) / 16u;
    const U32 range_groups_y =
        ((sResources.viewportHeight + 15u) / 16u + 15u) / 16u;
    gAVBOITVolumeProgram.uniform1i(pass, 13);
    glDispatchCompute(range_groups_x, range_groups_y, 1u);
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    gAVBOITBoundsProgram.bind();
    gAVBOITBoundsProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITBoundsProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    LLCamera* camera = LLViewerCamera::getInstance();
    gAVBOITBoundsProgram.uniform2f(depth_range, camera->getNear(),
                                   camera->getFar());
    gAVBOITBoundsProgram.uniform1f(
        linearization, fittedLinearization(camera->getFar()));
    gAVBOITBoundsProgram.uniform1i(
        opaque_depth_sampler, directOpaqueDepthTextureUnit());
    gAVBOITBoundsProgram.uniform1i(tile_range_uniform, tileRange() ? 1 : 0);
    gAVBOITBoundsProgram.uniform1i(exact_proxy, 0);
    // AABB centers are in agent space. Clear any model matrix left by the
    // preceding scene draw so their projection matches CPU camera depths.
    LLRenderPass::applyModelMatrix(nullptr);
    gPipeline.mCubeVB->setBuffer();

    LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);
    struct BoundRecord
    {
        LLVector3 center;
        LLVector3 size;
        F32 minimumDepth = 0.f;
        F32 maximumDepth = 0.f;
    };
    std::vector<BoundRecord> bounds;
    std::unordered_set<LLSpatialGroup*> gathered_groups;
    const F32 water_height = LLEnvironment::instance().getWaterHeight();
    const bool above_water = !LLPipeline::sUnderWaterRender;
    const LLVector3 camera_origin = camera->getOrigin();
    const LLVector3 camera_at = camera->getAtAxis();
    const auto gather_group_range =
        [&bounds, &gathered_groups, camera_origin, camera_at,
         water_height, above_water](
            LLCullResult::sg_iterator begin,
            LLCullResult::sg_iterator end,
            U32 draw_pass)
    {
        for (LLCullResult::sg_iterator iter = begin; iter != end; ++iter)
        {
            LLSpatialGroup* group = *iter;
            if (!group || group->isDead() ||
                !group->getSpatialPartition()->mRenderByGroup)
            {
                continue;
            }
            const auto draw_entries = group->mDrawMap.find(draw_pass);
            if (draw_entries == group->mDrawMap.end() ||
                draw_entries->second.empty() ||
                !gathered_groups.insert(group).second)
            {
                continue;
            }

            LLSpatialBridge* bridge =
                group->getSpatialPartition()->asBridge();
            const LLVector4a* extents =
                bridge ? bridge->getSpatialExtents() : group->getExtents();
            const U32 partition_type =
                group->getSpatialPartition()->mPartitionType;
            const bool particle =
                partition_type == LLViewerRegion::PARTITION_PARTICLE ||
                partition_type == LLViewerRegion::PARTITION_HUD_PARTICLE;
            if ((!gPipeline.sRenderParticles && particle) ||
                (above_water &&
                 extents[1].getF32ptr()[2] < water_height) ||
                (!above_water &&
                 extents[0].getF32ptr()[2] > water_height))
            {
                continue;
            }
            LLVector4a center;
            LLVector4a size;
            center.setAdd(extents[0], extents[1]);
            center.mul(0.5f);
            size.setSub(extents[1], extents[0]);
            size.mul(0.5f);

            BoundRecord record;
            record.center = LLVector3(center.getF32ptr());
            record.size = LLVector3(size.getF32ptr());
            constexpr F32 proxy_fudge = 0.25f;
            record.size.mV[0] += proxy_fudge;
            record.size.mV[1] += proxy_fudge;
            record.size.mV[2] += proxy_fudge;
            const F32 center_depth =
                (record.center - camera_origin) * camera_at;
            const F32 depth_radius =
                fabsf(camera_at.mV[0]) * record.size.mV[0] +
                fabsf(camera_at.mV[1]) * record.size.mV[1] +
                fabsf(camera_at.mV[2]) * record.size.mV[2];
            record.minimumDepth = center_depth - depth_radius;
            record.maximumDepth = center_depth + depth_radius;
            bounds.push_back(record);
        }
    };

    gather_group_range(gPipeline.beginAlphaGroups(),
                       gPipeline.endAlphaGroups(), LLRenderPass::PASS_ALPHA);
    gather_group_range(gPipeline.beginRiggedAlphaGroups(),
                       gPipeline.endRiggedAlphaGroups(),
                       LLRenderPass::PASS_ALPHA_RIGGED);

    const F32 near_depth = camera->getNear();
    const F32 far_depth = camera->getFar();

    for (U32 index = 0; index < bounds.size(); ++index)
    {
        const BoundRecord& record = bounds[index];
        gAVBOITBoundsProgram.uniform2f(
            proxy_depth_interval,
            llmax(record.minimumDepth, near_depth),
            llmin(record.maximumDepth, far_depth));
        gAVBOITBoundsProgram.uniform3fv(
            LLShaderMgr::BOX_CENTER, 1, record.center.mV);
        gAVBOITBoundsProgram.uniform3fv(
            LLShaderMgr::BOX_SIZE, 1, record.size.mV);
        LLVector4a center;
        center.load3(record.center.mV);
        const U32 near_fan = get_box_fan_indices(camera, center);
        const U32 far_fan = near_fan ^ (7u * 8u);
        gPipeline.mCubeVB->drawRange(
            LLRender::TRIANGLE_FAN, 0, 7, 8, near_fan);
        gPipeline.mCubeVB->drawRange(
            LLRender::TRIANGLE_FAN, 0, 7, 8, far_fan);
    }

    // Static draw geometry supplies a coordinate-exact conservative proxy.
    // It intentionally ignores texture alpha, materials, and lighting, so
    // every actual alpha-tested fragment remains covered. Group AABBs stay
    // active for rigged geometry and as a coarse spatial fallback.
    gAVBOITBoundsProgram.uniform1i(exact_proxy, 1);
    gAVBOITBoundsProgram.uniform3f(
        LLShaderMgr::BOX_CENTER, 0.f, 0.f, 0.f);
    gAVBOITBoundsProgram.uniform3f(
        LLShaderMgr::BOX_SIZE, 1.f, 1.f, 1.f);
    for (LLCullResult::sg_iterator iter = gPipeline.beginAlphaGroups();
         iter != gPipeline.endAlphaGroups(); ++iter)
    {
        LLSpatialGroup* group = *iter;
        if (!group || group->isDead() ||
            !group->getSpatialPartition()->mRenderByGroup)
        {
            continue;
        }
        const U32 partition_type =
            group->getSpatialPartition()->mPartitionType;
        const bool particle =
            partition_type == LLViewerRegion::PARTITION_PARTICLE ||
            partition_type == LLViewerRegion::PARTITION_HUD_PARTICLE;
        if (!gPipeline.sRenderParticles && particle)
        {
            continue;
        }
        const auto found =
            group->mDrawMap.find(LLRenderPass::PASS_ALPHA);
        if (found == group->mDrawMap.end())
        {
            continue;
        }
        for (LLPointer<LLDrawInfo>& draw : found->second)
        {
            if (draw.isNull() || draw->mAvatar != nullptr ||
                draw->mVertexBuffer.isNull())
            {
                continue;
            }
            LLRenderPass::applyModelMatrix(*draw);
            draw->mVertexBuffer->setBuffer();
            draw->mVertexBuffer->drawRange(
                LLRender::TRIANGLES, draw->mStart, draw->mEnd,
                draw->mCount, draw->mOffset);
        }
    }
    gAVBOITBoundsProgram.unbind();

    // Rigged draw geometry uses the viewer's object-skinning feature and
    // palette uploader, producing the same skinned positions as alpha.
    gAVBOITSkinnedBoundsProgram.bind();
    gAVBOITSkinnedBoundsProgram.uniform2i(
        viewport, sResources.viewportWidth, sResources.viewportHeight);
    gAVBOITSkinnedBoundsProgram.uniform2i(
        volume_size, sResources.volumeWidth, sResources.volumeHeight);
    gAVBOITSkinnedBoundsProgram.uniform2f(
        depth_range, camera->getNear(), camera->getFar());
    gAVBOITSkinnedBoundsProgram.uniform1f(
        linearization, fittedLinearization(camera->getFar()));
    gAVBOITSkinnedBoundsProgram.uniform1i(
        opaque_depth_sampler, directOpaqueDepthTextureUnit());
    gAVBOITSkinnedBoundsProgram.uniform1i(
        tile_range_uniform, tileRange() ? 1 : 0);
    gAVBOITSkinnedBoundsProgram.uniform1i(exact_proxy, 1);
    for (LLCullResult::sg_iterator iter =
             gPipeline.beginRiggedAlphaGroups();
         iter != gPipeline.endRiggedAlphaGroups(); ++iter)
    {
        LLSpatialGroup* group = *iter;
        if (!group || group->isDead() ||
            !group->getSpatialPartition()->mRenderByGroup)
        {
            continue;
        }
        const auto found =
            group->mDrawMap.find(LLRenderPass::PASS_ALPHA_RIGGED);
        if (found == group->mDrawMap.end())
        {
            continue;
        }
        for (LLPointer<LLDrawInfo>& draw : found->second)
        {
            if (draw.isNull() || draw->mAvatar == nullptr ||
                draw->mVertexBuffer.isNull() ||
                !LLRenderPass::uploadMatrixPalette(*draw))
            {
                continue;
            }
            LLRenderPass::applyModelMatrix(*draw);
            draw->mVertexBuffer->setBuffer();
            draw->mVertexBuffer->drawRange(
                LLRender::TRIANGLES, draw->mStart, draw->mEnd,
                draw->mCount, draw->mOffset);
        }
    }
    gAVBOITSkinnedBoundsProgram.unbind();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // A box crossing the near plane is not guaranteed to leave a closed
    // fixed-function proxy footprint. Conservatively seed every cell for
    // those rare camera-intersecting bounds before spatial dilation.
    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITVolumeProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    gAVBOITVolumeProgram.uniform2f(depth_range, camera->getNear(),
                                   camera->getFar());
    gAVBOITVolumeProgram.uniform1f(
        linearization, fittedLinearization(camera->getFar()));
    for (U32 index = 0; index < bounds.size(); ++index)
    {
        const BoundRecord& record = bounds[index];
        if (record.minimumDepth <= near_depth &&
            record.maximumDepth >= near_depth)
        {
            gAVBOITVolumeProgram.uniform2f(
                proxy_depth_interval, near_depth,
                llmin(record.maximumDepth, far_depth));
            gAVBOITVolumeProgram.uniform1i(pass, 10);
            glDispatchCompute(groups_x, groups_y, 1u);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
    }
    // Dilate raw proxy intervals before material occupancy compares against
    // them. This pass depends only on the completed bounds raster.
    gAVBOITVolumeProgram.uniform1i(pass, 8);
    glDispatchCompute(groups_x, groups_y, 1u);
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Bind a disposable target sized far smaller than the viewport for pass
    // 0's material occupancy draws: color output is masked off and unread
    // (occupancy is tracked via SSBO/image atomics instead), so any
    // fragment landing outside this target's bounds simply has its color
    // write discarded by GL -- a cheap sink, not a real render target for
    // this pass. Distinct from gAVBOITCellDepthTarget (A2), which holds
    // real per-cell depth data consumed by pass 1's hardware depth test.
    gAVBOITPrepassTarget.bindTarget();
    glViewport(0, 0, sResources.viewportWidth, sResources.viewportHeight);
}

void FSAVBOIT::finishDirectColorRaster()
{
    for (GLuint attachment = 1; attachment <= 3; ++attachment)
    {
        glDisablei(GL_BLEND, attachment);
        glColorMaski(attachment, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + attachment,
            GL_TEXTURE_2D, 0, 0);
    }
    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw_buffer);
    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT |
                    GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    // Pass 2's color raster (between finishDirectExtinction() and here) drew
    // into gAVBOITOpaqueTarget, not gAVBOITPrepassTarget -- flush whatever
    // is actually bound. gAVBOITPrepassTarget was bound only transiently for
    // pass 0's occupancy draws and left unflushed once pass 1 rebinds a
    // different target (see finishDirectOccupancy()).
    gAVBOITOpaqueTarget.flush();
}

void FSAVBOIT::configureDirectRasterShader(LLGLSLShader* shader)
{
    if (!shader)
    {
        return;
    }

    static LLStaticHashedString raster_pass("avboitRasterPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    static LLStaticHashedString transmittance_sampler("avboitTransmittanceSampler");
    static LLStaticHashedString opaque_depth_sampler("avboitOpaqueDepthSampler");
    static LLStaticHashedString depth_range("avboitDepthRange");
    static LLStaticHashedString linearization("avboitLinearization");
    static LLStaticHashedString wide_extinction("avboitWideExtinction");
    static LLStaticHashedString sampling_bias("avboitSamplingBias");
    static LLStaticHashedString tile_range("avboitTileRange");
    static LLStaticHashedString capture_debug_mode("avboitDebugMode");
    static LLStaticHashedString pass1_subsample("avboitPass1Subsample");
    static LLStaticHashedString front_layers_uniform("avboitFrontLayers");
    GLint location = shader->getUniformLocation(raster_pass);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location, sDirectRasterPass);
    }
    if (sDirectRasterPass < 0)
    {
        return;
    }
    if (sDirectRasterPass == 2)
    {
        // LLDrawPoolAlpha disables ordinary framebuffer blending for OIT
        // capture, so restore independent additive blending at draw time.
        configureAccumulationBlend();
    }
    location = shader->getUniformLocation(viewport);
    if (location >= 0)
    {
        glProgramUniform2i(shader->mProgramObject, location,
                           sResources.viewportWidth, sResources.viewportHeight);
    }
    location = shader->getUniformLocation(volume_size);
    if (location >= 0)
    {
        glProgramUniform2i(shader->mProgramObject, location,
                           sResources.volumeWidth, sResources.volumeHeight);
    }
    location = shader->getUniformLocation(wide_extinction);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location,
                           wideExtinction() ? 1 : 0);
    }
    location = shader->getUniformLocation(sampling_bias);
    if (location >= 0)
    {
        glProgramUniform1f(shader->mProgramObject, location, samplingBias());
    }
    location = shader->getUniformLocation(tile_range);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location,
                           tileRange() ? 1 : 0);
    }
    location = shader->getUniformLocation(pass1_subsample);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location,
                           static_cast<GLint>(AVBOIT_PASS1_SUBSAMPLE));
    }
    location = shader->getUniformLocation(front_layers_uniform);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location,
                           frontLayers() ? 1 : 0);
    }
    location = shader->getUniformLocation(capture_debug_mode);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location, debugMode());
    }
    location = shader->getUniformLocation(transmittance_sampler);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location,
                           directTransmittanceTextureUnit());
    }
    location = shader->getUniformLocation(opaque_depth_sampler);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location,
                           directOpaqueDepthTextureUnit());
    }
    location = shader->getUniformLocation(depth_range);
    if (location >= 0)
    {
        const LLCamera& camera = *LLViewerCamera::getInstance();
        glProgramUniform2f(shader->mProgramObject, location,
                           camera.getNear(), camera.getFar());
    }
    location = shader->getUniformLocation(linearization);
    if (location >= 0)
    {
        const LLCamera& camera = *LLViewerCamera::getInstance();
        glProgramUniform1f(shader->mProgramObject, location,
                           fittedLinearization(camera.getFar()));
    }
}

void FSAVBOIT::finishDirectOccupancy()
{
    // Pop back off gAVBOITPrepassTarget (pass 0's disposable occupancy
    // sink, bound by rasterizeConservativeBounds()) to the target it was
    // pushed on top of (gAVBOITOpaqueTarget, from beginDirectFrame()).
    // LLRenderTarget's bind stack must be flushed in the same order it was
    // pushed -- skipping this flush would leave gAVBOITPrepassTarget
    // buried under every target bound below, and its eventual flush() call
    // would then rebind the wrong target.
    gAVBOITPrepassTarget.flush();

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    static LLStaticHashedString depth_range("avboitDepthRange");
    static LLStaticHashedString linearization("avboitLinearization");
    static LLStaticHashedString wide_extinction("avboitWideExtinction");
    const U32 groups_x = (sResources.volumeWidth + 15u) / 16u;
    const U32 groups_y = (sResources.volumeHeight + 15u) / 16u;

    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITVolumeProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    gAVBOITVolumeProgram.uniform1i(wide_extinction, wideExtinction() ? 1 : 0);
    const LLCamera& camera = *LLViewerCamera::getInstance();
    gAVBOITVolumeProgram.uniform2f(depth_range, camera.getNear(),
                                   camera.getFar());
    gAVBOITVolumeProgram.uniform1f(
        linearization, fittedLinearization(camera.getFar()));
    gAVBOITVolumeProgram.uniform1i(pass, 1);
    glDispatchCompute(1u, 1u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 11);
    glDispatchCompute(1u, 1u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 2);
    glDispatchCompute(groups_x, groups_y, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 4);
    glDispatchCompute(1u, 1u, 1u);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 3);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, sResources.work);
    glDispatchComputeIndirect(0);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    // gAVBOITOpaqueTarget is already the current target here (restored by
    // the gAVBOITPrepassTarget.flush() at the top of this function) -- no
    // flush/rebind needed before pushing the new depth-prepass target below.

    // A2: bake each cell's farthest opaque depth into a small volume-
    // resolution target before pass 1 binds it, so pass 1's hardware
    // early_fragment_tests rejects against the correct 8x8 block instead of
    // a single full-res pixel sampled at the wrong location. See
    // avboitCellDepthF.glsl and doc/ayanestorm-oit-performance-audit-plan.md
    // A2.
    {
        static LLStaticHashedString cell_depth_sampler(
            "avboitOpaqueDepthSampler");
        static LLStaticHashedString cell_depth_subsample(
            "avboitPass1Subsample");
        gAVBOITCellDepthTarget.bindTarget();
        LLGLDepthTest depth_test(GL_TRUE, GL_TRUE, GL_ALWAYS);
        gAVBOITCellDepthTarget.clear(GL_DEPTH_BUFFER_BIT);
        gAVBOITCellDepthProgram.bind();
        gAVBOITCellDepthProgram.uniform2i(
            viewport, sResources.viewportWidth, sResources.viewportHeight);
        gAVBOITCellDepthProgram.uniform1i(
            cell_depth_sampler, directOpaqueDepthTextureUnit());
        gAVBOITCellDepthProgram.uniform1i(
            cell_depth_subsample, static_cast<GLint>(AVBOIT_PASS1_SUBSAMPLE));
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gAVBOITCellDepthProgram.unbind();
        gAVBOITCellDepthTarget.flush();
    }
    // gAVBOITOpaqueTarget is the current target here (the cell-depth bake
    // above pushed and flushed gAVBOITCellDepthTarget in a balanced pair) --
    // A9's pass 3 (front key), inserted by the caller between this function
    // and beginPass1(), reads/writes it directly rather than rebinding it.
}

void FSAVBOIT::beginPass1()
{
    gAVBOITCellDepthTarget.bindTarget();
    beginDirectRasterPass(1);
}

void FSAVBOIT::finishDirectExtinction()
{
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    static LLStaticHashedString wide_extinction("avboitWideExtinction");
    const U32 tile_groups_x =
        ((sResources.viewportWidth + 15u) / 16u + 15u) / 16u;
    const U32 tile_groups_y =
        ((sResources.viewportHeight + 15u) / 16u + 15u) / 16u;
    const U32 volume_groups_x =
        (sResources.volumeWidth + 15u) / 16u;
    const U32 volume_groups_y =
        (sResources.volumeHeight + 15u) / 16u;
    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITVolumeProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    gAVBOITVolumeProgram.uniform1i(wide_extinction, wideExtinction() ? 1 : 0);
    gAVBOITVolumeProgram.uniform1i(pass, 5);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, sResources.work);
    glDispatchComputeIndirect(0);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 12);
    glDispatchCompute(volume_groups_x, volume_groups_y, 1u);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 6);
    glDispatchCompute(tile_groups_x, tile_groups_y, 1u);
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT |
                    GL_TEXTURE_FETCH_BARRIER_BIT);
    // A2: pass 1's material raster (between finishDirectOccupancy() and
    // here) drew into gAVBOITCellDepthTarget, not gAVBOITOpaqueTarget.
    // Flushing it pops the bind stack back to gAVBOITOpaqueTarget (its
    // mPreviousRT, set when finishDirectOccupancy() bound it for pass 1),
    // which is therefore already the current target afterward -- do not
    // bindTarget() it again, that would push a second, self-referential
    // entry onto the stack (gAVBOITOpaqueTarget is already its own
    // ancestor here) and corrupt the eventual unbind-to-screen.
    gAVBOITCellDepthTarget.flush();

    // Rasterize conservative zero-transmittance quads into a private copy of
    // opaque depth. The final color pass then receives ordinary early-Z/Hi-Z
    // rejection without modifying the viewer's shared scene depth texture.
    {
        LLGLDepthTest depth_test(GL_TRUE, GL_TRUE, GL_LEQUAL);
        gAVBOITEarlyDepthProgram.bind();
        gAVBOITEarlyDepthProgram.uniform2i(
            viewport, sResources.viewportWidth, sResources.viewportHeight);
        gAVBOITEarlyDepthProgram.uniform2i(
            volume_size, sResources.volumeWidth, sResources.volumeHeight);
        gPipeline.mScreenTriangleVB->setBuffer();
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sResources.work);
        glDrawArraysIndirect(
            GL_TRIANGLES,
            reinterpret_cast<const void*>(4u * sizeof(U32)));
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        gAVBOITEarlyDepthProgram.unbind();
    }
    beginDirectRasterPass(2);
}

bool FSAVBOIT::directFrameReady()
{
    return sDirectFrameReady;
}

bool FSAVBOIT::finishDirectFrame(LLRenderTarget& screen)
{
    if (!sDirectFrameReady)
    {
        return false;
    }

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    gGL.getTexUnit(directOpaqueDepthTextureUnit())->unbind(LLTexUnit::TT_TEXTURE);

    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    static LLStaticHashedString debug_mode_uniform("avboitDebugMode");
    static LLStaticHashedString transmittance_sampler(
        "avboitTransmittanceSampler");
    const S32 debug_mode = debugMode();
    // Diagnostic 13 compares the volume against the exact accumulation, so the
    // transmittance volume must stay readable during resolve. Every other mode
    // releases it as before.
    if (debug_mode != 13)
    {
        gGL.getTexUnit(directTransmittanceTextureUnit())->unbind(
            LLTexUnit::TT_TEXTURE_3D);
    }
    static S32 previous_debug_mode = -1;
    if (debug_mode != previous_debug_mode)
    {
        LL_INFOS("AVBOIT") << "AVBOIT diagnostic mode " << debug_mode
                            << LL_ENDL;
        previous_debug_mode = debug_mode;
    }
    const U32 groups_x = (sResources.viewportWidth + 15u) / 16u;
    const U32 groups_y = (sResources.viewportHeight + 15u) / 16u;

    // Read-write: pass 7 imageLoads its own texel's current (opaque) colour
    // before imageStoring the composited result over it. See the comment on
    // beginDirectFrame()'s removed colour copy.
    glBindImageTexture(2, screen.getTexture(), 0, GL_FALSE, 0,
                       GL_READ_WRITE, GL_RGBA16F);
    glBindImageTexture(0, sResources.accumulatedColorGlow, 0, GL_FALSE, 0,
                       GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, sResources.accumulatedWeight, 0, GL_FALSE, 0,
                       GL_READ_ONLY, GL_R16F);
    glBindImageTexture(5, sResources.accumulatedExtinction, 0, GL_FALSE, 0,
                       GL_READ_ONLY, GL_R16F);
    gAVBOITResolveProgram.bind();
    gAVBOITResolveProgram.uniform2i(viewport, sResources.viewportWidth,
                                    sResources.viewportHeight);
    gAVBOITResolveProgram.uniform2i(volume_size, sResources.volumeWidth,
                                    sResources.volumeHeight);
    gAVBOITResolveProgram.uniform1i(pass, 7);
    gAVBOITResolveProgram.uniform1i(debug_mode_uniform, debug_mode);
    if (debug_mode == 13)
    {
        gAVBOITResolveProgram.uniform1i(transmittance_sampler,
                                        directTransmittanceTextureUnit());
    }
    glDispatchCompute(groups_x, groups_y, 1u);
    gAVBOITResolveProgram.unbind();
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    // <AS:Chanayane> All AVBOIT raster passes above target the private opaque
    // copy. Restore the caller's screen target now: the compute resolve wrote
    // screen color through imageStore and did not need an FBO, while the
    // isolate coverage pass below must update screen's shared scene depth.
    // Leaving gAVBOITOpaqueTarget bound made that pass write the private depth
    // copy instead, so the late isolate pass painted over AVBOIT transparency.
    gAVBOITOpaqueTarget.flush();
    // </AS:Chanayane>

    // <AS:Chanayane> Self-lighting floater isolate-background mode: the
    // compute resolve above writes color only -- imageStore cannot target a
    // depth-format texture, so gPipeline.mRT->deferredScreen's shared depth
    // never gets written for AVBOIT-resolved pixels. Without this, a later
    // depth-tested isolate backdrop pass can't tell "AVBOIT drew real alpha
    // content here" from "nothing was drawn here" and paints over it. This
    // small fragment pass samples the same per-pixel coverage data
    // (accumulatedWeight/accumulatedColorGlow, still valid GL textures at
    // this point) and writes a near-plane depth wherever coverage is
    // non-zero. No-op with zero cost when isolate mode is inactive, and has
    // no effect whatsoever on AVBOIT's own color output either way.
    if (ASBackgroundIsolate::isActive() && gAVBOITIsolateDepthProgram.mProgramObject)
    {
        LL_PROFILE_GPU_ZONE("AVBOIT isolate depth");
        static LLStaticHashedString isolate_weight("avboitIsolateWeight");
        static LLStaticHashedString isolate_color_glow("avboitIsolateColorGlow");
        const S32 weight_unit = directOpaqueDepthTextureUnit();
        const S32 color_glow_unit = directTransmittanceTextureUnit();

        LLGLDepthTest depth_test(GL_TRUE, GL_TRUE, GL_LEQUAL);
        gAVBOITIsolateDepthProgram.bind();
        gGL.getTexUnit(weight_unit)->bindManual(
            LLTexUnit::TT_TEXTURE, sResources.accumulatedWeight);
        gAVBOITIsolateDepthProgram.uniform1i(isolate_weight, weight_unit);
        gGL.getTexUnit(color_glow_unit)->bindManual(
            LLTexUnit::TT_TEXTURE, sResources.accumulatedColorGlow);
        gAVBOITIsolateDepthProgram.uniform1i(isolate_color_glow, color_glow_unit);

        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        gGL.getTexUnit(weight_unit)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(color_glow_unit)->unbind(LLTexUnit::TT_TEXTURE);
        gAVBOITIsolateDepthProgram.unbind();
    }
    // </AS:Chanayane>

    sDirectRasterPass = -1;
    sDirectFrameReady = false;
    return true;
}

#endif // LL_DARWIN
