# Volumetric lighting: exact (noise-free) redesign research

Status: research / proposal, nothing implemented. 2026-10-09.

Goal (user): no patterns, no ghosts, no tricks, fast, and able to show
crepuscular rays that cross the view (side-on shafts, rays fanning down from
cloud gaps), not only shafts pointing at the camera. The current
implementation stays; a setting switches between the two.

## Why the current raymarch cannot reach "perfect"

Current path: `asVolumetricLightF.glsl` (per-pixel raymarch, 16-64 steps,
one PCF shadow fetch per step), Bayer 4x4 jitter, `asVolumetricCompositeF.glsl`
depth-aware 4x4 box gather.

- Structural: the in-scatter integral is of a discontinuous function (lit /
  shadowed). Any finite-sample estimate is either aliased (fixed offsets ->
  banding/lattice patterns) or noisy (jitter). Removing noise needs a
  reconstruction filter (spatial or temporal), and every filter that spans a
  depth edge produces ghosts/halos. Bayer + box gather + silhouette step
  multiplier (`volumetricNearSilhouette`) are all mitigations of this, not
  fixes. More samples only shrink the error; cost is linear in samples
  (measured High: ~9-10 ms directional).
- `MAX_MARCH_DISTANCE = 128 m`: nothing beyond 128 m scatters, although sun
  shadow cascades reach up to 512 m (`pipeline.cpp` far_clip clamp 16-512 m).
  Distant shafts (the ones seen side-on) are cut off.
- Composite sky handling also assumes 128 m (`SKY_DISTANCE`).
- No cloud occluders: SL clouds are a sky-dome texture, not in any shadow map.
  Rays from cloud gaps (the reference photo) cannot exist in the current model
  whatever the quality.
- HG phase with g = 0.4 (sun): 90-degree scatter is ~6x weaker than forward,
  and output is clamped to [0,1] and normalised by 128 m; side-on shafts end up
  near-invisible even where they exist.
- Shafts are additive only. Real god rays are mostly *darker* haze in shadow
  next to lit haze; SL's own sky/haze in-scatter is never removed in shadowed
  air, so contrast stays low.

## Candidate techniques

| Technique | Noise/pattern | Ghosts | Cost | GL 4.1 | Verdict |
|---|---|---|---|---|---|
| Raymarch + temporal reprojection (UE/Frostbite style) | low | yes (temporal) | medium | yes | rejected: temporal ghosts |
| Froxel grid (Wronski 2014, Hillaire 2015) | blur, low-res shafts | temporal | medium | no compute; slice rendering | rejected: soft shafts, needs TAA |
| Epipolar sampling + 1D min/max trees (Engelhardt 2010, Yusov 2013) | none | none | low | yes, ~10 passes | viable, complex, cascades hard |
| Moment shadow maps prefiltered scattering (Peters 2016) | none | none | low | needs prefix sums | rejected: light leaking (biased) |
| Polygonal light volumes (Billeter et al. 2010, HPG) | none | none | low-medium | yes | **recommended for geometry shadows** |
| Prefiltered march of cloud shadow (band-limited) | none | none | low | yes | **recommended for cloud shafts** |

## Recommended design ("Exact" mode)

### A. Geometry shafts: polygonal light volume (exact analytic integral)

Directional light, homogeneous medium: phase angle is constant along a view
ray, so single scattering is

    L = sigma_s * phase(view, sun) * E_sun * sum over lit intervals [a,b] of g(b) - g(a)
    g(t) = (1 - exp(-sigma_t * t)) / sigma_t

Exact, no sampling. The lit volume is bounded by the shadow-map depth
surface. Render that surface as a grid mesh (one vertex per shadow texel,
vertex shader `texelFetch` of the shadow depth, unprojected with the inverse
shadow matrix) from the camera into an additive RGBA16F/RGBA32F target:

- back-facing fragment (exit of lit region): +g(t)
- front-facing fragment (entry): -g(t)
- hardware depth test against scene depth culls everything behind surfaces
- plus one fullscreen term: +g(t_scene) if the scene point is inside the lit
  volume (same depth map, same comparison as the mesh, not surface PCF)
- camera inside the lit volume needs no special case (g(0) = 0)

Result is exact per shadow texel: hard-edged shafts at any angle, no dither,
no upsampling, no history. Side-on shafts appear wherever the shadow map
covers (up to 512 m), not just within 128 m.

Details to solve:
- Grid skirt: border vertices pushed to the light far plane so the volume is
  closed at the cascade box sides.
- Cascades: render each cascade's mesh clipped (gl_ClipDistance) to its
  view-depth slab; per slab add boundary terms g at slab near/far planes using
  that cascade's lit test (2 point fetches per cascade per pixel). Exact across
  splits, no cross-fade.
- Depth texture is bound with compare mode; mesh needs raw depth -> sampler
  object with `GL_TEXTURE_COMPARE_MODE = GL_NONE` (GL 3.3 core).
- Vertex budget: 4 cascades x shadow res^2. Full-res shadow (~2M texels per
  cascade) is ~8M vertices / 16M triangles per frame - too many. Reduce
  without losing exactness: per-tile min/max of the shadow map; tiles whose
  depth range is flat emit one quad (2 tris) instead of 2*N^2; only
  discontinuity tiles emit full texel grids. Implementation options in 4.1:
  geometry shader per tile, or transform-feedback compaction of
  discontinuity tiles then instanced draw. Needs profiling.
- Precision: sun-facing surfaces lie exactly on the depth surface; lit test
  for the scene point needs a consistent small bias toward the light. Must be
  the same bias the mesh uses or slivers appear.
- Fill cost scales with number of shadow walls in front of the scene
  (forest canopy toward the sun is the worst case); fragments are trivial
  (one exp, additive).
- Sun disc softness: optional, as a penumbra width from the min/max tiles; not
  needed for v1.
- Transparent surfaces: existing atlas approach can stay or be replaced by
  evaluating the same mesh into the atlas slices; open question.

### B. Cloud shafts: cloud shadow from the visible SL clouds

SL cloud UVs come from the sky dome: `llvowlsky.cpp:457`
`uv = ((-z + 1)/2, (-x + 1)/2)` of the unit dome direction, scaled by
`cloud_scale` in `cloudsV.glsl`; density from `cloudsF.glsl` noise textures.
So cloud coverage is a function of direction from the camera. Model the cloud
layer as a camera-centred dome of radius R (km scale). For a point p, project
along the sun direction to the dome, take that direction, evaluate the same
cloud density function as `cloudsF.glsl` -> cloud transmittance. Rays then
originate from the gaps the user actually sees.

Cloud transmittance is low-frequency (noise texture). March it with a fixed
small step count over the full haze range (km), sampling a mip level matched
to the step footprint (prefiltered = band-limited -> no aliasing, no jitter,
no reconstruction filter). Cheap, can run at reduced resolution with a plain
bilinear upsample because the signal has no depth edges except the scene
depth itself (which only clamps the ray end; handled by the same
depth-aware term).

To verify before implementation: cloud scroll uniforms and the exact density
formula in `cloudsF.glsl`; whether the dome mapping is stable enough at low
sun elevation.

### C. Composite: modulate SL haze instead of only adding

Shadowed fraction of the view ray should remove SL atmospheric haze
in-scatter, not only add a separate fog. Composite:

    out = scene * T + scatter_lit - haze_inscatter * shadowed_fraction

`haze_inscatter` from the same atmospheric functions the sky/deferred shaders
use (`calcAtmosphericVars` additive term). This produces the dark/bright
alternation of the reference photo. Needs care to stay >= 0 and to match sky
pixels.

### Phase function

Keep HG but evaluate a two-lobe mix (forward lobe + near-isotropic lobe)
so side-on scatter is not 6x suppressed; remove the [0,1] clamp and the
128 m normalisation (scene is HDR before tonemap).

## Hard requirement: macOS OpenGL 4.1 core

Everything must run on macOS GL 4.1 core (Apple Silicon GL is layered on
Metal). Allowed / forbidden:

- Allowed (<= 4.1 core): sampler objects (3.3), vertex-shader `texelFetch`,
  `gl_VertexID` / `gl_InstanceID` attribute-less draws (VAO must be bound),
  `gl_ClipDistance`, float render targets with additive blending,
  `glDrawArraysIndirect` (4.0), pixel pack buffers, `textureGather` (4.0).
- Forbidden (> 4.1): compute shaders, SSBOs, image load/store, atomic
  counters, `layout(binding=)`, `textureQueryLevels`, base-instance draws,
  `glDrawTransformFeedbackInstanced` (4.2).
- Avoid even though core: geometry shaders and transform feedback - emulated
  and slow on Apple Silicon.

Consequences for the design:

- Tile compaction (A) without compute/GS: HistoPyramid stream compaction
  (Ziegler et al. 2006) built with fragment passes; the discontinuity-tile
  count is written GPU-side into the indirect-draw buffer with
  `glReadPixels` into a buffer bound as `GL_PIXEL_PACK_BUFFER`, then the same
  buffer is bound as `GL_DRAW_INDIRECT_BUFFER` for `glDrawArraysIndirect`.
  No CPU readback, no stall. Each instance walks the pyramid in the vertex
  shader to find its tile. Flat tiles draw from a separate one-quad-per-tile
  pass.
- Accumulation target must be R32F (single scalar integral, colour applied in
  composite): +g/-g pairs cancel, and 16-bit floats lose ~0.25 m at 512 m,
  which shows as per-pixel speckle. R32F additive blending on Apple Silicon
  GL must be verified at runtime first (Metal blendability of R32Float).
  If unsupported: fallback is to be determined; this is a gating check for
  phase 1.
- Validate shaders with `#version 410 core` (already the project rule).

### macOS runtime check result (2026-10-09): PASS

`scripts/as_gl41_volumetric_check.c` (headless CGL, build/run command in its
header), run on Apple M4, macOS 26.6.2, `4.1 Metal - 90.5`:

- R32F framebuffer complete, additive blending incl. negative values: PASS.
- Light-volume +g/-g cancellation (64 intervals, sigma 0.002): R32F error
  0.000011; R16F error 1.46 (confirms R32F is required).
- Fragment pass -> `glReadPixels` into pack buffer -> same buffer as
  `GL_DRAW_INDIRECT_BUFFER` -> `glDrawArraysIndirect`: PASS (instance count 7).
- Vertex-shader and fragment `texelFetch` of D32F/D24 depth textures, with the
  texture in compare mode overridden by a `GL_NONE` sampler object, and of a
  depth texture written by rasterization: PASS.

The R32F gating check is cleared; no fallback needed on Apple Silicon.

## Phase 1 implementation (unbuilt)

Setting `RenderVolumetricLightingMode` (0 raymarch, 1 exact), UI combo
"Implementation" in `panel_as_volumetric_lighting.xml`. Diagnostic
`RenderVolumetricLightingExactMeshStride` (non-persistent, 1-8, default 1).

Files: `asvolumetriclighting.cpp/.h` (`accumulateExactVolumes()`,
`resolveExact()`, `isExactMode()`), shaders `asVolumetricExactVolumeV.glsl`,
`asVolumetricExactVolumeF.glsl`, `asVolumetricExactResolveF.glsl`.

### Formulation actually used (no lit test at all)

Per cascade k, S_k = closed shadowed volume in shadow texture space
(u, v, depth): top = shadow depth surface triangulated through texel centers,
four hull walls down to depth 1, bottom fan at depth 1. All faces wound
outward; rendered with no depth test, no culling, `GL_DEPTH_CLAMP` on, into a
full-res R32F target with additive blending. Each fragment adds

    sign * (G(clamp(t, a_k, b_k)) - G(a_k)),   G(t) = (1 - exp(-sigma t)) / sigma

sign = -1 entering S, +1 leaving; a_k/b_k = slab near/far distance along the
pixel ray (view z = -clip[k-1] / -clip[k]) clamped to the scene distance.
Telescoping: crossings before a_k give 0, crossings after b_k cancel in
pairs, and subtracting G(a_k) cancels the camera-inside-S term (sum of signs
= s(0)). So the result is the exact shadowed transmittance-weighted length in
the slab, from mesh topology only. This replaced the earlier "depth test +
scene-point lit lookup" idea, which double-counts when the scene point and
the depth-tested face disagree by float error (bright speckle).

Resolve: lit = G(min(scene distance, 128)) - accumulated; brightness formula
and constants identical to `asVolumetricLightF.glsl` (BRIGHTNESS_SCALE 64,
reference 128 m, disc-clamped HG phase, [0,1] clamp). Composite uses the raw
per-pixel path (no gather). Range kept at 128 m for A/B comparison (phase 2).

Orientation: `gl_FrontFacing` = outward normal faces camera (entering) for a
standard projection (det < 0) and orientation-preserving shadow matrix
(det > 0); `as_orientation_flip` = (det(shadow) < 0) != (det(proj) > 0).
If wrong, Exact output is visibly inverted (shadows bright) - flip the sign.

SL sun cascades can be perspective (LiSPSM-like) or ortho; the mesh is
unprojected with `inverse(mSunShadowMatrix[k])`, valid for both. Cleared
cascades (no receivers, stale matrix) are skipped via `mShadowExtents`.

Not changed in phase 1: transparency atlas (still raymarch sampled), local
lights, far-cascade fade (exact mode cuts hard at clip[3]).

Runtime checks for the user:
1. Mode 1 vs mode 0 at Ultra: same brightness/shape, no noise, no patterns,
   no silhouette ghosts.
2. Debug 2 (occlusion) in mode 1: clean hard-edged shafts.
3. Camera inside a shadow (under a tree) and in sun: no global brightness
   jump.
4. GPU cost (stride 1 vs 2/4).
5. macOS run.

## Fixes found while testing phase 1 (2026-10-09, bokt)

- Alpha surfaces (hair, alpha foliage) rendered at their unfogged color in
  both modes: `bindTransparencyAtlas()` cached the atlas enable decision per
  `gFrameCount` from the first call of the frame, including the
  `!gCubeSnapshot && !sRenderingHUDs` terms. A probe/HUD draw deciding the
  cache disabled the atlas for the main view (and the reverse leaked it into
  probes/HUDs). Now those two terms are checked per call. Diagnosed with
  temporary per-program logging (removed): atlas produced/bound correctly.
- Exact mode barely darkened distant objects: Exact uses the composite's
  raw (no-gather) branch, which derived distance via `getPosition()`
  (`inv_proj`), not valid for this plainly bound program. Now uses
  `linearViewDepth()` like the gathered branch.

Phase 1 status: **bokt** (2026-10-09).

## Known issue: dither lattice on alpha surfaces (hair)

Regular dot lattice on alpha-blended surfaces (hair), stronger with high sun
asymmetry ("sky gradient (sun)"), in both modes and every OIT renderer. Cause
(by code reading, not yet confirmed by a test): the transparency atlas
(`asVolumetricAtlasF.glsl`) is still the sampled path - one Bayer-jittered
shadow sample per slice, half resolution, bilinearly read by alpha shaders.
Higher asymmetry raises scatter toward the sun, so the dither amplitude grows.
Fix: exact atlas - accumulate the phase 1 light-volume crossings per slice
(clamp to slice far distance instead of scene depth).

### Exact atlas (bokt 2026-10-10: hair dither lattice gone)

Measured cost (user scene, RTX 3080 Ti, exact opaque + exact atlas): volumetric
off 44 fps (22.7 ms); stride 1: 30 fps (+10.6 ms); stride 2: 40 fps
(+2.3 ms). 4x fewer vertices -> ~4.6x less cost: vertex/primitive bound.

Rejected: coarse atlas mesh (stride x4 + min-depth 4x4 footprint). Measured
26.6 -> 30.4 fps (off 40.8), but min-depth dilation closed foliage gaps and
visibly darkened alpha bushes; any coarse mesh must grow, shrink or
point-sample occluders (darker / brighter / popping rays). Removed. Same for
`RenderVolumetricLightingExactMeshStride` > 1: point-sampled grid slides with
the per-frame cascade refit, rays jump.

### Exact speedups (tried, measured, REVERTED 2026-10-10)

Everything in this section (flat tiles, GPU tile compaction, slab-near
clipping, profiling settings) was reverted to the exact atlas commit; the
measurements below are kept as the record of why. Exact mode stays as the
expensive, artifact-free option; work moves on to phase 2.

Measured split: atlas pass ~4.7 ms with almost no fragments -> vertex/setup
cost ~4.7 ms per pass; opaque pass adds ~3.7 ms fragment work.

1. Flat tiles: `asVolumetricExactTileClassifyF.glsl` marks each 8x8-cell
   tile flat when all 81 vertex depths are within 2 mm (along the light,
   converted per cascade) of the plane through three corners. The vertex
   shader draws non-flat tiles fully (part 6, row strips joined by
   degenerates, even counts keep winding) and flat tiles as one fan from the
   center vertex through every border vertex (part 7, clockwise -> normal
   -depth). Fans share all border vertices, so no T-junctions. Each part
   collapses (zero-area) the tiles it does not own. Flags computed once per
   frame, shared by opaque and atlas passes. Fallback: full row strips.
2. Slab-near clipping: crossings with t < a contribute exactly 0, so
   `gl_ClipDistance[0] = -z - slab_near` clips them (GL_CLIP_DISTANCE0).
   Clip cracks lie on that plane, where the contribution is 0.
Index-buffer vertex reuse still deferred (small-batch reuse on modern GPUs).

Result (built): no gain (exact 27.8 fps, raymarch Ultra 38.9, off 39.4).
Suspected cause: collapsing does not skip vertex shading. Every tile runs
both parts (158 + 34 vertices vs ~128 per tile before), so vertex
invocations rose ~50% and only flat tiles' rasterization was saved. A real
gain needs compaction (draw only the tiles each part owns). To verify first:
`RenderVolumetricLightingExactProfile` logs per-stage GPU time and flat-tile
share per cascade; `RenderVolumetricLightingExactTiles` = false A/Bs the
full mesh.

Profile (RTX 3080 Ti, 4 cascades of 3456x1344, 72576 tiles each): flat
72-93%. Tiles on 18.9 ms (volumes 12.4, atlas volumes 6.2, classify 0.26);
tiles off 17.0 ms (11.5 / 5.4). Confirms the collapse cost.

### GPU tile compaction (implemented, unbuilt)

Per cascade, no compute shaders: classify into level 0 of an RG32UI count
pyramid (full, flat), five 4x4 reductions to a 1x1 top
(`asVolumetricExactTileReduceF.glsl`), then a list pass
(`asVolumetricExactTileListF.glsl`, HistoPyramid traversal) writes entry k =
k-th full tile (.xy) and k-th flat tile (.zw) into an RGBA16UI list. The top
counts go by glReadPixels (RED/GREEN_INTEGER) into a pixel pack buffer that
holds two DrawArraysIndirectCommands per cascade; parts 6/7 draw with
glDrawArraysIndirect and read their tile from the list (no CPU sync, no
collapsed instances). Up to 1024 tiles per axis, else full mesh.
`scripts/as_gl41_tile_compaction_check.c` runs the real reduce/list shaders on
macOS: all pass on Apple M4 (4.1 Metal - 90.5): exact lists, integer
readback into the pack buffer, indirect draw with VS list fetch.

Result (built, correct counts): tiles on 21.9 ms (lists 3.1, volumes 13.8,
atlas volumes 4.9); off 20.1 ms (14.1 / 6.0). ~2.4x fewer vertices barely
moved the mesh passes: they are NOT vertex bound (earlier stride inference
conflated vertex count with triangle size). Tiles now default off. Profile
now also logs fragments (GL_SAMPLES_PASSED) and primitives
(GL_PRIMITIVES_GENERATED) per mesh pass plus target sizes, to measure
overdraw before the next change.

Measured (tiles off, 3440x1328, 4 cascades 3456x1344): volumes 11.9 ms =
229.6 M fragments (50 layers per pixel) + 37.2 M primitives; atlas volumes
5.5 ms = 3.6 M fragments (same 50 layers at 430x166) + the same 37.2 M
primitives. So the atlas pass is triangle-setup bound (~6.8 G prim/s) and
the opaque pass adds ~6.4 ms of fill. Both are hardware throughput limits
of the method at this shadow resolution, not vertex shading. Most layers
are crossings behind the visible scene (no depth test: rays run through the
whole cascade volume) and cancel in pairs. Cutting them exactly needs an
inside-volume test at the clip point, which disagrees with the rasterizer
at ties on caster surfaces (seam sparkles). Note: raymarch fps comparisons
may be CPU-masked (Ultra 38.9 vs off 39.4 fps); its GPU time is unmeasured.

`renderExactAtlas()` replaces the sampled atlas in Exact mode (sampled one
stays as fallback and for Raymarch mode). Pass 1: the same light-volume mesh
(shared `drawExactVolumes()` helper) rasterized at atlas tile resolution
into `sExactAtlasAccum` (4 x RGBA32F, slice k -> attachment k/4, channel
k%4) with `asVolumetricExactAtlasVolumeF.glsl`. Per fragment, with slab
entry/exit A/B and slice end e (radial, ((k+1)/16)^2 * 128 m), G monotonic:

    slice_k = e <= A ? 0 : sign * (clamp(G(t), G(A), min(G(B), G(e))) - G(A))

3 exp() per fragment for all 16 slices; G(e_k) uploaded as `as_slice_g[16]`
and shared with the resolve. Pass 2 `asVolumetricExactAtlasResolveF.glsl`
writes the 4x4 atlas: lit = G(e_k) - shadowed, disc-clamped phase, same
brightness constants; alpha = same transmittance as the sampled atlas.
Consumers unchanged. Cost: one more full light-volume vertex pass (fill is
tiny at tile resolution); replaces the ~0.28 ms sampled atlas.

## Switch

New setting e.g. `RenderVolumetricLightingMode` (0 = current raymarch,
1 = exact). Exact mode replaces only the directional pass; local-light pass
(`asVolumetricLocalLightF.glsl`) shared.

## Plan of phases

1. A only (geometry shafts, single cascade first, then cascades). Validate
   exactness against current Ultra (64 samples) as reference; must work in
   snapshots.
2. Remove 128 m cap in exact mode, phase function change.
3. C (haze modulation).
4. B (cloud shafts).
5. Perf: tile compaction, profiling vs current Normal/High.

## References

- Billeter, Sintorn, Assarsson, "Real-time Volumetric Shadows using
  Polygonal Light Volumes", HPG 2010.
- Engelhardt, Dachsbacher, "Epipolar Sampling for Shadows and Crepuscular
  Rays in Participating Media with Single Scattering", I3D 2010.
- Yusov, "Practical Implementation of Light Scattering Effects Using Epipolar
  Sampling and 1D Min/Max Binary Trees", GDC 2013.
- Peters et al., "Beyond Hard Shadows: Moment Shadow Maps for Single
  Scattering, Soft Shadows and Translucent Occluders", I3D 2016.
- Wronski, "Volumetric Fog", SIGGRAPH 2014; Hillaire, "Physically Based and
  Unified Volumetric Rendering in Frostbite", SIGGRAPH 2015.
