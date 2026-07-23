# Exact OIT opaque-cutoff implementation plan

## Summary

Add a lossless optimization that removes fragments hidden behind the nearest
mathematically complete overwrite node before Exact OIT sorting.

The initial implementation will use only a resulting source alpha exactly equal
to `1.0`. The approximate `0.995` proposal remains a deferred exploration and
is not part of this implementation.

True alpha-mask rendering remains unchanged. Alpha-mask materials already
discard rejected texels and render accepted texels through a depth-writing path
outside Exact OIT. A binary-looking texture configured for alpha blending does
enter Exact OIT; its fully opaque texels may qualify for the cutoff, while
fractional texels produced around filtered edges may not.

## Current status

Implemented:

- first-sort-pass-only cutoff discovery in the existing composite shader;
- default-enabled `RenderExactOITOpaqueCutoff` debug toggle for A/B validation;
- diagnostic mode 7 for cutoff eligibility and removable-node visualization;
- exact standard-blend and final-alpha predicate;
- nearest-cutoff selection using the existing depth/allocation-index order;
- retained-list relinking and retained-count handoff to natural merge sort;
- Exact OIT and AVBOIT shader-cache revision v20 after subsequent shader experiments,
  restoration of the stable fullscreen sorter, removal of a redundant
  normal-composite diagnostic traversal, correction of diagnostic glow, and
  exact zero-alpha capture rejection, and the optional compute sorter;
- implementation notes in the findings and how-it-works documents.

Pending:

- viewer shader compilation and runtime rendering validation;
- the full correctness comparison matrix below;
- cutoff-heavy and no-qualifier GPU measurements;
- a ship/no-ship decision based on visual parity and whole-frame performance.

## Implementation

### Cutoff discovery

During the first natural-sort pass, scan each nontrivial pixel list for the
nearest qualifying cutoff. Use the existing Exact OIT total ordering:

- greater depth comes first, producing far-to-near order;
- allocation sequence breaks equal-depth ties;
- the selected cutoff is the qualifying node appearing latest, or nearest, in
  that order.

A node qualifies only when all of the following are true:

- it is an ordinary color node, not a glow-only node;
- its resulting shader-produced `color.a` is exactly `1.0`;
- its packed color blend factors are `SOURCE_ALPHA` and
  `ONE_MINUS_SOURCE_ALPHA`;
- its packed alpha blend factors are `ZERO` and
  `ONE_MINUS_SOURCE_ALPHA`.

This is the standard blend tuple currently captured by the regular alpha and
GLTF alpha paths. At alpha `1.0`, both destination color and destination alpha
are multiplied by zero. The separate glow expression also multiplies the
existing glow by zero. The node therefore completely replaces every
contribution behind it.

Do not initially recognize other blend tuples, even if additional tuples might
later be proven safe. Custom particle blends, destination-dependent modes, and
glow-only nodes never qualify in this version.

### List pruning and sorting

When a qualifying cutoff exists:

1. Rebuild the pixel's linked list with the cutoff and every node ordered
   nearer or later than it.
2. Remove only nodes ordered farther or earlier than the cutoff.
3. Preserve the existing equal-depth allocation-sequence semantics.
4. Pass the retained head and retained fragment count directly to the existing
   natural merge pass.

Run discovery and pruning only during the first natural-sort invocation. Add a
first-sort-pass shader uniform for that purpose. A default-enabled debug setting
may gate the optimization for A/B validation. Do not add another fullscreen
draw, synchronous CPU readback, approximation, or fallback path.

Allocated but pruned nodes may remain in the node buffer until the next frame;
they simply become unreachable from the pixel list. Capture counts and overflow
decisions must continue to use the complete captured allocation count.

### Integration constraints

- Execute the optimization only when Exact OIT is enabled and capture has
  completed without overflow.
- Leave the disabled Exact OIT and vanilla rendering paths untouched.
- Preserve the existing complete same-frame vanilla fallback after overflow or
  failure.
- Do not change capture resolution, discard visible fragments, introduce a
  layer limit, or use WBOIT as a fallback.
- Ownership tags are not required in Exact OIT-owned source files.
- Bump the Exact OIT shader-cache revision whenever the shader changes. The
  implementation currently uses v20.

## Validation

### Rendering correctness

Compare the optimized result against the current Exact OIT result for:

- an opaque-alpha sprite texel hiding many deeper transparent fragments;
- translucent and glow nodes nearer than the cutoff;
- multiple qualifying nodes at different depths;
- equal-depth nodes with different capture sequences;
- custom particle blend factors;
- glow-only lists;
- lists without a qualifying cutoff;
- alpha values immediately below and above `1.0`;
- filtered edges of otherwise binary alpha textures;
- actual Alpha Mask materials;
- GLTF alpha blend and GLTF alpha mask materials;
- overflow followed by the complete same-frame fallback;
- Exact OIT disabled.

Values such as `0.995` and `254/255` must not qualify. Only exact equality with
`1.0` is accepted.

The optimized Exact OIT output must match the current output, and disabling
Exact OIT must retain the current vanilla appearance.

### Performance

Profile at least:

- the moving-camera sprite scene that previously exposed the slowdown;
- a sprite scene with large opaque interiors and filtered transparent edges;
- deeply overlapping translucent content with no qualifying cutoff;
- glow-heavy content;
- a stationary-camera comparison.

Measure the natural-sort and final-blend GPU zones. Also check whole-frame GPU
time, because cutoff discovery adds traversal work even when nothing qualifies.

If a pixel captures `n` fragments and pruning retains `k`, the intended change
is from approximately `O(n log n)` sorting plus `O(n)` blending to cutoff
discovery followed by `O(k log k)` sorting and `O(k)` blending. Capture shading,
node allocation, VRAM demand, and overflow frequency are not reduced.

Do not ship the optimization if the discovery cost creates a material overall
regression in representative scenes without qualifying cutoffs. In that case,
retain the findings and revisit a more efficiently fused discovery mechanism.

## Documentation work during implementation

Update `ayanestorm-special-exact-oit-findings.md` with implementation results,
measurements, limitations, and any rejected approaches.

Update `ayanestorm-special-exact-oit-how-it-works.md` with:

1. a simple explanation of why fragments behind a fully overwriting transparent
   node can be skipped;
2. a separate technical section describing alpha-mask routing, the exact packed
   blend predicate, total ordering, relinking, and unchanged overflow behavior.

Keep the near-opaque `0.995` idea documented only as a later approximate
exploration requiring an explicit quality decision. Do not add ownership-tag
comments to Markdown files.

## Fixed assumptions

- “Alpha mask” means the viewer's actual mask material mode. A binary alpha
  texture configured for blending is handled as Alpha Blend.
- The final shader-produced alpha determines Exact OIT cutoff eligibility.
- Only exact alpha `1.0` is accepted.
- The first implementation supports only the standard packed blend tuple.
- The optimization targets sorting and final compositing, not capture cost,
  allocation demand, or overflow prevention.
