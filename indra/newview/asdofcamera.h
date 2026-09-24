/**
 * @file asdofcamera.h
 * @author chanayane@firestorm
 * @brief Thin-lens camera math for aperture-sampled depth of field.
 *
 * Single place for lens units and per-sample matrices. Units are metres;
 * lens offsets are GL eye-space (right, up). Each lens sample translates
 * the eye parallel to the image plane and shears the projection so the
 * focal plane (eye z == -focus) stays registered; no toe-in rotation.
 * Mirrored and tested by scripts/testing/dof_reference.py (viewer_*).
 */
#ifndef AS_DOF_CAMERA_H
#define AS_DOF_CAMERA_H

#include "stdtypes.h"
#include "glm/mat4x4.hpp"
#include "glm/vec2.hpp"

namespace ASDoFCamera
{
    struct Lens
    {
        F32 mFocusDistance = 0.f;  // metres along the view axis
        F32 mFocalLength = 0.f;    // metres, zoom-adjusted
        F32 mFNumber = 0.f;
        F32 mApertureRadius = 0.f; // metres, focal length / (2 * f-number)
        F32 mFovY = 0.f;           // radians, current vertical FOV
        bool mValid = false;       // false: inputs rejected, caller must not sample
    };

    // Zoom mapping matches LLPipeline::renderDoF: the default FOV and focal
    // length (mm) define a fixed sensor height. Requires focus > focal length.
    // Invalid input yields mValid == false; nothing is clamped.
    Lens makeLens(F32 fov_y, F32 default_fov_y, F32 default_focal_length_mm,
                  F32 f_number, F32 focus_distance);

    // Scales a unit-disk sample to a lens offset in metres.
    glm::vec2 lensOffset(const Lens& lens, const glm::vec2& unit_disk_sample);

    // Eye translated by offset: returns T(-offset) * modelview.
    glm::mat4 lensModelview(const glm::mat4& modelview, const glm::vec2& offset);

    // Off-axis shear of a perspective projection (P[2][3] == -1) keeping
    // eye z == -focus_distance registered after lensModelview().
    // Depth mapping is unchanged.
    glm::mat4 lensProjection(const glm::mat4& projection, const glm::vec2& offset,
                             F32 focus_distance);

    // Shifts the image by jitter_px output pixels at every depth (pixel
    // footprint sampling). Commutes with lensProjection().
    glm::mat4 jitterProjection(const glm::mat4& projection, const glm::vec2& jitter_px,
                               F32 viewport_width, F32 viewport_height);

    // Image-space radius (pixels) of a point at depth metres for the
    // aperture rim. Diagnostics and sample-count planning only.
    F32 cocRadiusPixels(const Lens& lens, F32 depth, F32 viewport_height_px);
}

#endif
