/**
 * asrjointpose.h
 * Shape-preserving joint prediction, shared by pose upload and portable checks.
 * Author: chanayane@firestorm
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
