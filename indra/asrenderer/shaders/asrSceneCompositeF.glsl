/**
 * asrSceneCompositeF.glsl
 * The scene colour into the window, at the world view's place in it: push.list holds that
 * offset, x low, y high, as the viewport does.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(set = 1, binding = 9, rgba16f) uniform readonly image2D sceneColor;

layout(location = 0) out vec4 frag_color;

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy) - ivec2(push.list & 0xFFFFu, push.list >> 16);
    frag_color = vec4(imageLoad(sceneColor, pixel).rgb, 1.0);
}
