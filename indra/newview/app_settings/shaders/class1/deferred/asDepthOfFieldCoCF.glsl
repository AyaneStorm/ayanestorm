/**
 * @file asDepthOfFieldCoCF.glsl
 * @author chanayane@firestorm
 * @brief Signed physical circle-of-confusion preparation for AyaneStorm DoF.
 */
// Effective, opaque, transparent CoC and transparent coverage. Keeping the
// components separate allows later passes to evolve without recapturing the
// scene and makes transparent-depth failures directly diagnosable.
out vec4 frag_coc;

uniform sampler2D depthMap;
// Viewer-reserved slots carry the private transparent coverage and depth.
uniform sampler2D noiseMap;
uniform sampler2D lightMap;
uniform mat4 inv_proj;
uniform float focal_distance;
uniform float blur_constant;
uniform float tan_pixel_angle;
uniform float magnification;
uniform float max_coc;
uniform int has_transparent_depth;

in vec2 vary_fragcoord;

float calculateCoC(float depth)
{
    float coc = (depth - focal_distance) / -depth * blur_constant;
    coc /= magnification;
    float pixel_length = tan_pixel_angle * -focal_distance;
    coc = coc / pixel_length;
    return coc * 1.41421356237;
}

void main()
{
    float device_depth = texture(depthMap, vary_fragcoord).r;
    float ndc_depth = device_depth * 2.0 - 1.0;
    vec4 view_position = inv_proj * vec4(0.0, 0.0, ndc_depth, 1.0);
    float view_depth = view_position.z / view_position.w;
    // Firestorm's camera-space formula is positive in front of focus and
    // negative behind it. Store the module's explicit convention instead:
    // negative foreground, positive background.
    float opaque_coc = clamp(-calculateCoC(view_depth) / max(max_coc, 0.0001), -1.0, 1.0);
    float effective_coc = opaque_coc;
    float transparent_coc = opaque_coc;
    float transparent_coverage = 0.0;

    if (has_transparent_depth != 0)
    {
        transparent_coverage = texture(noiseMap, vary_fragcoord).a;
        float transparent_device_depth = texture(lightMap, vary_fragcoord).r;
        // Ignore cleared pixels and transparent fragments hidden by opaque
        // geometry. Coverage smoothly controls how much the visible color is
        // associated with the transparent surface rather than what lies behind.
        if (transparent_coverage > 0.0 && transparent_device_depth < device_depth - 0.0000001)
        {
            float transparent_ndc_depth = transparent_device_depth * 2.0 - 1.0;
            vec4 transparent_position = inv_proj * vec4(0.0, 0.0, transparent_ndc_depth, 1.0);
            float transparent_view_depth = transparent_position.z / transparent_position.w;
            transparent_coc = clamp(-calculateCoC(transparent_view_depth) /
                                    max(max_coc, 0.0001), -1.0, 1.0);
            float influence = smoothstep(0.05, 0.65, transparent_coverage);
            effective_coc = mix(opaque_coc, transparent_coc, influence);
        }
        else
        {
            transparent_coverage = 0.0;
        }
    }

    frag_coc = vec4(effective_coc, opaque_coc,
                    transparent_coc, transparent_coverage);
}
