/**
 * @file asdiffuseglow.cpp
 * @author chanayane@firestorm
 * @brief Optional bright-surface bloom controls for the existing glow pass.
 */

#include "llviewerprecompiledheaders.h"

#include "asdiffuseglow.h"

#include "ascolorgrading.h"

#include "llcontrol.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "lluictrl.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

namespace
{
    const LLStaticHashedString sThreshold("asDiffuseGlowThreshold");
    const LLStaticHashedString sSoftness("asDiffuseGlowSoftness");
    const LLStaticHashedString sStrength("asDiffuseGlowStrength");
    const LLStaticHashedString sExposure("exposure");
    const LLStaticHashedString sMinLuminance("minLuminance");
    const LLStaticHashedString sMaxExtractAlpha("maxExtractAlpha");
    const LLStaticHashedString sLumWeights("lumWeights");
    const LLStaticHashedString sWarmthWeights("warmthWeights");
    const LLStaticHashedString sWarmthAmount("warmthAmount");

    LLGLSLShader sHDRExtractProgram;
    LLGLSLShader sHDRCombineProgram;
    LLRenderTarget sHDRGlow[3];
    LLRenderTarget sHDRComposite;
    U32 sSceneWidth = 0;
    U32 sSceneHeight = 0;
    U32 sGlowHeight = 0;
    U32 sFailedWidth = 0;
    U32 sFailedHeight = 0;
    U32 sFailedGlowHeight = 0;
    bool sSuppressNextCompatibilityExtraction = false;

    bool ensureHDRResources(U32 width, U32 height, U32 glow_height)
    {
        if (width == 0 || height == 0 || glow_height == 0)
        {
            return false;
        }
        if (sFailedWidth == width && sFailedHeight == height &&
            sFailedGlowHeight == glow_height)
        {
            return false;
        }
        if (sSceneWidth == width && sSceneHeight == height && sGlowHeight == glow_height &&
            sHDRComposite.isComplete() && sHDRGlow[0].isComplete() &&
            sHDRGlow[1].isComplete() && sHDRGlow[2].isComplete())
        {
            return true;
        }

        ASDiffuseGlow::releaseHDRResources();
        bool success = sHDRComposite.allocate(width, height, GL_RGBA16F);
        for (U32 i = 0; success && i < 3; ++i)
        {
            success = sHDRGlow[i].allocate(1024, glow_height, GL_RGBA16F);
        }
        if (!success)
        {
            ASDiffuseGlow::releaseHDRResources();
            sFailedWidth = width;
            sFailedHeight = height;
            sFailedGlowHeight = glow_height;
            return false;
        }
        sSceneWidth = width;
        sSceneHeight = height;
        sGlowHeight = glow_height;
        return true;
    }
}

void ASDiffuseGlow::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASDiffuseGlow.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            const std::string name = data.asString();
            if (name == "ASDiffuseGlowThreshold" || name == "ASDiffuseGlowSoftness" ||
                name == "ASDiffuseGlowStrength")
            {
                if (LLControlVariable* control = gSavedSettings.getControl(name))
                {
                    control->resetToDefault(true);
                }
            }
        });
}

void ASDiffuseGlow::appendShader(LLGLSLShader& shader)
{
    shader.mShaderFiles.emplace_back("effects/asDiffuseGlowF.glsl", GL_FRAGMENT_SHADER);
}

void ASDiffuseGlow::registerShaders(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sHDRExtractProgram);
    shaders.push_back(&sHDRCombineProgram);
}

bool ASDiffuseGlow::createHDRShaders(S32 shader_level)
{
    sHDRExtractProgram.mName = "AyaneStorm HDR Bright-Surface Bloom Extract";
    sHDRExtractProgram.mShaderFiles.clear();
    sHDRExtractProgram.clearPermutations();
    sHDRExtractProgram.mShaderFiles.emplace_back("effects/glowExtractV.glsl", GL_VERTEX_SHADER);
    sHDRExtractProgram.mShaderFiles.emplace_back("effects/asHDRDiffuseGlowExtractF.glsl", GL_FRAGMENT_SHADER);
    ASColorGrading::appendLinearShader(sHDRExtractProgram);
    sHDRExtractProgram.mShaderLevel = shader_level;

    sHDRCombineProgram.mName = "AyaneStorm HDR Bright-Surface Bloom Combine";
    sHDRCombineProgram.mShaderFiles.clear();
    sHDRCombineProgram.clearPermutations();
    sHDRCombineProgram.mShaderFiles.emplace_back("interface/glowcombineV.glsl", GL_VERTEX_SHADER);
    sHDRCombineProgram.mShaderFiles.emplace_back("effects/asHDRDiffuseGlowCombineF.glsl", GL_FRAGMENT_SHADER);
    sHDRCombineProgram.mShaderLevel = shader_level;

    const bool extract_created = sHDRExtractProgram.createShader();
    const bool combine_created = sHDRCombineProgram.createShader();
    if (!extract_created || !combine_created)
    {
        unloadHDRShaders();
        return false;
    }
    return true;
}

void ASDiffuseGlow::unloadHDRShaders()
{
    sHDRExtractProgram.unload();
    sHDRCombineProgram.unload();
}

void ASDiffuseGlow::releaseHDRResources()
{
    sHDRComposite.release();
    for (LLRenderTarget& target : sHDRGlow)
    {
        target.release();
    }
    sSceneWidth = 0;
    sSceneHeight = 0;
    sGlowHeight = 0;
    sFailedWidth = 0;
    sFailedHeight = 0;
    sFailedGlowHeight = 0;
    sSuppressNextCompatibilityExtraction = false;
}

void ASDiffuseGlow::bindExtractionUniforms(LLGLSLShader& shader)
{
    if (sSuppressNextCompatibilityExtraction)
    {
        // The HDR pass already consumed automatic bright-pixel extraction.
        // Zero only that contribution in the immediately following
        // compatibility pass; authored alpha glow remains active.
        shader.uniform1f(LLShaderMgr::GLOW_MAX_EXTRACT_ALPHA, 0.f);
    }
    const bool enabled = gSavedSettings.getBOOL("ASCameraEffectsEnabled") &&
                         gSavedSettings.getBOOL("ASDiffuseGlowEnabled") &&
                         !sSuppressNextCompatibilityExtraction;
    shader.uniform1f(sThreshold, llclamp(gSavedSettings.getF32("ASDiffuseGlowThreshold"), 0.f, 1.f));
    shader.uniform1f(sSoftness, llclamp(gSavedSettings.getF32("ASDiffuseGlowSoftness"), 0.f, 1.f));
    shader.uniform1f(sStrength,
                     enabled ? llclamp(gSavedSettings.getF32("ASDiffuseGlowStrength"), 0.f, 1.f) : 0.f);
    // renderFinalize() calls the compatibility extractor immediately after a
    // successful HDR pass. Consume the one-shot here so unrelated preview
    // renders retain their normal extraction behavior.
    sSuppressNextCompatibilityExtraction = false;
}

LLRenderTarget* ASDiffuseGlow::renderHDR(LLRenderTarget& scene, LLRenderTarget& exposure_map,
                                         LLVertexBuffer& screen_triangle)
{
    const bool enabled = LLPipeline::sRenderGlow && gSavedSettings.getBOOL("RenderGlow") &&
                         gSavedSettings.getBOOL("ASCameraEffectsEnabled") &&
                         gSavedSettings.getBOOL("ASDiffuseGlowEnabled");
    if (!enabled)
    {
        if (sSceneWidth != 0)
        {
            releaseHDRResources();
        }
        return nullptr;
    }
    if (!sHDRExtractProgram.isComplete() || !sHDRCombineProgram.isComplete() ||
        !gGlowProgram.isComplete())
    {
        return nullptr;
    }

    const S32 glow_resolution_pow = llclamp(gSavedSettings.getS32("RenderGlowResolutionPow"), 0, 10);
    const U32 glow_height = 1U << glow_resolution_pow;
    if (!ensureHDRResources(scene.getWidth(), scene.getHeight(), glow_height))
    {
        return nullptr;
    }

    LL_PROFILE_GPU_ZONE("AyaneStorm HDR Bright-Surface Bloom");
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);

    sHDRGlow[2].bindTarget();
    sHDRGlow[2].clear();
    sHDRExtractProgram.bind();
    sHDRExtractProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, &scene);
    sHDRExtractProgram.bindTexture(LLShaderMgr::EXPOSURE_MAP, &exposure_map);
    static LLCachedControl<F32> exposure(gSavedSettings, "RenderExposure", 1.f);
    sHDRExtractProgram.uniform1f(sExposure, llclamp((F32)exposure, 0.5f, 4.f));
    bindExtractionUniforms(sHDRExtractProgram);
    sHDRExtractProgram.uniform1f(sMinLuminance, LLPipeline::RenderGlowMinLuminance);
    sHDRExtractProgram.uniform1f(sMaxExtractAlpha, LLPipeline::RenderGlowMaxExtractAlpha);
    sHDRExtractProgram.uniform3fv(sLumWeights, 1, LLPipeline::RenderGlowLumWeights.mV);
    sHDRExtractProgram.uniform3fv(sWarmthWeights, 1, LLPipeline::RenderGlowWarmthWeights.mV);
    sHDRExtractProgram.uniform1f(sWarmthAmount, LLPipeline::RenderGlowWarmthAmount);
    ASColorGrading::bindLinearUniforms(sHDRExtractProgram, false);
    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    sHDRExtractProgram.unbind();
    sHDRGlow[2].flush();

    static LLCachedControl<S32> iterations(gSavedSettings, "RenderGlowIterations", 2);
    static LLCachedControl<F32> width(gSavedSettings, "RenderGlowWidth", 1.3f);
    static LLCachedControl<F32> strength(gSavedSettings, "RenderGlowStrength", 0.325f);
    const S32 kernel = llmax(2, llclamp((S32)iterations, 1, 128) * 2);
    F32 delta = llclamp((F32)width, 0.f, 64.f) / (F32)glow_height;
    if (glow_resolution_pow < 9)
    {
        delta *= 0.5f;
    }

    gGlowProgram.bind();
    gGlowProgram.uniform1f(LLShaderMgr::GLOW_STRENGTH, llclamp((F32)strength, 0.f, 64.f));
    for (S32 i = 0; i < kernel; ++i)
    {
        sHDRGlow[i % 2].bindTarget();
        sHDRGlow[i % 2].clear();
        gGlowProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP,
                                 i == 0 ? &sHDRGlow[2] : &sHDRGlow[(i - 1) % 2]);
        gGlowProgram.uniform2f(LLShaderMgr::GLOW_DELTA,
                               i % 2 == 0 ? delta : 0.f,
                               i % 2 == 0 ? 0.f : delta);
        screen_triangle.setBuffer();
        screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
        sHDRGlow[i % 2].flush();
    }
    gGlowProgram.unbind();

    sHDRComposite.bindTarget();
    sHDRComposite.clear();
    sHDRCombineProgram.bind();
    sHDRCombineProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &scene);
    sHDRCombineProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE, &sHDRGlow[1]);
    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    sHDRCombineProgram.unbind();
    sHDRComposite.flush();
    sSuppressNextCompatibilityExtraction = true;
    return &sHDRComposite;
}
