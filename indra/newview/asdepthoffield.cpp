/**
 * @file asdepthoffield.cpp
 * @author chanayane@firestorm
 * @brief AyaneStorm-owned cinematic depth-of-field renderer.
 *
 * The signed near/far split and foreground coverage are an AyaneStorm design
 * informed by OtisFX CinematicDOF, Intel's Vanilla DoF sample, and qUINT ADoF.
 * This is not a line-for-line translation of any of those implementations.
 */
#include "llviewerprecompiledheaders.h"

#include <algorithm>

#include "asdepthoffield.h"

#include "asbackgroundisolate.h"
#include "asdofrenderer.h"
#include "llcontrol.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "lluictrl.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"

namespace
{
    LLGLSLShader sCoCProgram;
    LLGLSLShader sFarProgram;
    LLGLSLShader sNearProgram;
    LLGLSLShader sTransparentProgram;
    LLGLSLShader sOccupancyProgram;
    LLGLSLShader sResolveProgram;

    LLRenderTarget sCoCTarget;
    LLRenderTarget sFarTarget;
    LLRenderTarget sNearTarget;
    LLRenderTarget sTransparentFarTarget;
    LLRenderTarget sTransparentNearTarget;
    LLRenderTarget sRiggedFarTarget;
    LLRenderTarget sRiggedNearTarget;
    LLRenderTarget sOccupancyTarget;
    LLRenderTarget sOpaqueColorTarget;
    LLRenderTarget sHDROutputTarget;
    LLRenderTarget sOpaqueDepthTarget;
    LLRenderTarget sTransparentCoverageTarget;
    LLRenderTarget sRiggedLayerTarget;
    LLRenderTarget sTransparentDepthTarget;
    LLRenderTarget sRiggedDepthTarget;
    LLRenderTarget sWorldDepthTarget;
    bool sOpaqueLayerReady = false;
    bool sTransparentDepthPrepared = false;
    bool sTransparentCoverageReady = false;
    bool sTransparentDepthReady = false;
    bool sRiggedCoverageReady = false;
    bool sRiggedDepthReady = false;
    bool sWorldDepthReady = false;
    U32 sWidth = 0;
    U32 sHeight = 0;
    U32 sBlurWidth = 0;
    U32 sBlurHeight = 0;

    const LLStaticHashedString U_FOCAL_DISTANCE("focal_distance");
    const LLStaticHashedString U_BLUR_CONSTANT("blur_constant");
    const LLStaticHashedString U_TAN_PIXEL_ANGLE("tan_pixel_angle");
    const LLStaticHashedString U_MAGNIFICATION("magnification");
    const LLStaticHashedString U_MAX_COC("max_coc");
    const LLStaticHashedString U_HAS_TRANSPARENT_DEPTH("has_transparent_depth");
    const LLStaticHashedString U_HAS_LAYERS("has_layers");
    const LLStaticHashedString U_SAMPLE_COUNT("sample_count");
    const LLStaticHashedString U_MAX_RADIUS("max_radius");
    const LLStaticHashedString U_FOREGROUND_RADIUS("foreground_radius");
    const LLStaticHashedString U_NEAR_MAX_RADIUS("near_max_radius");
    const LLStaticHashedString U_APERTURE_BLADES("aperture_blades");
    const LLStaticHashedString U_APERTURE_ROUNDNESS("aperture_roundness");
    const LLStaticHashedString U_APERTURE_ROTATION("aperture_rotation");
    const LLStaticHashedString U_ANAMORPHIC_RATIO("anamorphic_ratio");
    const LLStaticHashedString U_HIGHLIGHT_BOOST("highlight_boost");
    const LLStaticHashedString U_DEBUG_MODE("debug_mode");
    const LLStaticHashedString U_PLANE("plane");
    const LLStaticHashedString U_LAYER_MODE("layer_mode");
    const LLStaticHashedString U_USE_OCCUPANCY("use_occupancy");

    void releaseGatherResources()
    {
        sCoCTarget.release();
        sFarTarget.release();
        sNearTarget.release();
        sTransparentFarTarget.release();
        sTransparentNearTarget.release();
        sRiggedFarTarget.release();
        sRiggedNearTarget.release();
        sOccupancyTarget.release();
        sWidth = 0;
        sHeight = 0;
        sBlurWidth = 0;
        sBlurHeight = 0;
    }

    bool ensureResources(U32 width, U32 height, F32 scale)
    {
        const U32 blur_width = llmax(1U, (U32)ll_round((F32)width * scale));
        const U32 blur_height = llmax(1U, (U32)ll_round((F32)height * scale));
        const U32 tile_width = (width + 15U) / 16U;
        const U32 tile_height = (height + 15U) / 16U;
        if (sCoCTarget.isComplete() && sCoCTarget.getNumTextures() == 2 &&
            sFarTarget.isComplete() && sNearTarget.isComplete() &&
            sTransparentFarTarget.isComplete() && sTransparentNearTarget.isComplete() &&
            sRiggedFarTarget.isComplete() && sRiggedNearTarget.isComplete() &&
            sOccupancyTarget.isComplete() &&
            sOccupancyTarget.getWidth() == tile_width &&
            sOccupancyTarget.getHeight() == tile_height &&
            sWidth == width && sHeight == height &&
            sBlurWidth == blur_width && sBlurHeight == blur_height)
        {
            return true;
        }

        releaseGatherResources();
        if (!sCoCTarget.allocate(width, height, GL_RGBA16F) ||
            !sCoCTarget.addColorAttachment(GL_RGBA16F) ||
            !sFarTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sNearTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sTransparentFarTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sTransparentNearTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sRiggedFarTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sRiggedNearTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sOccupancyTarget.allocate(tile_width, tile_height, GL_RGBA16F,
                                       false, LLTexUnit::TT_TEXTURE,
                                       LLTexUnit::TMG_AUTO))
        {
            releaseGatherResources();
            return false;
        }

        sWidth = width;
        sHeight = height;
        sBlurWidth = blur_width;
        sBlurHeight = blur_height;
        return true;
    }

    void configureGather(LLGLSLShader& shader, S32 samples, F32 radius,
                         S32 blades, F32 roundness, F32 rotation,
                         F32 anamorphic, F32 highlight_boost)
    {
        shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)sWidth, (F32)sHeight);
        shader.uniform1i(U_SAMPLE_COUNT, samples);
        shader.uniform1f(U_MAX_RADIUS, radius);
        shader.uniform1i(U_APERTURE_BLADES, blades);
        shader.uniform1f(U_APERTURE_ROUNDNESS, roundness);
        shader.uniform1f(U_APERTURE_ROTATION, rotation);
        shader.uniform1f(U_ANAMORPHIC_RATIO, anamorphic);
        shader.uniform1f(U_HIGHLIGHT_BOOST, highlight_boost);
    }

    void draw(LLVertexBuffer& triangle)
    {
        triangle.setBuffer();
        triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    }
}

extern bool gCubeSnapshot;

void ASDepthOfField::registerUICallbacks()
{
    ASDoFRenderer::registerUICallbacks();
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASDepthOfField.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            static const std::vector<std::string> controls = {
                "ASDepthOfFieldMode", "ASDepthOfFieldBackend",
                "ASDepthOfFieldQuality", "ASDepthOfFieldNearRadius",
                "ASDepthOfFieldFarRadius", "ASDepthOfFieldApertureBlades",
                "ASDepthOfFieldApertureRoundness", "ASDepthOfFieldApertureRotation",
                "ASDepthOfFieldAnamorphicRatio", "ASDepthOfFieldHighlightBoost",
                "ASDepthOfFieldDebug", "ASDepthOfFieldApertureSamples",
                "ASDepthOfFieldApertureMaxSamples", "ASDepthOfFieldApertureSnapshotSamples",
                "ASDepthOfFieldApertureSnapshotMaxSeconds",
                "ASDepthOfFieldApertureResidualBlur",
                "ASDepthOfFieldApertureAxialCA", "ASDepthOfFieldApertureAxialCAStrength",
                "ASDepthOfFieldApertureShowProgress"
            };
            const std::string name = data.asString();
            if (name == "All")
            {
                // Renderer and backend are mode choices, not tuning values.
                for (auto it = controls.begin() + 2; it != controls.end(); ++it)
                {
                    if (LLControlVariable* control = gSavedSettings.getControl(*it))
                    {
                        control->resetToDefault(true);
                    }
                }
                return;
            }
            if (std::find(controls.begin(), controls.end(), name) != controls.end())
            {
                if (LLControlVariable* control = gSavedSettings.getControl(name))
                {
                    control->resetToDefault(true);
                }
            }
        });
}

void ASDepthOfField::registerShaders(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sCoCProgram);
    shaders.push_back(&sFarProgram);
    shaders.push_back(&sNearProgram);
    shaders.push_back(&sTransparentProgram);
    shaders.push_back(&sOccupancyProgram);
    shaders.push_back(&sResolveProgram);
    // Aperture-sampled renderer shares this module's registration hooks.
    ASDoFRenderer::registerShaders(shaders);
}

bool ASDepthOfField::createShaders(S32 shader_level)
{
    struct ShaderSpec
    {
        LLGLSLShader* shader;
        const char* name;
        const char* fragment;
    };
    const ShaderSpec specs[] = {
        { &sCoCProgram, "AyaneStorm Depth of Field CoC Shader", "deferred/asDepthOfFieldCoCF.glsl" },
        { &sFarProgram, "AyaneStorm Depth of Field Far Bokeh Shader", "deferred/asDepthOfFieldFarF.glsl" },
        { &sNearProgram, "AyaneStorm Depth of Field Near Bokeh Shader", "deferred/asDepthOfFieldNearF.glsl" },
        { &sTransparentProgram, "AyaneStorm Depth of Field Transparent Bokeh Shader", "deferred/asDepthOfFieldTransparentF.glsl" },
        { &sOccupancyProgram, "AyaneStorm Depth of Field Layer Occupancy Shader", "deferred/asDepthOfFieldOccupancyF.glsl" },
        { &sResolveProgram, "AyaneStorm Depth of Field Resolve Shader", "deferred/asDepthOfFieldResolveF.glsl" }
    };

    bool success = true;
    for (const ShaderSpec& spec : specs)
    {
        spec.shader->mName = spec.name;
        spec.shader->mShaderFiles.clear();
        spec.shader->clearPermutations();
        spec.shader->mFeatures.isDeferred = true;
        spec.shader->mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
        spec.shader->mShaderFiles.emplace_back(spec.fragment, GL_FRAGMENT_SHADER);
        spec.shader->mShaderLevel = shader_level;
        success = spec.shader->createShader() && success;
    }
    success = ASDoFRenderer::createShaders(shader_level) && success;
    return success;
}

void ASDepthOfField::unloadShaders()
{
    sCoCProgram.unload();
    sFarProgram.unload();
    sNearProgram.unload();
    sTransparentProgram.unload();
    sOccupancyProgram.unload();
    sResolveProgram.unload();
    ASDoFRenderer::unloadShaders();
    releaseResources();
}

void ASDepthOfField::releaseResources()
{
    releaseGatherResources();
    sHDROutputTarget.release();
    sTransparentDepthTarget.release();
    sTransparentCoverageTarget.release();
    sRiggedLayerTarget.release();
    sOpaqueColorTarget.release();
    sOpaqueDepthTarget.release();
    sRiggedDepthTarget.release();
    sWorldDepthTarget.release();
    sOpaqueLayerReady = false;
    sTransparentDepthPrepared = false;
    sTransparentCoverageReady = false;
    sTransparentDepthReady = false;
    sRiggedCoverageReady = false;
    sRiggedDepthReady = false;
    sWorldDepthReady = false;
}

LLRenderTarget* ASDepthOfField::hdrOutput(U32 width, U32 height)
{
    if (!width || !height)
    {
        return nullptr;
    }
    if (!sHDROutputTarget.isComplete() ||
        sHDROutputTarget.getWidth() != width ||
        sHDROutputTarget.getHeight() != height)
    {
        sHDROutputTarget.release();
        if (!sHDROutputTarget.allocate(width, height, GL_RGBA16F))
        {
            return nullptr;
        }
    }
    return &sHDROutputTarget;
}

bool ASDepthOfField::prepareTransparentDepthCapture(U32 width, U32 height)
{
    sOpaqueLayerReady = false;
    sTransparentDepthPrepared = false;
    sTransparentCoverageReady = false;
    sTransparentDepthReady = false;
    sRiggedCoverageReady = false;
    sRiggedDepthReady = false;
    sWorldDepthReady = false;
    if (gSavedSettings.getS32("ASDepthOfFieldMode") != 1 ||
        width == 0 || height == 0 || ASBackgroundIsolate::isActive() ||
        !sCoCProgram.isComplete() || !sFarProgram.isComplete() ||
        !sNearProgram.isComplete() || !sTransparentProgram.isComplete() ||
        !sOccupancyProgram.isComplete() ||
        !sResolveProgram.isComplete())
    {
        return false;
    }

    const F32 scale = llclamp(gSavedSettings.getF32("CameraDoFResScale"), 0.25f, 1.f);
    if (!ensureResources(width, height, scale))
    {
        return false;
    }

    if (!sOpaqueDepthTarget.isComplete() ||
        sOpaqueDepthTarget.getWidth() != width ||
        sOpaqueDepthTarget.getHeight() != height)
    {
        sOpaqueDepthTarget.release();
        if (!sOpaqueDepthTarget.allocate(width, height, 0, true))
        {
            sOpaqueDepthTarget.release();
            return false;
        }
    }

    if (!sOpaqueColorTarget.isComplete() ||
        sOpaqueColorTarget.getWidth() != width ||
        sOpaqueColorTarget.getHeight() != height)
    {
        sOpaqueColorTarget.release();
        if (!sOpaqueColorTarget.allocate(width, height, GL_RGBA16F))
        {
            sOpaqueColorTarget.release();
            return false;
        }
    }

    if (!sTransparentDepthTarget.isComplete() ||
        sTransparentDepthTarget.getWidth() != width ||
        sTransparentDepthTarget.getHeight() != height)
    {
        sTransparentDepthTarget.release();
        if (!sTransparentDepthTarget.allocate(width, height, GL_RGBA16F, true))
        {
            sTransparentDepthTarget.release();
            return false;
        }
    }

    if (!sTransparentCoverageTarget.isComplete() ||
        sTransparentCoverageTarget.getWidth() != width ||
        sTransparentCoverageTarget.getHeight() != height)
    {
        sTransparentCoverageTarget.release();
        if (!sTransparentCoverageTarget.allocate(width, height, GL_RGBA16F, true))
        {
            sTransparentCoverageTarget.release();
            return false;
        }
    }

    if (!sRiggedLayerTarget.isComplete() ||
        sRiggedLayerTarget.getWidth() != width ||
        sRiggedLayerTarget.getHeight() != height)
    {
        sRiggedLayerTarget.release();
        if (!sRiggedLayerTarget.allocate(width, height, GL_RGBA16F))
        {
            return false;
        }
    }

    if (!sRiggedDepthTarget.isComplete() ||
        sRiggedDepthTarget.getWidth() != width ||
        sRiggedDepthTarget.getHeight() != height)
    {
        sRiggedDepthTarget.release();
        if (!sRiggedDepthTarget.allocate(width, height, 0, true))
        {
            return false;
        }
    }

    if (!sWorldDepthTarget.isComplete() ||
        sWorldDepthTarget.getWidth() != width ||
        sWorldDepthTarget.getHeight() != height)
    {
        sWorldDepthTarget.release();
        if (!sWorldDepthTarget.allocate(width, height, 0, true))
        {
            return false;
        }
    }

    const U32 scene_fbo = LLRenderTarget::sCurFBO;
    LLGLDisable scissor(GL_SCISSOR_TEST);
    gGL.setColorMask(true, true);

    sOpaqueColorTarget.bindTarget();
    const U32 opaque_color_fbo = LLRenderTarget::sCurFBO;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, opaque_color_fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, opaque_color_fbo);
    sOpaqueColorTarget.flush();

    sOpaqueDepthTarget.bindTarget();
    const U32 opaque_fbo = LLRenderTarget::sCurFBO;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, opaque_fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, opaque_fbo);
    sOpaqueDepthTarget.flush();

    sTransparentDepthTarget.bindTarget();
    const U32 transparent_fbo = LLRenderTarget::sCurFBO;
    gGL.setColorMask(true, true);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    sTransparentDepthTarget.clear(GL_COLOR_BUFFER_BIT);
    // Start from opaque visibility so the auxiliary pass captures only
    // transparent fragments which can actually contribute to the final pixel.
    // Framebuffer depth blits are part of the OpenGL 4.1 baseline.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, transparent_fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, transparent_fbo);
    sTransparentDepthTarget.flush();

    sTransparentCoverageTarget.bindTarget();
    const U32 coverage_fbo = LLRenderTarget::sCurFBO;
    gGL.setColorMask(true, true);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    sTransparentCoverageTarget.clear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, coverage_fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, coverage_fbo);
    sTransparentCoverageTarget.flush();

    sWorldDepthTarget.bindTarget();
    const U32 world_fbo = LLRenderTarget::sCurFBO;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, world_fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, world_fbo);
    sWorldDepthTarget.flush();
    sOpaqueLayerReady = true;
    sTransparentDepthPrepared = true;
    return true;
}

bool ASDepthOfField::beginTransparentCoverageCapture(U32 width, U32 height)
{
    sTransparentCoverageReady = false;
    if (!sTransparentDepthPrepared ||
        sTransparentCoverageTarget.getWidth() != width ||
        sTransparentCoverageTarget.getHeight() != height)
    {
        return false;
    }

    sTransparentCoverageTarget.bindTarget();
    gGL.setColorMask(true, true);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    sTransparentCoverageTarget.clear(GL_COLOR_BUFFER_BIT);
    return true;
}

void ASDepthOfField::endTransparentCoverageCapture()
{
    sTransparentCoverageTarget.flush();
    sTransparentCoverageReady = true;
}

bool ASDepthOfField::snapshotRiggedCoverage()
{
    // Preserve the premultiplied rigged contribution before non-rigged draws
    // enter the same auxiliary source-over target.
    if (!sTransparentDepthPrepared || !sRiggedLayerTarget.isComplete())
    {
        return false;
    }
    const U32 coverage_fbo = LLRenderTarget::sCurFBO;
    LLGLDisable scissor(GL_SCISSOR_TEST);
    sRiggedLayerTarget.bindTarget();
    const U32 rigged_fbo = LLRenderTarget::sCurFBO;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, coverage_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rigged_fbo);
    glBlitFramebuffer(0, 0, sWidth, sHeight, 0, 0, sWidth, sHeight,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, rigged_fbo);
    sRiggedLayerTarget.flush();
    sRiggedCoverageReady = true;
    return true;
}

bool ASDepthOfField::beginTransparentDepthCapture(U32 width, U32 height)
{
    sTransparentDepthReady = false;
    if (!sTransparentDepthPrepared ||
        sTransparentDepthTarget.getWidth() != width ||
        sTransparentDepthTarget.getHeight() != height)
    {
        return false;
    }

    sTransparentDepthTarget.bindTarget();
    gGL.setColorMask(true, true);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    sTransparentDepthTarget.clear(GL_COLOR_BUFFER_BIT);
    return true;
}

void ASDepthOfField::endTransparentDepthCapture()
{
    sTransparentDepthTarget.flush();
    sTransparentDepthPrepared = false;
    sTransparentDepthReady = sTransparentCoverageReady;
}

bool ASDepthOfField::snapshotRiggedDepth()
{
    // The combined nearest-depth pass has just rendered rigged alpha only.
    if (!sTransparentDepthPrepared || !sRiggedDepthTarget.isComplete())
    {
        return false;
    }
    const U32 capture_fbo = LLRenderTarget::sCurFBO;
    LLGLDisable scissor(GL_SCISSOR_TEST);
    sRiggedDepthTarget.bindTarget();
    const U32 rigged_fbo = LLRenderTarget::sCurFBO;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, capture_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rigged_fbo);
    glBlitFramebuffer(0, 0, sWidth, sHeight, 0, 0, sWidth, sHeight,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, rigged_fbo);
    sRiggedDepthTarget.flush();
    sRiggedDepthReady = true;
    return true;
}

bool ASDepthOfField::beginWorldDepthCapture(U32 width, U32 height)
{
    // Replaying non-rigged alpha against opaque depth supplies an independent
    // world stratum even where foreground hair is nearer.
    sWorldDepthReady = false;
    if (!sTransparentDepthReady ||
        sWorldDepthTarget.getWidth() != width ||
        sWorldDepthTarget.getHeight() != height)
    {
        return false;
    }
    sWorldDepthTarget.bindTarget();
    return true;
}

void ASDepthOfField::endWorldDepthCapture()
{
    sWorldDepthTarget.flush();
    sWorldDepthReady = true;
}

bool ASDepthOfField::render(LLRenderTarget& source, LLRenderTarget& destination,
                             LLRenderTarget& depth, LLVertexBuffer& screen_triangle,
                             F32 focal_distance, F32 blur_constant, F32 tan_pixel_angle,
                             F32 magnification, F32 max_coc)
{
    if (gSavedSettings.getS32("ASDepthOfFieldMode") != 1)
    {
        if (sCoCTarget.isComplete() || sFarTarget.isComplete() || sNearTarget.isComplete())
        {
            releaseResources();
        }
        return false;
    }

    if (!sCoCProgram.isComplete() || !sFarProgram.isComplete() ||
        !sNearProgram.isComplete() || !sTransparentProgram.isComplete() ||
        !sOccupancyProgram.isComplete() ||
        !sResolveProgram.isComplete() ||
        gCubeSnapshot || ASBackgroundIsolate::isActive() || &source == &destination ||
        source.getWidth() <= 0 || source.getHeight() <= 0 ||
        source.getWidth() != destination.getWidth() || source.getHeight() != destination.getHeight())
    {
        if (!sCoCProgram.isComplete() || !sFarProgram.isComplete() ||
            !sNearProgram.isComplete() || !sTransparentProgram.isComplete() ||
            !sOccupancyProgram.isComplete() ||
            !sResolveProgram.isComplete())
        {
            LL_WARNS_ONCE("ASDepthOfField") << "Advanced DoF shaders are incomplete; using Firestorm DoF." << LL_ENDL;
        }
        return false;
    }

    const F32 abs_max_coc = llclamp(fabsf(max_coc), 0.f, 150.f);
    const F32 scale = llclamp(gSavedSettings.getF32("CameraDoFResScale"), 0.25f, 1.f);
    if (!ensureResources(source.getWidth(), source.getHeight(), scale))
    {
        LL_WARNS_ONCE("ASDepthOfField") << "Advanced DoF target allocation failed; using Firestorm DoF." << LL_ENDL;
        return false;
    }

    LL_INFOS_ONCE("ASDepthOfField") << "Advanced DoF active at "
        << source.getWidth() << "x" << source.getHeight()
        << ", gather scale " << scale << LL_ENDL;

    // Large polygonal bokeh needs enough aperture coverage to avoid dotted
    // highlights; the lower presets retain their cheaper gather budgets.
    static const S32 sample_counts[] = { 16, 32, 96 };
    const S32 quality = llclamp(gSavedSettings.getS32("ASDepthOfFieldQuality"), 0, 2);
    const S32 samples = sample_counts[quality];
    const F32 near_radius = abs_max_coc * llclamp(gSavedSettings.getF32("ASDepthOfFieldNearRadius"), 0.f, 4.f);
    const F32 far_radius = abs_max_coc * llclamp(gSavedSettings.getF32("ASDepthOfFieldFarRadius"), 0.f, 4.f);
    const S32 blades = llclamp(gSavedSettings.getS32("ASDepthOfFieldApertureBlades"), 0, 12);
    const F32 roundness = llclamp(gSavedSettings.getF32("ASDepthOfFieldApertureRoundness"), 0.f, 1.f);
    const F32 rotation = gSavedSettings.getF32("ASDepthOfFieldApertureRotation") * DEG_TO_RAD;
    const F32 anamorphic = llclamp(gSavedSettings.getF32("ASDepthOfFieldAnamorphicRatio"), 0.1f, 2.f);
    const F32 highlight_boost = llclamp(gSavedSettings.getF32("ASDepthOfFieldHighlightBoost"), 0.f, 2.f);
    const S32 debug_mode = llclamp(gSavedSettings.getS32("ASDepthOfFieldDebug"), 0, 15);

    LL_PROFILE_GPU_ZONE("AyaneStorm Depth of Field");
    LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);

    sCoCTarget.bindTarget();
    sCoCProgram.bind();
    const bool transparent_depth = sTransparentDepthReady && sOpaqueLayerReady &&
        sTransparentDepthTarget.getWidth() == source.getWidth() &&
        sTransparentDepthTarget.getHeight() == source.getHeight() &&
        sTransparentCoverageTarget.getWidth() == source.getWidth() &&
        sTransparentCoverageTarget.getHeight() == source.getHeight();
    const bool layered_transparency = transparent_depth &&
        sRiggedCoverageReady && sRiggedDepthReady && sWorldDepthReady;
    // Keep the rejected occupancy optimization dormant, including its draw
    // and mip generation, until its square-artifact defect is corrected.
    const bool use_occupancy = false;
    LLRenderTarget& opaque_depth = transparent_depth ? sOpaqueDepthTarget : depth;
    sCoCProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &opaque_depth, true,
                            LLTexUnit::TFO_POINT);
    if (transparent_depth)
    {
        sCoCProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE,
                               &sTransparentCoverageTarget, false, LLTexUnit::TFO_POINT);
        sCoCProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT,
                               &sTransparentDepthTarget, true, LLTexUnit::TFO_POINT);
        if (layered_transparency)
        {
            sCoCProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM,
                                   &sRiggedLayerTarget, false, LLTexUnit::TFO_POINT);
            sCoCProgram.bindTexture(LLShaderMgr::DEFERRED_POSITION,
                                   &sRiggedDepthTarget, true, LLTexUnit::TFO_POINT);
            sCoCProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE,
                                   &sWorldDepthTarget, true, LLTexUnit::TFO_POINT);
        }
    }
    sCoCProgram.uniform1i(U_HAS_TRANSPARENT_DEPTH, transparent_depth ? 1 : 0);
    sCoCProgram.uniform1i(U_HAS_LAYERS, layered_transparency ? 1 : 0);
    sCoCProgram.uniform1f(U_FOCAL_DISTANCE, focal_distance);
    sCoCProgram.uniform1f(U_BLUR_CONSTANT, blur_constant);
    sCoCProgram.uniform1f(U_TAN_PIXEL_ANGLE, tan_pixel_angle);
    sCoCProgram.uniform1f(U_MAGNIFICATION, magnification);
    sCoCProgram.uniform1f(U_MAX_COC, abs_max_coc);
    draw(screen_triangle);
    if (transparent_depth)
    {
        if (layered_transparency)
        {
            sCoCProgram.unbindTexture(LLShaderMgr::DEFERRED_EMISSIVE,
                                      sWorldDepthTarget.getUsage());
            sCoCProgram.unbindTexture(LLShaderMgr::DEFERRED_POSITION,
                                      sRiggedDepthTarget.getUsage());
            sCoCProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM,
                                      sRiggedLayerTarget.getUsage());
        }
        sCoCProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT,
                                  sTransparentDepthTarget.getUsage());
        sCoCProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE,
                                  sTransparentCoverageTarget.getUsage());
    }
    sCoCProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH, opaque_depth.getUsage());
    sCoCProgram.unbind();
    sCoCTarget.flush();

    if (layered_transparency && use_occupancy)
    {
        sOccupancyTarget.bindTarget();
        sOccupancyProgram.bind();
        sOccupancyProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE,
                                      &sCoCTarget, false, LLTexUnit::TFO_POINT, 1);
        draw(screen_triangle);
        sOccupancyProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE,
                                        sCoCTarget.getUsage());
        sOccupancyProgram.unbind();
        sOccupancyTarget.flush();
    }

    LLRenderTarget& opaque_color = transparent_depth ? sOpaqueColorTarget : source;

    sFarTarget.bindTarget();
    sFarProgram.bind();
    sFarProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &opaque_color, false, LLTexUnit::TFO_BILINEAR);
    sFarProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &sCoCTarget, false, LLTexUnit::TFO_BILINEAR);
    configureGather(sFarProgram, samples, far_radius, blades, roundness,
                    rotation, anamorphic, highlight_boost);
    sFarProgram.uniform1f(U_FOREGROUND_RADIUS, near_radius);
    draw(screen_triangle);
    sFarProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sCoCTarget.getUsage());
    sFarProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, opaque_color.getUsage());
    sFarProgram.unbind();
    sFarTarget.flush();

    sNearTarget.bindTarget();
    sNearProgram.bind();
    sNearProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &opaque_color, false, LLTexUnit::TFO_BILINEAR);
    sNearProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &sCoCTarget, false, LLTexUnit::TFO_BILINEAR);
    configureGather(sNearProgram, samples, near_radius, blades, roundness,
                    rotation, anamorphic, highlight_boost);
    draw(screen_triangle);
    sNearProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sCoCTarget.getUsage());
    sNearProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, opaque_color.getUsage());
    sNearProgram.unbind();
    sNearTarget.flush();

    if (transparent_depth)
    {
        auto gather_transparency = [&](LLRenderTarget& target, S32 plane,
                                       S32 layer_mode, F32 radius)
        {
            target.bindTarget();
            sTransparentProgram.bind();
            sTransparentProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE,
                                            &source, false, LLTexUnit::TFO_BILINEAR);
            sTransparentProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE,
                                            &sCoCTarget, false, LLTexUnit::TFO_BILINEAR,
                                            layered_transparency ? 1 : 0);
            sTransparentProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT,
                                            &sTransparentCoverageTarget, false,
                                            LLTexUnit::TFO_POINT);
            sTransparentProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM,
                                            &opaque_color, false, LLTexUnit::TFO_BILINEAR);
            if (layered_transparency)
            {
                sTransparentProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW0,
                                                &sOccupancyTarget, false,
                                                LLTexUnit::TFO_TRILINEAR);
                sTransparentProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR,
                                                &sRiggedLayerTarget, false,
                                                LLTexUnit::TFO_POINT);
                sTransparentProgram.bindTexture(LLShaderMgr::DEFERRED_POSITION,
                                                &sRiggedDepthTarget, true,
                                                LLTexUnit::TFO_POINT);
                sTransparentProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE,
                                                &sWorldDepthTarget, true,
                                                LLTexUnit::TFO_POINT);
            }
            configureGather(sTransparentProgram, samples, radius, blades,
                            roundness, rotation, anamorphic, highlight_boost);
            sTransparentProgram.uniform1i(U_PLANE, plane);
            sTransparentProgram.uniform1i(U_LAYER_MODE, layer_mode);
            // Temporarily bypass occupancy rejection to isolate square DoF artifacts.
            sTransparentProgram.uniform1i(U_USE_OCCUPANCY,
                                           use_occupancy ? 1 : 0);
            draw(screen_triangle);
            if (layered_transparency)
            {
                sTransparentProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW0,
                                                  sOccupancyTarget.getUsage());
                sTransparentProgram.unbindTexture(LLShaderMgr::DEFERRED_EMISSIVE,
                                                  sWorldDepthTarget.getUsage());
                sTransparentProgram.unbindTexture(LLShaderMgr::DEFERRED_POSITION,
                                                  sRiggedDepthTarget.getUsage());
                sTransparentProgram.unbindTexture(LLShaderMgr::DEFERRED_SPECULAR,
                                                  sRiggedLayerTarget.getUsage());
            }
            sTransparentProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM,
                                              opaque_color.getUsage());
            sTransparentProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT,
                                              sTransparentCoverageTarget.getUsage());
            sTransparentProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE,
                                              sCoCTarget.getUsage());
            sTransparentProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE,
                                              source.getUsage());
            sTransparentProgram.unbind();
            target.flush();
        };
        const S32 base_layer = layered_transparency ? 1 : 0;
        gather_transparency(sTransparentFarTarget, 1, base_layer, far_radius);
        gather_transparency(sTransparentNearTarget, -1, base_layer, near_radius);
        if (layered_transparency)
        {
            gather_transparency(sRiggedFarTarget, 1, 2, far_radius);
            gather_transparency(sRiggedNearTarget, -1, 2, near_radius);
        }
    }

    destination.bindTarget();
    sResolveProgram.bind();
    sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &opaque_color, false, LLTexUnit::TFO_BILINEAR);
    sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_PROJECTION, &source, false, LLTexUnit::TFO_BILINEAR);
    sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &sCoCTarget, false, LLTexUnit::TFO_BILINEAR);
    sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &sFarTarget, false, LLTexUnit::TFO_BILINEAR);
    sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sNearTarget, false, LLTexUnit::TFO_BILINEAR);
    if (transparent_depth)
    {
        sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR,
                                    &sTransparentCoverageTarget, false, LLTexUnit::TFO_POINT);
        sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE,
                                    &sTransparentFarTarget, false, LLTexUnit::TFO_BILINEAR);
        sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_POSITION,
                                    &sTransparentNearTarget, false, LLTexUnit::TFO_BILINEAR);
        if (layered_transparency)
        {
            sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW0,
                                        &sCoCTarget, false,
                                        LLTexUnit::TFO_BILINEAR, 1);
            sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW1,
                                        &sRiggedFarTarget, false,
                                        LLTexUnit::TFO_BILINEAR);
            sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW2,
                                        &sRiggedNearTarget, false,
                                        LLTexUnit::TFO_BILINEAR);
            sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW3,
                                        &sRiggedLayerTarget, false,
                                        LLTexUnit::TFO_POINT);
            sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW4,
                                        &sRiggedDepthTarget, true,
                                        LLTexUnit::TFO_POINT);
            sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW5,
                                        &sWorldDepthTarget, true,
                                        LLTexUnit::TFO_POINT);
        }
    }
    // The near layer carries its own coverage; this radius controls only the
    // signed far-layer transition at the focal plane.
    sResolveProgram.uniform1f(U_MAX_RADIUS, far_radius);
    sResolveProgram.uniform1f(U_NEAR_MAX_RADIUS, near_radius);
    sResolveProgram.uniform1i(U_DEBUG_MODE, debug_mode);
    sResolveProgram.uniform1i(U_HAS_TRANSPARENT_DEPTH, transparent_depth ? 1 : 0);
    sResolveProgram.uniform1i(U_HAS_LAYERS, layered_transparency ? 1 : 0);
    draw(screen_triangle);
    if (transparent_depth)
    {
        if (layered_transparency)
        {
            sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW5,
                                          sWorldDepthTarget.getUsage());
            sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW4,
                                          sRiggedDepthTarget.getUsage());
            sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW3,
                                          sRiggedLayerTarget.getUsage());
            sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW2,
                                          sRiggedNearTarget.getUsage());
            sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW1,
                                          sRiggedFarTarget.getUsage());
            sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW0,
                                          sCoCTarget.getUsage());
        }
        sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_POSITION,
                                      sTransparentNearTarget.getUsage());
        sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_EMISSIVE,
                                      sTransparentFarTarget.getUsage());
        sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_SPECULAR,
                                      sTransparentCoverageTarget.getUsage());
    }
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM, sNearTarget.getUsage());
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT, sFarTarget.getUsage());
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sCoCTarget.getUsage());
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_PROJECTION, source.getUsage());
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, opaque_color.getUsage());
    sResolveProgram.unbind();
    destination.flush();
    return true;
}
