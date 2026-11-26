/**
 * asrShadowShiftF.glsl
 * The static cache into the working map, each texel taken from where its world texel was in
 * last frame's window (the cascade scrolled by origin - previousOrigin), and empty depth in
 * the tiles the fit marked dirty: static casters are drawn there next. Strips that scrolled
 * into view are always dirty, so every texel read is inside the cache.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#include "asrSceneCommon.glsl"

layout(set = 1, binding = 21) uniform texture2DArray shadowCache;

layout(location = 0) flat in uint vary_layer;

void main()
{
    ViewRef view = ViewRef(push.view);
    ShadowViewsRef shadows = ShadowViewsRef(view.shadowViews);
    const uint v = vary_layer;
    const ivec2 texel = ivec2(gl_FragCoord.xy);
    const uvec2 tile = uvec2(vec2(texel) * (float(SHADOW_TILES) / view.shadowResolution));
    if ((shadows.dirty[v * SHADOW_TILES + tile.y] & (1u << tile.x)) != 0u)
    {
        gl_FragDepth = 0.0; // the side away from the light: nothing cast
        return;
    }
    const ivec2 from = texel + shadows.views[v].origin - shadows.views[v].previousOrigin;
    gl_FragDepth = texelFetch(shadowCache, ivec3(from, int(v)), 0).r;
}
