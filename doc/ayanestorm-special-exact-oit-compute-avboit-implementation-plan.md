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

V73 completes the portable range-masked iteration stage without allowing
coarse bounds to alter adaptive Z occupancy. The CPU expands the packed Z-bin
LUT into a 14-level sparse range-minimum/range-maximum table. A cell's
logarithmic proxy interval is converted back to the uniform linear Z-bin
domain; two table loads conservatively recover the minimum/maximum entity IDs
across the entire interval. Compute then:

1. derives the first and last relevant 32-bit entity words;
2. masks IDs outside the packed range, including the conservative bit-255
   overflow bucket; and
3. iterates surviving bits with `findLSB`, matching DRO17's scalar candidate
   loop without relying on unavailable core-GLSL-4.30 subgroup intrinsics.

The range table, entity masks, and bound intervals remain in the unified
binding-3 work SSBO. Fragment-derived alpha-tested coverage remains the sole
input to virtual-Z compaction, so v73 cannot repeat v69's coarse-depth fill.
The v68 material occupancy traversal is still retained pending a
conservative-superset diagnostic. V73 build, shader-link, visual, and
performance validation are pending.

### V74 proxy-superset diagnostics

V74 retains the v68 material traversal and compares each alpha-tested
occupancy sample with the conservative proxy interval for its 8-by-8 cell.
The diagnostics SSBO counts total and missed samples, while a per-cell miss
bit supports `RenderAVBOITDebugMode = 6`: green means covered and red means a
proxy miss. Normal rendering is unchanged.

V75 added separate 3-by-3-dilated proxy intervals, but its first runtime test
was entirely red because material comparison preceded the dilation compute
pass. V76 moves dilation immediately after bounds rasterization and before the
material occupancy traversal, preserving the raw/dilated separation without
reading uninitialized intervals.

V77 pads proxy depth intervals by one virtual bin to cover conversion-boundary
rounding. Bounds crossing the camera near plane receive a conservative
full-screen interval and entity mask before dilation, avoiding clipped proxy
geometry at close camera distances. The material fallback remains enabled.

V78 applies the viewer occlusion path's established 0.25-metre expansion to
transparent proxy half-extents. This prevents thin planar bounds such as
glass, foliage, and fences from collapsing under distant projection and also
covers small frame-to-frame animated-bound discrepancies.

V79 removes an erroneous additional half-pixel offset from proxy opaque-depth
sampling. `gl_FragCoord.xy` already denotes the pixel center; shifting it
again caused distant narrow glass and foliage proxies to sample adjacent
opaque frames or walls and collapse to the wrong depth.

V80 classifies proxy diagnostic failures: red denotes an absent proxy
footprint, yellow a material sample nearer than its interval, and magenta a
material sample farther than its interval. Green remains fully covered. This
keeps rendering unchanged while distinguishing spatial-raster failures from
depth-bound failures.

V81 makes each proxy-touched cell consume the AABB's complete conservative
CPU near/far interval rather than the depths of whichever cube surfaces happen
to rasterize that cell. It also removes the redundant fragment-level Z-bin
entity rejection, which could erase a thin proxy before its cell mask existed.
The packed Z-bin range remains active in the later per-cell candidate stage.

V82 adds a conservative depth guard of at least one metre, growing to one
percent of view distance, around CPU proxy intervals. Runtime diagnostics
showed only yellow misses on distant thin rigid geometry after v81, indicating
that spatial coverage was complete but group bounds could lag LOD/drawable
depth updates slightly.

V83 removes the v82 depth guard after runtime testing showed substantially
more yellow at zoomed-out distances. The implementation returns to v81's
better CPU AABB intervals while the remaining transient near-bound mismatch is
investigated without further heuristic expansion.

V84 applies a 16-virtual-bin guard only to the camera-facing side of GPU
per-cell proxy intervals. CPU AABB depths and Z-bin construction remain
unchanged, avoiding v82's harmful range-order interaction. The guard targets
the sole remaining yellow classification while preserving the packed Z-bin
candidate structure and fragment-derived adaptive occupancy.

V85 removes the ineffective v84 guard. Proxy near/far depth is now derived in
the bounds vertex shader from the same model-view matrix that rasterizes the
AABB, rather than from a separate CPU camera-dot calculation. This targets
the large zoomed-out yellow mismatch directly while retaining only one bin of
rounding padding.

V86 reverts v85's GPU model-view interval after it increased yellow coverage
in runtime testing. It restores v81's CPU AABB interval, the best validated
configuration, pending replacement of coarse spatial-group bounds with more
precise draw-entity bounds.

V87 replaces one coarse AABB per alpha spatial group with per-drawable
entities. A drawable is included only when one of its faces references an
`LLDrawInfo` in the active static or rigged alpha draw list. Its maintained
spatial extents provide the proxy; batched or synthetic draws without a
recoverable face link retain the group AABB as a conservative fallback.

V88 reverts v87 after aerial testing brought back red spatial misses and
increased yellow coverage. The drawable extents were not a safe common-space
replacement for render-group bounds. V88 restores the v81 group-bound
configuration, which remains the best validated conservative proxy baseline.

V89 supplements the restored group AABBs with coordinate-exact static alpha
draw geometry rasterized through the lightweight bounds shader. This proxy
uses the same vertex buffers and model matrices as the actual draw but skips
textures, alpha tests, materials, and lighting, making it a conservative
superset of static alpha coverage. Rigged geometry retains the validated group
AABB path. This is deliberately a correctness step; later bounds optimization
may replace the extra geometry only after diagnostics stay green.

V89 also explicitly restores the camera-only model-view matrix before drawing
agent-space AABBs. Earlier bounds passes could inherit the last object's model
matrix from scene rendering, explaining distance- and object-dependent yellow
intervals even when CPU bounds themselves were conservative.

V90 promotes the validated coordinate-exact static proxy to the source of
static virtual-Z occupancy. Normal rendering skips the redundant static
material occupancy traversal, while rigged geometry retains its skinned
material traversal. Debug mode 6 keeps the static traversal solely for
continued superset comparison. Extinction and weighted-color rendering are
unchanged.

V90 passed build and runtime testing (`bokt`) with no visual change. Static
material occupancy removal is therefore validated; the remaining fallback is
limited to rigged/skinned draws.

V91 adds a skinned lightweight proxy variant using the viewer's existing
object-skinning shader feature and matrix-palette uploader. Static and rigged
exact proxy geometry now populate virtual-Z occupancy. Normal mode removes
the final material occupancy traversal; debug mode 6 retains it solely to
verify that both proxy variants remain conservative supersets.

V91 passed build and runtime testing.

V92 supersamples extinction coverage at two-by-two raster samples per
one-eighth-resolution volume cell and averages optical depth by four. Fully
covered surfaces retain their previous extinction, while thin hair, foliage,
lace, and clothing receive stable fractional coverage instead of toggling
between one full sample and no sample as the camera moves.

V93 reverts v92 after runtime observation showed fence geometry becoming
visible through banana leaves. Averaged sub-cell extinction weakened
occlusion behind thin opaque coverage, violating the previously validated
banana-leaf result. V93 restores v91's extinction raster exactly.

V94 enables conservative effective-zero rejection during the weighted-color
pass. A fragment is rejected only when every 8-by-8 extinction cell in its
16-by-16 early-depth tile reached effective zero and the fragment is more than
the documented two-slice self-occlusion bias behind the farthest zero depth.
This targets opaque sprite/leaf layers visible through one another and avoids
the earlier unsafe single-cell boundary test.

V94 also resets Exact OIT's transient `sCaptureActive` flag at every frame
start, matching AVBOIT and preventing an interrupted capture from routing
later Standard-mode draws through an OIT shader.

V94 built and ran without a visible improvement or regression.

V95 removes the unrelated two-slice color-lookup bias from the effective-zero
rejection comparison. The 16-by-16 conservative maximum still requires every
covered extinction cell to be saturated, but color strictly behind that
farthest zero depth is now rejected immediately.

V96 reverts v95 after runtime testing produced severe 16-by-16 square holes in
layered hair. The two-slice margin is restored, returning to v94 behavior.
Effective-zero tightening is rejected as a solution for opaque sprite
layering.

V97 rebuilds alpha draw information on every transparency-renderer transition,
not only OIT-to-Standard. Returning to Standard repeats the rebuild once after
the next cull refresh so newly refreshed groups cannot retain OIT-era
ordering. AVBOIT equations and v96 hair/leaf rendering are unchanged.

V97 passed runtime mode-transition testing. The previously reported OIT
contamination after returning to Standard was no longer observable.

V98 makes virtual-depth indexing consistent with the LUT builder's 8192-bin
boundary convention. Material occupancy, exact proxy occupancy, proxy-bound
conversion, and LUT sampling now multiply normalized depth by 8192 and clamp
to index 8191; the previous 8191 multiplier disagreed with the compute
inverse's division by 8192 and could shift boundaries during camera movement.
The same correction is applied to legacy and PBR emissive/glow sampling so
those contributions cannot cross a different virtual boundary than color.

V98 also hardens resize/minimize lifecycle handling: zero-sized targets fall
back without allocation, and allocation status ignores unrelated stale GL
errors while still reporting errors produced by AVBOIT allocation itself.

V98 adds an on-GPU conformance scan of all 8192 depth-warp entries after LUT
construction. It checks coordinate bounds and monotonicity, legal
begin/end/middle marker combinations, and invariant coordinates across
consecutive empty entries. `RenderAVBOITDebugMode = 7` displays green when no
violation exists and red otherwise.

V99 adds an integration conformance scan over active volume cells. It verifies
that transmittance never increases with depth and that slices after recorded
effective zero remain zero within R8 precision. Debug mode 8 displays green
when overflow/saturation integration is valid and red on any violation.

V103 follows the user-confirmed regression boundary between
`a7e9a6928b3fab228706dd090d477d9b0a963ac1` and
`9ca84395c1dfb92c37cbb38bd1f83f99774c83a8`. That commit restored required
additive MRT accumulation (V53) and increased extinction precision (V54);
returning to the earlier overwrite behavior would also restore its severe
hair/card corruption. With correct accumulation active, color, legacy glow,
and PBR glow were sampling the transmittance integral two physical slices in
front of every surface. This incorrectly reused the two-slice early-depth
rejection margin and overrepresented rear layers. V103 samples the filterable
integral at the actual fractional warped surface coordinate in all three
paths. The two-slice margin remains only in conservative effective-zero
culling.

V104 addresses the V103 runtime result: changing the sampling coordinate alone
produced no visible correction. The resolve normalization explained why. An
exact integral satisfies `sum(alpha * T_front) == 1 - product(1 - alpha)`, so
the normalization factor is one. With the coarse AVBOIT integral, accumulated
weight can be smaller than aggregate alpha; the prior unrestricted
`aggregateAlpha / weight` factor then amplified transmittance-weighted color
and could cancel the intended attenuation of rear geometry. V104 caps this
factor at one. It continues to normalize when the approximate weight is above
aggregate alpha, remains mathematically neutral for an exact integral, and
prevents the approximation from generating more transparent-layer radiance
than capture integrated. The target is Vanilla/Exact-OIT visibility rather
than generalized suppression.

V105 responds to the V104 screenshot comparison. The capped normalization
reduced visibility through the dress but created dark curved regions following
underlying garment geometry, because it removed energy rather than correcting
the foreground/rear color ratio. V105 restores normalized resolve and instead
applies a live relative-depth correction to the sampled front transmittance
before color weighting. `RenderAVBOITTransmittancePower` defaults to 1.5 and
can be changed from 1.0 through 3.0 without rebuilding; 1.0 is the uncorrected
reference. Because final normalization and aggregate extinction are
unchanged, the control shifts contribution from rear layers toward foreground
transparent layers without the V104 dark-energy deficit. Color, legacy glow,
and PBR glow use the same correction.

V106 supersedes V105 after the window test confirmed that a tunable depth
power was compensating for a deterministic representation error. Extinction
capture and integration saturated opaque alpha at `-log(1/255)`, but the
full-resolution accumulated extinction used by resolve saturated the same
fragment at `-log(1/65536)`. Thus color behind opaque or nearly opaque alpha
surfaces retained up to 257 times the residual transmittance assumed by final
opacity, violating the normalization identity even with otherwise exact
sampling. V106 removes the power control and makes both paths use the same
1/65536 endpoint. The extinction scratch returns to two 16-bit slices per
R32UI word so the doubled range does not sacrifice sheer-alpha precision.
Unlike V102, this is a focused representation correction: low-resolution
extinction rasterization and the current adaptive pipeline remain unchanged.

V107 follows the V106 close-up diagnosis: the opaque black underwear band,
resolved through the opaque-background term, was correct; alpha-blended or
alpha-masked lace behind the same dress remained overrepresented. This
isolates the error to transparent-layer color weighting by the coarse 3D
integral rather than aggregate opacity. V107 uses the specification's prior
coverage stage to record the nearest surviving transparent sample per
full-resolution pixel as packed 24-bit window depth plus 8-bit alpha. For a
later transparent surface, the coarse transmittance is conservatively bounded
by the exact transmittance of that nearest foreground sample. It is never
increased, so existing multi-layer volume attenuation remains effective.
This adds one full-resolution `uint` SSBO (four bytes per pixel) and makes
material coverage run
every frame rather than only for proxy diagnostics. It targets both dress-
before-lace and window-before-avatar cases without resolve tuning.

V108 follows paired Exact-OIT normal/debug-5 window captures. The pane's debug
color decodes to the ordinary source-alpha / one-minus-source-alpha tuple, so
custom blend semantics are ruled out. V107's direct minimum bound fixed the
near dress/lace case but could not preserve a distant avatar's internal
transparent-layer ordering: window attenuation must multiply that existing
transmittance rather than replace it, otherwise normalized resolve cancels the
common factor. V108 samples the coarse integral immediately after the exact
nearest surface, computes `exactNearestT / approximateNearestT` capped at one,
and multiplies rear transmittance by that missing-attenuation ratio. If the
volume already represented the foreground correctly the ratio is one; it can
never increase transmittance. Color and both glow paths use the same rule.

The first V108 runtime attempt fell back to Vanilla because the standalone
legacy-emissive and PBR-glow shaders used the new nearest-depth correction but
did not define the shared `avboit_warped_slice` helper. The viewer log reported
GLSL C1503 at that call. V108 now includes the identical LUT decoding helper
in both standalone shaders; the correction itself is unchanged.

V110 replaces both V107's direct minimum and V108's ratio correction after
runtime V109 restored AVBOIT but regressed the dress/lace result without
fixing the window. The nearest full-resolution surface is now peeled as one
exact front layer. Its final lit/fogged RGB is packed during the color pass;
it contributes neither color nor extinction to the rear AVBOIT aggregate.
Resolve first composites all remaining transparent and opaque content, then
applies the peeled surface with ordinary source-alpha blending. This performs
the exact first front-to-back operation for either dress-over-lace or
glass-over-interior while retaining AVBOIT for all rear layers. The existing
nearest-layer SSBO grows from one to two uints per pixel (depth/alpha plus
RGB10 color); no additional geometry pass is added beyond V107.

The first V110 runtime attempt fell back because the NVIDIA GLSL 4.30 compiler
did not expose `unpackF2x11_1x10`. V111 uses explicit vendor-independent
unsigned 10-bit packing and unpacking for each RGB channel. It keeps the same
four-byte color plane and front-layer algorithm.

V112 extends the validated V111 front peel to two distinct depth layers after
the remaining screenshot difference showed a hair tip in front of the dress:
the first peel correctly captured hair, but dress and lace then returned to
the same approximate rear aggregate. The coverage pass atomically retains the
two nearest non-coincident packed depth/alpha keys. The color pass excludes
both and stores their RGB10 colors; resolve composites the rear AVBOIT result,
then the second layer, then the first. Window glass generally consumes only
the first layer, so its validated behavior is preserved. No geometry pass is
added, while the SSBO grows from two to four uints per pixel (16 bytes total).

V113 removes the V107-V112 nearest-layer SSBO, material coverage pass, exact
front peel, custom resolve, and associated memory/performance costs. Those
revisions were an unauthorized hybrid departure from AVBOIT rather than a fix
to the requested implementation. V113 restores the last spec-oriented AVBOIT
representation and equations: one-eighth-resolution extinction raster,
four packed 8-bit extinction slices per R32UI word, the 1/255 effective-zero
range, the established two-slice conservative sampling margin, normalized
weighted color resolve, adaptive depth warp, sparse integration, and proxy
coverage. The V107-V112 observations remain documented only as diagnostic
evidence that the unresolved error concerns transparent-layer weighting when
multiple foreground layers overlap; they are not retained functionality.

V98 passed build and runtime testing (`bokt`). Debug mode 7 remained entirely
green during camera movement.

V91 passed build and runtime testing (`bokt`). Normal rendering was visually
unchanged and felt slightly faster. Debug mode 6 remained green, validating
both static and skinned proxy coverage.

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

### Runtime reports after the v72 transition-cleanup batch

The following issues were reported during the final runtime session and remain
unresolved. No corrective implementation was attempted in this batch:

- some transparent assets are visibly more transparent than expected, with
  the user's dress as a reproducible example;
- opaque portions of mixed transparent/opaque content become more clearly
  visible when viewed through transparency, including opaque underwear lace
  behind transparent material and opaque hairstyle portions behind a window;
- hair generally appears darker and seems to retain less shine than expected;
- with long hair positioned over a slightly sheer dress, part of the hair can
  suddenly become more transparent while panning the camera left and right;
- overlapping sprites whose visible pixels should be opaque are not opaque:
  individual sprite layers remain visible through one another;
- contamination after switching from Exact OIT or AVBOIT back to Standard is
  still present, so the centralized visible-group plus all-region
  volume/bridge invalidation did not solve the full transition bug.

These observations must remain separate until diagnostics identify whether
the first two share a cause. In particular, they do not by themselves prove an
aggregate-extinction error, an ordering-weight error, an alpha-mode
classification error, an early-depth error, or lost specular/glow response.
The long-hair report is explicitly camera-dependent and occurs across
overlapping transparent layers; it must be tested for depth-warp boundary
movement and ordering-weight instability without assuming either cause. The
opaque-sprite report is a direct aggregate-opacity regression case and should
be compared against Exact OIT using known `alpha = 1` texels. It remains
unresolved and is not attributed to quantization, early depth, or material
classification without diagnostics. The
mode-switch validation checkbox remains unchecked.

Historical clarification: these appearance issues have been observable for
multiple recent revisions, but the user recalls that the earlier weighted-OIT
implementation, before the work to align it with the AVBOIT specification, did
not exhibit them. They should therefore be treated as unresolved behavioral
changes introduced somewhere during the conformance evolution, not as
intrinsic properties of the affected assets. The first offending revision is
not yet known and must be established by revision bisection or equivalent
feature isolation before attributing them to depth warping, packed extinction,
early depth, direct rasterization, or another specification stage.

The user identifies commit
`9a2c2f3841ac6400757422be6f0d1082630e154e` as a possible, but uncertain,
last-known visually good baseline. Read-only inspection identifies it as
AVBOIT shader revision v55, commit subject `fixed!`, dated 2026-07-24
01:41:22 +0200. Its relevant renderer changes were:

- replace packed two-byte extinction lanes' effective-zero normalization
  `-log(1/255)` with 16-bit normalization `-log(1/65536)`;
- replace the integrated `R8` transmittance volume with filterable `R16F`;
- raise the effective-zero threshold accordingly from `T <= 1/255` to
  `T <= 1/65536`; and
- disable the then-active coarse fragment-stage zero-depth rejection because
  it could discard visible geometry at 8-by-8 cell boundaries.

This commit is a high-value comparison point for the excessive transparency,
camera-dependent long-hair, and dark/low-shine reports. It is not yet a proven
good/bad boundary: the user is unsure, and v55 changed precision and culling
together. Future isolation should compare those changes independently while
remembering that the later 8-bit representation was deliberately restored to
match the selected PDF configuration.

### Screenshot evidence supplied 2026-07

The user supplied twelve Normal/AVBOIT screenshots under
`C:\Users\gabri\Documents\ShareX\Screenshots\2026-07`. They were inspected
through the equivalent WSL path. The paired framing and camera position are not
identical in every image, so these establish qualitative regressions rather
than pixel-aligned numerical differences:

- `avatar_through_window_avboit.png` versus
  `avatar_through_window_normal.png`: the AVBOIT view makes avatar, clothing,
  underwear detail, and hair structure more distinct through the window;
- `eye_avboit.png` versus `eye_normal.png`: AVBOIT eyelashes and the eyelid
  edge appear lighter and less solid than the darker Normal result;
- `dress_sleeve_avboit.png` versus `dress_sleeve_normal.png`: AVBOIT exposes
  overlapping sleeve layers and a localized block/step pattern, while Normal
  reads as a substantially continuous opaque sleeve;
- `hair_avboit_camera_angle_1.png` versus
  `hair_avboit_camera_angle_2.png`: a small camera-angle change substantially
  changes transmission through the long hair over the sheer dress, directly
  confirming camera-dependent instability;
- `dress_avboit.png` versus `dress_normal.png`: underwear seams and the central
  opaque motif are plainly visible through the AVBOIT dress but are effectively
  hidden in Normal; and
- `sprites_avboit.jpg` versus `sprites_normal.jpg`: overlapping AVBOIT heart
  sprites visibly transmit many underlying sprite layers, whereas Normal
  sprites occlude one another much more strongly.

Together these images strengthen the excessive-transmission report across
multiple content types. They do not establish one common implementation cause:
window transmission, material layers, alpha-textured hair/eyelashes, and
sprites may exercise different shader and blend paths.

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
- [x] Intersect cell words with a conservative interval Z-bin range and
  iterate surviving entity bits through the portable scalar path.
- [x] Compare material occupancy against proxy intervals and visualize misses.
- [x] Replace the v68 material fallback after conservative-superset
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
- [x] Empty-range LUT boundaries are continuous under camera movement.
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

### AVBOIT v114: consistent alpha derivatives across raster resolutions

The implementation audit found that AVBOIT evaluated texture alpha with
different implicit derivatives in its material passes. Occupancy and weighted
color rasterize at full resolution, while extinction rasterizes at one eighth
resolution in each axis. The extinction pass therefore selected alpha texture
mips from derivatives approximately eight times larger even though that
extinction is applied to full-resolution fragments.

Shader revision v114 reconstructs full-resolution alpha derivatives during
the low-resolution extinction raster. The correction covers indexed legacy
alpha, non-indexed alpha, fullbright alpha, legacy materials, PBR, and GLTF.
It added no storage or geometry pass and did not change the AVBOIT accumulation
or resolve equations. Runtime testing rejected this change: zoom and panning
became stable, but lace beneath the dress and the through-glass error remained,
and hair over the sheer dress became more transparent. The v114 source change
was removed without requiring a separate rollback build.

### AVBOIT v115: extinction-integral phase correction

The subsequent equation audit found that the integration compute shader stored
transmittance before adding each slice's extinction. The PDF integration order
is “add extinction” while iterating slices, and its linear-sampling `-2` bias is
defined against that post-slice integral. The old pre-slice storage introduced
one additional physical slice of delay.

This matters especially after adaptive compaction: transparent surfaces
separated by empty world space occupy adjacent physical slices. The extra
delay therefore omitted much of the foreground dress or glass extinction when
weighting transparent lace, hair, or avatar surfaces behind it. Opaque
background composition uses separately accumulated total extinction, which
explains why solid opaque portions could remain correct.

Shader revision v115 now adds the current packed extinction before writing the
current integral sample. Overflow saturation, effective-zero detection, sparse
tail clearing, linear splatting, the specified `-2` sampling bias, and all
resolve equations remain unchanged.

### AVBOIT v116: complete specification-to-code audit

The full AVBOIT portion of the 136-page SIGGRAPH 2025 presentation and every
AyaneStorm AVBOIT C++/GLSL path were reread end-to-end. The audit confirmed the
v115 integration-phase correction and found three additional implementation
drifts:

1. the indirect conservative depth-tile pipeline was active, but a second
   coarse fragment-stage zero-transmittance test was also active. The PDF uses
   the generated depth geometry so normal hardware depth rejection remains
   conservative. The duplicate fragment decision can expose 16-by-16 blocks
   and is now disabled;
2. GLTF alpha geometry is traversed by `GLTFSceneManager`, outside the alpha
   spatial-group maps used by the conservative occupancy raster. It now gets
   an explicit material-tested occupancy traversal before warp construction;
3. two material routes supplied different opacity to extinction and weighted
   color. Lit GLTF applies vertex alpha twice in its final output, while its
   AVBOIT prepass applied it once. Legacy materials can raise final alpha to
   specular glare after their former early prepass return. Both extinction
   paths now use the same final opacity as their weighted-color paths.

These corrections remain within AVBOIT. They add no peel, per-pixel fragment
storage, opacity exponent, or custom resolve. GLTF occupancy adds only the
previously missing GLTF occupancy traversal; legacy material extinction now
performs the lighting required to reproduce the viewer's final glare-derived
alpha.

The audit also corrected an inaccurate checklist claim: the presentation's
initial depth curve is not yet derived from a requested minimum world-space
slice thickness. AyaneStorm uses the recovered fixed proposal
`n=8192, a=16384`, then implements the specified conservative halving and
packing. The presentation does not provide a transferable world-unit value
for that requested thickness, so this remaining parameter-selection stage is
documented rather than guessed.

### AVBOIT v117: world-space minimum slice curve fit

Runtime testing showed no material visual improvement from v115-v116. A
known-good/first-bad comparison also established that the earlier appearance
partly resulted from missing additive accumulation on some draws; restoring
the required blending exposed rather than created the remaining ordering
approximation.

The audit then returned to the one declared conformance gap. Using the slide's
example `a=16384` directly in a metre-based viewer makes the 8192-entry curve
nearly uniform over a region-scale far plane. Close dress, lace, hair, and
glass layers can therefore occupy the same virtual bin before adaptive
packing. Revision v117 implements the specified requested minimum slice
thickness stage. `RenderAVBOITMinimumSliceThickness` defaults to `0.01` metre,
and the CPU solves

`a * log(1 + far / a) / 8192 = requested_thickness`

for the frame's initial linearization factor. The same fitted factor is used
by occupancy, conservative bounds, splatting, sampling, and inverse-depth
construction. The existing power-of-two reduction continues to divide both
slice count and linearization factor when occupied virtual slices do not fit
the 128 physical slices.

### AVBOIT v118: full-coverage low-resolution extinction

Runtime testing confirmed that v117 improved hair depth separation, including
hair in front of a sheer dress, but did not materially fix lace behind the
dress, the view through glass, or locally under-opaque dress regions.
Hair also remained darker than Vanilla and Exact OIT. AVBOIT's final color
pass still executes the same material shader; this observation therefore
remains attributed to weighting/order until v118 coverage is tested, not to
material evaluation being disabled.

History identified the spatial transition precisely. V55 folded all
full-resolution material samples into the one-eighth-resolution extinction
volume and was reported visually strong. V56 changed to rasterizing one
fragment sample per 8-by-8 extinction cell to match the presentation's
performance configuration; the contemporary runtime report immediately
recorded renewed corruption. One sample cannot represent high-frequency alpha
textures such as patterned sheer clothing, hair cards, or glass details.

V118 retains the AVBOIT volume, adaptive warp, integration, weighting, and
resolve, but restores full-resolution material coverage during extinction
splatting. Each fragment contributes one sixty-fourth of its optical depth to
the corresponding low-resolution cell. Packed scratch lanes use 16 bits so
these divided contributions do not quantize to zero before accumulation. This
is a viewer-content quality adaptation of the spatial prepass, not a peel or
per-pixel layer store. Its cost is increased extinction raster work and twice
the packed-extinction scratch storage.

Runtime testing found no visible change. V118 was therefore rejected and
removed without a standalone rollback build.

### AVBOIT v119: apply sampling bias before adaptive depth warp

The subsequent equation audit found that the two-slice self-occlusion bias was
subtracted from the compacted physical coordinate. That violates AVBOIT's
empty-space invariance: two events separated by a large empty world interval
become adjacent after compaction, so subtracting two physical slices can jump
past the entire foreground event. Rear lace or avatar surfaces then sample
transmittance from before the dress or glass.

V119 stores the selected virtual-resolution shift during warp construction.
For color and glow sampling it maps the surface to that selected logarithmic
curve, subtracts the specified two virtual slices there, reconstructs biased
world depth, and only then samples the Depth Warp LUT. Extinction splatting is
unchanged. This keeps the bias local to the surface in uncompressed depth while
allowing the LUT to snap across invariant empty ranges as specified.

### AVBOIT v120: extinction quantization floor and saturating accumulation

Revisions v103-v119 searched for the excessive-transmission cause in the depth
warp, sampling bias, integration phase, and curve fit. None of those changes
altered the reported symptoms. A fresh read of the splatting code found the
defect in the scratch representation instead, upstream of every equation those
revisions corrected.

`avboit_add_extinction` quantized each slice contribution into an 8-bit lane
normalized to `-log(1/255)`. One quantum is therefore about `0.0217` optical
depth, roughly `alpha = 0.0215`. Linear two-slice splatting divides a
fragment's optical depth between two lanes *before* that rounding, and the
function returned early on a zero quantum. The effective floor was consequently
about `alpha = 0.043`: every sheer surface below it contributed no extinction to
the ordering volume at all.

This single mechanism explains the reported cluster, because all of the affected
content is sheer rather than geometrically unusual:

- sheer dress fabric rounded to zero, so lace behind it sampled front
  transmittance near one;
- window glass rounded to zero, so avatar and hair behind it were unattenuated;
- anti-aliased sprite edges rounded to zero, so overlapping sprites did not
  occlude one another; and
- hair cards survived quantization, but resolve's `aggregate_alpha / weight`
  normalization then redistributed energy toward the layers that did register.

The asymmetry is the diagnostic key. Full-resolution `avboitAccumulatedExtinction`
is a float normalized to `-log(1/65536)` and recorded every one of those
fragments correctly, so aggregate opacity was right while the ordering weight
was wrong. This matches the V107 observation that the error concerns
transparent-layer weighting, and explains why depth-warp work could not fix it.

A second defect sat in the same function. The overflow test performed
`imageAtomicAdd` and then inspected the returned previous value. Under
contention every thread sees a stale previous, so a lane could wrap past 255
with no thread observing the crossing; the lane then retained a small value and
its carry corrupted the adjacent slice's lane. Because the outcome depends on
rasterization order, this is a direct candidate for the camera-dependent hair
transmission flip.

V120 changes three things:

1. `RenderAVBOITWideExtinction` (default enabled) selects two 16-bit lanes per
   `R32UI` word instead of the presentation's four 8-bit lanes. The quantum
   falls three orders of magnitude below any visible alpha step. The scratch
   volume is always allocated for the wide layout, so the setting switches at
   runtime without reallocation and the narrow layout simply leaves the upper
   half of its slices unused.
2. The volume's effective-zero endpoint follows the selected layout, so in wide
   mode it matches the `-log(1/65536)` endpoint already used by resolve. An
   opaque fragment no longer saturates the ordering volume 257 times earlier
   than it saturates final opacity. This restores the V106 correction that V113
   reverted for storage-conformance reasons.
3. Accumulation uses a saturating `imageAtomicCompSwap` loop. A lane clamps at
   its maximum instead of wrapping, carry cannot reach the adjacent slice, and
   overflow depth is recorded by whichever thread actually reaches saturation.

Widening the lanes is a deliberate, user-accepted deviation from the
presentation's storage configuration, recorded here in the same manner as the
existing glow and source-over adaptations. The presentation's 8-bit lanes are
paired with a genuine one-sample-per-cell low-resolution prepass; they are not
quantizing the same distribution of viewer alpha values. Scratch extinction
rises from 128 to 256 bits per XY cell. The adaptive warp, integration,
weighting, resolve equations, and sampling bias are unchanged.

Build and runtime validation are pending. The A/B procedure is to compare
`RenderAVBOITWideExtinction` enabled against disabled on the dress/lace,
through-window, and overlapping-sprite cases; disabling it reproduces the
previous behavior without a rebuild.

### AVBOIT v120 screenshot analysis: two independent defects

The user supplied one window pair and one close-up dress pair, the dress pair at
an identical camera position. They separate the reported cluster into two
mechanisms rather than one.

The dress pair is the decisive evidence. AVBOIT shows the thong outline and
waistband stitching through the sheer skirt as smooth curves that follow the
garment geometry exactly. There is no staircase and no cell-aligned edge. That
observation excludes the entire family of causes pursued from v55 through v96:
coarse-cell rejection, 16-by-16 tile rejection, and warp-boundary faults all
produce grid-aligned artifacts, which those revisions did in fact report. The
dress error is smooth, uniform under-attenuation of one foreground layer.

An important correction to the initial v120 reasoning: the 8-bit quantization
floor does **not** explain the dress. One 8-bit quantum is about `0.0217`
optical depth, so at a mid-alpha fabric of roughly `alpha = 0.5`
(`optical depth 0.693`) the floor is only a few percent of relative error even
after two-slice halving. The floor explains genuinely low-alpha content only.

The dress mechanism is the `-2` virtual-slice self-occlusion bias. The bias
exists so a surface does not occlude itself, and the presentation's value
assumes a slice is thick relative to the spacing between distinct surfaces.
Adaptive compaction deliberately removes empty world depth, so the front and
back fabric layers of one garment can occupy adjacent physical slices. Backing
off two slices from the rear layer then samples transmittance from a point
before the *front* layer, so the front fabric does not attenuate the rear
fabric or the underwear behind it. V119 moved the bias to the correct domain,
which preserves empty-space invariance, but did not reduce its magnitude, so
the garment case was unaffected.

The window pair confirms the quantization floor independently and also excludes
the atomic race for that case. A single glass pane produces almost no
per-lane fragment contention, yet everything behind the glass is visibly
under-attenuated, including the guitar, microphone stands, and desk as well as
the avatar. Only a representation floor can explain uniform under-attenuation
without contention.

The resulting attribution is:

| Reported case | Mechanism | v120 control |
| --- | --- | --- |
| Window glass, sprite edges, eyelash fringes | 8-bit quantization floor drops low-alpha extinction | `RenderAVBOITWideExtinction` |
| Sheer dress over underwear | `-2` slice bias skips the adjacent foreground layer after compaction | `RenderAVBOITSamplingBias` |
| Camera-dependent hair transmission | stale-read atomic add wraps a contended lane | saturating compare-and-swap |

`RenderAVBOITSamplingBias` therefore exposes the bias magnitude live, defaulting
to `1.0` instead of the presentation's `2.0`, clamped to `[0, 8]`. Ordinary
color, legacy emissive, and PBR glow all read the same value. This is a
documented parameter deviation, not a structural change: the bias still applies
in the selected virtual logarithmic curve before the Depth Warp LUT exactly as
v119 established.

Validation should vary the two controls independently, because they target
different images. `RenderAVBOITWideExtinction` is expected to change the window
pair and the sprites while leaving the dress close-up largely unchanged;
`RenderAVBOITSamplingBias` is expected to change the dress close-up. If lowering
the bias to `0.0` still does not let the front fabric attenuate the underwear,
the remaining suspect is the transmittance volume's spatial resolution rather
than the sampling coordinate, because both fabric layers then share one
one-eighth-resolution cell and one physical slice.

### AVBOIT v120 correction: alpha-mode path split, not quantization or bias

User clarification retired both mechanisms proposed earlier in v120. Recorded
here explicitly so neither is pursued again on the strength of those notes.

The dress close-up shows a thong carrying alpha-**masked** lace, worn under a
slightly sheer alpha-**blended** dress. Two consequences follow immediately:

- alpha-masked fragments survive a binary keep/discard test, so their alpha is
  approximately `1.0`. They saturate any extinction lane, and the 8-bit
  quantization floor cannot explain them; and
- the lace and the adjacent solid thong fabric are at essentially the same depth
  behind the same dress, so the `-2` slice bias applies to both identically and
  cannot explain a difference between them.

The differentiator is alpha mode. `class3/deferred/materialF.glsl` gates every
AVBOIT hook on `DIFFUSE_ALPHA_MODE == DIFFUSE_ALPHA_MODE_BLEND` (lines 318, 329,
and 465). Alpha-masked geometry is deferred opaque geometry: it writes the
G-buffer and never calls `avboit_store`. That is correct and matches vanilla.

The defect is therefore in resolve, where the two paths are recombined:

    transparent + opaque.rgb * total_transmittance
    transparent = weighted_color * (aggregate_alpha / weight)

`total_transmittance` and `aggregate_alpha` derive from full-resolution float
`accumulated_extinction` and are effectively exact. `weight` is accumulated from
`front_transmittance` sampled out of the coarse one-eighth-resolution volume and
is approximate. AVBOIT's normalization identity
`sum(alpha * T_front) == 1 - product(1 - alpha)` therefore compares an exact
right-hand side against an approximate left-hand side.

The consequences match both supplied image pairs:

- alpha-masked lace is attenuated by exact `total_transmittance`, while the
  solid alpha-blended thong fabric beside it is weighted by the coarse volume.
  Two surfaces at one depth behind one dress receive two different
  transmittance representations, so the lace separates from the fabric. Vanilla
  and Exact OIT composite both through one ordered path and show them
  uniformly; and
- in the window pair only the avatar differs, and it is darker under AVBOIT.
  The glass is therefore not at fault. The avatar's opaque body loses energy to
  exact `total_transmittance` that includes the avatar's own hair and dress,
  while those same layers return their color through the coarse, under-weighted
  transparent term. The two do not cancel, so the net result is darker.

When the coarse volume under-estimates occlusion, `weight` exceeds the exact
aggregate, `aggregate_alpha / weight` falls below one, and transparent layers
are darkened while the opaque geometry behind them receives no compensating
boost. This is one root cause presenting as several symptoms, which is why
single-stage conformance corrections from v103 onward could not resolve it.

Note that V104 already capped this ratio at one and was rejected for producing
dark regions; V105's tunable depth power was rejected as compensation. Both
treated the symptom in resolve. The correction must instead make the two paths
agree, by deriving the opaque attenuation from the same representation that
weights the transparent layers, or by raising the ordering weight's accuracy to
match the aggregate. Choosing between those is the next decision and must not be
approximated by another resolve-side scale factor.

### AVBOIT v120 measurement: Highlight Transparent pool identification

The alpha-mode question was settled by observation rather than inference, using
`View > Highlight Transparent` (`Ctrl+Alt+T`) together with
`- include Rigged Transparent` (`Ctrl+Alt+Shift+T`), which is required because
`renderAlphaHighlight` skips rigged batches unless `sShowDebugAlphaRigged` is
set. The highlight must be read in Standard mode, since enabling it changes
batch building.

Result on the reported dress/thong content:

| Surface | Highlight | Path |
| --- | --- | --- |
| Dress | orange (rigged blended) | AVBOIT transparent |
| Thong lace | orange (rigged blended) | AVBOIT transparent |
| Thong solid fabric | not highlighted | opaque G-buffer |

This retires the earlier assumption, recorded above, that the lace was
alpha-masked. It is alpha-blended. The solid thong fabric is the opaque surface,
which is the reverse of what was assumed when the alpha-mode path split was
first proposed. The `materialF.glsl` `DIFFUSE_ALPHA_MODE_BLEND` gating and
`LLDrawPoolMaterials` mask-pass routing are therefore correct and not implicated;
that analysis stands as an accurate description of masked geometry but does not
apply to this content.

The measured configuration is a thin blended layer (lace) immediately behind
another blended layer (dress), with an opaque surface (fabric) beside the lace
at nearly the same depth. That produces the observed symptom directly:

- the opaque fabric is composited as `opaque.rgb * total_transmittance`, using
  exact full-resolution accumulated extinction, so the dress attenuates it
  correctly; while
- the lace is weighted by `front_transmittance` sampled from the coarse volume
  after subtracting the `-2` virtual-slice bias. The dress and lace are
  separated by millimetres of world space, so adaptive compaction places them in
  adjacent physical slices and the bias samples transmittance from before the
  dress. The lace is therefore weighted as though the dress were absent.

Two surfaces at one depth behind one garment consequently receive two different
attenuations, which is why the lace separates from the fabric under AVBOIT while
Vanilla and Exact OIT, which composite both through one ordered path, show them
uniformly. The window pair is the same mechanism: the avatar's opaque body is
attenuated exactly while its own blended hair and dress return color through the
coarse, bias-displaced path, so the composite reads darker.

`RenderAVBOITSamplingBias` (default `1.0`, clamped `[0, 8]`, `2.0` reproduces the
presentation value) is therefore the control relevant to this case. Note that
this is the same control introduced earlier in v120 on reasoning that was then
disproved; the measurement above re-establishes it on a verified basis. The
earlier v119 change remains necessary and complementary: the bias must be applied
in the selected virtual curve before the Depth Warp LUT, and v120 only changes
its magnitude.

Expected validation outcome: lowering the bias should make the lace agree with
the solid fabric beside it and darken the through-window avatar toward Vanilla.
If `0.0` still leaves them disagreeing, the residual cause is that the dress and
lace share one one-eighth-resolution cell and one physical slice, which no
sampling coordinate can separate; the next step would then be the volume's
spatial or depth resolution rather than the sampling equation.

### AVBOIT v120 final change set

The pool measurement above localized the defect to the boundary between two
attenuation paths, and gave the error a consistent sign: opaque geometry renders
too dark while blended geometry renders too visible. That is misallocation
between the two resolve terms rather than lost energy, which points at the
precision of the approximate ordering weight relative to the exact aggregate.

The complete v120 change set is therefore:

1. `RenderAVBOITWideExtinction` (default enabled) stores scratch extinction as
   two 16-bit lanes per `R32UI` word instead of four 8-bit lanes. The volume is
   always allocated for the wide layout, so the setting switches at runtime
   without reallocation; the narrow layout leaves the upper words unused. Wide
   mode also uses the `-log(1/65536)` effective-zero endpoint, matching the
   endpoint already used by full-resolution accumulated extinction.
2. The integrated transmittance volume returns to `R16F` unconditionally. This
   volume is the entire ordering weight for blended geometry, so its precision
   bounds how closely the approximate weight can track the exact aggregate.
   `R8` quantizes the sheer range that viewer clothing occupies to 1/255 steps
   and cannot represent a 1/65536 endpoint at all. Because the GLSL image format
   qualifier is compile-time, this is not made switchable; `R16F` is the format
   in both extinction layouts. Storage rises from one to two bytes per voxel.
3. Packed accumulation uses a saturating `imageAtomicCompSwap` loop. A lane
   clamps at its maximum instead of wrapping, carry cannot corrupt the adjacent
   slice, and overflow depth is recorded by the thread that reaches saturation.
   This is independently correct regardless of the layout selected.
4. `RenderAVBOITSamplingBias` (default `1.0`, clamped `[0, 8]`) exposes the
   self-occlusion bias magnitude. `2.0` reproduces the presentation value. The
   measured content places the lace millimetres behind the dress, so after
   adaptive compaction they occupy adjacent physical slices and a two-slice
   backoff samples transmittance from before the dress.

Item 2 is supported by the strongest historical signal in this document.
Revision v55 used `R16F` for this volume and the contemporaneous runtime report
records that the previously persistent hair, clothing, and foliage corruption
appeared fixed and that AVBOIT looked very good. Revision v56 returned the
volume to `R8` for storage conformance, and the corruption was reported again
immediately afterward. That transition was previously attributed to v56's
change of extinction raster resolution; the format change shipped in the same
revision and was not separated.

Deviations from the presentation's storage configuration are items 1 and 2, both
recorded as deliberate viewer-content adaptations in the same manner as the
existing glow and source-over adaptations. No resolve-side scale factor,
opacity exponent, front peel, or per-pixel layer store is introduced, so the
V104, V105, and V107-V112 category of correction is not repeated. The adaptive
warp, integration order, splatting, normalization, and resolve equations are
unchanged.

Build and runtime validation are pending. Validation order:

1. with defaults, compare the dress/thong close-up and the through-window avatar
   against Standard. The lace should agree with the solid fabric beside it, and
   the avatar should no longer read darker;
2. set `RenderAVBOITSamplingBias` to `2.0` to isolate how much of any
   improvement came from the bias magnitude rather than precision; and
3. disable `RenderAVBOITWideExtinction` to isolate the extinction lane width.
   Note that this no longer reverts the transmittance volume format, so it is
   not a complete return to pre-v120 behavior.

### AVBOIT v120 runtime result: front-transmittance family excluded

V120 produced no visible change, tested both with `RenderAVBOITWideExtinction`
disabled and enabled. This is a decisive negative result and it excludes a whole
family of candidate causes, including several pursued across earlier revisions.

The reason is visible directly in the resolve equation:

    weighted_color = sum(color * alpha * T_front)
    weight         = sum(alpha * T_front)
    transparent    = weighted_color * (aggregate_alpha / weight)

`T_front` appears in both the numerator and the denominator of a weighted
average, so it largely cancels. For a single transparent layer it cancels
exactly, leaving `color * aggregate_alpha` regardless of the sampled
transmittance. Its influence survives only as the *relative* weighting between
multiple layers overlapping one pixel, which for a two-layer dress-over-lace
configuration is a small residual.

Every v120 change acted only on `T_front`: extinction lane width, the
1/65536 endpoint, the `R16F` transmittance volume, and the sampling-bias
magnitude. Their collective invisibility is therefore the predicted outcome
rather than a surprise, and it retroactively explains why v103, v114, v115,
v117, v118, and v119 also produced no material visual change. All of them
adjusted the precision, phase, or coordinate of the same cancelling quantity.

Conclusions now supported by test rather than inference:

- front-transmittance precision, quantization, endpoint, and sampling bias are
  excluded as causes of the reported appearance;
- resolve's arithmetic is correct for the measured content. Two blended layers
  at `alpha = 0.5` give `accumulated_extinction = 1.386`,
  `aggregate_alpha = 0.75`, matching `1-(1-0.5)^2`, and the opaque background
  correctly receives `exp(-1.386) = 0.25`; and
- the remaining candidate is what AVBOIT fundamentally computes. It forms an
  `alpha * T_front`-weighted average color over all transparent layers and then
  applies aggregate opacity. With `T_front` cancelling, layers are weighted
  essentially by alpha alone, largely independent of their order. Source-over
  gives the foreground layer substantially more influence because rear layers
  are multiplied by the front layer's transmittance.

This last point is a property of the method, not obviously an implementation
defect, and it must be measured before any algorithmic response is chosen.

Revision v121 therefore adds two resolve diagnostics rather than a fix:

- `RenderAVBOITDebugMode = 9` displays `aggregate_alpha` as greyscale; and
- `RenderAVBOITDebugMode = 10` displays the normalized average transparent
  color `weighted_color / weight` without aggregate opacity or the opaque
  background.

Interpretation for the dress-over-lace case: if mode 9 reads approximately
`0.75` over the garment while mode 10 is visibly lace-tinted rather than
dress-dominated, the ordering-independent color average is confirmed as the
cause and is localized precisely. If mode 9 instead reads low, extinction
accumulation is faulty and is correctable within the specification.

Also noted: `RenderAVBOITTransmittancePower` was removed from the code by v106
but persists in user settings files, where it appears meaningful and is inert.
An audit confirmed that every `RenderAVBOIT*` key currently in `settings.xml`
is wired to code and that no code reads an undefined key.

### AVBOIT v122: specular glare must not become extinction

The v121 diagnostics located the defect immediately and unambiguously.

Observed on the reported content:

- `RenderAVBOITDebugMode = 9` (aggregate alpha) rendered the sheer dress
  **entirely white**, meaning `aggregate_alpha` is approximately `1.0`. The
  garment is being integrated as fully opaque when its texture alpha is roughly
  `0.5`. Removing the dress showed the thong lace also white, while the solid
  thong fabric was uncolored, correctly indicating it is opaque geometry outside
  the transparent path; and
- `RenderAVBOITDebugMode = 10` (normalized average color) was nearly identical
  to normal rendering, confirming the color average is not at fault.

The error is therefore entirely in `accumulated_extinction`, and the cause is
`class3/deferred/materialF.glsl`:

    float al = max(diffcol.a, glare) * vertex_color.a;

`glare` is accumulated additively over every light and the environment
reflection, then clamped to `1.0`. It is a single-blend presentation trick that
raises output alpha so specular highlights read as solid. It is not a physical
opacity.

Under Standard rendering this is benign, because one source-over blend simply
makes highlights look solid at those pixels. Under AVBOIT the same value becomes
`-log(1 - alpha)`, so a glare of `1.0` produces the saturation clamp
`-log(1/65536) = 11.09` and `aggregate_alpha = 1 - exp(-11.09)` is
indistinguishable from one. A brightly lit shiny garment therefore integrates as
an opaque occluder over broad areas.

This single cause accounts for every outstanding report:

- lace and other rear transparent layers appear through the dress because the
  dress's own aggregate saturates and the normalization redistributes color;
- masked/opaque geometry behind transparency, notably hair and the avatar body
  seen through a window, is multiplied by `exp(-11.09)` and goes dark;
- hair loses apparent shine because it is masked geometry attenuated by that
  saturated aggregate rather than by the garment's true opacity; and
- the effect is camera dependent, because glare is view dependent. Panning
  changes specular response, so integrated opacity changes with view direction.
  This explains the long-standing camera-angle hair instability without
  invoking depth-warp boundary movement.

Revision v116 previously identified that legacy materials can raise final alpha
to specular glare, and responded by making extinction match that glare-inflated
alpha. That propagated the defect into the volume instead of removing it. V122
reverses that decision: AVBOIT extinction, occupancy, and weighting all use
`diffcol.a * vertex_color.a`, the material's own opacity. Glare's visual
contribution is preserved by scaling emitted color by `al / avboit_alpha`, so
highlights retain the luminance they contribute under Standard rendering while
no longer occluding as if opaque.

Only the legacy material path is affected. GLTF alpha is base-color alpha with
no glare term, and the fullbright and occupancy paths already used base alpha.

This is a correctness fix inside the specification rather than a deviation: the
PDF's `-log(1-alpha)` is defined on surface opacity, and glare is not opacity.
The v120 controls remain available but are now understood to act on a quantity
that largely cancels in the resolve average; they are not the fix and their
defaults are unchanged.

### AVBOIT v123: binary-alpha lace and diagnostic gating correction

User clarification refined the v121 reading. The lace texture is effectively
binary: its threads are opaque and its gaps are fully transparent, even though
the material is rendered through the alpha-blend path rather than the alpha-mask
path. In `RenderAVBOITDebugMode = 9` the white regions are therefore the
alpha-one threads and are correct. The surrounding regions, reported as dark or
missing rather than showing background, were a defect in the diagnostic itself.

Modes 9 and 10 were gated on `weight > 0.0`. At `alpha = 0`,
`weight = alpha * frontTransmittance` is zero, so those pixels fell through to
normal composited rendering. The resulting image mixed diagnostic output with
composited output and could not be read reliably. V123 makes mode 9
unconditional over AVBOIT-covered pixels and gives mode 10 an explicit blue
marker where the normalization denominator is zero, so absent weight is
distinguishable from black color.

The clarification strengthens rather than weakens the v122 diagnosis, and
sharpens its mechanism for this asset. With binary alpha,
`al = max(diffcol.a, glare) * vertex_color.a` is precisely what makes the gaps
visible: at a gap `diffcol.a` is zero, but `glare` need not be, so `al` becomes
`glare` and a fragment that should be fully transparent acquires opacity. Lace
structure consequently appears where nothing should be drawn, and because glare
is view dependent, the visible pattern changes with camera angle.

The v122 correction resolves this case directly: `avboit_alpha` is
`diffcol.a * vertex_color.a`, which is exactly zero at gap texels regardless of
glare, so those fragments contribute no extinction, no weight, and no color.

V123 also bounds the glare color compensation. The ratio `al / avboit_alpha`
diverges as base alpha approaches zero, so a sheer texel with strong glare could
receive an unbounded color multiplier and appear as a firefly. The scale is
clamped to `4.0`, which preserves ordinary highlight luminance while preventing
that failure. Fragments with zero base alpha are unscaled and contribute
nothing, as required.

### AVBOIT v124-v128: measurement-driven isolation of the ordering defect

This batch replaced theorising with instrumentation. Several intermediate
conclusions recorded earlier in v120-v123 were wrong and are corrected here.
The wrong turns are retained deliberately, because each one closes off a line of
investigation that looks attractive from the source alone.

#### Correction: no over-accumulation ever existed

Modes 9 and 11 initially appeared to show the sheer dress integrating as fully
opaque, and this was written up as over-accumulation. That conclusion assumed the
garment was about 50 percent transparent. The user confirmed it is about
**5 percent** transparent, which is the realistic value for this content.

At `alpha = 0.95`, `-log(1 - 0.95) = 3.0` and `aggregate_alpha = 0.95`. Every
reading was therefore correct:

- mode 9 white (`aggregate_alpha` 0.95) — correct;
- mode 11 red (`optical depth` >= 3.0) — correct;
- mode 12 blue (`weight` 0.95) — correct; and
- glass reading blue in mode 11 is simply a different, also correct, alpha.

The mode-11 and mode-12 band thresholds had been chosen around the assumed
50 percent garment, so they were mis-scaled for the actual content and were then
read as evidence. **Diagnostic bands must be derived from measured asset values,
not from assumed ones.** No extinction, alpha, or accumulation defect exists.

#### Isolation test: the defect requires a blended foreground layer

Setting the dress to fully opaque made the lace disappear correctly. That single
test, which required no build, established that the defect exists only when the
foreground layer travels the alpha-blend path, and excluded asset error,
occupancy coverage, and the opaque composition term in one step.

#### Modes 13 and 14

`RenderAVBOITDebugMode = 13` compares the volume's deepest slice against
`exp(-accumulated_extinction)`. Both measure survival through everything at a
pixel and must agree. Result:

- **green** on dress and hair: the volume matches the exact accumulation. The
  foreground garment's occlusion *is* recorded correctly. This excluded the
  hypothesis that the volume under-records dense geometry; and
- **blue** on glass panes and lace: the volume transmits more than exact. This
  is the one-eighth-resolution extinction raster taking one sample per 8x8
  screen block and missing thin geometry, exactly as v118 anticipated. It is a
  separate, genuine defect and is **not** the reported bug.

Mode 13 cannot see the value each fragment actually samples, only the volume
total. `RenderAVBOITDebugMode = 14` closes that gap: the capture pass substitutes
`frontTransmittance` for color under this mode, so resolve can display the
average front transmittance used for ordering. This required exposing
`avboitDebugMode` to the capture shaders.

#### The defect: self-occlusion from a misaligned volume lookup

Mode 14 showed the dress reading **0.5 to 0.85** where, as the frontmost
transparent layer, it must read approximately **1.0**. The garment was
attenuating itself.

Integration stores, for each physical slice, the transmittance *after* that
slice's extinction has been added. This post-slice phase is correct and was
established in v115; the PDF's linear-filtering bias is defined against it. A
surface lying in physical slice `i` must therefore sample slice `i-1`. Sampling
slice `i` includes the surface's own extinction.

The `-2` bias exists to prevent precisely this, but v119 moved it into the
virtual domain. That move is correct for empty-range snapping across compacted
depth and must be kept. However, after adaptive compaction 8192 virtual slices
map into 128 physical slices, so one virtual slice is typically a small fraction
of one physical slice and the subtraction rounds away entirely in physical space.

Consequently every surface sampled its own slice. A `0.95`-alpha garment reported
front transmittance well below one, its weight fell from about `0.95` toward
`0.6`, and it lost dominance in the normalized average
`weighted_color / weight` against the near-opaque lace behind it. The lace
therefore remained visible through a garment that is 95 percent opaque.

This also explains why the v120 batch was invisible: those changes altered the
precision, endpoint, and quantization of `frontTransmittance`, but the sampling
**coordinate** was wrong. It is further consistent with mode 13 reading green,
since the volume contents were always correct and only the lookup into them was
misaligned.

#### V128 correction

Revision v128 enforces the backoff in physical slice space, after the Depth Warp
LUT:

    float physical_slice = max(sample_slice - 1.0, 0.0);

The virtual-domain bias is retained for empty-range snapping; this adds only the
minimum offset that linear filtering of a post-slice integral requires. The same
correction is applied identically in the ordinary color, legacy emissive, and
PBR glow sampling paths so no contribution can cross a different boundary than
the others.

This is a correctness fix within the specification. It introduces no resolve
scale factor, opacity exponent, front peel, or per-pixel layer store, so it does
not repeat the rejected V104, V105, or V107-V112 approaches.

Validation pending. Expected: mode 14 reads red (>0.85) on the frontmost
transparent surface; mode 0 hides the lace behind the dress and stops darkening
hair and the through-window avatar. If mode 14 still reads yellow on a frontmost
surface, the LUT coordinate is off by more than one slice and `sample_slice`
itself must be instrumented rather than the offset guessed again.

#### Remaining known defect, not yet addressed

Mode 13's blue on glass and lace is real: one low-resolution sample per 8x8 block
under-occludes thin geometry. Neither obvious remedy is correct. A per-cell
maximum breaks dense geometry that currently reads green, and averaging all 64
samples is v118, which produced no visible change, while v92/v93 showed
coverage-fraction averaging weakens thin occlusion. Coverage-count normalization
is defensible but needs a second volume plus a normalization pass. Note also
that a volume cell represents an 8x8-pixel column of space, so averaging by
coverage is physically correct for a participating medium; the mismatch is that
the color pass queries the volume per pixel. `RenderAVBOITExtinctionCoverage` is
declared in `settings.xml` for this work but **is not implemented**.

### AVBOIT v128 runtime result: rejected, and the structural limit it exposed

V128 was built and tested. It is **rejected and reverted**.

Mode 14 confirmed the intended effect: frontmost surfaces changed from yellow
(0.5-0.85) to red (>0.85), so a one-physical-slice backoff did stop the dress and
the glass from attenuating themselves. That part of the v128 reasoning was
correct.

It did not fix any reported symptom. The lace remained visible through the dress
and hair through glass was unchanged. The decisive observation is that under
mode 14 **the lace reads yellow while the dress reads red at the same pixels**.
A layer correctly behind a `0.95`-alpha garment must report front transmittance
near `0.05`, which is the green band below `0.15`. Yellow means the lace samples
transmittance as though only a thin layer were in front of it.

The cause is a resolution limit, not an offset error. Warp construction assigns
`coordinate = warpScan[group] + fract(reduced_coordinate)`, so an entire
occupancy group maps to one physical slice with a fractional position inside it.
The dress and the lace are millimetres apart and therefore land in the **same
physical slice**, differing only by that fraction. Subtracting one whole physical
slice moves the lace to before the dress rather than between the two, which
reintroduces exactly the empty-space violation v119 was written to remove. No
sampling offset can separate two surfaces that share a slice: a backoff smaller
than one slice does not clear the foreground layer's own extinction, and a
backoff of one slice or more clears the foreground layer entirely.

This is the same limit from the opposite direction as the v96 result, where
tightening effective-zero rejection produced 16-by-16 holes in layered hair.

Established consequences:

- the volume contents are correct (mode 13 green on the dress);
- the extinction, alpha, and aggregate opacity are correct (5 percent garment
  yields optical depth 3.0 as measured);
- the resolve arithmetic is correct;
- the frontmost-surface self-occlusion was real but is not what causes the
  reported symptoms; and
- with 128 physical slices spanning the visible range, layered avatar clothing
  within a few centimetres cannot be ordered by the depth warp, however the
  lookup is biased.

Reverted in all three sampling paths (ordinary color, legacy emissive, PBR glow)
so they again match the specification's virtual-domain bias exactly.

The remaining options are therefore not sampling corrections. They are either an
increase in usable depth resolution for near-camera layered geometry, which v34
already showed is expensive at one-quarter spatial scale though depth resolution
was not tested independently, or an explicit documented deviation from the
normalized-average model for the near-layer case. The latter category includes
V104, V105, and V107-V112, all previously rejected. Neither should be attempted
without first deciding which is acceptable, because both change the method rather
than fix a defect in it.

### Decisive observation: the defect is separation-dependent

The user established that the lace is visible through the dress **only when the
two are very close**. When the lace moves away from the dress, for example
between the legs, it becomes correctly hidden.

Correct source-over compositing is separation-independent: a `0.95`-alpha garment
attenuates whatever is behind it by `0.05` at any distance. The failure therefore
occurs specifically at small separation, and this identifies the mechanism
precisely.

When two surfaces fall in the same occupancy group, warp construction assigns
both `coordinate = warpScan[group] + fract(...)`, so both sample the
transmittance volume at effectively the same Z texel. That texel holds the
transmittance *after* the group's extinction, which is the sum of **both**
surfaces. Consequently:

- the dress and the lace read the same front transmittance;
- neither is treated as being in front of the other;
- both receive equally small weight; and
- in the normalized average `weighted_color / weight`, equal weights make the
  lace contribute about half the resulting color instead of five percent.

Once the lace separates enough to enter a different occupancy group it receives a
deeper physical slice, reads post-dress transmittance while the dress reads
pre-dress transmittance, ordering is restored, and the lace is correctly hidden.
This matches the reported behavior exactly.

The relevant consequence is that this is **not** simply a depth-resolution
shortfall. The symmetry is the defect: two surfaces sharing a slice each include
the other's extinction in their own front transmittance. Increasing slice count
would reduce how often surfaces collide, but the same-slice case would remain
mutually self-occluding whenever it occurs, and near-camera layered clothing will
always produce collisions at some separation.

This also explains, consistently with every earlier measurement, why the volume
reads correct (mode 13 green), the extinction and aggregate opacity read correct,
the resolve arithmetic is correct, and yet ordering fails: all of those describe
totals, while the defect is in how a shared slice distributes those totals
between the surfaces inside it.

### AVBOIT v129: remove each surface's own contribution from its transmittance read

V129 was built and runtime tested. The user reports a clear improvement: rear
transparent layers now fade out much more quickly behind a near-opaque garment,
and the reference close-up shows the dress reading as a solid garment with its
seams and buttons intact rather than revealing the underwear behind it.

This confirms the shared-slice mechanism described in the preceding section.

#### Implementation

Pass 1 splats a surface's optical depth linearly across the physical slice pair
straddling its unbiased `slice_coordinate`, giving the lower slice the fraction
`1 - fract(slice_coordinate)`. The integral that pass 2 samples therefore already
contains the surface's own extinction, so every surface attenuated itself.

Pass 2 now removes exactly that amount before using the value as an ordering
weight:

    own_optical_depth = -log(max(1 - alpha, 1/65536))
    own_share = own_optical_depth * (1 - fract(slice_coordinate)) * read_overlap
    frontTransmittance = clamp(frontTransmittance * exp(own_share), 0, 1)

`slice_coordinate` is the unbiased coordinate computed before the pass branch,
which is precisely what pass 1 splatted with, so the amount removed matches the
amount deposited. `read_overlap` is zero once the sampled texel is no longer the
texel this surface splatted into, so raising `RenderAVBOITSamplingBias` past one
physical slice cannot cause over-brightening.

The correction is a per-read adjustment and never modifies the volume. All 64
full-resolution pass-2 fragments covering one one-eighth-resolution cell
therefore independently obtain the same corrected value; there is no cumulative
over-subtraction even though only one fragment per cell splatted in pass 1.

#### Relationship to the specification

This is the discrete form of the specified `-2` sampling bias. The bias exists to
prevent a surface from occluding itself by stepping back in depth. V128
established that adaptive compaction can collapse that step to a fraction of a
physical slice, at which point stepping back either fails to clear the surface or
clears the entire foreground layer. Subtracting the known self-contribution
achieves the bias's intended result exactly and is independent of the separation
between surfaces, so it cannot reproduce the v128 failure. The virtual-domain
bias from v119 is retained unchanged for empty-range snapping.

No resolve scale factor, opacity exponent, front peel, or per-pixel layer store is
introduced, so this does not repeat the rejected V104, V105, or V107-V112
approaches.

#### Known remaining limit

The correction removes only the self term, which is the part a fragment can know.
When two surfaces share a slice, linear splatting also places the rear surface's
extinction into the texel the front surface reads, and the summed integral does
not record which part came from behind. Predicted contribution of a rear
near-opaque layer to the normalized average color, for a `0.95`-alpha garment:

| configuration | rear layer share |
| --- | --- |
| before v129 | about 50 percent |
| after v129 | about 24 percent |
| correct source-over | 5 percent |

The residual improves as the two surfaces separate within a slice (about 24, 15,
and 10 percent at sub-slice fractions 0.6, 0.8, and 0.95), which is consistent
with the reported separation-dependent behavior and with the observed faster
fade-out. Fully reaching 5 percent for co-sliced surfaces would require
distinguishing front from rear contributions within a slice, which the current
summed-integral representation cannot express.

Still to be validated: the through-window avatar and hair darkening reports, the
overlapping-sprite case, sparse geometry where pass 1 undersamples (mode 13 blue
on glass and lace edges, where `own_share` may not correspond to a contribution
actually present in the cell), and whether any transparent surface appears
over-brightened by the `exp()` correction.

### Warp saturation is the root cause of co-sliced layers

`RenderAVBOITDebugMode = 3` displays physical-slice utilization as
`(u, u*u, 1-u)`. The user observes bright yellow, sometimes paler yellow, varying
with camera zoom and changing often. Bright yellow is `u` near `1.0`; paler
yellow with a slight blue tint is roughly `0.8` to `0.9`. The depth warp is
therefore **saturated or near-saturated**, and its chosen `group_shift`
fluctuates as the camera moves.

This is the upstream cause of the co-sliced-layer defect and it reframes the
earlier analysis.

`avboitDiagnostic[1]` is the occupied-group count after compaction has already
selected the finest `group_shift` that fits 128 physical slices. Saturation means
compaction is spending every available slice and still had to coarsen the
grouping. The warp spans `camera->getNear()` to `camera->getFar()`, which in
Second Life is the entire draw distance, and
`RenderAVBOITMinimumSliceThickness = 0.01` is solved against that full range.
Adaptive compaction is intended to remove empty depth, but `avboitOccupancy`
marks a virtual slice occupied if any geometry lies there, so an ordinary
furnished interior with walls, furniture, and a window leaves little empty range
to compact.

Consequences:

- a five-millimetre garment gap cannot receive its own physical slice, because
  the 128 slices are distributed across the whole occupied depth of the scene;
- adding depth resolution, whether as more slices or as sub-slice bins, is
  therefore poor value. The additional resolution is distributed by the same
  saturated warp and mostly lands on room geometry rather than on the layers
  that actually collide. Sub-slice binning is also mathematically equivalent to
  raising the slice count (four bins equals 512 slices), so it is not a
  different class of fix, only a constant factor, at four to eight times the
  volume memory; and
- the reported fluctuation with camera zoom means `group_shift` changes between
  frames. Each change re-quantizes which surfaces share a slice. This is a
  direct explanation for the long-standing camera-dependent hair transmission
  instability, which had previously been attributed variously to depth-warp
  boundary movement, atomic contention, and specular glare without diagnosis.

Sub-slice binning was verified arithmetically to produce the exact source-over
result for the measured case (dress `1.0000`, lace `0.0498`), and it is
order-independent so atomic accumulation is valid. It is nevertheless not
recommended while the warp remains saturated, for the reasons above.

The productive direction is to reduce the depth range the warp must cover, or to
make occupancy sparser so compaction has empty ranges to remove, so that the
existing 128 slices are concentrated where transparent layers actually overlap.
Both target the measured cause rather than adding resolution to compensate for
it.

### Depth-warp resolution limit versus the actual content requirement

The user specified the real requirement: Second Life clothing layers are
frequently **less than 5 mm** apart and the renderer must separate layers
**0.1 mm** apart. Deviation from the presentation is explicitly authorised to
achieve this. Earlier analysis in this document assumed a 5 mm garment gap; that
figure was an assumption, not measured content, and is withdrawn.

The requirement is not reachable by any global depth warp, for a budget reason
rather than a tuning reason:

- virtual bin size is `span / n`, so with `n = 8192` over a 0.1-128 m range the
  finest achievable spacing at 2 m depth is about **1.75 mm**, whatever shape
  parameter is chosen. `RenderAVBOITMinimumSliceThickness` redistributes
  precision between near and far but cannot lower this floor, which is why
  values below 0.001 stop improving near layers while continuing to degrade
  distant transparency (52 mm to 31 mm at 60 m between thickness 0.001 and
  0.002, versus 2.03 mm to 1.81 mm at 2 m);
- a pure logarithmic curve reaching 0.1 mm at 2 m over that range would require
  about **143,000 virtual bins**, roughly seventeen times the current domain;
- doubling to 16,384 bins would give 0.87 mm at 2 m and simultaneously improve
  60 m from 52 mm to 26 mm, at a trivial 64 KB of SSBO. It is nevertheless
  blocked as written because `shared uint avboitWarpScan[8192]` already occupies
  exactly 32 KB, which is the OpenGL 4.3 guaranteed maximum for compute shared
  memory. Doubling requires restructuring the single-workgroup Blelloch scan into
  a multi-pass scan over global memory; and
- fitting the curve to the measured occupied span does not rescue it either. A
  scene containing an avatar at 2 m and a window at 30 m has an occupied span of
  about 28 m, giving a 3.4 mm floor. Compaction removes empty bins only *after*
  binning, so detail already merged at bin-generation time cannot be recovered.

There are additionally two independent merge stages, and sharpening the first
worsens the second. Virtual binning is thickness-dependent; physical compaction
then merges `2^group_shift` virtual bins per physical slice once occupancy
exceeds 128 slices. Finer thickness concentrates more occupied bins into less
depth, which raises occupancy, which raises `group_shift`. Mode 3 showed the warp
already saturated with `group_shift` fluctuating as the camera moves, which also
explains the long-standing camera-dependent hair instability: each change of
`group_shift` re-quantizes which surfaces share a slice.

The consequence is structural. Separating surfaces 0.1 mm apart at arbitrary view
depth requires **per-pixel** ordering, because no fixed global slicing of the
view frustum can represent that separation across a scene of realistic depth
extent. AVBOIT's representation is a summed integral over shared slices and
cannot express it.

Note explicitly: the per-pixel approach was implemented previously as the
V107-V112 front-layer peel and was removed as an unauthorised hybrid departure.
With deviation now authorised it is admissible again, but it is the same
technique, and it should not be reintroduced without acknowledging that history
and the reasons V113 reverted it.

### AVBOIT v130: restore the presentation's linearization-factor proportionality

The user supplied the VBOIT depth-distribution and voxelization slides. Comparing
them against the implementation confirmed most stages and found one real
parameterization error.

Confirmed correct against the slides:

- the curve form `log2(x/a + 1) / log2(b/a + 1) * n`, matching
  `avboit_curve_coordinate` exactly;
- `n = 8192/2^d` and `a = 16384/2^d`, both scaled by the same divider, matching
  the v62/v63 implementation and the slide instruction to "adjust slice count
  together with linearization factor";
- the implicit near plane;
- extinction rather than transmittance, accumulated in log space with
  `InterlockedAdd`, normalized by the extinction bit mask
  (`saturate(-log(1-alpha) / -log(1/BIT_MASK))`), matching
  `avboit_add_extinction`; and
- UINT storage rescaled to a normalized range and scaled by bit depth.

The error concerns the linearization factor. The slide's reference configuration
is `a = 16384` against a far plane `b = 32000`, so `a/b = 0.512`: at the finest
divider the depth distribution is close to uniform, which is exactly what
"keeps spatial slice resolution close to camera ~constant" describes. Precision
is varied by the divider, which scales `n` and `a` together, not by reshaping the
curve at fixed `n`.

`fittedLinearization` instead solved `a` from a requested near-plane thickness,
decoupling it from the far plane. A requested `0.001` over a 128 m far plane
produced `a = 1.95`, an `a/b` ratio of `0.015` against the reference `0.512` --
a curve roughly thirty times more logarithmic than the presentation's finest
setting. Near-plane precision improved only by starving distance, which explains
the measured behaviour of the setting: at 0.001 the spacing at 2 m improved to
2.03 mm while 120 m degraded to 62.5 mm.

V130 restores the proportionality `a = 0.512 * far`, and retains
`RenderAVBOITMinimumSliceThickness` as a bounded scale on that reference rather
than an unbounded solve. The scale is clamped to one order of magnitude either
side, so the curve cannot leave the specified family. Behaviour on a 128 m far
plane:

| setting | a (m) | at 2 m | at 30 m | at 120 m |
| --- | --- | --- | --- | --- |
| 0.010 | 75.65 | 9.39 mm | 12.77 mm | 23.65 mm |
| 0.002 | 15.13 | 4.70 mm | 12.38 mm | 37.07 mm |
| 0.001 | 7.57 | 3.37 mm | 13.24 mm | 44.95 mm |
| 0.0005 and below | 6.55 | 3.16 mm | 13.49 mm | 46.69 mm |

The reference ratio corresponds to a setting of about `0.0087`. Note that the
previous default of `0.010` produced `a = 98`, already close to the reference
`a = 65.5`, so the default was never far from the specification; only the fine
settings distorted the curve.

An important consequence for the stated requirement: the presentation's own
configuration cannot separate 0.1 mm layers at metre-scale depths. Its reference
figures are 2.17 units of slice thickness against a 32000-unit far plane, which
is proportionally fine for that scene scale but corresponds to about 8.7 mm on a
128 m viewer draw distance. Following the specification more closely therefore
improves distant transparency and bounds the near/far trade-off, but does not by
itself reach the 0.1 mm target.

The unused `RenderAVBOITNearReference` setting introduced during this
investigation was removed rather than left declaring behaviour no longer present,
as was the never-implemented `RenderAVBOITExtinctionCoverage`.

V130 was built and runtime tested. The user reports no significant visual
difference. This is the predicted result: at the previous default of `0.010` the
old solve produced `a = 98` against the new reference `a = 75.65`, so the curve
barely moved at the setting actually in use. V130's value is conformance and the
bounding of fine settings, which previously drove `a` to `1.95` and starved
distant transparency; it is not a fix for the reported layering bug.

The v129 self-contribution correction remains the only change in this batch that
measurably improved the reported symptoms.

### AVBOIT v131: high-resolution virtual depth domain

Two constraints cited earlier in this investigation were not requirements. The
8192-slice virtual domain was copied from the presentation slide and hardcoded;
the OpenGL 4.3 baseline is a portability floor chosen elsewhere in the project.
The test machine reports **OpenGL 4.6 with 12 GB of video memory**, so neither
limited what was achievable. Depth resolution scales linearly with the domain
size and the domain costs only two `U32` buffers, so a larger domain is cheap:

| virtual slices | at 2 m | at 30 m | domain cost |
| --- | --- | --- | --- |
| 8192 (reference) | 8.93 mm | 12.63 mm | 64 KB |
| 65536 | 1.12 mm | 1.58 mm | 512 KB |
| 1048576 | 0.07 mm | 0.10 mm | 8 MB |

V131 implements the domain as a runtime selection rather than a constant:

- `AVBOIT_VIRTUAL_SLICES` and `AVBOIT_ZBIN_LEVELS` are injected into every AVBOIT
  program as compile-time defines from one C++ source of truth, so the buffers,
  the depth curve, the Z-bin range table, and the prefix scan cannot disagree;
- occupancy, warp, and Z-bin table allocation are sized from the selected domain,
  and the RMQ level count is derived as `log2(domain) + 1` rather than fixed at
  14; and
- the depth-warp prefix scan array moves from `shared uint avboitWarpScan[8192]`,
  which sat exactly on the 32 KB `GL_MAX_COMPUTE_SHARED_MEMORY_SIZE` guarantee,
  into a shader-storage buffer at SSBO binding 2. Every barrier guarding it is
  now `avboit_scan_barrier()`, which pairs `memoryBarrierBuffer()` with
  `barrier()` as buffer visibility requires.

`RenderAVBOITHighDepthResolution` selects the high-resolution domain and defaults
enabled. `FSAVBOIT::selectVirtualDomain()` falls back to the reference 8192
domain when the driver reports less than OpenGL 4.6 or less than 4 GB of video
memory, and logs the decision with the observed version and memory. The
fallback is a genuine configuration change rather than a failure path: the
reference domain remains fully functional.

The domain is set to **65536** rather than 1048576 deliberately. The existing
prefix scan runs in a single 256-thread workgroup, so its cost is
`O(domain/256)` serial iterations per thread; 65536 makes that 256 iterations
instead of 32, which is absorbable, while 1048576 would make it 4096 iterations
on one workgroup while the rest of the device idles. Reaching 0.07 mm therefore
requires replacing that scan with a multi-pass per-workgroup scan first. That
work is deferred until 65536 has been validated, because the open question is
not the plumbing but whether finer virtual bins survive `group_shift`
compaction, which mode 3 showed to be saturated.

Build and runtime validation pending. The measurement that matters is whether
close clothing layers separate at roughly 1.1 mm; if they do, the remaining
factor of sixteen is available through the multi-pass scan. Mode 3 should also be
rechecked: if utilization remains saturated and `group_shift` still fluctuates
with camera movement, compaction is absorbing the added resolution and the scan
rewrite would not help.

### AVBOIT v132: divider search cap must scale with the domain

V131 raised the virtual domain but left the divider search bounded by a literal
`candidate <= 6u`, which had been sized for the 8192-slice domain. At 65536 the
search therefore pinned at 6, leaving 1024 occupied groups for 128 physical
slices, so compaction could never fit and the warp was frozen at its maximum
coarseness. The reported symptoms matched exactly: no visual change from the
larger domain, and mode 3 showing bright yellow that was *stable* under camera
movement, i.e. stuck rather than converged.

The cap is now computed in C++ as `avboitMaxDivider()` (8192 -> 6, 65536 -> 9)
and injected as `AVBOIT_MAX_DIVIDER_VALUE` alongside the other domain defines,
consumed as `const uint AVBOIT_MAX_DIVIDER`. The computation was deliberately
kept out of GLSL: deriving it there would require `log2()` and `findMSB()` inside
a `const uint` initialiser, which is not reliably constant-foldable and would
fail the compile silently, dropping the viewer to the Standard path.

After v132 the mode 3 flicker resumed, confirming the search runs again. The
lace remained visible through the dress.

### AVBOIT v133: measure `group_shift` instead of inferring it

Every revision from v130 onward reasoned about what the selected divider *should*
be without reading it. V133 records it directly: pass 1 writes the final
`group_shift` to `avboitDiagnostic[8]` (the diagnostics SSBO grew from 8 to 16
slots, as 0-7 were occupied), and debug mode 15 displays it as one distinct
colour per value - black 0, grey 1, blue 2, cyan 3, green 4, yellow 5, orange 6,
red 7, magenta 8, white 9+ - so a divider pinned at `AVBOIT_MAX_DIVIDER` is
immediately distinguishable from one that converged.

### AVBOIT v133 runtime result: the virtual domain was never the bottleneck

Measured at roughly 2 m behind the avatar, mode 15 reads **red flickering to
magenta - divider 7 to 8 - and reads the same at both the 8192 and the 65536
domain.**

This is decisive, and it retracts the premise behind v131 and v132. The divider
search backs off until the occupied groups fit the **128 physical slices**; where
it stops is determined by the depth span present on the pixel, not by how many
virtual bins sit above it. The eightfold domain increase was absorbed entirely by
one additional divider step, which is why 65536 could not and did not change the
image. Effective resolution at divider 7 is on the order of 143 mm at 2 m -
coarser than the 8192 baseline had been credited with.

Consequences:

- **The 1048576-slice domain is abandoned, and with it the multi-pass prefix-scan
  rewrite that v131 deferred.** Both would have been absorbed by further divider
  backoff in exactly the same way. This work was correctly measured before being
  written.
- The achievable separation is approximately `depth_span / 128`. Looking through
  an avatar, that span covers the body depth plus the scene behind it, so slices
  are centimetres wide and the dress and thong necessarily share one. This is the
  same co-slicing mechanism identified under "Warp saturation is the root cause of
  co-sliced layers"; what is new is that no domain size or curve shape relieves
  it, because the warp is monotonic over the entire span and redistributing
  resolution toward the near plane merely relocates the failure.

### Next: per-tile depth ranging

The remaining lever is the depth *range* each set of 128 slices must cover, not
the number of bins above it. A single global warp forces every pixel to spend its
slices on the full transparent depth span in the view. If each screen tile instead
builds its warp from the minimum and maximum transparent depth actually present in
that tile, a tile containing only the dress and the thong spreads 128 slices across
a few millimetres, which is well past the 0.1 mm requirement, while a tile looking
through a corridor of glass retains a coarse spread - correctly, since those layers
are far apart and do not need fine separation. Resolution follows the geometry.

This is what "adaptive" means in the presentation: adapting to the occupied range
rather than to the view frustum. The pass-0 occupancy raster already visits every
transparent fragment and `tileOccupancy` already exists, so the per-tile min/max
is cheap to obtain.

Scope:

- extend the pass-0 occupancy raster to atomically min/max warped depth per tile;
- make warp construction per-tile, one workgroup per tile. Per-tile bin counts are
  small enough to stay in shared memory, so the SSBO scan added in v131 and the
  divider search may both become unnecessary;
- make lookup in `avboitCaptureF.glsl` and in the resolve pass read its own tile's
  warp parameters; and
- size the per-tile warp storage. A full per-tile LUT is faithful to the
  presentation but scales with tile count - roughly 8100 tiles at 1080p with 16x16
  tiles - and may force a coarser tile grid.

The open design decision is full per-tile LUT versus a compact analytic per-tile
log curve storing only near and far. The analytic curve costs almost nothing and
captures the entire resolution win, at the price of the presentation's
filterability metadata. Given the requirement to deviate from the specification
where necessary, the analytic curve is the recommended starting point; whether the
LUT machinery is worth restoring can be decided once the resolution win is
confirmed.

### Code reading that sharpens the v133 result: occupancy is global, not per pixel

Re-reading the capture and build shaders identifies the precise reason the domain
increase was absorbed, and it is stronger than "the depth span is too wide".

`avboitOccupancy` is a **single global array** of `AVBOIT_VIRTUAL_SLICES` bits.
Every transparent fragment on screen sets one bit in it
(`avboitCaptureF.glsl:270`, inside raster pass 0); there is no screen-space
dimension. The divider search in pass 1 then counts how many bins in that one
array are occupied and halves the resolution until the count fits
`AVBOIT_SLICES = 128`.

The divider is therefore driven by the number of **distinct depths anywhere on
screen**, not by the depth span of any pixel and not by anything local to the
dress. A single glass pane across the room contributes occupied bins that push the
divider up for every pixel simultaneously. This also explains the absorption
exactly: enlarging the domain subdivides the same scene into more distinct
occupied bins, so the global count rises proportionally and the search takes one
additional halving step to reach the same 128-group budget. Landing on divider 7-8
at both 8192 and 65536 is the predicted outcome, not a coincidence.

Two corrections to the preceding section follow. The effective resolution is not
simply `depth_span / 128`; it is set by the global distinct-depth population.
And the warp is not merely "monotonic over too wide a span" - it is shared by the
entire frame, so no reshaping of a single global curve can give the dress finer
treatment than the room behind it.

### Per-tile ranging: a full per-tile LUT is infeasible, which settles the design

Sizing the per-tile variants against the existing allocation in
`FSAVBOIT::allocateVolume` resolves the open decision from the previous section,
and not by preference:

| per-tile structure | 1080p, 16x16 tiles (~8100 tiles) |
| --- | --- |
| occupancy or warp array at 65536 bins | ~2.1 GB - infeasible |
| occupancy or warp array at 8192 bins | ~265 MB - infeasible |
| min/max depth only, two `U32` per tile | ~65 KB - negligible |

A per-tile bin array cannot be carried at any domain size, so the per-tile LUT
and the per-tile divider search are not options. This is a hard constraint rather
than a tradeoff, and it removes the choice: **per-tile min/max plus an analytic
per-tile curve is the only per-tile design that fits.**

The reason it is nonetheless sufficient: the resolution problem is entirely about
*which depth interval the 128 physical slices span*, and that interval is
described by two numbers. The extinction and transmittance volumes keep their
current dimensions - 128 slices per pixel, unchanged - so this is not a memory
increase in the volume at all. Only the mapping changes.

Implementation seam identified: `avboit_virtual_depth(window_depth)` in
`avboitCaptureF.glsl` is the single point where window depth becomes a normalized
`[0,1]` depth coordinate, and `avboit_warped_slice` is its only consumer feeding
slice indices. Rescaling the normalized coordinate against the tile's own
min/max inside `avboit_virtual_depth` therefore updates capture, integration, and
resolve together, because all three reach slices through that one function.
Raster pass 0 already visits every transparent fragment and already calls
`avboit_mark_tile`, so the per-tile atomic min/max has an existing home. Note that
pass 0 runs at one-eighth resolution while passes 1 and 2 run at full resolution,
so the tile index must be derived from the full-resolution pixel in all passes or
tile identity will not agree between them.

Once the range is per-tile, the global divider search loses its purpose: a tile
that spans a few millimetres needs no compaction, and compaction driven by a
global bin population is what defeated v131 and v132. Whether the search is
removed or retained as a fallback for tiles with genuinely wide ranges is the
first question the implementation will answer.

## Hybrid Exact-OIT / AVBOIT partitioning: design options

These are **design candidates under discussion, not accepted work**. They were
raised by the user as an alternative route to the same target: give the content
that needs exact ordering an exact renderer, and leave the rest on AVBOIT. None
has been implemented, prototyped, or measured.

They must be read against two established results in this document. First, v133
and the code reading that follows it show the co-slicing defect is a property of
the **global** warp: `avboitOccupancy` has no screen-space dimension, so the
divider is driven by the distinct-depth population of the entire frame. Second,
the per-tile ranging section identifies per-tile min/max plus an analytic curve
as the fix that addresses that cause directly. Every option below routes affected
content *away from* the defect rather than repairing it. If per-tile ranging
succeeds, most of the motivation for these options disappears, so per-tile
ranging should be resolved first.

Note also that all three are hybrid depth partitioning, which is what V107-V112
did and what V113 reverted as an unauthorised departure. Deviation is now
authorised, so they are admissible; the history is recorded here so that
reintroduction is deliberate rather than accidental.

### Shared structural facts

Two facts constrain every option and were verified by code reading:

- **The traversal seam is per pass, not per draw.** Both renderers already invoke
  `pool.forwardRender(true)` and `pool.forwardRender(false)` as separate
  statements (`fsavboit.cpp` render_pass lambda; `fsexactoit.cpp`
  `renderPostDeferredCapture`). A split along the rigged/non-rigged boundary
  therefore needs no per-draw classification and no change to the dispatcher's
  shader-accessor signatures.
- **Any depth- or screen-space split is per fragment.** A single draw call spans
  pixels on both sides of a depth threshold or a coverage mask, so those splits
  cannot select a renderer at draw time. Both renderers must traverse all
  transparency, with each fragment rejected on the wrong side. The traversal cost
  is not reduced; only the accumulated content is.

A further constraint applies to all options: `FSOITDispatcher` is written on the
assumption that at most one renderer is active. `captureActive()` ORs the two,
and every shader accessor is a
`FSAVBOIT::captureActive() ? ... : FSExactOIT::...` ternary. Hybrid mode makes
both live within one frame. Because the passes remain sequential, the ternaries
stay correct provided each renderer sets `sCaptureActive` only around its own
traversal, which is how both are already written. What requires audit is resource
coexistence: Exact's node pool alongside AVBOIT's MRT attachments and SSBOs in
the same frame, and whether their binding points collide. AVBOIT already uses
five SSBO bindings against the OpenGL 4.3 minimum guarantee of eight.

### Option A: rigged geometry to Exact OIT, world to AVBOIT

`RenderOITMode = 3, Hybrid`. Rigged alpha draws are captured by Exact OIT;
everything else goes through AVBOIT.

The user's rationale is content-based and is the strongest argument for this
option: tightly stacked transparent layers are almost always rigged, because
unrigged layers a millimetre apart break as soon as the skeleton animates.
Content creators therefore do not build them. Unrigged attachments are mostly
rigid separated shells, and the exceptions - bracelets, some belts - are thin
enough that AVBOIT handles them acceptably today. This matches the reported
defect list: the dress-over-lace, hair-card, eyelash, and sleeve reports are all
rigged content.

`params.mAvatar != nullptr` is the correct predicate for this option and is
already the pass boundary. Note that it means *skinned*, not *worn*: unrigged
attachments stay on the AVBOIT path and are treated as world geometry, which is
the intended behaviour here.

Cost: this is the only option whose split is per pass rather than per fragment,
so it needs no fragment-level rejection. Against that, avatar and world
transparency interleave in depth, so the merge cannot be a single composite.
Rigged geometry must still contribute occupancy and extinction to AVBOIT's volume
for world transparency behind an avatar to be attenuated correctly, while its
colour goes to Exact's lists - so rigged content is traversed by both pipelines.
Resolve must then walk Exact's sorted nodes front-to-back and apply the AVBOIT
volume transmittance for world content in each intervening depth interval. The
avatar-behind-glass case fixed in `37406630ea` depends on this; a naive
avatar-over-world composite would regress it.

### Option B: depth-threshold split at the avatar

Everything in front of the avatar, plus the avatar itself, goes to Exact OIT.
Everything behind goes to AVBOIT. The two are merged with one source-over.

The attraction is that the partition is a depth boundary, so the merge is a
single composite with no interleaving and no per-interval transmittance sampling.
It is also principled: front layers are the least attenuated and therefore the
most visible when misordered, and depth complexity is highest near the viewer.
It degrades gracefully - pushing the threshold to the far plane gives pure Exact,
pulling it to the near plane gives pure AVBOIT - so it is tunable and testable
against both endpoints.

Constraints:

- the threshold is per pixel, not a plane. The avatar occupies a depth range that
  varies across the screen and is undefined where the avatar is absent;
- it needs a dedicated depth-only rigged pass to build the threshold buffer,
  taking the **farthest** avatar sample per pixel so the avatar lands wholly on
  the exact side. Avatar alpha does not write opaque depth, so no existing buffer
  supplies this;
- rejection should reuse the v66 indirect early-depth tile machinery to generate a
  conservative depth bound for the partition, so AVBOIT's rear pass is rejected by
  hardware early-Z rather than shading and discarding. Without that the per-
  fragment rejection is a wash;
- front-side depth complexity is unbounded. An avatar inside smoke or behind a
  particle cloud puts the whole cloud on the exact path, which is the case AVBOIT
  was built to avoid. V25 already recorded needing a 32-node threshold before a
  clothing surface stabilised, so front-side depth is substantial even without
  particles. A node cap with fallback is likely needed, which reintroduces a
  resolve-side approximation; and
- the threshold cuts through objects. A glass panel crossing the avatar's depth
  range has its fragments split between two representations. The merge remains
  exact as a set operation - each fragment goes to exactly one side, so nothing is
  double-counted - but the two halves are shaded and attenuated by different
  pipelines, so a discontinuity across the cut is plausible and unmeasured.

### Option C: avatar-covered pixels entirely to Exact OIT

At pixels the avatar covers, the whole depth column - front, avatar, and behind -
goes to Exact OIT. AVBOIT handles only pixels the avatar does not cover. The
partition is a binary screen-space mask.

This is the simplest of the three. Every pixel is resolved entirely by one
renderer, so there is no compositing at all, only a select: no source-over, no
transmittance sampling, no double-counting question. Nothing is sliced in depth,
so option B's cut-through-objects case does not arise. The mask is coverage
rather than per-pixel depth, so a stencil from a depth-only rigged pass suffices,
and the stencil test then performs both renderers' fragment rejection at
fixed-function cost - better than option B, which needs the v66 tile machinery for
equivalent rejection. Avatars are fully exact, so every rigged ordering defect on
the reported list falls inside the mask.

Its two exposures are the reason it is not obviously the best:

- **Exact takes the full depth column at avatar pixels**, including everything
  behind the avatar, unbounded. An avatar in front of a glass building or in smoke
  puts the entire column on the exact path. Option B at least bounds Exact to the
  front layers; this does not bound it at all, and avatars are usually in front of
  something. This inverts the performance argument: cheaper than B in fixed costs
  and merge complexity, potentially much more expensive in a common worst case.
- **The seam lands on the avatar silhouette.** A glass panel spanning the outline
  gets Exact's ordering inside the mask and AVBOIT's approximation outside. The
  `avatar_through_window` screenshot pair records that those two disagree visibly
  on exactly this content, so a transmission step along the outline should be
  expected. Hair silhouettes are fringed and moving, so the step would crawl frame
  to frame. Dilating the mask relocates the step rather than removing it; removing
  it requires the two renderers to agree on transmission, which is the unresolved
  defect.

### Comparison

| | A: rigged split | B: depth threshold | C: avatar mask |
| --- | --- | --- | --- |
| split granularity | per pass | per fragment | per fragment |
| merge | interleaved resolve | one source-over | select, none |
| Exact's load | rigged only | front layers, bounded | full column at avatar pixels |
| seam location | depth interleave | inside objects | avatar silhouette |
| fragment rejection | not needed | needs v66 tile bounds | stencil, fixed-function |

### Performance expectation

No measurement exists. Reasoning from the recorded v31 figure of **34 FPS AVBOIT
against 23 FPS Exact OIT** in a glass-heavy scene, that gap bounds what any hybrid
can recover, and none recovers all of it:

- both pipelines run every frame, so AVBOIT's fixed costs - warp construction,
  sparse clear, integration, resolve, bounds and proxy passes - do not shrink when
  its input does;
- Exact's cost scales with per-pixel depth complexity, and every option
  deliberately assigns it the highest-complexity content; and
- option A traverses rigged geometry with both pipelines.

The expected result is between the two endpoints, closer to Exact in avatar-heavy
scenes and closer to AVBOIT in world-heavy ones. A scene with one avatar against
an empty sim could land slower than pure Exact, because AVBOIT's fixed cost is
paid for almost no benefit. Both a glass-heavy scene and a crowded-avatar scene
must be measured before any hybrid mode is credited with a performance gain.

### Prerequisite test before any of these is built

All three rest on one untested premise: that exact ordering on avatar layers
clears the reported artifacts. The premise is plausible and matches the rigged
character of most of the defect list, but the sprites report is **not** rigged and
is an aggregate-opacity failure rather than an ordering failure. If those share a
cause, no partition fixes it; the affected pixels merely move.

The cheap discriminator is to route rigged alpha through vanilla sorted blending
while AVBOIT retains everything else, and inspect the dress, hair, eyelashes, and
sleeves. This is one build, needs no dispatcher surgery, and is close to the
`RenderWBOITAvatarLegacy` experiment already attempted on the WBOIT branch. If the
artifacts clear, the premise holds and option B or C is worth the full effort. If
they do not, all three options are elaborate ways to relocate a defect and none
should be built.

This test should be run **after** per-tile ranging is resolved, since per-tile
ranging targets the measured cause and may remove the need for a hybrid mode
entirely.
