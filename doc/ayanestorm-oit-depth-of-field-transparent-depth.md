# OIT and depth of field: transparent avatar depth

Author: chanayane@firestorm. Updated: 2026-09-06.
Status: experimental implementation rejected by runtime testing; code rollback
approved. Keep this document as research for a future redesign.

Searchable terms: DoF hair blur, rigged alpha depth, Exact OIT, AVBOIT,
renderAlpha depth_only, minimum_alpha, finishDirectColorRaster, double flush.

## Rollback decision (2026-09-06)

Revert all code from the OIT-aware DoF experiment. Keep this document. After
the rollback, Exact OIT and AVBOIT must use the pre-experiment vanilla DoF path.
The original transparent-hair limitation will return, but the renderer will no
longer contain the more severe regressions introduced by this experiment.

Runtime testing rejected both attempted representations:

- A single transparent residual plus transmittance and nearest depth improved
  some strands but left sharp window structure inside hair, changed jewelry
  brightness, produced poor/unstable background blur, and could not represent
  skin, makeup, hair, glass and glow at independent depths.
- The first linked-list Exact DoF gather compiled only after forcing GLSL 4.30,
  but its focused/defocused selection did not preserve arbitrary interleaved
  layers and produced severely corrupted Exact OIT output. Direct access to
  exact nodes alone does not make the screen-space blur/composite operation
  exact.
- Moving the opaque-depth snapshot between alpha/forward stages first produced
  alternating complete/incomplete Exact frames at short draw distance, then a
  stable but wrong avatar when moved after forward rendering. These timing
  patches are not a valid foundation for the redesign.
- Sparse sunflower sampling caused radial streaks. Vanilla-density concentric
  sampling reduced that pattern but did not solve the missing layer data or
  temporal changes caused by object visibility.

The rollback includes `FSOITDoF`, `oitDoFF.glsl`, CMake/shader-manager
registration, pipeline/dispatcher hooks, Exact metadata textures and node-list
DoF exposure, Exact DoF sorting/fallback changes, AVBOIT DoF snapshot/accessors,
and the `lldrawpoolalpha.cpp` condition that skips the established alpha DoF
depth pass. Do not retain isolated pieces of that integration.

### Non-DoF fix to reapply after rollback

Reapply only the removal of the late `gAVBOITOpaqueTarget.flush()` from
`FSAVBOIT::finishDirectFrame()`.

`finishDirectColorRaster()` already flushes `gAVBOITOpaqueTarget` and restores
the caller's screen target after AVBOIT color capture. Calling
`gAVBOITOpaqueTarget.flush()` again after compute resolve pops the render-target
stack a second time. This can leave the wrong target active for the following
isolate-background depth pass. The correct code at that location performs no
flush; the existing `finishDirectColorRaster()` call remains the single restore.
This fix is independent of DoF and should be preserved.

### Requirements before another Exact DoF implementation

Do not restart from the aggregate or selected-node gather approaches described
below. A new design needs a mathematically defined aperture integration that
preserves depth ordering when focused and defocused transparent layers
interleave, handles destination-dependent blend modes explicitly, treats glow
as energy rather than opacity, matches vanilla color/bokeh controls, and has a
bounded performance strategy. Validate the algorithm on captured synthetic
layer stacks before integrating it into the viewer.

## Symptom

With the camera focused on a face, hair over the face is sharp but the
same strand over distant scenery inherits the background blur in both OIT modes.

## Verified cause

DoF samples deferredScreen depth, shared with screen. OIT capture correctly
suppresses vanilla's rigged depth writes. The existing final DoF alpha pass
calls renderAlpha(mask, true), whose default rigged=false excludes avatar
attachments. Restoring that pass in performance audit E12 therefore did not
restore rigged hair depth.

The earlier proposal in this document to export depth from OIT resolves was
unnecessary for this issue. AVBOIT already restores screen in
finishDirectColorRaster(), before the existing DoF pass. finishDirectFrame()
incorrectly flushed that private target again after resolve, despite it no
longer being bound.

## Superseded initial implementation

- lldrawpoolalpha.cpp: after successful OIT capture, draw rigged alpha into
  the existing DoF depth pass with depth test/write enabled and GL_LEQUAL.
- Apply the existing 0.33 texture-alpha cutoff to the actual bound shader
  for OIT depth-only draws, including rigged and PBR variants, after material
  setup. Reset the uniform after each draw. Legacy BLEND materials bypass
  minimum_alpha, so use the existing diffuse alpha-mask shader for their
  depth-only draws (including its rigged variant).
- fsavboit.cpp: remove the redundant private-target flush at resolve time.
  finishDirectColorRaster() remains the single restore after color capture.

Capture, sorting, four AVBOIT front keys, relative volume weights, and shader
versions are unchanged. No depth writes occur during OIT capture. The extra
rigged traversal runs only with DoF enabled and completed OIT capture.
Standard mode retains its existing path. Depth-only draws omit glow redraws.
Texels below 0.33 do not become solid DoF surfaces; this retains the existing
threshold policy rather than assigning transparent-pane depth unconditionally.

## Validation and limitations

Static review: capture scopes end before the DoF draw, screen shares depth
with deferredScreen, rigged shader/palette selection is reused, material
cutoffs are applied after binding, and AVBOIT restores its target once.
No build or runtime test performed.

User runtime checks, in Exact OIT and AVBOIT:

1. Focus on face: one hair strand stays consistently focused over face and
   distant background. Focus on background: foreground hair blurs.
2. Check legacy/fullbright/PBR hair, sheer edges, and hair behind glass.
3. Toggle DoF and OIT modes; check glow, water and isolate-background mode.

This uses the viewer's existing single-depth DoF approximation. Very sheer
edges still use background depth, and multiple translucent focal planes
cannot be represented independently. Isolate mode retains its pre-existing
near-plane depth override; combining isolate and DoF remains subject to that
existing limitation.


## Runtime follow-up: sharp background through hair (2026-09-05)

Reference: AyaneStormOS-Normal_2BBEqYcyOe.png, green circles around the
strands beside both cheeks and the lower right strand. The screenshot was
successfully read through the WSL /mnt/c path. The user confirmed the screenshot used Exact OIT and AVBOIT looks
exactly the same. Both modes reproduce the remaining artifact.

The circled regions visibly contain sharper background detail through hair
than in surrounding uncovered background. Some fine strands also remain
blurred. This is consistent with the depth threshold splitting translucent
pixels between two incompatible single-depth approximations. A screenshot
alone does not establish the alpha/depth values of individual pixels.

### Verified pipeline limitation

- OIT resolves transparent and opaque colors before pipeline.renderDoF().
- cofF.glsl samples one depth and copies the already-composited RGB into
  its output, storing one circle of confusion in alpha.
- postDeferredF.glsl blurs that mixed RGB using the circle of confusion.
- dofCombineF.glsl retains the original mixed RGB for focused pixels.

Consequently, assigning foreground hair depth also keeps the transmitted
background contribution sharp. Leaving background depth blurs the hair
contribution instead. The first fix restores missing rigged depth but cannot
solve this compositing-order limitation. The previous statement that resolve
changes were unnecessary applies only to restoring missing rigged depth,
not to correct DoF through partially transparent surfaces.

### Do not attempt these as a complete fix

- Lowering 0.33 to MINIMUM_ALPHA: extends sharp transmitted-background
  patches to fainter texels and transparent panes.
- Raising the cutoff: returns more visible hair to background blur.
- Exporting nearest OIT depth or blending foreground/background depths:
  still applies one blur to a mixture of two focal planes.
- Dilating depth across hair silhouettes: can make the sharp-background
  patches larger.

### Implemented follow-up: separate color contributions before DoF

This is implemented as a shared `FSOITDoF` module and small tagged pipeline
hooks. It replaces the threshold/depth-write approximation for OIT frames.

Implementation details:

- The OIT alpha depth-only pass is skipped when layered DoF is ready. A private
  depth snapshot preserves opaque depth before isolate and late depth writes.
- Exact OIT stores full-precision transmittance and nearest transparent depth
  in a full-resolution RG32F texture during the actual blend traversal,
  respecting shallow-list opaque cutoffs without another linked-list scan.
- AVBOIT reuses accumulated extinction and `frontKey0`. Its opaque RGBA16F
  copy is allocated only when DoF is enabled and copied immediately before
  resolve; the existing R8 private depth target remains unchanged.
- `oitDoFF.glsl` reconstructs premultiplied transparent color as
  `resolved - opaque * transmittance`, blurs opaque color and transparent
  premultiplied color/coverage independently, and composites them.
- The shared pass runs in linear HDR before tone mapping, CAS, bloom and the
  remaining post effects. Reflection history and exposure retain their original
  unblurred input. Its RGBA16F output and R8/depth snapshot are allocated only
  while OIT DoF is needed and released with the pipeline screen buffers.
- Standard transparency, cube snapshots, disabled DoF, both OIT diagnostics and
  Exact overflow fallback retain the ordinary DoF route. Darwin has inert
  renderer accessors and never selects the unsupported OIT route.

The representation deliberately has one transparent focal layer. Exact OIT
standard source-over content decomposes exactly. Custom blend modes use the
nearest retained transparent depth but cannot in general be factored into a
single transmittance. AVBOIT retains its existing approximate aggregate
opacity/color. Several transparent surfaces at different focal distances
therefore remain an approximation.

Incremental memory while active is approximately 21 bytes/pixel for either
mode: 8 for Exact metadata or the AVBOIT opaque copy, 8 for shared HDR output,
and about 5 for the R8/24-bit depth target (driver storage may differ).
This is about 44 MB at 1080p and 174 MB at 4K. Exact metadata is currently
allocated with its capture images even with DoF off. The full-resolution
shader uses 6-24 disk positions per blurred layer, up to 48 layer reads;
performance relative to the old reduced-resolution DoF is unmeasured.

### Review corrections (2026-09-05)

The initial follow-up implementation had these defects, corrected in code:

- Empty taps were omitted from transparent coverage normalization, keeping
  opaque strands opaque when blurred. They now count as spatially empty taps.
- Both layers used the larger CoC radius, dropping samples from the layer
  with the smaller blur. Each now samples its own disk and transitions smoothly
  from sharp to blurred.
- Exact window depth was quantized to 16 bits. RG32F preserves captured depth
  precision; metadata follows the nodes actually blended.
- Filtered color was combined with point-sampled metadata. All reconstruction
  inputs now use the same clamped pixel, including at screen boundaries.
- Clamping the transparent residual broke custom darkening blends. Signed
  residuals are retained until final recomposition. Glow is blurred with color,
  independently of coverage.
- A failed early DoF attempt could retry on tone-mapped color and skip ordinary
  DoF. The layered hook now accepts only the original linear screen source;
  the later call executes ordinary DoF when the early output is unavailable.
- AVBOIT diagnostics and stale captures during volumetric diagnostics could
  enter layered DoF. Both are excluded, as are non-main render targets.
- AVBOIT's background snapshot preceded intervening draws. It now matches the
  exact background read by resolve. The separate depth snapshot prevents
  isolate mode's near-plane write from being mistaken for opaque scene depth.
- Screen-buffer release now releases the shared DoF targets; allocation failure
  cleans partial targets. Darwin skips loading the unsupported shader.

The blur is still a bounded, destination-gather approximation: it does not
fully scatter defocused foreground coverage into neighboring focused pixels.
Inspect silhouette edges during background focus, especially thin strands.
Multiple transparent focal planes, custom blends, and effects drawn after OIT
resolve (including weather/isolate color replacement) remain approximations.
These corrections do not establish runtime quality or performance.

### Runtime follow-up: radial background streaks (2026-09-05)

The vanilla reference `AyaneStormOS-Normal_0IDV2Eumkk.png` has smooth,
uniform background blur. The Exact/AVBOIT reference
`AyaneStormOS-Normal_Y4Hl59IeNg.png` improves focused hair but shows radial
streaks and blocky bright background patches. Both OIT modes share
`oitDoFF.glsl`, and its bounded 6-24-tap sunflower disk was too sparse for a
large CoC. The visible rays follow that sampling pattern.

The shared shader now uses the same concentric ring density and
highlight-weighted samples as `postDeferredF.glsl`, independently for opaque
and transparent layers. This restores vanilla-quality background sampling
while retaining the layer separation needed for focused hair. It increases
the worst-case sample count to roughly the vanilla pass per blurred layer;
when both layers are blurred the shared pass can cost more than vanilla.

The reported startup-only visual jump could not be isolated from screenshots.
The earlier reviewed code incorrectly used the blurred result for luminance
and exposure; that path is already corrected to use the original linear
screen. Recheck after this shader change and capture a short video or exact
timing if the jump remains.

### Runtime follow-up: alternating complete/incomplete avatar depth (2026-09-05)

The paired Exact OIT frames `AyaneStormOS-Normal_ABIObLh66i.png` and
`AyaneStormOS-Normal_kcnrwKZ15t.png` establish that this is a depth snapshot
race, not exposure adaptation. AVBOIT remained stable and did not reproduce
the jump. In the bad Exact frame, the opaque torso and arms receive background
blur while the head/hair remain focused. The snapshot was taken from inside
the alpha pool, before every forward opaque/avatar pool had necessarily
completed. Changes in pool membership when objects crossed the 32 m
draw-distance boundary exposed the unstable timing.

The first attempted correction captured depth after `renderGeomPostDeferred()`.
Runtime rejected it: Exact stopped flickering but consistently blurred major
opaque facial/body contributions. Forward pools do not preserve the deferred
opaque depth needed by DoF. Depth capture therefore occurs once in
`FSOITDispatcher::beginFrame()`, after deferred rendering and immediately
before forward transparency. This point is independent of draw-pool membership
and precedes all forward depth replacement. The alpha depth-only pass remains
skipped based on completed OIT capture.

### Runtime follow-up: sharp window detail inside thin hair (2026-09-05)

Comparison of `AyaneStormOS-Normal_H7MQ9oyXUj.png` (DoF off) and
`AyaneStormOS-Normal_x8zYDYHs6d.png` (DoF on) shows narrow blue/window-frame
detail surviving inside hair at the small windows. Focused hair should remain
sharp, but its transmitted background should use the blurred opaque layer.

An attempted correction changed color/depth reconstruction to point sampling
to align it with discrete metadata. Runtime results
`AyaneStormOS-Normal_2wvAkVzcn6.png` (Exact) and
`AyaneStormOS-Normal_x231W5dKuY.png` (AVBOIT) rejected that change: Exact
blurred and removed major facial contributions, while AVBOIT still retained
window detail inside hair. The point-sampling change was reverted.

This establishes that filtering mismatch is not the root cause. One aggregate
transparent residual plus one depth cannot describe subpixel coverage and
several avatar layers independently. Exact requires retained per-layer
color/coverage/depth through DoF; AVBOIT requires multiple depth/color bins or
remains approximate. Do not retry point sampling or change the representative
depth as a complete fix: both only redistribute the same inseparable color.

### Acceptance checks

- Face focus: strands stay sharp while scenery visible through their alpha
  remains blurred, with no abrupt change at the face silhouette.
- Background focus: both hair color and its coverage blur without dark halos.
- Legacy/fullbright/PBR hair and glass-over-hair preserve their colors.
- DoF-off output, OIT overflow fallback, glow and isolate behavior remain
  consistent with their existing paths.
- Measure added GPU time/memory at normal resolution and in avatar crowds.

Validation is limited to source review, numerical checks of reconstruction,
coverage averaging and CoC algebra, and `git diff --check`. No project build,
shader compilation or runtime validation was performed, per instructions.

### Three-mode acceptance baseline and Exact DoF requirement (2026-09-06)

The following same-scene references define the current failures:

- `AyaneStormOS-Normal_VZkwZltZS2.jpg`, Standard: base skin, makeup and jewelry
  retain correct color relationships and vanilla-quality bokeh, but transparent
  hair inherits background depth.
- `AyaneStormOS-Normal_oSed6heqXJ.jpg`, Exact OIT: facial layers separate
  visibly. Base skin and parts of the eyebrows blur while eyes, lashes, lips
  and other transparent overlays remain sharp. The face no longer reads as one
  focused subject.
- `AyaneStormOS-Normal_WyBuUV58zM.jpg`, AVBOIT: the face remains coherent, but
  thin hair in front of the window retains sharp window structure and jewelry
  is brighter than the Standard reference.

This rejects the shared aggregate representation as a final implementation.
Sampling changes cannot recover information already collapsed into one
transparent residual, one coverage value and one representative depth. Glow
also cannot share coverage normalization without changing jewelry intensity.

The replacement must be a scene-wide Exact DoF path that responds to the
existing focus, f-number, focal-length, field-of-view, maximum-CoC,
resolution-scale and edit-mode controls. Its required ordering is:

1. Preserve the opaque color/depth input and blur it with vanilla-equivalent
   quality.
2. Preserve visible transparent color, coverage, glow and depth in multiple
   layers or CoC bins; do not reduce them to one representative depth.
3. Blur contributions at their own depths, farthest first.
4. Composite them back-to-front with their original attenuation/blend
   semantics, keeping glow separate from opacity.
5. Use full-resolution depth/coverage guidance around hair while allowing a
   reduced-resolution color blur controlled by `CameraDoFResScale`.

Exact OIT can supply individual retained nodes for this path. AVBOIT cannot be
mathematically exact after aggregation; it needs several color/depth bins and
must preserve energy/glow separately. Until those representations exist, the
current OIT DoF output is experimental and cannot meet this visual baseline.

### Exact DoF implementation decision

Exact DoF will read Exact OIT's retained per-pixel linked lists after sorting.
It will not introduce a fixed four/eight-layer texture representation: that
would recreate the same failure once a pixel exceeded the chosen limit.

The first correctness implementation must:

- disable the shallow-list shortcut while Exact DoF is active, ensuring every
  multi-node list remains globally far-to-near after resolve;
- expose the sorted head texture and node SSBO read-only to a dedicated Exact
  DoF shader;
- preserve the opaque scene as the farthest contribution;
- evaluate CoC from each node's own window depth;
- preserve standard source-over attenuation and keep glow as a separate energy
  channel; custom destination-dependent blends require an explicit fallback;
- apply focused center contributions without averaging them into the blurred
  background, then composite progressively from far to near;
- use the existing vanilla focus calculation and every existing DoF slider;
- retain the ordinary vanilla path on shader/resource failure.

The direct linked-list gather is intentionally correctness-first and can cost
roughly `aperture samples × transparent nodes per sampled pixel`. Optimization
must follow profiling. Safe later fast paths include single-node pixels,
uniform-CoC runs and tiles with no transparency; fixed layer truncation is not
an acceptable optimization.

### First Exact DoF implementation (2026-09-06)

The Exact branch of `oitDoFF.glsl` now reads the retained sorted head texture
and node SSBO directly. The shallow-list shortcut is disabled only while Exact
DoF is active so every multi-node list remains globally far-to-near. For each
pixel the shader:

1. applies the vanilla concentric blur to opaque color/depth;
2. evaluates CoC independently for every Exact node;
3. gathers defocused node contributions with the vanilla ring density;
4. composites focused center nodes over the blurred far contributions using
   the captured blend factors; and
5. carries glow separately through the same node equations used by Exact
   resolve.

Exact overflow now invalidates the capture before same-frame vanilla fallback,
so the incomplete node list cannot enter Exact DoF. The shader/resource checks
also require both the head texture and node buffer.

This first correctness path deliberately favors image quality over cost. A
focused layer behind a defocused foreground layer is still a hard screen-space
case: selected focused and defocused sets can interleave in depth. Runtime
testing must cover foreground glass/hair with the background in focus as well
as the primary face-focus case. No truncation or representative depth is used.

The first runtime build fell back to vanilla because `oitDoFF.glsl` was emitted
as GLSL 4.20. Its Exact node SSBO requires GLSL 4.30; NVIDIA reported a syntax
error at `ExactDoFNodes`, followed by undefined `oitNodes`, and the program
failed to link. Shader-version selection now classifies `oitDoF` with the other
OIT storage shaders, selecting GLSL 4.30 (or 4.50 when subgroup support is
active). This explains why that build looked exactly like vanilla DoF.
