/**
 * @file asweather.cpp
 * @author chanayane@firestorm
 * @brief Shared weather frame data and effect dispatch.
 */

#include "llviewerprecompiledheaders.h"

#include "asweather.h"

#include "asbackgroundisolate.h"
#include "asweathersnow.h"
#include "llcontrol.h"
#include "llenvironment.h"
#include "pipeline.h"
#include "llviewercontrol.h"
#include "llviewercamera.h"
#include "lltimer.h"

extern bool gCubeSnapshot;

namespace
{
    ASWeather::FrameContext sFrameContext;
    bool sFramePrepared = false;
    bool sLoggedEnabled = false;
    F32 sAppliedDistance = 32.f;
    F32 sPendingDistance = 32.f;
    bool sAppliedFalloff = true;
    bool sPendingFalloff = true;
    bool sDistanceInitialized = false;
    F64 sDistanceChangedAt = 0.0;

}

void ASWeather::registerShaders(std::vector<LLGLSLShader*>& shaders)
{
    ASWeatherSnow::registerShaders(shaders);
}

bool ASWeather::createShaders(S32 shader_level)
{
    return ASWeatherSnow::createShaders(shader_level);
}

void ASWeather::unloadShaders()
{
    releaseResources();
    ASWeatherSnow::unloadShaders();
}

void ASWeather::releaseResources()
{
    if (sLoggedEnabled)
    {
        LL_INFOS("Weather") << "Snow disabled; releasing Weather resources" << LL_ENDL;
    }
    sFramePrepared = false;
    sLoggedEnabled = false;
    sDistanceInitialized = false;
    ASWeatherSnow::releaseResources();
}

void ASWeather::prepare(LLPipeline& pipeline, LLCamera& camera)
{
    (void)pipeline;
    sFramePrepared = false;
    if (!gSavedSettings.getBOOL("ASWeatherSnowEnabled") ||
        !ASWeatherSnow::isSupported() || gCubeSnapshot ||
        LLPipeline::sImpostorRender || LLPipeline::sRenderingHUDs ||
        LLPipeline::sUnderWaterRender || ASBackgroundIsolate::isActive())
    {
        if (!gSavedSettings.getBOOL("ASWeatherSnowEnabled") && sLoggedEnabled)
        {
            releaseResources();
        }
        return;
    }

    if (!sLoggedEnabled)
    {
        LL_INFOS("Weather") << "Snow enabled; using incremental raycast shelter cache"
                            << LL_ENDL;
        sLoggedEnabled = true;
    }

    sFrameContext.center = camera.getOrigin();
    sFrameContext.drift.set(0.22f, 0.08f, 0.f);
    const F32 requested_distance = llclamp(
        gSavedSettings.getF32("ASWeatherSnowDistance"), 8.f, 128.f);
    const bool requested_falloff =
        gSavedSettings.getBOOL("ASWeatherSnowDistanceFalloff");
    const F64 now = LLTimer::getTotalSeconds();
    if (!sDistanceInitialized)
    {
        sAppliedDistance = sPendingDistance = requested_distance;
        sAppliedFalloff = sPendingFalloff = requested_falloff;
        sDistanceChangedAt = now;
        sDistanceInitialized = true;
    }
    else if (requested_distance != sPendingDistance ||
             requested_falloff != sPendingFalloff)
    {
        sPendingDistance = requested_distance;
        sPendingFalloff = requested_falloff;
        sDistanceChangedAt = now;
    }
    else if ((sAppliedDistance != sPendingDistance ||
              sAppliedFalloff != sPendingFalloff) &&
             now - sDistanceChangedAt >= 0.25)
    {
        sAppliedDistance = sPendingDistance;
        sAppliedFalloff = sPendingFalloff;
    }
    sFrameContext.fullDensityRadius = sAppliedDistance;
    sFrameContext.radius = sAppliedDistance + (sAppliedFalloff ? 8.f : 0.f);
    sFrameContext.top = sFrameContext.center.mV[VZ] + 128.f;
    sFrameContext.bottom = sFrameContext.center.mV[VZ] - 128.f;
    sFrameContext.waterHeight = LLEnvironment::instance().getWaterHeight();
    sFramePrepared = true;
}

void ASWeather::render(LLPipeline& pipeline, LLCamera& camera, LLRenderTarget& screen)
{
    (void)screen;
    if (sFramePrepared && gSavedSettings.getBOOL("ASWeatherSnowEnabled"))
    {
        // World intersection skips an entire spatial partition when its
        // drawable render type is disabled. Weather runs after scene passes,
        // where Volume is not guaranteed to remain enabled; explicitly expose
        // structural volumes to read-only shelter queries, then restore the
        // exact prior mask. No rendering or culling occurs in this scope.
        LL_INFOS_ONCE("Weather") << "Shelter query input mask: volume="
                                 << pipeline.hasRenderType(LLPipeline::RENDER_TYPE_VOLUME)
                                 << " terrain="
                                 << pipeline.hasRenderType(LLPipeline::RENDER_TYPE_TERRAIN)
                                 << LL_ENDL;
        pipeline.pushRenderTypeMask();
        pipeline.setRenderTypeMask(LLPipeline::RENDER_TYPE_VOLUME,
                                   LLPipeline::RENDER_TYPE_TERRAIN,
                                   LLPipeline::END_RENDER_TYPES);
        ASWeatherSnow::updateAndRender(sFrameContext, pipeline, camera);
        pipeline.popRenderTypeMask();
    }
}
