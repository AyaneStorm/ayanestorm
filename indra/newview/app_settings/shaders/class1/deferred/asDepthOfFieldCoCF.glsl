/**
 * @file asDepthOfFieldCoCF.glsl
 * @author chanayane@firestorm
 * @brief Signed physical circle-of-confusion preparation for AyaneStorm DoF.
 */
out float frag_coc;

uniform sampler2D depthMap;
uniform mat4 inv_proj;
uniform float focal_distance;
uniform float blur_constant;
uniform float tan_pixel_angle;
uniform float magnification;
uniform float max_coc;

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
    frag_coc = clamp(-calculateCoC(view_depth) / max(max_coc, 0.0001), -1.0, 1.0);
}
