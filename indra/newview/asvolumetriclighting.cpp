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
#include "llrender.h"
#include "llshadermgr.h"
#include "llviewercontrol.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
LLGLSLShader gASVolumetricLightProgram;
LLGLSLShader gASVolumetricCompositeProgram;
}

bool ASVolumetricLighting::sSupportChecked = false;
bool ASVolumetricLighting::sSupported = false;
bool ASVolumetricLighting::sShadersLoaded = false;
LLRenderTarget ASVolumetricLighting::sVolumetricTarget;

// Bump this string whenever a volumetric .glsl file changes so cached
// program binaries do not go stale (see llviewershadermgr.cpp shader cache
// hash, which folds this in alongside FSExactOIT::shaderCacheRevision()).
const char* ASVolumetricLighting::shaderCacheRevision()
{
    return "as-volumetric-lighting-v1";
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
        gASVolumetricCompositeProgram.unload();
    }

    sShadersLoaded = success;
    return true; // failure here must not fail the whole deferred shader load
}

void ASVolumetricLighting::unloadShaders()
{
    gASVolumetricLightProgram.unload();
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
}

void ASVolumetricLighting::releaseResources()
{
    sVolumetricTarget.release();
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
    static LLCachedControl<F32> intensity(gSavedSettings, "RenderVolumetricLightingIntensity", 0.5f);
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

void ASVolumetricLighting::renderPass(LLPipeline& pipeline, LLRenderTarget& screen)
{
    if (!isEnabled() || !sShadersLoaded || gCubeSnapshot || LLPipeline::sRenderingHUDs)
    {
        return;
    }

    if (!sVolumetricTarget.isComplete())
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

        pipeline.bindDeferredShader(gASVolumetricLightProgram);

        // TEMPORARY development aid - remove once the "always-far" depth
        // bug is diagnosed. Logs the actual texture channel depthMap got
        // mapped to, once, so we can tell whether bindDeferredShader's
        // DEFERRED_DEPTH enableTexture() call is even resolving the uniform
        // (index == -1 would mean depthMap is unbound and reading stale/
        // default texture-unit content, explaining an always-far/white read).
        {
            static bool logged_once = false;
            if (!logged_once)
            {
                S32 depth_uniform = gASVolumetricLightProgram.getUniformLocation(LLShaderMgr::DEFERRED_DEPTH);
                S32 depth_channel = (LLShaderMgr::DEFERRED_DEPTH >= 0 && LLShaderMgr::DEFERRED_DEPTH < (S32)gASVolumetricLightProgram.mTexture.size())
                    ? gASVolumetricLightProgram.mTexture[LLShaderMgr::DEFERRED_DEPTH] : -999;
                LL_WARNS() << "AS Volumetric depthMap diagnostic: uniform_loc=" << depth_uniform
                    << " texture_channel=" << depth_channel << LL_ENDL;
                logged_once = true;
            }
        }

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

        sVolumetricTarget.flush();
    }

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
    gASVolumetricCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE, &sVolumetricTarget);

    {
        LLGLEnable blend(GL_BLEND);
        gGL.setSceneBlendType(debug_mode != 0 ? LLRender::BT_REPLACE : LLRender::BT_ADD);
        pipeline.mScreenTriangleVB->setBuffer();
        pipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    gASVolumetricCompositeProgram.unbind();

    screen.flush();
}
