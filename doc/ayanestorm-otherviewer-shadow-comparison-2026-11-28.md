# AyaneStorm shadow comparison with another viewer

Date: 2026-11-28. Author: chanayane@firestorm.

## Findings that take priority

The extended source audit found correctness problems beyond the filter and
bandwidth differences identified in the first review:

1. **Partial static-cache updates can accumulate opacity twice in clean tiles.**
2. **Off-screen skinned meshes without a usable coarser shadow copy lose their
   shadow geometry before shadow culling.**
3. **Masked materials' displayed alpha cutoff is not applied to their shadows.**
4. **Contact-shadow reach follows the first cascade's bias, even when the
   receiver reads a coarser cascade with a larger bias.**

These follow from the current source paths. Arithmetic counterexamples were
checked separately; they are not GPU reproductions of the user's scene.
Correctness should be addressed before reducing samples or increasing blur.

### Fixed, 28 November 2026 (asrenderer_test, Vulkan and Metal; viewer unbuilt)

1. **Opacity in clean tiles:** in the static phase, the opacity fragment stage
   discards texels outside dirty tiles, with the same tile test as the shift
   (`asrShadowAlpha.glsl`). Depth is unchanged; it was already idempotent.
2. **Off-screen skinned casters:** a skinned binding without a shadow twin keeps a
   LOD off screen (`bind()` in `asrSceneSkinC.glsl`). It is still deformed only
   when the shadow cull admits it (skin demand).
3. **Masked cutoff:** the shadow vertex stage turns a masked material's cutoff into
   a texture alpha threshold, `cutoff / colour alpha`. Above 1 nothing casts, at 0
   everything is solid, and colour alpha no longer thins a masked shadow.
   Blended materials are unchanged. Limit kept: a masked texture whose own alpha
   is evenly partial is still read as a coverage share, because exactness would
   need finer texture reads per shadow texel.
4. **Contact reach:** taken from the cascade the resolve reads at the receiver,
   and from the next one inside the blend band. The 6 cm cap is unchanged.
5. **Collapsed shadow LODs:** an LOD with under half the finest skinnable LOD's
   surface (mean triangle width squared times count) is never a shadow LOD.
   `lodWidth` sends it as -1, and a twin never holds it; with none left there is
   no twin, so the main binding casts (item 2).
6. **Metal:** the shadow draw now passes its push constants to the fragment stage
   too. Before, the masked depth stage's texture demand and the new gate read an
   unset buffer, which dropped every static blended shadow once the gate existed.

Checks, `--contact-test --sun-elevation 20` with the camera lines it prints:
- Three skinned tubes outside the view: one LOD, a coarse LOD collapsed to 8
  triangles, and two LODs as control. All three cast their full strip into the
  view.
- A 50% blended card with a box on its edge that comes and goes every 0.5 s.
  With the gate disabled and interval 0, the card's shadow darkened over time
  (checker squares 97 to 77 and 50 to 38). With the gate, over 90 consecutive
  frames at 30 ms, the region stays within 0.02 (Windows 73.79 to 73.81, Mac
  73.84 to 73.85). The box's own shadow follows it within one frame.
- Masked cards at colour alpha 0.6: cutoff 0.7 shows and casts nothing,
  cutoff 0.3 casts a solid shadow.

## Scope and verification limits

Source audit of AyaneStorm's new Vulkan/Metal renderer against
the local source of another viewer. The OpenGL
pipeline is not the subject of this comparison. No renderer code was changed,
no build was attempted, and no viewer was launched. Findings describe the current
source, which can differ from the binaries used in earlier visual comparisons.
The extended audit covers both graphics backends, draw-state setup, cache
invalidation, skinning and shadow LOD, receiver and caster culling, texture
feedback, contact filtering, conventional and soft raster variants, ray-traced
path selection, mask consumers, and profiling boundaries. Detailed findings and
remaining validation requirements follow below.

Another viewer has three materially different paths: conventional raster shadows,
optional soft raster shadows, and ray-traced shadows. Its soft raster path is
off by default according to its development notes.
Its ray-traced path can bypass the raster sun result.
An actual comparison needs the active path and equal hardware,
camera, content, reach, resolution, contact-shadow and AA settings.

## 1. Filtering cost: substantial work even without a wide penumbra

AyaneStorm's cascade filter performs the following work:

- The fixed box is a 3 by 3 loop of gathers: **9 gathers**.
- The outer blocker search adds **4 gathers** unconditionally.
- A wide penumbra adds **16 gathers**, for **29** total.
- The opacity filter adds one central sample and four filtered samples, even for
  a point whose opacity texels are empty.
- A cascade blend can run both functions again in the adjacent cascade.
- Cross-cascade sampling replaces one gather with four explicit texel reads
  when redirecting into the finer map. This trades repeated caster rendering for
  extra work at each receiving lookup.

Thus one cascade costs 13 or 29 depth gathers plus five opacity samples, before
considering a blend. These are shader operation counts, not measured DRAM reads
or interchangeable units of GPU time. There is no all-lit/all-shadowed shortcut
before the box and outer search finish. The older design note's description of
eight gathers and early-outs is stale relative to this source.

The other viewer uses a min-depth hierarchy
over blocks of 4, 8 and 16 texels, one local gather and eight block searches,
normally 12 hardware comparison filter samples for the soft portion, and two
9-tap bilateral blur passes. The conventional filter can still run for narrow
penumbras and during its transition. This is not a universal 12-sample cost.

The other viewer computes the soft result from prepass depth, stores it in a mask, and
reads that mask in surface shading when the receiver matches its stored depth.
Other surfaces fall back to conventional filtering.
AyaneStorm evaluates shadows directly during opaque shading and
at each transparent layer. Its existing sun shadow mask holds contact shadows, not the
combined PCSS result. **Moving the same filter to a mask alone is not a speed
fix:** AyaneStorm's opaque visibility resolve already evaluates the visible
surface once. The opportunity is a cheaper filter supported by spatial smoothing,
and a separate cost policy for other consumers.

## 2. Contact quality: blocker weighting and filtering scale

AyaneStorm averages all blockers above its wide-penumbra threshold equally. That
threshold is the height required to make a penumbra wider than two cascade
texels. A nearby contact caster below it does not constrain distant high
blockers, which can set a wide filter at the contact.

The other viewer first identifies local blockers distinguishable from the
receiver's own slope. It limits distant blockers' weight using a nearby-blocker
distance limit and a smooth falloff. This directly addresses distant blockers washing out a
near caster's contact; the other viewer's source and task notes describe that case.

AyaneStorm's minimum box radius is two shadow texels: its physical width grows
with cascade texel size. The other viewer starts from a 3 cm conventional penumbra and a
1.5 cm soft radius floor, but also clamps footprints to the map's texel scale.
It is not resolution independent; it does make physical size an explicit target.
AyaneStorm's wide-disc sample positions are rounded to texel boundaries with a
fixed pattern and no subsequent bilateral smoothing. Their positions change
discretely as a receiver moves. The other viewer rotates its sample pattern and
spatially blurs the result. Its rotation phase changes between frames only while
temporal antialiasing is active; otherwise the phase is fixed, including photos.
The soft-shadow pass itself uses the current frame, but the final antialiasing
can accumulate those changing patterns. A comparison must therefore distinguish
spatial filtering from the extra stability supplied by temporal antialiasing.

## 3. Extra opacity rendering, memory and approximation error

AyaneStorm adds RG16F working and cached opacity arrays alongside D16 depth arrays.
Masked/blended geometry runs twice: depth first, then additive optical depth.
The other viewer's raster path instead uses alpha cutoffs and alternating
columns. It does not perform this separate optical-depth render. The two paths
are buying different transparency behavior.

At four 2048 squared layers:

| AyaneStorm allocation | MiB |
| --- | ---: |
| Working and cached D16 depth | 64 |
| Working and cached RG16F opacity | 128 |
| Total arrays | 192 |

This excludes auxiliary buffers and contact masks. The opacity feature accounts
for two thirds of these array bytes, plus extra rasterization and lookup work.

AyaneStorm's opacity filter averages optical depth and then exponentiates it. This is not
filtering transmitted light. For equal weights over texels transmitting 1.0
and 0.1, it gives `exp(-log(10)/2) = 0.316`, whereas filtering their transmission
gives `0.55`. It can therefore darken partially covered edges relative to an
area-coverage average. This scalar example is verified independently of a GPU;
the size of the visible error needs an actual scene.

Only total optical depth and its weighted mean caster depth are stored. Multiple
caster layers cannot be reconstructed for receivers between those layers.
This is a representation limit, not proof that every hair shadow is incorrect.

## 4. Static caching still incurs full-map work

The cache restore writes every texel in all four working layers every frame,
including between cache updates. The cache store copies both complete arrays
back into the cache on update frames. Static dirty-tile culling saves geometry,
but does not make these transfers proportional to dirty area.

At 2048 squared, a full depth-plus-opacity map is 96 MiB. The shift logically
reads and writes 192 MiB per frame; the store adds 192 MiB on an update frame.
Dirty tiles avoid their cache reads. Actual traffic depends on caching,
compression, attachment behavior and the driver: these are nominal payload
counts, not measured bandwidth or timings.

The default static update interval is **30 milliseconds**, not 30 frames.
Moving casters still draw every frame. The shift therefore remains on frames
where the static store and redraw are skipped. On a stationary mostly static
scene, this copy work may be unnecessary overhead compared with reuse of an
unchanged map; in a dense static scene caching may still save much more raster
work than it costs. Measure the tradeoff rather than removing caching outright.

## 5. Shadow combination and light stepping affect appearance

AyaneStorm multiplies map visibility by contact visibility.
The other viewer uses their minimum in the raster path.
Two overlapping partial results of 0.5 become 0.25 in AyaneStorm and 0.5 in
another viewer. Multiplication can double-darken the same partially occluded light.
Whether it does so in the reported view requires checking the two masks.

AyaneStorm defaults to a 0.25 degree cached sun step.
A threshold crossing changes the maps' light
direction discretely. At a 10 m caster-receiver separation, 0.25 degrees
corresponds to about **4.36 cm** of transverse displacement; grazing geometry
can amplify the displacement. Cache updates also wait for their interval.
The other viewer's cascade matrices follow the current sun and anchor their texel grid
near each cascade to reduce drift. Caching and
light stepping are deliberate performance/quality tradeoffs, not equivalent
smoothness guarantees.

## 6. Shared maps are not fully valid for every consumer

AyaneStorm marks receiver tiles from opaque screen
depth. Moving casters are culled against those tiles. A transparent layer over
sky cannot request its own moving-caster shadow through this mechanism.
This is a confirmed coverage limitation for transparency despite the shared
sampling function. Future water, volumetrics and probes also need appropriate
receiver coverage or a conservative fallback before these maps can serve them
reliably. The other viewer also makes view-dependent compromises; its screen mask has an
explicit matching-receiver test and conventional filtering fallback.

## Recommended implementation and measurement order

First correct partial-cache opacity accumulation, off-screen shadow geometry,
and masked-material semantics. Then verify contact reach and receiver coverage.
Those changes need focused reproduction scenes before performance tuning.

1. Capture current pass timings on one GPU and fixed scene: shadow fit/cull,
   shift, static draw, store, moving draw, contact shadows, opaque resolve and
   OIT. Compare against the other viewer's raster path with its soft setting explicitly
   identified. Whole-frame FPS alone cannot assign the slowdown to shadows.
2. Isolate opacity cost with a temporary diagnostic variant that omits its
   allocation/transfers, second draw and sampling together. Preserve the
   current path for quality comparison; simply hiding its final darkening
   would leave most of its cost intact.
3. Add near-blocker weighting and evaluate a cheaper PCSS filter with a
   current-frame bilateral mask. Retain the conventional/shared-map path for
   receivers the mask does not cover. Do not promise a gain from mask storage
   alone or from copying the other viewer's sample counts without its smoothing.
4. Compare multiplication versus minimum with separate map/contact debug
   images, especially on faces. Test thin contacts and low sun as well as noon.
5. Profile the cache copies before changing storage. Evaluate lower-resolution
   or selective opacity storage and clean-map reuse against the actual cost of
   redrawing static content. Keep moving casters correct every frame.
6. Test continuous sun motion against the cached step, then fix receiver
   coverage for transparent/off-screen consumers.

The existing AyaneStorm design note records an earlier Mac resolve increase
from 1.1 ms to 1.55 ms after PCSS. The other viewer's task notes record soft-shadow GPU
timings on an RTX 5090. These are historical measurements on different workloads
and hardware, not evidence of a measured cross-viewer speed ratio today.

## 7. Confirmed source defect: opacity accumulates outside dirty tiles

The cache restore clears dirty tiles and preserves clean tiles. Static caster
culling admits a cluster when any part of its bounding sphere touches a dirty
tile. The subsequent draw renders that cluster's whole footprint. There is no
per-fragment dirty-tile rejection in the opacity pass, and the raster scissor
covers the complete map. Both graphics backends use additive blending there.

Consequently, an unchanged static leaf card crossing a dirty/clean boundary is
added again over its previous opacity in the clean tile. That modified clean
tile is then copied back into the persistent cache. Depth-only redraws can be
idempotent; adding optical depth is not.

A constant 50% covering sheet gives a direct counterexample:

| Clean texel state | Transmission |
| --- | ---: |
| Correct initial contribution | 0.5 |
| One duplicate contribution | 0.25 |
| Ten duplicate contributions | 0.000488 |

No material or light change is required at that texel. A neighbouring dirty
tile or a scrolled-in strip can cause the cluster to be selected again. A full
cache refresh can erase the accumulated error, making the artifact appear
dependent on movement and update history.

The safe invariant is that a static opacity redraw modifies only texels whose
previous static contribution was cleared, or that every texel being redrawn is
cleared and all contributors to it are rebuilt. Fixing this does not require
changing the optical-depth representation itself.

## 8. Confirmed source defect: off-screen single-LOD skinned casters disappear

The skinned binding setup chooses no main geometry when its conservative bound
is outside the main camera. It then publishes zero clusters for that binding.
The shadow-only copy can rescue an off-screen caster only if a valid coarser
skinnable LOD exists. For a single-LOD chain, that copy is not created.

Shadow culling subsequently sees no geometry to admit. A skinned object outside
the camera can therefore cast no shadow onto an in-view floor, even if its
light-space bounds would otherwise pass. This is upstream of receiver-tile
culling and cannot be fixed by more shadow resolution or samples.

Another viewer tests eligibility for the camera and for shadows independently;
being outside its camera does not by itself discard the shadow candidate.

The required invariant is that shadow demand can select and deform valid
geometry independently of main-camera visibility. Reuse main-view deformation
when it exists; do not require a coarser LOD to retain an off-screen shadow.

## 9. Masked-material cutoff and opacity representation

AyaneStorm's visible masked pass compares texture alpha times face alpha with
the material's cutoff. The shadow vertex stage supplies that cutoff, but the
shadow fragment stages do not use it. Masked and blended casters instead share
the rule: coverage at least 0.9 writes solid depth, lower coverage above the
faint threshold writes optical depth.

For a spatially constant alpha of 0.6 and a cutoff of 0.7, the displayed masked
surface is absent, but its shadow contributes optical depth corresponding to
40% transmission. Conversely, a material whose cutoff is 0.3 displays that
same texel as solid while its shadow remains partly transmissive. Constant
alpha avoids ambiguity about different texture mips or edge antialiasing.

Another viewer preserves the material cutoff in its masked raster shadows.
Its blended-material policy remains different from AyaneStorm's optical model,
so replacing all alpha handling with one universal threshold would not solve
both cases.

The optical-depth representation also has a separate depth-order limitation.
Consider two layers of unit optical depth at reversed light depths 0.8 and 0.2,
with a receiver at 0.6. Only the layer at 0.8 is in front of the receiver, giving
transmission exp(-1), about 0.368. Their stored mean depth is 0.5: the current
mean-depth gate can reject both and return transmission 1. This independently
checked example proves that a mean depth cannot represent arbitrary layered
receivers. It does not establish how often the user's content triggers it.

Coarse-cascade opacity filtering has another inconsistency: its central sample,
which estimates caster height and filter radius, always comes from the coarse
map. The four attenuation samples can instead be redirected into the finer map.
A moving caster omitted from the coarse map can therefore influence attenuation
without influencing the radius estimate. The radius and attenuation should be
derived from compatible data. Its visible impact needs a cascade-boundary test.

## 10. Contact shadows differ in coverage, shape and reach

| Behavior | AyaneStorm | Another viewer |
| --- | --- | --- |
| Normal window sampling limit | Up to 128 comparisons | 60 samples |
| Trace extent | Short physical reach, at most 6 cm; receivers within 12 m | Pixel-based length, scaled for photos |
| Occluders | Opaque and masked depth | Also sufficiently opaque blended surfaces and impostors |
| Raw result | Binary visibility | Fractional result with sample grouping and endpoint fade |
| Subsequent filter | None | Surface-aware 5 by 5 tent filter |
| Combination with map visibility | Multiplication | Minimum |

The comparison counts are not a timing ratio: the traversal, early-outs, shared
reads and filtering costs differ. Another viewer also has device-specific
variants that avoid shared-memory operations on particular GPUs.

AyaneStorm's hard result can show whole-pixel steps and disappears when fewer
than two projected sample rows fit within its short reach. At greater distances,
small contact gaps can become too short in pixels even before the 12 m cutoff.
Its grazing-angle suppression reduces self-shadowing but also suppresses real
contact shadows in those directions. Neither is repaired by increasing PCSS
search samples.

The contact pass computes reach from cascade zero regardless of the cascade
actually shading the receiver. At 512 m shadow reach and 2048 texels, a first
cascade radius around 4.76 m gives a contact reach around 1.63 cm. A second
cascade radius around 22.63 m has approximately 5.52 cm of combined normal and
depth receiver offset when the receiver normal points towards the light.
Such receivers can be within the 12 m contact range, leaving part of the map's
bias gap uncovered. This is a mismatch in the intended gap-filling policy,
independent of whether the trace is fast.

## 11. Cascade geometry, bias and sharing are different tradeoffs

AyaneStorm uses four nested, camera-centred light-space windows with logarithmic
radii, quantized size, scrolling static caches and selection by window coverage.
Another viewer uses three windows fitted around view-frustum slices, selected
by per-pixel distance, with a 15% distance transition. AyaneStorm uses an 8%
window-edge transition. Equal map dimensions do not imply equal world-space
resolution, coverage, overlap or transition cost.

AyaneStorm can have finer resolution near the camera than the other approach.
It is therefore incorrect to attribute every quality difference to insufficient
resolution. Its outer windows cover areas behind the camera that a view-slice
fit can avoid. Static caching benefits from those stable windows; caster and
copy costs pay for their larger coverage.

Both raster paths draw opaque casters without a fragment shader and both cast
from both sides. That optimization is already present in AyaneStorm.
AyaneStorm uses 16-bit depth and receiver-side bias with slope tolerances.
Another viewer uses 32-bit depth plus raster constant/slope bias and receiver
offsets. AyaneStorm's unsigned box tolerance allows extra depth in either
sampling direction to avoid self-shadowing. That can also remove real shallow
occlusion on slopes. Treat bias and slope handling as a quality tradeoff to
measure, rather than copying constants between different depth conventions.

Another viewer merges opaque finer-map depth into coarser maps once, adjusting
for slope and retaining the nearer depth. Its alpha casters are drawn separately
in each cascade, preserving their cascade-dependent texture filtering.
AyaneStorm instead redirects receiving lookups into finer maps and eliminates
eligible moving clusters, including alpha casters, from coarser draws. This
saves caster work but adds sampling work and changes the alpha footprint. It
must be evaluated on hair, leaf cards and cascade boundaries, not opaque boxes
alone. A full comparison should count transformed vertices as well as triangles.

## 12. Geometry and deformation cost hidden by draw-call counts

AyaneStorm's cluster draw emits non-indexed triangle corners and fetches the
source index and position in the vertex shader. A shared mesh vertex is still
a separate invocation at each triangle corner. Fixed bucket sizes also invoke
vertices for spare triangles before collapsing them. Another viewer uses
indexed indirect mesh draws, which permit reuse of transformed vertices.
Few draw calls do not establish low vertex-processing cost.

AyaneStorm already computes deformed positions on demand and can share them
between views. However, when a shadow-only copy selects a coarser LOD, it has its
own binding and deformed-position storage. Main-view and shadow-copy geometry
can both require deformation in one frame. Palette sharing does not make that
second deformation free. The selected shadow LOD uses mean triangle area, not
a maximum silhouette or geometric-error bound: a low mean does not guarantee
that all important details survive. This is an approximation to test on real
avatar meshes.

The source also evaluates shadow visibility before the final fullbright mix in
AyaneStorm's opaque and transparent shading. Another viewer branches around
surface shadow evaluation for fullbright materials. There is an opportunity to
avoid unused receiver work, especially across many transparent layers; measure
actual compiled-shader behavior before assigning a time saving. The screen-mask
path can still pay its own computation cost at those pixels.

## 13. Cache invalidation, ordering and backend observations

Static cache invalidation is not missing wholesale. The audit traced movement,
settling, instance changes, material changes, texture residency changes, LOD
changes, camera scrolling, size changes, depth-origin changes and light steps.
Moving and skeleton-bound content is redrawn every frame. Static invalidations
can wait for the configured interval, which is an explicit freshness tradeoff.
The opacity duplication defect arises after candidate selection; adding more
invalidation events does not correct it.

Both backends bind the depth and opacity arrays together, restore the cache,
draw static depth and opacity, copy the static result back, and then draw moving
depth and opacity. Their opacity blending and map formats agree. This supports
the cache defect applying to both backends rather than being a single-driver
explanation.

The Vulkan graph declares the transfer, attachment, compute, fragment and
indirect dependencies for the reviewed shadow stages. The Metal backend orders
encoders using a shared fence. No missing inter-pass shadow dependency was
established in this audit. That is not equivalent to a validation-layer or
GPU-capture pass. Metal's repeated attachment loads/stores and encoder
boundaries are also real work on a tile-based GPU, but their cost cannot be
derived from source alone.

One backend's pass timings include graph transitions before its pass body;
the other's encoder samples measure its GPU encoders. The other viewer exposes
separate caster, soft-shadow and contact timing regions. Compare aggregate
equivalent work as well as individual labels, since boundaries differ.

## 14. Settings and consumers required for an honest comparison

Another viewer's ordinary shadow quality steps request approximately:

| Step | Cascade resolution | Maximum shadow reach |
| --- | ---: | ---: |
| Low | 1024 | 48 m |
| Medium | 2048 | 96 m |
| High | 2048 | 192 m |
| Highest | 4096 | 256 m |

AyaneStorm derives resolution from its memory-budget tier and resolution scale:
normally 1024 or 2048, bounded between 512 and 4096. Reach follows draw distance
up to 512 m. Matching a quality label can therefore compare a short shadow
range in another viewer against a much longer one in AyaneStorm. That changes
caster counts, map detail, bias and filtering footprints simultaneously.

The other viewer's soft-shadow option and its ray-traced path are independently
selectable. Its soft raster path can be compiled out when disabled. A softness
of zero also skips its mask. Its ray-traced soft sun shadows use changing rays,
history and denoising, unlike the raster path. Do not attribute a ray-traced
result's quality to its raster PCSS implementation.

Its screen mask has a receiver-depth check and falls back when a surface does
not match. Its main-view soft result is not universally reused by transparent
layers, water, reflections and probes. Those paths can retain conventional
filtering, use other maps, or deliberately omit a shadow. AyaneStorm shades
transparent layers with its full shared filter, so transparency-heavy scenes
can have materially different work even with the same opaque image.

The reviewed AyaneStorm graph currently provides directional maps. The other
renderer also has projector shadow maps and optional traced local-light
shadows. Local lights, water, reflections and probe captures must either be
excluded from a directional-shadow benchmark or accounted for separately.
They cannot be inferred from a single whole-frame FPS number.

## 15. Verification performed and what remains

Verified by source tracing on both sides:

- Active-path selection, quality defaults and resolution/reach mapping.
- Cascade construction, raster state, depth convention, bias and blending.
- Opaque and alpha caster handling, skinning demand and off-screen eligibility.
- Static-cache restore, invalidation, redraw and store, on both backends.
- Receiver culling, finer-cascade sharing and opacity sampling.
- Contact traces, masks, filters and their consumers.
- Spatial soft-shadow filtering, temporal-antialiasing interaction and
  ray-traced path separation.
- Resource dependencies and timing boundaries around the reviewed passes.

Independent arithmetic checks passed for duplicate optical-depth accumulation,
the two-layer mean-depth counterexample, filter transmission averaging, cascade
scale and receiver-bias mismatch, memory payload counts, and angular light-step
displacement. Source checks also confirmed the absence of a dirty-fragment gate
and material-cutoff use in the shadow opacity stages, and the off-screen binding
flow. These checks are not shader execution or visual validation.

An existing standalone test executable was found, but current shadow-related
source is newer than that executable. Running it would not establish the
behavior of all currently reviewed source. No rebuild was attempted, in keeping
with project instructions. No viewer runtime or driver validation was performed.

Required focused runtime checks, after implementation work is authorized:

1. A constant-alpha static card across two tiles; dirty one tile repeatedly.
   The unchanged tile's shadow must remain identical to a full rebuild.
2. A single-LOD skinned caster outside the camera with its shadow on an in-view
   receiver; rotate the camera without moving either object.
3. Uniform masked alpha with cutoffs above and below it, then real leaf/hair
   textures at several distances.
4. A shallow contact across a cascade transition, within 12 m, and the same
   contact at different screen resolutions.
5. Hair in front of and behind a receiver, and moving alpha near cascade edges.
6. Low and high sun with separate map/contact images; continuous sun motion;
   a camera move, a static edit and a texture-residency change.
7. Matched settings and fixed content on one GPU, recording caster, copy,
   contact, receiver filtering and transparency cost separately. Repeat on
   Metal before generalizing the timing result across platforms.

No rendering changes were made during this audit. The report identifies
source-level defects and performance mechanisms; it does not claim a measured
cross-viewer speed ratio or that every reported visual symptom has been reproduced.
