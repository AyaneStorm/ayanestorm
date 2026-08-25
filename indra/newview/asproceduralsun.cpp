/**
 * @file asproceduralsun.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local procedural sunset sun-disc controls.
 */

#include "llviewerprecompiledheaders.h"

#include "asproceduralsun.h"

#include "ascelestialtwilight.h"
#include "llcontrol.h"
#include "llappviewer.h"
#include "llenvironment.h"
#include "llface.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llmath.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "llsky.h"
#include "llviewercontrol.h"
#include "llvosky.h"
#include "lluictrl.h"

namespace
{
    constexpr F32 PEAK_ELEVATION_DEGREES = 2.f;

    F32 smoothStep(F32 value)
    {
        value = llclamp(value, 0.f, 1.f);
        return value * value * (3.f - 2.f * value);
    }

    LLColor3 mixSunsetColor(const LLColor3& hot, const LLColor3& final_color,
                            F32 warmth)
    {
        warmth = llclamp(warmth, 0.f, 1.f);
        LLColor3 color;
        color.mV[0] = lerp(hot.mV[0], final_color.mV[0], warmth);
        color.mV[1] = lerp(hot.mV[1], final_color.mV[1], warmth);
        // A straight white-to-red RGB blend retains blue too long and passes
        // through pink. Remove blue earlier while green falls normally to
        // create the yellow/orange temperatures seen between white and red.
        const F32 blue_warmth = powf(warmth, 0.25f);
        color.mV[2] = lerp(hot.mV[2], final_color.mV[2], blue_warmth);
        return color;
    }
}

void ASProceduralSun::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASProceduralSun.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            static const std::vector<std::string> sun_controls = {
                "ASProceduralSunStartAngle", "ASProceduralSunFinalColor",
                "ASProceduralSunBrightness", "ASProceduralSunFeather",
                "ASProceduralSunShimmer", "ASProceduralSunHaloStrength",
                "ASProceduralSunHaloRadius", "ASProceduralSunHaloSoftness"
            };
            const std::string control_name = data.asString();
            if (std::find(sun_controls.begin(), sun_controls.end(), control_name) != sun_controls.end())
            {
                if (LLControlVariable* control = gSavedSettings.getControl(control_name))
                {
                    control->resetToDefault(true);
                }
            }
        });
}

ASProceduralSun::RenderParams ASProceduralSun::getRenderParams(const LLSettingsSky* sky)
{
    RenderParams params;
    if (!sky || !gSavedSettings.getBOOL("ASProceduralSunEnabled"))
    {
        return params;
    }

    const F32 brightness = llclamp(gSavedSettings.getF32("ASProceduralSunBrightness"), 0.f, 5.f);

    const F32 elevation = asinf(llclamp(sky->getSunDirection().mV[VZ], -1.f, 1.f));
    const F32 start = llclamp(gSavedSettings.getF32("ASProceduralSunStartAngle"), 0.f, 45.f) * DEG_TO_RAD;
    const F32 peak = llmin(PEAK_ELEVATION_DEGREES * DEG_TO_RAD, start);
    const F32 cutoff = ASCelestialTwilight::discEdgeElevation(
        sky->getSunScale(), ASCelestialTwilight::SUN_DISC_RADIUS);

    if (elevation >= start || elevation <= cutoff)
    {
        return params;
    }

    F32 visibility;
    if (elevation >= peak && start > peak)
    {
        visibility = smoothStep((start - elevation) / (start - peak));
    }
    else
    {
        // Keep the descending disc bright through most of its horizon
        // crossing, then smoothly extinguish only its final sliver. A regular
        // smoothstep dims too early and makes the upper edge disappear before
        // the scale-aware billboard cutoff is reached.
        const F32 descent = peak > cutoff
                          ? llclamp((elevation - cutoff) / (peak - cutoff), 0.f, 1.f)
                          : 0.f;
        visibility = smoothStep(llmin(descent / 0.28f, 1.f))
                   * (0.72f + 0.28f * smoothStep(descent));
    }

    LLColor3 start_color = sky->getSunlightColor();
    const F32 max_component = llmax(start_color.mV[0], llmax(start_color.mV[1], start_color.mV[2]));
    if (max_component > 0.f)
    {
        start_color *= 1.f / max_component;
    }
    else
    {
        start_color.set(1.f, 0.95f, 0.8f);
    }

    const LLColor4 final_color_setting = gSavedSettings.getColor4("ASProceduralSunFinalColor");
    const LLColor3 final_color(final_color_setting.mV);
    const F32 color_mix = smoothStep((start - elevation) / llmax(start - cutoff, F_APPROXIMATELY_ZERO));

    params.enabled = true;
    // Opacity follows elevation only. The brightness control below changes
    // chroma up to one, then emitted intensity above one.
    params.opacity = visibility;
    // Activate refraction when the scale-aware lower limb approaches the
    // horizon, not only when the disc center does. Large EEP suns can begin
    // crossing while their centers are still several degrees high.
    const F32 disc_half_angle = -cutoff;
    const F32 lower_limb_elevation = elevation - disc_half_angle;
    params.horizon_factor = 1.f - smoothStep(llmax(lower_limb_elevation, 0.f)
                                             / (5.f * DEG_TO_RAD));
    // The EEP atmosphere commonly supplies enough glow above two degrees.
    // Add the procedural halo only through the final horizon approach.
    params.halo_factor = (1.f - smoothStep(llmax(elevation, 0.f)
                                           / (2.f * DEG_TO_RAD))) * visibility;
    // A real low sun remains very bright across most of its apparent surface,
    // but is no longer neutral white at the horizon. Preserve EEP chroma and
    // add a restrained, spatially uniform warm tint as atmospheric attenuation
    // increases. Broad red emission remains the separate halo's job.
    const LLColor3 hot_core = 0.75f * LLColor3::white + 0.25f * start_color;
    const F32 brightness_mix = llclamp(brightness, 0.f, 1.f);
    const F32 disc_color_mix = lerp(1.f, 0.24f * color_mix, brightness_mix);
    const F32 intensity = llmax(brightness, 1.f);
    params.color = mixSunsetColor(hot_core, final_color, disc_color_mix) * intensity;
    params.limb_color = ((1.f - color_mix) * hot_core
                       + color_mix * final_color);
    return params;
}

ASProceduralSun::WaterLightState
ASProceduralSun::getWaterLightState(const LLEnvironment& environment,
                                    const LLSettingsSky* sky)
{
    // Match water's celestial source to the actual procedural-disc lifetime;
    // the original center-based test switches to moonlight too early while a
    // scale-aware part of the sunset sun remains visible.
    WaterLightState state;
    const bool procedural_sun_visible = getRenderParams(sky).enabled;
    state.direction = procedural_sun_visible && sky
                    ? sky->getSunDirection()
                    : environment.getLightDirection();
    state.sun_up = environment.getIsSunUp() || procedural_sun_visible;
    return state;
}

void ASProceduralSun::renderHalo(LLFace* face, LLGLSLShader* shader,
                                 const RenderParams& params)
{
    const F32 strength = llclamp(gSavedSettings.getF32("ASProceduralSunHaloStrength"), 0.f, 2.f);
    if (!face || !shader || !params.enabled || params.halo_factor <= 0.f || strength <= 0.f ||
        !gSky.mVOSkyp)
    {
        return;
    }

    LLVector3 center;
    for (S32 corner = 0; corner < 4; ++corner)
    {
        center += gSky.mVOSkyp->getSun().corner(corner);
    }
    center = center * 0.25f + gSky.mVOSkyp->getCameraPosAgent();

    shader->bind();
    shader->uniform1i(LLStaticHashedString("procedural_sun_alignment_enabled"), 1);
    shader->uniform1i(LLStaticHashedString("procedural_sun_halo_pass"), 1);
    shader->uniform3fv(LLStaticHashedString("procedural_sun_center"), 1, center.mV);
    shader->uniform1f(LLStaticHashedString("procedural_sun_halo_radius"),
                      llclamp(gSavedSettings.getF32("ASProceduralSunHaloRadius"), 1.1f, 8.f));
    shader->uniform1f(LLStaticHashedString("procedural_sun_halo_opacity"),
                      params.halo_factor * strength);
    shader->uniform1f(LLStaticHashedString("procedural_sun_halo_softness"),
                      llclamp(gSavedSettings.getF32("ASProceduralSunHaloSoftness"), 0.1f, 1.f));
    shader->uniform3fv(LLStaticHashedString("procedural_sun_limb_color"), 1,
                       params.limb_color.mV);
    {
        LLGLDepthTest halo_depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
        // The shader premultiplies halo RGB and writes zero glow-mask alpha.
        // Pure additive blending preserves that zero alpha while compositing
        // the visible halo, preventing a second post-process bloom halo.
        gGL.setSceneBlendType(LLRender::BT_ADD);
        face->renderIndexed();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }
    shader->unbind();
}

void ASProceduralSun::renderDisc(LLFace* face, const RenderParams& params)
{
    if (!face)
    {
        return;
    }

    if (params.enabled)
    {
        // Blend visible disc RGB normally but preserve the destination alpha.
        // Scene alpha is the post-process glow mask; allowing the procedural
        // disc to write it creates a second automatic halo around the explicit
        // configurable halo.
        gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                      LLRender::BF_ZERO, LLRender::BF_ONE);
        face->renderIndexed();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        return;
    }

    face->renderIndexed();
}

void ASProceduralSun::configureDiscShader(LLGLSLShader* shader,
                                          const RenderParams& params,
                                          bool texture_available)
{
    if (!shader)
    {
        return;
    }

    shader->uniform1i(LLStaticHashedString("procedural_sun_halo_pass"), 0);
    shader->uniform1i(LLStaticHashedString("procedural_sun_alignment_enabled"),
                      gSavedSettings.getBOOL("ASProceduralSunEnabled") ? 1 : 0);
    shader->uniform1i(LLStaticHashedString("sun_texture_available"),
                      texture_available ? 1 : 0);
    shader->uniform1i(LLStaticHashedString("procedural_sun_enabled"),
                      params.enabled ? 1 : 0);
    shader->uniform1f(LLStaticHashedString("procedural_sun_opacity"), params.opacity);
    shader->uniform1f(LLStaticHashedString("procedural_sun_feather"),
                      llclamp(gSavedSettings.getF32("ASProceduralSunFeather"), 0.f, 0.35f));
    shader->uniform1f(LLStaticHashedString("procedural_sun_shimmer"),
                      llclamp(gSavedSettings.getF32("ASProceduralSunShimmer"), 0.f, 0.10f));
    shader->uniform1f(LLStaticHashedString("procedural_sun_horizon_factor"),
                      params.horizon_factor);
    shader->uniform1f(LLStaticHashedString("procedural_sun_time"), gFrameTimeSeconds);
    shader->uniform3fv(LLStaticHashedString("procedural_sun_color"), 1, params.color.mV);
    shader->uniform3fv(LLStaticHashedString("procedural_sun_limb_color"), 1,
                       params.limb_color.mV);
}
