/**
 * @file asweatherSnowRenderV.glsl
 * @author chanayane@firestorm
 * @brief Native LLVertexBuffer snow billboard transform.
 */

uniform mat4 modelview_projection_matrix;
in vec3 position;
in vec2 texcoord0;
in vec4 diffuse_color;

out vec2 snow_uv;
out vec4 snow_color;

void main()
{
    snow_uv = texcoord0;
    snow_color = diffuse_color;
    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
}
