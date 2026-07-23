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
- Oversubscribed virtual depth is mapped proportionally into the fixed physical
  budget. The iterative virtual-resolution reduction and logarithmic-depth
  reparametrization from the presentation are not implemented.
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
