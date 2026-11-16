/**
 * @file aswaterhorizonfog.cpp
 * @author chanayane@ayanestorm
 * @brief AyaneStorm water-horizon fog uniform setup.
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

#include "llviewerprecompiledheaders.h"

#include "aswaterhorizonfog.h"

#include "llenvironment.h"
#include "llglslshader.h"
#include "llmath.h"
#include "llsettingssky.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"

namespace
{
    const LLStaticHashedString sHorizonFogStrength("as_horizon_fog_strength");
    const LLStaticHashedString sHorizonFogIntensity("as_horizon_fog_intensity");
}

void ASWaterHorizonFog::uploadUniforms(LLGLSLShader& shader)
{
    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const F32 water_height = LLEnvironment::instance().getWaterHeight();
    const F32 camera_height = LLViewerCamera::getInstance()->getOrigin().mV[VZ];

    F32 horizon_dip = 0.f;
    if (sky)
    {
        const F32 radius = llmax(sky->getDomeRadius(), 1.f);
        const F32 height = llmax(camera_height - water_height, 0.f);
        horizon_dip = llmin(acosf(radius / (radius + height)), 12.f * DEG_TO_RAD);
    }

    F32 sun_fade = 0.f;
    if (sky)
    {
        const F32 elevation = asinf(llclamp(sky->getSunDirection().mV[VZ], -1.f, 1.f)) * RAD_TO_DEG
                            + horizon_dip * RAD_TO_DEG;
        const F32 start = llclamp(gSavedSettings.getF32("ASHorizonScatteringStartElevation"), 0.f, 45.f);
        const F32 daylight_fade = start > 0.f
                                ? llclamp(1.f - elevation / start, 0.f, 1.f)
                                : (elevation <= 0.f ? 1.f : 0.f);
        const F32 twilight_fade = llclamp((elevation + 12.f) / 7.f, 0.f, 1.f);
        sun_fade = daylight_fade * twilight_fade;
    }

    static LLCachedControl<F32> fog_strength(gSavedSettings, "ASWaterHorizonFogStrength", 0.f);
    static LLCachedControl<F32> fog_intensity(gSavedSettings, "ASWaterHorizonFogIntensity", 0.5f);
    static LLCachedControl<bool> enabled(gSavedSettings, "ASHorizonScatteringEnabled", false);

    shader.uniform1f(sHorizonFogStrength,
        enabled() ? llclamp(fog_strength(), 0.f, 4.f) : 0.f);
    shader.uniform1f(sHorizonFogIntensity,
        enabled() ? llclamp(fog_intensity(), 0.f, 1.f) * sun_fade : 0.f);
}
