# AVBOIT SIGGRAPH 2025 implementation notes

## Source

Michal Drobot, *Adaptive Voxel-Based Order Independent Transparency*,
Advances in Real-Time Rendering in Games, SIGGRAPH 2025. Local source:
`doc/AVBOIT_SIG2025_MDROBOT-final.pdf`.

## Core algorithm

The production algorithm is a transmittance-integral weighted OIT method:

1. Build conservative transparency depth occupancy.
2. Generate an adaptive one-dimensional depth-warp LUT.
3. Clear occupied extinction voxels.
4. Splat logarithmic extinction into a reduced-resolution 3D volume.
5. Integrate extinction front-to-back into a filterable transmittance LUT.
6. Render transparency at full resolution in any order, weighting color by
   front transmittance.
7. Normalize accumulated color and resolve it with total extinction over the
   opaque scene.

### AyaneStorm mapping to the VBOIT basic steps

The direct-raster implementation follows the presentation's baseline sequence:

- `beginDirectFrame()` preserves the opaque scene and clears frame occupancy.
- raster pass 0 records conservative depth occupancy at one-eighth resolution;
- `finishDirectOccupancy()` builds the adaptive depth-warp LUT and sparsely
  clears the extinction and integrated-transmittance volumes;
- raster pass 1 splats logarithmic extinction at one-eighth resolution;
- `finishDirectExtinction()` integrates extinction front-to-back into the
  filterable 3D transmittance texture;
- raster pass 2 draws transparency at full resolution in arbitrary order,
  accumulating color weighted by `alpha * front_transmittance`, normalization
  weight, total logarithmic extinction, and glow;
- `finishDirectFrame()` normalizes accumulated transparent color and resolves
  it over the preserved opaque scene using total transmittance.

The presentation lists opaque rendering between extinction integration and the
full-resolution transparency draw. The viewer has already completed opaque
deferred rendering before its post-water alpha pool runs, so AyaneStorm copies
the existing opaque target at the start of AVBOIT instead. Opaque rendering is
independent of the transparency-volume construction, making this scheduling
difference mathematically equivalent.

The phrase "fit curve distribution for slices over depth" is partially
implemented. AyaneStorm uses a logarithmic mapping from the camera near/far
range into 8192 high-resolution virtual slices. The warp builder tests
conservative occupancy at group sizes 1, 2, 4, and so on. Each increase halves
effective virtual resolution without rerasterizing coverage, and the builder
stops at the finest resolution whose occupied groups fit 128 physical slices.
Sampling the resulting grouped LUT is equivalent to using that reduced
logarithmic parametrization.

Revision v117 completes selection of the initial high-resolution curve.
AyaneStorm keeps the specified 8192 virtual slices but solves the
linearization factor from a requested minimum world-space slice thickness and
the live far plane. The default requested precision is one centimetre and is
explicitly exposed as a viewer setting rather than hidden in the equation.

The presentation uses 128 physical slices, an 8K depth-warp LUT, and a
transmittance prepass at one-eighth resolution. Those values match the initial
AyaneStorm prototype, but the algorithms using them differ materially.

## Filtering requirements

The presentation identifies depth aliasing and self-occlusion as primary VBOIT
failure modes:

- Point splatting and sampling causes hard slice transitions.
- Extinction should be split linearly between adjacent depth slices.
- The integrated transmittance should be sampled with hardware linear
  filtering.
- Linear sampling over linear splats needs an approximately two-slice bias
  toward the camera to avoid a surface attenuating itself.
- Bicubic sampling needs approximately 2.5 slices of bias.
- The depth-warp LUT must preserve filterable range boundaries so filtering
  does not interpolate incorrectly across compacted empty depth.

This directly applies to the dark hair patches seen in the AyaneStorm
prototype. Revision v21 introduced linear transmittance sampling but retained
point extinction splats and no self-occlusion bias. That combination is not the
algorithm described by the presentation and is incomplete.

## Adaptive depth packing

The production method starts from a parametrized logarithmic depth curve and a
requested minimum slice thickness. It builds virtual-slice occupancy, tests
whether occupied virtual slices fit the physical budget, and repeatedly lowers
virtual resolution when necessary. A prefix sum compacts occupied depth into
the physical volume. The LUT marks the starts, middles, and ends of occupied
ranges so sampling can snap across skipped empty space while remaining
filterable.

The current prototype merely distributes occupied 8K bins across 128 physical
slices by ordinal. It does not yet implement:

- logarithmic depth parametrization;
- iterative virtual-resolution reduction;
- conservative range-boundary markers;
- filterable warp interpolation across compacted empty regions.

These omissions can create unstable precision and slice mixing, especially for
close hair cards and other thin overlapping surfaces.

## Performance features

The presentation's main performance gains do not come solely from avoiding
sorting:

- A low-resolution tiled depth prepass marks sparse spatial/depth occupancy.
- Clear and integration skip empty tiles and slices.
- Integration stops when transmittance reaches effective zero.
- The zero-transmittance depth generates conservative depth geometry used to
  cull transparency behind fully extinct regions.
- Extinction slices are packed into integer words; overflow depth is recorded
  instead of using expensive saturating compare-and-swap loops.
- Monochrome and chromatic extinction can be separated because most VFX use
  scalar extinction.
- The final transparency draw is genuinely full-resolution rasterization into
  accumulation targets rather than traversal of previously captured linked
  lists.

The AyaneStorm prototype still captures and traverses every Exact OIT node and
clears/integrates the complete physical volume. It therefore cannot be expected
to reproduce the paper's performance results yet.

### DRO17 Z-binning relevance

The cited local source,
`doc/2017_Sig_Improved_Culling_final.pdf`, is *Improved Culling for Tiled and
Clustered Rendering* (Michal Drobot, SIGGRAPH 2017). It is not another OIT
algorithm. AVBOIT borrows its depth-indirection and conservative sparse-culling
techniques.

The DRO17 F+ algorithm sorts entities by view depth and stores a
packed 16-bit minimum/maximum sorted entity ID for each uniform Z bin. GPU
waves merge those ranges, intersect spatial entity masks with a generated
Z-range bitmask, merge surviving bits across lanes, and iterate the resulting
scalar candidate mask.

In AVBOIT this technique belongs before conservative bounds voxelization. It
reduces the transparent mesh/VFX candidates considered by a screen/depth work
group; it does not replace the later one-dimensional occupancy prefix sum or
depth-warp LUT. OpenGL 4.3 also requires a shared-memory fallback for the
presentation's wave operations.

The source's raster-culling section is also applicable: conservative proxy
geometry atomically marks entity bits in 8-by-8 screen tiles, optionally using
4x MSAA to emulate conservative rasterization, early depth bounds, and
wave-compacted duplicate atomic writes. Its cluster variant derives
conservative triangle depth bounds from derivatives plus all three vertex
depths. Those methods can generate the missing transparent-bound occupancy,
provided AyaneStorm preserves false-positive coverage and never drops a
potentially contributing alpha-tested draw.

The reported production examples show AVBOIT close to ordinary transparency,
not dramatically faster than it: one 4K PS5 example reports 2.95 ms for
monochrome AVBOIT versus 2.8 ms without OIT. A GTX 1060 1080p example reports
12.2 ms for RGB-transmittance AVBOIT versus 11.2 ms without OIT. Its value is
providing stable approximate OIT near conventional transparency cost, rather
than making transparency itself free.

## Quality limits

When multiple events occupy the same physical slice, VBOIT degenerates toward
weighted blended OIT. Linear splatting improves the mixture but does not make
it exact. The presentation states that a one-slice separation can be
artifact-free and emphasizes allocating sufficient slice resolution where
content exists.

Manual VFX sort order, 3D UI layering, custom composition, and other
non-physical ordering remain problematic. Depth offsets or separately composed
2D content are suggested for those cases. This reinforces that AVBOIT must
remain separate from strictly lossless Exact OIT.

## AyaneStorm implementation priorities

1. Pair filtered transmittance sampling with linear depth splatting and the
   required self-occlusion bias.
2. Make the adaptive warp filterable across occupied-range boundaries.
3. Replace global one-bit Z occupancy with conservative tiled XYZ occupancy.
4. Skip unoccupied clear and integration work.
5. Record zero-transmittance depth and use it to reject deeper transparency.
6. Only then replace linked-list capture with a lightweight extinction prepass
   and full-resolution accumulation pass.

The first two items are correctness work for the current prototype. Items
three through six are required before meaningful comparison with the
presentation's performance claims.

## Implementation audit after direct-raster revision v34

The AyaneStorm renderer implements the paper's broad VBOIT composition model,
but it is not yet a complete implementation of the presented AVBOIT system.

Implemented:

- a separate direct, node-free raster path;
- integer logarithmic-extinction atomics;
- an 8192-entry virtual-depth occupancy domain and depth-warp LUT;
- depth-linear extinction splatting between two physical slices;
- integrated 3D front transmittance with hardware linear sampling and a
  two-slice self-occlusion bias;
- full-resolution accumulated color, normalization weight, total extinction,
  and glow;
- zero-transmittance depth generation and rejection in the final raster pass;
- conservative neighboring-cell marking and spatially sparse clear/integration;
- automatic Exact OIT fallback when direct AVBOIT is unavailable.

Material differences and missing work:

- The warp builder performs a single-thread serial scan and stores only a
  physical-slice index. It does not encode filterable occupied-range
  boundaries, snap interpolation across empty ranges, or recompute fractional
  coordinates as described by the paper.
- The initial high-resolution logarithmic curve is fixed at 8192 virtual
  slices rather than fitted from a requested minimum world-space slice
  thickness. Iterative power-of-two resolution reduction and conservative
  occupancy rewriting are implemented by the grouped warp builder.
- Direct occupancy is produced by a full material raster traversal rather than
  conservative software raster bounds. Its tile mask indicates spatial use but
  does not preserve useful per-slice occupancy, so an occupied cell still
  clears and integrates every physical slice.
- Extinction is stored as one `R32UI` value per voxel. The paper's packed
  8-bit extinction representation, packed atomic update, and splat-time
  overflow-depth handling are not implemented.
- Zero-transmittance rejection occurs after fragment material evaluation in
  the final pass. The generated indirect depth quads and early-depth pipeline
  proposed by the paper are not implemented.
- Occupancy and extinction prepasses reuse the fully evaluated transparency
  shaders. They are not yet lightweight bounds/extinction-only passes.
- Full-resolution outputs use six fixed-point `uint` values per pixel rather
  than the paper's packed float render-target formats. This is an OpenGL 4.3
  compatibility design and has different precision, overflow, memory, and
  bandwidth behavior.
- Chromatic/RGB extinction, sparse RGB allocation, volumetric-fog coupling,
  distortion, motion-vector, refraction, and other optional extensions are not
  implemented. Custom Second Life blend modes are deliberately approximated
  as source-over.

Consequently, visual and performance results from the presentation cannot be
assumed for this implementation. The most relevant missing correctness feature
for the observed hair artifacts is the filterable warp-boundary representation.
The most relevant missing performance features are per-slice sparse work,
lightweight prepasses, packed extinction, and early-depth culling.

Revision v37 corrects a separate warp-builder defect discovered during this
audit. The original implementation stored occupied-bin ordinals rather than
prefix values at virtual-bin boundaries, making isolated occupied bins flat
instead of filterable. The corrected exclusive prefix gives occupied bins a
slope and leaves empty ranges constant. Explicit begin/end metadata and
oversubscription reparametrization remain to be implemented.

V37's first runtime result showed that scaling the exclusive prefix over the
entire physical range was not the presentation's compact mapping: sparse
coverage produced severe depth contours on layered clothing. V38 instead
coarsens virtual-depth groups until occupied coverage fits the 128-slice
budget, assigns one physical interval per occupied group, and stores
fractional compact coordinates in 16.16 fixed point. This implements the
presentation's resolution-reduction and compact-prefix behavior without
requiring a filterable integer texture format.

## Packed-extinction conformance in revision v59

Revision v58 packed four 8-bit extinction slices into each `R32UI` word, but
updated an individual lane with a saturating compare-and-swap loop. That was
the presentation's reference implementation, not its optimized production
method: slides 56 through 59 describe the spin loop as 2–10 times more
expensive and replace it with an integer atomic add plus minimum overflow
depth.

Revision v59 follows that method. Splatting now:

1. shifts the quantized extinction into its 8-bit lane;
2. adds the packed value with one image atomic operation;
3. detects lane overflow from the value returned by the atomic operation; and
4. records the earliest overflowing physical slice with `imageAtomicMin`.

Carry into a later packed lane is intentional. Once any lane overflows,
front-to-back integration saturates at the recorded slice and ignores all
extinction at that depth and behind it, so carried data cannot affect the
integrated transmittance. This removes the contended spin loop while preserving
the presentation's conservative zero-transmittance behavior.

## Filterable depth-warp ranges in revisions v60-v61

The v58 depth warp identified occupied entries with one generic filterable bit.
It did not distinguish the beginning and end of an occupied range. In addition,
a coarsened group used `offset / group_size`, so its final LUT entry stopped one
subdivision short of the physical interval endpoint. Snapping at a following
empty range therefore lost part of the occupied interval.

Revision v60 reserves separate range-begin and range-end bits alongside the
filterable bit and 16.16 physical coordinate. Occupied coarsened groups now span
both endpoints using `offset / (group_size - 1)` when the group contains more
than one virtual entry. All AVBOIT transmittance consumers use the same rules:

- interpolate with the recomputed local fractional virtual coordinate when
  both neighboring entries belong to filterable occupied depth;
- snap to the marked end of the preceding occupied range when entering empty
  depth;
- snap to the marked beginning of the following occupied range when leaving
  empty depth; and
- keep empty ranges invariant rather than interpolating physical slice
  coordinates through them.

Both extinction splatting and full-resolution transmittance sampling therefore
use the same compacted, boundary-aware depth warp.

Revision v61 replaces the serial builder with a 256-lane compute algorithm.
It conservatively evaluates occupancy at successively halved virtual
resolutions, chooses the finest resolution fitting the physical budget, and
uses an 8192-entry Blelloch exclusive scan to compact occupied groups. The
output explicitly marks the beginning, end, and middle of contiguous occupied
ranges. This completes the plan's prefix-compaction and range-metadata stages;
initial curve fitting from a requested minimum world-space thickness remains
pending and prevents a complete adaptive-depth conformance claim.

Revision v62 recovers the exact depth-curve equation from slide 49 rather than
inferring it from the extracted slide text. The high-resolution proposal uses
8192 slices, linearization factor 16384, and the live far plane:

`log2(depth / 16384 + 1) / log2(far / 16384 + 1) * 8192`

The camera near plane is used only to reconstruct linear view depth and is
implicit in the curve itself.

Revisions v61-v62 passed build and runtime testing (`bokt`).

Revision v63 completes divider-dependent redistribution. It maps the depth
interval covered by each occupied 8192-slice bin through every candidate
`n=8192/2^d`, `a=16384/2^d` curve, conservatively marks both resulting
boundary bins, and selects the finest remapped occupancy fitting the physical
budget. This replaces the earlier assumption that curve reparameterization
was equivalent to merging adjacent LUT indices.

Revision v63 passed build and runtime testing (`bokt`).

Revision v64 adds conservative opaque visible-depth bounds to the isolated
low-resolution prepass. A fragment is excluded only when its window depth is
behind the farthest opaque depth in all 64 full-resolution pixels represented
by its extinction cell. Occupancy and extinction share this bound.

The corrected v64 path passed build and runtime testing (`bokt`).

Revision v65 converts conservative spatial occupancy into a compact GPU cell
list and indirect compute command. Sparse clear and extinction integration now
launch work only for listed cells instead of dispatching the complete volume
grid and branching out empty invocations.

Revision v65 passed build and runtime testing (`bokt`).

Revision v66 implements indirect early-depth geometry. Compute conservatively
reduces effective-zero depth over 16-by-16 screen tiles and emits a GPU tile
list plus indirect instance count. A depth-only instanced draw expands each
entry to two triangles at the farthest safe window depth. The quads update a
private copy of opaque depth owned by the AVBOIT framebuffer, so the final
weighted-color raster receives hardware early-Z/Hi-Z rejection without
altering the viewer's shared scene depth. Fragment-stage coarse-cell rejection
remains disabled.

The NVIDIA-reserved local identifier found in the first v66 build was renamed
in v67. Revision v67 passed build and runtime testing (`bokt`).

Revision v68 uses the DRO17 full-resolution raster fallback for conservative
occupancy coverage. Full-resolution visible fragments mark their corresponding
8-by-8 AVBOIT cell, while extinction still rasterizes directly at one-eighth
resolution. This prevents thin final-raster geometry from being absent from
the occupancy/warp domain. It does not replace the still-missing optimized
transparent-entity bounds and Z-bin candidate generation.

Revision v68 passed build and runtime testing (`bokt`). Runtime comparison on
thin banana leaves fixed a case where the reverse side showed through because
the leaf had not conservatively entered occupancy. The corrected result is
closer to Exact OIT.

Revision v69 began the optimized bounds path while retaining that proven
fallback. Visible alpha spatial-group AABBs were rasterized as two-sided cube
proxies and each covered 8-by-8 cell received a conservative logarithmic depth
interval. Expanding every coarse group interval into virtual-Z coverage
regressed the banana-leaf result: false-positive depths consumed adaptive
slices and reduced precision even though exact fragment coverage remained.

Revision v70 retains proxy intervals and conservative spatial work but permits
only alpha-tested fragments to populate the Z-warp curve. This preserves sparse
XY preparation without filling empty depth. Bounds may influence Z occupancy
only after the DRO17 per-entity Z-bin candidate stage makes their intervals
sufficiently precise. Storage remains in the unified work SSBO, preserving the
OpenGL 4.3 eight-binding baseline.

Revision v70 runtime testing restored the banana-leaf result relative to v69,
though it remains slightly less opaque than Exact OIT. AVBOIT's
effective-zero threshold is `T <= 1/255`; once indirect early depth skips later
events, up to roughly 0.4 percent residual transmission is expected.

Revision v71 adds DRO17's CPU sorted-depth range and packed Z-bin LUT. Visible
alpha bounds are sorted by conservative minimum view depth, swept over 8192
uniform linear-depth bins, and encoded as 16-bit minimum/maximum entity IDs.
The proxy fragment stage loads and applies that range before recording spatial
coverage. Per-tile entity words and their range-masked iteration remain
unfinished; core OpenGL 4.3 also lacks the presentation's native wave
operations, so that portion requires a portable workgroup adaptation.

Revision v71 passed build and runtime validation (`bokt`).

Revision v72 adds the fixed 256-bit per-cell entity mask used by the cited
DRO17 configuration. Accepted proxy fragments atomically OR their sorted ID
into eight 32-bit words. Bit 255 conservatively represents all IDs beyond the
portable mask budget. Sparse spatial work now requires a surviving entity bit;
range-masked candidate iteration remains the next stage.

Revision v72 passed build and runtime validation (`bokt`) with visual parity to
v71. The same test reconfirmed stale OIT alpha ordering after switching back to
Standard. Cleanup is now owned by the neutral dispatcher: it invalidates the
visible alpha groups and traverses all region volume/bridge octrees on an
OIT-to-Standard transition, covering groups that were outside the cull result
when the switch occurred.

Revision v73 adds portable range-masked entity iteration. A 14-level sparse
table over the packed uniform Z bins returns conservative minimum/maximum IDs
for a cell's complete logarithmic depth interval with two loads. Compute masks
the eight cell words to that range and walks surviving bits with `findLSB`.
This substitutes scalar per-invocation iteration for DRO17's platform wave
intrinsics while retaining the same packed-range semantics. Bound candidates
still do not populate the adaptive Z curve.

## Unresolved runtime observations

The final runtime session reported three unresolved behaviors; they are
recorded without an asserted common cause:

- the user's dress is more transparent than expected;
- opaque content is unusually clear through transparent layers, including
  opaque underwear lace and opaque hairstyle portions viewed through a window;
- hair generally looks darker, with less apparent shine;
- part of long hair suddenly becomes more transparent during left/right camera
  pans when the hair lies over a slightly sheer dress;
- overlapping sprites with pixels expected to be opaque remain visible through
  one another instead of fully occluding the layers behind them;
- Exact OIT/AVBOIT contamination remains after returning to Standard despite
  the centralized transition invalidation.

No fix was attempted for these reports during this session. Future diagnosis
must distinguish opacity accumulation, weighted color ordering, material
classification, early-depth rejection, and mode-transition state before
changing the renderer. The hair report additionally requires separating base
color weighting from specular, emissive/glow, and exposure behavior. The
camera-dependent long-hair case must also be checked against adaptive-warp
boundary motion and overlapping-layer ordering weights without presuming
either explanation. The opaque-sprite case should be isolated with known
`alpha = 1` texels and compared directly with Exact OIT before assigning a
cause.

The user recalls that the earlier weighted-OIT implementation predating the
AVBOIT conformance work did not show these appearance problems. Although they
have existed for several recent revisions, their first offending revision is
unknown. Future work should use revision or feature-stage isolation rather
than classify them as unavoidable AVBOIT asset behavior.

Candidate visual baseline: commit
`9a2c2f3841ac6400757422be6f0d1082630e154e` (`fixed!`, 2026-07-24,
AVBOIT shader revision v55). The user thinks this may have looked best but is
not certain. That commit jointly changed packed extinction from 8-bit
`-log(1/255)` normalization to 16-bit `-log(1/65536)`, changed integrated
transmittance from `R8` to `R16F`, moved effective zero from `1/255` to
`1/65536`, and disabled unsafe coarse fragment culling. It is therefore useful
for feature isolation, but it cannot establish which one of those changes
controlled the reported appearance.

## Screenshot comparison set

Twelve screenshots in
`C:\Users\gabri\Documents\ShareX\Screenshots\2026-07` provide qualitative
Normal/AVBOIT evidence. Framing differs between some pairs, so they are not
pixel-aligned measurements.

- The window pair shows the avatar and normally obscured clothing/hair details
  more clearly in AVBOIT.
- The eye pair shows lighter, less-solid eyelashes and eyelid edges in AVBOIT.
- The sleeve pair shows transparent layer repetition and a localized stepped
  block in AVBOIT where Normal appears continuous and opaque.
- Two AVBOIT hair angles directly show transmission changing with a small
  camera pan over the sheer dress.
- The dress pair shows underwear seams and its opaque central motif through
  AVBOIT but not meaningfully through Normal.
- The sprite pair gives the strongest aggregate-opacity example: many
  underlying heart layers remain visible through AVBOIT hearts, while Normal
  sprites occlude one another strongly.

The set confirms excessive transmission across several content paths but does
not prove that all examples share one defect.

## Selected extinction configuration

AyaneStorm targets the presentation's scalar/monochrome AVBOIT configuration.
Split RGB extinction, memory-constrained chroma skew, slice-overlap color
correction, and volumetric-fog interaction are optional extensions and are
explicitly excluded from this initial renderer. They must not be claimed as
implemented through the scalar volume or custom source-over approximation.

## Accumulation and sampling equation audit

The full-resolution ordinary-transparency path matches the PDF equations:
each event contributes `color * alpha * frontTransmittance` to the color sum
and `alpha * frontTransmittance` to its normalization denominator. Resolve
multiplies their quotient by aggregate alpha
`1-exp(-sum(-log(1-alpha)))`, then adds opaque color multiplied by aggregate
transmittance. The separate accumulated-glow channel is an explicit viewer
adaptation outside the physical AVBOIT color model.

The transmittance lookup uses hardware-linear sampling of the linearly splatted
volume at `warpedSlice - 2.0`. This is the PDF's stated `-2.0` slice bias for
linear interpolation; the `-2.5` alternative applies only to bicubic sampling,
which AyaneStorm does not use. Custom Second Life blend modes remain explicitly
approximated as source-over rather than claimed as PDF-equivalent.

## Low-resolution extinction alpha derivatives

The one-eighth-resolution extinction raster changes implicit texture
derivatives by approximately eight times relative to the full-resolution
occupancy and weighted-color rasters. Without compensation, the passes can
evaluate different alpha mip levels for the same transparent surface. The
v114 experimentally scaled extinction-pass alpha derivatives by `1/8` before
sampling. Runtime testing rejected and removed that experiment: it stabilized
camera motion but worsened layered transparency and did not fix glass.

## Extinction-integral phase

The PDF integration sequence adds a slice's extinction before emitting that
slice's integral value. Its linear-filtering `-2` bias assumes this post-slice
phase. Storing the pre-slice value delays attenuation by one additional
physical slice, which is especially damaging after empty-space compaction
places otherwise distant surfaces in adjacent physical slices. AVBOIT v115
corrects the integral to post-slice storage without changing the specified
bias or resolve equation.

## Full implementation audit in v116

An end-to-end reread of the presentation and renderer confirmed that the
generated zero-transmittance depth tiles must be the sole coarse culling
mechanism. The extra fragment-stage tile decision was not part of the specified
pipeline and could discard visible geometry, so v116 disables it.

The audit also found two fragment-set/opacity mismatches. GLTF transparency is
managed outside the alpha spatial-group draw maps and was absent from normal
warp occupancy; it now receives its own occupancy traversal. Lit GLTF and
legacy specular materials could also supply a different alpha to extinction
than to weighted color. Their extinction evaluation now matches their final
surface opacity.

Revision v117 completes initial curve selection from a requested minimum
world-space slice thickness. The viewer setting defaults to one centimetre,
and the logarithmic linearization factor is solved from the requested
near-camera thickness and live far plane. All depth producers and consumers
use the same result before the existing halving/reparameterization stage.

Revision v118 selects a higher-quality spatial construction for viewer alpha
content. The extinction buffer remains one-eighth resolution, but all 64
full-resolution coverage samples are folded into each cell instead of
selecting one low-resolution raster sample. Contributions are divided by 64
and accumulated in packed 16-bit lanes to preserve them through quantization.
This remains the VBOIT extinction/integration model, but deliberately trades
prepass work and scratch memory for alpha-texture coverage beyond the
presentation's measured one-eighth-raster performance configuration.

Runtime testing found no visible difference, so v118 was removed. Revision
v119 instead corrects the ordering of sampling operations: the `-2` bias is
applied in the selected virtual logarithmic curve before the Depth Warp LUT
lookup. Subtracting it after compaction incorrectly crosses foreground events
that became physically adjacent when empty world depth was removed. Applying
the bias before the LUT preserves the presentation's empty-space invariance
and lets range-boundary snapping select the preceding integral endpoint.

### Specification-to-code status

| Presentation stage | AyaneStorm status after v116 |
| --- | --- |
| Logarithmic depth distribution | Equation, live far plane, and requested minimum world-space thickness curve fit implemented in v117. |
| Lower-resolution extinction splat | Implemented at one eighth resolution with linear two-slice splatting. |
| Packed scalar extinction and overflow | Implemented as four 8-bit lanes per `R32UI`, atomic add, and earliest overflow depth. |
| Extinction integration | Corrected to post-slice integral storage in v115; sparse work and effective-zero termination implemented. |
| Linear transmittance sampling | Implemented with the specified `-2` slice bias. |
| Adaptive Z occupancy and redistribution | 8K occupancy, conservative power-of-two reparameterization, prefix compaction, and boundary metadata implemented. |
| Empty-range filtering | Begin/end snapping and invariant empty ranges implemented consistently in splat and sample paths. |
| Conservative tiled bounds | Static and rigged alpha geometry implemented; GLTF occupancy added in v116. |
| Zero-transmittance early depth | Indirect 16-by-16 depth tiles implemented; non-spec fragment culling disabled in v116. |
| Full-resolution accumulation/resolve | PDF color, normalization weight, and total-extinction equations implemented; glow is a viewer adaptation. |
| RGB extinction | Intentionally excluded by selecting the PDF's scalar configuration. |
| DRO17 candidate optimization | CPU Z bins, cell masks, and portable range filtering exist, but the implementation remains primarily a spatial-work optimization rather than the presentation's complete wave-scalarized bounds pipeline. |
| Viewer coverage | Main post-water world transparency is covered; HUD, impostor, cube-snapshot, and pre-water paths intentionally remain outside AVBOIT capture. |
