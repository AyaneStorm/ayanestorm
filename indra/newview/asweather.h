/**
 * @file asweather.h
 * @author chanayane@ayanestorm
 * @brief Shared viewer-local weather rendering and shelter resources.
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
