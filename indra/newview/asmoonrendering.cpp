/**
 * @file asmoonrendering.cpp
 * @author chanayane@ayanestorm
 * @brief Viewer-local moon disc and halo rendering integration.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (C) 2026, Ayane Lyla
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
 *
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "asmoonrendering.h"

#include "asvolumetriclighting.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "llcontrol.h"
#include "llviewercamera.h"
#include "llviewerwindow.h"
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
#include "pipeline.h"
#include "lluictrl.h"

void ASMoonRendering::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASMoonRendering.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            static const std::vector<std::string> moon_controls = {
                "ASMoonHorizonMinOpacity", "ASMoonBrightnessMultiplier",
                "ASMoonPhase", "ASMoonPhaseCurvature", "ASMoonSizeMultiplier", "ASMoonPhaseSoftness",
                "ASMoonPhaseTilt", "ASMoonTerminatorReliefStrength",
                "ASMoonTerminatorReliefWidth", "ASMoonEarthshineStrength",
                "ASMoonHorizonTint", "ASMoonHorizonTintStrength",
                "ASMoonHorizonTintAngle", "ASMoonHaloStrength",
                "ASMoonHaloRadius", "ASMoonHaloSoftness"
            };
            const std::string control_name = data.asString();
            if (std::find(moon_controls.begin(), moon_controls.end(), control_name) != moon_controls.end())
            {
                if (LLControlVariable* control = gSavedSettings.getControl(control_name))
                {
                    control->resetToDefault(true);
                }
            }
        });

    // OpenGL builds the moon's squares with the sky's geometry, which is not rebuilt every
    // frame: a new size asks for it. The native renderer reads the setting every frame.
    if (LLControlVariable* size = gSavedSettings.getControl("ASMoonSizeMultiplier"))
    {
        size->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
        {
            if (gSky.mVOSkyp && gSky.mVOSkyp->mDrawable.notNull())
            {
                gPipeline.markRebuild(gSky.mVOSkyp->mDrawable, LLDrawable::REBUILD_VOLUME);
            }
        });
    }
}

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
    const LLColor3 halo_color = horizonTint(sky->getMoonDirection().mV[VZ]);

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
        gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);
        halo_face->renderIndexed();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    shader->unbind();
}

LLColor3 ASMoonRendering::horizonTint(F32 moon_z)
{
    const F32 tint_height = sinf(llclamp(gSavedSettings.getF32("ASMoonHorizonTintAngle"), 0.5f, 90.f) * DEG_TO_RAD);
    const F32 height_t = llclamp(llmax(moon_z, 0.f) / tint_height, 0.f, 1.f);
    const F32 amount = (1.f - height_t * height_t * (3.f - 2.f * height_t))
                     * llclamp(gSavedSettings.getF32("ASMoonHorizonTintStrength"), 0.f, 1.f);
    const LLColor4 tint = gSavedSettings.getColor4("ASMoonHorizonTint");
    LLColor3 color;
    for (S32 component = 0; component < 3; ++component)
    {
        color.mV[component] = lerp(1.f, llclamp(tint.mV[component], 0.f, 1.f), amount);
    }
    return color;
}

F32 ASMoonRendering::sizeMultiplier()
{
    static LLCachedControl<F32> multiplier(gSavedSettings, "ASMoonSizeMultiplier", 1.f);
    return llclamp((F32)multiplier, 0.1f, 10.f);
}

F32 ASMoonRendering::angularRadius(const LLSettingsSky& sky)
{
    // llvosky.cpp's moon square reaches HEAVENLY_BODY_FACTOR x MOON_DISK_RADIUS (0.45) x
    // scale of its distance from its centre.
    return atanf(HEAVENLY_BODY_FACTOR * 0.45f) * llmax(sky.getMoonScale(), 0.f) * sizeMultiplier();
}

namespace
{
    constexpr F32 MOON_FOCUS_DISTANCE = 2.f; // metres ahead: orbiting then turns the view in place
    constexpr F32 MOON_ZOOM_STEP = 1.15f;    // field of view per wheel click
    bool sMoonFocused = false;
    LLVector3d sMoonFocus;
    F32 sFovBefore = 0.f;

    // Ends the moon focus once the camera has gone elsewhere, giving back the field of view.
    void watchMoonFocus(void*)
    {
        // The target, not the focus, which animates towards it.
        if (sMoonFocused && (gAgentCamera.getFocusOnAvatar() || gAgentCamera.getFocusTargetGlobal() != sMoonFocus))
        {
            sMoonFocused = false;
            LLViewerCamera::getInstance()->setDefaultFOV(sFovBefore);
            gIdleCallbacks.deleteFunction(watchMoonFocus, nullptr);
        }
    }
}

bool ASMoonRendering::focusOnMoon(S32 x, S32 y)
{
    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    if (!sky || sky->getMoonTextureId().isNull())
    {
        return false;
    }
    LLVector3 moon = sky->getMoonDirection();
    moon.normVec();
    if (moon.mV[VZ] < -sinf(angularRadius(*sky))) // wholly below the horizon
    {
        return false;
    }
    const LLVector3 click = gViewerWindow->mouseDirectionGlobal(x, y);
    // A little past the disc, so a small moon is easy to hit.
    if (click * moon < cosf(llmin(angularRadius(*sky) * 1.25f, F_PI_BY_TWO)))
    {
        return false;
    }
    const LLVector3d camera = gAgentCamera.getCameraPositionGlobal();
    sMoonFocus = camera + LLVector3d(moon * MOON_FOCUS_DISTANCE);
    gAgentCamera.setFocusOnAvatar(false, ANIMATE);
    gAgentCamera.setCameraPosAndFocusGlobal(camera, sMoonFocus, LLUUID::null);
    if (!sMoonFocused)
    {
        sMoonFocused = true;
        sFovBefore = LLViewerCamera::getInstance()->getDefaultFOV();
        gIdleCallbacks.addFunction(watchMoonFocus, nullptr);
    }
    return true;
}

bool ASMoonRendering::handleScrollWheel(S32 clicks)
{
    watchMoonFocus(nullptr);
    if (!sMoonFocused)
    {
        return false;
    }
    // Wheel towards the screen (negative clicks) zooms in, as it moves the camera in.
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    camera->setDefaultFOV(llmin(camera->getDefaultFOV() * powf(MOON_ZOOM_STEP, (F32)clicks), sFovBefore));
    return true;
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
