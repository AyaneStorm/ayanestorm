/**
 * @file asDepthOfFieldNearF.glsl
 * @author chanayane@firestorm
 * @brief Foreground-spreading near bokeh gather for AyaneStorm DoF.
 */
// Two premultiplied foreground layers split by source blur radius. The
// resolve composites back, then front, so a nearer foreground occludes a
// farther one instead of averaging with it.
layout(location = 0) out vec4 frag_back;
layout(location = 1) out vec4 frag_front;

// Gather input: opaque color with extracted highlight sprites removed.
uniform sampler2D diffuseRect;
// Reuse a viewer-reserved sampler name so LLGLSLShader assigns a texture unit.
uniform sampler2D noiseMap;
uniform vec2 screen_res;
uniform int sample_count;
uniform float max_radius;
uniform int aperture_blades;
uniform float aperture_roundness;
uniform float aperture_rotation;
uniform float anamorphic_ratio;
uniform float highlight_boost;

in vec2 vary_fragcoord;

#define AS_DOF_MAX_SAMPLES 96
#define AS_DOF_PI 3.14159265358979323846

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

void main()
{
    vec2 uv = vary_fragcoord;
    vec3 center_color = texture(diffuseRect, uv).rgb;
    float center_coc = texture(noiseMap, uv).g;
    // x: back layer, y: front layer.
    vec3 sum_back = vec3(0.0);
    vec3 sum_front = vec3(0.0);
    vec2 weight_sum = vec2(0.0);
    vec2 coverage_sum = vec2(0.0);
    vec2 coverage = vec2(0.0);
    float kernel_area_sum = 0.0;
    float phase = samplePhase();

    if (center_coc < 0.0)
    {
        float center_radius = -center_coc * max_radius;
        float front = frontShare(center_radius);
        float weight = highlightWeight(center_color) / max(center_radius * center_radius, 1.0);
        sum_back += center_color * weight * (1.0 - front);
        sum_front += center_color * weight * front;
        weight_sum += weight * vec2(1.0 - front, front);
    }

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
            float sample_coc = texture(noiseMap, sample_uv).g;
            float sample_radius = max(-sample_coc, 0.0) * max_radius;
            // Compare radii in aperture space so anamorphic and polygonal
            // kernels retain their intended foreground coverage.
            float distance_pixels = (float(i) + 0.5) / float(sample_count) * max_radius;
            float support = 1.0 - smoothstep(sample_radius - 1.0,
                                             sample_radius + 1.0,
                                             distance_pixels);
            support *= sample_coc < 0.0 ? 1.0 : 0.0;
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
        }

        // Estimate each layer's accumulated opacity rather than taking the
        // hardest individual sample. A uniform foreground plane converges to
        // one while a silhouette edge produces a naturally fractional mask.
        float coverage_scale = max_radius * max_radius / max(kernel_area_sum, 0.0001);
        coverage = clamp(coverage_sum * coverage_scale, vec2(0.0), vec2(1.0));

        // A pixel which is itself on the foreground surface remains owned by
        // that surface, in its own layer. The gather estimates only the
        // coverage spreading outside its original silhouette; allowing it to
        // under-estimate an interior pixel exposes the synthesized background
        // as torn holes.
        if (center_coc < 0.0)
        {
            float center_radius = -center_coc * max_radius;
            float own = smoothstep(0.25, 1.0, center_radius);
            float front = frontShare(center_radius);
            coverage = max(coverage, own * vec2(1.0 - front, front));
        }
    }

    vec3 back_color = weight_sum.x > 0.0001 ? sum_back / weight_sum.x : center_color;
    vec3 front_color = weight_sum.y > 0.0001 ? sum_front / weight_sum.y : center_color;
    frag_back = vec4(back_color * coverage.x, coverage.x);
    frag_front = vec4(front_color * coverage.y, coverage.y);
}
