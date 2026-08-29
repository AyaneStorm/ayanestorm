/**
 * @file asweather.h
 * @author chanayane@firestorm
 * @brief Shared viewer-local weather rendering and shelter resources.
 */

#ifndef AS_WEATHER_H
#define AS_WEATHER_H

#include <vector>

#include "llglslshader.h"
#include "v3math.h"

class LLCamera;
class LLPipeline;
class LLRenderTarget;

namespace ASWeather
{
    struct FrameContext
    {
        LLVector3 center;
        LLVector3 drift;
        F32 radius{ 0.f };
        F32 fullDensityRadius{ 0.f };
        F32 top{ 0.f };
        F32 bottom{ 0.f };
        F32 waterHeight{ 0.f };
    };

    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool createShaders(S32 shader_level);
    void unloadShaders();
    void releaseResources();
    // Capture shared precipitation shelter data before final scene compositing.
    void prepare(LLPipeline& pipeline, LLCamera& camera);
    // Draw enabled effects into the already-composited scene target.
    void render(LLPipeline& pipeline, LLCamera& camera, LLRenderTarget& screen);
}

#endif
