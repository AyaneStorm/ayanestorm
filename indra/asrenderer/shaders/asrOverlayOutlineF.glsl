/**
 * asrOverlayOutlineF.glsl
 * Selection outlines, from the mask (see asrOverlayMaskF.glsl), blended over the image
 * after tone mapping: around each selected shape, a band outlineWidth pixels wide that
 * fades outward, in the colour of the nearest selected pixel. Parts behind other objects
 * show at 0.4, as OpenGL's hidden selection lines, when hiddenSelections is set; else they
 * are not outlined. Faces the face tool selected are tinted. Scene phase: the scene's
 * selections, drawn before the HUD; HUD phase: the HUD's own, drawn over it.
 * Author: chanayane@firestorm
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(set = 1, binding = 10, rgba8) uniform readonly image2D selectionMask;

layout(location = 0) out vec4 frag_color;

const uint OVERLAY_OUTLINE_SCENE = 0u; // push.phase
const uint OVERLAY_OUTLINE_HUD = 1u;
const int MAX_OUTLINE_RADIUS = 4;      // the search square's half size, in pixels
const float HIDDEN_STRENGTH = 0.4;

// How strongly a mask texel counts as selected, and in which colour (1 root, 0.5 child).
float coverage(vec4 mask, bool hud, bool hidden, out float colour)
{
    if (hud)
    {
        colour = mask.a;
        return mask.a > 0.0 ? 1.0 : 0.0;
    }
    colour = mask.g;
    if (mask.r > 0.75)
    {
        return 1.0;
    }
    return mask.r > 0.0 && hidden ? HIDDEN_STRENGTH : 0.0;
}

void main()
{
    ViewRef view = ViewRef(push.view);
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const ivec2 size = imageSize(selectionMask);
    const bool hud = push.phase == OVERLAY_OUTLINE_HUD;
    const bool hidden = view.hiddenSelections != 0u;
    const vec4 own = imageLoad(selectionMask, pixel);

    float ownColour = 0.0;
    if (coverage(own, hud, hidden, ownColour) > 0.0)
    {
        // Inside a selected shape: only the face tool's tint shows here.
        if (!hud && own.b > 0.0)
        {
            frag_color = view.selectionColors[SELECTION_COLOR_FACE];
            return;
        }
        discard;
    }

    // Outside: the nearest selected pixel within the band decides colour and fade.
    const float width = clamp(view.outlineWidth, 1.0, float(MAX_OUTLINE_RADIUS));
    const int radius = int(ceil(width));
    float best = width + 1.0;
    float strength = 0.0;
    float colour = 0.0;
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            const ivec2 at = pixel + ivec2(x, y);
            if (any(lessThan(at, ivec2(0))) || any(greaterThanEqual(at, size)))
            {
                continue;
            }
            float nearColour = 0.0;
            const float near = coverage(imageLoad(selectionMask, at), hud, hidden, nearColour);
            const float distance = length(vec2(x, y));
            // The nearer pixel wins; at the same distance, the stronger and the root's.
            if (near > 0.0 && (distance < best || (distance == best && (near > strength || nearColour > colour))))
            {
                best = distance;
                strength = near;
                colour = nearColour;
            }
        }
    }
    if (strength == 0.0 || best > width)
    {
        discard;
    }
    const vec4 tint = view.selectionColors[colour > 0.75 ? SELECTION_COLOR_ROOT : SELECTION_COLOR_CHILD];
    const float fade = 1.0 - (best - 1.0) / width; // full at the edge, gone past the band
    frag_color = vec4(tint.rgb, tint.a * strength * clamp(fade, 0.0, 1.0));
}
