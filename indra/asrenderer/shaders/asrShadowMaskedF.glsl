/**
 * asrShadowMaskedF.glsl
 * A shadow caster's holes, masked and blended alike, as OpenGL's shadowAlphaMaskF.glsl: the
 * texture's alpha is cut at the material's cutoff, then taken with the colour's alpha; below
 * 0.05 nothing is written, and below 0.88 every other column, a half-dense shadow.
 */
#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];

layout(location = 1) in vec2 vary_uv;
layout(location = 2) flat in uint vary_texture;
layout(location = 3) flat in vec2 vary_alpha;

void main()
{
    const uint slot = vary_texture & 0xFFFFFu;
    const uint sampler_index = (vary_texture >> 28) & 3u;
    float alpha = texture(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(sampler_index)]), vary_uv).a;
    if (alpha < vary_alpha.y)
    {
        discard;
    }
    alpha *= vary_alpha.x;
    if (alpha < 0.05 || (alpha < 0.88 && (uint(gl_FragCoord.x) & 1u) == 0u))
    {
        discard;
    }
}
