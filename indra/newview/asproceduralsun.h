/**
 * @file asproceduralsun.h
 * @author chanayane@firestorm
 * @brief Viewer-local procedural sunset sun-disc controls.
 */

#ifndef AS_PROCEDURAL_SUN_H
#define AS_PROCEDURAL_SUN_H

#include "v3color.h"
#include "v3math.h"

class LLEnvironment;
class LLSettingsSky;
class LLFace;
class LLGLSLShader;

namespace ASProceduralSun
{
    // Registers reset buttons used by the standalone settings panel.
    void registerUICallbacks();

    struct RenderParams
    {
        bool enabled = false;
        F32 opacity = 0.f;
        F32 horizon_factor = 0.f;
        F32 halo_factor = 0.f;
        LLColor3 color;
        LLColor3 limb_color;
    };

    struct WaterLightState
    {
        LLVector3 direction;
        bool sun_up = false;
    };

    RenderParams getRenderParams(const LLSettingsSky* sky);
    WaterLightState getWaterLightState(const LLEnvironment& environment,
                                       const LLSettingsSky* sky);
    void renderHalo(LLFace* face, LLGLSLShader* shader, const RenderParams& params);
    void renderDisc(LLFace* face, const RenderParams& params);
    void configureDiscShader(LLGLSLShader* shader, const RenderParams& params,
                             bool texture_available);
}

#endif
