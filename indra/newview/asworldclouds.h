/**
 * @file asworldclouds.h
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

#ifndef AS_WORLDCLOUDS_H
#define AS_WORLDCLOUDS_H

#include "v2math.h"
#include "v3dmath.h"

class LLGLSLShader;
class LLPipeline;
class LLSettingsSky;

// SL paints its clouds on a sky dome attached to the camera: they look the
// same wherever the camera goes. While Exact volumetric lighting renders
// cloud shadows, this offsets the cloud UVs by the camera's horizontal
// motion so the drawn clouds stay fixed in the world (with parallax), and
// the cloud shadow bake applies the same offset, so drawn clouds, cloud
// shadows and cloud rays match anywhere. Colours and windlight parameters
// are untouched. See doc/ayanestorm-world-fixed-clouds-plan.md.
class ASWorldClouds
{
public:
    // Sky draws clouds (same tests as LLDrawPoolWLSky::renderSkyCloudsDeferred()).
    static bool skyHasClouds(LLPipeline& pipeline);

    // ASCloudLayerHeight: cloud layer height above the viewer (metres).
    static F32 layerHeight();

    // Physical dome sphere radius: cap top layerHeight() above the viewer,
    // centre dome_offset radii below it.
    static F32 radius(const LLSettingsSky& sky);

    // Cloud uv1 offset for a viewer at a global position (the camera, or the
    // cloud shadow bake's anchor). Zero when inactive.
    static LLVector2 offsetAt(const LLVector3d& global);

    // Sets as_cloud_world_offset for the camera on a bound cloud shader.
    static void applyUniforms(LLGLSLShader& shader);

private:
    // Once per frame: accumulates the camera motion while active.
    static void update();

    static U32 sFrame;
    static bool sActive;
    static bool sCameraValid;
    static LLVector3d sCameraGlobal; // camera the offset refers to
    static F64 sOffsetX;             // uv1 offset, wrapped to [0, 16)
    static F64 sOffsetY;
    static F64 sUVPerMetre;          // 1 / (2 R cloud_scale)
};

#endif // AS_WORLDCLOUDS_H
