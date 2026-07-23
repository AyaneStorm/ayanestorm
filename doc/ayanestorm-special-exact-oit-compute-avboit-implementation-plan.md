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
validation are pending.

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
per XY cell, still half the earlier one-`U32`-per-slice prototype. Integrated
transmittance remains the specified filterable 8-bit representation. The
additional scratch precision is required by the current full-resolution
folding adaptation; a future true low-resolution conservative prepass can
return the scratch representation to packed 8-bit without losing individual
contributions.

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
