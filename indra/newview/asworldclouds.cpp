/**
 * @file asworldclouds.cpp
 * @brief AyaneStorm world-fixed SL clouds (Exact volumetric lighting).
 * @author chanayane@firestorm
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (c) 2026 Chanayane @ Second Life
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "asworldclouds.h"

#include "asvolumetriclighting.h"
#include "llagentcamera.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "llsky.h"
#include "llviewercontrol.h"
#include "llvosky.h"
#include "pipeline.h"

#include <cmath>

extern LLPointer<LLImageGL> gEXRImage;

// Maths (cloudsV.glsl, LLVOWLSky::buildStripsBuffer()): uv1 =
// (vec2(-texcoord0.x, texcoord0.y) - 0.5) / cloud_scale + 0.5 with
// texcoord0 = ((-dir.z + 1) / 2, (-dir.x + 1) / 2), dome x = north,
// z = east. On the physical sphere (radius R, centre below the viewer)
// dir.z = east / R and dir.x = north / R of the cloud point relative to the
// viewer, so d uv1 = (d east, -d north) / (2 R cloud_scale). Adding the
// camera's motion mapped this way to uv1 keeps every cloud at its world
// position. uv2 = uv1 + c, uv3 = 16 uv1, uv4 = 16 uv2: one offset moves
// them all. Every cloudsF.glsl lookup (uv1/8, 17 uv1/16, 17 uv1/4,
// 17 uv1/8, uv1..uv4) repeats when the offset grows by 16, so it is wrapped
// to [0, 16) in F64: invisible, and small floats in the shader.
//
// Incremental (camera motion), not global position / (2 R s): global
// positions are millions of metres, so a cloud_scale change during a
// windlight blend would make the clouds race.

namespace
{
constexpr F64 OFFSET_PERIOD = 16.0;
// Larger jumps (teleports) are not accumulated: the clouds just continue.
constexpr F64 MAX_CAMERA_STEP = 5000.0;

F64 wrap_offset(F64 value)
{
    value = fmod(value, OFFSET_PERIOD);
    return value < 0.0 ? value + OFFSET_PERIOD : value;
}
}

U32 ASWorldClouds::sFrame = U32_MAX;
bool ASWorldClouds::sActive = false;
bool ASWorldClouds::sCameraValid = false;
LLVector3d ASWorldClouds::sCameraGlobal;
F64 ASWorldClouds::sOffsetX = 0.0;
F64 ASWorldClouds::sOffsetY = 0.0;
F64 ASWorldClouds::sUVPerMetre = 0.0;

bool ASWorldClouds::skyHasClouds(LLPipeline& pipeline)
{
    // Same conditions under which LLDrawPoolWLSky::renderSkyCloudsDeferred()
    // draws clouds (none with an HDRI sky, see use_hdri_sky() there).
    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    static LLCachedControl<F32> hdri_split(gSavedSettings, "RenderHDRISplitScreen", 1.f);
    return sky && !(gEXRImage.notNull() && hdri_split > 0.f) &&
           pipeline.canUseWindLightShaders() &&
           pipeline.hasRenderType(LLPipeline::RENDER_TYPE_CLOUDS) &&
           gSky.mVOSkyp && gSky.mVOSkyp->getCloudNoiseTex() &&
           sky->getCloudScale() >= 0.001f;
}

F32 ASWorldClouds::layerHeight()
{
    static LLCachedControl<F32> height(gSavedSettings, "ASCloudLayerHeight", 500.f);
    return llclamp((F32)height, 250.f, 4000.f);
}

F32 ASWorldClouds::radius(const LLSettingsSky& sky)
{
    const F32 dome_offset = llclamp(sky.getDomeOffset(), 0.f, 0.999f);
    return layerHeight() / (1.f - dome_offset);
}

void ASWorldClouds::update()
{
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame == sFrame)
    {
        return;
    }
    sFrame = frame;

    sActive = ASVolumetricLighting::isEnabled() && ASVolumetricLighting::isExactMode() &&
              skyHasClouds(gPipeline);
    if (!sActive)
    {
        // Vanilla camera-attached clouds; restart from them when re-enabled.
        sCameraValid = false;
        sOffsetX = 0.0;
        sOffsetY = 0.0;
        return;
    }

    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    sUVPerMetre = 1.0 / (2.0 * (F64)radius(*sky) * (F64)sky->getCloudScale());

    // The main view's camera: LLViewerCamera's origin moves to the probe
    // during reflection probe passes.
    const LLVector3d camera = gAgentCamera.getCameraPositionGlobal();
    if (sCameraValid)
    {
        const F64 east = camera.mdV[VX] - sCameraGlobal.mdV[VX];
        const F64 north = camera.mdV[VY] - sCameraGlobal.mdV[VY];
        if (east * east + north * north < MAX_CAMERA_STEP * MAX_CAMERA_STEP)
        {
            sOffsetX = wrap_offset(sOffsetX + east * sUVPerMetre);
            sOffsetY = wrap_offset(sOffsetY - north * sUVPerMetre);
        }
    }
    sCameraGlobal = camera;
    sCameraValid = true;
}

LLVector2 ASWorldClouds::offsetAt(const LLVector3d& global)
{
    update();
    if (!sActive)
    {
        return LLVector2::zero;
    }
    const F64 east = global.mdV[VX] - sCameraGlobal.mdV[VX];
    const F64 north = global.mdV[VY] - sCameraGlobal.mdV[VY];
    return LLVector2((F32)wrap_offset(sOffsetX + east * sUVPerMetre),
                     (F32)wrap_offset(sOffsetY - north * sUVPerMetre));
}

void ASWorldClouds::applyUniforms(LLGLSLShader& shader)
{
    update();
    static LLStaticHashedString offset_name("as_cloud_world_offset");
    shader.uniform2f(offset_name, sActive ? (F32)sOffsetX : 0.f, sActive ? (F32)sOffsetY : 0.f);
}
