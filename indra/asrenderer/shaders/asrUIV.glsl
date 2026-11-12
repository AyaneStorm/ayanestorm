/**
 * asrUIV.glsl
 * The one vertex shader of the native UI. Vertices arrive already transformed, so
 * it holds no matrix and needs no per-draw data (see asruilist.h).
 */
#version 460

layout(location = 0) in vec4 position; // clip space, with GL's axes; world space with UI_WORLD_BIT
layout(location = 1) in vec2 texcoord;
layout(location = 2) in vec4 color;
layout(location = 3) in uint texture_bits;

layout(location = 0) out vec2 vary_texcoord;
layout(location = 1) out vec4 vary_color;
layout(location = 2) flat out uint vary_texture_bits;

// World space to the window, through the camera the scene is drawn with this frame and
// the world view's place in the window: the renderer's axes and reversed depth.
layout(push_constant) uniform UIPush
{
    mat4 worldToWindow;
} push;

const uint UI_WORLD_BIT = 1u << 27; // as asruilist.h

void main()
{
    if ((texture_bits & UI_WORLD_BIT) != 0u)
    {
        gl_Position = push.worldToWindow * position;
    }
    else
    {
        // GL has Y up and Z from -1 to 1. Vulkan has Y down and Z from 0 to 1. The
        // Metal shader build flips Y again, because Metal has Y up.
        gl_Position = vec4(position.x, -position.y, (position.z + position.w) * 0.5, position.w);
    }
    gl_PointSize = 1.0;
    vary_texcoord = texcoord;
    vary_color = color;
    vary_texture_bits = texture_bits;
}
