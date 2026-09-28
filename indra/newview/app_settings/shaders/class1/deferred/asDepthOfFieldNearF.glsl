/**
 * @file asDepthOfFieldNearF.glsl
 * @author chanayane@firestorm
 * @brief Foreground-spreading near bokeh gather for AyaneStorm DoF.
 *
 * near_pass 0 (gather resolution, four attachments, mips generated after):
 *   foreground source pyramid, see buildSource().
 * near_pass 1: two premultiplied foreground layers split by source blur
 *   radius. The resolve composites back, then front, so a nearer foreground
 *   occludes a farther one instead of averaging with it.
 *
 * Area taps. A point tap sees a thin defocused strand only when it happens to
 * land on it, so its spread coverage flickers from pixel to pixel (fine
 * pattern on defocused hair). With use_pyramid, each tap instead reads the
 * source pyramid at the mip level matching the local tap spacing, so the
 * strand contributes its share of the tap's area on every tap.
 *
 * The pyramid stores, per band [a, b] of tap distance, the area average of
 * each source's reach w [d < r] (w = R^2 / r^2, the inverse splat area
 * scaled by R^2 for half-float range):
 *     A_k = w (clamp(r, a, b)^2 - a^2) / (b^2 - a^2).
 * This is linear in the sources, so mip averaging stays exact for any mix of
 * blur radii, and the area-integrated coverage is exact; only the edge
 * position inside a band is approximated. Band edges: 0, 1, then
 * 1 + (R - 1) (j / 10)^1.5, j = 1..10 (11 bands over [0, R]).
 *
 * Split by source radius (split_radius = 8 pi R / N, at least 1 px): only
 * sources blurred at least that much enter the pyramid. Smaller ones stay
 * point taps, which are dense enough there (tap spacing at distance r is
 * under r / 2), and keep their own color: one pyramid color per texel mixed
 * a sharp dark hair lock into the near-focused face beside it, whose 1 / r^2
 * weight dominates the coverage (dark sharp strands).
 * Measured (scripts/testing/dof_near_gather_sim.py, 96 taps, R = 24): thin
 * strands over a near-focused face, coverage-weighted color error 0.011
 * (point taps 0.033, single pyramid 0.044); strand coverage error 7x lower
 * than point taps when all strands are above the split.
 */
layout(location = 0) out vec4 frag_data0;   // back layer | source color
layout(location = 1) out vec4 frag_data1;   // front layer | bands 0-3
layout(location = 2) out vec4 frag_data2;   // bands 4-7 | radius moments
layout(location = 3) out vec4 frag_data3;   // bands 8-10, front weight

// Gather input: opaque color with extracted highlight sprites removed.
uniform sampler2D diffuseRect;
// Reuse a viewer-reserved sampler name so LLGLSLShader assigns a texture unit.
uniform sampler2D noiseMap;
// Source pyramid attachments 0-3 (near_pass 1 with use_pyramid).
uniform sampler2D specularRect;
uniform sampler2D emissiveRect;
uniform sampler2D lightMap;
uniform sampler2D bloomMap;
uniform vec2 screen_res;
uniform vec2 target_res;
uniform int sample_count;
uniform float max_radius;
uniform int aperture_blades;
uniform float aperture_roundness;
uniform float aperture_rotation;
uniform float anamorphic_ratio;
uniform float highlight_boost;
uniform int near_pass;
uniform int use_pyramid;
uniform float split_radius;

in vec2 vary_fragcoord;

#define AS_DOF_MAX_SAMPLES 96
#define AS_DOF_PI 3.14159265358979323846
#define BAND_COUNT 11

float samplePhase()
{
    vec2 pixel = floor(gl_FragCoord.xy);
    return fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453) * 2.0 * AS_DOF_PI;
}

vec2 apertureSample(int index, int count, float phase, out float area_weight)
{
    float fi = float(index) + 0.5;
    // Reserve samples near the center even when the maximum disc is large.
    float radius = fi / float(max(count, 1));
    // Polar angle before rotation; a blade vertex lies at angle 0 (ASDoFAperture).
    float phi = fi * 2.399963229728653 + phase;
    float boundary = 1.0;
    if (aperture_blades >= 3)
    {
        float sector = 2.0 * AS_DOF_PI / float(aperture_blades);
        float local_angle = mod(phi, sector) - 0.5 * sector;
        float polygon = cos(0.5 * sector) / max(cos(local_angle), 0.001);
        boundary = mix(polygon, 1.0, aperture_roundness);
    }
    // Correct both uniform-radius sampling density and polygon deformation.
    area_weight = 2.0 * radius * boundary * boundary;
    float angle = phi + aperture_rotation;
    return vec2(cos(angle) * anamorphic_ratio, sin(angle)) * radius * boundary;
}

float highlightWeight(vec3 color)
{
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return 1.0 + highlight_boost * smoothstep(0.5, 1.5, luminance);
}

// Share of a source with this blur radius owned by the front layer. The soft
// split around half the maximum radius avoids popping between layers.
float frontShare(float radius)
{
    return smoothstep(0.4 * max_radius, 0.6 * max_radius, radius);
}

// Share of a source spread by this gather. Sources under 0.5-2 px of blur
// stay in the resolve's sharp base (same ramp there): the near layers are
// composited over the transparent layers too, where a nearly focused skin
// pixel spread at full coverage would wash out in-focus alpha brows or hair.
float spreadShare(float radius)
{
    return smoothstep(0.5, 2.0, radius);
}

float bandEdge(int k)
{
    return k == 0 ? 0.0 :
        1.0 + (max_radius - 1.0) * pow(float(k - 1) / 10.0, 1.5);
}

int bandIndex(float distance)
{
    if (distance < 1.0)
    {
        return 0;
    }
    float t = pow(clamp((distance - 1.0) / max(max_radius - 1.0, 0.0001), 0.0, 1.0),
                  2.0 / 3.0);
    return min(1 + int(floor(10.0 * t)), BAND_COUNT - 1);
}

// Pyramid level 0: the gather-resolution texel averages a 2x2 set of
// full-resolution sources (point fetches: interpolating a foreground and a
// background texel would mix CoC signs).
void buildSource()
{
    vec2 scale = screen_res / target_res;
    ivec2 last = ivec2(screen_res) - 1;
    vec4 color_sum = vec4(0.0);
    float bands[BAND_COUNT];
    for (int k = 0; k < BAND_COUNT; ++k)
    {
        bands[k] = 0.0;
    }
    float front_sum = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        vec2 offset = vec2(float(i & 1), float(i >> 1)) * 0.5 + 0.25;
        ivec2 p = clamp(ivec2((floor(gl_FragCoord.xy) + offset) * scale),
                        ivec2(0), last);
        float r = -texelFetch(noiseMap, p, 0).g * max_radius;
        if (r < split_radius)
        {
            continue;
        }
        float w = spreadShare(r) * max_radius * max_radius / (r * r);
        vec3 color = min(texelFetch(diffuseRect, p, 0).rgb, vec3(60000.0));
        float hw = highlightWeight(color);
        color_sum += vec4(color * hw, hw);
        for (int k = 0; k < BAND_COUNT; ++k)
        {
            float a = bandEdge(k);
            float b = bandEdge(k + 1);
            float rc = clamp(r, a, b);
            bands[k] += w * (rc * rc - a * a) / (b * b - a * a);
        }
        front_sum += w * frontShare(r);
    }
    frag_data0 = color_sum * 0.25;
    frag_data1 = vec4(bands[0], bands[1], bands[2], bands[3]) * 0.25;
    frag_data2 = vec4(bands[4], bands[5], bands[6], bands[7]) * 0.25;
    frag_data3 = vec4(bands[8], bands[9], bands[10], front_sum) * 0.25;
}

float bandValue(int k, vec2 uv, float lod)
{
    vec4 bands;
    if (k < 4)
    {
        bands = textureLod(emissiveRect, uv, lod);
        return bands[k];
    }
    if (k < 8)
    {
        bands = textureLod(lightMap, uv, lod);
        return bands[k - 4];
    }
    bands = textureLod(bloomMap, uv, lod);
    return bands[k - 8];
}

void main()
{
    if (near_pass == 0)
    {
        buildSource();
        return;
    }

    vec2 uv = vary_fragcoord;
    vec3 center_color = texture(diffuseRect, uv).rgb;
    float center_coc = texture(noiseMap, uv).g;
    // x: back layer, y: front layer.
    vec3 sum_back = vec3(0.0);
    vec3 sum_front = vec3(0.0);
    vec2 weight_sum = vec2(0.0);
    vec2 coverage_sum = vec2(0.0);
    vec2 coverage = vec2(0.0);
    // Source blur radius moments per layer, same weights as the color:
    // (back r, back r^2, front r, front r^2).
    vec4 moment_sum = vec4(0.0);
    float kernel_area_sum = 0.0;
    float phase = samplePhase();
    bool pyramid = use_pyramid != 0;
    float pixel_scale = max(screen_res.x / target_res.x, 1.0);
    float inverse_scale = 1.0 / max(max_radius * max_radius, 0.0001);

    // No separate center term. It added the center pixel's color at weight
    // 1 / r^2 while a disc fully covered by one source totals N / R^2 over
    // all taps, so the center counted R^2 / (N r^2) times a full disc (about
    // 30% of the veil color for R 20, N 96, r 3): each thin strand redrew
    // itself, sharp, into the veil at its own pixels. The taps near d = 0
    // already sample the center with its true area share.

    if (max_radius > 0.0)
    {
        for (int i = 0; i < AS_DOF_MAX_SAMPLES; ++i)
        {
            if (i >= sample_count)
            {
                break;
            }
            float aperture_weight;
            vec2 disk = apertureSample(i, sample_count, phase, aperture_weight);
            kernel_area_sum += aperture_weight;
            vec2 offset_pixels = disk * max_radius;
            // A foreground point images as the inverted aperture (see
            // ASDoFCamera): the source reaching this pixel lies at +disk.
            vec2 sample_uv = clamp(uv + offset_pixels / screen_res,
                                   0.5 / screen_res, vec2(1.0) - 0.5 / screen_res);
            // Compare radii in aperture space so anamorphic and polygonal
            // kernels retain their intended foreground coverage.
            float distance_pixels = (float(i) + 0.5) / float(sample_count) * max_radius;

            if (pyramid)
            {
                // Mip level matching the local tap spacing: uniform-radius
                // taps put 2 pi d R / N px^2 around each tap at distance d.
                float spacing = sqrt(2.0 * AS_DOF_PI * max(distance_pixels, 0.5) *
                                     max_radius / float(sample_count));
                float lod = log2(max(spacing / pixel_scale, 1.0));
                float reach = bandValue(bandIndex(distance_pixels), sample_uv, lod);
                if (reach > 0.0)
                {
                    float front = min(reach, textureLod(bloomMap, sample_uv, lod).a);
                    vec2 share = vec2(reach - front, front) * inverse_scale;
                    vec4 source = textureLod(specularRect, sample_uv, lod);
                    vec3 sample_color = source.rgb / max(source.a, 0.000001);
                    float weight = aperture_weight * highlightWeight(sample_color);
                    sum_back += sample_color * weight * share.x;
                    sum_front += sample_color * weight * share.y;
                    weight_sum += weight * share;
                    coverage_sum += aperture_weight * share;
                    // The pyramid keeps no per-source radius; every source
                    // reaching this tap is blurred at least this much.
                    float r = max(distance_pixels, split_radius);
                    moment_sum += weight * vec4(share.x * vec2(r, r * r),
                                                share.y * vec2(r, r * r));
                }
            }

            float sample_coc = texture(noiseMap, sample_uv).g;
            float sample_radius = max(-sample_coc, 0.0) * max_radius;
            // Point tap: every source without the pyramid, otherwise only
            // sources under split_radius, which the pyramid leaves out.
            if (sample_coc >= 0.0 || (pyramid && sample_radius >= split_radius))
            {
                continue;
            }
            float support = (1.0 - smoothstep(sample_radius - 1.0,
                                              sample_radius + 1.0,
                                              distance_pixels)) *
                            spreadShare(sample_radius);
            if (support <= 0.0)
            {
                continue;
            }
            vec3 sample_color = texture(diffuseRect, sample_uv).rgb;
            float inverse_splat_area = 1.0 / max(sample_radius * sample_radius, 1.0);
            float front = frontShare(sample_radius);
            vec2 share = vec2(1.0 - front, front);
            float weight = support * aperture_weight * inverse_splat_area * highlightWeight(sample_color);
            sum_back += sample_color * weight * share.x;
            sum_front += sample_color * weight * share.y;
            weight_sum += weight * share;
            coverage_sum += support * aperture_weight * inverse_splat_area * share;
            vec2 radius_moments = vec2(sample_radius, sample_radius * sample_radius);
            moment_sum += weight * vec4(share.x * radius_moments,
                                        share.y * radius_moments);
        }

        // Estimate each layer's accumulated opacity rather than taking the
        // hardest individual sample. A uniform foreground plane converges to
        // one while a silhouette edge produces a naturally fractional mask.
        float coverage_scale = max_radius * max_radius / max(kernel_area_sum, 0.0001);
        coverage = clamp(coverage_sum * coverage_scale, vec2(0.0), vec2(1.0));
        // No ownership rule: inside a solid foreground every tap nearer than
        // the source's radius lands on it, so the estimate reaches full
        // coverage by itself, and nearly focused pixels stay in the
        // resolve's sharp base (spreadShare()).
    }

    vec3 back_color = weight_sum.x > 0.0001 ? sum_back / weight_sum.x : center_color;
    vec3 front_color = weight_sum.y > 0.0001 ? sum_front / weight_sum.y : center_color;
    frag_data0 = vec4(back_color * coverage.x, coverage.x);
    frag_data1 = vec4(front_color * coverage.y, coverage.y);
    frag_data2 = vec4(moment_sum.xy / max(weight_sum.x, 0.0001),
                      moment_sum.zw / max(weight_sum.y, 0.0001));
    frag_data3 = vec4(0.0);
}
