/**
 * @file asmoonrendering.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local moon disc and halo rendering integration.
 */

#include "llviewerprecompiledheaders.h"

#include "asmoonrendering.h"

#include "asvolumetriclighting.h"
#include "llenvironment.h"
#include "llface.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "llshadermgr.h"
#include "llsky.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llvosky.h"

void ASMoonRendering::renderHalo(LLFace* halo_face, LLFace* moon_face,
                                 LLGLSLShader* shader)
{
    LLViewerTexture* mask = moon_face
                          ? moon_face->getTexture(LLRender::DIFFUSE_MAP)
                          : nullptr;
    if (!halo_face || !moon_face || !shader || !mask || !gSky.mVOSkyp ||
        !gSavedSettings.getBOOL("ASMoonHaloEnabled") ||
        !gSky.mVOSkyp->getMoon().getDraw() || !halo_face->getGeomCount())
    {
        return;
    }

    const F32 brightness = llclamp(gSavedSettings.getF32("ASMoonBrightnessMultiplier"), 0.f, 5.f);
    const F32 brightness_response = brightness / (1.f + 0.7f * brightness);
    const F32 strength = llclamp(gSavedSettings.getF32("ASMoonHaloStrength"), 0.f, 2.f)
                       * brightness_response;
    if (strength <= 0.f)
    {
        return;
    }

    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const F32 tint_height = sinf(llclamp(gSavedSettings.getF32("ASMoonHorizonTintAngle"),
                                        0.5f, 90.f) * DEG_TO_RAD);
    const F32 height_t = llclamp(sky->getMoonDirection().mV[VZ] / tint_height, 0.f, 1.f);
    const F32 horizon_amount = (1.f - height_t * height_t * (3.f - 2.f * height_t))
                             * llclamp(gSavedSettings.getF32("ASMoonHorizonTintStrength"), 0.f, 1.f);
    const LLColor4 tint = gSavedSettings.getColor4("ASMoonHorizonTint");
    LLColor3 halo_color;
    for (S32 component = 0; component < 3; ++component)
    {
        halo_color.mV[component] = lerp(1.f, tint.mV[component], horizon_amount);
    }

    F32 phase_energy = ASVolumetricLighting::getMoonPhaseIlluminatedFraction();
    constexpr F32 PHASE_COMPRESSION_START = 0.406f;
    if (phase_energy > PHASE_COMPRESSION_START)
    {
        const F32 excess = phase_energy - PHASE_COMPRESSION_START;
        phase_energy /= 1.f + 1.52f * excess * excess;
    }

    shader->bind();
    shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, mask, LLTexUnit::TT_TEXTURE);
    shader->uniform1i(LLStaticHashedString("moon_halo_pass"), 1);
    shader->uniform3fv(LLStaticHashedString("moon_halo_color"), 1, halo_color.mV);
    shader->uniform1f(LLStaticHashedString("moon_halo_strength"), strength);
    shader->uniform1f(LLStaticHashedString("moon_halo_radius"),
                      llclamp(gSavedSettings.getF32("ASMoonHaloRadius"), 1.25f, 4.f));
    shader->uniform1f(LLStaticHashedString("moon_halo_softness"),
                      llclamp(gSavedSettings.getF32("ASMoonHaloSoftness"), 0.1f, 2.f));
    shader->uniform1f(LLStaticHashedString("moon_halo_illumination"), phase_energy);
    shader->uniform1f(LLStaticHashedString("moon_phase"),
                      llclamp(gSavedSettings.getF32("ASMoonPhase"), 0.f, 1.f));
    shader->uniform1f(LLStaticHashedString("moon_phase_curvature"),
                      llclamp(gSavedSettings.getF32("ASMoonPhaseCurvature"), 0.25f, 5.f));
    shader->uniform1f(LLStaticHashedString("moon_phase_softness"),
                      llclamp(gSavedSettings.getF32("ASMoonPhaseSoftness"), 0.f, 0.30f));
    shader->uniform1f(LLStaticHashedString("moon_phase_tilt"),
                      llclamp(gSavedSettings.getF32("ASMoonPhaseTilt"), -180.f, 180.f));
    {
        LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
        // Match the proven procedural-sun halo path: the shader premultiplies
        // RGB and writes zero alpha, so pure addition avoids driver-dependent
        // source-alpha blending into macOS's RGB-only emissive attachment.
        gGL.setSceneBlendType(LLRender::BT_ADD);
        halo_face->renderIndexed();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    shader->unbind();
}

void ASMoonRendering::configureDiscShader(LLGLSLShader* shader,
                                          const LLSettingsSky* sky)
{
    if (!shader || !sky)
    {
        return;
    }

    shader->uniform1i(LLStaticHashedString("moon_halo_pass"), 0);
    shader->uniform1f(LLShaderMgr::MOON_BRIGHTNESS,
                      sky->getMoonBrightness()
                      * llclamp(gSavedSettings.getF32("ASMoonBrightnessMultiplier"), 0.f, 5.f));
    shader->uniform1f(LLShaderMgr::MOON_HORIZON_MIN_OPACITY,
                      gSavedSettings.getF32("ASMoonHorizonMinOpacity"));
    const LLColor4 tint = gSavedSettings.getColor4("ASMoonHorizonTint");
    shader->uniform3fv(LLShaderMgr::MOON_HORIZON_TINT, 1, tint.mV);
    shader->uniform1f(LLShaderMgr::MOON_HORIZON_TINT_STRENGTH,
                      gSavedSettings.getF32("ASMoonHorizonTintStrength"));
    shader->uniform1f(LLStaticHashedString("moon_horizon_tint_height"),
                      sinf(llclamp(gSavedSettings.getF32("ASMoonHorizonTintAngle"),
                                  0.5f, 90.f) * DEG_TO_RAD));
    shader->uniform1i(LLStaticHashedString("moon_render_partial"),
                      gSavedSettings.getBOOL("ASRenderPartialMoonBelowHorizon") ? 1 : 0);
    shader->uniform1f(LLStaticHashedString("moon_phase"),
                      llclamp(gSavedSettings.getF32("ASMoonPhase"), 0.f, 1.f));
    shader->uniform1f(LLStaticHashedString("moon_phase_curvature"),
                      llclamp(gSavedSettings.getF32("ASMoonPhaseCurvature"), 0.25f, 5.f));
    shader->uniform1f(LLStaticHashedString("moon_phase_softness"),
                      llclamp(gSavedSettings.getF32("ASMoonPhaseSoftness"), 0.f, 0.30f));
    shader->uniform1f(LLStaticHashedString("moon_phase_tilt"),
                      llclamp(gSavedSettings.getF32("ASMoonPhaseTilt"), -180.f, 180.f));
    shader->uniform1f(LLStaticHashedString("moon_earthshine_strength"),
                      llclamp(gSavedSettings.getF32("ASMoonEarthshineStrength"), 0.f, 0.30f));
    shader->uniform1f(LLStaticHashedString("moon_terminator_relief_strength"),
                      llclamp(gSavedSettings.getF32("ASMoonTerminatorReliefStrength"), 0.f, 2.f));
    shader->uniform1f(LLStaticHashedString("moon_terminator_relief_width"),
                      llclamp(gSavedSettings.getF32("ASMoonTerminatorReliefWidth"), 0.01f, 0.5f));
}
