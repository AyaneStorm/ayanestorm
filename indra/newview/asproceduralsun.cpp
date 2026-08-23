/**
 * @file asproceduralsun.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local procedural sunset sun-disc controls.
 */

#include "llviewerprecompiledheaders.h"

#include "asproceduralsun.h"

#include "ascelestialtwilight.h"
#include "llappviewer.h"
#include "llface.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llmath.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "llsky.h"
#include "llviewercontrol.h"
#include "llvosky.h"

namespace
{
    constexpr F32 PEAK_ELEVATION_DEGREES = 2.f;

    F32 smoothStep(F32 value)
    {
        value = llclamp(value, 0.f, 1.f);
        return value * value * (3.f - 2.f * value);
    }
}

ASProceduralSun::RenderParams ASProceduralSun::getRenderParams(const LLSettingsSky* sky)
{
    RenderParams params;
    if (!sky || !gSavedSettings.getBOOL("ASProceduralSunEnabled"))
    {
        return params;
    }

    const F32 brightness = llclamp(gSavedSettings.getF32("ASProceduralSunBrightness"), 0.f, 5.f);
    if (brightness <= 0.f)
    {
        return params;
    }

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
        visibility = peak > cutoff
                   ? smoothStep((elevation - cutoff) / (peak - cutoff))
                   : 0.f;
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
    params.opacity = visibility * llmin(brightness, 1.f);
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
    // A real low sun remains white-hot across most of its apparent surface.
    // Preserve only a little EEP chroma in the core and move the configured
    // sunset color toward the limb/feather where atmospheric attenuation is
    // visually strongest. Broad red emission remains the EEP haze's job.
    const LLColor3 hot_core = 0.75f * LLColor3::white + 0.25f * start_color;
    params.color = ((1.f - 0.12f * color_mix) * hot_core
                  + (0.12f * color_mix) * final_color) * brightness;
    params.limb_color = ((1.f - color_mix) * hot_core
                       + color_mix * final_color) * brightness;
    return params;
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
                      llclamp(gSavedSettings.getF32("ASProceduralSunHaloRadius"), 1.1f, 4.f));
    shader->uniform1f(LLStaticHashedString("procedural_sun_halo_opacity"),
                      params.halo_factor * strength);
    shader->uniform3fv(LLStaticHashedString("procedural_sun_limb_color"), 1,
                       params.limb_color.mV);
    {
        LLGLDepthTest halo_depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
        gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);
        face->renderIndexed();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }
    shader->unbind();
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
