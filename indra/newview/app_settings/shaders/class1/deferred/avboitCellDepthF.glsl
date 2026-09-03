/**
 * @file avboitCellDepthF.glsl
 * @author chanayane@firestorm
 * @brief AVBOIT pass 1's occupancy raster runs at one fragment per 8x8-pixel
 *        volume cell, but relies on hardware early_fragment_tests against
 *        the FULL-RESOLUTION opaque depth buffer for its cull test. Testing
 *        cell (x,y) against opaque depth at pixel (x,y) is the wrong sample
 *        entirely -- it must test against the farthest (most conservative)
 *        opaque depth across the whole 8x8 block that cell covers. This
 *        pass runs once per cell, ahead of pass 1, to bake that per-cell
 *        farthest depth into a small volume-resolution depth target, so
 *        pass 1's ordinary hardware depth test becomes correct without any
 *        manual per-fragment re-test. See doc/ayanestorm-oit-performance-
 *        audit-plan.md A2.
 */

uniform sampler2D avboitOpaqueDepthSampler;
uniform ivec2 avboitViewport;

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * 8;
    float farthest = 0.0;
    for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x)
    {
        ivec2 sample_pixel = min(
            base + ivec2(x, y), avboitViewport - ivec2(1));
        farthest = max(
            farthest, texelFetch(avboitOpaqueDepthSampler, sample_pixel, 0).r);
    }
    gl_FragDepth = farthest;
}
