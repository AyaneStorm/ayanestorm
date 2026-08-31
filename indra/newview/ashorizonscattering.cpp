/**
 * @file ashorizonscattering.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local analytic horizon-scattering sky layer.
 */

#include "llviewerprecompiledheaders.h"

#include "ashorizonscattering.h"

#include "llenvironment.h"
#include "llgl.h"
#include "llmath.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "llsky.h"
#include "lluictrl.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvowlsky.h"
#include "pipeline.h"

namespace
{
    LLGLSLShader sHorizonProgram;
    LLGLSLShader sCloudTintProgram;
    bool sShaderAvailable = false;
    bool sCloudShaderAvailable = false;

    const LLStaticHashedString sSunDirection("as_horizon_sun_direction");
    const LLStaticHashedString sSunColor("as_horizon_sun_color");
    const LLStaticHashedString sTint("as_horizon_tint");
    const LLStaticHashedString sStrength("as_horizon_strength");
    const LLStaticHashedString sOverrideOpacity("as_horizon_override_opacity");
    const LLStaticHashedString sBandHeight("as_horizon_band_height");
    const LLStaticHashedString sBandSoftness("as_horizon_band_softness");
    const LLStaticHashedString sRayleighStrength("as_horizon_rayleigh_strength");
    const LLStaticHashedString sAerosolStrength("as_horizon_aerosol_strength");
    const LLStaticHashedString sMieAnisotropy("as_horizon_mie_anisotropy");
    const LLStaticHashedString sAzimuthSpread("as_horizon_azimuth_spread");
    const LLStaticHashedString sTintMix("as_horizon_tint_mix");
    const LLStaticHashedString sSunFade("as_horizon_sun_fade");
    const LLStaticHashedString sHorizonDip("as_horizon_dip");
    const LLStaticHashedString sBlendMode("as_horizon_blend_mode");
    const LLStaticHashedString sPass("as_horizon_pass");
    const LLStaticHashedString sCamPosLocal("camPosLocal");
    const LLStaticHashedString sCloudTintColor("as_horizon_cloud_tint");
    const LLStaticHashedString sCloudTintStrength("as_horizon_cloud_strength");
    const LLStaticHashedString sSkyHazeStrength("as_horizon_sky_haze_strength");
    const LLStaticHashedString sSkyHazeIntensity("as_horizon_sky_haze_intensity");

    F32 smoothStep(F32 value)
    {
        value = llclamp(value, 0.f, 1.f);
        return value * value * (3.f - 2.f * value);
    }

    F32 getHorizonDip(const LLSettingsSky* sky)
    {
        if (!sky)
        {
            return 0.f;
        }

        // Treat the EEP dome radius as the local atmospheric curvature. As
        // the camera rises, the visible horizon drops and the sun is
        // effectively higher relative to it. Clamp the artistic correction
        // so very high-altitude cameras cannot pull the band across the sky.
        const F32 radius = llmax(sky->getDomeRadius(), 1.f);
        const F32 camera_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
        const F32 height = llmax(camera_z - LLEnvironment::instance().getWaterHeight(), 0.f);
        return llmin(acosf(radius / (radius + height)), 12.f * DEG_TO_RAD);
    }

    F32 getSunFade(const LLSettingsSky* sky, F32 horizon_dip)
    {
        if (!sky)
        {
            return 0.f;
        }

        const F32 elevation = asinf(llclamp(sky->getSunDirection().mV[VZ], -1.f, 1.f)) * RAD_TO_DEG
                            + horizon_dip * RAD_TO_DEG;
        const F32 start = llclamp(gSavedSettings.getF32("ASHorizonScatteringStartElevation"), 0.f, 45.f);
        const F32 end = llclamp(gSavedSettings.getF32("ASHorizonScatteringEndElevation"), -30.f, 0.f);
        const F32 daylight_fade = start > 0.f
                                ? 1.f - smoothStep(elevation / start)
                                : (elevation <= 0.f ? 1.f : 0.f);
        const F32 twilight_fade = end < 0.f
                                ? smoothStep((elevation - end) / -end)
                                : (elevation >= 0.f ? 1.f : 0.f);
        return daylight_fade * twilight_fade;
    }

    LLVector3 getDomeSunDirection(const LLSettingsSky* sky)
    {
        const LLVector3 direction = sky->getSunDirection();
        // The WindLight dome uses Y as up; EEP/world uses Z as up.
        return LLVector3(direction.mV[VY], direction.mV[VZ], direction.mV[VX]);
    }

    LLColor3 normalizedSunColor(const LLSettingsSky* sky)
    {
        LLColor3 color = sky->getSunlightColor();
        const F32 maximum = llmax(color.mV[0], llmax(color.mV[1], color.mV[2]));
        if (maximum > F_APPROXIMATELY_ZERO)
        {
            color *= 1.f / maximum;
        }
        else
        {
            color.set(1.f, 0.95f, 0.8f);
        }
        return color;
    }

    void uploadCommonUniforms(LLGLSLShader& shader, const LLSettingsSky* sky,
                              F32 sun_fade, F32 horizon_dip)
    {
        shader.uniform1f(sSunFade, sun_fade);
        shader.uniform1f(sHorizonDip, horizon_dip);
        shader.uniform1f(sBandHeight,
            llclamp(gSavedSettings.getF32("ASHorizonScatteringBandHeight"), 1.f, 35.f) * DEG_TO_RAD);
        shader.uniform1f(sBandSoftness,
            llclamp(gSavedSettings.getF32("ASHorizonScatteringBandSoftness"), 0.5f, 20.f) * DEG_TO_RAD);
        shader.uniform1f(sAzimuthSpread,
            llclamp(gSavedSettings.getF32("ASHorizonScatteringAzimuthSpread"), 10.f, 180.f) * DEG_TO_RAD);
        if (sky)
        {
            const LLVector3 sun_direction = getDomeSunDirection(sky);
            shader.uniform3fv(sSunDirection, 1, sun_direction.mV);
        }
    }
}

void ASHorizonScattering::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASHorizonScattering.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            static const std::vector<std::string> controls = {
                "ASHorizonScatteringBlendMode", "ASHorizonScatteringStrength",
                "ASHorizonScatteringOverrideOpacity", "ASHorizonScatteringBandHeight",
                "ASHorizonScatteringBandSoftness", "ASHorizonScatteringRayleighStrength",
                "ASHorizonScatteringAerosolStrength", "ASHorizonScatteringMieAnisotropy",
                "ASHorizonScatteringAzimuthSpread", "ASHorizonScatteringTint",
                "ASHorizonScatteringTintMix", "ASHorizonScatteringStartElevation",
                "ASHorizonScatteringEndElevation", "ASHorizonScatteringCloudStrength",
                "ASWaterHorizonFogStrength", "ASWaterHorizonFogIntensity",
                "ASHorizonScatteringSkyHazeStrength", "ASHorizonScatteringSkyHazeIntensity"
            };
            const std::string name = data.asString();
            if (std::find(controls.begin(), controls.end(), name) != controls.end())
            {
                if (LLControlVariable* control = gSavedSettings.getControl(name))
                {
                    control->resetToDefault(true);
                }
            }
        });
}

void ASHorizonScattering::registerShader(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sHorizonProgram);
    shaders.push_back(&sCloudTintProgram);
}

bool ASHorizonScattering::createShader(S32 shader_level)
{
    sShaderAvailable = false;
    sHorizonProgram.mName = "AyaneStorm Horizon Scattering Shader";
    sHorizonProgram.mShaderFiles.clear();
    sHorizonProgram.clearPermutations();
    sHorizonProgram.mFeatures.isDeferred = true;
    sHorizonProgram.mShaderFiles.emplace_back("deferred/asHorizonScatteringV.glsl", GL_VERTEX_SHADER);
    sHorizonProgram.mShaderFiles.emplace_back("deferred/asHorizonScatteringF.glsl", GL_FRAGMENT_SHADER);
    sHorizonProgram.mShaderLevel = shader_level;
    sHorizonProgram.mShaderGroup = LLGLSLShader::SG_SKY;
    if (gSavedSettings.getBOOL("RenderEnableEmissiveBuffer"))
    {
        sHorizonProgram.addPermutation("HAS_EMISSIVE", "1");
    }
    sShaderAvailable = sHorizonProgram.createShader();
    if (!sShaderAvailable)
    {
        // This is an optional layer. Remove any partially-created GL state so
        // a rejected shader can never affect the established EEP sky path.
        LL_WARNS("ASHorizonScattering")
            << "Horizon scattering shader failed to load; leaving EEP sky unchanged."
            << LL_ENDL;
        sHorizonProgram.unload();
    }

    sCloudShaderAvailable = false;
    if (sShaderAvailable)
    {
        sCloudTintProgram.mName = "AyaneStorm Horizon Cloud Tint Shader";
        sCloudTintProgram.mShaderFiles.clear();
        sCloudTintProgram.clearPermutations();
        sCloudTintProgram.mFeatures.calculatesAtmospherics = true;
        sCloudTintProgram.mFeatures.hasAtmospherics = true;
        sCloudTintProgram.mFeatures.hasGamma = true;
        sCloudTintProgram.mFeatures.hasSrgb = true;
        sCloudTintProgram.mFeatures.isDeferred = true;
        sCloudTintProgram.mShaderFiles.emplace_back("deferred/asHorizonCloudTintV.glsl", GL_VERTEX_SHADER);
        sCloudTintProgram.mShaderFiles.emplace_back("deferred/asHorizonCloudTintF.glsl", GL_FRAGMENT_SHADER);
        sCloudTintProgram.mShaderLevel = shader_level;
        sCloudTintProgram.mShaderGroup = LLGLSLShader::SG_SKY;
        if (gSavedSettings.getBOOL("RenderEnableEmissiveBuffer"))
        {
            sCloudTintProgram.addPermutation("HAS_EMISSIVE", "1");
        }
        sCloudShaderAvailable = sCloudTintProgram.createShader();
        if (!sCloudShaderAvailable)
        {
            LL_WARNS("ASHorizonScattering")
                << "Cloud tint shader failed to load; leaving EEP clouds unchanged."
                << LL_ENDL;
            sCloudTintProgram.unload();
        }
    }
    return sShaderAvailable;
}

void ASHorizonScattering::unloadShader()
{
    sShaderAvailable = false;
    sCloudShaderAvailable = false;
    sHorizonProgram.unload();
    sCloudTintProgram.unload();
}

namespace
{
bool configureShader()
{
    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    if (!sky || !gSavedSettings.getBOOL("ASHorizonScatteringEnabled") ||
        !sShaderAvailable || !sHorizonProgram.isComplete())
    {
        return false;
    }

    const F32 horizon_dip = getHorizonDip(sky.get());
    const F32 sun_fade = getSunFade(sky.get(), horizon_dip);
    const F32 strength = llclamp(gSavedSettings.getF32("ASHorizonScatteringStrength"), 0.f, 4.f);
    if (sun_fade * strength <= 0.0001f)
    {
        return false;
    }

    const LLColor3 sun_color = normalizedSunColor(sky.get());
    const LLColor4 tint = gSavedSettings.getColor4("ASHorizonScatteringTint");

    sHorizonProgram.bind();
    uploadCommonUniforms(sHorizonProgram, sky.get(), sun_fade, horizon_dip);
    sHorizonProgram.uniform3fv(sSunColor, 1, sun_color.mV);
    sHorizonProgram.uniform3fv(sTint, 1, tint.mV);
    sHorizonProgram.uniform1f(sStrength, strength);
    sHorizonProgram.uniform1f(sOverrideOpacity,
        llclamp(gSavedSettings.getF32("ASHorizonScatteringOverrideOpacity"), 0.f, 1.f));
    sHorizonProgram.uniform1f(sRayleighStrength,
        llclamp(gSavedSettings.getF32("ASHorizonScatteringRayleighStrength"), 0.f, 4.f));
    sHorizonProgram.uniform1f(sAerosolStrength,
        llclamp(gSavedSettings.getF32("ASHorizonScatteringAerosolStrength"), 0.f, 4.f));
    sHorizonProgram.uniform1f(sMieAnisotropy,
        llclamp(gSavedSettings.getF32("ASHorizonScatteringMieAnisotropy"), 0.f, 0.95f));
    sHorizonProgram.uniform1f(sTintMix,
        llclamp(gSavedSettings.getF32("ASHorizonScatteringTintMix"), 0.f, 1.f));
    sHorizonProgram.uniform1i(sBlendMode, (S32)ASHorizonScattering::getBlendMode());
    sHorizonProgram.uniform1f(sSkyHazeStrength,
        llclamp(gSavedSettings.getF32("ASHorizonScatteringSkyHazeStrength"), 0.f, 2.f));
    sHorizonProgram.uniform1f(sSkyHazeIntensity,
        llclamp(gSavedSettings.getF32("ASHorizonScatteringSkyHazeIntensity"), 0.f, 1.f));
    return true;
}
}

ASHorizonScattering::BlendMode ASHorizonScattering::getBlendMode()
{
    const S32 mode = gSavedSettings.getS32("ASHorizonScatteringBlendMode");
    if (mode == BLEND_ATMOSPHERIC)
    {
        return BLEND_ATMOSPHERIC;
    }
    if (mode == BLEND_REPLACE)
    {
        return BLEND_REPLACE;
    }
    return BLEND_ADDITIVE;
}

void ASHorizonScattering::render(bool hdri_sky_active)
{
    if (hdri_sky_active || !gSky.mVOWLSkyp || !configureShader())
    {
        return;
    }

    LLGLSPipelineBlendSkyBox horizon_state(false, false);

    const LLVector3& origin = LLViewerCamera::getInstance()->getOrigin();
    const F32 cam_height = LLEnvironment::instance().getCamHeight();

    // Reproduce the established WindLight dome transform inside this AS-owned
    // module so the upstream draw pool needs only one insertion-point call.
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    if (LLPipeline::sReflectionRender && origin.mV[VZ] > 256.f)
    {
        gGL.translatef(origin.mV[VX], origin.mV[VY], 256.f - origin.mV[VZ] * 0.5f);
    }
    else
    {
        gGL.translatef(origin.mV[VX], origin.mV[VY], origin.mV[VZ]);
    }
    gGL.rotatef(120.f, 1.f / F_SQRT3, 1.f / F_SQRT3, 1.f / F_SQRT3);
    gGL.scalef(0.333f, 0.333f, 0.333f);
    gGL.translatef(0.f, -cam_height, 0.f);
    sHorizonProgram.uniform3f(sCamPosLocal, 0.f, cam_height, 0.f);

    // <AS:Chanayane> Sky horizon haze needs a genuine multiplicative
    // darkening pass to actually dim the sky near the horizon -- additive
    // mode has no such pass by design (it can only ADD light on top of
    // EEP), so with the extinction draw gated to Atmospheric-only, sky
    // horizon haze silently had no visible effect at all in Additive mode
    // regardless of its own strength slider (confirmed against the user's
    // actual saved ASHorizonScatteringBlendMode = 0/Additive). Per explicit
    // user request ("find a way to make it work like a subtractive pass"
    // rather than switch away from the additive look they prefer), the
    // SAME extinction draw used by Atmospheric mode is now also issued in
    // Additive mode whenever sky horizon haze is active, layered alongside
    // (not replacing) additive mode's existing radiance-only glow pass
    // below. This makes Additive mode's horizon band capable of local
    // darkening for the first time -- an intentional, labeled exception to
    // "additive can only brighten" (see the Sky horizon haze slider's
    // tooltip and doc), scoped tightly to this one sub-feature.
    const bool sky_haze_active =
        llclamp(gSavedSettings.getF32("ASHorizonScatteringSkyHazeStrength"), 0.f, 2.f) > 0.0001f;
    if (getBlendMode() == BLEND_ATMOSPHERIC ||
        (getBlendMode() == BLEND_ADDITIVE && sky_haze_active))
    {
        // Beer-Lambert extinction: destination RGB *= shader transmittance.
        // Preserve destination alpha, which carries the glow mask.
        sHorizonProgram.uniform1i(sPass, 0);
        gGL.blendFunc(LLRender::BF_ZERO, LLRender::BF_SOURCE_COLOR,
                      LLRender::BF_ZERO, LLRender::BF_ONE);
        gSky.mVOWLSkyp->drawDome();
    }
    // </AS:Chanayane>

    // In-scattering is emitted radiance. Atmospheric and additive modes add
    // it; replace mode alpha-blends the generated band over EEP.
    sHorizonProgram.uniform1i(sPass, 1);
    if (getBlendMode() == BLEND_REPLACE)
    {
        gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                      LLRender::BF_ZERO, LLRender::BF_ONE);
    }
    else
    {
        gGL.setSceneBlendType(LLRender::BT_ADD);
    }
    gSky.mVOWLSkyp->drawDome();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();

    sHorizonProgram.unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

LLGLSLShader* ASHorizonScattering::getCloudShader(bool hdri_sky_active)
{
    if (hdri_sky_active || !gSavedSettings.getBOOL("ASHorizonScatteringEnabled") ||
        !sCloudShaderAvailable || !sCloudTintProgram.isComplete())
    {
        return nullptr;
    }

    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    if (!sky)
    {
        return nullptr;
    }

    const F32 sun_fade = getSunFade(sky.get(), getHorizonDip(sky.get()));
    F32 strength = llclamp(gSavedSettings.getF32("ASHorizonScatteringStrength"), 0.f, 4.f)
                 * sun_fade;
    if (getBlendMode() != BLEND_ADDITIVE)
    {
        strength *= llclamp(gSavedSettings.getF32("ASHorizonScatteringOverrideOpacity"), 0.f, 1.f);
    }
    // Independent artistic multiplier for cloud tinting only; defaults to 1.0
    // so it reproduces the prior fixed cloud strength exactly.
    strength *= llclamp(gSavedSettings.getF32("ASHorizonScatteringCloudStrength"), 0.f, 2.f);
    if (strength <= 0.0001f)
    {
        return nullptr;
    }

    const LLColor3 sun_color = normalizedSunColor(sky.get());
    const LLColor4 tint_setting = gSavedSettings.getColor4("ASHorizonScatteringTint");
    const F32 tint_mix = llclamp(gSavedSettings.getF32("ASHorizonScatteringTintMix"), 0.f, 1.f);
    const F32 cloud_tint_mix = 0.5f + 0.5f * tint_mix;
    LLColor3 cloud_tint;
    for (S32 channel = 0; channel < 3; ++channel)
    {
        cloud_tint.mV[channel] = sun_color.mV[channel]
                                * lerp(1.f, tint_setting.mV[channel], cloud_tint_mix);
    }

    // Upload once here; the established cloud renderer binds this program
    // again and supplies all ordinary EEP cloud textures and uniforms.
    sCloudTintProgram.bind();
    sCloudTintProgram.uniform3fv(sCloudTintColor, 1, cloud_tint.mV);
    sCloudTintProgram.uniform1f(sCloudTintStrength, llmin(strength, 2.f));
    sCloudTintProgram.unbind();
    return &sCloudTintProgram;
}
