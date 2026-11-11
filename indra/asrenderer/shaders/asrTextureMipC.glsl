/**
 * asrTextureMipC.glsl
 * One mip of a texture being compressed, from the one before it: a 2x2 box, as the linear
 * blit made the mips of an uncompressed texture. view is the larger mip, instances the
 * smaller one, list the larger one's width and height (see asrEncodeCommon.glsl).
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrEncodeCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

void main()
{
    const uvec2 size = encodeSize();
    const uvec2 next = max(size / 2u, uvec2(1u));
    const uvec2 at = gl_GlobalInvocationID.xy;
    if (at.x >= next.x || at.y >= next.y)
    {
        return;
    }
    EncodeTexelsRef source = EncodeTexelsRef(push.view);
    EncodeWordsRef target = EncodeWordsRef(push.instances);
    vec4 sum = vec4(0.0);
    for (uint i = 0u; i < 4u; ++i)
    {
        const uvec2 from = min(at * 2u + uvec2(i & 1u, i >> 1), size - 1u);
        sum += unpackUnorm4x8(source.texels[from.y * size.x + from.x]);
    }
    target.words[at.y * next.x + at.x] = packUnorm4x8(sum * 0.25);
}
