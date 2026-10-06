/**
 * @file asmilkywayV.glsl
 * @author chanayane@firestorm
 * @brief AyaneStorm real-sky Milky Way glow, sky-dome vertex shader.
 */

uniform mat4 modelview_projection_matrix;
uniform vec3 camPosLocal;

in vec3 position;
// View direction in the agent frame (x east, y north, z up).
out vec3 vary_mw_direction;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
    // The WL dome is Y-up; renderDome()'s 120 degree rotation about (1,1,1)
    // maps dome x -> north, y -> up, z -> east.
    vec3 d = position - camPosLocal;
    vary_mw_direction = vec3(d.z, d.x, d.y);
}
