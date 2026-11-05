/**
 * asrUIF.glsl
 * The one fragment shader of the native UI. It does the work of both gUIProgram
 * and gSolidColorProgram, and each vertex chooses its texture and sampler, so
 * neither a texture change nor that choice splits a draw. The packing is
 * uiTextureBits() in asruilist.h, and the array size is MAX_TEXTURES.
 */
#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];

layout(location = 0) in vec2 vary_texcoord;
layout(location = 1) in vec4 vary_color;
layout(location = 2) flat in uint vary_texture_bits;

layout(location = 0) out vec4 frag_color;

void main()
{
    const uint slot = vary_texture_bits & 0xFFFFFu;
    const uint sampler_index = (vary_texture_bits >> 28) & 3u;
    const vec4 texel = texture(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(sampler_index)]), vary_texcoord);

    if ((vary_texture_bits & 0x80000000u) != 0u)
    {
        frag_color = vec4(vary_color.rgb, texel.a * vary_color.a);
    }
    else
    {
        frag_color = vary_color * texel;
    }
}
