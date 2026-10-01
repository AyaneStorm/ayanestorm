/**
 * @file asDoFLiveCommonF.glsl
 * @author chanayane@firestorm
 * @brief Live DoF (mode 3) shared library: blur radius, CoC bins and the
 *        aperture tap pattern. Linked into every Live DoF program (no main()).
 *
 * Content is split by blur into five bins, front to back: N2 (strong
 * foreground), N1 (foreground), F (focus), B1 (near background), B2 (far
 * background). Every bin is kept as premultiplied sums so box filtering
 * (mips) is exact; see
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
// B1/B2 boundary, likewise sqrt(2 * far_radius). One background bin mixed
// nearly sharp hair with the far background behind it: the far background
// was blurred by the hair's radius between strands, and spread over them.
uniform float far_split_radius;

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

// Bin weights (N2, N1, F, B1) of a surface blurred signed_radius full px;
// B2 takes the rest, 1 - their sum. F keeps the sharp ramp of the other
// renderers (0.5 to 2 px); the rest is split softly in two around
// split_radius in front of the focus, around far_split_radius behind it.
vec4 liveBinWeights(float signed_radius)
{
    float a = abs(signed_radius);
    float focus = 1.0 - smoothstep(0.5, 2.0, a);
    bool behind = signed_radius >= 0.0;
    float split = behind ? far_split_radius : split_radius;
    float strong = (1.0 - focus) * smoothstep(0.8 * split, 1.25 * split, a);
    float weak = 1.0 - focus - strong;
    return behind ? vec4(0.0, 0.0, focus, weak) : vec4(strong, weak, focus, 0.0);
}

// ---------------------------------------------------------------- pixels
//
// One full-resolution pixel split into the five bins, as (S.rgb, W) sums.
// bins_source 0: one surface, diffuseRect at depthMap's depth (the
//   composited image by the depth buffer; the fallback).
// bins_source 1: the transparency bins of Mac OIT's capture (shadowMap0..4,
//   energies in shadowMap5), rescaled as asMacOITResolveF.glsl composites
//   them, plus the opaque surface (diffuseRect, depthMap) weighted by T, the
//   transmittance in front of it. Their sum is the transparency composite.

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;
uniform sampler2D shadowMap0;   // N2 (colour * w, w)
uniform sampler2D shadowMap1;   // N1
uniform sampler2D shadowMap2;   // F
uniform sampler2D shadowMap3;   // B1
uniform sampler2D shadowMap4;   // B2
uniform sampler2D shadowMap5;   // energies (w / r^2 N2, N1, B1; w r B2)
uniform sampler2D positionMap;  // sum of weights, sum of optical depth
uniform int bins_source;
uniform vec2 screen_res;        // full resolution
uniform float gather_scale;     // full-resolution to gather pixels

// bins[0..4] = N2, N1, F, B1, B2; energy as shadowMap5. B2 keeps its mean
// radius M only: its energy is W^3 / M^2 (liveFarEnergy()), exact for one
// radius and close within the bin's small radius ratio.
void liveDecompose(ivec2 p, out vec4 bins[5], out vec4 energy)
{
    vec3 color = max(texelFetch(diffuseRect, p, 0).rgb, vec3(0.0));
    // Depth by position: its target may not match the image's size.
    float signed_radius = liveBlurRadius(texture(depthMap, (vec2(p) + 0.5) / screen_res).r);
    float opaque_weight = 1.0;
    bins[0] = vec4(0.0);
    bins[1] = vec4(0.0);
    bins[2] = vec4(0.0);
    bins[3] = vec4(0.0);
    bins[4] = vec4(0.0);
    energy = vec4(0.0);
    if (bins_source != 0)
    {
        vec2 weight_depth = texelFetch(positionMap, p, 0).xy;
        float transmittance = exp(-weight_depth.y);
        // Mac OIT composites sum(c w) / sum(w) * (1 - T): the same scale
        // makes the bins add up to exactly that.
        float scale = weight_depth.x > 0.0 ? (1.0 - transmittance) / weight_depth.x : 0.0;
        bins[0] = texelFetch(shadowMap0, p, 0) * scale;
        bins[1] = texelFetch(shadowMap1, p, 0) * scale;
        bins[2] = texelFetch(shadowMap2, p, 0) * scale;
        bins[3] = texelFetch(shadowMap3, p, 0) * scale;
        bins[4] = texelFetch(shadowMap4, p, 0) * scale;
        energy = texelFetch(shadowMap5, p, 0) * scale;
        opaque_weight = transmittance;
    }
    vec4 shares = liveBinWeights(signed_radius);
    vec4 w = shares * opaque_weight;
    float far_weight = max(1.0 - dot(shares, vec4(1.0)), 0.0) * opaque_weight;
    float r = max(abs(signed_radius) * gather_scale, 0.5);
    float inv_r2 = 1.0 / (r * r);
    bins[0] += vec4(color * w.x, w.x);
    bins[1] += vec4(color * w.y, w.y);
    bins[2] += vec4(color * w.z, w.z);
    bins[3] += vec4(color * w.w, w.w);
    bins[4] += vec4(color * far_weight, far_weight);
    energy += vec4(w.x * inv_r2, w.y * inv_r2, w.w * inv_r2, far_weight * r);
}

// Energy sum w / r^2 of B2 sums with weight W and radius moment M.
float liveFarEnergy(float weight, float moment)
{
    return moment > 0.0 ? weight * weight * weight / (moment * moment) : 0.0;
}

// ---------------------------------------------------------------- reads
//
// Every layer is read "alone", completed where nearer bins hide it, by the
// push-pull recurrence
//     c(l) = S + (1 - V) (S + k c(l + 1)) / (V + k),
// ending with S / V at the top level (scripts/testing/dof_live_reference.py,
// read_completed()). What the bin shows (V = 1) is read exactly, in one
// fetch. What is hidden takes the visible part's own density S / V, blended
// continuously toward the coarser levels as less is visible: a hole (V = 0)
// fills from the content around it, and a surface split softly between two
// bins (on a blur ramp) stays opaque in each. Plain push-pull
// (c(l) = S + (1 - V) c(l + 1)) averaged the empty space around an object
// into its hidden part: the far background leaked through the avatar as a
// grey veil in squares (coarse mip texels). A threshold rule ("first level
// where half the footprint shows the bin") fails in any hole as large as
// the visible part.

// Stop once the hidden share left is below this.
const float LIVE_COMPLETE_EPSILON = 0.01;
// k: weight of the coarser estimate against the level's own density.
const float LIVE_COMPLETE_PRIOR = 0.05;

// layer: (S.rgb, W) mips; energy_map: its energy mips (all four channels
// completed alike); vis_map .channel: the visibility in front of the bin,
// channel < 0 for the front bin (always visible). Returns false where no
// level shows anything of the bin.
bool liveCompleted(sampler2D layer, sampler2D energy_map, sampler2D vis_map,
                   int vis_channel, vec2 uv, float lod, float max_lod,
                   out vec4 value, out vec4 energy)
{
    value = vec4(0.0);
    energy = vec4(0.0);
    float hidden = 1.0;
    for (int step = 0; step <= 16; ++step)
    {
        float level = min(lod + float(step), max_lod);
        bool last = level >= max_lod;
        float visibility = vis_channel < 0 ? 1.0 :
            textureLod(vis_map, uv, level)[vis_channel];
        float density = 1.0 / (visibility + LIVE_COMPLETE_PRIOR);
        // The top level normalizes what is left by its own visibility.
        float share = last ? (visibility > 0.000001 ? hidden / visibility : 0.0) :
                             hidden * (1.0 + LIVE_COMPLETE_PRIOR) * density;
        value += textureLod(layer, uv, level) * share;
        energy += textureLod(energy_map, uv, level) * share;
        hidden *= LIVE_COMPLETE_PRIOR * (1.0 - visibility) * density;
        if (last || hidden <= LIVE_COMPLETE_EPSILON)
        {
            break;
        }
    }
    return value.a > 0.000001;
}

// Visibility in front of N1, F, B1 and B2 of a decomposed pixel.
vec4 liveBinVisibility(vec4 bins[5])
{
    float n1 = 1.0 - bins[0].a;
    float focus = n1 - bins[1].a;
    float back1 = focus - bins[2].a;
    return vec4(n1, focus, back1, back1 - bins[3].a);
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

// Integral of boundary^2 / 2 within a blade, centred on its side normal.
// Same primitive as ASDoFAperture::unitArea(). A boundary sample at the
// tap angle underestimates a hexagon's first ring by about 9%, exposing
// B2 through an opaque B1 in tile-aligned squares (reference model).
float liveBladeAreaPrimitive(float local_angle, float half_sector)
{
    float a = (1.0 - aperture_roundness) * cos(half_sector);
    float b = aperture_roundness;
    float tangent = tan(local_angle);
    return 0.5 * (a * a * tangent +
                  2.0 * a * b * log(1.0 / cos(local_angle) + tangent) +
                  b * b * local_angle);
}

// Cumulative unit-aperture area from angle 0, continued across blades.
// Rotation changes offsets only; squeeze scales all areas alike.
float liveApertureAreaTo(float angle)
{
    if (aperture_blades < 3 || aperture_roundness >= 1.0)
    {
        return 0.5 * anamorphic_ratio * angle;
    }
    float sector = 2.0 * LIVE_PI / float(aperture_blades);
    float blade = floor(angle / sector);
    float local_angle = angle - blade * sector - 0.5 * sector;
    float blade_area = unit_area / float(aperture_blades);
    return (blade + 0.5) * blade_area +
           anamorphic_ratio * liveBladeAreaPrimitive(local_angle, 0.5 * sector);
}

// Exact area of the angular sector represented by one ring tap. These
// sectors partition each annulus, so any uniform source radius <= kernel
// keeps its coverage, including sources that reach only the inner rings.
float liveTapSectorArea(float angle, float half_angle)
{
    return liveApertureAreaTo(angle + half_angle) -
           liveApertureAreaTo(angle - half_angle);
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
