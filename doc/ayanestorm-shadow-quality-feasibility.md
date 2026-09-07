# AyaneStorm Shadow Quality and Performance Feasibility

Author: chanayane@firestorm

## Current Implementation

Source inspection of `indra/newview/pipeline.cpp` shows four directional shadow-map allocations sized from viewport resolution and RenderShadowResolutionScale, plus two square spotlight maps at higher shadow detail. `generateSunShadow()` renders cascades subject to visibility and split conditions; `renderShadow()` performs culling, sorting, and geometry rendering. Reflection-probe shadow rendering has a separate two-cascade path.

`app_settings/shaders/class1/deferred/shadowUtil.glsl` uses five shadow comparison texture lookups per directional filter evaluation, with weighted center, explicit X-coordinate snapping, and screen-dependent jitter. Spotlight filtering also uses five comparisons. Hardware comparison filtering may perform multiple underlying depth comparisons per lookup. The deferred pipeline also contains shadow/SSAO blur processing.

These are potential quality and cost factors, not measured explanations of the user's current scene. No profiling or runtime settings inspection was performed.

## Recommended Investigation Order

1. Profile shadow-map generation separately from shadow evaluation/blur. Capture representative avatar, foliage, indoor, and outdoor scenes.
2. Audit cascade coverage, split placement, and projection stability to concentrate existing texels near the camera and reduce movement shimmer. Verify existing fitting before replacing it.
3. Compare an efficient, stable PCF kernel against current snapping/jitter at similar sampling cost. Adjust bias carefully to retain contacts without self-shadow artifacts. Filtering cannot recover geometry absent from the maps.
4. Investigate tighter caster culling and less expensive distant cascades. Cached or staggered updates require correct invalidation for camera, sun, moving objects, avatars, and animated alpha textures; careless reuse causes lagging shadows.
5. Consider optional contact shadows for visible fine detail or PCSS for contact-dependent softness only after baseline optimization. These add work and are not automatic performance improvements. Screen-space detail cannot represent offscreen occluders.

## Feasibility and Limits

Better quality is feasible within the existing raster shadow architecture. Simultaneous speed gains need measurement and are not guaranteed. Doubling both map dimensions quadruples texel storage and potentially raster work, but not necessarily total frame cost. Ray tracing or virtual shadow maps would be much larger renderer projects, with no established performance benefit for this viewer.

Place substantial new functionality in a dedicated AyaneStorm module and keep upstream integration minimal and ownership-tagged. Changes must account for opaque, alpha-tested, transparent, and volumetric shadow consumers.

## Primary References

- Microsoft, Cascaded Shadow Maps: https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps — perspective aliasing, cascade placement, projection stability, and filtering tradeoffs.
- NVIDIA, Shadow Map Antialiasing: https://developer.nvidia.com/gpugems/gpugems/part-ii-lighting-and-shadows/chapter-11-shadow-map-antialiasing — efficient percentage-closer filtering.
- NVIDIA, Percentage-Closer Soft Shadows: https://developer.download.nvidia.com/SDK/9.5/Samples/MEDIA/docPix/docs/PCSS.pdf — blocker search and variable-width filtering for approximate soft shadows.

No implementation changes or builds were performed.

## Reported Ground-Related Shadow Disappearance

The user reports shadows being discarded when any part of an avatar is not above ground, and confirms ordinary objects are affected too. Exact geometry trigger remains unconfirmed: surface penetration versus overhanging an edge, terrain versus prim/mesh floors, and whole-shadow versus partial disappearance require clarification.

Initial source inspection found no explicit ground-height rejection in `LLDrawPoolAvatar::renderShadow()`. The shared directional pipeline fits cascades from `getVisiblePointCloud()` / `getVisibleExtents()` and clears a cascade when no possible receivers are found. Caster frustum setup also replaces the near plane. These are investigation points, not confirmed causes. No fix has been made; do not assume avatar-specific culling or increase resolution as a remedy without reproduction.
