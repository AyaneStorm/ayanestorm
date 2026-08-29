/**
 * @file asweathersnow.h
 * @author chanayane@firestorm
 * @brief Stateful world-space snow effect for ASWeather.
 */

#ifndef AS_WEATHER_SNOW_H
#define AS_WEATHER_SNOW_H

#include <vector>

#include "llglslshader.h"

namespace ASWeather
{
    struct FrameContext;
}

class LLCamera;
class LLPipeline;

namespace ASWeatherSnow
{
    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool createShaders(S32 shader_level);
    void unloadShaders();
    void releaseResources();
    bool isSupported();
    void updateAndRender(const ASWeather::FrameContext& context, LLPipeline& pipeline,
                         LLCamera& camera);
}

#endif
