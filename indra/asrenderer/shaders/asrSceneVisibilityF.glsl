/**
 * asrSceneVisibilityF.glsl
 * Writes which triangle covers each pixel, and nothing else.
 */
#version 460

layout(location = 0) flat in uint vary_visibility;
layout(location = 0) out uint frag_visibility;

void main()
{
    frag_visibility = vary_visibility;
}
