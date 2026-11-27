/**
 * asrShadowShiftF.glsl
 * The static cache into the working map, each texel taken from where its world texel was in
 * last frame's window (the cascade scrolled by origin - previousOrigin), and empty depth in
 * the tiles the fit marked dirty: static casters are drawn there next. Strips that scrolled
 * into view are always dirty, so every texel read is inside the cache. The opacity map
 * (asrShadowAlpha.glsl) moves with it, from its own cache.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#include "asrSceneCommon.glsl"

layout(set = 1, binding = 21) uniform texture2DArray shadowCache;
layout(set = 1, binding = 23) uniform texture2DArray opacityCache;

layout(location = 0) out vec2 frag_opacity;

layout(location = 0) flat in uint vary_layer;

void main()
{
    ViewRef view = ViewRef(push.view);
    ShadowViewsRef shadows = ShadowViewsRef(view.shadowViews);
    const uint v = vary_layer;
    const ivec2 texel = ivec2(gl_FragCoord.xy);
    if (view.shadowUpdate == 0u)
    {
        // Between updates: the cache as it is, under this frame's moving casters. Dirty marks
        // gathered meanwhile wait for the next update.
        gl_FragDepth = texelFetch(shadowCache, ivec3(texel, int(v)), 0).r;
        frag_opacity = texelFetch(opacityCache, ivec3(texel, int(v)), 0).rg;
        return;
    }
    const uvec2 tile = uvec2(vec2(texel) * (float(SHADOW_TILES) / view.shadowResolution));
    if ((shadows.dirty[v * SHADOW_TILES + tile.y] & (1u << tile.x)) != 0u)
    {
        gl_FragDepth = 0.0; // the side away from the light: nothing cast
        frag_opacity = vec2(0.0);
        return;
    }
    const ivec2 from = texel + shadows.views[v].origin - shadows.views[v].previousOrigin;
    gl_FragDepth = texelFetch(shadowCache, ivec3(from, int(v)), 0).r;
    frag_opacity = texelFetch(opacityCache, ivec3(from, int(v)), 0).rg;
}
