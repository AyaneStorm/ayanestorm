# Adaptive Voxel-Based OIT research

## Source

The primary reference is Michal Drobot's Activision presentation, *Adaptive
Voxel-Based Order-Independent Transparency*, from the SIGGRAPH 2025 Advances in
Real-Time Rendering in Games course:

https://advances.realtimerendering.com/s2025/content/AVBOIT_SIG2025_MDROBOT-final.pdf

## Algorithm

Adaptive Voxel-Based OIT (AVBOIT) is an approximate transmittance-integral
method. It is not a compressed per-pixel linked list and does not retain every
fragment.

The base VBOIT algorithm:

1. Distributes a fixed number of depth slices along camera depth.
2. Rasterizes transparency into a lower-resolution 3D extinction volume.
3. Converts alpha to logarithmic extinction and accumulates it with integer
   atomic addition.
4. Integrates extinction from the camera through the depth slices.
5. Renders transparent surfaces in any order at output resolution. Each
   fragment samples the integrated volume to estimate how much transparent
   material lies in front of it.
6. Resolves accumulated transparent color and the opaque background.

The adaptive extension first computes one-dimensional depth occupancy. A prefix
sum produces a depth-warp lookup table that removes empty depth ranges and packs
occupied ranges into the available physical slices. If the occupied virtual
resolution does not fit, it is repeatedly reduced until it fits the fixed
physical allocation. This spends depth precision where transparent content
actually exists without implementing a fully sparse three-dimensional volume.

## Storage and performance properties

AVBOIT has bounded storage rather than storage proportional to the number of
captured fragments. Activision's comparison used a one-eighth-resolution
transmittance prepass, 128 physical slices, and an 8K one-dimensional depth-warp
LUT. Their monochrome AVBOIT representation used 8-bit extinction slices; RGB
variants required additional storage.

The presentation reports that the depth-warp LUT has approximately zero
generation and sampling cost in its tested implementation, while integration
cost scales with occupied slices. Its representative PS5 4K test measured:

- no OIT: 4.4 ms;
- AVBOIT without its culling optimization: 5.667 ms;
- AVBOIT with zero-transmittance culling: 4.967 ms.

These numbers describe Activision's content, renderer, hardware, and
configuration and should not be treated as an AyaneStorm prediction.

AVBOIT also records the depth slice where integrated transmittance reaches an
assumed zero. It can populate conservative depth tiles there and use normal
depth rejection to skip transparent work behind effectively opaque accumulated
transparency. This differs from Exact OIT's opaque-cutoff optimization: AVBOIT
can derive effective opacity from many overlapping fractional-alpha events,
whereas the exact cutoff needs a fragment whose stored alpha is already
mathematically opaque.

## Accuracy limits

AVBOIT is approximate. When transparent events occupy distinct depth slices, it
can reproduce their occlusion well. When multiple events overlap one slice,
their individual order is lost and the method degenerates toward weighted
blending within that slice. Linear splatting and sampling soften the error but
do not make the result exact. Slice allocation therefore controls the
precision, memory, and performance trade-off.

Consequences include:

- thin or nearly coplanar transparent layers may mix incorrectly;
- close surface intersections can be over- or under-occluded;
- increasing depth slices reduces but does not eliminate the failure;
- artist-authored nonphysical manual sort orders remain incompatible with a
  physically depth-based result.

## Relevance to AyaneStorm Exact OIT

AVBOIT directly addresses the workload that currently hurts Exact OIT most:
large screen-space smoke, splashes, and particle cards with deep
fractional-alpha overdraw. It avoids allocating, traversing, and sorting one
node per surviving fragment. Zooming still increases rasterized fragment work,
and AVBOIT normally rasterizes transparency once for extinction and again for
color, but its later work and memory do not grow as a deep per-pixel linked-list
sort.

It is not suitable as a lossless optimization inside the current Exact OIT
mode. Adopting it would mean adding a separate approximate OIT mode or a
content-specific hybrid. A hybrid would also need a reliable classification
rule: combining exact and approximate transparent contributions at overlapping
depths is itself order-dependent and cannot be done by simply compositing two
completed buffers.

For AyaneStorm, AVBOIT is best considered as:

- a future high-performance approximate mode aimed at particles, smoke, and
  water effects;
- a source of ideas for occupancy-aware scheduling and accumulated-opacity
  culling;
- not a replacement for the current exact mode when exact per-fragment
  compositing is the requirement.

Implementation would be a substantial new renderer path. It needs an extinction
prepass, a depth occupancy and warp stage, a three-dimensional extinction
buffer, prefix integration, modified transparent shading, and a new resolve.
It should therefore be designed as a separate module rather than folded into
the existing Exact OIT linked-list shaders.

## macOS OpenGL 4.1 compatibility

The published AVBOIT design is not practically implementable on the portable
macOS OpenGL 4.1 feature set. Its GPU implementation relies on facilities that
became core after OpenGL 4.1:

- compute shaders and shader storage buffer objects became core in OpenGL 4.3;
- shader image load/store and image atomics became core in OpenGL 4.2;
- the adaptive occupancy scan, depth-warp construction, extinction integration,
  and integer atomic splatting rely on those kinds of writable random-access
  resources and general GPU work.

Apple's OpenGL implementation stops at 4.1 and OpenGL has been deprecated on
macOS since 10.14 in favor of Metal. Extensions must not be assumed to provide
the missing facilities consistently across supported Macs.

A substantially different raster-only approximation could use layered
rendering, fixed-function additive or multiplicative blending, and multiple
fullscreen passes. However, arbitrary mesh voxelization, adaptive occupancy
compaction, prefix integration, synchronization, and zero-transmittance work
generation would either be unavailable or require expensive emulation. Such a
path would no longer be the published AVBOIT implementation and is unlikely to
retain its performance advantages.

AVBOIT should therefore be considered viable through Metal on macOS, or through
OpenGL 4.3-class functionality on other platforms, but not as a portable
AyaneStorm OpenGL 4.1 feature.
