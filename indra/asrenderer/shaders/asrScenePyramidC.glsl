/**
 * asrScenePyramidC.glsl
 * Builds three levels of the depth pyramid from one, in one dispatch, so the whole chain
 * takes only a few dispatches and few barriers. A thread stands for one texel of the
 * coarsest of the three, and reads the 8x8 under it. Each level keeps the farthest depth,
 * which in reversed depth is the smallest value, so a test against it is never too eager.
 * push.phase is the level to read, and level 0 is the depth buffer.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 1, binding = 3) uniform texture2D depthImage;

layout(buffer_reference, std430) buffer PyramidRef { float values[]; };

const float NOTHING = 1.0; // the nearest value: past the image's edge it changes no minimum

void main()
{
    ViewRef view = ViewRef(push.view);
    PyramidRef pyramid = PyramidRef(push.pyramid);
    const uvec2 size = uvec2(view.pixelSize.xy);
    const uint source = push.phase;
    const uvec2 sourceSize = uvec2(pyramidLevelSize(size.x, source), pyramidLevelSize(size.y, source));
    const uvec2 texel = gl_GlobalInvocationID.xy;

    float values[64];
    for (uint y = 0u; y < 8u; ++y)
    {
        for (uint x = 0u; x < 8u; ++x)
        {
            const uvec2 at = texel * 8u + uvec2(x, y);
            float value = NOTHING;
            if (at.x < sourceSize.x && at.y < sourceSize.y)
            {
                value = source == 0u ? texelFetch(depthImage, ivec2(at), 0).r
                                     : pyramid.values[view.pyramidOffset[source - 1u] + at.y * sourceSize.x + at.x];
            }
            values[y * 8u + x] = value;
        }
    }

    uint blocks = 8u;
    for (uint level = source + 1u; level <= min(source + 3u, view.pyramidLevels); ++level)
    {
        const uint halved = blocks >> 1;
        const uvec2 levelSize = uvec2(pyramidLevelSize(size.x, level), pyramidLevelSize(size.y, level));
        const uint offset = view.pyramidOffset[level - 1u];
        for (uint y = 0u; y < halved; ++y)
        {
            for (uint x = 0u; x < halved; ++x)
            {
                const uint at = y * 2u * blocks + x * 2u;
                const float value = min(min(values[at], values[at + 1u]), min(values[at + blocks], values[at + blocks + 1u]));
                values[y * halved + x] = value;
                const uvec2 target = texel * halved + uvec2(x, y);
                if (target.x < levelSize.x && target.y < levelSize.y)
                {
                    pyramid.values[offset + target.y * levelSize.x + target.x] = value;
                }
            }
        }
        blocks = halved;
    }
}
