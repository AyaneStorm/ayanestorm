/**
 * @file asauroraV.glsl
 * @author chanayane@firestorm
 * @brief AyaneStorm procedural aurora sky-dome vertex shader.
 */

uniform mat4 modelview_projection_matrix;
uniform vec3 camPosLocal;

in vec3 position;
out vec3 vary_aurora_direction;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
    vary_aurora_direction = normalize(position - camPosLocal + vec3(0.0, 50.0, 0.0));
}
