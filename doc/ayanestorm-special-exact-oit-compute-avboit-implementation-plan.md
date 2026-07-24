# Exact OIT compute sorting and AVBOIT implementation plan

## Status

In progress. The lossless compute sorter passed its first build/runtime gate:
mode 9 proves live compute dispatch and visual parity, but the observed gain was
only about one FPS. It remains default-disabled. AVBOIT is now the active
implementation stage. Revision v31 contains the first direct-raster prototype: it
reuses complete Exact OIT capture as a fallback-safe input, bypasses sorting,
builds adaptive occupancy, warp, extinction, and transmittance in compute, and
resolves unsorted nodes approximately.

The initial v18 build exposed and safely fell back from a GLSL 4.20 loader
selection for the AVBOIT compute source. V19 requires GLSL 4.30 for every
compute stage.

The initial active resolve exposed severe hair striping from unnormalized
same-slice color sums. V20 uses front-transmittance-weighted average color with
one exact aggregate-opacity application per pixel.

The remaining localized voxel blocks in hair led to v21: integrated
transmittance is now a filterable `R32F` volume sampled trilinearly during
resolve. This targets coarse-cell and physical-slice boundaries without
increasing the fixed one-eighth spatial resolution.

The SIGGRAPH 2025 AVBOIT presentation showed that filtered sampling must be
paired with linear extinction splatting and a camera-side sampling bias to
avoid self-occlusion. V22 splits each extinction contribution across adjacent
warped slices and samples front transmittance with the documented two-slice
bias.

V23 corrects opaque-scene glow behind transparency: screen alpha carries glow,
so it must be attenuated by total transmittance just like opaque RGB rather
than copied unchanged.

V24 uses a bounded hybrid resolve for shallow ordinary transparency. Lists of
at most 16 source-over or glow-only nodes are sorted and composited exactly;
deeper and custom-blend pixels continue through AVBOIT. This targets thin hair
and clothing layers without applying exact sorting to dense smoke pixels.

V25 raises that threshold to 32 after runtime testing showed a clothing surface
became stable only when close enough for its per-pixel depth to fall below 16.
The larger private array requires explicit performance validation.

V26 adds live branch diagnostic `RenderAVBOITDebugMode=1`: green identifies
captured pixels using the shallow exact branch and magenta identifies pixels
using approximate AVBOIT.

V27 corrects shallow-branch screen alpha so it contains only ordered,
glass-attenuated glow. It no longer restores unattenuated opaque glow or maps
ordinary accumulated opacity into the glow channel.

V27 passed build and runtime testing (`bokt`). The glass/glow regression is
fixed and the tested hybrid output is visually satisfactory. Performance
optimization remains the next implementation stage.

V29 classifies pixels as empty, shallow exact, or approximate before building
the volume. Only approximate pixels contribute occupancy and extinction. A
conservative 128-bit XYZ occupancy mask per low-resolution cell allows clear
and integration to skip inactive cells while retaining neighbors required for
trilinear sampling. Runtime rendering remained identical, but the measured
gain was only about one FPS (28 to 29).

V30 records effective-zero transmittance depth, stops integration when it is
reached, and skips deeper approximate color/glow sampling during resolve.
Linked-list traversal remains unavoidable until direct AVBOIT capture can
apply this depth before rasterization.

V30 passed build/runtime testing (`bokt`) with correct rendering but no visible
FPS gain. The captured-list prototype is now closed for performance work.
Implementation must proceed to independent AVBOIT raster passes with no Exact
OIT node allocation or shallow exact resolve.

V31 implements that transition. Occupancy, extinction, and weighted-color
accumulation are three direct transparency raster traversals separated by warp,
sparse-clear, and integration compute work. Full-resolution fixed-point atomic
accumulation replaces fragment nodes. Successful direct rendering performs no
node allocation, list traversal, or shallow exact sort; Exact OIT remains only
failure fallback infrastructure. Build/runtime validation is pending.

V31 runtime performance was about 34 FPS versus 23 FPS for Exact OIT in the
same glass-heavy scene, and close smoke no longer caused severe lag. Smoke
quality was good, but hair silhouettes were blurred because final opacity came
from the filtered one-eighth-resolution volume.

V32 follows the paper's full-resolution accumulated-extinction output. A sixth
fixed-point atomic value per pixel stores total logarithmic extinction, and
resolve derives final opacity from it. Low-resolution transmittance remains
only the color/glow ordering weight. Hair sharpness and the v31 performance
gain were confirmed at runtime, but the older internal hair-patch artifact
returned once it was no longer blurred.

V33 increases the physical depth resolution from 128 to 192 slices. The
associated occupancy mask grows from four to six words per low-resolution
cell. This is intended to reduce same-slice collisions in layered hair without
changing the full-resolution opacity path. Validate hair, smoke, close
intersections, and FPS before deciding whether the 50 percent increase in
volume memory and integration work is worthwhile.

V34 responds to screenshot evidence that the residual hair glitches have
screen-space voxel footprints. It increases spatial volume resolution from
one-eighth to one-quarter scale and adjusts extinction averaging from 64 to 16
full-resolution samples per cell. Validate whether the reduced artifacts
justify four times the volume memory and integration work.

V34 was rejected at runtime: it produced severe lag without eliminating the
hair glitches. V35 restores one-eighth resolution and the matching 1/64
extinction normalization. Do not pursue increased spatial volume resolution
as the hair fix; implement the missing filterable adaptive-warp behavior and
inspect same-slice normalization instead.

V36 restores the presentation's 128-slice configuration at the user's request
to follow the official implementation. Further work must implement the
paper's algorithms rather than compensate through higher fixed resolution.
Platform adaptations required by the OpenGL 4.3 baseline must be identified
as adaptations and must preserve the paper's representation and behavior as
closely as the API permits.

## Renderer separation

AVBOIT must not be hosted by or compiled into Exact OIT. The implemented
separation restores the Exact module and shaders to pre-AVBOIT source, gives
AVBOIT independent material shader programs and capture state, and routes both
through `fsoitdispatcher`.

The user-facing preference is now the live `RenderOITMode` selector:

1. `0`, Standard;
2. `1`, Exact-OIT;
3. `2`, AVBOIT.

The neutral dispatcher translates that one choice into the two legacy internal
booleans before either renderer starts its frame. The legacy `-1` default
migrates an existing Exact OIT or AVBOIT choice on first use. The modes are
mutually exclusive: unavailable AVBOIT returns to Standard rather than silently
running Exact OIT. The same selector replaces the old Exact OIT checkbox in
AyaneStorm Preferences and Phototools.

Selection and fallback do not permit AVBOIT declarations, uniforms, resources,
shader revisions, or frame state inside Exact OIT.

### Mode-selector runtime validation

The first build with the selector compiled, but selecting AVBOIT visibly used
Standard. `AyaneStorm.log` showed NVIDIA GLSL compilation errors at every
AVBOIT shader-storage `buffer` declaration. The shared loader recognized only
Exact OIT filenames/defines when selecting GLSL 4.30, so AVBOIT material
fragment shaders received GLSL 4.20 even though AVBOIT requires shader-storage
blocks. The loader now recognizes AVBOIT independently as a GLSL 4.30 storage
shader, and the AVBOIT-only cache revision is v41.

The following runtime reached GLSL linking but failed because the viewer
injected its indexed-texture `diffuseLookup` helper into both the ordinary
material fragment stage and the appended AVBOIT capture-library stage. The
existing exclusion covered only the Exact OIT capture library. All three
AVBOIT fragment libraries are now independently excluded from duplicate helper
injection, and the AVBOIT cache revision is v42.

The next run exposed the underlying feature-cloning error. Vanilla alpha
programs retain post-link `hasLighting` state, which caused cloned AVBOIT
programs to attach another lighting fragment object and define
`diffuseLookup` twice. Capture clones now reset those link-time lighting flags.
AVBOIT emissive and PBR-glow shaders are complete terminal fragment stages, not
capture libraries; they now replace the vanilla terminal stage and retain their
required indexed-texture helper. The AVBOIT cache revision is v43.

V43 restored independent AVBOIT rendering at runtime. Noticeable hair and
clothing artifacts remain, but artifact correction is deferred while the
implementation is brought closer to the presentation specification.

V44 replaces the prototype's 32-bit extinction value per physical slice with
the presentation's four packed 8-bit extinction values per 32-bit word. Atomic
adds return the previous packed word; if the addressed byte overflows, the
fragment records the minimum overflowing physical slice in a separate
low-resolution `R32UI` image. Integration saturates at that slice, so carry
propagation into later packed bytes cannot alter the transmittance result.
The 128-slice extinction volume therefore falls from 512 to 128 bits per XY
cell, plus one 32-bit overflow depth per XY cell. The unused prototype total
transmittance image was removed. Build and runtime validation are pending.

The first v44 runtime attempt fell back to Standard because NVIDIA's GLSL
compiler parsed the new identifiers `slice` and `packed` as reserved/contextual
tokens. V45 renames them to `slice_index` and `packed_word` throughout the
packed-extinction code. V45 passed build and runtime validation; AVBOIT was
active again and retained the pre-existing visual artifacts.

V46 changes the integrated monochrome transmittance volume from the prototype's
`R32F` to the presentation's one-byte-per-slice `R8` representation. Its
effective-zero extinction is correspondingly `-log(1/255)`, matching the
8-bit extinction normalization and overflow rule. This reduces transmittance
volume storage and filtered-sampling bandwidth by 75 percent. Runtime rendering
was visibly worse, but the specified representation is retained while the
remaining depth-warp work proceeds.

V47 stores an occupied-range filterability bit alongside every fixed-point
depth-warp coordinate. All ordinary, emissive, and PBR-glow AVBOIT sampling
paths mask that metadata consistently. Coordinates interpolate only when both
neighboring virtual-depth entries belong to occupied ranges; transitions into
or out of empty space snap to the occupied endpoint. This implements the
presentation's rule that empty ranges disable filtering instead of allowing
the hardware-equivalent linear lookup to blend across them. Build and runtime
validation passed (`bokt`).

V48 replaces direct window-depth binning with the presentation's logarithmic
view-depth parametrization. Every AVBOIT material path reconstructs linear
view depth from `gl_FragCoord.z` and the live camera near/far planes, maps the
visible range logarithmically into the 8K virtual domain, and then samples the
adaptive warp. Occupancy generation, extinction splatting, ordinary color,
emissive, and PBR glow therefore use one consistent depth function. This gives
the adaptive compaction a physically meaningful minimum near-camera slice
thickness instead of inheriting the projection buffer's reciprocal depth
distribution. Build and runtime validation are pending.

V49 corrects two coupled zero-transmittance faults found during the larger
specification pass. Sparse clear now initializes integrated-transmittance tail
voxels to zero, so slices left unwritten after the specified saturation early
out cannot incorrectly transmit the background. Color and glow rasterization
also conservatively takes the farthest zero-transmittance depth across the
four cells in the actual bilinear sampling footprint rather than culling from
one nearest cell. This matches the presentation's conservative-read rule and
targets the screen-space blocks seen in hair and clothing. GPU profiling is
split into occupancy raster, warp/sparse clear, extinction raster, integration,
and weighted-color raster zones so the remaining work can be measured by
stage. Build and runtime validation of the combined v46-v49 batch is pending.

V50 implements the lightweight alpha/extinction prepass requirement across
legacy alpha, legacy material, fullbright, PBR alpha, and GLTF alpha-blend
shaders. AVBOIT passes 0 and 1 now return immediately after base-alpha texture
sampling, vertex-alpha application, and alpha masking. They no longer execute
normal/ORM/emissive sampling, reflection probes, atmosphere, fog, shadows, or
local lighting merely to write occupancy or extinction. Exact OIT and Standard
branches retain their original shader flow.

V51 moves conservative zero-transmittance rejection to the same early point in
the final weighted-color pass. Fragments known to be behind effective-zero
transmittance now return before material, reflection, atmosphere, fog, and
lighting evaluation. The capture helper retains a defensive late check for
any future material shader that does not call the early hook. Together v50 and
v51 implement the presentation's two most important draw-cost requirements:
a lightweight transmittance prepass and shading cull behind saturated
transparency. Build and runtime validation of v50-v51 is pending.

V52 replaces the prototype's six-`U32` (24-byte) per-pixel SSBO and six atomic
adds per ordinary fragment with the presentation's additive framebuffer
accumulation model. Three temporary attachments are added to the already-bound
scene framebuffer so opaque depth testing is preserved:

- `RGBA16F` stores transmittance-weighted RGB and attenuated glow;
- `R16F` stores the normalization denominator;
- `R16F` stores full-resolution accumulated extinction.

Independent additive blending is enabled only for those attachments, while
the opaque scene attachment remains color-masked. The attachments are detached
before compute resolve, which reads them as images. Accumulation storage falls
from 24 to 12 bytes per full-resolution pixel, fixed-point quantization and
per-fragment integer clamps are removed, and color-pass atomics are eliminated.
The extra color alpha channel is the viewer adaptation needed to retain its
separate glow signal.

The GPU-only diagnostic block now records virtual occupied-slice count,
physical warp utilization, and saturated volume-cell count without a CPU
readback. AVBOIT debug modes are:

1. active AVBOIT fragments;
2. virtual-depth occupancy;
3. physical-slice utilization;
4. integrated total transmittance;
5. effective-zero depth/culling.

The v52 batch completes the core monochrome AVBOIT pipeline described by the
presentation and the project plan: lightweight occupancy/extinction raster,
adaptive logarithmic depth compaction with filterable range metadata, packed
8-bit extinction with overflow-min handling, sparse 8-bit transmittance
integration, conservative effective-zero early culling, hardware-blended
weighted color/glow accumulation, and resolve over opaque color. RGB
transmittance, distortion, depth of field, motion vectors, and sparse virtual
allocation are presentation extensions/future work rather than requirements
of the selected initial monochrome configuration. Build and runtime validation
of the complete v46-v52 batch are pending.

V52 built and activated, but screenshot comparison exposed large opaque-scene
patches through hair cards rather than merely the pre-existing close-layer
approximation. The MRT attachments were configured for additive blending
before `LLDrawPoolAlpha::forwardRender`, but the pool deliberately disables
global framebuffer blending during OIT capture. That global disable reset the
per-attachment enables, so each pixel retained only a later fragment instead
of the required sum. V53 reapplies independent additive blending from the
per-draw AVBOIT configuration hook after the pool's disable, and again inside
the captured-emissive hook. Standard and Exact OIT blend state remain
untouched.

V54 fixes the independently confirmed pre-integration quantization defect.
The full-resolution-folding adaptation cannot safely quantize each
`opticalDepth / 64` contribution to eight bits: alpha values through 0.5 round
to zero before accumulation, and linear splatting can erase still higher
values. The extinction scratch volume now packs two 16-bit physical slices per
`R32UI` word. Contributions are normalized to 65535 before the atomic add and
integration reconstructs them at the same precision. Overflow-min handling is
unchanged and protects against carry into the adjacent half-word.

This raises extinction scratch storage from the official 128 bits to 256 bits
per XY cell, still half the earlier one-`U32`-per-slice prototype. V54 retained
filterable 8-bit integrated transmittance. The additional scratch precision is
required by the current full-resolution folding adaptation; a future true
low-resolution conservative prepass can return the scratch representation to
packed 8-bit without losing individual contributions.

V55 addresses runtime screenshots showing hard staircase artifacts aligned to
the 8-by-8 extinction-volume footprint. The prototype sampled
low-resolution effective-zero depth independently for each full-resolution
fragment instead of conservatively reducing it over a screen tile as required
by the reference early-depth pipeline. Dense coverage in part of a coarse cell
could consequently discard visible hair, clothing, or foliage elsewhere in
that tile.

Until matching conservative bounds exist, effective-zero depth remains
available for diagnostics but does not reject full-resolution geometry.
Integrated transmittance is upgraded from `R8` to `R16F`, and the
effective-zero threshold changes from 1/255 to 1/65536. This deliberately
forgoes premature zero-transmittance culling for correctness and removes the
8-bit threshold discontinuity that made coarse cells visibly blocky.

V55 built and passed initial runtime visual testing. The previously persistent
hair, clothing, and foliage corruption appeared fixed, and the user reported
that AVBOIT looked very good.

V56 replaces the remaining full-resolution-folding adaptation with the
reference one-eighth-resolution transparency prepass. Occupancy and extinction
geometry are now rasterized directly at the volume resolution, so extinction
is no longer divided by 64 before quantization. This permits the specified four
packed 8-bit slices per `R32UI` word, saturating compare-and-swap atomics,
1/255 effective-zero threshold, and filterable `R8` integrated
transmittance. The low-resolution passes use an isolated framebuffer rather
than incorrectly testing their coordinates against the full-resolution scene
depth attachment.

Zero-transmittance rejection now uses a fixed 16-by-16 full-resolution tile.
It reads the four corresponding extinction cells and uses their farthest
zero-depth value; if any cell has not reached zero, the tile cannot reject.
This matches the conservative reduction used to generate early-depth tiles in
the reference pipeline. The current OpenGL implementation applies the result
in the AVBOIT fragment output rather than generating indirect depth quads, so
it restores correctness-preserving work rejection but not yet the paper's full
hardware early-depth performance benefit.

The paper's shared conservative opaque-depth bounds and indirect early-depth
quad generation remain performance stages still to implement. The v56
low-resolution extinction pass conservatively includes geometry hidden by
opaque surfaces; this can waste prepass work but does not cause hidden
extinction to attenuate visible fragments in front of it.

V56 runtime testing brought back the same staircase corruption in visible
geometry. Its fragment-stage 16-by-16 zero-depth reduction was therefore not
equivalent to the paper's generated indirect early-depth quads. V57 disables
all fragment-stage zero-depth rejection again, including glow/emissive draws.
Zero depth remains diagnostic data until the actual indirect depth pipeline is
implemented; no further approximation may stand in for that stage.

The user also reported that all AVBOIT diagnostic modes stopped responding.
V58 reads `RenderAVBOITDebugMode` explicitly on every resolve, clamps it to the
implemented 0-5 range, and logs every live transition. This removes cached
control ambiguity and provides direct evidence that the selected diagnostic
value reaches the independent AVBOIT renderer.

V59 replaces the packed-extinction saturating compare-and-swap loop with the
PDF's production atomic-add and earliest-overflow-depth method. V60 introduces
separate occupied-range begin/end metadata and boundary-aware snapping.

V61 replaces the single-thread warp scan with a 256-lane compute construction.
It evaluates conservatively OR-reduced occupancy at successively halved
virtual resolutions, selects the finest level fitting 128 physical slices,
performs an 8192-entry Blelloch exclusive prefix sum, and writes compacted
coordinates in parallel. The LUT now distinguishes begin, end, and middle
entries of contiguous occupied ranges. Empty-boundary sampling snaps to the
marked endpoint; occupied ranges use the recomputed fractional coordinate.
Minimum-world-thickness fitting of the initial logarithmic curve remains
pending. Build and runtime validation of v59-v61 is pending.

V62 replaces the prototype's near-normalized logarithm with the PDF slide 49
curve:

`slice = log2(depth / a + 1) / log2(far / a + 1) * n`

At the proposed high virtual resolution, `n=8192` and `a=16384`; the near plane
is implicit. Occupancy, extinction, ordinary color, emissive, and PBR glow all
use this same equation. Reparameterizing both `n` and `a` for the selected
power-of-two divider remains part of the active v62 work.

V61-v62 passed build and runtime testing (`bokt`).

V63 begins exact resolution redistribution. The compute builder now owns the
PDF's divider-dependent curve (`n=8192/2^d`, `a=16384/2^d`) and its inverse
high-resolution curve. It can conservatively map both depth boundaries of an
occupied high-resolution bin into the reparameterized domain without
rerasterizing transparency. The v61 adjacent-index grouping has been replaced:
each candidate divider clears a shared occupancy domain, conservatively maps
the lower and upper depth boundaries of every occupied high-resolution bin,
counts the remapped coverage in parallel, and selects the finest candidate
that fits 128 physical slices. Prefix compaction and LUT fractional
coordinates operate in that selected reparameterized domain. Build and runtime
validation passed (`bokt`).

V64 supplies the isolated one-eighth-resolution occupancy and extinction
rasters with the opaque scene's visible-depth bound. Each low-resolution
fragment conservatively reduces the farthest conventional-Z depth over its
covered 8-by-8 full-resolution block and rejects only if it lies behind every
opaque sample. Occupancy, scalar extinction, emissive, and PBR glow use the
same bound. Build and runtime validation are pending.

The first v64 runtime test selected Standard because the startup guard queried
the post-deferred `screen` target for an owned depth texture. That target only
shares the attachment and consequently returns zero from `getDepth()`. The
corrected v64 path samples the texture from its owner, `deferredScreen`, while
continuing to render against the shared depth attachment on `screen`.

The corrected v64 path passed build and runtime testing (`bokt`).

V65 compacts spatially occupied extinction cells into a GPU work list. A dense
classification pass appends each conservative occupied cell once, a one-group
finalization pass writes an indirect compute command, and both sparse clear
and extinction integration consume the same list through
`glDispatchComputeIndirect`. Empty cells no longer launch clear or integration
invocations. The list is sized to the complete low-resolution grid and its
counter remains GPU-only in the diagnostics buffer. Build and runtime
validation passed (`bokt`).

V66 implements the PDF's zero-transmittance early-depth pipeline without
modifying the viewer's shared scene depth:

1. the full-resolution opaque-copy target now owns a private depth texture;
2. opaque color and the shared deferred depth are copied into that target;
3. warp construction records a conservative maximum window-depth bound for
   every physical AVBOIT slice;
4. compute reduces each 16-by-16 screen tile over its four 8-by-8 extinction
   cells, rejects any tile containing a non-saturated cell, and appends the
   remaining tile/depth pairs to a GPU list;
5. compute increments the instance count of a four-word indirect draw command;
6. an indirect six-vertex instanced draw rasterizes conservative depth-only
   tile quads into the private depth texture; and
7. full-resolution weighted transparency renders in the same framebuffer and
   is rejected through ordinary `GL_LEQUAL` early depth and hierarchical Z.

The physical-slice bound uses the far boundary of every contributing virtual
depth interval. Tile reduction takes the farthest of all four extinction
cells, so generated depth may retain false-positive work but cannot reject a
potentially visible fragment merely because another part of the tile saturated
earlier. The invalid fragment-stage substitute remains disabled. Build and
runtime validation are pending.

All transient sparse-cell, indirect-dispatch, physical-depth-bound, tile-list,
and indirect-draw data shares one SSBO at binding 3. Together with occupancy,
warp, tile occupancy, and diagnostics, v66 uses five shader-storage bindings
and remains within OpenGL 4.3's minimum guarantee of eight compute-stage SSBO
bindings.

The first v66 build fell back to Standard because NVIDIA GLSL reserves the
identifier `output`; it was used for the tile-list work-buffer offset in the
volume compute shader. V67 renames it to `work_offset` and bumps the AVBOIT
shader-cache revision. Runtime validation is pending.

V67 passed build and runtime testing (`bokt`). The indirect private-depth path
is active with conventional `GL_LEQUAL` depth, clipped partial viewport tiles,
restored framebuffer/depth state, and no mutation of shared scene depth.

V68 applies DRO17's portable full-resolution conservative-raster fallback to
AVBOIT occupancy. Occupancy geometry rasterizes against the private
full-resolution opaque depth target and atomically folds every covered
fragment into its 8-by-8 volume cell. Extinction remains a separate
one-eighth-resolution raster, as required by the selected AVBOIT
configuration. Thin geometry that contributes to the final color raster can
therefore no longer disappear merely because it missed every low-resolution
occupancy sample center. Neighbor-cell dilation remains in place for
trilinear-filter support.

This is a correctness-preserving material-raster fallback for conservative
coverage, not completion of the optimized DRO17 entity-bound/Z-bin job. The
latter remains necessary to avoid a full material draw traversal for
occupancy.

V68 passed build and runtime testing (`bokt`). It fixed a visible thin-banana-
leaf false negative where the opposite side showed through the leaf; the
result is now closer to Exact OIT. This confirms that the earlier
one-eighth-resolution occupancy raster could omit geometry that still covered
pixels in the final full-resolution pass.

V69 implements the first conservative transparent-bounds job from the DRO17
adaptation. Before alpha-tested occupancy rasterization, it visits the visible
static and rigged alpha spatial-group ranges, applies the matching water-side,
particle-visibility, dead-group, bridge, and draw-entry tests, and rasterizes
both opposing fans of every conservative group AABB at full resolution.
Fragments atomically reduce a logarithmic virtual-depth interval for each
covered 8-by-8 cell in the existing binding-3 work SSBO.

The initial v69 implementation converted the intervals to start/end events and
unioned every depth inside each coarse AABB into virtual-Z coverage. It built
after adding the missing `llviewerregion.h` definition, but runtime testing
regressed the v68 banana-leaf fix: the far leaf side became visible through
the front again. The alpha-tested fragment was still covered; the regression
came from erasing genuine empty Z ranges, which forced adaptive compaction to
spend physical slices on empty space inside coarse group bounds.

V70 preserves the proxy intervals and their conservative spatial-work
dilation, but restores fragment-derived virtual-Z occupancy as the sole input
to depth-warp fitting. Bounds therefore cannot flatten the adaptive
distribution. Per-entity bounds plus the packed DRO17 Z-bin candidate stage
must be implemented before bound intervals are precise enough to influence Z
occupancy. Proxy depth remains clamped by the same private opaque-depth source
and logarithmic near/far parameters used by material splatting. The packed
work layout continues to preserve the OpenGL 4.3 eight-SSBO-binding baseline.
V70 passed build and runtime testing. The banana-leaf regression is improved:
the far surface is no longer exposed as in v69, although the result remains
slightly less opaque than Exact OIT. The full-resolution aggregate-opacity
equation is correct; the residual difference is consistent with AVBOIT's
specified effective-zero threshold. At `T <= 1/255`, indirect early depth may
skip deeper events, intentionally retaining at most about 0.4 percent
transmission rather than continuing Exact OIT's product toward mathematical
zero.

V71 implements the first active DRO17 Z-bin candidate range. The CPU gathers
and conservatively depth-projects visible alpha bounds, sorts them by minimum
view depth, and sweeps uniformly distributed visible-depth bins. Every one of
8192 bins is uploaded as a packed pair of 16-bit minimum/maximum sorted entity
IDs. The proxy fragment stage loads its bin, decodes the range, and rejects an
entity outside that conservative candidate interval before writing spatial
coverage. ID 65534 is a conservative overflow bucket and
`0xffff|0xffff` denotes an empty bin.

This is the OpenGL 4.3 adaptation of DRO17's scalar range stage. Core GLSL 4.30
does not expose subgroup/wave operations, so wave-uniform min/max and
wave-compacted bit-word iteration cannot be assumed. The remaining optimized
stage is a per-tile entity bit mask intersected with the packed Z-bin range;
until that exists, v68's material occupancy traversal remains the correctness
oracle. V71 build, shader-link, visual, and CPU/GPU performance validation are
pending.

V71 passed build and runtime validation (`bokt`).

V72 adds the presentation's 256-bit screen-cell entity mask. After the packed
Z-bin range accepts a proxy fragment, it atomically ORs the sorted entity ID
into one of eight 32-bit words for that 8-by-8 AVBOIT cell. IDs above 254 share
bit 255 as a conservative overflow bucket, so a scene exceeding the portable
mask budget cannot silently lose spatial coverage. Compute clears the masks
and requires at least one surviving entity bit before proxy bounds enlarge
sparse spatial work. The masks and intervals remain packed into the unified
binding-3 work buffer.

The remaining DRO17 step is range-masked word iteration: derive the applicable
Z-bin range for a voxel job, mask the first/last words, and iterate the merged
candidate bits with a portable workgroup reduction. V72 passed build and
runtime validation (`bokt`) with rendering unchanged from v71.

The v72 runtime also reconfirmed a mode-transition regression: after returning
from either Exact OIT or AVBOIT to Standard, parts of the scene could retain
OIT-era alpha ordering. Both renderer modules independently invalidated only
the current cull result, and AVBOIT did so only when its preceding capture had
completed. Groups outside the current view could therefore later enter
Standard without ever being rebuilt.

Transition cleanup is now centralized in `FSOITDispatcher`. On an OIT-to-
Standard transition it marks current static and rigged alpha groups
`ALPHA_DIRTY` and also calls the existing all-region volume/bridge octree
rebuild traversal. The individual renderers now only reset their private
frame state. This prevents selection logic and cleanup from diverging again.
Build/runtime validation of Exact-to-Standard, AVBOIT-to-Standard, and objects
entering view after the switch is pending.

## Lossless Exact OIT compute sorter

- Extend the shader loader for program-local OpenGL compute shaders.
- Compact pixels with lists longer than one node through a 16-by-16 compute
  classification pass.
- Sort linked-list blocks of up to 64 nodes with one 64-lane workgroup and a
  fixed shared-memory bitonic network.
- Preserve the existing depth-descending and allocation-index-ascending total
  order.
- Apply the existing lossless opaque cutoff before block sorting.
- Ping-pong unfinished pixels through GPU-generated indirect queues while
  natural-run merge passes finish deep lists.
- Retain the existing fullscreen fragment sorter as the live and automatic
  fallback.
- Keep synchronous overflow validation and same-frame complete vanilla fallback.
- Add `RenderExactOITComputeSort`, default `false`.

Implemented in shader-cache revision v17. Diagnostic mode 9 displays green on
captured pixels when compute sorting was used and red when the fullscreen
fallback was used. State changes also log requested, available, and used flags.
Runtime testing confirmed the switching and visual parity, but not a significant
performance gain.

Promotion to default-enabled requires at least 25 percent lower Exact OIT
composite GPU time and 10 percent lower whole-frame GPU time in the deep-smoke
benchmark, with no avatar regression above 5 percent.

## Approximate AVBOIT renderer

- Implement the renderer in a separate `fsavboit` module.
- Keep `RenderAVBOIT` as internal compatibility state selected through
  `RenderOITMode`.
- Require OpenGL and GLSL 4.3; fall back to Standard after shader or resource
  failure.
- Use initially one-eighth and experimentally one-quarter viewport dimensions,
  initially 128 and experimentally 192 physical depth slices, and an 8K
  adaptive depth-occupancy/warp domain.
- Rasterize extinction, build the adaptive warp, integrate transmittance,
  render attenuated transparent color, and resolve over the opaque scene.
- Approximate all ordinary and custom color blends as source-over. Accumulate
  glow and emissive radiance additively with estimated front transmittance.
- Provide diagnostics for occupied depth, warp use, integrated transmittance,
  and zero-transmittance culling.

AVBOIT remains explicitly approximate and opt-in unless its visual differences
are accepted after smoke, splashes, foliage, hair, intersections, custom
particles, glow, and bright-background testing.

## AVBOIT PDF conformance checklist

This checklist is authoritative for claims of conformance with
`AVBOIT_SIG2025_MDROBOT-final.pdf`. Revision notes elsewhere in this document
describe history, not completion. A stage must not be called implemented from
the specification merely because a custom approximation exists.

### Implemented directly from the PDF

- [x] Convert alpha to logarithmic extinction with `-log(1-alpha)`.
- [x] Use a one-eighth-resolution spatial extinction volume.
- [x] Use 128 physical depth slices.
- [x] Splat extinction into adjacent depth slices.
- [x] Pack four 8-bit scalar-extinction slices into each `R32UI` word.
- [x] Saturate packed atomic accumulation and record the earliest overflow
  depth.
- [x] Integrate extinction along view rays into filterable `R8`
  transmittance.
- [x] Stop integration at effective-zero transmittance or recorded overflow.
- [x] Store the depth at which effective-zero transmittance is reached.
- [x] Render full-resolution transparency in arbitrary order while sampling
  estimated front transmittance.
- [x] Resolve normalized accumulated transparent color over opaque color using
  accumulated extinction.

### Viewer integration requiring validation as mathematically equivalent

- [x] Audit the full-resolution MRT color, normalization-weight, glow, and
  extinction equations line by line against the PDF equations.
- [x] Audit the two-slice sampling offset and self-occlusion avoidance against
  the PDF's stated depth bias.
- [x] Verify custom source-over mapping and glow treatment are explicitly
  outside the physical AVBOIT model rather than claiming PDF equivalence.
- [x] Verify the isolated low-resolution raster target uses the same required
  visible-depth bounds as the reference prepass.

### Missing adaptive depth-distribution stages

- [x] Parameterize the logarithmic depth curve from requested minimum slice
  thickness over the visible depth range.
- [x] Generate coverage at the proposed high virtual-slice resolution.
- [x] Implement a parallel prefix sum of virtual Z occupancy.
- [x] Compact occupied virtual slices into physical slices.
- [x] When occupied slices exceed the 128-slice budget, halve/reparameterize
  virtual resolution and conservatively rewrite occupancy until it fits.
- [x] Encode distinct range-begin, range-end, and range-middle filterability
  metadata in the depth-warp LUT.
- [x] Recalculate the fractional coordinate within occupied ranges.
- [x] Snap sampling at empty-range boundaries as described by the PDF.
- [x] Replace the serial scan and one-bit filterability approximation with
  parallel conservative resolution reduction and prefix compaction.

### Sparse spatial-work stages

- [x] Gather conservative transparent static, rigged, and VFX spatial-group
  bounding boxes for the GPU bounds job.
- [x] Conservatively proxy-rasterize and voxelize those bounds into the
  low-resolution occupancy bit buffer.
- [x] Keep one scalar occupancy path for the selected monochrome-extinction
  configuration; RGB occupancy is not required by this selection.
- [x] Drive clear and integration dispatches from occupied work rather than
  merely branching inside dense dispatches.
- [x] Share the private conservative opaque-depth copy and logarithmic
  near/far parameters with the transparency prepass.

#### DRO17 Z-binning input

The local cited source is Michal Drobot, *Improved Culling for Tiled and
Clustered Rendering*, SIGGRAPH 2017,
`doc/2017_Sig_Improved_Culling_final.pdf`. It is a Forward+/clustered-culling
presentation rather than an OIT method, but AVBOIT explicitly reuses its
one-dimensional Z-binning idea and conservative raster-culling machinery.

The F+ Z-binning algorithm is:

- CPU sorts entities by Z and creates uniform depth bins;
- every bin stores a packed pair of 16-bit minimum/maximum sorted entity IDs;
- a pixel or compute wave loads its Z-bin range;
- wave-wide minimum and maximum operations scalarize the entity-word range;
- per-lane Z-bin masks intersect the spatial visibility words;
- wave-wide bitwise OR produces a scalar merged candidate mask; and
- set bits are iterated with first-bit extraction.

For AVBOIT this is not a replacement for adaptive depth-warp prefix compaction.
It is the candidate-generation mechanism for the missing conservative
mesh/VFX bounds job. A conforming AyaneStorm design should therefore:

1. create a transient list of transparent draw bounds sorted by conservative
   view-space minimum depth;
2. build the packed `minID|maxID` Z-bin LUT over the selected visible range;
3. combine those Z ranges with conservative screen-tile entity masks;
4. scalarize candidate word ranges per compute wave where subgroup operations
   are available;
5. software-rasterize only surviving bounds into AVBOIT occupancy; and
6. retain a portable shared-memory reduction path because OpenGL 4.3 does not
   guarantee subgroup/wave intrinsics.

The final occupancy must remain conservative. Z-bin min/max ranges may include
false-positive entities but must never exclude an entity whose bounds overlap
the queried depth bin.

Current AyaneStorm status:

- [x] Conservatively depth-project and sort transparent bound entities on CPU.
- [x] Generate the uniform packed 16-bit minimum/maximum entity-ID LUT.
- [x] Load and apply that Z-bin range in the proxy fragment stage.
- [x] Generate eight per-cell entity bit words after Z-bin range acceptance.
- [ ] Intersect tile words with the Z-bin range and iterate surviving entities.
- [ ] Replace the v68 material fallback only after conservative-superset
  diagnostics show no missing alpha-tested coverage.

The same source provides a more direct implementation route for the bounds
stage than CPU screen rectangles alone:

- represent each bounded entity with conservative proxy geometry;
- rasterize the proxy through the fixed-function pipeline;
- atomically OR its entity bit into an 8-by-8 screen-tile flat bit array;
- use early depth/stencil and conservative depth bounds where available;
- emulate conservative rasterization with full-resolution raster plus 4x MSAA
  when hardware conservative rasterization is unavailable;
- compact duplicate `(tile, word)` atomic writes within a wave before the OR;
- for cluster walking, derive conservative triangle depth from fine
  derivatives and the triangle's three vertex depths.

The presentation measured its 256-bit tiled Z-bin lookup at 7.65 ms versus
9.00 ms for the tiled baseline in one PS4 scene, and the combined scalarized
tile plus Z-bin path at 4.6 ms versus 5.7 ms for its base tiled path in another.
Its raster-culling example reduced three full-screen lights from 1.44 ms to
0.10 ms using 4x MSAA plus atomic compaction. These figures establish the
intended optimization direction but are not transferable AVBOIT performance
claims.

AyaneStorm should adapt the method to transparent draw bounds, not individual
fragments. The resulting tile/entity mask limits which bounds the occupancy
compute job examines; the existing material raster remains responsible for
alpha-tested extinction until a conservative proxy can reproduce all relevant
draw bounds without false negatives.

#### AyaneStorm conservative-bounds implementation contract

The viewer already exposes the two inputs needed for the portable DRO17 path:
each visible alpha `LLSpatialGroup` supplies conservative agent-space extents
(or its bridge's spatial extents), and `LLPipeline::mCubeVB` supplies the cube
proxy used by the existing occlusion renderer. The AVBOIT bounds stage should
therefore use fixed-function proxy rasterization rather than CPU-projected
screen rectangles:

1. before material occupancy/extinction splatting, visit the same alpha and
   rigged-alpha group ranges used by `LLDrawPoolAlpha`;
2. reject groups with no applicable alpha draw entries and apply the same
   water-side, particle-visibility, dead-group, and bridge tests as the real
   traversal;
3. rasterize each surviving conservative AABB with the cube proxy at the
   full-resolution fallback sample rate (or hardware conservative raster when
   a future capability path is added);
4. atomically reduce the nearest and farthest logarithmic virtual-depth bin
   touched by the proxy into each covered 8-by-8 AVBOIT cell;
5. use those cell intervals to seed conservative spatial work, while retaining
   alpha-tested fragment coverage as the Z-warp input until per-entity Z-bin
   filtering makes the bounds sufficiently precise; and
6. retain v68's alpha-tested full-resolution occupancy raster until comparison
   diagnostics prove that proxy occupancy is a conservative superset. Only
   then may the proxy path replace the fallback.

Using CPU-projected rectangles was rejected for this stage: AABB edges that
cross the near plane require explicit homogeneous clipping, and clamping only
the eight projected corners can under-cover the screen. Proxy clipping is
instead handled by the existing graphics pipeline. Rasterizing only one proxy
surface depth is also insufficient; the interval reduction and expansion are
required so all virtual slices intersected by the bound become occupied.

The first implementation should operate on spatial-group bounds because these
are already available before material splatting and conservatively include
mesh, rigged, and VFX draws. They may introduce false positives. A later
per-draw bound list and packed 16-bit Z-bin entity-ID LUT can reduce those
false positives without changing the occupancy contract. Bounds and the
alpha-tested fallback must use the same opaque-depth copy and logarithmic
near/far parameters as extinction splatting.

### Missing zero-transmittance early-depth pipeline

- [x] Conservatively reduce `zeroTransmittanceDepth` for screen tiles of at
  least 16 by 16 pixels.
- [x] Generate a tile-quad list and indirect draw command in compute.
- [x] Draw the generated quads at their conservative zero-transmittance depth
  into the scene depth buffer.
- [x] Validate depth convention, reversed-Z state if applicable, viewport
  edges, partial tiles, and hierarchical-Z behavior.
- [x] Re-enable zero-transmittance rejection only through this depth pipeline.
- [x] Disable the invalid fragment-stage coarse-cell rejection.

### Color extinction and advanced PDF stages

- [x] Determine and document whether AyaneStorm targets the scalar-only,
  split scalar/RGB, or memory-constrained chroma-skew reference configuration.
- [x] Explicitly select scalar-only extinction; split RGB occupancy,
  splatting, integration, and RGB integral storage are not part of this
  selected reference configuration.
- [x] Explicitly exclude the optional PDF slice-overlap detection and
  color-skew self-correction.
- [x] Explicitly exclude optional fog-transmittance interaction.

AyaneStorm targets the PDF's monochrome/scalar AVBOIT configuration: one
packed 8-bit extinction value and one integrated scalar transmittance value per
voxel. This matches the presentation's primary 16.8 MB one-eighth-resolution
4K configuration and the current Second Life source-over approximation.
Split RGB extinction, memory-constrained chroma skew, overlap correction, and
volumetric-fog composition are optional extensions in the presentation. They
are deliberately excluded from the initial conforming renderer rather than
being silently represented by scalar data. They can be added as separately
named modes later without changing the scalar path's conformance claim.

### Required validation before claiming PDF conformance

- [ ] Hair, clothing, foliage, and banana leaves have no grid-aligned holes.
- [ ] Smoke and splashes retain the measured AVBOIT performance advantage.
- [ ] Equal-depth and closely intersecting transparent surfaces show only
  documented AVBOIT approximation, not implementation corruption.
- [ ] Empty-range LUT boundaries are continuous under camera movement.
- [ ] Overflow and effective-zero paths do not discard visible geometry.
- [ ] Resize, world entry, live mode switching, shader failure, and fallback
  remain stable.
- [ ] Every unchecked item above is completed or explicitly documented as an
  intentional exclusion accepted by the user.

## Validation contract

- Exact compute output must match the existing Exact OIT sorter, including equal
  depths, custom blends, glow, cutoff toggles, GLTF, rigging, overflow, resize,
  and camera transitions.
- Repeated world entry and live setting changes must not reproduce the previous
  NVIDIA point-draw crashes.
- No exact-path node limit, silent discard, reduced resolution, or approximate
  fallback is allowed.
- The user performs builds and runtime tests; `bok`, `bokt`, crashes, images,
  and timings are recorded in the Exact OIT findings.
