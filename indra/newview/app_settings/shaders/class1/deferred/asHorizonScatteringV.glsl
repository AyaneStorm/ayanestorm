/**
 * @file asHorizonScatteringV.glsl
 * @author chanayane@firestorm
 * @brief AyaneStorm analytic horizon-scattering sky-dome vertex shader.
 */

uniform mat4 modelview_projection_matrix;
uniform vec3 camPosLocal;

in vec3 position;
out vec3 vary_horizon_direction;

vec3 safeNormalize(vec3 value, vec3 fallback)
{
    float magnitude_squared = dot(value, value);
    return magnitude_squared > 1e-8
         ? value * inversesqrt(magnitude_squared)
         : fallback;
}

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
    // Use the true EEP direction, matching the zero-offset procedural sun.
    vary_horizon_direction = safeNormalize(
        position - camPosLocal,
        vec3(0.0, 1.0, 0.0));
}
