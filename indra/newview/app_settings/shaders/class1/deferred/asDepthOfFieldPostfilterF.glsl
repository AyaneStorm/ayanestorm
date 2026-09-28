/**
 * @file asDepthOfFieldPostfilterF.glsl
 * @author chanayane@firestorm
 * @brief Radius-aware sampling-noise postfilter for AyaneStorm DoF.
 *
 * The gathers sample each disc with a bounded number of taps under a
 * per-pixel random phase, so large discs come out grainy. This pass smooths
 * one gathered layer (premultiplied color at blur resolution) over the
 * local tap spacing, using the first and second moments of the source blur
 * radius the gather accumulated:
 * - width: half the tap spacing at the local mean radius (far: sqrt-radius
 *   taps, r sqrt(pi / N); near and transparent: uniform-radius taps,
 *   sqrt(2 pi r R / N)), at most a third of that radius;
 * - mixed near/far edges: neighbours weigh by how close their mean radius
 *   is (tolerance max(1, 0.15 r)), so regions of different blur do not
 *   average. Widening that tolerance by the radius variance, as first
 *   planned, blurred the true transition where the near/far mix changes
 *   fast (scripts/testing/dof_postfilter_sim.py: edge error 0.019 -> 0.032
 *   at N 96); the second moment only feeds the debug view;
 * - aperture boundaries: neighbours also weigh by coverage similarity; a
 *   disc rim jumps in coverage within one tap spacing, sampling noise does
 *   not.
 * Pixels blurred under 2 px are returned unchanged.
 * Measured (dof_postfilter_sim.py, N 16/32/96): thin-strand veil noise
 * 2.1-2.9x lower, disc rim error 1.9-2.4x lower, mixed edge error slightly
 * lower, mean coverage kept.
 */
out vec4 frag_color;

// Layer color and its radius moments (blur resolution, point sampled).
uniform sampler2D diffuseRect;
uniform sampler2D noiseMap;
uniform vec2 screen_res;
uniform vec2 target_res;
uniform int sample_count;
uniform float max_radius;
// 0: far gather (sqrt-radius taps); 1: near/transparent (uniform radius).
uniform int plane_kind;
// Moments in .xy (0) or .zw (1) of noiseMap.
uniform int moment_channel;
// 1: write the radius spread sqrt(var) / R instead of filtering (debug 25).
uniform int debug_view;

#define TAP_COUNT 12
#define AS_DOF_PI 3.14159265358979323846

vec2 moments(vec2 uv)
{
    vec4 data = texture(noiseMap, uv);
    return moment_channel == 0 ? data.xy : data.zw;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / target_res;
    vec4 center = texture(diffuseRect, uv);
    vec2 center_moments = moments(uv);
    float pixel_scale = max(screen_res.x / target_res.x, 1.0);
    vec2 texel = 1.0 / target_res;

    // An empty pixel between sparse hits takes the coverage-weighted radius
    // of its neighbours at 1 and 3 px, so gaps inside a thin veil fill in.
    // Direct neighbours alone left most gaps of a sparse disc empty while
    // covered pixels averaged them in: mean coverage fell 15 % at N 16 (7 %
    // with this probe, under 1 % at N 96).
    if (center.a <= 0.0001)
    {
        vec3 sum = vec3(0.0);
        for (int i = 0; i < 8; ++i)
        {
            int k = i & 3;
            vec2 offset = k < 2 ? vec2(float(k * 2 - 1), 0.0) : vec2(0.0, float(k * 2 - 5));
            vec2 n_uv = uv + offset * (i < 4 ? 1.0 : 3.0) * texel;
            float a = texture(diffuseRect, n_uv).a;
            sum += a * vec3(moments(n_uv), 1.0);
        }
        center_moments = sum.z > 0.0001 ? sum.xy / sum.z : vec2(0.0);
    }

    float mean_radius = center_moments.x;
    float variance = max(center_moments.y - mean_radius * mean_radius, 0.0);
    if (debug_view != 0)
    {
        float spread = sqrt(variance) / max(max_radius, 0.0001);
        frag_color = center.a > 0.0001 ? vec4(vec3(spread), 1.0) : vec4(0.0);
        return;
    }
    if (mean_radius < 2.0 || sample_count <= 0)
    {
        frag_color = center;
        return;
    }

    float spacing = plane_kind == 0
        ? mean_radius * sqrt(AS_DOF_PI / float(sample_count))
        : sqrt(2.0 * AS_DOF_PI * mean_radius * max_radius / float(sample_count));
    float width = min(0.5 * spacing, mean_radius / 3.0) / pixel_scale;
    if (width < 0.75)
    {
        frag_color = center;
        return;
    }

    float sigma = max(1.0, 0.15 * mean_radius);
    vec4 sum = center;
    float weight_sum = 1.0;
    for (int i = 0; i < TAP_COUNT; ++i)
    {
        // Fixed Vogel disc: deterministic, adds no noise of its own.
        float fi = float(i) + 0.5;
        float d = sqrt(fi / float(TAP_COUNT));
        float angle = fi * 2.399963229728653;
        vec2 n_uv = uv + vec2(cos(angle), sin(angle)) * d * width * texel;
        vec4 neighbour = texture(diffuseRect, n_uv);
        vec2 neighbour_moments = moments(n_uv);
        // An empty neighbour's radius is unknown, not zero: only its
        // coverage is compared.
        if (neighbour.a <= 0.0001)
        {
            neighbour_moments = center_moments;
        }
        float radius_delta = neighbour_moments.x - mean_radius;
        float weight = exp(-2.0 * d * d);
        weight *= exp(-radius_delta * radius_delta / (2.0 * sigma * sigma));
        float coverage_delta = neighbour.a - center.a;
        weight *= exp(-coverage_delta * coverage_delta / 0.13);
        sum += neighbour * weight;
        weight_sum += weight;
    }
    frag_color = sum / weight_sum;
}
