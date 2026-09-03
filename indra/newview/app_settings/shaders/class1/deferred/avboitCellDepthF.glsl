/**
 * @file avboitCellDepthF.glsl
 * @author chanayane@firestorm
 * @brief AVBOIT pass 1's occupancy raster runs at avboitPass1Subsample
 *        fragments per axis per 8x8-pixel volume cell (round 3; one
 *        fragment per cell before that), but relies on hardware
 *        early_fragment_tests against the FULL-RESOLUTION opaque depth
 *        buffer for its cull test. Testing this pass's own fragment (x,y)
 *        against opaque depth at pixel (x,y) is the wrong sample entirely
 *        -- it must test against the farthest (most conservative) opaque
 *        depth across the (8/avboitPass1Subsample)-pixel-square block that
 *        fragment covers. This pass runs once per pass-1 fragment, ahead of
 *        pass 1, to bake that per-block farthest depth into a small target
 *        scaled the same way, so pass 1's ordinary hardware depth test
 *        becomes correct without any manual per-fragment re-test. See
 *        doc/ayanestorm-oit-performance-audit-plan.md A2 and A3 (round 3 in
 *        doc/ayanestorm-oit-avboit-hair-flicker-regression-todo.md).
 */

uniform sampler2D avboitOpaqueDepthSampler;
uniform ivec2 avboitViewport;
uniform int avboitPass1Subsample;

void main()
{
    int block = 8 / avboitPass1Subsample;
    ivec2 base = ivec2(gl_FragCoord.xy) * block;
    float farthest = 0.0;
    for (int y = 0; y < block; ++y)
    for (int x = 0; x < block; ++x)
    {
        ivec2 sample_pixel = min(
            base + ivec2(x, y), avboitViewport - ivec2(1));
        farthest = max(
            farthest, texelFetch(avboitOpaqueDepthSampler, sample_pixel, 0).r);
    }
    gl_FragDepth = farthest;
}
