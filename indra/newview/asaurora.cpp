/**
 * @file asaurora.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local configurable procedural aurora rendering.
 */

#include "llviewerprecompiledheaders.h"

#include "asaurora.h"

#include "llagentcamera.h"
#include "llappviewer.h"
#include "llenvironment.h"
#include "llmath.h"
#include "llrand.h"
#include "llsettingssky.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

namespace
{
    LLGLSLShader sAuroraProgram;
    const LLStaticHashedString sAuroraTime("aurora_time");
    const LLStaticHashedString sAuroraIntensity("aurora_intensity");
    const LLStaticHashedString sAuroraSpeed("aurora_speed");
    const LLStaticHashedString sAuroraScale("aurora_scale");
    const LLStaticHashedString sAuroraCoverage("aurora_coverage");
    const LLStaticHashedString sAuroraHeight("aurora_height");
    const LLStaticHashedString sAuroraThickness("aurora_thickness");
    const LLStaticHashedString sAuroraSteps("aurora_steps");
    const LLStaticHashedString sAuroraWorldOrigin("aurora_world_origin");
    const LLStaticHashedString sAuroraLowColor("aurora_low_color");
    const LLStaticHashedString sAuroraHighColor("aurora_high_color");
    const LLStaticHashedString sAuroraSeedOffset("aurora_seed_offset");
}

void ASAurora::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASAurora.RandomizeSeed",
        [](LLUICtrl*, const LLSD&) { gSavedSettings.setS32("ASAuroraSeed", ll_rand(1000000)); });
}

void ASAurora::registerShader(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sAuroraProgram);
}

bool ASAurora::createShader(S32 shader_level)
{
    sAuroraProgram.mName = "AyaneStorm Aurora Shader";
    sAuroraProgram.mShaderFiles.clear();
    sAuroraProgram.clearPermutations();
    sAuroraProgram.mFeatures.isDeferred = true;
    sAuroraProgram.mShaderFiles.emplace_back("deferred/asauroraV.glsl", GL_VERTEX_SHADER);
    sAuroraProgram.mShaderFiles.emplace_back("deferred/asauroraF.glsl", GL_FRAGMENT_SHADER);
    sAuroraProgram.mShaderLevel = shader_level;
    sAuroraProgram.mShaderGroup = LLGLSLShader::SG_SKY;
    // Match the active deferred sky render-target layout. The upstream sky
    // programs receive this through add_common_permutations(), which is local
    // to LLViewerShaderMgr; this independent program must select it here.
    if (gSavedSettings.getBOOL("RenderEnableEmissiveBuffer"))
    {
        sAuroraProgram.addPermutation("HAS_EMISSIVE", "1");
    }
    return sAuroraProgram.createShader();
}

void ASAurora::unloadShader()
{
    sAuroraProgram.unload();
}

bool ASAurora::configureShader()
{
    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    if (!sky || !gSavedSettings.getBOOL("ASAuroraEnabled") || !sAuroraProgram.isComplete())
    {
        return false;
    }

    const F32 sun_elevation = asinf(llclamp(sky->getSunDirection().mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
    const F32 fade_start = llclamp(gSavedSettings.getF32("ASAuroraSunFadeAngle"), -18.f, 5.f);
    const F32 daylight_fade = 1.f - llclamp((sun_elevation - fade_start) / 6.f, 0.f, 1.f);
    const F32 intensity = llclamp(gSavedSettings.getF32("ASAuroraIntensity"), 0.f, 4.f) * daylight_fade;
    if (intensity <= 0.001f)
    {
        return false;
    }

    const LLVector3d camera = gAgentCamera.getCameraPositionGlobal();
    const LLColor4 low_color = gSavedSettings.getColor4("ASAuroraLowColor");
    const LLColor4 high_color = gSavedSettings.getColor4("ASAuroraHighColor");
    const S32 quality = llclamp(gSavedSettings.getS32("ASAuroraQuality"), 0, 5);
    const F32 seed = (F32)llclamp(gSavedSettings.getS32("ASAuroraSeed"), 0, 999999);
    const F32 thickness = llclamp(gSavedSettings.getF32("ASAuroraThickness"), 0.03f, 1.0f);

    sAuroraProgram.bind();
    sAuroraProgram.uniform1f(sAuroraTime, gFrameTimeSeconds);
    sAuroraProgram.uniform1f(sAuroraIntensity, intensity);
    sAuroraProgram.uniform1f(sAuroraSpeed, llclamp(gSavedSettings.getF32("ASAuroraSpeed"), 0.f, 3.f));
    sAuroraProgram.uniform1f(sAuroraScale, llclamp(gSavedSettings.getF32("ASAuroraScale"), 0.1f, 4.f));
    sAuroraProgram.uniform1f(sAuroraCoverage, llclamp(gSavedSettings.getF32("ASAuroraCoverage"), 0.f, 1.f));
    sAuroraProgram.uniform1f(sAuroraHeight, llclamp(gSavedSettings.getF32("ASAuroraHeight"), 0.15f, 0.9f));
    sAuroraProgram.uniform1f(sAuroraThickness, thickness);
    sAuroraProgram.uniform1i(sAuroraSteps, quality == 0 ? 12 :
                                           quality == 1 ? 20 :
                                           quality == 2 ? 32 :
                                           quality == 3 ? 64 :
                                           quality == 4 ? 128 : 256);
    sAuroraProgram.uniform2f(sAuroraWorldOrigin, (F32)fmod(camera.mdV[VX], 65536.0),
                                                (F32)fmod(camera.mdV[VY], 65536.0));
    sAuroraProgram.uniform3fv(sAuroraLowColor, 1, low_color.mV);
    sAuroraProgram.uniform3fv(sAuroraHighColor, 1, high_color.mV);
    sAuroraProgram.uniform2f(sAuroraSeedOffset, seed * 0.00137f, seed * 0.00211f);
    return true;
}

LLGLSLShader& ASAurora::getShader()
{
    return sAuroraProgram;
}
