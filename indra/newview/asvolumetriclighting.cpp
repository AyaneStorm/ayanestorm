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

#include "llenvironment.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "lllightconstants.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "lldrawable.h"
#include "llviewercontrol.h"
#include "llvovolume.h"
#include "pipeline.h"

#include <algorithm>
#include <vector>

extern bool gCubeSnapshot;

namespace
{
LLGLSLShader gASVolumetricLightProgram;
LLGLSLShader gASVolumetricLocalLightProgram;
LLGLSLShader gASVolumetricCompositeProgram;
LLGLSLShader gASVolumetricAtlasProgram;

constexpr S32 MAX_VOLUMETRIC_LOCAL_LIGHTS = 64;
constexpr F32 VOLUMETRIC_LOCAL_LIGHT_FALLOFF = 0.5f;

struct LocalLight
{
    LLVector4 center_radius;
    LLVector4 color_falloff;
    F32 score;
};
}

bool ASVolumetricLighting::sSupportChecked = false;
bool ASVolumetricLighting::sSupported = false;
bool ASVolumetricLighting::sShadersLoaded = false;
LLRenderTarget ASVolumetricLighting::sVolumetricTarget;
LLRenderTarget ASVolumetricLighting::sResolvedTarget;
LLRenderTarget ASVolumetricLighting::sTransparencyAtlas;
U32 ASVolumetricLighting::sAtlasIntegralTex[2] = { 0, 0 };
U32 ASVolumetricLighting::sAtlasFBO = 0;
U32 ASVolumetricLighting::sAtlasIntegralWidth = 0;
U32 ASVolumetricLighting::sAtlasIntegralHeight = 0;

// Folded into the shader cache hash in llviewershadermgr.cpp alongside
// FSExactOIT's revision. During active development the shader cache is cleared
// manually, so do not bump this for every edit and trigger an avoidable LTO
// relink. Bump it before distributing a build whose users will retain caches.
const char* ASVolumetricLighting::shaderCacheRevision()
{
    return "as-volumetric-lighting-v13";
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
        && gSavedSettings.getBOOL("RenderVolumetricLighting");
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
    sResolvedTarget.allocate(llmax((U32)1, width), llmax((U32)1, height), GL_RGBA16F);

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
    sResolvedTarget.release();
    sTransparencyAtlas.release();
    releaseAtlasIntegralAttachments();
}

void ASVolumetricLighting::bindTransparencyAtlas(LLGLSLShader& shader)
{
    static LLStaticHashedString atlas_sampler("asVolumetricAtlas");
    static LLStaticHashedString resolved_sampler("asVolumetricFull");
    static LLStaticHashedString atlas_enabled("asVolumetricEnabled");
    const bool enabled = isEnabled() && getDebugMode() == 0 && sShadersLoaded &&
        sTransparencyAtlas.isComplete();
    shader.uniform1i(atlas_enabled, enabled ? 1 : 0);
    if (enabled)
    {
        const S32 location = shader.getUniformLocation(atlas_sampler);
        const S32 resolved_location = shader.getUniformLocation(resolved_sampler);
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

        // Fixed-sampler water removes the exact resolved field and performs a
        // continuous per-fragment foreground march, so it needs no atlas unit.
        GLint resolved_channel = -1;
        if (resolved_location > -1)
        {
            glGetUniformiv(shader.mProgramObject, resolved_location, &resolved_channel);
        }
        if (resolved_location > -1 && resolved_channel >= 0 &&
            resolved_channel < gGLManager.mNumTextureImageUnits &&
            sResolvedTarget.isComplete())
        {
            gGL.getTexUnit(resolved_channel)->bindManual(sResolvedTarget.getUsage(),
                                                         sResolvedTarget.getTexture(0));
            gGL.getTexUnit(resolved_channel)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
            gGL.getTexUnit(resolved_channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        }

        shader.uniform1f(LLStaticHashedString("scatter_intensity"), getScatterIntensity());
        shader.uniform1f(LLStaticHashedString("scatter_asymmetry"), getScatterAsymmetry());
        shader.uniform1f(LLStaticHashedString("scatter_extinction"), getExtinction());
    }
}

S32 ASVolumetricLighting::getSampleCount()
{
    // The explicit quality control keeps the default affordable while letting
    // users opt into a denser march independently of shadow-map quality.
    static LLCachedControl<bool> high_quality(gSavedSettings,
        "RenderVolumetricLightingHighQuality", false);
    return high_quality ? 32 : 16;
}

F32 ASVolumetricLighting::getScatterIntensity()
{
    static LLCachedControl<F32> intensity(gSavedSettings, "RenderVolumetricLightingIntensity", 0.8f);
    return intensity;
}

F32 ASVolumetricLighting::getScatterAsymmetry()
{
    // Henyey-Greenstein g parameter; positive values bias scatter toward the
    // view direction (forward scattering), matching how sunbeams look when
    // looking roughly toward the sun.
    static LLCachedControl<F32> asymmetry(gSavedSettings, "RenderVolumetricLightingAsymmetry", 0.3f);
    return asymmetry;
}

F32 ASVolumetricLighting::getExtinction()
{
    // Beer-Lambert attenuation per view-space metre. This damps distant
    // in-scatter without changing the directional-light or shadow geometry.
    static LLCachedControl<F32> extinction(gSavedSettings,
        "RenderVolumetricLightingExtinction", 0.012f);
    return llmax((F32)extinction, 0.f);
}

S32 ASVolumetricLighting::getDebugMode()
{
    static LLCachedControl<S32> debug_mode(gSavedSettings, "RenderVolumetricLightingDebug", 0);
    return debug_mode;
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

    LLVector4 centers[MAX_VOLUMETRIC_LOCAL_LIGHTS];
    LLVector4 colors[MAX_VOLUMETRIC_LOCAL_LIGHTS];
    for (S32 i = 0; i < (S32)lights.size(); ++i)
    {
        centers[i] = lights[i].center_radius;
        colors[i] = lights[i].color_falloff;
    }

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
void ASVolumetricLighting::renderTransparencyAtlas(LLPipeline& pipeline)
{
    if (sAtlasFBO == 0 || sAtlasIntegralTex[0] == 0 || sAtlasIntegralTex[1] == 0)
    {
        return;
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
    gASVolumetricAtlasProgram.uniform1f(LLStaticHashedString("scatter_intensity"), getScatterIntensity());
    gASVolumetricAtlasProgram.uniform1f(LLStaticHashedString("scatter_asymmetry"), getScatterAsymmetry());
    gASVolumetricAtlasProgram.uniform1f(LLStaticHashedString("scatter_extinction"), getExtinction());
    gASVolumetricAtlasProgram.uniform1i(LLStaticHashedString("atlas_debug"), debug_mode == 10 ? 1 : 0);
    gASVolumetricAtlasProgram.uniform1i(LLShaderMgr::SUN_UP_FACTOR, LLEnvironment::instance().getIsSunUp() ? 1 : 0);

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
        return;
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
}

void ASVolumetricLighting::renderPass(LLPipeline& pipeline, LLRenderTarget& screen)
{
    if (!isEnabled() || !sShadersLoaded || gCubeSnapshot || LLPipeline::sRenderingHUDs)
    {
        return;
    }

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

    if (!sVolumetricTarget.isComplete() || !sResolvedTarget.isComplete() ||
        !sTransparencyAtlas.isComplete())
    {
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

    // ---- Raymarch pass: sample shadow occlusion along the view ray -------
    {
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

            gASVolumetricLightProgram.uniform1i(LLStaticHashedString("sample_count"), getSampleCount());
            gASVolumetricLightProgram.uniform1f(LLStaticHashedString("scatter_intensity"), getScatterIntensity());
            gASVolumetricLightProgram.uniform1f(LLStaticHashedString("scatter_asymmetry"), getScatterAsymmetry());
            gASVolumetricLightProgram.uniform1f(LLStaticHashedString("scatter_extinction"), getExtinction());
            gASVolumetricLightProgram.uniform1i(LLStaticHashedString("debug_mode"), debug_mode);
            // bindDeferredShader() does not set this; renderDeferredLighting()'s
            // callers normally do it per-shader (see softenLightF's soften_shader).
            gASVolumetricLightProgram.uniform1i(LLShaderMgr::SUN_UP_FACTOR, LLEnvironment::instance().getIsSunUp() ? 1 : 0);

            pipeline.mScreenTriangleVB->setBuffer();
            pipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            pipeline.unbindDeferredShader(gASVolumetricLightProgram);
        }

        sVolumetricTarget.flush();
    }

    // Build 16 cumulative camera-to-depth integrals in a 4x4 atlas.
    renderTransparencyAtlas(pipeline);

    // Add the optional, explicitly unshadowed local-light fog contribution.
    // Sphere/ray intersection in the shader confines work and illumination to
    // each light volume; the candidate count is bounded in preferences.
    sVolumetricTarget.bindTarget();
    renderLocalLights(pipeline);
    sVolumetricTarget.flush();

    // ---- Composite: resolve scatter, then blend it into screen -----------
    //
    // Any non-zero debug mode replaces screen outright with the raw target
    // contents instead of blending, so the signal can be inspected without
    // going through tonemap/composite math. Mode 1's target holds scatter
    // (color+intensity applied); mode 2's holds raw grayscale occlusion -
    // either way, once it is in sVolumetricTarget this composite step just
    // needs to show it verbatim rather than add it on top of the already
    // fully-lit scene (which is what made mode 2 read as "all white": it
    // was adding raw occlusion onto an already bright tonemapped frame).
    auto draw_composite = [&](LLRenderTarget& destination,
                              LLRenderTarget& composite_source,
                              bool depth_aware,
                              bool replace)
    {
        destination.bindTarget();
        gASVolumetricCompositeProgram.bind();

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
        gASVolumetricCompositeProgram.uniform2f(
            LLStaticHashedString("emissiveRectDelta"),
            1.f / (F32)composite_source.getWidth(),
            1.f / (F32)composite_source.getHeight());
        gASVolumetricCompositeProgram.uniform1i(
            LLStaticHashedString("depthAwareUpsample"), depth_aware ? 1 : 0);

        LLGLEnable blend(GL_BLEND);
        gGL.setSceneBlendType(replace ? LLRender::BT_REPLACE : LLRender::BT_ADD);
        // Preserve screen alpha. BT_ADD applies ONE,ONE to alpha as well as
        // RGB, and the HDR screen alpha is consumed by later post-processing.
        // Adding the composite shader's alpha every frame corrupts those
        // passes and produces a full-screen whiteout unrelated to scatter RGB.
        gGL.setColorMask(true, false);
        pipeline.mScreenTriangleVB->setBuffer();
        pipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gGL.setColorMask(true, true);

        // Do not leak temporary scatter/depth bindings into later rendering.
        gASVolumetricCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_EMISSIVE);
        gASVolumetricCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
        gASVolumetricCompositeProgram.unbind();
        destination.flush();
    };

    if (debug_mode == 0)
    {
        // Keep the exact full-resolution field added to screen so late water
        // can remove that same field from its refracted framebuffer sample.
        const bool needs_depth_upsample =
            sVolumetricTarget.getWidth() != sResolvedTarget.getWidth() ||
            sVolumetricTarget.getHeight() != sResolvedTarget.getHeight();
        draw_composite(sResolvedTarget, sVolumetricTarget, needs_depth_upsample, true);
        draw_composite(screen, sResolvedTarget, false, false);
    }
    else
    {
        LLRenderTarget& debug_source = debug_mode == 10 ?
            sTransparencyAtlas : sVolumetricTarget;
        draw_composite(screen, debug_source, false, true);
    }
}
