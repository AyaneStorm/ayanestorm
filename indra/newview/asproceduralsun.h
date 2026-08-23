/**
 * @file asproceduralsun.h
 * @author chanayane@firestorm
 * @brief Viewer-local procedural sunset sun-disc controls.
 */

#ifndef AS_PROCEDURAL_SUN_H
#define AS_PROCEDURAL_SUN_H

#include "v3color.h"

class LLSettingsSky;

namespace ASProceduralSun
{
    struct RenderParams
    {
        bool enabled = false;
        F32 opacity = 0.f;
        F32 horizon_factor = 0.f;
        LLColor3 color;
        LLColor3 limb_color;
    };

    RenderParams getRenderParams(const LLSettingsSky* sky);
}

#endif
