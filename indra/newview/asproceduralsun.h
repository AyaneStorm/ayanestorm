/**
 * @file asproceduralsun.h
 * @author chanayane@ayanestorm
 * @brief Viewer-local procedural sunset sun-disc controls.
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
