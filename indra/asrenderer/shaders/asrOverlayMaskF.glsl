/**
 * asrOverlayMaskF.glsl
 * The selection mask, drawn with the MAX blend so each pixel keeps the strongest of the
 * fragments on it. Scene phase, from the selection list in the scene's view:
 *   R: outlined here, 1 in front of the scene's depth, 0.5 behind it
 *   G: the outline colour, 1 the root's, 0.5 a linked child's (the root wins)
 *   B: 1 where a face the face tool selected shows: tinted
 * HUD phase, from the HUD list's selected entries in the HUD view, no depth:
 *   A: as G, for the HUD's own outline
 * The outline pass reads it, see asrOverlayOutlineF.glsl.
 * Author: chanayane@firestorm
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require
#include "asrSceneCommon.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];
layout(set = 1, binding = 3) uniform texture2D depthImage;

layout(location = 0) in vec2 vary_uv;
layout(location = 1) flat in uint vary_texture;
layout(location = 2) flat in vec4 vary_color;
layout(location = 3) flat in uint vary_flags;
layout(location = 4) flat in float vary_cutoff;

layout(location = 0) out vec4 frag_mask;

const uint OVERLAY_MASK_SCENE = 0u; // push.phase
const uint OVERLAY_MASK_HUD = 1u;

void main()
{
    // Holes cut by the texture are no part of the outline, as they are none of the scene.
    if ((vary_flags & INSTANCE_MASKED) != 0u)
    {
        const uint slot = vary_texture & 0xFFFFFu;
        const uint samplerIndex = (vary_texture >> 28) & 3u;
        const float alpha = vary_color.a * texture(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(samplerIndex)]), vary_uv).a;
        if (alpha < vary_cutoff)
        {
            discard;
        }
    }
    const bool outlined = (vary_flags & (INSTANCE_SELECTED | INSTANCE_SELECTED_CHILD)) != 0u;
    const float colour = !outlined ? 0.0 : (vary_flags & INSTANCE_SELECTED) != 0u ? 1.0 : 0.5;
    if (push.phase == OVERLAY_MASK_HUD)
    {
        frag_mask = vec4(0.0, 0.0, 0.0, colour);
        return;
    }
    // Reversed depth: nearer is larger. The same vertices as the scene's, and an invariant
    // position, so its own surface ties; the margin takes rounding.
    const float sceneDepth = texelFetch(depthImage, ivec2(gl_FragCoord.xy), 0).r;
    const bool visible = gl_FragCoord.z * 1.00001 >= sceneDepth;
    const bool face = visible && (vary_flags & INSTANCE_FACE_SELECTED) != 0u;
    frag_mask = vec4(!outlined ? 0.0 : visible ? 1.0 : 0.5, colour, face ? 1.0 : 0.0, 0.0);
}
