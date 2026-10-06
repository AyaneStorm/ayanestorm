/**
 * @file asdepthoffield.cpp
 * @author chanayane@firestorm
 * @brief AyaneStorm depth-of-field hub: shader registration and UI callbacks
 * for the Aperture-sampled (asdofrenderer.cpp) and Live (asdoflive.cpp)
 * renderers and autofocus, the shared lens field, and the linear-HDR output
 * the Live renderer writes. The Advanced renderer (mode 1) is retired
 * (doc/ayanestorm-depth-of-field-live-plan.md).
 */
#include "llviewerprecompiledheaders.h"

#include <algorithm>

#include "asdepthoffield.h"

#include "asdofautofocus.h"
#include "asdoflive.h"
#include "asdofrenderer.h"
#include "llcontrol.h"
#include "llgl.h"
#include "llrendertarget.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

namespace
{
    LLRenderTarget sHDROutputTarget;

    using LensField = ASDepthOfField::LensField;

    // Reads the lens character settings for this frame. Focus shifts are
    // given in percent of the focal length f; on the thin lens with a fixed
    // sensor, a focal shift p f moves the in-focus inverse distance by p / f,
    // which the CoC pass turns into K p / f pixels, with K = sqrt(2) *
    // blur_constant / (magnification * tan_pixel_angle) its pixels per unit
    // of (1 / focus - 1 / distance) (liveBlurRadius(), asDoFLiveCommonF.glsl) and
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
        return field;
    }
}

void ASDepthOfField::registerUICallbacks()
{
    ASDoFRenderer::registerUICallbacks();
    ASDoFAutofocus::registerUICallbacks();
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASDepthOfField.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            static const std::vector<std::string> controls = {
                "ASDepthOfFieldMode", "ASDepthOfFieldFocusMode",
                "ASDepthOfFieldQuality", "ASDepthOfFieldNearRadius",
                "ASDepthOfFieldFarRadius", "ASDepthOfFieldApertureBlades",
                "ASDepthOfFieldApertureRoundness", "ASDepthOfFieldApertureRotation",
                "ASDepthOfFieldAnamorphicRatio",
                "ASDepthOfFieldHighlightSprites", "ASDepthOfFieldHighlightIsolation",
                "ASDepthOfFieldHighlightMaxSprites", "ASDepthOfFieldHighlightSaturation",
                "ASDepthOfFieldPhysicalBlur", "ASDepthOfFieldMaxBlur",
                "ASDepthOfFieldFieldCurvature", "ASDepthOfFieldFieldCurvatureStrength",
                "ASDepthOfFieldApertureSamples",
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
                "ASDepthOfFieldLiveExactLayers"
            };
            const std::string name = data.asString();
            if (name == "All")
            {
                // Renderer and focus mode are mode choices, not tuning
                // values.
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
    // Aperture-sampled and Live renderers and autofocus share this module's
    // registration hooks.
    ASDoFLive::registerShaders(shaders);
    ASDoFRenderer::registerShaders(shaders);
    ASDoFAutofocus::registerShaders(shaders);
}

bool ASDepthOfField::createShaders(S32 shader_level)
{
    bool success = ASDoFLive::createShaders(shader_level);
    success = ASDoFRenderer::createShaders(shader_level) && success;
    success = ASDoFAutofocus::createShaders(shader_level) && success;
    return success;
}

void ASDepthOfField::unloadShaders()
{
    ASDoFLive::unloadShaders();
    ASDoFRenderer::unloadShaders();
    ASDoFAutofocus::unloadShaders();
    releaseResources();
}

bool ASDepthOfField::usesScreenSpaceRenderer()
{
    return gSavedSettings.getS32("ASDepthOfFieldMode") == ASDoFLive::LIVE_MODE;
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
    sHDROutputTarget.release();
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

void ASDepthOfField::prepareTransparentDepthCapture(U32 width, U32 height)
{
    if (gSavedSettings.getS32("ASDepthOfFieldMode") == ASDoFLive::LIVE_MODE)
    {
        // Live DoF keeps the opaque layer for its transparency bins.
        ASDoFLive::prepareCapture(width, height);
    }
}

bool ASDepthOfField::render(LLRenderTarget& source, LLRenderTarget& destination,
                             LLRenderTarget& depth, LLVertexBuffer& screen_triangle,
                             F32 focal_distance, F32 blur_constant, F32 tan_pixel_angle,
                             F32 magnification, F32 max_coc)
{
    if (gSavedSettings.getS32("ASDepthOfFieldMode") == ASDoFLive::LIVE_MODE)
    {
        return ASDoFLive::render(source, destination, depth, screen_triangle, focal_distance,
                                 blur_constant, tan_pixel_angle, magnification, max_coc);
    }
    ASDoFLive::releaseResources();
    return false;
}
