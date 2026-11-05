/**
 * asrTestTriangleF.glsl
 * Test triangle for asrenderer_test. Its brightness pulses with time, which
 * proves per-frame constants reach the shader on every backend.
 */
#version 460

layout(push_constant) uniform FrameConstants
{
    float seconds;
} frame_constants;

layout(location = 0) in vec3 vary_color;
layout(location = 0) out vec4 frag_color;

void main()
{
    float pulse = 0.6 + 0.4 * sin(frame_constants.seconds * 3.0);
    frag_color = vec4(vary_color * pulse, 1.0);
}
