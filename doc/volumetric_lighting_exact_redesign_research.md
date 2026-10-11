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

Keep HG (two-lobe mix tried and rejected, see Phase 2); remove the [0,1]
clamp and the 128 m normalisation (scene is HDR before tonemap).

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

## Phase 2: range, brightness, phase (implemented, unbuilt, 2026-10-10)

Exact mode only; Raymarch unchanged.

- Range R = `mSunClipPlanes[RenderShadowSplits]` (last cascade far plane,
  up to 512 m), used as a radial distance: radial >= view depth, so the
  medium always ends inside shadow coverage. Fallback 128 m.
- Fade: medium density weight w = 1 up to 0.75 R, linear to 0 at R
  (`EXACT_FADE_START_FRACTION`). Effective length
  `D(t) = min(t, F0) + x - x^2 / (2 (R - F0))`, x = clamp(t - F0, 0, R - F0).
  Every exact shader uses G(D(t)) instead of G(t) (`fadedLength()`). With
  density sigma*w, scatter integrand w * exp(-sigma D) = d/dt G(D(t)), so
  this is the exact single-scatter integral of the faded medium, and
  G(D) is non-decreasing, so telescoping and the atlas clamp formula hold.
  Ray end = min(scene distance, R). No hard edge at R.
- Brightness: physical, HDR: scatter = albedo * density * phase * lit, no
  [0,1] clamp, no /128. Old factor was BRIGHTNESS_SCALE / 128 = 0.5, so
  Exact is ~2x brighter per metre than before plus the longer range;
  retune albedo/density at runtime.
- Phase: plain HG kept. A tried `mix(HG(g), 1/(4 pi), w)` slider (Side
  scatter) was removed: a normalised mix only redistributes light. At
  g = 0.4, w = 1 makes 0 deg ~4x dimmer, 90 deg only 1.5x brighter
  (crossover ~70 deg), so toward-sun shafts vanished into a flat veil
  (confirmed at runtime). Side-on shaft visibility must come from contrast
  (phase 3 haze modulation), not from the phase function.
- Atlas: slices span R (`as_atlas_range`, slice_g = G(D(e_k)) on CPU).
  Consumers (`alphaF`, `pbralphaF`, `materialF`, `fullbrightF`, `waterF`)
  read the slice range from `asVolumetricAtlasRange` (`sAtlasRange`: 128
  for the sampled atlas, R for the exact atlas).
- Composite / atlas transmittance: same view-depth metric and optical-depth
  limiter; the sky fade window (`skyFadeStart`, `skyDistance` uniforms) is
  100-128 m in Raymarch, 0.75 R - R in Exact. Distant geometry beyond R
  stays unattenuated like the sky (haze modulation is phase 3).

Runtime checks:
1. Shafts visible beyond 128 m, no edge/seam at R, no hard ring on terrain.
2. No speckle at long range (R32F precision at 512 m).
3. Alpha surfaces (hair, foliage) at 150-500 m match opaque neighbours.
4. Raymarch mode unchanged (atlas range 128, sky fade 100-128).

## Phase 3: haze modulation (built, tested, REVERTED 2026-10-10)

Reverted: the GBA haze channels made the accumulation RGBA32F (4x blend
bandwidth on a fill-bound pass of ~50 layers/pixel, plus 3 exp per
fragment). Measured 30 -> 14 fps (user scene, Exact) for an invisible gain
(see runtime result below). Code is back to phase 2. For phase 4, remove the
shadowed share of SL haze inside the cloud-shadow march instead (km range,
where it is visible), not via extra light-volume channels. The method and
SL haze model below remain valid for that.

Where SL haze comes from: not `softenLightF.glsl` but the separate deferred
haze pass (`LLPipeline::renderDeferredLighting()`, `gHazeProgram`,
`class3/deferred/hazeF.glsl`): `screen = screen * atten.r +
srgb_to_linear(2 * additive) * sky_hdr_scale`, skipped for depth 1 (sky) and
below water when the camera is under water. `additive` from
`calcAtmosphericVars()` (`class1/windlight/atmosphericsFuncs.glsl`) is
`[blue_horizon*bw*(cs + amb) + haze_horizon*hw*(cs*haze_glow + amb)] *
(1 - T_sl)`, `cs` = attenuated sunlight. It is a uniform medium with per-RGB
extinction `sigma = (blue_density + haze_density) * density_multiplier *
distance_multiplier * k`, k = max_y / |eye y| when |eye y| > max_y (SL scales
the whole eye position), so `1 - T_sl = 1 - exp(-sigma * len)`.

Method (exact w.r.t. SL's model):
- `sExactAccum` is RGBA32F. GBA = shadowed transmittance-weighted length in
  SL's medium per channel, same light-volume crossings (telescoping),
  unfaded, clamped to the scene depth only.
- Shadowed share of SL in-scatter: `f = sigma * G_shadowed / (1 - T_sl)`.
- `asVolumetricExactResolveF.glsl` recomputes `additive` and its sun part
  (every term with `cs`) with a local copy of `calcAtmosphericVars()`
  (must stay in sync with upstream), then
  `removed = haze(additive) - haze(additive - f * sun_part)` with `haze()` =
  hazeF.glsl's output incl. its clamp at 10, and writes
  `scatter - removed * T` (T = composite scene transmittance, which
  multiplies the screen haze included).
- Uniforms: our programs are not in `LLViewerShaderMgr::mShaderList`, so
  environment uniforms never arrive by themselves; `mUniformsDirty = true`
  before `bindDeferredShader()` applies them on bind, then the haze pass's
  own overrides are set identically (`sun_up_factor`, `lightnorm` via
  `ASCelestialTwilight`, `waterPlane`; sun/moon dir and color come from
  `bindDeferredShader()`).
- Diagnostic toggle `RenderVolumetricLightingExactHazeModulation`
  (non-persistent, default on). Debug modes never subtract.

Limits (v1):
- Within the shadow range (<= 512 m) SL haze is often a small share of the
  pixel, so the contrast gain depends on the sky's haze/density settings.
  Large gains need phase 4 (km-scale cloud shadows).
- Sky pixels untouched (separate sky shader, very long path).
- Alpha surfaces draw after the haze pass with their own haze; the opaque
  pixel's removal is applied under them too (by opaque depth), so foliage or
  hair in front of distant shadowed terrain may darken slightly. Check at
  runtime.

Runtime result (built, 2026-10-10): plumbing verified with Exact debug
modes 4-6 (4: shadowed fraction f, 5: removed haze x100, 6: R density
multiplier x1e4, G distance multiplier, B shadowed SL length / 100 m).
Debug 4 shows clean exact shafts, debug 6 shows the sky uniforms arriving
and the GBA accumulation, debug 5 is near black: the removal is ~1e-4
linear, below one 8-bit step after tonemap, so toggling the diagnostic gives
pixel-identical screenshots even with haze density raised. Correct, but
invisible at <= 512 m; kept as the base for phase 4 (km-scale cloud shadows,
where SL haze is a large share of the pixel).

Runtime checks:
1. Toggle the diagnostic: shadowed air darker, lit shafts unchanged.
2. Hazy sky (high haze density / density multiplier): strong dark bands.
3. No change on sky, none under water, no NaN/black pixels.
4. Alpha foliage/hair edges against distant shadowed terrain.

## Phase 4 research: cloud shadows (2026-10-10)

### How SL clouds are drawn

- Geometry: `LLVOWLSky::buildStripsBuffer()` unit dome direction (x0, y0,
  z0), Y up, times `getDomeRadius()` (fixed `LLSettingsSky::DOME_RADIUS` =
  15000). `LLDrawPoolWLSky::renderDome()`: translate to camera, rotate 120
  deg about (1,1,1) (dome (x0, y0, z0) -> agent (east, north, up) =
  (z0, x0, y0)), scale 0.333 (~5000 m), translate -camHeightLocal in dome Y.
  So clouds are painted on a camera-centred sphere; opacity is a function of
  view direction only.
- UV: `texcoord0 = ((1 - z0)/2, (1 - x0)/2)` = ((1 - east)/2, (1 - north)/2)
  of the unit direction (planar projection of the hemisphere).
  `cloudsV.glsl`: x negated, then `(uv - 0.5) / cloud_scale + 0.5`;
  uv1 (self-shadow) = uv0 + lightnorm.xz * 0.0125; uv2/uv3 = x16 (detail);
  `altitude_blend_factor = clamp((15000 y0 - camHeight + 50 + 512) / max_y)`
  (fades only within ~4 deg of the horizon).
- Opacity `alpha1` (`cloudsF.glsl`, copied unchanged in AyaneStorm's
  `asHorizonCloudTintF.glsl`): 4 disturbance fetches + 2 density fetches of
  `cloud_noise_texture` / `_next` mixed by `blend_factor`, scroll
  `cloud_pos_density1/2.xy`, density `cloud_pos_density1.z`,
  `cloud_shadow`, `cloud_variance`, two smoothing squarings, times
  `altitude_blend_factor`. Uniforms set in
  `LLDrawPoolWLSky::renderSkyCloudsDeferred()`, scroll in
  `LLSettingsVOSky::applySpecial()` (x flipped).

### Key property: cloud shadow is a 2D map

Model the clouds as that sphere (radius R, ~5000 m). For a point p, the sun
ray p + uL hits the sphere at one point q, and every point on the same sun
ray hits the same q. So cloud transmittance c(p) is constant along L: a 2D
function of p projected on the plane perpendicular to L, exactly a shadow
map. Bake it once per frame into a small 2D texture (each texel: sphere
intersection, direction -> SL uv, `alpha1`; transmittance = 1 - alpha1),
then any 3D lookup is 1 bilinear fetch. Low frequency (km-scale features):
a few hundred texels square suffice; mipmaps give band-limited samples.

### Which medium shows cloud shafts

Our medium (default density 0.012/m, mean free path ~83 m) dies out within
~250 m; cloud shadow barely varies over that range, so km-long shafts can
not appear in it. Cloud shafts live in SL's own haze (sigma ~1e-4/m: 1 - T
~40% at 5 km) and in the sky colour. So phase 4 is the phase 3 method
applied to cloud shadow instead of geometry shadow: remove the
cloud-shadowed share of SL's sun in-scatter, with the shadow integral
computed by a prefiltered march (N fixed steps over the full view ray, each
1 fetch of the baked map at a mip matched to the step footprint: band
limited, no jitter, no history), for terrain pixels (hazeF.glsl model, up to
scene depth) and sky pixels (sky shader model, its own path length).
Computed in a cheap pass, not in the light-volume mesh (phase 3 lesson).

Decisions (user, 2026-10-10):
- Light: the active directional light (sun or moon), same as the shafts.
- Sphere, not plane: SL paints clouds by view direction (dome moved to the
  camera every frame), so only a sphere matches the drawn clouds (rays from
  the visible gaps). A plane with world uv is stable but stretches vs the
  drawn clouds away from zenith (~40% at 45 deg elevation).
- Lazy world anchor: sphere centre fixed in world (stable shadows while
  walking, orbiting, zooming); re-anchored only when the camera is > ~300 m
  away, sliding smoothly to the camera over a few seconds. Mismatch with the
  drawn clouds ~ offset / R (300 m / 5 km = 6%). Independent of sims.
- Our medium under clouds: dim the near-field shafts too, by multiplying
  each pixel's scatter by the e^(-sigma t)-weighted mean cloud transmittance
  along its view ray (same march). Not exact (exact needs per-segment
  products, i.e. extra light-volume channels, phase 3 cost); error only
  where a cloud-shadow edge lies within our medium's ~250 m.
### Sky colour model (`class1/deferred/skyV.glsl`, `skyF.glsl`)

Per dome vertex: `rel_pos = dir * 15000 - (0, camHeight, 0) + (0, 50, 0)`,
scaled to y = max_y above the horizon (32000 below); path length L =
|rel_pos|; `T = exp(-(blue_density + haze_density) * density_multiplier *
L)` (no distance_multiplier, unlike terrain haze). Colour = above-cloud
haze `[blue_horizon*bw*(sun + amb) + haze_horizon*hw*(sun*glow + amb)] *
(1 - T)`, blended toward the below-cloud colour (sun dimmed by
cloud_shadow, amb raised) by `1 - T^(1/4)`. Linear in (sunlight, ambient),
so the sun part = same formula with ambient terms zeroed. `skyF.glsl`:
+ rainbow + halo, x2, clamp [0,5] into the G-buffer (sRGB), clouds then
alpha-blended over it (`alpha1`), `softenLightF.glsl` (SKIP_ATMOS):
`srgb_to_linear(gbuffer) * sky_hdr_scale`.

### Implementation plan (Exact mode, not started)

1. Lazy anchor (CPU): world point; when the camera is > 300 m away, slide
   it to the camera over a few seconds.
2. Cloud shadow bake: one fullscreen pass into a R16F texture (~1024^2,
   mipmapped), texel = point on the plane through the anchor perpendicular
   to the active light L, extent 2R x 2R. Per texel: intersect the sun ray
   with the sphere (radius R ~5000 m, centre anchor), direction -> SL uv
   exactly as `cloudsV.glsl`, `alpha1` exactly as `cloudsF.glsl`, store
   1 - alpha1. Program in SG_SKY (gets cloud_pos_density1, lightnorm),
   cloud noise textures and blend_factor as `renderSkyCloudsDeferred()`.
   Skipped when cloud_scale < 0.001 or clouds disabled. Points outside the
   sphere: no cloud data, lit.
3. Prefiltered march, importance-spaced: with u = 1 - e^(-sigma t), the
   e^(-sigma t)-weighted mean of c along [0, t_end] is the plain mean of c
   over u uniform in [0, 1 - e^(-sigma t_end)]; N fixed midpoints
   t_i = -ln(1 - u_i) / sigma, each 1 fetch at a mip matched to the
   spacing. No jitter, no history, band limited.
4. In `asVolumetricExactResolveF.glsl`:
   - our scatter *= mean cloud transmittance (our sigma, to the ray end);
   - terrain pixels: f = shadowed share of SL haze (terrain sigma incl.
     distance_multiplier and max_y k, to scene depth), remove
     `haze(A) - haze(A - f * sun_part)` (phase 3 code, kept in the doc);
   - sky pixels: f with the sky path (sky sigma, length L), remove the
     sun part of the sky colour through skyF/softenLight's nonlinearity,
     weighted by (1 - alpha1) of the pixel's own cloud.
5. Cost estimate: bake ~1024^2 x 6 fetches; per pixel ~16 + 8 fetches,
   plus 6 on sky pixels: ~1 ms at 3440x1328 (to measure).

### Correction: the dome is not centred on the camera

`LLEnvironment::getCamHeight()` = `getDomeOffset() * getDomeRadius()` =
0.96 * 15000 = 14400, and `renderDome()` translates the dome by
-camHeightLocal: the camera sits 0.96 R above the dome centre, so the drawn
clouds lie on a shallow cap above the viewer (direction from the camera ->
hit point on that sphere -> unit vector from the centre -> UV and
altitude_blend). The first build A used a sphere centred on the anchor:
directions below ~74 deg elevation gave y0 < 0.96, a negative altitude in
altitude_blend_factor and no cloud (bokt symptom: cloud shadows only with
the sun at 74 deg). Fixed: physical sphere radius R = CLOUD_CEILING_HEIGHT
(2000 m) / (1 - 0.96) = 50 km, centre 0.96 R below the anchor; map 2048^2
over +-25 km (low suns hit the ceiling ~H / tan(elevation) away); anchor
re-centres at 100 m (mismatch ~ offset / 2000 m). Debug 4 on sky pixels
shows the baked map seen from the anchor (must reproduce the drawn clouds
toward the light, grey elsewhere).

### Build A (implemented, unbuilt)

Steps 1, 2 and the near-shaft dimming of step 4.
- `asVolumetricCloudShadowF.glsl`: bake into `gCloudShadowMap` (R16F
  1024^2, `TMG_AUTO` mips on flush), program in SG_SKY with
  `mUniformsDirty` set before bind; noise textures / blend_factor /
  cloud_variance as `renderSkyCloudsDeferred()`. Skipped (no clouds) with an
  HDRI sky, clouds render type off, no noise texture, cloud_scale < 0.001.
- `ASVolumetricLighting::renderCloudShadow()`: lazy anchor in global
  coordinates (snap > 5 km, slide when > 300 m, time constant 1.5 s, stop
  within 5 m), dome-space light (`toLightNorm` permutation of the sun/moon
  direction), basis e1/e2, and `as_cloud_from_view` (view -> map coords and
  along-light distance / R) from the camera axes and the anchor offset.
- Resolve: scatter *= importance-spaced mean cloud transmittance (8 steps,
  our sigma, to the ray end). Debug (Exact): 4 cloud shadow on the scene,
  5 dimming factor.
- Not yet: the transparency atlas (alpha surfaces keep undimmed shafts
  under clouds), haze/sky removal (build B).

Result (bokt after the dome correction): debug 4 sky reproduces the drawn
clouds, shafts dim under clouds. Alpha foliage stayed bright (atlas not
dimmed) -> fixed in build B.

### Build B (implemented, unbuilt)

- `asVolumetricCloudUtilF.glsl`: shared cloud lookups/marches, linked into
  both exact resolves (`applyCloudUniforms()` binds the map on an appended
  unit for either program).
- Atlas resolve: shafts * mean cloud transmittance up to each slice end
  (alpha surfaces now dim like opaque ones).
- Opaque resolve, cloud rays (`RenderVolumetricLightingExactHazeModulation`,
  non-persistent, default on): per-channel cloud-shadowed share f of SL's
  in-scatter (16-step importance march for the mean RGB sigma, each
  channel reweighted by its exact density ratio), then
  - terrain: hazeF.glsl model (phase 3 `terrainHaze()`), removed x
    composite T;
  - sky: skyV.glsl model replicated for the view direction (dome vertex hit
    from camPosLocal = (0, getCamHeight(), 0), same max_y / -32000 clamp,
    sky sigma without distance_multiplier, horizon blend toward the
    below-cloud colour), SG_SKY light colours (`getSunlightColor() *
    twilight influence`), output through skyF/softenLight
    (`srgb_to_linear(clamp(2c, 0, 5)) * sky_hdr_scale`). Rainbow, halo and
    the clouds drawn over the sky are not modelled (only affect the
    nonlinearity).
  - environment uniforms via `mUniformsDirty` + haze pass overrides, as in
    phase 3.
- Debug (Exact): 6 shadowed share f, 7 removed x4.

Built result: with a cloud over the sun the whole sky went black (debug 6
all white) and clouds turned red. Causes: (1) SL's sky path ends at max_y
(1.6 km), entirely below the 2 km ceiling, so a covered sun shadowed all of
it; (2) cloud pixels over the sky lost sky light; (3) removal from the
model, not the pixel. Fixes (unbuilt):
1. Sky shadowed share over a physical atmosphere: weight e^(-altitude /
   8 km) along the view ray (= meanCloudTransmittance with sigma = sin(elev)
   / 8000, path capped at 100 km): only the air below the ceiling (~22%)
   can be shadowed.
2. Scaled by the drawn cloud's transmittance at that pixel
   (`skyCloudTransmittance()`, from the baked map toward the light).
3. Removed in G-buffer space from the actual pixel (screen bound as
   `as_scene_color`): delta = f * min(2 * modelled sun part, pixel *
   modelled sun share), so the sun disc and model mismatch cannot overshoot.

Built result (fixes 1-3): sky correct, but no cloud rays at all. Debug 6
facing a cloud-covered sun: black around the sun (all drawn cloud, so fix 2
zeroed it), grey only in sky gaps, no streaks. Fixes (unbuilt):
4. Cloud pixels: cloudsV.glsl adds its own below-cloud haze in front of each
   cloud (`oHazeColorBelowCloud = additiveColorBelowCloud * (1 -
   sqrt(T_sky))`, same dome vertex, half the optical depth). That air is all
   below the ceiling, which is where crepuscular rays are: its sunlit part
   is removed with the shadowed share marched from the camera to the cloud
   layer, weighted by the cloud alpha; the sky part keeps the clear-sky
   weight. delta = min(2 (f_sky (1 - a) sun_sky + f_below a sun_cloud_haze),
   pixel * sun share). Debug 6 shows f_sky (1 - a) + f_below a.
5. Mip selection: the map is constant along the light, so a march step moves
   on it by step * sin(angle to light), not by step. The old LOD over-blurred
   by 1 / sin, i.e. most toward the light where the rays are (~6x at 10 deg).
   `cloudTexelsPerMetre(ray_dir)` now scales by that sine.

Built result (fixes 4-5): ground effect good, but clouds near the sun went
dark with teal/blue fringes (debug 6 white there: the air in front of them
fully shadowed). Cause: the per-channel front-haze model ignored the
cloud's per-channel clamp (cloudsF.glsl clamps colour to 1, the sun glow
saturates it) and AyaneStorm's horizon cloud tint shader
(asHorizonCloudTintF.glsl), which redraws the clouds with its own colour;
`cloud_color` is also SG_SKY only. A red channel modelled far above the
pixel was removed entirely. Fix (unbuilt): the cloud part is one scale for
all channels, kept *= 1 - f_below * a * s, with s = luminance of the sunlit
front-haze share (1 - sqrt(T)) * sun_below_cloud / add_below_cloud: bounded,
hue-preserving, independent of the cloud shader. Saturation by the glow is
not modelled (slight over-darkening right next to the sun).
Also seen (same build, higher sun): the sun disc itself turned grey (the
glow-sized cap let the removal take most of the disc pixel). Fix (unbuilt):
no sky/cloud removal inside the disc (`as_disc_sin_cos`); the disc is
direct light, and a cloud over it is already drawn by SL.

Built result (disc fix): bright hard disc in a muddy brown sky, green ring,
dark flat horizon; with the setting off SL's soft glow. Causes: (1) under
cloud, the "disc" is the cloud's saturated glow, so excluding it left a
hard bright disc; (2) double counting: SL already dims the sunlight below
the clouds by the coverage (`sunlight * (1 - cloud_shadow)` in cloudsV.glsl
and the terrain haze), and the removal came on top, strongest at the
horizon (longest air path below the clouds); (3) the sky part was per
channel: removing the red/orange glow left green. Fix (unbuilt):
redistribute instead of remove.
- Map is RG16F: g = 1 where the plane point is under the cloud layer; the
  1x1 mip gives the mean transmittance there,
  mean_t = (r - (1 - g)) / g (`cloudMeanTransmittance()`).
- Terrain and cloud front haze (SL dims by coverage): sunlit part *
  litRatio = lit / mean_t, capped at the unshadowed sun 1 / (1 - cloud_shadow).
- Sky (SL does not dim it): change = shadowed - below_share * (1 - mean_t),
  below_share = the e^(-kt) weight below the cloud layer (hit distance).
- Sky and cloud scales are luminance scalars on the pixel (hue kept); the
  disc only skips the sky part, the cloud part applies over it.
- Average equals SL; shadowed air darker, gaps brighter (removed < 0).
- Debug 6: brightness relative to SL (0.5 = unchanged); 7: |change| x4.

Built result (redistribution): sky still darker than SL toward the sun,
green blob, ground brighter. ABANDONED: SL's sky, clouds and haze are
painted terms (glow, per-channel clamps, coverage dimming, AyaneStorm's
horizon cloud tint); every way of shadowing them (remove, cap,
redistribute) hit another one. Modulating SL's haze/sky/clouds is removed
entirely (setting `RenderVolumetricLightingExactHazeModulation`, scene
colour binding, haze/sky replicas, RG map).

### Build C: cloud rays and cloud shadows, our own medium only (unbuilt)

The request is rays and shadows from clouds, not a change of SL's sky.
1. Cloud rays: our medium continues beyond the exact range, lit only by the
   cloud shadow map. Its density ramps in over the exact fade
   [0.75R, R] (complement of the exact medium's fade), extinction e^-sigma t,
   up to the scene or the cloud layer (max 20 km): `cloudRayLength()`, 16
   importance-spaced prefiltered samples. scatter = albedo density phase
   (lit_exact * cloud + far_lit). Added light only. Debug 6: lit share of
   the far medium.
2. Cloud shadows on the ground (next): cloud transmittance on SL's sun
   shadow term, needs the bake before the deferred sun lighting.
3. Done: all SL haze/sky/cloud modulation removed.
Known: alpha surfaces only get the near shafts (atlas range).

Built result (build C): sky colours correct again, fps unchanged (~30).
Debug 6 (frozen clouds) shows cloud-shaped lit/shadowed air, but debug 1
shows no far contribution: the far medium used the near density (0.012/m,
fog-like), so e^(-sigma t) absorbed everything past ~80 m beyond the
shaft range (far share ~3% of the glow), and debug 6 only measured that
near air. e^(-sigma t) from the camera could also underflow (sigma t0 >
~87) into NaN (black). Fix (unbuilt):
- far medium has its own density `RenderVolumetricLightingCloudRayDensity`
  (default 0.0002/m, clear air), scatter = albedo phase (density lit cloud
  + cloud_ray_density far_lit);
- not dimmed by the near medium; extinction e^(-sigma (t - t0)) relative
  to its start (no underflow).
Expected brightness: optical depth 0.0002 x 14 km (horizon) ~ 2.8, ~0.4
overhead (2 km): comparable to the near shafts.

Built result: debug 6 and debug 1 show real crepuscular rays fanning from
the sun (frozen clouds). Debug 0 barely changes, even with a denser far
medium: single scattering saturates at albedo * phase * sunlight (1 -
e^-tau is already ~0.9 toward the horizon), a few percent of SL's bright
sunset sky. The Debug Settings field also rounds to 3 decimals (0.0005 ->
0.001). Fix (unbuilt): density in per-km units
(`ASRenderVolumetricLightingCloudRayDensityKm`, default 0.2) and an artistic
multiplier `ASRenderVolumetricLightingCloudRayStrength` (default 1 =
physical) on the far scatter.

Built result: user does not want SL's sky touched again (additive only).
With Strength raised, debug 0 shows no beams, only the clouds around the
sun getting brighter: the far medium used the near shafts' strongly
forward phase (user asymmetry), so its light piled up around the sun,
where the clouds are, and the streaks away from it stayed dim (debug 1:
brightest top centre). Fix (unbuilt): far medium uses its own broad phase
(HG g = 0.2, `CLOUD_RAY_ASYMMETRY`), near shafts unchanged.

Built result: no visible cloud beams in debug 0 (only foliage/avatar
shafts). Cloud rays left as they are; moving on to cloud shadows.

### Cloud shadows on the scene (unbuilt)

Requirement: a cloud shadow must look exactly like a sun shadow, so the
normal shadows disappear inside it.
- `ASVolumetricLighting::renderCloudShadows()`, called from
  `LLPipeline::renderDeferredLighting()` right after the sun shadow pass
  (sunLightF / sunLightSSAOF into `mRT->deferredLight`): bakes the cloud
  map (moved here from renderPass, so it exists before the lighting), then
  `asVolumetricCloudApplyF.glsl` multiplies `deferredLight.r` (the sun
  shadow softenLight reads, 1 = lit) by the cloud transmittance (blend
  ZERO, SRC_COLOR; output (T, 1, 1, 1)). Same darkening as a sun shadow by
  construction; also beyond the cascades (r = 1 there). Map mip 1 (~50 m
  soft edge). No LL shader edits.
- Exact mode only (the bake lives there).
Built result: works on the opaque scene; alpha-blended surfaces (dress
lace, hair) stay lit. Fix (unbuilt): the surfaces that sample the sun
shadow themselves (alphaF, pbralphaF, materialF blend, class3 waterF)
multiply sampleDirectionalShadow() by an inline `asCloudShadow()` (same
lookup as cloudTransmittance(), mip 1). bindTransparencyAtlas() binds the
map on the unit after the atlas (applyCloudUniforms(); otherwise the
sampler points at the atlas unit, Apple shared-unit rule; never in cube
snapshots/HUDs); asmacoit.cpp keeps channels + 2 below the unit limit for
its shared state unit. Not covered: GLTF alpha (pbrmetallicroughnessF).

Realism (user): clouds never block the sun completely; the light they
diffuse comes from the whole cloud and casts no shadows. Sun shadow under
clouds = shadow * T + d * (1 - T), d = `ASRenderVolumetricLightingCloudShadowLight`
(default 0.1, tuned by the user in an overcast view; 0.3 was too bright):
under a full cloud, geometry shadows vanish and the ground keeps 10% of
the sun. The setting is the share through a full overcast; the deck
thickens with coverage (Beer-Lambert), d = setting ^ cloud_shadow
(coverage 0.5 -> ~0.32, 0.3 -> ~0.5), computed in applyCloudUniforms().

Refactor (user): no duplicated AS code in SL shaders. The lookup lives in
one AS file, `class1/deferred/asCloudShadowUtilF.glsl` (uniforms,
asCloudTransmittance(), asCloudSunShadow()), registered with the basic
shaders (llviewershadermgr.cpp) and attached next to shadowUtil.glsl for
every hasShadows program (LLShaderMgr::attachShaderFeatures(); unused code
and uniforms are dropped at link). alphaF / pbralphaF / materialF / waterF
keep only a prototype and the tagged one-line call.

Built result: trees (self-shadowed canopies) got lighter under a cloud and
darker when it left. Cause: shadow * T + d * (1 - T) relit what the sun
shadow had darkened (directional, shadow-free light). Fix (unbuilt): a
cloud only dims the sun, sun term = shadow * mix(d, 1, T): shaded parts
never brighten; under a thick cloud the sun is down to d (0.1), so
geometry shadows almost vanish. Opaque apply pass back to a multiply
(ZERO, SRC_COLOR, r only).

User: shadows on the ground no longer disappear (10% of SL's bright sun
still draws them). Physical model instead: the light through a cloud comes
from the whole cloud, like sky light: it ignores the sun shadow but is
occluded like ambient light. Opaque: r = shadow * T + d * (1 - T) * ao
(blend ONE, SRC_ALPHA, r only), ao = GTAO visibility when effective
(ASAmbientOcclusion::bindResult(), .a with bent normals, as softenLight),
else legacy SSAO recomputed in the pass (hasAmbientOcclusion / aoUtil; the
sun pass's own copy is in the target's g), else 1. Shadows vanish on open
ground; canopy interiors, eaves, corners stay dark. Forward surfaces
(alpha, water) have no AO: mix(d, shadow, T).
Tuned by the user with this model: setting 0.001 (d = 0.001 ^ coverage:
~0.03 at coverage 0.5, ~0.13 at 0.3); default changed to 0.001.
That is the input's 3-decimal limit, so the setting became an optical
thickness: `ASRenderVolumetricLightingCloudThickness` (default 6.9 =
-ln 0.001), d = e^(-thickness * coverage); replaces
ASRenderVolumetricLightingCloudShadowLight. Opaque: apply pass blends ONE, SRC_ALPHA into r only
(output (d (1 - T), 0, 0, T)); alpha/water shaders `asCloudSunShadow()`.

Runtime checks: debug 4 on open ground shows cloud-shadow patches that
drift with the cloud scroll and line up with the gaps seen toward the
light; shadows stay still while walking/orbiting; no clouds -> all white;
fps cost vs build without clouds.

## Switch

New setting e.g. `RenderVolumetricLightingMode` (0 = current raymarch,
1 = exact). Exact mode replaces only the directional pass; local-light pass
(`asVolumetricLocalLightF.glsl`) shared.

## Plan of phases

1. A only (geometry shafts, single cascade first, then cascades). Validate
   exactness against current Ultra (64 samples) as reference; must work in
   snapshots.
2. Remove 128 m cap in exact mode, phase function change (implemented,
   see "Phase 2").
3. C (haze modulation): tried and reverted (see "Phase 3"); folded into 4.
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
