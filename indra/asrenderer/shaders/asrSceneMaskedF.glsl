/**
 * asrSceneMaskedF.glsl
 * Writes which triangle covers each pixel, unless the texture has a hole there. A hole
 * leaves no depth either, so it hides nothing behind it from occlusion culling.
 */
#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];

layout(location = 0) flat in uint vary_visibility;
layout(location = 1) in vec2 vary_uv;
layout(location = 2) flat in uint vary_texture;
layout(location = 3) flat in vec2 vary_alpha;
layout(location = 0) out uint frag_visibility;

void main()
{
    const uint slot = vary_texture & 0xFFFFFu;
    const uint sampler_index = (vary_texture >> 28) & 3u;
    const float alpha = texture(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(sampler_index)]), vary_uv).a;
    if (alpha * vary_alpha.x < vary_alpha.y)
    {
        discard;
    }
    frag_visibility = vary_visibility;
}
