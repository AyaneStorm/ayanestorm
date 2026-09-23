/**
 * @file asDepthOfFieldFarF.glsl
 * @author chanayane@firestorm
 * @brief Plane-discriminating far bokeh gather for AyaneStorm DoF.
 */
out vec4 frag_color;

uniform sampler2D diffuseRect;
// Reuse a viewer-reserved sampler name so LLGLSLShader assigns a texture unit.
uniform sampler2D noiseMap;
uniform vec2 screen_res;
uniform int sample_count;
uniform float max_radius;
uniform float foreground_radius;
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
    // Uniform angle and sqrt-radius sampling has an area Jacobian proportional
    // to boundary squared. Weighting by it makes polygonal apertures integrate
    // uniformly without a lookup table or a platform-specific compute pass.
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
    vec3 center_color = texture(diffuseRect, uv).rgb;
    float center_coc = texture(noiseMap, uv).g;
    float phase = samplePhase();

    // Build a background plate beneath defocused foreground edges. The near
    // pass owns every foreground center pixel, so this plate is revealed only
    // by its deliberate fractional silhouette coverage.
    if (center_coc < -0.0001 && foreground_radius > 0.0)
    {
        vec3 fill_sum = vec3(0.0);
        float fill_weight = 0.0;
        for (int i = 0; i < AS_DOF_MAX_SAMPLES; ++i)
        {
            if (i >= sample_count)
            {
                break;
            }
            float aperture_weight;
            vec2 disk = apertureSample(i, sample_count, phase, aperture_weight);
            vec2 sample_uv = clamp(uv + disk * foreground_radius / screen_res,
                                   0.5 / screen_res, vec2(1.0) - 0.5 / screen_res);
            float sample_coc = texture(noiseMap, sample_uv).g;
            if (sample_coc >= -0.0001)
            {
                fill_sum += texture(diffuseRect, sample_uv).rgb * aperture_weight;
                fill_weight += aperture_weight;
            }
        }
        frag_color = fill_weight > 0.0001
            ? vec4(fill_sum / fill_weight, 1.0)
            : vec4(0.0);
        return;
    }

    if (center_coc <= 0.0001 || max_radius <= 0.0)
    {
        frag_color = vec4(0.0);
        return;
    }

    float center_radius = center_coc * max_radius;
    vec3 sum = center_color * highlightWeight(center_color);
    float weight_sum = highlightWeight(center_color);

    for (int i = 0; i < AS_DOF_MAX_SAMPLES; ++i)
    {
        if (i >= sample_count)
        {
            break;
        }
        float aperture_weight;
        vec2 disk = apertureSample(i, sample_count, phase, aperture_weight);
        vec2 offset_pixels = disk * center_radius;
        vec2 sample_uv = clamp(uv + offset_pixels / screen_res,
                               0.5 / screen_res, vec2(1.0) - 0.5 / screen_res);
        float sample_coc = texture(noiseMap, sample_uv).g;
        float sample_radius = max(sample_coc, 0.0) * max_radius;
        // Compare radii in aperture space so anamorphic and polygonal kernels
        // retain their intended coverage instead of being clipped as circles.
        float distance_pixels = sqrt((float(i) + 0.5) / float(sample_count)) * center_radius;
        float coverage = 1.0 - smoothstep(sample_radius - 1.0,
                                          sample_radius + 1.0,
                                          distance_pixels);
        coverage *= sample_coc > 0.0 ? 1.0 : 0.0;
        vec3 sample_color = texture(diffuseRect, sample_uv).rgb;
        float weight = coverage * aperture_weight * highlightWeight(sample_color);
        sum += sample_color * weight;
        weight_sum += weight;
    }

    frag_color = vec4(sum / max(weight_sum, 0.0001), 1.0);
}
