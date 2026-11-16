/**
 * @file asweathersnow.h
 * @author chanayane@ayanestorm
 * @brief Stateful world-space snow effect for ASWeather.
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
