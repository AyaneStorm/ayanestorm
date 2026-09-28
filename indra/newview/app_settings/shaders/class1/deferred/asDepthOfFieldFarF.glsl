/**
 * @file asDepthOfFieldFarF.glsl
 * @author chanayane@firestorm
 * @brief Plane-discriminating far bokeh gather for AyaneStorm DoF.
 */
out vec4 frag_color;

// Gather input: opaque color with extracted highlight sprites removed.
uniform sampler2D diffuseRect;
// Reuse a viewer-reserved sampler name so LLGLSLShader assigns a texture unit.
uniform sampler2D noiseMap;
// Background completion (rgb, signed opaque CoC) behind foreground pixels.
uniform sampler2D lightMap;
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
    float radius = sqrt(fi / float(max(count, 1)));
    // Polar angle before rotation. A blade vertex lies at angle 0, as in the
    // aperture-sampled renderer (ASDoFAperture), so both modes share one shape.
    float phi = fi * 2.399963229728653 + phase;
    float boundary = 1.0;
    if (aperture_blades >= 3)
    {
        float sector = 2.0 * AS_DOF_PI / float(aperture_blades);
        float local_angle = mod(phi, sector) - 0.5 * sector;
        float polygon = cos(0.5 * sector) / max(cos(local_angle), 0.001);
        boundary = mix(polygon, 1.0, aperture_roundness);
    }
    // Uniform angle and sqrt-radius sampling has an area Jacobian proportional
    // to boundary squared. Weighting by it makes polygonal apertures integrate
    // uniformly without a lookup table or a platform-specific compute pass.
    area_weight = boundary * boundary;
    float angle = phi + aperture_rotation;
    return vec2(cos(angle) * anamorphic_ratio, sin(angle)) * radius * boundary;
}

float highlightWeight(vec3 color)
{
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return 1.0 + highlight_boost * smoothstep(0.5, 1.5, luminance);
}

// Color and signed CoC of a tap. Foreground taps are replaced by the
// background completion: the far blur behind a silhouette integrates what the
// foreground hides instead of renormalizing over the remaining taps.
vec4 farSource(vec2 sample_uv, bool completion_only)
{
    float coc = texture(noiseMap, sample_uv).g;
    if (completion_only || coc < -0.0001)
    {
        return texture(lightMap, sample_uv);
    }
    return vec4(texture(diffuseRect, sample_uv).rgb, coc);
}

vec4 gatherFar(vec2 uv, vec3 center_color, float center_radius, float phase,
               bool completion_only)
{
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
        // A background point images as the upright aperture (see
        // ASDoFCamera): the source reaching this pixel lies at -disk.
        vec2 offset_pixels = disk * center_radius;
        vec2 sample_uv = clamp(uv - offset_pixels / screen_res,
                               0.5 / screen_res, vec2(1.0) - 0.5 / screen_res);
        vec4 source = farSource(sample_uv, completion_only);
        float sample_radius = max(source.a, 0.0) * max_radius;
        // Compare radii in aperture space so anamorphic and polygonal kernels
        // retain their intended coverage instead of being clipped as circles.
        float distance_pixels = sqrt((float(i) + 0.5) / float(sample_count)) * center_radius;
        float coverage = 1.0 - smoothstep(sample_radius - 1.0,
                                          sample_radius + 1.0,
                                          distance_pixels);
        coverage *= source.a > 0.0 ? 1.0 : 0.0;
        float weight = coverage * aperture_weight * highlightWeight(source.rgb);
        sum += source.rgb * weight;
        weight_sum += weight;
    }

    return vec4(sum / max(weight_sum, 0.0001), 1.0);
}

void main()
{
    vec2 uv = vary_fragcoord;
    float center_coc = texture(noiseMap, uv).g;
    float phase = samplePhase();

    // Foreground center: blur the depth-biased background completion with its
    // own CoC. The near layers own these pixels; this plate is revealed only by
    // their fractional silhouette coverage.
    if (center_coc < -0.0001)
    {
        vec4 background = texture(lightMap, uv);
        float background_radius = max(background.a, 0.0) * max_radius;
        frag_color = background_radius < 0.5
            ? vec4(background.rgb, 1.0)
            : gatherFar(uv, background.rgb, background_radius, phase, true);
        return;
    }

    if (center_coc <= 0.0001 || max_radius <= 0.0)
    {
        frag_color = vec4(0.0);
        return;
    }

    frag_color = gatherFar(uv, texture(diffuseRect, uv).rgb,
                           center_coc * max_radius, phase, false);
}
