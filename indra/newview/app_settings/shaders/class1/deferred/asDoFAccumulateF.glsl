/**
 * @file asDoFAccumulateF.glsl
 * @author chanayane@firestorm
 * @brief Aperture-sampled DoF: weighted copy of one resolved linear-HDR
 * lens sample (RGB plus glow in alpha). Additive blending sums samples;
 * the same shader with weight 1 and no residual writes the average back.
 *
 * Residual blur: N lens samples image an out-of-focus point as N separate
 * dots spaced about coc_radius * sqrt(pi / N) pixels apart. Each sample is
 * softened by a small disk of that radius (from this sample's own depth),
 * which fills the aperture shape instead of stippling it. Zero at the
 * focal plane; shrinks as N grows.
 *
 * Axial chromatic aberration: a sample standing for one wavelength is
 * weighted per channel (inv_focus is then that wavelength's focus).
 *
 * Lens character (per-pixel sample weights, no extra render):
 * - Cat's eye: off-axis, the lens barrel clips the aperture. A sample only
 *   reaches a pixel inside a unit circle shifted by cat_eye * field
 *   position (1 at the frame corner); the overlap is the lemon-shaped
 *   pupil. mode 2 divides the average by the analytic open fraction.
 * - Spherical aberration: weight 1 - sa * sigma * (2 rho^2 - 1), rho^2 the
 *   sample's uniform normalized pupil radius^2 (mean weight 1), sigma the
 *   eased sign of defocus at the pixel (0 at the focal plane).
 * - Bright highlights (artistic, off by default): real point lights are far
 *   brighter than lit surfaces and stay bright when spread over a bokeh
 *   disc; viewer lights are not. Isolated bright pixels (much brighter than
 *   a surrounding ring) gain energy growing with their blur disc area:
 *   1 + strength * bright * isolated * min((coc / 4)^2, 1024). In-focus
 *   pixels and large bright areas are unchanged.
 *
 * Final smoothing (lens_mode 3 and 4, on the average before display): after
 * N samples an out-of-focus point is still N dots about
 * coc * sqrt(pi / N) apart, and point taps cannot spread a dot smaller
 * than their spacing. Mode 3 writes the normalized average premultiplied
 * by a defocus mask into a mipmapped target; mode 4 gathers a disk of that
 * radius from it, each tap (the centre too) reading a mip level whose
 * texels overlap the tap's share of the disk, so every dot is spread over
 * the whole area.
 * The mask keeps sharp pixels (in-focus hair) out of the mips, the same
 * scatter-as-gather and occlusion rules as the residual keep defocused
 * content off nearer edges, and in-focus pixels are left unchanged. The
 * result is used only inside bokeh shapes of small sources, located by
 * the source map (mode 5 summed over the samples: where each sample's
 * source dots landed); elsewhere the sampled image stays as is.
 * Depth is the last rendered view's: at the focal plane it matches every
 * sample; elsewhere it is off by less than the blur the radius follows.
 */
in vec2 tc;

uniform sampler2D diffuseMap;
uniform sampler2D altDiffuseMap; // mode 4: mipmapped masked average from mode 3
uniform sampler2D specularMap;   // mode 4: mipmapped source map (sum of mode 5 over the samples)
uniform sampler2D depthMap;
uniform vec4 sample_weight;    // per channel: axial CA colour weights, glow 1
uniform float residual_scale;  // pixels per unit |1/focus - 1/distance|; 0 disables
                               // (modes 3 and 4: the final smoothing radius)
uniform float residual_max;    // pixel cap on the residual radius
uniform float inv_focus;       // 1 / focus distance (metres)
uniform vec2 proj_z;           // projection [2][2], [3][2]
uniform vec2 texel_size;       // 1 / target size
uniform float tap_rotation;    // varies the tap pattern per sample
uniform int lens_mode;         // 0 plain copy, 1 weight this sample, 2 normalize the average,
                               // 3 masked average for smoothing, 4 smoothed average,
                               // 5 this sample's small-source indicator
uniform vec2 lens_pos;         // this sample's unit aperture position
uniform float cat_eye;         // barrel shift at the frame corner (aperture radii); 0 off
uniform vec2 field_scale;      // (tc - 0.5) * field_scale = field position, corner length 1
uniform float sa_strength;     // spherical aberration; 0 off, negative = over-corrected
uniform float sa_coc_scale;    // full-aperture CoC pixels per unit (1/focus - 1/distance)
uniform float pupil_r2;        // this sample's normalized pupil radius squared
uniform float hl_strength;     // bright highlights; 0 off
uniform float hl_threshold;    // luminance where the highlight ramp is centred
uniform float show_smoothing;  // mode 4: > 0 tints the smoothed area red (debug)
uniform float has_star_mask;   // mode 5: > 0 when altDiffuseMap holds this sample's star mask

out vec4 frag_color;

const int TAPS = 12;
const float GOLDEN_ANGLE = 2.39996323;
const float PI = 3.14159265;
const float SA_FOCUS_PIXELS = 3.0; // defocus over which spherical weighting fades in
const float RESIDUAL_DEPTH_TOLERANCE = 1.05; // same-surface slack for the occlusion test
const vec3 LUMINANCE = vec3(0.2126, 0.7152, 0.0722);
const float HL_RING_PIXELS = 6.0;  // isolation ring radius
const float HL_AREA_PIXELS = 4.0;  // CoC radius where the area boost reaches 1
const float HL_MAX_AREA = 1024.0;  // cap on the area factor
const int SMOOTH_TAPS = 16;
const float HALF_MAX = 60000.0; // below the half-float maximum (65504)
const float GLOW_SOURCE_MIN = 0.01; // glow above this marks a light
// Source-map coverage (dot area per bokeh area, see mode 4) where the
// smoothing fades in.
const float COVERAGE_LOW = 0.05;
const float COVERAGE_HIGH = 0.3;

// View-axis distance from the GL depth buffer.
float viewDistance(vec2 uv)
{
    float ndc_z = texture(depthMap, uv).r * 2.0 - 1.0;
    return proj_z.y / (ndc_z + proj_z.x);
}

float residualRadius(float distance_m)
{
    return min(residual_scale * abs(inv_focus - 1.0 / max(distance_m, 1e-4)), residual_max);
}

// Open fraction of a unit-circle aperture clipped by a unit circle at
// distance d: lens (vesica) area over pi.
float catEyeFraction(float d)
{
    if (d >= 2.0)
    {
        return 0.0;
    }
    float h = 0.5 * d;
    return (2.0 * acos(h) - 2.0 * h * sqrt(1.0 - h * h)) / PI;
}

// Normalized average with cat's-eye compensation (sample_weight = 1 / N).
vec4 averageAt(vec2 uv)
{
    vec4 color = texture(diffuseMap, uv) * sample_weight;
    if (cat_eye > 0.0)
    {
        color /= max(catEyeFraction(cat_eye * length((uv - 0.5) * field_scale)), 0.05);
    }
    return color;
}

void main()
{
    if (lens_mode == 5)
    {
        // Light sources in this lens sample, from what the renderer drew
        // (no brightness test): glowing pixels (glow is the screen's alpha,
        // set by the content on bulbs, neon, lamps) and, from
        // altDiffuseMap, the stars drawn again by the sky pool with the
        // scene depth attached (so only visible star pixels). Only out of
        // focus: an in-focus light has no bokeh.
        float glow = texture(diffuseMap, tc).a;
        float star = has_star_mask > 0.0 ? texture(altDiffuseMap, tc).r : 0.0;
        float defocus = abs(sa_coc_scale * (inv_focus - 1.0 / max(viewDistance(tc), 1e-4)));
        bool source = (glow > GLOW_SOURCE_MIN || star > 0.0) && defocus >= 1.0;
        frag_color = vec4(source ? 1.0 : 0.0, 0.0, 0.0, 0.0);
        return;
    }
    if (lens_mode == 3)
    {
        // Premultiplied by the defocus mask: sharp pixels never enter the mips.
        // The target is half float and its mips spread every texel over
        // large areas: a stray NaN/Inf, or HDR beyond half range (sun),
        // would reach whole regions and the exposure meter. Such pixels stay
        // out; very bright ones are clamped to the half range.
        vec4 color = averageAt(tc);
        if (any(isnan(color.rgb)) || any(isinf(color.rgb)))
        {
            frag_color = vec4(0.0);
            return;
        }
        float mask = smoothstep(0.5, 1.5, residualRadius(viewDistance(tc)));
        frag_color = vec4(min(color.rgb, vec3(HALF_MAX)) * mask, mask);
        return;
    }
    if (lens_mode == 4)
    {
        vec4 color = averageAt(tc); // glow (alpha) stays unsmoothed
        float center_distance = viewDistance(tc);
        float radius = residualRadius(center_distance);
        if (radius > 0.5)
        {
            // Texels about twice the spacing of the taps (one level above
            // it) overlap and cover the disk. Every tap, the centre too,
            // reads the mips: one point tap on a dot would keep it visible.
            float lod = log2(max(radius * sqrt(PI / float(SMOOTH_TAPS)), 1.0)) + 1.0;
            vec3 sum = vec3(0.0);
            float weight = 0.0;
            for (int i = 0; i < SMOOTH_TAPS; ++i)
            {
                float r = radius * sqrt((float(i) + 0.5) / float(SMOOTH_TAPS));
                float a = float(i) * GOLDEN_ANGLE;
                vec2 tap_tc = tc + vec2(cos(a), sin(a)) * r * texel_size;
                float tap_distance = viewDistance(tap_tc);
                if (residualRadius(tap_distance) >= r &&
                    tap_distance <= center_distance * RESIDUAL_DEPTH_TOLERANCE)
                {
                    vec4 tap = textureLod(altDiffuseMap, tap_tc, lod);
                    sum += tap.rgb;
                    weight += tap.a;
                }
            }
            vec3 smoothed = sum / max(weight, 1e-4);
            if (weight > 0.01 && !any(isnan(smoothed)) && !any(isinf(smoothed)))
            {
                // Only inside bokeh shapes: where the samples' small-source
                // dots landed. Inside a source's bokeh they cover
                // dot_area / (pi coc^2) of the pixels per sample, so the
                // map's mean over a few dot spacings, times pi coc^2 / N,
                // is about the dot area (>= 1 px) there and exactly 0 where
                // no small source was ever seen.
                float coc = abs(sa_coc_scale * (inv_focus - 1.0 / max(center_distance, 1e-4)));
                float hits = textureLod(specularMap, tc, log2(max(2.0 * radius, 1.0))).r;
                float coverage = hits * sample_weight.x * PI * coc * coc;
                float bokeh = smoothstep(COVERAGE_LOW, COVERAGE_HIGH, coverage);
                color.rgb = mix(color.rgb, smoothed, bokeh);
                if (show_smoothing > 0.0)
                { // Debug overlay: smoothed area tinted red.
                    color.rgb = mix(color.rgb, vec3(max(dot(color.rgb, LUMINANCE), 0.05), 0.0, 0.0), 0.6 * bokeh);
                }
            }
        }
        frag_color = color;
        return;
    }

    vec4 color = texture(diffuseMap, tc);
    float luminance = dot(color.rgb, LUMINANCE); // before residual smoothing
    bool need_depth = residual_scale > 0.0 ||
        (lens_mode == 1 && (sa_strength != 0.0 || hl_strength > 0.0));
    float defocus = 0.0; // signed 1/focus - 1/distance
    float center_distance = 0.0;
    if (need_depth)
    {
        // View-axis distance from the GL depth buffer.
        float ndc_z = texture(depthMap, tc).r * 2.0 - 1.0;
        center_distance = proj_z.y / (ndc_z + proj_z.x);
        defocus = inv_focus - 1.0 / max(center_distance, 1e-4);
    }
    if (residual_scale > 0.0)
    {
        float radius = min(residual_scale * abs(defocus), residual_max);
        if (radius > 0.5)
        {
            // Equal-area Vogel disk, centre tap included. Scatter-as-gather:
            // a tap only contributes when its own residual disk reaches this
            // pixel, so sharp content (radius ~0, e.g. in-focus hair) never
            // bleeds into defocused neighbours, and only when it is not
            // behind this pixel's surface: in every lens position the nearer
            // surface occludes it, so a blurred background never spreads
            // over a nearer edge (hair against the sky). Defocused content
            // still fills its own gaps.
            vec4 sum = color;
            float count = 1.0;
            for (int i = 0; i < TAPS; ++i)
            {
                float r = radius * sqrt((float(i) + 0.5) / float(TAPS));
                float a = float(i) * GOLDEN_ANGLE + tap_rotation;
                vec2 tap_tc = tc + vec2(cos(a), sin(a)) * r * texel_size;
                float tap_ndc = texture(depthMap, tap_tc).r * 2.0 - 1.0;
                float tap_distance = proj_z.y / (tap_ndc + proj_z.x);
                float tap_radius = min(residual_scale * abs(inv_focus - 1.0 / max(tap_distance, 1e-4)),
                                       residual_max);
                if (tap_radius >= r && tap_distance <= center_distance * RESIDUAL_DEPTH_TOLERANCE)
                {
                    sum += texture(diffuseMap, tap_tc);
                    count += 1.0;
                }
            }
            color = sum / count;
        }
    }

    vec2 field = (tc - 0.5) * field_scale;
    if (lens_mode == 1)
    {
        float w = 1.0;
        if (cat_eye > 0.0 && length(lens_pos - cat_eye * field) > 1.0)
        {
            w = 0.0; // blocked by the barrel for this pixel
        }
        if (sa_strength != 0.0)
        {
            float sigma = clamp(sa_coc_scale * defocus / SA_FOCUS_PIXELS, -1.0, 1.0);
            w *= 1.0 - sa_strength * sigma * (2.0 * pupil_r2 - 1.0);
        }
        if (hl_strength > 0.0 && w > 0.0)
        {
            float bright = smoothstep(0.5 * hl_threshold, 1.5 * hl_threshold, luminance);
            float coc = abs(sa_coc_scale * defocus);
            if (bright > 0.0 && coc > 1.0)
            {
                float ring = 0.0;
                for (int i = 0; i < 8; ++i)
                {
                    float a = float(i) * (PI * 0.25);
                    ring += dot(texture(diffuseMap, tc + vec2(cos(a), sin(a)) * HL_RING_PIXELS * texel_size).rgb,
                                LUMINANCE);
                }
                float isolated = smoothstep(2.0, 8.0, luminance / max(ring * 0.125, 1e-3));
                float area = min((coc / HL_AREA_PIXELS) * (coc / HL_AREA_PIXELS), HL_MAX_AREA);
                color.rgb *= 1.0 + hl_strength * bright * isolated * area;
            }
        }
        color *= w;
    }
    else if (lens_mode == 2 && cat_eye > 0.0)
    {
        color /= max(catEyeFraction(cat_eye * length(field)), 0.05);
    }
    frag_color = color * sample_weight;
}
