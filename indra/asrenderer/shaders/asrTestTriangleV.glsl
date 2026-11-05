/**
 * asrTestTriangleV.glsl
 * Test triangle for asrenderer_test. Needs no vertex buffer, so it proves the
 * shader build step and pipeline creation on every backend.
 */
#version 460

layout(location = 0) out vec3 vary_color;

const vec2 corners[3] = vec2[3](vec2(0.0, -0.7), vec2(0.7, 0.7), vec2(-0.7, 0.7));
const vec3 colors[3] = vec3[3](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.2, 1.0));

void main()
{
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
    vary_color = colors[gl_VertexIndex];
}
