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
#include "asdofaperture.h"
#include "asdofautofocus.h"
#include "asdoflive.h"
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
    LLGLSLShader sHighlightProgram;
    LLGLSLShader sSpriteProgram;
    LLGLSLShader sBackgroundProgram;
    LLGLSLShader sPostfilterProgram;

    LLRenderTarget sCoCTarget;
    // Gather targets carry, next to their color, the source blur radius
    // moments the postfilter reads (far: attachment 1; near: 2; transparent
    // gathers: 1).
    LLRenderTarget sFarTarget;
    // Two premultiplied foreground layers: 0 back, 1 front (plus near sprites).
    LLRenderTarget sNearTarget;
    // Foreground source pyramid for area taps (asDepthOfFieldNearF.glsl):
    // color, 11 reach bands and the front-layer weight in 4 attachments.
    LLRenderTarget sNearSourceTarget;
    // Highlight cells (energy + occupancy with mips; centroid, CoC;
    // brightness levels 0-3 and 4-7 with mips, for the budget ranking) and
    // the gather input with their energy removed. Allocated only with
    // sprites on.
    LLRenderTarget sCellTarget;
    LLRenderTarget sGatherInputTarget;
    // Background completion: weighted push pyramid and its pulled result.
    LLRenderTarget sBackgroundPushTarget;
    LLRenderTarget sBackgroundTarget;
    // Postfilter scratch: one layer filtered here, then blitted back.
    LLRenderTarget sPostfilterTarget;
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
    const LLStaticHashedString U_TARGET_RES("target_res");
    const LLStaticHashedString U_ISOLATION("isolation");
    const LLStaticHashedString U_HIGHLIGHT_PASS("highlight_pass");
    const LLStaticHashedString U_CELL_GRID("cell_grid");
    const LLStaticHashedString U_CELL_TOP_LEVEL("cell_top_level");
    const LLStaticHashedString U_SPRITE_BUDGET("sprite_budget");
    const LLStaticHashedString U_UNIT_AREA("unit_area");
    const LLStaticHashedString U_BG_PASS("bg_pass");
    const LLStaticHashedString U_MAX_LEVEL("max_level");
    const LLStaticHashedString U_NEAR_PASS("near_pass");
    const LLStaticHashedString U_USE_PYRAMID("use_pyramid");
    const LLStaticHashedString U_GATHER_PASS("gather_pass");
    const LLStaticHashedString U_SPLIT_RADIUS("split_radius");
    const LLStaticHashedString U_PLANE_KIND("plane_kind");
    const LLStaticHashedString U_MOMENT_CHANNEL("moment_channel");
    const LLStaticHashedString U_DEBUG_VIEW("debug_view");
    const LLStaticHashedString U_POSTFILTERED("postfiltered");
    const LLStaticHashedString U_FIELD_SCALE("field_scale");
    const LLStaticHashedString U_CAT_EYE("cat_eye");
    const LLStaticHashedString U_ASTIGMATISM("astigmatism");
    const LLStaticHashedString U_CA_SHIFT("ca_shift");
    const LLStaticHashedString U_FIELD_CURVATURE("field_curvature");
    const LLStaticHashedString U_VIGNETTE_SHIFT("vignette_shift");
    const LLStaticHashedString U_SA_STRENGTH("sa_strength");

    // Spatially varying lens character of the current frame (render()).
    using LensField = ASDepthOfField::LensField;
    LensField sLensField;

    // Lens field uniforms of the gathers and sprites (shaders without one
    // ignore it).
    void setLensUniforms(LLGLSLShader& shader)
    {
        shader.uniform2f(U_FIELD_SCALE, sLensField.mFieldScale[0], sLensField.mFieldScale[1]);
        shader.uniform1f(U_CAT_EYE, sLensField.mCatEye);
        shader.uniform1f(U_ASTIGMATISM, sLensField.mAstigmatism);
        shader.uniform1f(U_CA_SHIFT, sLensField.mAxialCA);
        shader.uniform1f(U_SA_STRENGTH, sLensField.mSpherical);
    }

    // Reads the lens character settings for this frame. Focus shifts are
    // given in percent of the focal length f; on the thin lens with a fixed
    // sensor, a focal shift p f moves the in-focus inverse distance by p / f,
    // which the CoC pass turns into K p / f pixels, with K = sqrt(2) *
    // blur_constant / (magnification * tan_pixel_angle) its pixels per unit
    // of (1 / focus - 1 / distance) (asDepthOfFieldCoCF.glsl) and
    // f = magnification * S / (1 + magnification) (S the focus distance).
    LensField computeLensField(U32 width, U32 height, F32 focal_distance, F32 blur_constant,
                               F32 tan_pixel_angle, F32 magnification, F32 max_coc)
    {
        LensField field;
        const F32 aspect = (F32)width / (F32)llmax(height, 1U);
        const F32 diagonal = sqrtf(aspect * aspect + 1.f);
        field.mFieldScale[0] = 2.f * aspect / diagonal;
        field.mFieldScale[1] = 2.f / diagonal;

        // Shared with the aperture-sampled renderer.
        if (gSavedSettings.getBOOL("ASDepthOfFieldApertureCatEye"))
        {
            field.mCatEye = llclamp(gSavedSettings.getF32("ASDepthOfFieldApertureCatEyeStrength"), 0.f, 2.f);
            if (gSavedSettings.getBOOL("ASDepthOfFieldApertureCatEyeDarken"))
            {
                field.mVignette = field.mCatEye;
            }
        }
        if (gSavedSettings.getBOOL("ASDepthOfFieldApertureSpherical"))
        {
            field.mSpherical = llclamp(gSavedSettings.getF32("ASDepthOfFieldApertureSphericalStrength"), -5.f, 5.f);
        }

        const F32 focus = -focal_distance;
        if (focus <= 0.f || magnification <= 0.f || tan_pixel_angle <= 0.f || max_coc <= 0.f)
        {
            return field;
        }
        const F32 focal_length = magnification * focus / (1.f + magnification);
        const F32 pixels_per_inverse = F_SQRT2 * fabsf(blur_constant) / (magnification * tan_pixel_angle);
        // Normalized CoC per unit relative focal shift.
        const F32 shift_scale = pixels_per_inverse / (focal_length * max_coc);
        if (gSavedSettings.getBOOL("ASDepthOfFieldApertureAxialCA"))
        {
            // Red-to-blue shift alpha f: each extreme wavelength moves by
            // half (the aperture-sampled renderer's 1/S' = 1/S - s alpha / 2f).
            field.mAxialCA = 0.5f * 0.01f *
                llclamp(gSavedSettings.getF32("ASDepthOfFieldApertureAxialCAStrength"), 0.f, 10.f) * shift_scale;
        }
        if (gSavedSettings.getBOOL("ASDepthOfFieldFieldCurvature"))
        {
            field.mCurvature = 0.01f *
                llclamp(gSavedSettings.getF32("ASDepthOfFieldFieldCurvatureStrength"), -3.f, 3.f) * shift_scale;
        }
        if (gSavedSettings.getBOOL("ASDepthOfFieldAstigmatism"))
        {
            field.mAstigmatism = 0.01f *
                llclamp(gSavedSettings.getF32("ASDepthOfFieldAstigmatismStrength"), -3.f, 3.f) * shift_scale;
        }
        return field;
    }

    // Area taps (asDepthOfFieldNearF.glsl): sources blurred less than this
    // stay point taps, which are dense enough there (tap spacing at distance
    // r under r / 2 once r > 8 pi R / N) and keep their own color.
    F32 pyramidSplitRadius(F32 radius, S32 samples)
    {
        return llmax(1.f, 8.f * F_PI * radius / (F32)llmax(samples, 1));
    }

    // The pyramid only helps when some sources can exceed the split; below
    // 2 px of maximum blur its bands degenerate.
    bool usePyramid(F32 radius, S32 samples)
    {
        return radius >= 2.f && pyramidSplitRadius(radius, samples) < radius;
    }

    // Full-resolution pixels per highlight cell side (CELL_SIZE in
    // asDepthOfFieldHighlightF.glsl).
    const U32 HIGHLIGHT_CELL_SIZE = 8;

    void releaseSpriteResources()
    {
        sCellTarget.release();
        sGatherInputTarget.release();
    }

    void releaseGatherResources()
    {
        sCoCTarget.release();
        sFarTarget.release();
        sNearTarget.release();
        sNearSourceTarget.release();
        releaseSpriteResources();
        sBackgroundPushTarget.release();
        sBackgroundTarget.release();
        sPostfilterTarget.release();
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
            sFarTarget.isComplete() && sFarTarget.getNumTextures() == 2 &&
            sNearTarget.isComplete() && sNearTarget.getNumTextures() == 3 &&
            sNearSourceTarget.isComplete() && sNearSourceTarget.getNumTextures() == 4 &&
            sBackgroundPushTarget.isComplete() && sBackgroundTarget.isComplete() &&
            sPostfilterTarget.isComplete() &&
            sTransparentFarTarget.isComplete() && sTransparentFarTarget.getNumTextures() == 2 &&
            sTransparentNearTarget.isComplete() && sTransparentNearTarget.getNumTextures() == 2 &&
            sRiggedFarTarget.isComplete() && sRiggedFarTarget.getNumTextures() == 2 &&
            sRiggedNearTarget.isComplete() && sRiggedNearTarget.getNumTextures() == 2 &&
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
            !sFarTarget.addColorAttachment(GL_RGBA16F) ||
            !sNearTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sNearTarget.addColorAttachment(GL_RGBA16F) ||
            !sNearTarget.addColorAttachment(GL_RGBA16F) ||
            !sNearSourceTarget.allocate(blur_width, blur_height, GL_RGBA16F,
                                        false, LLTexUnit::TT_TEXTURE,
                                        LLTexUnit::TMG_MANUAL) ||
            !sNearSourceTarget.addColorAttachment(GL_RGBA16F) ||
            !sNearSourceTarget.addColorAttachment(GL_RGBA16F) ||
            !sNearSourceTarget.addColorAttachment(GL_RGBA16F) ||
            !sBackgroundPushTarget.allocate(blur_width, blur_height, GL_RGBA16F,
                                            false, LLTexUnit::TT_TEXTURE,
                                            LLTexUnit::TMG_MANUAL) ||
            !sBackgroundPushTarget.addColorAttachment(GL_RGBA16F) ||
            !sBackgroundTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sPostfilterTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sTransparentFarTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sTransparentFarTarget.addColorAttachment(GL_RGBA16F) ||
            !sTransparentNearTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sTransparentNearTarget.addColorAttachment(GL_RGBA16F) ||
            !sRiggedFarTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sRiggedFarTarget.addColorAttachment(GL_RGBA16F) ||
            !sRiggedNearTarget.allocate(blur_width, blur_height, GL_RGBA16F) ||
            !sRiggedNearTarget.addColorAttachment(GL_RGBA16F) ||
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
        setLensUniforms(shader);
    }

    void draw(LLVertexBuffer& triangle)
    {
        triangle.setBuffer();
        triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    bool ensureSpriteResources(U32 width, U32 height)
    {
        const U32 cells_x = (width + HIGHLIGHT_CELL_SIZE - 1) / HIGHLIGHT_CELL_SIZE;
        const U32 cells_y = (height + HIGHLIGHT_CELL_SIZE - 1) / HIGHLIGHT_CELL_SIZE;
        if (sCellTarget.isComplete() && sCellTarget.getNumTextures() == 4 &&
            sCellTarget.getWidth() == cells_x && sCellTarget.getHeight() == cells_y &&
            sGatherInputTarget.isComplete() &&
            sGatherInputTarget.getWidth() == width && sGatherInputTarget.getHeight() == height)
        {
            return true;
        }
        releaseSpriteResources();
        if (!sCellTarget.allocate(cells_x, cells_y, GL_RGBA16F, false,
                                  LLTexUnit::TT_TEXTURE, LLTexUnit::TMG_MANUAL) ||
            !sCellTarget.addColorAttachment(GL_RGBA16F) ||
            !sCellTarget.addColorAttachment(GL_RGBA16F) ||
            !sCellTarget.addColorAttachment(GL_RGBA16F) ||
            !sGatherInputTarget.allocate(width, height, GL_RGBA16F))
        {
            releaseSpriteResources();
            return false;
        }
        return true;
    }

    bool shadersComplete()
    {
        return sCoCProgram.isComplete() && sFarProgram.isComplete() &&
               sNearProgram.isComplete() && sTransparentProgram.isComplete() &&
               sOccupancyProgram.isComplete() && sResolveProgram.isComplete() &&
               sHighlightProgram.isComplete() && sSpriteProgram.isComplete() &&
               sBackgroundProgram.isComplete() && sPostfilterProgram.isComplete();
    }

    // Top mip level index of a target (its 1x1 level).
    S32 topMipLevel(const LLRenderTarget& target)
    {
        return (S32)floorf(log2f((F32)llmax(target.getWidth(), target.getHeight())));
    }

    void generateMips(LLRenderTarget& target, U32 attachment)
    {
        // Explicit unit: glGenerateMipmap acts on the active unit's texture.
        LLTexUnit* unit = gGL.getTexUnit(0);
        unit->bindManual(LLTexUnit::TT_TEXTURE, target.getTexture(attachment), true);
        unit->activate();
        glGenerateMipmap(GL_TEXTURE_2D);
        unit->unbind(LLTexUnit::TT_TEXTURE);
    }
}

extern bool gCubeSnapshot;

void ASDepthOfField::registerUICallbacks()
{
    ASDoFRenderer::registerUICallbacks();
    ASDoFAutofocus::registerUICallbacks();
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASDepthOfField.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            static const std::vector<std::string> controls = {
                "ASDepthOfFieldMode", "ASDepthOfFieldBackend",
                "ASDepthOfFieldFocusMode",
                "ASDepthOfFieldQuality", "ASDepthOfFieldNearRadius",
                "ASDepthOfFieldFarRadius", "ASDepthOfFieldApertureBlades",
                "ASDepthOfFieldApertureRoundness", "ASDepthOfFieldApertureRotation",
                "ASDepthOfFieldAnamorphicRatio", "ASDepthOfFieldHighlightBoost",
                "ASDepthOfFieldHighlightSprites", "ASDepthOfFieldHighlightIsolation",
                "ASDepthOfFieldHighlightMaxSprites", "ASDepthOfFieldPostfilter",
                "ASDepthOfFieldPhysicalBlur", "ASDepthOfFieldMaxBlur",
                "ASDepthOfFieldFieldCurvature", "ASDepthOfFieldFieldCurvatureStrength",
                "ASDepthOfFieldAstigmatism", "ASDepthOfFieldAstigmatismStrength",
                "ASDepthOfFieldDebug", "ASDepthOfFieldApertureSamples",
                "ASDepthOfFieldApertureMaxSamples", "ASDepthOfFieldApertureSnapshotSamples",
                "ASDepthOfFieldApertureSnapshotMaxSeconds",
                "ASDepthOfFieldApertureResidualBlur", "ASDepthOfFieldApertureSmoothing",
                "ASDepthOfFieldApertureAxialCA", "ASDepthOfFieldApertureAxialCAStrength",
                "ASDepthOfFieldApertureCatEye", "ASDepthOfFieldApertureCatEyeStrength",
                "ASDepthOfFieldApertureCatEyeDarken",
                "ASDepthOfFieldApertureSpherical", "ASDepthOfFieldApertureSphericalStrength",
                "ASDepthOfFieldApertureHighlights", "ASDepthOfFieldApertureHighlightStrength",
                "ASDepthOfFieldApertureHighlightThreshold",
                "ASDepthOfFieldApertureShowProgress",
                "ASDepthOfFieldAutofocusArea", "ASDepthOfFieldAutofocusX",
                "ASDepthOfFieldAutofocusY", "ASDepthOfFieldAutofocusTime",
                "ASDepthOfFieldAutofocusNearPriority", "ASDepthOfFieldAutofocusShowArea",
                "ASDepthOfFieldAutofocusLockMode", "ASDepthOfFieldAutofocusTrackOutside",
                "ASDepthOfFieldAutofocusEyeRadius", "ASDepthOfFieldLiveDebug",
                "ASDepthOfFieldLiveTransparency", "ASDepthOfFieldLiveExactLayers"
            };
            const std::string name = data.asString();
            if (name == "All")
            {
                // Renderer, backend and focus mode are mode choices, not
                // tuning values.
                for (auto it = controls.begin() + 3; it != controls.end(); ++it)
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
    shaders.push_back(&sHighlightProgram);
    shaders.push_back(&sSpriteProgram);
    shaders.push_back(&sBackgroundProgram);
    shaders.push_back(&sPostfilterProgram);
    // Aperture-sampled and Live renderers and autofocus share this module's
    // registration hooks.
    ASDoFLive::registerShaders(shaders);
    ASDoFRenderer::registerShaders(shaders);
    ASDoFAutofocus::registerShaders(shaders);
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
        { &sResolveProgram, "AyaneStorm Depth of Field Resolve Shader", "deferred/asDepthOfFieldResolveF.glsl" },
        { &sHighlightProgram, "AyaneStorm Depth of Field Highlight Extraction Shader", "deferred/asDepthOfFieldHighlightF.glsl" },
        { &sBackgroundProgram, "AyaneStorm Depth of Field Background Completion Shader", "deferred/asDepthOfFieldBackgroundF.glsl" },
        { &sPostfilterProgram, "AyaneStorm Depth of Field Postfilter Shader", "deferred/asDepthOfFieldPostfilterF.glsl" }
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

    // Attribute-free instanced aperture sprites (see asDepthOfFieldSpriteV.glsl).
    sSpriteProgram.mName = "AyaneStorm Depth of Field Highlight Sprite Shader";
    sSpriteProgram.mShaderFiles.clear();
    sSpriteProgram.clearPermutations();
    sSpriteProgram.mFeatures.attachNothing = true;
    sSpriteProgram.mShaderFiles.emplace_back("deferred/asDepthOfFieldSpriteV.glsl", GL_VERTEX_SHADER);
    sSpriteProgram.mShaderFiles.emplace_back("deferred/asDepthOfFieldSpriteF.glsl", GL_FRAGMENT_SHADER);
    sSpriteProgram.mShaderLevel = shader_level;
    success = sSpriteProgram.createShader() && success;

    success = ASDoFLive::createShaders(shader_level) && success;
    success = ASDoFRenderer::createShaders(shader_level) && success;
    success = ASDoFAutofocus::createShaders(shader_level) && success;
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
    sHighlightProgram.unload();
    sSpriteProgram.unload();
    sBackgroundProgram.unload();
    sPostfilterProgram.unload();
    ASDoFLive::unloadShaders();
    ASDoFRenderer::unloadShaders();
    ASDoFAutofocus::unloadShaders();
    releaseResources();
}

bool ASDepthOfField::usesScreenSpaceRenderer()
{
    const S32 mode = gSavedSettings.getS32("ASDepthOfFieldMode");
    return mode == 1 || mode == ASDoFLive::LIVE_MODE;
}

ASDepthOfField::LensField ASDepthOfField::lensField(U32 width, U32 height, F32 focal_distance,
                                                    F32 blur_constant, F32 tan_pixel_angle,
                                                    F32 magnification, F32 max_coc)
{
    return computeLensField(width, height, focal_distance, blur_constant, tan_pixel_angle,
                            magnification, max_coc);
}

void ASDepthOfField::releaseResources()
{
    ASDoFLive::releaseResources();
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
    if (gSavedSettings.getS32("ASDepthOfFieldMode") == ASDoFLive::LIVE_MODE)
    {
        // Live DoF keeps the opaque layer for its transparency bins; the
        // alpha pool keeps its vanilla DoF depth pass (false).
        ASDoFLive::prepareCapture(width, height);
        return false;
    }
    if (gSavedSettings.getS32("ASDepthOfFieldMode") != 1 ||
        width == 0 || height == 0 || ASBackgroundIsolate::isActive() ||
        !shadersComplete())
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
    const S32 mode = gSavedSettings.getS32("ASDepthOfFieldMode");
    if (mode == ASDoFLive::LIVE_MODE)
    {
        // The Advanced renderer's targets are not needed meanwhile.
        if (sCoCTarget.isComplete() || sFarTarget.isComplete() || sNearTarget.isComplete())
        {
            releaseGatherResources();
        }
        return ASDoFLive::render(source, destination, depth, screen_triangle, focal_distance,
                                 blur_constant, tan_pixel_angle, magnification, max_coc);
    }
    ASDoFLive::releaseResources();
    if (mode != 1)
    {
        if (sCoCTarget.isComplete() || sFarTarget.isComplete() || sNearTarget.isComplete())
        {
            releaseResources();
        }
        return false;
    }

    if (!shadersComplete() ||
        gCubeSnapshot || ASBackgroundIsolate::isActive() || &source == &destination ||
        source.getWidth() <= 0 || source.getHeight() <= 0 ||
        source.getWidth() != destination.getWidth() || source.getHeight() != destination.getHeight())
    {
        if (!shadersComplete())
        {
            LL_WARNS_ONCE("ASDepthOfField") << "Advanced DoF shaders are incomplete; using Firestorm DoF." << LL_ENDL;
        }
        return false;
    }

    // Physical blur size by default: the lens CoC up to a share of the image
    // height (the same framing at any resolution, snapshots included),
    // instead of Firestorm's Max CoF (10 px by default), which was most of
    // the difference with the aperture-sampled renderer.
    const F32 abs_max_coc = gSavedSettings.getBOOL("ASDepthOfFieldPhysicalBlur") ?
        0.01f * llclamp(gSavedSettings.getF32("ASDepthOfFieldMaxBlur"), 1.f, 10.f) * (F32)source.getHeight() :
        llclamp(fabsf(max_coc), 0.f, 150.f);
    sLensField = computeLensField(source.getWidth(), source.getHeight(), focal_distance,
                                  blur_constant, tan_pixel_angle, magnification, abs_max_coc);
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
    const S32 debug_mode = llclamp(gSavedSettings.getS32("ASDepthOfFieldDebug"), 0, 25);
    const F32 isolation = llclamp(gSavedSettings.getF32("ASDepthOfFieldHighlightIsolation"), 1.2f, 8.f);
    const F32 sprite_budget = (F32)llclamp(gSavedSettings.getS32("ASDepthOfFieldHighlightMaxSprites"), 256, 32768);

    // Sprites are optional; without their resources the gathers keep every
    // highlight, as before.
    bool sprites = gSavedSettings.getBOOL("ASDepthOfFieldHighlightSprites");
    if (sprites && !ensureSpriteResources(source.getWidth(), source.getHeight()))
    {
        LL_WARNS_ONCE("ASDepthOfField") << "Advanced DoF highlight sprite allocation failed; "
                                           "highlights stay in the gather." << LL_ENDL;
        sprites = false;
    }
    if (!sprites && sCellTarget.isComplete())
    {
        releaseSpriteResources();
    }

    ASDoFAperture::Shape shape;
    shape.mBlades = blades;
    shape.mRoundness = roundness;
    shape.mRotation = rotation;
    shape.mAnamorphic = anamorphic;
    const F32 unit_area = ASDoFAperture::unitArea(shape);

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
    sCoCProgram.uniform2f(U_FIELD_SCALE, sLensField.mFieldScale[0], sLensField.mFieldScale[1]);
    sCoCProgram.uniform1f(U_FIELD_CURVATURE, sLensField.mCurvature);
    sCoCProgram.uniform1f(U_ASTIGMATISM, sLensField.mAstigmatism);
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
    const S32 cells_x = sprites ? (S32)sCellTarget.getWidth() : 0;
    const S32 cells_y = sprites ? (S32)sCellTarget.getHeight() : 0;
    const S32 cell_top_level = sprites ? topMipLevel(sCellTarget) : 0;

    // Highlight extraction: cells of isolated defocused highlight energy,
    // then the gather input with that energy removed (sprites redraw it).
    if (sprites)
    {
        auto highlight_pass = [&](LLRenderTarget& target, S32 pass)
        {
            target.bindTarget();
            sHighlightProgram.bind();
            sHighlightProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &opaque_color,
                                          false, LLTexUnit::TFO_POINT);
            sHighlightProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &sCoCTarget,
                                          false, LLTexUnit::TFO_POINT);
            if (pass == 1)
            {
                sHighlightProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR, &sCellTarget,
                                              false, LLTexUnit::TFO_TRILINEAR);
                sHighlightProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &sCellTarget,
                                              false, LLTexUnit::TFO_TRILINEAR, 2);
                sHighlightProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sCellTarget,
                                              false, LLTexUnit::TFO_TRILINEAR, 3);
            }
            sHighlightProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES,
                                        (F32)sWidth, (F32)sHeight);
            sHighlightProgram.uniform1f(U_MAX_RADIUS, far_radius);
            sHighlightProgram.uniform1f(U_NEAR_MAX_RADIUS, near_radius);
            sHighlightProgram.uniform1f(U_ISOLATION, isolation);
            sHighlightProgram.uniform1i(U_HIGHLIGHT_PASS, pass);
            sHighlightProgram.uniform2i(U_CELL_GRID, cells_x, cells_y);
            sHighlightProgram.uniform1i(U_CELL_TOP_LEVEL, cell_top_level);
            sHighlightProgram.uniform1f(U_SPRITE_BUDGET, sprite_budget);
            draw(screen_triangle);
            if (pass == 1)
            {
                sHighlightProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM,
                                                sCellTarget.getUsage());
                sHighlightProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT,
                                                sCellTarget.getUsage());
                sHighlightProgram.unbindTexture(LLShaderMgr::DEFERRED_SPECULAR,
                                                sCellTarget.getUsage());
            }
            sHighlightProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sCoCTarget.getUsage());
            sHighlightProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, opaque_color.getUsage());
            sHighlightProgram.unbind();
            target.flush();
        };
        highlight_pass(sCellTarget, 0);
        // The top levels' averages give the sprite count and the counts per
        // brightness level for the budget rule.
        generateMips(sCellTarget, 0);
        generateMips(sCellTarget, 2);
        generateMips(sCellTarget, 3);
        highlight_pass(sGatherInputTarget, 1);
    }
    LLRenderTarget& gather_input = sprites ? sGatherInputTarget : opaque_color;

    // Background completion behind foreground pixels: weighted push, mips,
    // then pull into sBackgroundTarget (rgb, signed CoC).
    {
        auto background_pass = [&](LLRenderTarget& target, S32 pass)
        {
            target.bindTarget();
            sBackgroundProgram.bind();
            sBackgroundProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &gather_input,
                                           false, pass == 0 ? LLTexUnit::TFO_POINT
                                                            : LLTexUnit::TFO_BILINEAR);
            sBackgroundProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &sCoCTarget,
                                           false, LLTexUnit::TFO_POINT);
            if (pass == 1)
            {
                sBackgroundProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR,
                                               &sBackgroundPushTarget, false,
                                               LLTexUnit::TFO_TRILINEAR, 0);
                sBackgroundProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE,
                                               &sBackgroundPushTarget, false,
                                               LLTexUnit::TFO_TRILINEAR, 1);
            }
            sBackgroundProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES,
                                         (F32)sWidth, (F32)sHeight);
            sBackgroundProgram.uniform2f(U_TARGET_RES, (F32)sBlurWidth, (F32)sBlurHeight);
            sBackgroundProgram.uniform1i(U_BG_PASS, pass);
            // Enough levels to span the largest foreground disc.
            const F32 blur_radius = llmax(near_radius * (F32)sBlurWidth / (F32)sWidth, 1.f);
            sBackgroundProgram.uniform1i(U_MAX_LEVEL,
                llclamp((S32)ceilf(log2f(blur_radius)) + 2, 1,
                        topMipLevel(sBackgroundPushTarget)));
            draw(screen_triangle);
            if (pass == 1)
            {
                sBackgroundProgram.unbindTexture(LLShaderMgr::DEFERRED_EMISSIVE,
                                                 sBackgroundPushTarget.getUsage());
                sBackgroundProgram.unbindTexture(LLShaderMgr::DEFERRED_SPECULAR,
                                                 sBackgroundPushTarget.getUsage());
            }
            sBackgroundProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sCoCTarget.getUsage());
            sBackgroundProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, gather_input.getUsage());
            sBackgroundProgram.unbind();
            target.flush();
        };
        background_pass(sBackgroundPushTarget, 0);
        generateMips(sBackgroundPushTarget, 0);
        generateMips(sBackgroundPushTarget, 1);
        background_pass(sBackgroundTarget, 1);
    }

    // Additive aperture sprites for one plane into the currently bound
    // target: far into sFarTarget, near into sNearTarget's front layer.
    auto draw_sprites = [&](S32 plane, F32 target_width, F32 target_height)
    {
        LLGLEnable sprite_blend(GL_BLEND);
        gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
        sSpriteProgram.bind();
        sSpriteProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR, &sCellTarget,
                                   false, LLTexUnit::TFO_TRILINEAR, 0);
        sSpriteProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE, &sCellTarget,
                                   false, LLTexUnit::TFO_POINT, 1);
        sSpriteProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &sCellTarget,
                                   false, LLTexUnit::TFO_TRILINEAR, 2);
        sSpriteProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sCellTarget,
                                   false, LLTexUnit::TFO_TRILINEAR, 3);
        sSpriteProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)sWidth, (F32)sHeight);
        sSpriteProgram.uniform2f(U_TARGET_RES, target_width, target_height);
        sSpriteProgram.uniform1f(U_MAX_RADIUS, far_radius);
        sSpriteProgram.uniform1f(U_NEAR_MAX_RADIUS, near_radius);
        sSpriteProgram.uniform1i(U_APERTURE_BLADES, blades);
        sSpriteProgram.uniform1f(U_APERTURE_ROUNDNESS, roundness);
        sSpriteProgram.uniform1f(U_APERTURE_ROTATION, rotation);
        sSpriteProgram.uniform1f(U_ANAMORPHIC_RATIO, anamorphic);
        sSpriteProgram.uniform1f(U_UNIT_AREA, unit_area);
        sSpriteProgram.uniform1i(U_PLANE, plane);
        sSpriteProgram.uniform2i(U_CELL_GRID, cells_x, cells_y);
        sSpriteProgram.uniform1i(U_CELL_TOP_LEVEL, cell_top_level);
        sSpriteProgram.uniform1f(U_SPRITE_BUDGET, sprite_budget);
        setLensUniforms(sSpriteProgram);
        // Attribute-free: any bound vertex buffer satisfies the core-profile
        // VAO; positions come from gl_VertexID/gl_InstanceID.
        screen_triangle.setBuffer();
        glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2 * cells_x * cells_y);
        sSpriteProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM, sCellTarget.getUsage());
        sSpriteProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT, sCellTarget.getUsage());
        sSpriteProgram.unbindTexture(LLShaderMgr::DEFERRED_EMISSIVE, sCellTarget.getUsage());
        sSpriteProgram.unbindTexture(LLShaderMgr::DEFERRED_SPECULAR, sCellTarget.getUsage());
        sSpriteProgram.unbind();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    };

    // Sampling-noise postfilter (asDepthOfFieldPostfilterF.glsl): one
    // gathered layer is filtered into sPostfilterTarget, then blitted back
    // into its own attachment. Runs before the sprites, so the analytic
    // aperture shapes stay crisp. Debug 25 always runs it (its view).
    const bool postfilter = gSavedSettings.getBOOL("ASDepthOfFieldPostfilter") ||
                            debug_mode == 25;
    auto postfilter_layer = [&](LLRenderTarget& target, U32 color_attachment,
                                U32 moment_attachment, S32 moment_channel,
                                S32 plane_kind, F32 radius)
    {
        LLGLDisable scissor(GL_SCISSOR_TEST);
        sPostfilterTarget.bindTarget();
        const U32 filter_fbo = LLRenderTarget::sCurFBO;
        sPostfilterProgram.bind();
        sPostfilterProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &target, false,
                                       LLTexUnit::TFO_POINT, color_attachment);
        sPostfilterProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &target, false,
                                       LLTexUnit::TFO_POINT, moment_attachment);
        sPostfilterProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)sWidth, (F32)sHeight);
        sPostfilterProgram.uniform2f(U_TARGET_RES, (F32)sBlurWidth, (F32)sBlurHeight);
        sPostfilterProgram.uniform1i(U_SAMPLE_COUNT, samples);
        sPostfilterProgram.uniform1f(U_MAX_RADIUS, radius);
        sPostfilterProgram.uniform1i(U_PLANE_KIND, plane_kind);
        sPostfilterProgram.uniform1i(U_MOMENT_CHANNEL, moment_channel);
        sPostfilterProgram.uniform1i(U_DEBUG_VIEW, debug_mode == 25 ? 1 : 0);
        draw(screen_triangle);
        sPostfilterProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, target.getUsage());
        sPostfilterProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, target.getUsage());
        sPostfilterProgram.unbind();
        sPostfilterTarget.flush();

        // Framebuffer blits are part of the OpenGL 4.1 baseline; the draw
        // buffer selects the one attachment to replace.
        target.bindTarget();
        const U32 target_fbo = LLRenderTarget::sCurFBO;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, filter_fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
        glDrawBuffer(GL_COLOR_ATTACHMENT0 + color_attachment);
        glBlitFramebuffer(0, 0, sBlurWidth, sBlurHeight, 0, 0, sBlurWidth, sBlurHeight,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
        target.flush();
    };

    sFarTarget.bindTarget();
    sFarProgram.bind();
    sFarProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &gather_input, false, LLTexUnit::TFO_BILINEAR);
    sFarProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &sCoCTarget, false, LLTexUnit::TFO_BILINEAR);
    sFarProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &sBackgroundTarget, false, LLTexUnit::TFO_BILINEAR);
    configureGather(sFarProgram, samples, far_radius, blades, roundness,
                    rotation, anamorphic, highlight_boost);
    draw(screen_triangle);
    sFarProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT, sBackgroundTarget.getUsage());
    sFarProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sCoCTarget.getUsage());
    sFarProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, gather_input.getUsage());
    sFarProgram.unbind();
    sFarTarget.flush();
    if (postfilter)
    {
        postfilter_layer(sFarTarget, 0, 1, 0, 0, far_radius);
    }
    if (sprites)
    {
        sFarTarget.bindTarget();
        draw_sprites(1, (F32)sBlurWidth, (F32)sBlurHeight);
        sFarTarget.flush();
    }

    // Foreground source pyramid, so near taps integrate an area instead of
    // a point (fine pattern on defocused hair), for sources above the split
    // radius only (see usePyramid()).
    const bool near_pyramid = usePyramid(near_radius, samples);
    static const LLShaderMgr::eGLSLReservedUniforms pyramid_slots[] = {
        LLShaderMgr::DEFERRED_SPECULAR, LLShaderMgr::DEFERRED_EMISSIVE,
        LLShaderMgr::DEFERRED_LIGHT, LLShaderMgr::DEFERRED_BLOOM };
    if (near_pyramid)
    {
        sNearSourceTarget.bindTarget();
        sNearProgram.bind();
        sNearProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &gather_input, false, LLTexUnit::TFO_POINT);
        sNearProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &sCoCTarget, false, LLTexUnit::TFO_POINT);
        configureGather(sNearProgram, samples, near_radius, blades, roundness,
                        rotation, anamorphic, highlight_boost);
        sNearProgram.uniform2f(U_TARGET_RES, (F32)sBlurWidth, (F32)sBlurHeight);
        sNearProgram.uniform1i(U_NEAR_PASS, 0);
        sNearProgram.uniform1f(U_SPLIT_RADIUS, pyramidSplitRadius(near_radius, samples));
        draw(screen_triangle);
        sNearProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sCoCTarget.getUsage());
        sNearProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, gather_input.getUsage());
        sNearProgram.unbind();
        sNearSourceTarget.flush();
        for (U32 attachment = 0; attachment < 4; ++attachment)
        {
            generateMips(sNearSourceTarget, attachment);
        }
    }

    sNearTarget.bindTarget();
    sNearProgram.bind();
    sNearProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &gather_input, false, LLTexUnit::TFO_BILINEAR);
    sNearProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &sCoCTarget, false, LLTexUnit::TFO_BILINEAR);
    if (near_pyramid)
    {
        for (U32 attachment = 0; attachment < 4; ++attachment)
        {
            sNearProgram.bindTexture(pyramid_slots[attachment], &sNearSourceTarget, false,
                                     LLTexUnit::TFO_TRILINEAR, attachment);
        }
    }
    configureGather(sNearProgram, samples, near_radius, blades, roundness,
                    rotation, anamorphic, highlight_boost);
    sNearProgram.uniform2f(U_TARGET_RES, (F32)sBlurWidth, (F32)sBlurHeight);
    sNearProgram.uniform1i(U_NEAR_PASS, 1);
    sNearProgram.uniform1i(U_USE_PYRAMID, near_pyramid ? 1 : 0);
    sNearProgram.uniform1f(U_SPLIT_RADIUS, pyramidSplitRadius(near_radius, samples));
    draw(screen_triangle);
    if (near_pyramid)
    {
        for (U32 attachment = 0; attachment < 4; ++attachment)
        {
            sNearProgram.unbindTexture(pyramid_slots[attachment], sNearSourceTarget.getUsage());
        }
    }
    sNearProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sCoCTarget.getUsage());
    sNearProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, gather_input.getUsage());
    sNearProgram.unbind();
    sNearTarget.flush();
    if (postfilter)
    {
        // Back layer: moments .xy; front layer: .zw.
        postfilter_layer(sNearTarget, 0, 2, 0, 1, near_radius);
        postfilter_layer(sNearTarget, 1, 2, 1, 1, near_radius);
    }
    if (sprites)
    {
        sNearTarget.bindTarget();
        draw_sprites(-1, (F32)sBlurWidth, (F32)sBlurHeight);
        sNearTarget.flush();
    }

    if (transparent_depth)
    {
        // One transparent pass: gather_pass 0 builds this gather's source
        // pyramid into the shared sNearSourceTarget (the opaque near gather
        // is done with it), gather_pass 1 gathers, with area taps when the
        // pyramid was built (see asDepthOfFieldTransparentF.glsl).
        auto transparent_pass = [&](LLRenderTarget& target, S32 plane,
                                    S32 layer_mode, F32 radius, S32 pass,
                                    bool pyramid)
        {
            static const LLShaderMgr::eGLSLReservedUniforms pyramid_slots[] = {
                LLShaderMgr::DEFERRED_SHADOW1, LLShaderMgr::DEFERRED_SHADOW2,
                LLShaderMgr::DEFERRED_SHADOW3, LLShaderMgr::DEFERRED_SHADOW4 };
            target.bindTarget();
            sTransparentProgram.bind();
            if (pass == 1 && pyramid)
            {
                for (U32 attachment = 0; attachment < 4; ++attachment)
                {
                    sTransparentProgram.bindTexture(pyramid_slots[attachment],
                                                    &sNearSourceTarget, false,
                                                    LLTexUnit::TFO_TRILINEAR, attachment);
                }
            }
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
            sTransparentProgram.uniform2f(U_TARGET_RES, (F32)sBlurWidth, (F32)sBlurHeight);
            sTransparentProgram.uniform1i(U_GATHER_PASS, pass);
            sTransparentProgram.uniform1i(U_USE_PYRAMID, pyramid ? 1 : 0);
            sTransparentProgram.uniform1f(U_SPLIT_RADIUS, pyramidSplitRadius(radius, samples));
            draw(screen_triangle);
            if (pass == 1 && pyramid)
            {
                for (U32 attachment = 0; attachment < 4; ++attachment)
                {
                    sTransparentProgram.unbindTexture(pyramid_slots[attachment],
                                                      sNearSourceTarget.getUsage());
                }
            }
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
        auto gather_transparency = [&](LLRenderTarget& target, S32 plane,
                                       S32 layer_mode, F32 radius)
        {
            const bool pyramid = usePyramid(radius, samples);
            if (pyramid)
            {
                transparent_pass(sNearSourceTarget, plane, layer_mode, radius, 0, false);
                for (U32 attachment = 0; attachment < 4; ++attachment)
                {
                    generateMips(sNearSourceTarget, attachment);
                }
            }
            transparent_pass(target, plane, layer_mode, radius, 1, pyramid);
            if (postfilter)
            {
                postfilter_layer(target, 0, 1, 0, 1, radius);
            }
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
    sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sNearTarget, false, LLTexUnit::TFO_BILINEAR, 0);
    sResolveProgram.bindTexture(LLShaderMgr::EXPOSURE_MAP, &sNearTarget, false, LLTexUnit::TFO_BILINEAR, 1);
    sResolveProgram.bindTexture(LLShaderMgr::DEFERRED_BRDF_LUT, &sBackgroundTarget, false, LLTexUnit::TFO_BILINEAR);
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
    sResolveProgram.uniform1i(U_POSTFILTERED, postfilter ? 1 : 0);
    sResolveProgram.uniform1f(U_VIGNETTE_SHIFT, sLensField.mVignette);
    sResolveProgram.uniform2f(U_FIELD_SCALE, sLensField.mFieldScale[0], sLensField.mFieldScale[1]);
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
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_BRDF_LUT, sBackgroundTarget.getUsage());
    sResolveProgram.unbindTexture(LLShaderMgr::EXPOSURE_MAP, sNearTarget.getUsage());
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM, sNearTarget.getUsage());
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT, sFarTarget.getUsage());
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sCoCTarget.getUsage());
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_PROJECTION, source.getUsage());
    sResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, opaque_color.getUsage());
    sResolveProgram.unbind();
    destination.flush();
    return true;
}
