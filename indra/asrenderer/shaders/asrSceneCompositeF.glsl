/**
 * asrSceneCompositeF.glsl
 * Shows the scene's final colour: the opaque image with Mac OIT's transparency over it.
 * Tone mapping and effects come after this in P4.
 */
#version 460

layout(set = 1, binding = 9, rgba16f) uniform readonly image2D sceneColor;

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = vec4(imageLoad(sceneColor, ivec2(gl_FragCoord.xy)).rgb, 1.0);
}
