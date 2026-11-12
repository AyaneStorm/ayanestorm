/**
 * asrOverlayDepthF.glsl
 * The scene's depth into the window-sized UI depth, in the world view, so the UI's
 * world part (manipulators, beacons, parcel lines: see asruilist.h) tests against it as
 * OpenGL's render_hud_elements() does. Both are reversed depth with no far plane.
 * Phase 1 writes farthest instead: Metal clears the depth this way after that part, as it
 * has no clear inside a render pass.
 * Author: chanayane@firestorm
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#include "asrSceneCommon.glsl"

layout(set = 1, binding = 3) uniform texture2D depthImage;

void main()
{
    if (push.phase == 1u)
    {
        gl_FragDepth = 0.0;
        return;
    }
    // The window pixel, less the world view's place in it (push.list, x low, y high).
    const ivec2 pixel = ivec2(gl_FragCoord.xy) - ivec2(push.list & 0xFFFFu, push.list >> 16);
    // Drawn in the world view, which is the scene's size: the pixel is always inside.
    gl_FragDepth = texelFetch(depthImage, pixel, 0).r;
}
