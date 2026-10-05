/**
 * @file asdofaperture.h
 * @author chanayane@firestorm
 * @brief Lens-position samples for aperture-sampled depth of field.
 *
 * Equal-weight, deterministic, nested samples (prefixes of an
 * Owen-scrambled Sobol sequence: dimensions 0/1 lens, 2/3 pixel jitter,
 * 4 wavelength) with uniform area density over a circular or
 * rounded-polygon aperture, so brightness stays normalized for every shape
 * and sample count. Each sample also carries a box-filter pixel jitter so
 * sub-pixel strands integrate over the pixel footprint jointly with the
 * lens. Lens output is unit circumradius; scale with
 * ASDoFCamera::lensOffset(). Mirrored and tested by
 * scripts/testing/dof_reference.py (viewer_aperture_samples,
 * viewer_pixel_jitter).
 */
#ifndef AS_DOF_APERTURE_H
#define AS_DOF_APERTURE_H

#include <vector>

#include "stdtypes.h"
#include "glm/vec2.hpp"
#include "glm/vec3.hpp"

namespace ASDoFAperture
{
    struct Shape
    {
        S32 mBlades = 0;        // below 3: circular
        F32 mRoundness = 1.f;   // 0 polygon .. 1 circle
        F32 mRotation = 0.f;    // radians
        F32 mAnamorphic = 1.f;  // horizontal scale
    };

    // Reads ASDepthOfFieldAperture* / ASDepthOfFieldAnamorphicRatio with the
    // same clamps as the legacy renderer.
    Shape shapeFromSettings();

    // Replaces both outputs with the first count samples; count N is a
    // prefix of 2N. pixel_jitter is in output pixels, [-0.5, 0.5); apply
    // with ASDoFCamera::jitterProjection().
    void generate(const Shape& shape, U32 count, std::vector<glm::vec2>& lens,
                  std::vector<glm::vec2>& pixel_jitter);

    // Axial (longitudinal) chromatic aberration. Each lens sample also
    // stands for a wavelength: spectralCoordinate() maps the sample index
    // to s in [-1, 1) (blue -1, green 0, red +1) from its own Sobol
    // dimension, decorrelated from the lens and jitter dimensions and
    // nested like them.
    F32 spectralCoordinate(U32 index);

    // RGB weights of a sample at s: red 1+s, green 1.5(1-s^2), blue 1-s.
    // Each averages to exactly 1 over uniform s, so in-focus content stays
    // neutral and brightness is preserved.
    glm::vec3 spectralWeights(F32 s);

    // Normalized pupil radius squared of sample index (radius over the
    // aperture edge at its angle; Sobol dimension 1): uniform in
    // [0, 1), so any weight linear
    // in it averages exactly as over the aperture area. Used by spherical
    // aberration.
    F32 pupilRadius2(U32 index);

    // Area of the unit-circumradius aperture, anamorphic scale included
    // (pi for a circle). The advanced renderer divides a highlight sprite's
    // energy by unitArea * R^2 so its brightness is shape-independent.
    F32 unitArea(const Shape& shape);

    // Edge radius of the unit-circumradius aperture at a polar angle >= 0,
    // rotation and anamorphic scale excluded (1 for a circle).
    F64 boundaryAt(const Shape& shape, F64 angle);

    // Aperture area swept from polar angle 0 to angle, anamorphic scale
    // included, continued across blades and below 0: one whole turn adds
    // unitArea(). The Live renderer's per-frame tap table takes its sector
    // areas from it (asDoFLiveCommonF.glsl, liveApertureAreaTo()).
    F64 areaTo(const Shape& shape, F64 angle);
}

#endif
