/**
 * @file asvolumetriclighting.cpp
 * @brief AyaneStorm optional volumetric (god-ray) lighting pass.
 * @author chanayane@firestorm
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (c) 2026 Chanayane @ Second Life
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "asvolumetriclighting.h"

#include "asbackgroundisolate.h"
#include "llcontrol.h"
#include "lluictrl.h"
#include "ascelestialtwilight.h"

#include "llenvironment.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "lllightconstants.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "lluiimage.h"
#include "lldrawable.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <vector>

#define AS_VOLUMETRIC_PERFORMANCE_LOGGING 0

extern bool gCubeSnapshot;

namespace
{
LLGLSLShader gASVolumetricLightProgram;
LLGLSLShader gASVolumetricLocalLightProgram;
LLGLSLShader gASVolumetricCompositeProgram;
LLGLSLShader gASVolumetricAtlasProgram;
// Loaded directly rather than via LLUI::getUIImage(): that path shares the
// generic UI-icon texture pipeline (asset streaming, discard levels, and
// compressed internal formats), any of which would corrupt this texture's
// per-texel blue-noise distribution. BOOST_UI plus an explicit uncompressed
// format and no mipmaps guarantee the exact source bytes reach the GPU.
LLPointer<LLViewerFetchedTexture> sBlueNoiseImage;

constexpr S32 MAX_VOLUMETRIC_LOCAL_LIGHTS = 64;
constexpr F32 VOLUMETRIC_LOCAL_LIGHT_FALLOFF = 0.5f;

#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
class ASVolumetricGpuTimer
{
public:
    void begin()
    {
        // The viewer's optional one-frame shader profiler also owns the
        // GL_TIME_ELAPSED target. Skip that diagnostic frame rather than
        // nesting queries, which OpenGL forbids for the same target.
        if (LLGLSLShader::sProfileEnabled)
        {
            mActive = false;
            return;
        }

        if (mQueries[0] == 0)
        {
            glGenQueries(QUERY_RING_SIZE, mQueries);
        }

        if (mPending[mWriteIndex])
        {
            GLuint available = GL_FALSE;
            glGetQueryObjectuiv(mQueries[mWriteIndex], GL_QUERY_RESULT_AVAILABLE,
                                &available);
            if (available == GL_FALSE)
            {
                mActive = false;
                return;
            }

            GLuint64 elapsed_ns = 0;
            glGetQueryObjectui64v(mQueries[mWriteIndex], GL_QUERY_RESULT,
                                  &elapsed_ns);
            mTotalMs += (F64)elapsed_ns / 1000000.0;
            ++mSamples;
            mPending[mWriteIndex] = false;
        }

        glBeginQuery(GL_TIME_ELAPSED, mQueries[mWriteIndex]);
        mActive = true;
    }

    void end()
    {
        if (!mActive)
        {
            return;
        }
        glEndQuery(GL_TIME_ELAPSED);
        mPending[mWriteIndex] = true;
        mWriteIndex = (mWriteIndex + 1) % QUERY_RING_SIZE;
        mActive = false;
    }

    F64 averageMs() const
    {
        return mSamples > 0 ? mTotalMs / (F64)mSamples : 0.0;
    }

    U32 samples() const { return mSamples; }

    void resetTotals()
    {
        mTotalMs = 0.0;
        mSamples = 0;
    }

    void release()
    {
        if (mQueries[0] != 0)
        {
            glDeleteQueries(QUERY_RING_SIZE, mQueries);
        }
        for (U32 i = 0; i < QUERY_RING_SIZE; ++i)
        {
            mQueries[i] = 0;
            mPending[i] = false;
        }
        mWriteIndex = 0;
        mActive = false;
        resetTotals();
    }

private:
    static constexpr U32 QUERY_RING_SIZE = 4;
    GLuint mQueries[QUERY_RING_SIZE] = { 0, 0, 0, 0 };
    bool mPending[QUERY_RING_SIZE] = { false, false, false, false };
    U32 mWriteIndex = 0;
    bool mActive = false;
    F64 mTotalMs = 0.0;
    U32 mSamples = 0;
};

ASVolumetricGpuTimer sDirectionalGpuTimer;
ASVolumetricGpuTimer sAtlasGpuTimer;
ASVolumetricGpuTimer sLocalLightGpuTimer;
ASVolumetricGpuTimer sCompositeGpuTimer;

void resetVolumetricGpuTiming()
{
    sDirectionalGpuTimer.release();
    sAtlasGpuTimer.release();
    sLocalLightGpuTimer.release();
    sCompositeGpuTimer.release();
}

void logVolumetricGpuTiming()
{
    if (sCompositeGpuTimer.samples() < 120)
    {
        return;
    }

    LL_INFOS("Volumetric") << "Volumetric GPU timing average: quality="
                            << (gSavedSettings.getBOOL("RenderVolumetricLightingHighQuality")
                                ? "high" : "normal")
                            << " samples=" << ASVolumetricLighting::getSampleCount()
                            << " blur="
                            << gSavedSettings.getF32("RenderVolumetricLightingBlurStrength")
                            << " radius="
                            << gSavedSettings.getF32("RenderVolumetricLightingBlurRadius")
                            << " blue="
                            << gSavedSettings.getF32("RenderVolumetricLightingBlueNoiseStrength")
                            << " directional="
                            << sDirectionalGpuTimer.averageMs() << "ms/"
                            << sDirectionalGpuTimer.samples()
                            << " atlas=" << sAtlasGpuTimer.averageMs() << "ms/"
                            << sAtlasGpuTimer.samples()
                            << " local=" << sLocalLightGpuTimer.averageMs() << "ms/"
                            << sLocalLightGpuTimer.samples()
                            << " composite=" << sCompositeGpuTimer.averageMs() << "ms/"
                            << sCompositeGpuTimer.samples() << LL_ENDL;

    sDirectionalGpuTimer.resetTotals();
    sAtlasGpuTimer.resetTotals();
    sLocalLightGpuTimer.resetTotals();
    sCompositeGpuTimer.resetTotals();
}
#endif

// Match the single-source priority used by atmospheric twilight. This keeps
// moon-only phase/tint controls off while the solar twilight tail is active.
bool isVolumetricSunSource()
{
    LLEnvironment& environment = LLEnvironment::instance();
    const LLSettingsSky::ptr_t sky = environment.getCurrentSky();
    return ASCelestialTwilight::isSunSource(sky.get());
}

// Numerically integrate the nonlinear phase mask used by moonF.glsl. The
// cached result changes only when a phase control changes, avoiding per-frame
// integration while keeping god-ray energy tied to visible lunar surface.
F32 calculateMoonPhaseIlluminatedFraction(F32 phase, F32 curvature, F32 softness)
{
    static F32 cached_phase = -1.f;
    static F32 cached_curvature = -1.f;
    static F32 cached_softness = -1.f;
    static F32 cached_fraction = 1.f;

    if (phase == cached_phase && curvature == cached_curvature && softness == cached_softness)
    {
        return cached_fraction;
    }

    const F32 linear_fraction = 1.f - fabsf(2.f * phase - 1.f);
    const F32 light_z = 2.f * linear_fraction - 1.f;
    F32 light_x = sqrtf(llmax(1.f - light_z * light_z, 0.f));
    light_x *= phase <= 0.5f ? 1.f : -1.f;

    constexpr S32 GRID_SIZE = 96;
    F32 mask_sum = 0.f;
    S32 disc_samples = 0;
    const F32 edge_width = llmax(softness, 0.002f);
    for (S32 y = 0; y < GRID_SIZE; ++y)
    {
        const F32 py = 2.f * ((F32)y + 0.5f) / (F32)GRID_SIZE - 1.f;
        for (S32 x = 0; x < GRID_SIZE; ++x)
        {
            const F32 px = 2.f * ((F32)x + 0.5f) / (F32)GRID_SIZE - 1.f;
            const F32 radius_squared = px * px + py * py;
            if (radius_squared > 1.f)
            {
                continue;
            }

            const F32 surface_z = powf(sqrtf(llmax(1.f - radius_squared, 0.f)), curvature);
            const F32 phase_light = px * light_x + surface_z * light_z;
            const F32 t = llclamp((phase_light + edge_width) / (2.f * edge_width), 0.f, 1.f);
            // Match moonF.glsl's terminator-centered smooth transition.
            mask_sum += t * t * (3.f - 2.f * t);
            ++disc_samples;
        }
    }

    cached_phase = phase;
    cached_curvature = curvature;
    cached_softness = softness;
    cached_fraction = disc_samples > 0 ? mask_sum / (F32)disc_samples : 0.f;
    return cached_fraction;
}

// Keep god-ray horizon tint and phase energy visually consistent with the
// separately rendered moon disc without altering general moonlight color.
void applyMoonAppearance(LLGLSLShader& shader)
{
    const LLColor4 tint = gSavedSettings.getColor4("ASMoonHorizonTint");
    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const F32 elevation = sky ? sky->getMoonDirection().mV[VZ] : 1.f;
    const F32 phase = llclamp(gSavedSettings.getF32("ASMoonPhase"), 0.f, 1.f);
    const F32 curvature = llclamp(gSavedSettings.getF32("ASMoonPhaseCurvature"), 0.25f, 5.f);
    const F32 softness = llclamp(gSavedSettings.getF32("ASMoonPhaseSoftness"), 0.f, 0.30f);
    const F32 illuminated_fraction =
        calculateMoonPhaseIlluminatedFraction(phase, curvature, softness);
    shader.uniform3fv(LLStaticHashedString("moon_horizon_tint"), 1, tint.mV);
    shader.uniform1f(LLStaticHashedString("moon_horizon_tint_strength"),
                     gSavedSettings.getF32("ASMoonHorizonTintStrength"));
    shader.uniform1f(LLStaticHashedString("moon_horizon_elevation"), elevation);
    shader.uniform1f(LLStaticHashedString("moon_horizon_tint_height"),
                     sinf(llclamp(gSavedSettings.getF32("ASMoonHorizonTintAngle"), 0.5f, 90.f) * DEG_TO_RAD));
    shader.uniform1f(LLStaticHashedString("moon_phase_illumination"), illuminated_fraction);
}

struct LocalLight
{
    LLVector4 center_radius;
    LLVector4 color_falloff;
    F32 score;
};
}

void ASVolumetricLighting::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASVolumetricLighting.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            static const std::vector<std::string> volumetric_controls = {
                "RenderVolumetricLightingAlbedo", "RenderVolumetricLightingDensity",
                "RenderVolumetricLightingSunAsymmetry", "RenderVolumetricLightingAsymmetry",
                "RenderVolumetricLightingHighQuality", "RenderVolumetricLightingDebug",
                "RenderVolumetricLocalLightsIntensity", "RenderVolumetricLocalLightsMaxCount"
            };
            const std::string control_name = data.asString();
            if (std::find(volumetric_controls.begin(), volumetric_controls.end(), control_name)
                != volumetric_controls.end())
            {
                if (LLControlVariable* control = gSavedSettings.getControl(control_name))
                {
                    control->resetToDefault(true);
                }
            }
        });
}

F32 ASVolumetricLighting::getMoonPhaseIlluminatedFraction()
{
    const F32 phase = llclamp(gSavedSettings.getF32("ASMoonPhase"), 0.f, 1.f);
    const F32 curvature = llclamp(gSavedSettings.getF32("ASMoonPhaseCurvature"), 0.25f, 5.f);
    const F32 softness = llclamp(gSavedSettings.getF32("ASMoonPhaseSoftness"), 0.f, 0.30f);
    return calculateMoonPhaseIlluminatedFraction(phase, curvature, softness);
}

bool ASVolumetricLighting::sSupportChecked = false;
bool ASVolumetricLighting::sSupported = false;
bool ASVolumetricLighting::sShadersLoaded = false;
LLRenderTarget ASVolumetricLighting::sVolumetricTarget;
LLRenderTarget ASVolumetricLighting::sTransparencyAtlas;
U32 ASVolumetricLighting::sAtlasIntegralTex[2] = { 0, 0 };
U32 ASVolumetricLighting::sAtlasFBO = 0;
U32 ASVolumetricLighting::sAtlasIntegralWidth = 0;
U32 ASVolumetricLighting::sAtlasIntegralHeight = 0;
bool ASVolumetricLighting::sAtlasConsumerSeen = true;
bool ASVolumetricLighting::sAtlasProducedThisFrame = false;
U32 ASVolumetricLighting::sAtlasUnusedFrames = 0;

// Folded into the shader cache hash in llviewershadermgr.cpp alongside
// FSExactOIT's revision. During active development the shader cache is cleared
// manually, so do not bump this for every edit and trigger an avoidable LTO
// relink. Bump it before distributing a build whose users will retain caches.
const char* ASVolumetricLighting::shaderCacheRevision()
{
    return "as-volumetric-lighting-v20";
}

// GLSL 4.00 is the floor here (not FSAVBOIT's 4.30): this feature is
// designed to also run on macOS, which caps at OpenGL 4.1 / GLSL 4.10.
bool ASVolumetricLighting::isSupported()
{
    if (!sSupportChecked)
    {
        sSupported = gGLManager.mGLVersion >= 4.0f &&
            (gGLManager.mGLSLVersionMajor > 4 ||
             (gGLManager.mGLSLVersionMajor == 4 && gGLManager.mGLSLVersionMinor >= 0));
        sSupportChecked = true;
    }
    return sSupported;
}

bool ASVolumetricLighting::isEnabled()
{
    return isSupported()
        && LLPipeline::sRenderDeferred
        && LLPipeline::RenderShadowDetail > 0
        && gSavedSettings.getBOOL("RenderVolumetricLighting")
        && !ASBackgroundIsolate::isActive();
}

bool ASVolumetricLighting::loadShaders(S32 shader_level)
{
    sShadersLoaded = false;

    if (!isSupported())
    {
        return true;
    }

    gASVolumetricLightProgram.unload();
    gASVolumetricLightProgram.mName = "AS Volumetric Light Shader";
    gASVolumetricLightProgram.mFeatures.isDeferred = true;
    gASVolumetricLightProgram.mFeatures.hasShadows = true;
    gASVolumetricLightProgram.clearPermutations();
    gASVolumetricLightProgram.mShaderFiles.clear();
    gASVolumetricLightProgram.mShaderFiles.push_back(std::make_pair("deferred/asVolumetricLightV.glsl", GL_VERTEX_SHADER));
    gASVolumetricLightProgram.mShaderFiles.push_back(std::make_pair("deferred/asVolumetricShadowUtil.glsl", GL_FRAGMENT_SHADER));
    gASVolumetricLightProgram.mShaderFiles.push_back(std::make_pair("deferred/asVolumetricLightF.glsl", GL_FRAGMENT_SHADER));
    gASVolumetricLightProgram.mShaderLevel = shader_level;

    bool success = gASVolumetricLightProgram.createShader();

    if (success)
    {
        gASVolumetricLocalLightProgram.unload();
        gASVolumetricLocalLightProgram.mName = "AS Volumetric Local Light Shader";
        gASVolumetricLocalLightProgram.mFeatures.isDeferred = true;
        gASVolumetricLocalLightProgram.clearPermutations();
        gASVolumetricLocalLightProgram.mShaderFiles.clear();
        gASVolumetricLocalLightProgram.mShaderFiles.push_back(std::make_pair("deferred/asVolumetricLightV.glsl", GL_VERTEX_SHADER));
        gASVolumetricLocalLightProgram.mShaderFiles.push_back(std::make_pair("deferred/asVolumetricLocalLightF.glsl", GL_FRAGMENT_SHADER));
        gASVolumetricLocalLightProgram.mShaderLevel = shader_level;

        success = gASVolumetricLocalLightProgram.createShader();
    }

    if (success)
    {
        gASVolumetricAtlasProgram.unload();
        gASVolumetricAtlasProgram.mName = "AS Volumetric Transparency Atlas Shader";
        gASVolumetricAtlasProgram.mFeatures.isDeferred = true;
        gASVolumetricAtlasProgram.mFeatures.hasShadows = true;
        gASVolumetricAtlasProgram.clearPermutations();
        gASVolumetricAtlasProgram.mShaderFiles.clear();
        gASVolumetricAtlasProgram.mShaderFiles.push_back(std::make_pair("deferred/asVolumetricLightV.glsl", GL_VERTEX_SHADER));
        gASVolumetricAtlasProgram.mShaderFiles.push_back(std::make_pair("deferred/asVolumetricShadowUtil.glsl", GL_FRAGMENT_SHADER));
        gASVolumetricAtlasProgram.mShaderFiles.push_back(std::make_pair("deferred/asVolumetricAtlasF.glsl", GL_FRAGMENT_SHADER));
        gASVolumetricAtlasProgram.mShaderLevel = shader_level;
        success = gASVolumetricAtlasProgram.createShader();
    }

    if (success)
    {
        gASVolumetricCompositeProgram.unload();
        gASVolumetricCompositeProgram.mName = "AS Volumetric Composite Shader";
        gASVolumetricCompositeProgram.mFeatures.isDeferred = true;
        gASVolumetricCompositeProgram.clearPermutations();
        gASVolumetricCompositeProgram.mShaderFiles.clear();
        gASVolumetricCompositeProgram.mShaderFiles.push_back(std::make_pair("deferred/asVolumetricLightV.glsl", GL_VERTEX_SHADER));
        gASVolumetricCompositeProgram.mShaderFiles.push_back(std::make_pair("deferred/asVolumetricCompositeF.glsl", GL_FRAGMENT_SHADER));
        gASVolumetricCompositeProgram.mShaderLevel = shader_level;

        success = gASVolumetricCompositeProgram.createShader();
    }

    if (!success)
    {
        gASVolumetricLightProgram.unload();
        gASVolumetricLocalLightProgram.unload();
        gASVolumetricAtlasProgram.unload();
        gASVolumetricCompositeProgram.unload();
    }

    sShadersLoaded = success;
    return true; // failure here must not fail the whole deferred shader load
}

void ASVolumetricLighting::unloadShaders()
{
    gASVolumetricLightProgram.unload();
    gASVolumetricLocalLightProgram.unload();
    gASVolumetricAtlasProgram.unload();
    gASVolumetricCompositeProgram.unload();
    sShadersLoaded = false;
}

void ASVolumetricLighting::allocateResources(U32 width, U32 height)
{
    if (!isSupported())
    {
        return;
    }

    // High quality is explicit because full-resolution raymarching is a large
    // GPU cost increase; the persisted default remains half resolution.
    const bool full_resolution = gSavedSettings.getBOOL("RenderVolumetricLightingHighQuality");
    U32 target_width  = llmax((U32)1, full_resolution ? width : width / 2);
    U32 target_height = llmax((U32)1, full_resolution ? height : height / 2);

    sVolumetricTarget.allocate(target_width, target_height, GL_RGBA16F);

    // Half resolution here still leaves each of the 4x4 atlas tiles at a
    // real width/8 x height/8 - the atlas is already trilinearly sampled by
    // every consumer (per-tile bilinear plus a lerp across adjacent slices),
    // so a coarser source grid is visually forgiving the same way the main
    // raymarch target already relies on its own half-res + upsample design.
    // This was the single largest new GPU cost in the feature: the atlas
    // shader runs its cumulative shadow-sampling loop at full resolution,
    // with the deepest tile alone doing 16 shadow samples per pixel.
    //
    // Rounded UP to a multiple of 4 (not just clamped to a minimum of 4):
    // renderTransparencyAtlas()'s per-slice scissor rects are computed from
    // exact integer tile boundaries (x0 = width*tile/4, x1 = width*(tile+1)/4)
    // rather than a single floored "tile_width" constant, but that alone
    // only prevents OVERLAP/gaps between tiles - it does not by itself
    // guarantee the last tile reaches the true right/top edge of a
    // dimension that isn't a multiple of 4. Rounding the allocation itself
    // removes the ambiguity entirely: every tile boundary lands on an exact
    // integer pixel with no remainder to strand.
    const U32 atlas_width  = llmax((U32)4, ((width  / 2 + 3) / 4) * 4);
    const U32 atlas_height = llmax((U32)4, ((height / 2 + 3) / 4) * 4);
    sTransparencyAtlas.allocate(atlas_width, atlas_height, GL_RGBA16F);

    // Two ping-pong single-channel scratch textures carrying the raw
    // cumulative integral between per-slice atlas draws (see the header
    // comment on sAtlasIntegralTex in asvolumetriclighting.h for why this
    // can't just be a second managed LLRenderTarget attachment). Must match
    // sTransparencyAtlas's resolution exactly since both are attached to the
    // same FBO as MRT outputs during the atlas-building draws.
    releaseAtlasIntegralAttachments();
    sAtlasIntegralWidth = sTransparencyAtlas.getWidth();
    sAtlasIntegralHeight = sTransparencyAtlas.getHeight();
    LLImageGL::generateTextures(2, sAtlasIntegralTex);
    for (U32 i = 0; i < 2; ++i)
    {
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, sAtlasIntegralTex[i]);
        LLImageGL::setManualImage(GL_TEXTURE_2D, 0, GL_R16F,
                                   sAtlasIntegralWidth, sAtlasIntegralHeight,
                                   GL_RED, GL_FLOAT, NULL, false);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    if (sAtlasFBO == 0)
    {
        glGenFramebuffers(1, &sAtlasFBO);
    }
}

void ASVolumetricLighting::releaseAtlasIntegralAttachments()
{
    if (sAtlasIntegralTex[0] != 0 || sAtlasIntegralTex[1] != 0)
    {
        glDeleteTextures(2, sAtlasIntegralTex);
        sAtlasIntegralTex[0] = 0;
        sAtlasIntegralTex[1] = 0;
    }
    if (sAtlasFBO != 0)
    {
        glDeleteFramebuffers(1, &sAtlasFBO);
        sAtlasFBO = 0;
    }
}

void ASVolumetricLighting::releaseResources()
{
    sVolumetricTarget.release();
    sTransparencyAtlas.release();
    releaseAtlasIntegralAttachments();
    sAtlasConsumerSeen = true;
    sAtlasProducedThisFrame = false;
    sAtlasUnusedFrames = 0;
#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
    resetVolumetricGpuTiming();
#endif
}

void ASVolumetricLighting::bindTransparencyAtlas(LLGLSLShader& shader)
{
    static LLStaticHashedString atlas_sampler("asVolumetricAtlas");
    static LLStaticHashedString atlas_enabled("asVolumetricEnabled");
    const bool atlas_consumer = isEnabled() && getDebugMode() == 0 &&
        !gCubeSnapshot && !LLPipeline::sRenderingHUDs;
    sAtlasConsumerSeen = sAtlasConsumerSeen || atlas_consumer;
    const bool enabled = atlas_consumer && sAtlasProducedThisFrame &&
        sShadersLoaded && sTransparencyAtlas.isComplete();
    shader.uniform1i(atlas_enabled, enabled ? 1 : 0);
    if (enabled)
    {
        const S32 location = shader.getUniformLocation(atlas_sampler);
        const GLint channel = shader.mActiveTextureChannels;

        // Generic/indexed alpha material submission reuses link-mapped units
        // after shader binding, so the atlas uses the proven appended channel.
        if (location > -1 && channel >= 0 && channel < gGLManager.mNumTextureImageUnits)
        {
            glUniform1i(location, channel);
            gGL.getTexUnit(channel)->bindManual(sTransparencyAtlas.getUsage(),
                                                sTransparencyAtlas.getTexture(0));
            gGL.getTexUnit(channel)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
            gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        }

        shader.uniform1f(LLStaticHashedString("scatter_albedo"), getScatterAlbedo());
        shader.uniform1f(LLStaticHashedString("scatter_asymmetry"),
                          getScatterAsymmetry(isVolumetricSunSource()));
        shader.uniform1f(LLStaticHashedString("scatter_density"), getScatterDensity());

        // Water is a real surface beyond the atlas's 128 m sky cutoff, so it
        // evaluates bounded scene extinction itself instead of consuming the
        // atlas alpha's sky fade. Other consumers optimize this uniform out.
        const LLVector3 camera_pos = LLViewerCamera::getInstance()->getOrigin();
        const F32 ground_height = LLWorld::instance().resolveLandHeightAgent(camera_pos);
        const F32 camera_altitude = camera_pos.mV[VZ] - ground_height;
        const F32 altitude_fade = 1.f - llclamp((camera_altitude - 10.f) / 90.f,
                                                0.f, 1.f);
        shader.uniform1f(LLStaticHashedString("asVolumetricSceneDensity"),
                         getScatterDensity() * altitude_fade);
    }
}

S32 ASVolumetricLighting::getSampleCount()
{
    // The explicit quality control keeps the default affordable while letting
    // users opt into a denser march independently of shadow-map quality.
    static LLCachedControl<bool> high_quality(gSavedSettings,
        "RenderVolumetricLightingHighQuality", false);
    static LLCachedControl<S32> sample_override(gSavedSettings,
        "RenderVolumetricLightingSampleCountOverride", 0);
    if (sample_override != 0)
    {
        return llclamp((S32)sample_override, 4, 32);
    }
    return high_quality ? 32 : 16;
}

F32 ASVolumetricLighting::getScatterAlbedo()
{
    // Single-scattering albedo fraction. Combined with density and the fixed
    // BRIGHTNESS_SCALE shader constant to derive final scatter brightness -
    // see the formula comment in asVolumetricLightF.glsl.
    static LLCachedControl<F32> albedo(gSavedSettings, "RenderVolumetricLightingAlbedo", 0.35f);
    return llclamp((F32)albedo, 0.f, 1.f);
}

F32 ASVolumetricLighting::getScatterAsymmetry(bool sun_up)
{
    // Henyey-Greenstein g parameter; positive values bias scatter toward the
    // view direction (forward scattering), matching how sunbeams look when
    // looking roughly toward the sun/moon. Separate settings per source:
    // confirmed by the user that the moon's much dimmer light wants a
    // sharper forward bias (0.7) to show a visible sky gradient, while the
    // sun is bright enough that the same g looks overblown - 0.4 reads
    // better for the sun.
    if (sun_up)
    {
        static LLCachedControl<F32> sun_asymmetry(gSavedSettings, "RenderVolumetricLightingSunAsymmetry", 0.4f);
        return sun_asymmetry;
    }
    static LLCachedControl<F32> moon_asymmetry(gSavedSettings, "RenderVolumetricLightingAsymmetry", 0.7f);
    return moon_asymmetry;
}

F32 ASVolumetricLighting::getScatterDensity()
{
    // Beer-Lambert per-view-space-metre coefficient (formerly "Extinction").
    // Damps distant in-scatter without changing directional-light/shadow
    // geometry, and now also directly scales scatter brightness together
    // with albedo (see BRIGHTNESS_SCALE in asVolumetricLightF.glsl).
    static LLCachedControl<F32> density(gSavedSettings,
        "RenderVolumetricLightingDensity", 0.012f);
    return llmax((F32)density, 0.f);
}

S32 ASVolumetricLighting::getDebugMode()
{
    static LLCachedControl<S32> debug_mode(gSavedSettings, "RenderVolumetricLightingDebug", 0);
    return debug_mode;
}

void ASVolumetricLighting::applyDirectionalInvariants(LLGLSLShader& shader,
                                                       LLPipeline& pipeline,
                                                       bool sun_source)
{
    LLVector3 active_direction(sun_source ? pipeline.mTransformedSunDir
                                          : pipeline.mTransformedMoonDir);
    active_direction.normVec();

    LLColor3 active_color = sun_source ? LLColor3(pipeline.mSunDiffuse.mV)
                                       : LLColor3(pipeline.mMoonDiffuse.mV);
    if (sun_source)
    {
        static LLCachedControl<bool> auto_adjust(gSavedSettings,
                                                  "RenderSkyAutoAdjustLegacy", false);
        static LLCachedControl<F32> color_scale(gSavedSettings,
                                                 "RenderSkyAutoAdjustSunColorScale", 1.f);
        const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (auto_adjust && sky && sky->canAutoAdjust())
        {
            active_color *= (F32)color_scale;
        }
    }
    else
    {
        const LLColor4 tint = gSavedSettings.getColor4("ASMoonHorizonTint");
        const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        const F32 elevation = sky ? sky->getMoonDirection().mV[VZ] : 1.f;
        const F32 tint_height = sinf(llclamp(
            gSavedSettings.getF32("ASMoonHorizonTintAngle"), 0.5f, 90.f) * DEG_TO_RAD);
        const F32 height_t = llclamp(llmax(elevation, 0.f) / tint_height, 0.f, 1.f);
        const F32 smooth_height = height_t * height_t * (3.f - 2.f * height_t);
        const F32 tint_amount = (1.f - smooth_height) * llclamp(
            gSavedSettings.getF32("ASMoonHorizonTintStrength"), 0.f, 1.f);
        for (S32 component = 0; component < 3; ++component)
        {
            const F32 tint_component = llclamp(tint.mV[component], 0.f, 1.f);
            active_color.mV[component] *= 1.f +
                (tint_component - 1.f) * tint_amount;
        }
        active_color *= getMoonPhaseIlluminatedFraction();
    }

    constexpr F32 CELESTIAL_ANGULAR_RADIUS = 0.0372f;
    shader.uniform3fv(LLStaticHashedString("as_active_light_dir"), 1,
                      active_direction.mV);
    shader.uniform3fv(LLStaticHashedString("as_active_light_color"), 1,
                      active_color.mV);
    shader.uniform2f(LLStaticHashedString("as_disc_sin_cos"),
                     sinf(CELESTIAL_ANGULAR_RADIUS),
                     cosf(CELESTIAL_ANGULAR_RADIUS));
}

void ASVolumetricLighting::renderLocalLights(LLPipeline& pipeline)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "RenderVolumetricLocalLights", false);
    static LLCachedControl<F32> intensity(gSavedSettings, "RenderVolumetricLocalLightsIntensity", 0.35f);
    static LLCachedControl<S32> max_lights(gSavedSettings, "RenderVolumetricLocalLightsMaxCount", 8);

    const S32 limit = llclamp((S32)max_lights, 0, MAX_VOLUMETRIC_LOCAL_LIGHTS);
    const S32 debug_mode = getDebugMode();
    if (!enabled || limit == 0 || intensity <= 0.f || (debug_mode != 0 && debug_mode < 8))
    {
        return;
    }

    std::vector<LocalLight> lights;
    lights.reserve(pipeline.mNearbyLights.size());

    for (LLPipeline::light_set_t::const_iterator iter = pipeline.mNearbyLights.begin();
         iter != pipeline.mNearbyLights.end(); ++iter)
    {
        LLDrawable* drawable = iter->drawable;
        LLVOVolume* volume = drawable ? drawable->getVOVolume() : nullptr;
        if (!volume || (volume->isAttachment() && !LLPipeline::sRenderAttachedLights))
        {
            continue;
        }

        F32 radius = volume->getLightRadius() * 1.5f;
        LLColor3 color = volume->getLightLinearColor();
        F32 fade = iter->fade >= 0.f ? iter->fade / LIGHT_FADE_TIME
                                    : 1.f + iter->fade / LIGHT_FADE_TIME;
        color *= llclamp(fade, 0.f, 1.f);
        if (radius <= 0.001f || color.magVecSquared() < 0.001f)
        {
            continue;
        }

        LLVector3 center = drawable->getPositionAgent();
        F32 brightness = color.mV[0] * 0.2126f + color.mV[1] * 0.7152f + color.mV[2] * 0.0722f;
        F32 score = brightness * radius * radius / (1.f + llmax(iter->dist, 0.f));
        lights.push_back({ LLVector4(center, radius),
                           LLVector4(color.mV[0], color.mV[1], color.mV[2],
                                     volume->getLightFalloff(VOLUMETRIC_LOCAL_LIGHT_FALLOFF)),
                           score });
    }

    std::sort(lights.begin(), lights.end(), [](const LocalLight& a, const LocalLight& b)
    {
        return a.score > b.score;
    });
    if ((S32)lights.size() > limit)
    {
        lights.resize(limit);
    }
    if (lights.empty())
    {
        return;
    }

    // Each light's center is the same for every fragment of this full-screen
    // draw, so transform agent space -> view space once here on the CPU
    // instead of repeating `modelview_matrix * vec4(center, 1.0)` per light
    // per fragment in the shader. Radius is a scale-invariant scalar under
    // this rotation+translation matrix, so it is copied through unchanged.
    const LLMatrix4& modelview = LLViewerCamera::getInstance()->getModelview();
    LLVector4 centers[MAX_VOLUMETRIC_LOCAL_LIGHTS];
    LLVector4 colors[MAX_VOLUMETRIC_LOCAL_LIGHTS];
    for (S32 i = 0; i < (S32)lights.size(); ++i)
    {
        LLVector3 agent_center(lights[i].center_radius.mV[VX],
                               lights[i].center_radius.mV[VY],
                               lights[i].center_radius.mV[VZ]);
        LLVector3 view_center = agent_center * modelview;
        centers[i] = LLVector4(view_center, lights[i].center_radius.mV[VW]);
        colors[i] = lights[i].color_falloff;
    }

    // Bind/flush sVolumetricTarget only now that a draw is actually going to
    // happen - every early return above (disabled, no candidates, debug mode
    // excluded) skips this FBO bind/flush pair entirely, which matters since
    // RenderVolumetricLocalLights defaults off.
    LL_PROFILE_GPU_ZONE("AS volumetric local lights");
#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
    sLocalLightGpuTimer.begin();
#endif
    sVolumetricTarget.bindTarget();

    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ADD);
    pipeline.bindDeferredShader(gASVolumetricLocalLightProgram);
    gASVolumetricLocalLightProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH,
                                                &pipeline.mRT->deferredScreen, true);
    gASVolumetricLocalLightProgram.uniform1i(LLStaticHashedString("local_light_count"), (S32)lights.size());
    gASVolumetricLocalLightProgram.uniform4fv(LLStaticHashedString("local_light"), (S32)lights.size(), centers[0].mV);
    gASVolumetricLocalLightProgram.uniform4fv(LLStaticHashedString("local_light_color"), (S32)lights.size(), colors[0].mV);
    gASVolumetricLocalLightProgram.uniform1f(LLStaticHashedString("local_light_intensity"), intensity);
    gASVolumetricLocalLightProgram.uniform1i(LLStaticHashedString("debug_mode"), debug_mode);
    gGL.syncMatrices();
    gGL.setColorMask(true, false);
    pipeline.mScreenTriangleVB->setBuffer();
    pipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    gGL.setColorMask(true, true);
    pipeline.unbindDeferredShader(gASVolumetricLocalLightProgram);

    sVolumetricTarget.flush();
#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
    sLocalLightGpuTimer.end();
#endif
}

// Builds the transparency atlas one tile (slice) at a time instead of all 16
// in a single full-resolution draw, so each slice computes only its own new
// depth segment rather than redundantly re-summing every earlier segment's
// shadow sampling (the previous version made the deepest tile alone repeat
// all 16 segments' work, at full screen resolution, every frame - this was
// the single largest new GPU cost in the feature). See
// asVolumetricAtlasF.glsl's file header for the shader-side half of this.
//
// Every tile written to sTransparencyAtlas is still exactly the same final,
// independently valid light_color * clamped-scatter value the old version
// produced for that slice - alpha/material consumers are completely
// unaffected by this restructure, only the cost of producing that value
// changes. sAtlasIntegralTex[0]/[1] are a private implementation detail
// never sampled by anything outside this function.
bool ASVolumetricLighting::renderTransparencyAtlas(LLPipeline& pipeline,
                                                     F32 attenuate_scene_strength)
{
    if (sAtlasFBO == 0 || sAtlasIntegralTex[0] == 0 || sAtlasIntegralTex[1] == 0)
    {
        return false;
    }

    const U32 atlas_tex = sTransparencyAtlas.getTexture(0);
    const U32 atlas_width = sTransparencyAtlas.getWidth();
    const U32 atlas_height = sTransparencyAtlas.getHeight();

    const S32 debug_mode = getDebugMode();

    LLGLDisable blend(GL_BLEND);
    LLGLEnable scissor(GL_SCISSOR_TEST);

    // This function bypasses LLRenderTarget's own bindTarget()/flush() for
    // the duration of the 16-slice loop (LLRenderTarget has no API for
    // "swap one MRT attachment's texture between draws" - see the header
    // comment on sAtlasIntegralTex in asvolumetriclighting.h), so its static
    // FBO/viewport bookkeeping (sCurFBO/sCurResX/sCurResY, asserted against
    // elsewhere, e.g. LLRenderTarget::flush()) must be saved and restored by
    // hand rather than left pointing at whatever this function's raw GL
    // calls last touched.
    const U32 saved_fbo = LLRenderTarget::sCurFBO;
    const U32 saved_res_x = LLRenderTarget::sCurResX;
    const U32 saved_res_y = LLRenderTarget::sCurResY;

    glBindFramebuffer(GL_FRAMEBUFFER, sAtlasFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, atlas_tex, 0);
    glViewport(0, 0, atlas_width, atlas_height);

    static const GLenum draw_buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, draw_buffers);

    // Slice 0's shader draw never samples previous_slice_integral (guarded
    // by slice_index > 0 in the shader), so leaving buffer 0 uninitialized
    // going into slice 0 is safe - it is only ever a write target that draw.
    S32 read_buffer = 1;
    S32 write_buffer = 0;

    pipeline.bindDeferredShader(gASVolumetricAtlasProgram);
    const bool sun_source = isVolumetricSunSource();
    gASVolumetricAtlasProgram.uniform1f(LLStaticHashedString("scatter_albedo"), getScatterAlbedo());
    gASVolumetricAtlasProgram.uniform1f(LLStaticHashedString("scatter_asymmetry"),
                                         getScatterAsymmetry(sun_source));
    gASVolumetricAtlasProgram.uniform1f(LLStaticHashedString("scatter_density"), getScatterDensity());
    // Scaled by the altitude fade so the atlas's baked-in scene transmittance
    // (sampled by foliage/glass/water) fades out at altitude in step with
    // the opaque composite's sceneDensity, instead of staying at full
    // density while the opaque darkening fades away - see renderPass()'s
    // attenuate_scene_strength computation for the full rationale.
    gASVolumetricAtlasProgram.uniform1f(LLStaticHashedString("transmittance_density"),
                                          getScatterDensity() * attenuate_scene_strength);
    // Mode 11 shows the REAL (non-debug) atlas's alpha channel, not a special
    // diagnostic encoding - transmittance is already a naturally-visible
    // [0,1] grayscale value, unlike the dim raw scatter mode 10 amplifies.
    gASVolumetricAtlasProgram.uniform1i(LLStaticHashedString("atlas_debug"), debug_mode == 10 ? 1 : 0);
    gASVolumetricAtlasProgram.uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_source ? 1 : 0);
    applyDirectionalInvariants(gASVolumetricAtlasProgram, pipeline, sun_source);
    applyMoonAppearance(gASVolumetricAtlasProgram);

    // This shader-specific sampler has no predefined mTexture[] slot. Keep
    // it on the appended channel proven by the working atlas implementation
    // rather than disturbing any link-assigned deferred/shadow sampler unit.
    static const LLStaticHashedString previous_slice_sampler("previous_slice_integral");
    const S32 previous_slice_channel = gASVolumetricAtlasProgram.mActiveTextureChannels;
    const bool have_previous_slice_channel =
        gASVolumetricAtlasProgram.getUniformLocation(previous_slice_sampler) > -1 &&
        previous_slice_channel >= 0 &&
        previous_slice_channel < gGLManager.mNumTextureImageUnits;
    if (!have_previous_slice_channel)
    {
        pipeline.unbindDeferredShader(gASVolumetricAtlasProgram);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
        glViewport(0, 0, saved_res_x, saved_res_y);
        LLRenderTarget::sCurFBO = saved_fbo;
        LLRenderTarget::sCurResX = saved_res_x;
        LLRenderTarget::sCurResY = saved_res_y;
        return false;
    }
    gASVolumetricAtlasProgram.uniform1i(previous_slice_sampler, previous_slice_channel);

    for (S32 slice_index = 0; slice_index < 16; ++slice_index)
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                                sAtlasIntegralTex[write_buffer], 0);

        // Exact integer tile boundaries rather than a single floored
        // "tile_width" constant: atlas_width/height are now always rounded
        // up to a multiple of 4 at allocation time (see allocateResources()),
        // but computing each edge from width*tile/GRID here as well removes
        // any remaining ambiguity and matches the shader's own tile-local
        // UV decode exactly, with no assumption that width % 4 == 0 baked
        // into a separate, potentially-inconsistent tile size elsewhere.
        const S32 tile_x = slice_index % 4;
        const S32 tile_y = slice_index / 4;
        const S32 x0 = (S32)((U64)atlas_width * tile_x / 4);
        const S32 x1 = (S32)((U64)atlas_width * (tile_x + 1) / 4);
        const S32 y0 = (S32)((U64)atlas_height * tile_y / 4);
        const S32 y1 = (S32)((U64)atlas_height * (tile_y + 1) / 4);
        glScissor(x0, y0, x1 - x0, y1 - y0);

        gASVolumetricAtlasProgram.uniform1i(LLStaticHashedString("slice_index"), slice_index);
        if (slice_index > 0)
        {
            gGL.getTexUnit(previous_slice_channel)->bindManual(LLTexUnit::TT_TEXTURE,
                                                                 sAtlasIntegralTex[read_buffer]);
        }

        pipeline.mScreenTriangleVB->setBuffer();
        pipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        std::swap(read_buffer, write_buffer);
    }

    pipeline.unbindDeferredShader(gASVolumetricAtlasProgram);

    // Detach both MRT color attachments before restoring the caller's FBO -
    // leaving atlas_tex/sAtlasIntegralTex bound to sAtlasFBO's attachment
    // points would keep them "in use as a framebuffer attachment" from GL's
    // point of view even while unbound, which is exactly the read-while-
    // attached hazard this whole ping-pong scheme exists to avoid for the
    // NEXT frame's slice 0 draw (which samples the real atlas via
    // bindTransparencyAtlas() elsewhere, and reuses these same two integral
    // textures again next frame).
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
    glViewport(0, 0, saved_res_x, saved_res_y);
    LLRenderTarget::sCurFBO = saved_fbo;
    LLRenderTarget::sCurResX = saved_res_x;
    LLRenderTarget::sCurResY = saved_res_y;
    return true;
}

void ASVolumetricLighting::renderPass(LLPipeline& pipeline, LLRenderTarget& screen)
{
    if (!isEnabled() || !sShadersLoaded || gCubeSnapshot || LLPipeline::sRenderingHUDs)
    {
        return;
    }

#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
    // Never label an average with a configuration different from the frames
    // it contains. Dropping pending diagnostic queries on a live quality or
    // sample-count change is safe and avoids a mixed transition interval.
    static bool timing_initialized = false;
    static bool timing_high_quality = false;
    static S32 timing_sample_count = 0;
    static F32 timing_blur_strength = 0.f;
    static F32 timing_blur_radius = 1.f;
    static F32 timing_blue_noise_strength = 0.f;
    const bool current_high_quality =
        gSavedSettings.getBOOL("RenderVolumetricLightingHighQuality");
    const S32 current_sample_count = getSampleCount();
    const F32 current_blur_strength = llclamp(
        gSavedSettings.getF32("RenderVolumetricLightingBlurStrength"), 0.f, 1.f);
    const F32 current_blur_radius = llclamp(
        gSavedSettings.getF32("RenderVolumetricLightingBlurRadius"), 1.f, 2.f);
    const F32 current_blue_noise_strength = llclamp(
        gSavedSettings.getF32("RenderVolumetricLightingBlueNoiseStrength"), 0.f, 1.f);
    if (timing_initialized &&
        (current_high_quality != timing_high_quality ||
         current_sample_count != timing_sample_count ||
         current_blur_strength != timing_blur_strength ||
         current_blur_radius != timing_blur_radius ||
         current_blue_noise_strength != timing_blue_noise_strength))
    {
        resetVolumetricGpuTiming();
    }
    timing_initialized = true;
    timing_high_quality = current_high_quality;
    timing_sample_count = current_sample_count;
    timing_blur_strength = current_blur_strength;
    timing_blur_radius = current_blur_radius;
    timing_blue_noise_strength = current_blue_noise_strength;
#endif

    // Apply quality changes live. The caller flushes screen before entering
    // this pass, so resizing the AS-owned source target here is safe.
    static LLCachedControl<bool> high_quality(gSavedSettings,
        "RenderVolumetricLightingHighQuality", false);
    const U32 desired_width = llmax((U32)1,
        high_quality ? screen.getWidth() : screen.getWidth() / 2);
    const U32 desired_height = llmax((U32)1,
        high_quality ? screen.getHeight() : screen.getHeight() / 2);
    if (sVolumetricTarget.getWidth() != desired_width ||
        sVolumetricTarget.getHeight() != desired_height)
    {
        sVolumetricTarget.release();
        sVolumetricTarget.allocate(desired_width, desired_height, GL_RGBA16F);
    }

    if (!sVolumetricTarget.isComplete() || !sTransparencyAtlas.isComplete())
    {
        // Resource allocation is synchronous today, but do not leave a
        // previous frame's atlas marked current if either target becomes
        // incomplete during a future lifecycle refactor.
        sAtlasProducedThisFrame = false;
        return;
    }

    LL_PROFILE_GPU_ZONE("as volumetric lighting");

    // Both draws are full-screen triangles with no meaningful depth test;
    // explicitly force depth off and blend off (except where the composite
    // deliberately re-enables it below) rather than relying on whatever
    // state renderFinalize() happened to be in when it called us - that
    // state is otherwise unmanaged here and was previously left to chance.
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable   cull(GL_CULL_FACE);

    S32 debug_mode = getDebugMode();
    const bool sun_source = isVolumetricSunSource();

    // Atlas consumers are submitted later in the frame, so this flag records
    // demand observed during the previous frame. Preserve two full unused
    // frames before skipping to avoid flicker from transient visibility or
    // draw-pool changes. Debug modes 10/11 inspect the atlas itself and must
    // always produce it. Reset before forward rendering so later bindings
    // accumulate demand for the next frame.
    if (sAtlasConsumerSeen)
    {
        sAtlasUnusedFrames = 0;
    }
    else
    {
        sAtlasUnusedFrames = llmin(sAtlasUnusedFrames + 1, (U32)2);
    }
    const bool produce_atlas = debug_mode == 10 || debug_mode == 11 ||
        sAtlasUnusedFrames < 2;
    sAtlasConsumerSeen = false;
    sAtlasProducedThisFrame = false;

    // Fade scene-attenuating transmittance out with camera altitude above the
    // ground beneath it: a wide, zoomed-out or high-altitude view spans far
    // more distant terrain/objects at once than a ground-level view, so the
    // same per-pixel density reads as a much larger, more oppressive dark
    // area the higher the camera sits. Full strength at/below
    // ALTITUDE_FADE_START, fading to none by ALTITUDE_FADE_END; both are
    // agent-space metres above the terrain directly below the camera, not
    // sea level, so this still reads correctly over hills/valleys. Computed
    // once here and threaded into both the opaque composite AND the
    // transparency atlas (renderTransparencyAtlas() below) - transparent
    // materials (foliage, glass, water) sample the atlas's own baked-in
    // transmittance, which is a completely separate code path from the
    // composite's sceneDensity; scaling only one of the two left the other
    // at full density at altitude, an inconsistency between opaque and
    // transparent surfaces that looked like foliage/water going darker than
    // everything else the higher the camera flew.
    const LLVector3 camera_pos = LLViewerCamera::getInstance()->getOrigin();
    const F32 ground_height = LLWorld::instance().resolveLandHeightAgent(camera_pos);
    const F32 camera_altitude = camera_pos.mV[VZ] - ground_height;
    const F32 ALTITUDE_FADE_START = 10.f;
    const F32 ALTITUDE_FADE_END = 100.f;
    const F32 attenuate_scene_strength = 1.f - llclamp(
        (camera_altitude - ALTITUDE_FADE_START) / (ALTITUDE_FADE_END - ALTITUDE_FADE_START),
        0.f, 1.f);

    // ---- Raymarch pass: sample shadow occlusion along the view ray -------
    {
        LL_PROFILE_GPU_ZONE("AS volumetric directional");
#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
        sDirectionalGpuTimer.begin();
#endif
        LLGLDisable blend(GL_BLEND);

        sVolumetricTarget.bindTarget();
        sVolumetricTarget.clear(GL_COLOR_BUFFER_BIT);

        if (debug_mode < 8)
        {
            pipeline.bindDeferredShader(gASVolumetricLightProgram);

            // Bind the shared depth attachment explicitly. This is the same path
            // used by the working deferred post-process passes and avoids relying
            // on the generic G-buffer attachment binding for a depth texture.
            gASVolumetricLightProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH,
                                                   &pipeline.mRT->deferredScreen,
                                                   true);
            if (sBlueNoiseImage.isNull())
            {
                // MIPMAP_NO + explicit uncompressed GL_RGBA/GL_RGBA keep the
                // source PNG's per-texel byte values intact; BOOST_UI forces
                // an immediate full-resolution decode instead of the
                // progressive discard-level streaming used for world assets,
                // any of which would silently corrupt the blue-noise
                // distribution and reintroduce spatial correlation.
                sBlueNoiseImage = LLViewerTextureManager::getFetchedTextureFromFile(
                    "as/as_blue_noise.png", FTT_LOCAL_FILE, MIPMAP_NO,
                    LLGLTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE,
                    GL_RGBA, GL_RGBA);
                if (sBlueNoiseImage.notNull())
                {
                    // Drives the desired discard level to 0: the streamer
                    // otherwise sizes texel-per-pixel expectations against
                    // on-screen draw size, which this texture never has since
                    // it is never drawn as a quad.
                    sBlueNoiseImage->setKnownDrawSize(
                        sBlueNoiseImage->getFullWidth(),
                        sBlueNoiseImage->getFullHeight());
                }
            }
            if (sBlueNoiseImage.notNull())
            {
                gASVolumetricLightProgram.bindTexture(
                    "blueNoiseMap", sBlueNoiseImage.get());
            }
            static LLCachedControl<F32> blue_noise_strength(gSavedSettings,
                "RenderVolumetricLightingBlueNoiseStrength", 0.f);
            gASVolumetricLightProgram.uniform1f(
                LLStaticHashedString("blueNoiseStrength"),
                llclamp((F32)blue_noise_strength, 0.f, 1.f));

            gASVolumetricLightProgram.uniform1i(LLStaticHashedString("sample_count"), getSampleCount());
            gASVolumetricLightProgram.uniform1f(LLStaticHashedString("scatter_albedo"), getScatterAlbedo());
            gASVolumetricLightProgram.uniform1f(LLStaticHashedString("scatter_asymmetry"),
                                                 getScatterAsymmetry(sun_source));
            gASVolumetricLightProgram.uniform1f(LLStaticHashedString("scatter_density"), getScatterDensity());
            gASVolumetricLightProgram.uniform1i(LLStaticHashedString("debug_mode"), debug_mode);
            // bindDeferredShader() does not set this; renderDeferredLighting()'s
            // callers normally do it per-shader (see softenLightF's soften_shader).
            gASVolumetricLightProgram.uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_source ? 1 : 0);
            applyDirectionalInvariants(gASVolumetricLightProgram, pipeline, sun_source);
            applyMoonAppearance(gASVolumetricLightProgram);

            pipeline.mScreenTriangleVB->setBuffer();
            pipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            gASVolumetricLightProgram.unbindTexture("blueNoiseMap");
            pipeline.unbindDeferredShader(gASVolumetricLightProgram);
        }

        sVolumetricTarget.flush();
#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
        sDirectionalGpuTimer.end();
#endif
    }

    // Build 16 cumulative camera-to-depth integrals in a 4x4 atlas only while
    // late transparent/fullbright/water submission has recently consumed it.
    if (produce_atlas)
    {
        LL_PROFILE_GPU_ZONE("AS volumetric atlas");
#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
        sAtlasGpuTimer.begin();
#endif
        sAtlasProducedThisFrame = renderTransparencyAtlas(
            pipeline, attenuate_scene_strength);
#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
        sAtlasGpuTimer.end();
#endif
    }

    // Add the optional, explicitly unshadowed local-light fog contribution.
    // Sphere/ray intersection in the shader confines work and illumination to
    // each light volume; the candidate count is bounded in preferences.
    // renderLocalLights() binds/flushes sVolumetricTarget itself, but only
    // once it knows it actually has a draw to make - RenderVolumetricLocalLights
    // defaults off, so the common frame skips this FBO bind/flush pair
    // entirely rather than paying for it around a no-op.
    renderLocalLights(pipeline);

    // ---- Composite: upsample scatter, attenuate scene by transmittance ----
    //
    // Mode 0 (the real composite) attenuates screen's existing contents by
    // transmittance before adding scatter (scene * T + scatter). The shader
    // writes scatter to RGB and T to alpha; destination blending applies the
    // equation directly without a scene-copy texture or extra draw.
    //
    // Any non-zero debug mode replaces screen outright with the raw target
    // contents instead, so the signal can be inspected without going through
    // tonemap/composite math. Mode 1's target holds scatter (color+intensity
    // applied); mode 2's holds raw grayscale occlusion - either way, once it
    // is in sVolumetricTarget this composite step just needs to show it
    // verbatim rather than add it on top of the already fully-lit scene
    // (which is what made mode 2 read as "all white": it was adding raw
    // occlusion onto an already bright tonemapped frame).
    auto draw_composite = [&](LLRenderTarget& destination,
                              LLRenderTarget& composite_source,
                              bool depth_aware,
                              bool replace,
                              bool show_alpha_channel = false,
                              F32 attenuate_scene_strength = 0.f)
    {
        LL_PROFILE_GPU_ZONE("AS volumetric composite");
#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
        sCompositeGpuTimer.begin();
#endif
        const bool attenuate_scene = attenuate_scene_strength > 0.f;

        destination.bindTarget();
        gASVolumetricCompositeProgram.bind();
        gASVolumetricCompositeProgram.uniform1i(
            LLStaticHashedString("showAlphaChannel"), show_alpha_channel ? 1 : 0);
        static LLCachedControl<F32> scatter_blur(gSavedSettings,
            "RenderVolumetricLightingBlurStrength", 1.f);
        gASVolumetricCompositeProgram.uniform1f(
            LLStaticHashedString("scatterBlurStrength"),
            (debug_mode == 0 || debug_mode == 1)
                ? llclamp((F32)scatter_blur, 0.f, 1.f) : 0.f);
        static LLCachedControl<F32> scatter_blur_radius(gSavedSettings,
            "RenderVolumetricLightingBlurRadius", 2.f);
        gASVolumetricCompositeProgram.uniform1f(
            LLStaticHashedString("scatterBlurRadius"),
            llclamp((F32)scatter_blur_radius, 1.f, 2.f));
        if (show_alpha_channel)
        {
            // Cancel out postDeferredTonemap.glsl's later exposure multiply
            // (see the shader-side comment) so the debug view reads as the
            // raw stored scalar regardless of scene brightness/RenderExposure.
            gASVolumetricCompositeProgram.bindTexture(
                LLShaderMgr::EXPOSURE_MAP, &pipeline.mExposureMap);
            static LLCachedControl<F32> exposure(gSavedSettings, "RenderExposure", 1.f);
            gASVolumetricCompositeProgram.uniform1f(
                LLStaticHashedString("debugExposure"), llclamp(exposure(), 0.5f, 4.f));
        }

        gASVolumetricCompositeProgram.uniform1i(
            LLStaticHashedString("attenuateScene"), attenuate_scene ? 1 : 0);
        if (attenuate_scene)
        {
            // Scaling density itself (rather than gating attenuation on/off)
            // makes transmittance exp(-density*strength*dist) vary smoothly
            // as strength ramps 0->1 - at strength=0 this is exp(0)=1, i.e.
            // exactly the same "no attenuation" result as the disabled case,
            // so there is no seam where the fade begins.
            gASVolumetricCompositeProgram.uniform1f(
                LLStaticHashedString("sceneDensity"),
                getScatterDensity() * attenuate_scene_strength);
        }

        // Clamp explicitly rather than assume this render target's default
        // wrap mode is clamp-to-edge.
        S32 emissive_channel = gASVolumetricCompositeProgram.bindTexture(
            LLShaderMgr::DEFERRED_EMISSIVE, &composite_source);
        if (emissive_channel > -1)
        {
            gGL.getTexUnit(emissive_channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        }

        // The normal half-resolution composite uses full-resolution opaque
        // depth to avoid enlarging its silhouette edges into visible stairs.
        // Diagnostic modes remain plain samples so their output is unchanged.
        gASVolumetricCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH,
                                                   &pipeline.mRT->deferredScreen,
                                                   true);
        S32 normal_channel = -1;
        if (depth_aware)
        {
            // The normal guide is only useful for the half-resolution
            // upsample. Bind deferredScreen attachment 2 explicitly; the
            // generic LLRenderTarget overload would bind attachment 0.
            normal_channel = gASVolumetricCompositeProgram.enableTexture(
                LLShaderMgr::NORMAL_MAP, pipeline.mRT->deferredScreen.getUsage());
            if (normal_channel > -1)
            {
                pipeline.mRT->deferredScreen.bindTexture(
                    2, normal_channel, LLTexUnit::TFO_POINT);
                gGL.getTexUnit(normal_channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }
        }
        gASVolumetricCompositeProgram.uniform2f(
            LLStaticHashedString("emissiveRectDelta"),
            1.f / (F32)composite_source.getWidth(),
            1.f / (F32)composite_source.getHeight());
        gASVolumetricCompositeProgram.uniform1i(
            LLStaticHashedString("depthAwareUpsample"), depth_aware ? 1 : 0);

        LLGLEnable blend(GL_BLEND);
        if (attenuate_scene)
        {
            // RGB: scatter * ONE + scene * transmittance. Alpha writes remain
            // masked below so later post-processing sees the original screen
            // alpha, independent of the blend equation's alpha factors.
            gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_SOURCE_ALPHA,
                          LLRender::BF_ZERO, LLRender::BF_ONE);
        }
        else
        {
            gGL.setSceneBlendType(replace ? LLRender::BT_REPLACE : LLRender::BT_ADD);
        }
        // Preserve screen alpha; it is consumed by later post-processing.
        gGL.setColorMask(true, false);
        pipeline.mScreenTriangleVB->setBuffer();
        pipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gGL.setColorMask(true, true);

        // Do not leak temporary scatter/depth bindings into later rendering.
        gASVolumetricCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_EMISSIVE);
        gASVolumetricCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
        if (normal_channel > -1)
        {
            gASVolumetricCompositeProgram.disableTexture(
                LLShaderMgr::NORMAL_MAP, pipeline.mRT->deferredScreen.getUsage());
        }
        if (show_alpha_channel)
        {
            gASVolumetricCompositeProgram.unbindTexture(LLShaderMgr::EXPOSURE_MAP);
        }
        gASVolumetricCompositeProgram.unbind();
        destination.flush();
#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
        sCompositeGpuTimer.end();
#endif
    };

    if (debug_mode == 0)
    {
        const bool needs_depth_upsample =
            sVolumetricTarget.getWidth() != screen.getWidth() ||
            sVolumetricTarget.getHeight() != screen.getHeight();
        draw_composite(screen, sVolumetricTarget, needs_depth_upsample, false, false,
                        attenuate_scene_strength);
    }
    else
    {
        LLRenderTarget& debug_source = (debug_mode == 10 || debug_mode == 11) ?
            sTransparencyAtlas : sVolumetricTarget;
        draw_composite(screen, debug_source, false, true, debug_mode == 11);
    }
#if AS_VOLUMETRIC_PERFORMANCE_LOGGING
    logVolumetricGpuTiming();
#endif
}
