/**
 * @file asDepthOfFieldNearF.glsl
 * @author chanayane@firestorm
 * @brief Foreground-spreading near bokeh gather for AyaneStorm DoF.
 */
out vec4 frag_color;

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

#define AS_DOF_MAX_SAMPLES 48
#define AS_DOF_PI 3.14159265358979323846

float samplePhase()
{
    vec2 pixel = floor(gl_FragCoord.xy);
    return fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453) * 2.0 * AS_DOF_PI;
}

vec2 apertureSample(int index, int count, float phase, out float area_weight)
{
    float fi = float(index) + 0.5;
    float radius = sqrt(fi / float(max(count, 1)));
    float angle = fi * 2.399963229728653 + phase + aperture_rotation;
    float boundary = 1.0;
    if (aperture_blades >= 3)
    {
        float sector = 2.0 * AS_DOF_PI / float(aperture_blades);
        float local_angle = mod(angle - aperture_rotation + 0.5 * sector, sector) - 0.5 * sector;
        float polygon = cos(0.5 * sector) / max(cos(local_angle), 0.001);
        boundary = mix(polygon, 1.0, aperture_roundness);
    }
    // Correct the area Jacobian introduced by radial polygon deformation.
    // This is equivalent to equal-area angular sampling but needs no LUT.
    area_weight = boundary * boundary;
    return vec2(cos(angle) * anamorphic_ratio, sin(angle)) * radius * boundary;
}

float highlightWeight(vec3 color)
{
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return 1.0 + highlight_boost * smoothstep(0.5, 1.5, luminance);
}

void main()
{
    vec2 uv = vary_fragcoord;
    vec4 center_color = texture(diffuseRect, uv);
    float center_coc = texture(noiseMap, uv).r;
    vec3 sum = vec3(0.0);
    float weight_sum = 0.0;
    float foreground_coverage = 0.0;
    float coverage_sum = 0.0;
    float kernel_area_sum = 0.0;
    float phase = samplePhase();

    if (center_coc < 0.0)
    {
        float center_radius = -center_coc * max_radius;
        float weight = highlightWeight(center_color.rgb) / max(center_radius * center_radius, 1.0);
        sum += center_color.rgb * weight;
        weight_sum += weight;
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
            // Gather the source of a foreground splat which reaches this
            // destination pixel. The sign preserves asymmetric odd-blade PSFs.
            vec2 sample_uv = clamp(uv - offset_pixels / screen_res,
                                   0.5 / screen_res, vec2(1.0) - 0.5 / screen_res);
            float sample_coc = texture(noiseMap, sample_uv).r;
            float sample_radius = max(-sample_coc, 0.0) * max_radius;
            // Compare radii in aperture space so anamorphic and polygonal
            // kernels retain their intended foreground coverage.
            float distance_pixels = sqrt((float(i) + 0.5) / float(sample_count)) * max_radius;
            float coverage = 1.0 - smoothstep(sample_radius - 1.0,
                                              sample_radius + 1.0,
                                              distance_pixels);
            coverage *= sample_coc < 0.0 ? 1.0 : 0.0;
            vec3 sample_color = texture(diffuseRect, sample_uv).rgb;
            float inverse_splat_area = 1.0 / max(sample_radius * sample_radius, 1.0);
            float weight = coverage * aperture_weight * inverse_splat_area * highlightWeight(sample_color);
            sum += sample_color * weight;
            weight_sum += weight;
            coverage_sum += coverage * aperture_weight * inverse_splat_area;
        }

        // Estimate the accumulated foreground opacity rather than taking the
        // hardest individual sample. A uniform foreground plane converges to
        // one while a silhouette edge produces a naturally fractional mask.
        float coverage_scale = max_radius * max_radius / max(kernel_area_sum, 0.0001);
        foreground_coverage = clamp(coverage_sum * coverage_scale, 0.0, 1.0);

        // A pixel which is itself on the foreground surface remains owned by
        // that surface. The gather estimates only the coverage spreading
        // outside its original silhouette; allowing it to under-estimate an
        // interior pixel exposes the synthesized background as torn holes.
        if (center_coc < 0.0)
        {
            foreground_coverage = max(foreground_coverage,
                                      smoothstep(0.25, 1.0, -center_coc * max_radius));
        }
    }

    vec3 near_color = weight_sum > 0.0001 ? sum / weight_sum : center_color.rgb;
    foreground_coverage = clamp(foreground_coverage, 0.0, 1.0);
    frag_color = vec4(near_color * foreground_coverage, foreground_coverage);
}
