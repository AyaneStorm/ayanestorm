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

out vec4 frag_color;

const int TAPS = 12;
const float GOLDEN_ANGLE = 2.39996323;

void main()
{
    vec4 color = texture(diffuseMap, tc);
    if (residual_scale > 0.0)
    {
        // View-axis distance from the GL depth buffer.
        float ndc_z = texture(depthMap, tc).r * 2.0 - 1.0;
        float distance = proj_z.y / (ndc_z + proj_z.x);
        float radius = residual_scale * abs(inv_focus - 1.0 / max(distance, 1e-4));
        radius = min(radius, residual_max);
        if (radius > 0.5)
        {
            // Equal-area Vogel disk, centre tap included.
            vec4 sum = color;
            for (int i = 0; i < TAPS; ++i)
            {
                float r = radius * sqrt((float(i) + 0.5) / float(TAPS));
                float a = float(i) * GOLDEN_ANGLE + tap_rotation;
                sum += texture(diffuseMap, tc + vec2(cos(a), sin(a)) * r * texel_size);
            }
            color = sum / float(TAPS + 1);
        }
    }
    frag_color = color * sample_weight;
}
