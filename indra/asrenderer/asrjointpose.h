/**
 * @file asrjointpose.h
 * asrjointpose.h
 * Shape-preserving joint prediction, shared by pose upload and portable checks.
 * Author: chanayane@ayanestorm
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
#pragma once

namespace asr
{

// Shortest world-space rotation from previous to newest; singular/reflected changes
// use identity. The GPU applies its fractional power to the newest affine transform.
void jointRotationDelta(const float newest[3][4], const float previous[3][4], float quaternion[4]);

// CPU reference of the GPU predictor. Translation continues linearly; the latest
// scale and shear stay intact while rotation follows a unit quaternion.
void extrapolateJointRows(const float newest[3][4], const float previous[3][4],
                          const float quaternion[4], float lead, float result[3][4]);

} // namespace asr
