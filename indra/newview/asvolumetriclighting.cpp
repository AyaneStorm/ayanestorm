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
LLRenderTarget ASVolumetricLighting::sTransparencyAtlas;

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

    // Raymarching at full resolution is the dominant cost of this pass; the
    // bilateral-upsample composite hides the loss of half-res detail.
    U32 target_width  = llmax((U32)1, width / 2);
    U32 target_height = llmax((U32)1, height / 2);

    sVolumetricTarget.allocate(target_width, target_height, GL_RGBA16F);
    sTransparencyAtlas.allocate(llmax((U32)4, width), llmax((U32)4, height), GL_RGBA16F);
}

void ASVolumetricLighting::releaseResources()
{
    sVolumetricTarget.release();
    sTransparencyAtlas.release();
}

void ASVolumetricLighting::bindTransparencyAtlas(LLGLSLShader& shader)
{
    static LLStaticHashedString atlas_sampler("asVolumetricAtlas");
    static LLStaticHashedString atlas_enabled("asVolumetricEnabled");
    const bool enabled = isEnabled() && getDebugMode() == 0 && sShadersLoaded &&
        sTransparencyAtlas.isComplete();
    shader.uniform1i(atlas_enabled, enabled ? 1 : 0);
    if (enabled)
    {
        // Custom samplers are present in mUniformMap but not in the reserved
        // mTexture table used by LLGLSLShader::bindTexture(). Assign the first
        // channel after all shader-mapped samplers explicitly; passing a raw
        // GL location to bindTexture(S32) would misinterpret it as a reserved
        // uniform index and bind an unrelated material texture.
        const S32 location = shader.getUniformLocation(atlas_sampler);
        const S32 channel = shader.mActiveTextureChannels;
        if (location > -1 && channel < gGLManager.mNumTextureImageUnits)
        {
            glUniform1i(location, channel);
            gGL.getTexUnit(channel)->bindManual(sTransparencyAtlas.getUsage(),
                                                sTransparencyAtlas.getTexture(0));
            gGL.getTexUnit(channel)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
            gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        }
    }
}

S32 ASVolumetricLighting::getSampleCount()
{
    // Scale down with shadow detail: detail 1 gets a cheaper march than the
    // highest detail level, since both already imply the user favors quality
    // over raw framerate.
    return LLPipeline::RenderShadowDetail >= 2 ? 24 : 16;
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

void ASVolumetricLighting::renderPass(LLPipeline& pipeline, LLRenderTarget& screen)
{
    if (!isEnabled() || !sShadersLoaded || gCubeSnapshot || LLPipeline::sRenderingHUDs)
    {
        return;
    }

    if (!sVolumetricTarget.isComplete() || !sTransparencyAtlas.isComplete())
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

    // Build 16 cumulative camera-to-depth integrals in a 4x4 atlas. Each tile
    // is quarter resolution, so the complete atlas occupies one screen-sized
    // RGBA16F texture and is trilinearly reconstructed by transparent shaders.
    {
        LLGLDisable blend(GL_BLEND);
        sTransparencyAtlas.bindTarget();
        sTransparencyAtlas.clear(GL_COLOR_BUFFER_BIT);
        pipeline.bindDeferredShader(gASVolumetricAtlasProgram);
        gASVolumetricAtlasProgram.uniform1f(LLStaticHashedString("scatter_intensity"), getScatterIntensity());
        gASVolumetricAtlasProgram.uniform1f(LLStaticHashedString("scatter_asymmetry"), getScatterAsymmetry());
        gASVolumetricAtlasProgram.uniform1i(LLStaticHashedString("atlas_debug"), debug_mode == 10 ? 1 : 0);
        gASVolumetricAtlasProgram.uniform1i(LLShaderMgr::SUN_UP_FACTOR, LLEnvironment::instance().getIsSunUp() ? 1 : 0);
        pipeline.mScreenTriangleVB->setBuffer();
        pipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        pipeline.unbindDeferredShader(gASVolumetricAtlasProgram);
        sTransparencyAtlas.flush();
    }

    // Add the optional, explicitly unshadowed local-light fog contribution.
    // Sphere/ray intersection in the shader confines work and illumination to
    // each light volume; the candidate count is bounded in preferences.
    sVolumetricTarget.bindTarget();
    renderLocalLights(pipeline);
    sVolumetricTarget.flush();

    // ---- Composite: blend the (low-res) scatter into screen --------------
    // screen is bound as both the render target and (implicitly, via
    // GL_BLEND BT_ADD) the destination being added into - it must NOT also
    // be bound as a source texture here, so only the volumetric target is
    // sampled.
    //
    // Any non-zero debug mode replaces screen outright with the raw target
    // contents instead of blending, so the signal can be inspected without
    // going through tonemap/composite math. Mode 1's target holds scatter
    // (color+intensity applied); mode 2's holds raw grayscale occlusion -
    // either way, once it is in sVolumetricTarget this composite step just
    // needs to show it verbatim rather than add it on top of the already
    // fully-lit scene (which is what made mode 2 read as "all white": it
    // was adding raw occlusion onto an already bright tonemapped frame).
    screen.bindTarget();

    gASVolumetricCompositeProgram.bind();
    {
        // Clamp explicitly rather than assume this render target's default
        // wrap mode is clamp-to-edge.
        LLRenderTarget* composite_source = debug_mode == 10 ?
            &sTransparencyAtlas : &sVolumetricTarget;
        S32 emissive_channel = gASVolumetricCompositeProgram.bindTexture(
            LLShaderMgr::DEFERRED_EMISSIVE, composite_source);
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
            1.f / (F32)composite_source->getWidth(),
            1.f / (F32)composite_source->getHeight());
        gASVolumetricCompositeProgram.uniform1i(
            LLStaticHashedString("depthAwareUpsample"), debug_mode == 0 ? 1 : 0);
    }

    {
        LLGLEnable blend(GL_BLEND);
        gGL.setSceneBlendType(debug_mode != 0 ? LLRender::BT_REPLACE : LLRender::BT_ADD);
        // Preserve screen alpha. BT_ADD applies ONE,ONE to alpha as well as
        // RGB, and the HDR screen alpha is consumed by later post-processing.
        // Adding the composite shader's alpha every frame corrupts those
        // passes and produces a full-screen whiteout unrelated to scatter RGB.
        gGL.setColorMask(true, false);
        pipeline.mScreenTriangleVB->setBuffer();
        pipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gGL.setColorMask(true, true);
    }

    // Do not leak the temporary scatter texture binding into later rendering.
    gASVolumetricCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_EMISSIVE);
    gASVolumetricCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    gASVolumetricCompositeProgram.unbind();

    screen.flush();
}
