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

extern bool gCubeSnapshot;

namespace
{
    ASWeather::FrameContext sFrameContext;
    bool sFramePrepared = false;
    bool sLoggedEnabled = false;

    F32 radiusForQuality(S32 quality)
    {
        (void)quality;
        // Intensity controls volumetric density; Quality must not change the
        // area over which the same storm is measured.
        return 32.f;
    }
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

    const S32 quality = llclamp(gSavedSettings.getS32("ASWeatherSnowQuality"), 0, 2);
    sFrameContext.center = camera.getOrigin();
    sFrameContext.drift.set(0.22f, 0.08f, 0.f);
    sFrameContext.radius = radiusForQuality(quality);
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
        ASWeatherSnow::updateAndRender(sFrameContext, pipeline, camera);
    }
}
