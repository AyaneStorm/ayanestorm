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
 */
in vec2 tc;

uniform sampler2D diffuseMap;
uniform sampler2D depthMap;
uniform vec4 sample_weight;    // per channel: axial CA colour weights, glow 1
uniform float residual_scale;  // pixels per unit |1/focus - 1/distance|; 0 disables
uniform float residual_max;    // pixel cap on the residual radius
uniform float inv_focus;       // 1 / focus distance (metres)
uniform vec2 proj_z;           // projection [2][2], [3][2]
uniform vec2 texel_size;       // 1 / target size
uniform float tap_rotation;    // varies the tap pattern per sample
uniform int lens_mode;         // 0 plain copy, 1 weight this sample, 2 normalize the average
uniform vec2 lens_pos;         // this sample's unit aperture position
uniform float cat_eye;         // barrel shift at the frame corner (aperture radii); 0 off
uniform vec2 field_scale;      // (tc - 0.5) * field_scale = field position, corner length 1
uniform float sa_strength;     // spherical aberration; 0 off, negative = over-corrected
uniform float sa_coc_scale;    // full-aperture CoC pixels per unit (1/focus - 1/distance)
uniform float pupil_r2;        // this sample's normalized pupil radius squared
uniform float hl_strength;     // bright highlights; 0 off
uniform float hl_threshold;    // luminance where the highlight ramp is centred

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

void main()
{
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
