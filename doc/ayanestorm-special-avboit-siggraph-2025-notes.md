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
