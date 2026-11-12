/**
 * asrOverlayHudF.glsl
 * HUD objects, unlit as OpenGL draws them (gHUDFullbrightProgram): texture times colour,
 * in display colours after tone mapping, as the UI is. Opaque ones keep no alpha, masked
 * ones cut theirs, blended ones blend in the list's back-to-front order. Their textures
 * report the size they show at, as the scene's do.
 * Author: chanayane@firestorm
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "asrSceneCommon.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];

layout(location = 0) in vec2 vary_uv;
layout(location = 1) flat in uint vary_texture;
layout(location = 2) flat in vec4 vary_color;
layout(location = 3) flat in uint vary_flags;
layout(location = 4) flat in float vary_cutoff;

layout(location = 0) out vec4 frag_color;

void main()
{
    // One draw takes one-sided and double-sided faces alike, so culling is off and backs
    // go here: a translucent panel would show its mirrored back through its front.
    if (!gl_FrontFacing && (vary_flags & INSTANCE_DOUBLE_SIDED) == 0u)
    {
        discard;
    }
    const uint slot = vary_texture & 0xFFFFFu;
    const uint samplerIndex = (vary_texture >> 28) & 3u;
    const vec2 uvDx = dFdx(vary_uv);
    const vec2 uvDy = dFdy(vary_uv);
    vec4 color = vary_color * texture(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(samplerIndex)]), vary_uv);
    if ((vary_flags & INSTANCE_MASKED) != 0u && color.a < vary_cutoff)
    {
        discard;
    }
    giveFeedback(ivec2(gl_FragCoord.xy), slot, uvDx, uvDy);
    if ((vary_flags & INSTANCE_BLENDED) == 0u)
    {
        color.a = 1.0;
    }
    frag_color = color;
}
