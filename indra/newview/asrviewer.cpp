/**
 * @file asrviewer.cpp
 * @brief See asrviewer.h.
 * @author chanayane@ayanestorm
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

#include "asrviewer.h"

#include "asrbridge.h"
#include "asrcameraturn.h"
#include "asrgltable.h"
#include "asrtexturecache.h"
#include "asrworld.h"
#include "asdofrenderer.h"
#include "asmilkyway.h"
#include "asmoonrendering.h"
#include "asstars.h"
#include "asworldclouds.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llsky.h"
#include "llvosky.h"
#include "llapp.h"
#include "llappviewer.h"
#include "llenvironment.h"
#include "llsettingssky.h"
#include "llstartup.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llcoord.h"
#include "lldir.h"
#include "llgl.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "pipeline.h"

// The Linux window is still to do, so that build stays on OpenGL.
#if defined(AS_RENDERER) && (LL_WINDOWS || LL_DARWIN)
#define ASR_VIEWER_SUPPORTED 1
#include "asrdevice.h"
#include "asrrenderthread.h"
#include "asrtexturestreaming.h"
#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <OpenGL/OpenGL.h>
#endif
#else
#define ASR_VIEWER_SUPPORTED 0
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace
{

constexpr char BACKEND_SETTING[] = "ASRendererBackend";
constexpr char BACKEND_OPENGL[] = "OpenGL";
constexpr char BACKEND_NATIVE[] = "Native";

bool sFellBack = false;

#if ASR_VIEWER_SUPPORTED

using Clock = std::chrono::steady_clock;

// The one gate for the viewer's world state in the per-frame code: the environment, the
// agent and its regions, the selection, and any other singleton. Frames are published from
// the window's creation to its end, so also before login and during shutdown, when those
// do not exist yet or any more, and calling instance() then builds one out of order: that
// is how these crashes happen. Every read of them from here goes inside this gate, rather
// than a check of its own at each place.
bool worldReady()
{
    return LLStartUp::getStartupState() >= STATE_STARTED && !LLApp::isExiting();
}

// One EEP density profile (rayleigh_config, mie_config, absorption_config) in the
// renderer's form. EEP keeps heights in metres and the renderer in kilometres, so widths
// shrink by 1000 and the per-metre terms grow by 1000. A single EEP layer, 'the entire
// atmosphere', is Bruneton's second layer, after a first of width 0.
asr::AtmosphereProfile toProfile(const LLSD& layers)
{
    asr::AtmosphereProfile profile;
    if (!layers.isArray() || layers.size() == 0)
    {
        return profile;
    }
    const size_t first = layers.size() == 1 ? 1 : 0;
    for (size_t i = 0; i < std::min<size_t>(layers.size(), 2); ++i)
    {
        const LLSD& layer = layers[i];
        asr::AtmosphereLayer& out = profile.layers[first + i];
        out.width = F32(layer[LLSettingsSky::SETTING_DENSITY_PROFILE_WIDTH].asReal()) * 0.001f;
        out.expTerm = F32(layer[LLSettingsSky::SETTING_DENSITY_PROFILE_EXP_TERM].asReal());
        out.expScale = F32(layer[LLSettingsSky::SETTING_DENSITY_PROFILE_EXP_SCALE_FACTOR].asReal()) * 1000.f;
        out.linearTerm = F32(layer[LLSettingsSky::SETTING_DENSITY_PROFILE_LINEAR_TERM].asReal()) * 1000.f;
        out.constantTerm = F32(layer[LLSettingsSky::SETTING_DENSITY_PROFILE_CONSTANT_TERM].asReal());
    }
    if (first == 1)
    {
        profile.layers[0].width = 0.f;
    }
    return profile;
}

// Where a texture is in the renderer, as SkyInput takes it; NO_SKY_TEXTURE until it has an image.
U32 skyTextureBits(LLViewerTexture* image, asr::UISampler sampler)
{
    const U32 slot = image && image->getTexName() ? ASR::textureSlot(image->getTexName()) : 0;
    return slot ? asr::uiTextureBits(slot, sampler) : asr::NO_SKY_TEXTURE;
}

// The moon from EEP and the AyaneStorm moon settings (ASMoon*). The default moon is the lit
// sphere with the NASA maps; a custom moon stays a flat image, as OpenGL draws it.
void fillMoon(asr::SkyInput& sky, const LLSettingsSky& current)
{
    // The size is OpenGL's, which EEP skies were made against (ASMoonRendering::angularRadius(),
    // about 2.6 degrees at scale 1, ten times the real moon). Its stretch near the horizon is
    // left out: it would make the sphere oval.
    // The whole moon's light on the sky against EEP's moonlight colour: SL nights have a moon
    // far brighter than the real one. Tuned at the side-by-side with OpenGL.
    constexpr F32 MOON_SKY_SHARE = 3e-3f;

    const LLVector3 direction = current.getMoonDirection();
    LLVector3 up = direction.mV[VZ] > 0.99f ? LLVector3::z_axis * current.getMoonRotation() : LLVector3::z_axis; // as llvosky.cpp
    if (ASStars::realSkyActive())
    {
        // The real sky: the moon's north towards the celestial pole, as the stars are turned
        // for ASStarsLatitude and the sidereal time (fillStars() ran first). Upside down
        // south of the equator, tipped near it and turning through the night. The phase
        // tilt then turns the terminator against the moon's own face.
        const LLVector3 pole(sky.starRotation[6], sky.starRotation[7], sky.starRotation[8]);
        if (fabsf(pole * direction) < 0.99f)
        {
            up = pole;
        }
    }
    for (S32 i = 0; i < 3; ++i)
    {
        sky.moonDirection[i] = direction.mV[i];
        sky.moonUp[i] = up.mV[i];
    }
    const LLColor3 moonlight = current.getMoonlightColor();
    for (S32 i = 0; i < 3; ++i)
    {
        sky.moonIlluminance[i] = moonlight.mV[i] * current.getMoonBrightness() * MOON_SKY_SHARE;
    }
    sky.moonBrightness = current.getMoonBrightness() * llclamp(gSavedSettings.getF32("ASMoonBrightnessMultiplier"), 0.f, 5.f);
    sky.moonAngularRadius = ASMoonRendering::angularRadius(current);
    const LLColor3 tint = ASMoonRendering::horizonTint(direction.mV[VZ]);
    for (S32 i = 0; i < 3; ++i)
    {
        sky.moonTint[i] = tint.mV[i];
    }

    const LLUUID texture = current.getMoonTextureId();
    if (texture.isNull())
    {
        sky.moonAngularRadius = 0.f; // no image: OpenGL draws no moon, its light stays
        return;
    }
    sky.moonSphere = texture == LLSettingsSky::GetDefaultMoonTextureId();
    if (sky.moonSphere)
    {
        static LLPointer<LLViewerFetchedTexture> albedo, normals;
        if (albedo.isNull())
        {
            albedo = LLViewerTextureManager::getFetchedTextureFromFile("as/as_moon_albedo.png", FTT_LOCAL_FILE, true, LLGLTexture::BOOST_UI);
            normals = LLViewerTextureManager::getFetchedTextureFromFile("as/as_moon_normal.png", FTT_LOCAL_FILE, true, LLGLTexture::BOOST_UI);
            albedo->setNoDelete();
            normals->setNoDelete();
        }
        sky.moonTexture = skyTextureBits(albedo, asr::UISampler::LinearWrap);
        sky.moonNormalTexture = skyTextureBits(normals, asr::UISampler::LinearWrap);
        sky.moonHeightRange = 19.75f; // as_moon_normal.png's alpha: -9 to 10.75 km
    }
    else
    {
        sky.moonTexture = gSky.mVOSkyp ? skyTextureBits(gSky.mVOSkyp->getMoonTex(), asr::UISampler::LinearClamp) : asr::NO_SKY_TEXTURE;
    }

    sky.moonPhase = llclamp(gSavedSettings.getF32("ASMoonPhase"), 0.f, 1.f);
    sky.moonPhaseTilt = llclamp(gSavedSettings.getF32("ASMoonPhaseTilt"), -180.f, 180.f) * DEG_TO_RAD;
    sky.moonTerminatorSoftness = llclamp(gSavedSettings.getF32("ASMoonPhaseSoftness"), 0.f, 0.30f);
    sky.moonEarthshine = llclamp(gSavedSettings.getF32("ASMoonEarthshineStrength"), 0.f, 0.30f);
    sky.moonRelief = llclamp(gSavedSettings.getF32("ASMoonTerminatorReliefStrength"), 0.f, 2.f);
    sky.moonHaloStrength = gSavedSettings.getBOOL("ASMoonHaloEnabled") ? llclamp(gSavedSettings.getF32("ASMoonHaloStrength"), 0.f, 2.f) : 0.f;
    // Up to 12: OpenGL's halo square stops at 4 (asmoonrendering.cpp clamps there), the native one does not.
    sky.moonHaloRadius = llclamp(gSavedSettings.getF32("ASMoonHaloRadius"), 1.25f, 12.f);
    sky.moonHaloSoftness = llclamp(gSavedSettings.getF32("ASMoonHaloSoftness"), 0.1f, 2.f);
}

// The stars (ASStars) and the Milky Way (ASMilkyWay), as OpenGL draws them: the list is
// rebuilt only when ASStars makes a new catalogue.
void fillStars(asr::SkyInput& sky, const LLSettingsSky& current)
{
    static std::shared_ptr<const std::vector<asr::SkyStar>> sStars;
    static U32 sStarsGeneration = 0;
    const LLVector3* directions = nullptr;
    const LLColor4* colors = nullptr;
    F32 radius = 1.f;
    const U32 generation = ASStars::catalogue(directions, colors, radius);
    if (!sStars || generation != sStarsGeneration)
    {
        auto stars = std::make_shared<std::vector<asr::SkyStar>>(ASStars::starCount());
        for (U32 i = 0; i < stars->size(); ++i)
        {
            asr::SkyStar& star = (*stars)[i];
            for (S32 c = 0; c < 3; ++c)
            {
                star.direction[c] = directions[i].mV[c];
                star.color[c] = colors[i].mV[c];
            }
            star.size = ASStars::starSize(i) / radius;
            star.brightness = ASStars::starBrightness(i);
        }
        sStars = std::move(stars);
        sStarsGeneration = generation;
    }
    sky.stars = sStars;

    // starsF.glsl's chain: EEP star brightness through smoothstep(0, 0.9), x32, then
    // ASStarsBrightness. OpenGL draws nothing below 0.001.
    const F32 alpha = current.getStarBrightness() / 500.f;
    const F32 t = llclamp(alpha / 0.9f, 0.f, 1.f);
    const F32 eepStars = alpha < 0.001f ? 0.f : t * t * (3.f - 2.f * t) * 32.f * ASStars::brightness();
    // Our sky shows the real night whatever the EEP says (user): many EEPs keep stars off or
    // drowned all night. The stars come out as the sun goes down, the first at the end of civil
    // twilight (-6 degrees), all by nautical twilight (-12), at the EEP chain's full level. An
    // EEP asking for more, as for stars by day, keeps its own. The Firestorm sky option (to
    // come) follows the EEP alone, as OpenGL.
    const F32 sunElevation = asinf(llclamp(current.getSunDirection().mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
    const F32 night = llclamp((-6.f - sunElevation) / 6.f, 0.f, 1.f);
    sky.starBrightness = llmax(eepStars, night * night * (3.f - 2.f * night) * 32.f * ASStars::brightness());
    sky.starTwinkle = ASStars::twinkleAmount();
    // starsV.glsl: the dome position times mod(time, 1.25), time being seconds / 2.
    sky.starTwinklePhase = radius * F32(fmod(LLFrameTimer::getElapsedSeconds() * 0.5, 1.25));
    ASStars::starFrame(ASDoFRenderer::starRotationTime(gFrameTimeSeconds), sky.starRotation);
    sky.starTexture = gSky.mVOSkyp ? skyTextureBits(gSky.mVOSkyp->getBloomTex(), asr::UISampler::LinearClamp) : asr::NO_SKY_TEXTURE;

    F32 band = 0.f, deepSky = 0.f, saturation = 1.f;
    if (ASMilkyWay::levels(band, deepSky, saturation))
    {
        sky.milkyWayTexture = skyTextureBits(ASMilkyWay::rendererMap(false), asr::UISampler::LinearWrap);
        sky.deepSkyTexture = skyTextureBits(ASMilkyWay::rendererMap(true), asr::UISampler::LinearWrap);
        sky.milkyWayIntensity = band;
        sky.deepSkyIntensity = deepSky;
        sky.milkyWaySaturation = saturation;
    }
}

// The frame's sky and tone settings from the current EEP sky and the graphics preferences.
// Scattering coefficients are not in EEP, so the Earth values stay until the legacy values
// are translated (doc/ayanestorm-new-renderer-sky.md, decision 1).
// EEP's 2D clouds, as LLDrawPoolWLSky::renderSkyCloudsDeferred() and LLSettingsVOSky::
// applySpecial() hand them to cloudsF.glsl: the same gates, textures and values.
void fillClouds(asr::SkyInput& sky, const LLSettingsSky& current)
{
    LLViewerTexture* noise = gSky.mVOSkyp ? gSky.mVOSkyp->getCloudNoiseTex() : nullptr;
    LLViewerTexture* next = gSky.mVOSkyp ? gSky.mVOSkyp->getCloudNoiseTexNext() : nullptr;
    if (!noise || !gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_CLOUDS))
    {
        sky.cloudScale = 0.f;
        return;
    }
    sky.cloudTexture = skyTextureBits(noise, asr::UISampler::LinearWrap);
    sky.cloudTextureNext = next && next != noise ? skyTextureBits(next, asr::UISampler::LinearWrap) : asr::NO_SKY_TEXTURE;
    sky.cloudBlend = current.getCloudScrollRate().isExactlyZero() ? 0.f : F32(current.getBlendFactor());
    sky.cloudScale = current.getCloudScale();
    sky.cloudVariance = current.getCloudVariance();
    sky.cloudShadow = current.getCloudShadow();
    // The wind: the scroll so far, its x flipped as the custom-texture preview shows it (SL-13084).
    const LLVector2 scroll = LLEnvironment::instance().getCloudScrollDelta();
    const LLColor3 density1 = current.getCloudPosDensity1();
    const LLColor3 density2 = current.getCloudPosDensity2();
    const LLColor3 color = current.getCloudColor();
    for (S32 i = 0; i < 3; ++i)
    {
        sky.cloudPosDensity1[i] = density1.mV[i];
        sky.cloudPosDensity2[i] = density2.mV[i];
        sky.cloudColor[i] = color.mV[i];
    }
    sky.cloudPosDensity1[0] -= scroll.mV[0];
    sky.cloudPosDensity1[1] += scroll.mV[1];
    sky.cloudMaxY = current.getMaxY();
    sky.cloudDomeRadius = current.getDomeRadius();
    sky.cloudDomeOffset = current.getDomeOffset();
    sky.cloudLayerHeight = ASWorldClouds::layerHeight();
    const LLVector2 world = ASWorldClouds::offsetAt(gAgentCamera.getCameraPositionGlobal());
    sky.cloudWorldOffset[0] = world.mV[0];
    sky.cloudWorldOffset[1] = world.mV[1];
    // cloudsV.glsl's self-shadow step, from lightnorm's x and z (the sky's y and x). OpenGL takes
    // the sky's light, which jumps from the sun to the moon as the sun crosses 0 and jolted the
    // clouds' shading (user: a gap between sun -0.01 and 0). The clouds take the sun's light down
    // to -6 degrees (asrSkyClouds.glsl), so the step follows the sun there, then moves smoothly
    // to the moon by -12.
    const LLVector3 sun = current.getSunDirection();
    const LLVector3 moon = current.getMoonDirection();
    const F32 sunElevation = asinf(llclamp(sun.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
    const F32 toMoon = llclamp((-6.f - sunElevation) / 6.f, 0.f, 1.f);
    const LLVector3 light = lerp(sun, moon, toMoon * toMoon * (3.f - 2.f * toMoon));
    sky.cloudShadowOffset[0] = light.mV[VY] * 0.0125f;
    sky.cloudShadowOffset[1] = light.mV[VX] * 0.0125f;
}

void fillSky(asr::SkyInput& sky, asr::ToneInput& tone)
{
    sky = asr::earthSky();
    // Region shifts are horizontal, so render space heights are region heights.
    if (const LLViewerRegion* region = gAgent.getRegion())
    {
        sky.seaLevel = region->getWaterHeight();
    }

    static LLCachedControl<F32> exposure(gSavedSettings, "RenderExposure", 1.f);
    static LLCachedControl<U32> mapper(gSavedSettings, "RenderTonemapType", 0U);
    static LLCachedControl<bool> autoAdjustLegacy(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
    tone.exposure = llclamp(F32(exposure()), 0.5f, 4.f); // GL's range
    tone.mapper = llmin(U32(mapper()), asr::TONE_MAPPER_AGX);
    static LLCachedControl<F32> bloomStrength(gSavedSettings, "ASRendererBloomStrength", 0.04f);
    static LLCachedControl<bool> renderGlow(gSavedSettings, "RenderGlow", true);
    static LLCachedControl<F32> glowStrength(gSavedSettings, "RenderGlowStrength", 0.325f);
    tone.bloom = llclamp(F32(bloomStrength()), 0.f, 1.f);
    tone.glow = renderGlow() ? llmax(F32(glowStrength()), 0.f) : 0.f;
    static LLCachedControl<F32> adaptation(gSavedSettings, "ASRendererExposureAdaptation", 0.7f);
    tone.adaptation = llclamp(F32(adaptation()), 0.f, 1.f);

    const LLSettingsSky::ptr_t current = LLEnvironment::instance().getCurrentSky(); // inside worldReady()
    if (!current)
    {
        tone.mix = 0.7f;
        return;
    }
    // As GL: RenderTonemapMix for EEP skies (LLSettingsVOSky sets it on the sky), none for a
    // sky made from legacy settings unless those are adjusted.
    tone.mix = current->getTonemapMix(autoAdjustLegacy());

    sky.bottomRadius = current->getSkyBottomRadius();
    sky.topRadius = current->getSkyTopRadius();
    sky.rayleigh = toProfile(current->getRayleighConfigs());
    sky.mie = toProfile(current->getMieConfigs());
    sky.absorption = toProfile(current->getAbsorptionConfigs());
    sky.mieG = current->getMieAnisotropy();
    const LLColor3 glow = current->getGlow();
    sky.sunGlowSize = glow.mV[0];
    sky.sunGlowFocus = glow.mV[2];

    const LLVector3 sun = current->getSunDirection();
    sky.sunDirection[0] = sun.mV[0];
    sky.sunDirection[1] = sun.mV[1];
    sky.sunDirection[2] = sun.mV[2];
    const LLColor3 sunlight = current->getSunlightColor();
    sky.sunIlluminance[0] = sunlight.mV[0];
    sky.sunIlluminance[1] = sunlight.mV[1];
    sky.sunIlluminance[2] = sunlight.mV[2];

    // The sun's image at OpenGL's size, which EEP skies were made against: llvosky.cpp's sun
    // square reaches HEAVENLY_BODY_FACTOR x SUN_DISK_RADIUS (0.5) x sun_scale of its distance
    // from its centre, about 2.9 degrees, its glow included. With no image OpenGL draws no
    // sun, and neither does this: the disc's radius goes to 0.
    const LLVector3 sunUp = sun.mV[VZ] > 0.99f ? LLVector3::z_axis * current->getSunRotation() : LLVector3::z_axis; // as llvosky.cpp
    for (S32 i = 0; i < 3; ++i)
    {
        sky.sunUp[i] = sunUp.mV[i];
    }
    sky.sunTextureRadius = atanf(HEAVENLY_BODY_FACTOR * 0.5f * llmax(current->getSunScale(), 0.f));
    // No image (most EEPs): the sun's disc at the EEP's sun scale, as large as OpenGL's
    // image, carrying the sun's whole light; bloom gives it its glare.
    if (current->getSunTextureId().isNull())
    {
        sky.sunAngularRadius = sky.sunTextureRadius;
    }
    else if (gSky.mVOSkyp)
    {
        sky.sunTexture = skyTextureBits(gSky.mVOSkyp->getSunTex(), asr::UISampler::LinearClamp);
    }

    fillStars(sky, *current); // first: the moon takes its orientation from the stars' frame
    fillMoon(sky, *current);
    fillClouds(sky, *current);
}

constexpr char STALL_SETTING[] = "ASRendererStallTestAt";
constexpr F64 STALL_SECONDS = 5.0; // well short of the 30 s that would log the agent out
constexpr auto STATS_PERIOD = std::chrono::seconds(60);

std::unique_ptr<asr::Device> sDevice;
std::unique_ptr<asr::RenderThread> sRenderThread;
Clock::time_point sStarted;
Clock::time_point sStatsLogged;
Clock::time_point sNextUI;
U32 sWidth = 0;
U32 sHeight = 0;
U32 sUIWidth = 0; // the size of the newest UI list
U32 sUIHeight = 0;
asr::SceneCamera3D sCamera{};
asr::CameraTurn sCameraTurn{};
bool sHasCamera = false;
Clock::time_point sShownAt{};   // see takeShownFrames()
Clock::time_point sRateStart{};
uint64_t sRateFrames = 0;
F64 sShownRate = 0.0;

F64 secondsSince(Clock::time_point start)
{
    return std::chrono::duration<F64>(Clock::now() - start).count();
}

// True once, when the seconds in this setting have passed since the renderer
// started. Setting it later than that, in a running viewer, fires it at once.
bool dueOnce(LLCachedControl<F32>& at, const char* setting)
{
    if (at() <= 0.f || secondsSince(sStarted) < at())
    {
        return false;
    }
    gSavedSettings.setF32(setting, 0.f);
    return true;
}

void logFrameStats(const char* what, const asr::FrameStats& stats)
{
    LL_INFOS("ASRenderer") << what << ": " << stats.frames << " frames, worst frame " << stats.worstFrameSeconds * 1000.0
                           << " ms, oldest state " << stats.worstStateAgeSeconds * 1000.0 << " ms, average state " << stats.averageStateAgeSeconds * 1000.0
                           << " ms, frames with a new state " << stats.newStates << ", frames the mouse turned ahead of the main thread "
                           << stats.turnedFrames << " of " << stats.activeTurnFrames << " with an active turn, frames a zoom eased on " << stats.zoomedFrames << ", of which with clicks not handled yet "
                           << stats.clickFrames << LL_ENDL;
    const asr::FrameParts& parts = stats.worstFrameParts;
    const double recordParts = parts.retireTextures + parts.textureCommands + parts.readResults + parts.sceneUpdate + parts.sceneGrowth + parts.sceneStaging;
    // Temporary through step d: finds what the render thread's record spikes are.
    LL_INFOS("ASRenderer") << "worst frame, ms: wait for GPU " << parts.waitForGpu * 1000.0 << ", acquire " << parts.acquire * 1000.0 << ", UI " << parts.prepareUI * 1000.0
                           << ", record " << parts.record * 1000.0 << " of which retire " << parts.texturesRetired << " textures " << parts.retireTextures * 1000.0
                           << ", texture commands " << parts.textureCommands * 1000.0 << " (" << parts.texturesCreated << " created, " << parts.textureUploadBytes / 1024
                           << " KB uploaded, " << parts.textureCommandsLeft << " left for later), read results " << parts.readResults * 1000.0 << ", scene update " << parts.sceneUpdate * 1000.0 << ", scene growth "
                           << parts.sceneGrowth * 1000.0 << ", scene staging " << parts.sceneStaging * 1000.0 << " (" << parts.sceneStagingBytes / 1024
                           << " KB), the rest " << std::max(0.0, parts.record - recordParts) * 1000.0 << ", submit " << parts.submit * 1000.0
                           << ", present " << parts.present * 1000.0 << "; worst memory refresh " << stats.worstRefreshSeconds * 1000.0
                           << "; uploads waiting for room in the ring now " << (sDevice ? sDevice->textures().waitingBytes() / 1024 : 0) << " KB" << LL_ENDL;
    const std::string gpu_errors = sDevice ? sDevice->takeGpuErrors() : std::string();
    if (!gpu_errors.empty())
    {
        LL_WARNS("ASRenderer") << gpu_errors << LL_ENDL;
    }
}

// The P2 gate: a stopped main thread must not reach the render rate.
void runStallTest()
{
    logFrameStats("before the stall", sRenderThread->takeStats());
    const Clock::time_point start = Clock::now();
    ms_sleep(U32(STALL_SECONDS * 1000.0));
    const F64 seconds = secondsSince(start);
    const asr::FrameStats stats = sRenderThread->takeStats();
    logFrameStats("during the stall", stats);
    LL_INFOS("ASRenderer") << "main thread stopped for " << seconds << " s, render rate " << stats.frames / seconds << " fps" << LL_ENDL;
}

U32 sourcePixelBytes(ASR::PixelLayout layout)
{
    switch (layout)
    {
        case ASR::PixelLayout::RGBA:
        case ASR::PixelLayout::BGRA:           return 4;
        case ASR::PixelLayout::RGB:
        case ASR::PixelLayout::BGR:            return 3;
        case ASR::PixelLayout::RG:
        case ASR::PixelLayout::LuminanceAlpha: return 2;
        default:                               return 1;
    }
}

asr::TextureDesc textureDesc(U32 width, U32 height, U32 mipLevels, ASR::PixelLayout layout, U8 storage = 0)
{
    if (storage != 0)
    {
        // Compressed: the swizzle still applies, to what the format keeps.
        asr::TextureDesc desc = textureDesc(width, height, mipLevels, layout);
        desc.format = asr::TextureFormat(storage);
        return desc;
    }
    asr::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.mipLevels = mipLevels;
    switch (layout)
    {
        case ASR::PixelLayout::RGBA:
        case ASR::PixelLayout::RGB:
            desc.format = asr::TextureFormat::RGBA8;
            break;
        case ASR::PixelLayout::BGRA:
        case ASR::PixelLayout::BGR:
            desc.format = asr::TextureFormat::BGRA8;
            break;
        case ASR::PixelLayout::Red:
            desc.format = asr::TextureFormat::R8;
            break;
        case ASR::PixelLayout::RG:
            desc.format = asr::TextureFormat::RG8;
            break;
        case ASR::PixelLayout::Alpha:
            desc.format = asr::TextureFormat::R8;
            desc.swizzle = asr::TextureSwizzle::AlphaOnly;
            break;
        case ASR::PixelLayout::Luminance:
            desc.format = asr::TextureFormat::R8;
            desc.swizzle = asr::TextureSwizzle::Luminance;
            break;
        case ASR::PixelLayout::LuminanceAlpha:
            desc.format = asr::TextureFormat::RG8;
            desc.swizzle = asr::TextureSwizzle::LuminanceAlpha;
            break;
    }
    return desc;
}

class ViewerLink final : public ASR::RendererLink
{
public:
    U32 createTexture(U32 width, U32 height, U32 mipLevels, ASR::PixelLayout layout, U8 storage) override
    {
        return sDevice->textures().create(textureDesc(width, height, mipLevels, layout, storage));
    }

    // BC formats for fetched images, chosen from their pixels: see asr::chooseTextureFormat().
    // Web and media pages (BGRA) are redrawn often and stay as they are.
    U8 chooseStorage(U32 width, U32 height, ASR::PixelLayout layout, const void* pixels) override
    {
        static LLCachedControl<bool> compress(gSavedSettings, "ASTextureCompression", true);
        if (!compress || !sDevice->textures().compressionSupported() || layout == ASR::PixelLayout::BGRA || layout == ASR::PixelLayout::BGR)
        {
            return 0;
        }
        const U32 channels = sourcePixelBytes(layout);
        const asr::TextureFormat format = asr::chooseTextureFormat(static_cast<const U8*>(pixels), width, height, channels);
        return asr::textureCompressed(format) ? U8(format) : 0;
    }

    void uploadTexture(U32 slot, U32 level, U32 x, U32 y, U32 width, U32 height, ASR::PixelLayout layout, const void* pixels, U32 rowPixels, U8 storage) override
    {
        // GPUs have no three-channel format, so those rows gain an opaque alpha. A texture
        // the GPU compresses takes four bytes a texel, its channels first.
        const U32 sourceBytes = sourcePixelBytes(layout);
        const U32 targetBytes = sourceBytes == 3 || storage != 0 ? 4 : sourceBytes;
        const size_t sourceRow = size_t(rowPixels) * sourceBytes;
        const size_t targetRow = size_t(width) * targetBytes;

        // Written once, usually straight into the memory the GPU copies from.
        asr::TextureQueue& textures = sDevice->textures();
        U8* target = textures.beginUpload(slot, level, x, y, width, height, U32(targetRow));
        const U8* source = static_cast<const U8*>(pixels);
        for (U32 row = 0; target && row < height; ++row)
        {
            if (sourceBytes == targetBytes)
            {
                memcpy(target, source, targetRow);
            }
            else
            {
                for (U32 column = 0; column < width; ++column)
                {
                    for (U32 c = 0; c < 4; ++c)
                    {
                        target[column * 4 + c] = c < sourceBytes ? source[column * sourceBytes + c] : c == 3 ? 255 : 0;
                    }
                }
            }
            source += sourceRow;
            target += targetRow;
        }
        textures.finishUpload();
    }

    void generateMips(U32 slot, const void* cacheOwner) override
    {
        // The disk cache may keep what is encoded: it hands a token to read the blocks back.
        LLViewerFetchedTexture* owner = static_cast<LLViewerFetchedTexture*>(const_cast<void*>(cacheOwner));
        sDevice->textures().generateMips(slot, owner ? ASRTextureCache::store(owner) : 0);
    }

    U32 createEncodedTexture(U32 width, U32 height, U32 mipLevels, ASR::PixelLayout layout, U8 storage) override
    {
        return sDevice->textures().createEncoded(textureDesc(width, height, mipLevels, layout, storage));
    }

    void replaceEncodedTexture(U32 slot, U32 width, U32 height, U32 mipLevels, ASR::PixelLayout layout, U8 storage) override
    {
        sDevice->textures().replaceEncoded(slot, textureDesc(width, height, mipLevels, layout, storage));
    }

    void uploadBlocks(U32 slot, U32 width, U32 height, U32 mipLevels, U8 storage, U32 level, const U8* blocks) override
    {
        asr::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.mipLevels = mipLevels;
        desc.format = asr::TextureFormat(storage);
        sDevice->textures().uploadBlocks(slot, desc, level, blocks);
    }

    void replaceTexture(U32 slot, U32 width, U32 height, U32 mipLevels, ASR::PixelLayout layout, U32 fromMip, U8 storage) override
    {
        sDevice->textures().replace(slot, textureDesc(width, height, mipLevels, layout, storage), fromMip);
    }

    void publishTexture(U32 slot) override
    {
        sDevice->textures().publish(slot);
    }

    void destroyTexture(U32 slot) override
    {
        sDevice->textures().destroy(slot, sRenderThread->currentUIGeneration());
    }

    void getTextureMemory(U64& used, U64& limit, U64& hardLimit) override
    {
        const asr::MemoryBudget& budget = sDevice->memoryBudget();
        used = budget.used(asr::MemoryCategory::Textures);
        limit = budget.textureLimit();
        // Honour the viewer's texture allowance; unused VRAM is not a request to fill it.
        static LLCachedControl<bool> manual(gSavedSettings, "FSLimitTextureVRAMUsage");
        static LLCachedControl<U32> megabytes(gSavedSettings, "RenderMaxVRAMBudget", 0);
        static LLCachedControl<U32> divisor(gSavedSettings, "RenderTextureVRAMDivisor", 2);
        const U64 configured = (manual() ? U64(megabytes()) : U64(llmax(1024.f, F32(gGLManager.mVRAM) / F32(divisor() ? divisor() : 1)))) * 1024 * 1024;
        limit = llmin(limit, configured);
        hardLimit = limit;
    }

    U32 firstTextureMip(U32 width, U32 height, U32 components, bool mips) override
    {
        U64 used = 0, limit = 0, hard = 0;
        getTextureMemory(used, limit, hard);
        const U64 target = limit / 100 * 85;
        return asr::firstTextureMip(width, height, components == 3 ? 4 : components, mips, target > used ? target - used : 0);
    }

    U32 freeTextureSlots() override
    {
        return sDevice->textures().freeSlots();
    }

    void beginUI() override
    {
        mList = &sRenderThread->ui();
        mList->clear();
        mList->setSize(sWidth, sHeight);
    }

    void setUIBlend(const U8 factors[4]) override
    {
        asr::UIBlend blend;
        blend.colorSource = asr::UIBlendFactor(factors[0]);
        blend.colorDest = asr::UIBlendFactor(factors[1]);
        blend.alphaSource = asr::UIBlendFactor(factors[2]);
        blend.alphaDest = asr::UIBlendFactor(factors[3]);
        mList->setBlend(blend);
    }

    void setUIScissor(bool enabled, S32 x, S32 y, S32 width, S32 height) override
    {
        asr::UIScissor scissor;
        scissor.enabled = enabled;
        scissor.x = x;
        scissor.y = y;
        scissor.width = width;
        scissor.height = height;
        mList->setScissor(scissor);
    }

    void setUIDepthCull(bool test, bool write, U8 compare, bool cull, U8 cullFace) override
    {
        mList->setDepthTest(test);
        mList->setDepthWrite(write);
        mList->setDepthCompare(asr::UICompare(compare & 7));
        mList->setCullEnabled(cull);
        mList->setCullFace(cullFace == 0 ? asr::UICull::Front : asr::UICull::Back);
    }

    void setUIViewport(S32 x, S32 y, S32 width, S32 height) override
    {
        mList->setViewport(x, y, width, height);
    }

    void clearUIDepth() override
    {
        mList->clearDepth();
    }

    void beginUIWorld() override
    {
        // The camera published this frame: its eye space is GL's, so the inverse of its
        // rows takes GL eye space to the renderer's world (render origin included).
        // Rows: eye = R * world + t, so world = R^T * (eye - t). Column-major.
        const float (&rows)[3][4] = sCamera.viewRows;
        for (U32 column = 0; column < 3; ++column)
        {
            for (U32 row = 0; row < 3; ++row)
            {
                mEyeToWorld[column * 4 + row] = rows[column][row];
            }
            mEyeToWorld[column * 4 + 3] = 0.f;
        }
        for (U32 row = 0; row < 3; ++row)
        {
            mEyeToWorld[12 + row] = -(rows[0][row] * rows[0][3] + rows[1][row] * rows[1][3] + rows[2][row] * rows[2][3]);
        }
        mEyeToWorld[15] = 1.f;
        mList->beginWorld();
    }

    void markUIHud() override
    {
        mList->markHud();
    }

    void drawUI(const ASR::UIDrawData& data) override
    {
        asr::UIStreams streams;
        streams.positions = data.positions;
        streams.uvs = data.uvs;
        streams.colors = data.uniformColor ? nullptr : data.colors;
        streams.singleColor = data.singleColor;
        streams.matrix = data.matrix;
        F32 toWorld[16];
        if (data.world)
        {
            // Eye space (data.matrix) on to the renderer's world: see beginUIWorld().
            for (U32 column = 0; column < 4; ++column)
            {
                for (U32 row = 0; row < 4; ++row)
                {
                    F32 sum = 0.f;
                    for (U32 k = 0; k < 4; ++k)
                    {
                        sum += mEyeToWorld[k * 4 + row] * data.matrix[column * 4 + k];
                    }
                    toWorld[column * 4 + row] = sum;
                }
            }
            streams.matrix = toWorld;
            streams.world = true;
        }
        streams.uvMatrix = data.uvMatrix;
        streams.texture = asr::uiTextureBits(data.slot, asr::UISampler(data.sampler), data.solidColor);
        mList->append(asr::UIGeometry(data.mode), streams, data.count);
    }

    void publishUI() override
    {
        sRenderThread->publishUI();
        mList = nullptr;
    }

private:
    asr::UIList* mList = nullptr;
    F32 mEyeToWorld[16] = {}; // column-major, see beginUIWorld()
};

ViewerLink sViewerLink;

// A context would mean a GL path was missed. Loaded GL libraries prove nothing:
// the Vulkan driver loads them too.
void logGLContextCheck(const char* when)
{
#if LL_WINDOWS
    const bool hasContext = wglGetCurrentContext() != nullptr;
#else
    const bool hasContext = CGLGetCurrentContext() != nullptr;
#endif
    if (hasContext)
    {
        LL_WARNS("ASRenderer") << "a GL context exists " << when << LL_ENDL;
    }
    else
    {
        LL_INFOS("ASRenderer") << "no GL context " << when << LL_ENDL;
    }
}

// No GL context exists in this session, so OpenGL cannot take over now. The next
// launch uses it instead.
[[noreturn]] void failAndQuit(const std::string& reason)
{
    LL_WARNS("ASRenderer") << "stopping: " << reason << LL_ENDL;
    gSavedSettings.setString(BACKEND_SETTING, BACKEND_OPENGL);
    gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
    OSMessageBox("The new renderer stopped: " + reason + "\nThe next launch will use OpenGL.", "AyaneStorm", OSMB_OK);
    LLAppViewer::instance()->fastQuit(1);
    std::abort(); // fastQuit does not return
}

std::string startRenderer(LLWindow* window)
{
    asr::SurfaceTarget target;
    target.nativeWindow = window->getRendererSurface();
#if LL_WINDOWS
    target.nativeInstance = GetModuleHandle(nullptr);
#endif
    if (!target.nativeWindow)
    {
        return "the window has no surface for the renderer";
    }

    asr::DeviceOptions options;
    options.shaderDir = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders_native");
    options.validation = gSavedSettings.getBOOL("ASRendererValidation");
    options.report = [](const char* message) { LL_WARNS("ASRenderer") << "renderer: " << message << LL_ENDL; };
    options.vsync = gSavedSettings.getBOOL("RenderVSyncEnable");
    options.memoryCapBytes = U64(gSavedSettings.getU32("ASRendererVRAMBudgetMB")) * 1024 * 1024;

    std::string error;
    sDevice = asr::Device::create(target, options, error);
    if (!sDevice)
    {
        return error;
    }

    sRenderThread = std::make_unique<asr::RenderThread>(*sDevice, ASRCameraTurn::motion());
    sRenderThread->start();
    ASRCameraTurn::start(window);
    sStarted = Clock::now();
    sStatsLogged = sStarted;
    ASR::setRendererLink(&sViewerLink);
    ASR::addUniformColorProgram(&gDebugProgram); // the rotate manipulator's rings
    ASRWorld::start();
    ASRTextureCache::start();
    LL_INFOS("ASRenderer") << "running on " << sDevice->info().backend << ", " << sDevice->info().name << ", budget "
                           << sDevice->memoryBudget().budget() / (1024 * 1024) << " MB" << LL_ENDL;
    return std::string();
}

void stopRenderer()
{
    ASRCameraTurn::stop();
    ASRTextureCache::stop();
    ASRWorld::stop();
    ASR::setRendererLink(nullptr);
    if (sRenderThread)
    {
        sRenderThread->stop();
        logFrameStats("last period", sRenderThread->takeStats());
        LL_INFOS("ASRenderer") << "stopped with " << sDevice->validationErrors() << " validation errors" << LL_ENDL;
    }
    sRenderThread.reset();
    sDevice.reset();
}

#endif // ASR_VIEWER_SUPPORTED

} // namespace

void ASRViewer::selectBackend()
{
    const std::string backend = gSavedSettings.getString(BACKEND_SETTING);
    if (backend != BACKEND_NATIVE)
    {
        if (backend != BACKEND_OPENGL)
        {
            LL_WARNS("ASRenderer") << "unknown " << BACKEND_SETTING << " '" << backend << "', using OpenGL" << LL_ENDL;
        }
        return;
    }

#if ASR_VIEWER_SUPPORTED
    const asr::ProbeResult probe = asr::Device::probe();
    if (probe.supported)
    {
        LL_INFOS("ASRenderer") << "probe accepts " << probe.info.name << LL_ENDL;
        ASR::setActive(probe.info.vendor, probe.info.name);
        return;
    }
    LL_WARNS("ASRenderer") << "using OpenGL: " << probe.reason << LL_ENDL;
#else
    LL_WARNS("ASRenderer") << "using OpenGL: this build has no new renderer" << LL_ENDL;
#endif
    sFellBack = true;
}

void ASRViewer::start(LLWindow* window)
{
#if ASR_VIEWER_SUPPORTED
    if (ASR::active())
    {
        logGLContextCheck("after creating the window");
        const std::string error = startRenderer(window);
        if (!error.empty())
        {
            failAndQuit(error);
        }
        publishFrame(window);
    }
#endif
}

void ASRViewer::publishFrame(LLWindow* window)
{
#if ASR_VIEWER_SUPPORTED
    if (!ASR::active() || !sRenderThread)
    {
        return;
    }
    if (sRenderThread->failed())
    {
        failAndQuit(sRenderThread->error());
    }

    LLCoordWindow size;
    window->getSize(&size);
    sWidth = U32(llmax(size.mX, 0));
    sHeight = U32(llmax(size.mY, 0));
    asr::FrameState& state = sRenderThread->state();
    state.width = sWidth;
    state.height = sHeight;
    state.hasCamera = sHasCamera;
    state.camera = sCamera;
    state.turn = sCameraTurn;
    static LLCachedControl<bool> predict_camera(gSavedSettings, "ASRendererCameraPrediction", true);
    state.predictCamera = predict_camera;
    static LLCachedControl<bool> limit_framerate(gSavedSettings, "FSLimitFramerate", false);
    static LLCachedControl<U32> max_fps(gSavedSettings, "FramePerSecondLimit", 0);
    state.maxFramesPerSecond = limit_framerate() ? F32(max_fps()) : 0.f;
    // Everything read from the viewer's world: see worldReady().
    if (worldReady())
    {
        fillSky(state.sky, state.tone);
        ASRWorld::fillOverlay(state.overlay);
    }
    else
    {
        state.sky = asr::earthSky();
        state.tone = asr::ToneInput{};
        state.overlay = asr::OverlayInput{};
    }
    // The world view, where OpenGL draws the scene and HUDs, and picking reads them: it can
    // leave part of the window to the UI. From the window's top left, as the renderer's.
    // Frames are published while the viewer window is still being built: whole window then.
    const LLRect world = gViewerWindow ? gViewerWindow->getWorldViewRectRaw()
                                       : LLRect(0, size.mY, size.mX, 0);
    state.worldView[0] = U32(llmax(world.mLeft, 0));
    state.worldView[1] = U32(llmax(size.mY - world.mTop, 0));
    state.worldView[2] = U32(llmax(world.getWidth(), 0));
    state.worldView[3] = U32(llmax(world.getHeight(), 0));
    static LLCachedControl<F32> anisotropy(gSavedSettings, "RenderAnisotropicLevel", 0.f);
    state.anisotropy = llmax(F32(anisotropy()), 1.f); // 0 is off, as 1
    sRenderThread->publishState();
    sDevice->textures().flush(); // uploads that found the ring full

    if (Clock::now() - sStatsLogged >= STATS_PERIOD)
    {
        sStatsLogged = Clock::now();
        logFrameStats("last minute", sRenderThread->takeStats());
        ASRCameraTurn::logCounts();
    }

    static LLCachedControl<F32> stall_at(gSavedSettings, STALL_SETTING, 0.f);
    if (dueOnce(stall_at, STALL_SETTING))
    {
        runStallTest();
    }
#endif
}

bool ASRViewer::beginUI()
{
#if ASR_VIEWER_SUPPORTED
    static LLCachedControl<U32> ui_hz(gSavedSettings, "ASRendererUIHz", 60);
    // A list of the wrong size leaves part of the window black, so it does not wait.
    const bool resized = sWidth != sUIWidth || sHeight != sUIHeight;
    if (ASR::active() && ui_hz() != 0 && !resized)
    {
        const Clock::time_point now = Clock::now();
        if (now < sNextUI)
        {
            return false;
        }
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<F64>(1.0 / ui_hz()));
        sNextUI = std::max(sNextUI + period, now);
    }
    sUIWidth = sWidth;
    sUIHeight = sHeight;
#endif
    ASR::beginUI();
    return true;
}

void ASRViewer::publishUI()
{
    ASR::endUI();
}

void ASRViewer::stop()
{
#if ASR_VIEWER_SUPPORTED
    if (ASR::active())
    {
        logGLContextCheck("at shutdown");
        stopRenderer();
    }
#endif
}

bool ASRViewer::fellBackToOpenGL()
{
    return sFellBack;
}

asr::Device* ASRViewer::device()
{
#if ASR_VIEWER_SUPPORTED
    return sDevice.get();
#else
    return nullptr;
#endif
}

void ASRViewer::setCamera(const asr::SceneCamera3D& camera, const asr::CameraTurn& turn)
{
#if ASR_VIEWER_SUPPORTED
    sCamera = camera;
    sCameraTurn = turn;
    sHasCamera = true;
#endif
}

F64 ASRViewer::takeShownFrames()
{
#if ASR_VIEWER_SUPPORTED
    if (ASR::active() && sRenderThread)
    {
        // The viewer shows the median of its loops' rates. With the render thread slower
        // than the main loop, as under a frame limit, most loops would see no new frame and
        // show 0. So each loop gets the render rate of the last quarter second, for its own
        // length, and the sum still comes to the frames shown.
        constexpr auto RATE_PERIOD = std::chrono::milliseconds(250);
        const Clock::time_point now = Clock::now();
        const uint64_t shown = sRenderThread->presentedFrames();
        if (sShownAt == Clock::time_point{})
        {
            sShownAt = now;
            sRateStart = now;
            sRateFrames = shown;
        }
        if (now - sRateStart >= RATE_PERIOD)
        {
            sShownRate = F64(shown - sRateFrames) / std::chrono::duration<F64>(now - sRateStart).count();
            sRateStart = now;
            sRateFrames = shown;
        }
        const F64 frames = sShownRate * std::chrono::duration<F64>(now - sShownAt).count();
        sShownAt = now;
        return frames;
    }
#endif
    return 1;
}
