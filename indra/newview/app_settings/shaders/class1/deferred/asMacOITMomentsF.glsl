// AyaneStorm OIT shader. Author: chanayane@firestorm.
/**
 * @file asMacOITMomentsF.glsl
 * @brief Mac OIT moment reconstruction library (GLSL 4.10, no main()).
 *
 * Linked into the capture programs and the merge program so both evaluate
 * the identical function. Implements the four-power-moment reconstruction of
 * Moment-Based Order-Independent Transparency (Muenstermann, Krumpen, Klein,
 * Peters, 2018): the Hamburger bound on the fraction of a pixel's optical
 * depth lying in front of a warped depth z, interpolated between its lower
 * and upper bound by MACOIT_OVERESTIMATION.
 *
 * Inputs are the pixel's normalized, biased moments b1 = E[z], b2 = E[z^2]
 * and the LDL^T factorization of the Hankel matrix
 *
 *     B = | 1  b1 b2 |     L = | 1   0   0 |    D = diag(1, D11, D22)
 *         | b1 b2 b3 |         | b1  1   0 |
 *         | b2 b3 b4 |         | b2  L21 1 |
 *
 * which the merge pass computes once per pixel (asMacOITMergeF.glsl), so a
 * fragment pays only the solve below.
 */

// Weight given to the query point itself: 0 is the lower bound (nothing at
// z counts as in front), 1 the upper bound. The paper's recommended value.
const float MACOIT_OVERESTIMATION = 0.25;

float macoit_absorbance_fraction(float z, float b1, float b2, float l21,
                                 float inv_d11, float inv_d22)
{
    // Solve B c = (1, z, z^2) with the stored factorization:
    // forward substitution, diagonal scaling, backward substitution.
    vec3 c = vec3(1.0, z, z * z);
    c.y -= b1;
    c.z -= b2 + l21 * c.y;
    c.y *= inv_d11;
    c.z *= inv_d22;
    c.y -= l21 * c.z;
    c.x -= c.y * b1 + c.z * b2;

    // The other two support points of the canonical representation are the
    // roots of c.x + c.y x + c.z x^2.
    float inv_c2 = 1.0 / c.z;
    float p = c.y * inv_c2;
    float q = c.x * inv_c2;
    float r = sqrt(max(0.25 * p * p - q, 0.0));
    float z1 = -0.5 * p - r;
    float z2 = -0.5 * p + r;

    // Quadratic through (z, overestimation), (z1, z1 < z), (z2, z2 < z) in
    // Newton form, expanded to monomials. Applying the moment functional to
    // it sums the canonical weights of the support points in front of z.
    float f0 = MACOIT_OVERESTIMATION;
    float f1 = z1 < z ? 1.0 : 0.0;
    float f2 = z2 < z ? 1.0 : 0.0;
    float f01 = (f1 - f0) / (z1 - z);
    float f12 = (f2 - f1) / (z2 - z1);
    float f012 = (f12 - f01) / (z2 - z);
    float p2 = f012;
    float p1 = f01 - f012 * (z + z1);
    float p0 = f0 - f01 * z + f012 * z * z1;
    float fraction = p0 + p1 * b1 + p2 * b2;

    // Coincident support points divide by zero. A NaN would poison the
    // additive accumulation for the whole pixel, so it maps to "nothing in
    // front" instead; every comparison with NaN is false.
    return fraction > 0.0 ? min(fraction, 1.0) : 0.0;
}
