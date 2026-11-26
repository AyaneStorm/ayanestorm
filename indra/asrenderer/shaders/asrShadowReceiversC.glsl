/**
 * asrShadowReceiversC.glsl
 * Which shadow map tiles this frame's visible pixels read: from the final depth, each pixel
 * finds the cascade asr_shadow_sun() will read for it and marks the tiles its filter can
 * reach (GpuShadowViews::receivers). The moving cull then draws a moving caster into a
 * cascade only where its footprint touches a marked tile: an avatar whose shadow no visible
 * surface reads in that cascade is not drawn there. A workgroup gathers its marks in shared
 * memory, then adds them with one atomic per word.
 *
 * Mac OIT's layers are not in the depth: a blended layer over the sky marks nothing, so a
 * moving caster's shadow on it alone is not drawn.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 1, binding = 3) uniform texture2D depthImage;

// Texels around the point a lookup can read: the widest search or filter, and the normal
// offset's 1.5. Near a cascade's edge asr_shadow_sun() reads the next one too (SHADOW_BLEND).
const float RECEIVER_REACH = SHADOW_FILTER_MARGIN + 1.5;
const float RECEIVER_BLEND = 0.08; // asrShadow.glsl's SHADOW_BLEND

shared uint sMask[SHADOW_CASCADES * SHADOW_TILES];

void main()
{
    ViewRef view = ViewRef(push.view);
    ShadowViewsRef shadows = ShadowViewsRef(view.shadowViews);
    for (uint word = gl_LocalInvocationIndex; word < SHADOW_CASCADES * SHADOW_TILES; word += 256u)
    {
        sMask[word] = 0u;
    }
    barrier();

    const uint count = view.shadowCascades == 0u ? 0u : min(shadows.count, SHADOW_CASCADES);
    const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    const float depth = count != 0u && all(lessThan(vec2(pixel), view.pixelSize.xy)) ? texelFetch(depthImage, pixel, 0).r : 0.0;
    if (depth > 0.0)
    {
        // Back to the camera's space, as the cull's pyramid test projects (reversed depth is
        // near / distance), then to world directions, relative to the camera.
        const float along = view.projection.z / depth;
        const vec2 uv = (vec2(pixel) + 0.5) * view.pixelSize.zw;
        const vec3 eye = vec3((uv.x * 2.0 - 1.0) * along / view.projection.x, (1.0 - uv.y * 2.0) * along / view.projection.y, -along);
        const vec3 relative = eye.x * view.viewRows[0].xyz + eye.y * view.viewRows[1].xyz + eye.z * view.viewRows[2].xyz;
        const float size = view.shadowResolution;
        const float tile = size / float(SHADOW_TILES);
        bool blending = false; // the last cascade marked was read near its edge: the next one is read too
        for (uint i = 0u; i < count; ++i)
        {
            const vec4 clip = shadows.views[i].viewProjection * vec4(relative, 1.0);
            const vec2 texel = (clip.xy * 0.5 + 0.5) * size;
            const vec2 inside = min(texel, vec2(size) - texel) - SHADOW_FILTER_MARGIN; // as shadowPlace()
            if (min(inside.x, inside.y) < 0.0 || clip.z < 0.0)
            {
                continue;
            }
            const uint columns = shadowTileBits(texel.x - RECEIVER_REACH, texel.x + RECEIVER_REACH, tile);
            const int first = max(int(floor((texel.y - RECEIVER_REACH) / tile)), 0);
            const int last = min(int(ceil((texel.y + RECEIVER_REACH) / tile)) - 1, int(SHADOW_TILES) - 1);
            for (int row = first; row <= last; ++row)
            {
                atomicOr(sMask[i * SHADOW_TILES + uint(row)], columns);
            }
            if (blending)
            {
                break; // the outer cascade of a blend
            }
            blending = min(inside.x, inside.y) < RECEIVER_BLEND * size;
            if (!blending)
            {
                break; // the finest cascade holding it is the one read
            }
        }
    }
    barrier();

    for (uint word = gl_LocalInvocationIndex; word < SHADOW_CASCADES * SHADOW_TILES; word += 256u)
    {
        if (sMask[word] != 0u)
        {
            atomicOr(shadows.receivers[word], sMask[word]);
        }
    }
}
