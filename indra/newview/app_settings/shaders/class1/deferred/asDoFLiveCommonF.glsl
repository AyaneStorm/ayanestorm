/**
 * @file asDoFLiveCommonF.glsl
 * @author chanayane@firestorm
 * @brief Live DoF (mode 3) shared library: blur radius, CoC bins and the
 *        aperture tap pattern. Linked into every Live DoF program (no main()).
 *
 * Content is split by blur into four bins, front to back: N2 (strong
 * foreground), N1 (foreground), F (focus), B (background). Every bin is kept
 * as premultiplied sums so box filtering (mips) is exact; see
 * doc/ayanestorm-depth-of-field-live-plan.md and
 * scripts/testing/dof_live_reference.py, which this mirrors.
 */

uniform mat4 inv_proj;
uniform float focal_distance;
uniform float blur_constant;
uniform float tan_pixel_angle;
uniform float magnification;
// Largest lens blur radius in full-resolution pixels; the foreground and
// background radii are this times their multipliers.
uniform float max_coc;
uniform float near_radius;
uniform float far_radius;
// N1/N2 boundary, full-resolution pixels: sqrt(2 * near_radius), so each
// near bin spans the same radius ratio (mixed radii stay close in a bin).
uniform float split_radius;

const float LIVE_PI = 3.14159265358979323846;

// Lens circle of confusion at view depth z (negative forward), in
// full-resolution pixels, positive in front of the focus. Same formula as
// asDepthOfFieldCoCF.glsl (Firestorm's thin-lens frontend).
float liveLensCoC(float view_depth)
{
    float coc = (view_depth - focal_distance) / -view_depth * blur_constant;
    coc /= magnification;
    float pixel_length = tan_pixel_angle * -focal_distance;
    return coc / pixel_length * 1.41421356237;
}

// Signed blur radius in full-resolution pixels of a device depth: negative
// in front of the focus (foreground), positive behind it.
float liveBlurRadius(float device_depth)
{
    vec4 p = inv_proj * vec4(0.0, 0.0, device_depth * 2.0 - 1.0, 1.0);
    float coc = -liveLensCoC(p.z / p.w) / max(max_coc, 0.0001);
    coc = clamp(coc, -1.0, 1.0);
    return coc < 0.0 ? coc * near_radius : coc * far_radius;
}

// Bin weights (N2, N1, F, B) of a surface blurred signed_radius full px.
// F keeps the sharp ramp of the other renderers (0.5 to 2 px); the rest
// goes to B behind the focus, or to N1/N2 split softly around split_radius.
vec4 liveBinWeights(float signed_radius)
{
    float a = abs(signed_radius);
    float focus = 1.0 - smoothstep(0.5, 2.0, a);
    if (signed_radius >= 0.0)
    {
        return vec4(0.0, 0.0, focus, 1.0 - focus);
    }
    float strong = smoothstep(0.8 * split_radius, 1.25 * split_radius, a);
    return vec4((1.0 - focus) * strong, (1.0 - focus) * (1.0 - strong), focus, 0.0);
}

// Visibility in front of each bin for one surface split by weights w:
// (V_N1, V_F, V_B); V_N2 is 1.
vec3 liveVisibility(vec4 w)
{
    return vec3(1.0 - w.x, 1.0 - w.x - w.y, 1.0 - w.x - w.y - w.z);
}

// ---------------------------------------------------------------- pixels
//
// One full-resolution pixel split into the four bins, as (S.rgb, W) sums.
// bins_source 0: one surface, diffuseRect at depthMap's depth (the
//   composited image by the depth buffer; the fallback).
// bins_source 1: the transparency bins of Mac OIT's capture (shadowMap0..3,
//   energies in shadowMap4), rescaled as asMacOITResolveF.glsl composites
//   them, plus the opaque surface (diffuseRect, depthMap) weighted by T, the
//   transmittance in front of it. Their sum is the transparency composite.

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;
uniform sampler2D shadowMap0;   // N2 (colour * w, w)
uniform sampler2D shadowMap1;   // N1
uniform sampler2D shadowMap2;   // F
uniform sampler2D shadowMap3;   // B
uniform sampler2D shadowMap4;   // energies (w / r^2 N2, N1; w r B; w / r^2 B)
uniform sampler2D shadowMap5;   // sum of weights, sum of optical depth
uniform int bins_source;
uniform vec2 screen_res;        // full resolution
uniform float gather_scale;     // full-resolution to gather pixels

// bins[0..3] = N2, N1, F, B; energy as shadowMap4.
void liveDecompose(ivec2 p, out vec4 bins[4], out vec4 energy)
{
    vec3 color = max(texelFetch(diffuseRect, p, 0).rgb, vec3(0.0));
    // Depth by position: its target may not match the image's size.
    float signed_radius = liveBlurRadius(texture(depthMap, (vec2(p) + 0.5) / screen_res).r);
    float opaque_weight = 1.0;
    bins[0] = vec4(0.0);
    bins[1] = vec4(0.0);
    bins[2] = vec4(0.0);
    bins[3] = vec4(0.0);
    energy = vec4(0.0);
    if (bins_source != 0)
    {
        vec2 weight_depth = texelFetch(shadowMap5, p, 0).xy;
        float transmittance = exp(-weight_depth.y);
        // Mac OIT composites sum(c w) / sum(w) * (1 - T): the same scale
        // makes the bins add up to exactly that.
        float scale = weight_depth.x > 0.0 ? (1.0 - transmittance) / weight_depth.x : 0.0;
        bins[0] = texelFetch(shadowMap0, p, 0) * scale;
        bins[1] = texelFetch(shadowMap1, p, 0) * scale;
        bins[2] = texelFetch(shadowMap2, p, 0) * scale;
        bins[3] = texelFetch(shadowMap3, p, 0) * scale;
        energy = texelFetch(shadowMap4, p, 0) * scale;
        opaque_weight = transmittance;
    }
    vec4 w = liveBinWeights(signed_radius) * opaque_weight;
    float r = max(abs(signed_radius) * gather_scale, 0.5);
    float inv_r2 = 1.0 / (r * r);
    bins[0] += vec4(color * w.x, w.x);
    bins[1] += vec4(color * w.y, w.y);
    bins[2] += vec4(color * w.z, w.z);
    bins[3] += vec4(color * w.w, w.w);
    energy += vec4(w.x * inv_r2, w.y * inv_r2, w.w * r, w.w * inv_r2);
}

// Visibility in front of N1, F and B of a decomposed pixel.
vec3 liveBinVisibility(vec4 bins[4])
{
    return vec3(1.0 - bins[0].a, 1.0 - bins[0].a - bins[1].a,
                1.0 - bins[0].a - bins[1].a - bins[2].a);
}

// ---------------------------------------------------------------- taps
//
// A centre tap plus rings k = 1..n of 6k taps at aperture-space distance
// k s, s = R / (n + 1/2). In aperture space every ring tap stands for an
// area of pi s^2 / 3 and the centre for pi s^2 / 4: together they tile the
// disc of radius R. Polygonal apertures scale each tap's image offset and
// area by the boundary b(angle)^2, anamorphic squeezes x.

uniform int aperture_blades;
uniform float aperture_roundness;
uniform float aperture_rotation;
uniform float anamorphic_ratio;
// ASDoFAperture::unitArea(): image area of the unit aperture.
uniform float unit_area;

// Aperture boundary radius at polar angle (before rotation); a blade vertex
// lies at angle 0, as in the aperture-sampled renderer (ASDoFAperture).
float liveBoundary(float angle)
{
    if (aperture_blades < 3)
    {
        return 1.0;
    }
    float sector = 2.0 * LIVE_PI / float(aperture_blades);
    float local_angle = mod(angle, sector) - 0.5 * sector;
    float polygon = cos(0.5 * sector) / max(cos(local_angle), 0.001);
    return mix(polygon, 1.0, aperture_roundness);
}

// Image offset (gather pixels) of a tap at angle and aperture-space
// distance; the boundary is returned for the tap's area.
vec2 liveTapOffset(float angle, float distance, out float boundary)
{
    boundary = liveBoundary(angle);
    float a = angle + aperture_rotation;
    return vec2(cos(a) * anamorphic_ratio, sin(a)) * (distance * boundary);
}

// Share of a tap's area a source of radius r reaches: the part of the
// annulus [d - s/2, d + s/2] (centre tap: the disc of radius s/2) inside r,
// so the taps integrate the source's whole disc for any r.
float liveReach(float r, float d, float s)
{
    if (d <= 0.0)
    {
        return clamp(r * r / (0.25 * s * s), 0.0, 1.0);
    }
    float inner = d - 0.5 * s;
    return clamp((r * r - inner * inner) / (2.0 * d * s), 0.0, 1.0);
}
