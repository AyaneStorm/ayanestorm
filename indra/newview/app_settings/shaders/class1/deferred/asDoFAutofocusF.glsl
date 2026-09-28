/**
 * @file asDoFAutofocusF.glsl
 * @author chanayane@firestorm
 * @brief Depth-of-field autofocus samples (see asdofautofocus.cpp).
 */
// Drawn into a 64 x 33 R32F target read back by the CPU. Rows 0-31: one
// scene distance (metres along the view axis) per texel, at 2048 points in
// a Gaussian pattern over the autofocus area, denser at its centre. Row 32:
// the scene distance at the tracked eyes (occlusion probe).
layout(location = 0) out vec4 frag_color;

uniform sampler2D depthMap;
uniform mat4 inv_proj;
uniform vec4 af_rect; // area in scene UV: min xy, max xy
uniform vec2 eye_uv;

const int GRID_W = 64;
const int GRID_H = 32;
const float SAMPLE_COUNT = 2048.0;

float viewDistance(vec2 uv)
{
    float device_depth = texture(depthMap, clamp(uv, 0.0, 1.0)).r;
    vec4 p = inv_proj * vec4(0.0, 0.0, device_depth * 2.0 - 1.0, 1.0);
    return -p.z / p.w;
}

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    if (texel.y >= GRID_H)
    {
        frag_color = vec4(viewDistance(eye_uv));
        return;
    }
    // Quasi-random angle (golden ratio sequence) and stratified radius,
    // Box-Muller mapped to a Gaussian with sigma = 1/3 of the half area.
    float id = float(texel.y * GRID_W + texel.x);
    float angle = 6.28318531 * fract(0.5 + id * 0.61803399);
    float radius = sqrt(-2.0 * log(1.0 - (id + 0.5) / SAMPLE_COUNT)) * 0.33333;
    // The few samples beyond 3 sigma stay on the area's edge.
    vec2 g = clamp(vec2(cos(angle), sin(angle)) * radius * 0.5 + 0.5, 0.0, 1.0);
    frag_color = vec4(viewDistance(mix(af_rect.xy, af_rect.zw, g)));
}
