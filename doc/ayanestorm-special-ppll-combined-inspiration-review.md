# PPLL Combined Patch Inspiration Review

Date: 2026-08-12

## Scope and constraints

This review compares commit `e52773ff8b12ba9c268ad7a5a9c81d13a2fcf44c`
(`Skip the view-angle alpha re-sort under order-independent transparency`) with
the former `ppll-combined.patch.txt` experiment. The untracked patch was deleted
after this review because its useful ideas were already implemented or recorded
here, while its remaining techniques violated the project's rendering contracts.

Ideas are acceptable only when they preserve both contracts:

- Exact OIT remains lossless: every eligible fragment is captured and sorted,
  original blend behavior is retained, and overflow causes complete same-frame
  vanilla fallback rather than approximation or fragment loss.
- Standard mode retains the vanilla render path and ordering behavior.

## Latest commit review

The commit changes five files and passes `git diff --check`.

It adopts the patch's best optimization: suppress the camera-angle-driven
`ALPHA_DIRTY` rebuild when alpha ordering cannot affect an OIT result. The
implementation is materially safer than the patch:

- It uses the neutral `FSOITDispatcher`, covering Exact OIT and AVBOIT without
  coupling `llspatialpartition.cpp` to either implementation.
- The state is snapshotted before culling, avoiding settings work in the
  per-spatial-group hot path.
- The check uses user intent plus hardware support, rather than lazy resource
  availability, so the first enabled frame is handled correctly.
- HUD, impostor, and cube-snapshot passes keep vanilla sorting because those
  passes do not use OIT.
- Switching modes already invalidates vanilla alpha ordering through
  `invalidateVanillaAlphaOrdering()`, including all region volume and bridge
  octrees, so Standard mode does not inherit stale OIT-era order.
- The upstream-owned edits have AyaneStorm ownership tags, and `isEnabled()` is
  exposed with a narrow purpose rather than duplicating capability logic.

One lifecycle edge deserves runtime testing: the snapshot is refreshed inside
the world-camera display/cull block. Verify Standard-to-OIT and OIT-to-Standard
changes during snapshot-only, startup/loading, teleport, and minimized/restore
frames. The exclusion checks make a stale `true` value harmless for HUD,
impostor, and cube snapshots, while the existing mode-change invalidation
repairs vanilla ordering when normal rendering resumes. No concrete defect was
found in source review.

## Useful ideas already adopted or superseded

### Skip view-angle alpha re-sorting

This was the one high-value idea in the patch. Commit `e52773ff8b` implements it
with correct mode, capability, pass-exclusion, and vanilla-restoration guards.
No further code should be copied from the patch for this optimization.

### Explicit post-pass GL-state restoration

The patch documents a real integration hazard: its composite changed color
masks and the blend function, then explicitly restored the state expected by
selection highlighting and glow. This is a useful **audit principle**, but its
specific restoration values must not be copied. Exact OIT currently scopes or
sets its own composite state and restores the opaque scene target. Any future
state fix should be derived from Exact OIT's actual caller/callee contract and
must not alter Standard mode.

### Conditional diagnostics

The patch gates atomic-counter readback behind a debug setting because it
forces synchronization. Exact OIT already does better: its mandatory capture
validation reads overflow, allocation count, and maximum list length together,
and its reporting is throttled. A second stats readback would add cost without
new correctness information.

### Empty-pixel discard and narrow barriers

Discarding an empty resolve pixel and using the narrowest sufficient memory
barrier are generally sound micro-optimizations. Exact OIT's current composite
and multi-pass sort have different synchronization and output semantics, so
these are candidates only for profiler-guided audit, not direct ports. Barrier
changes require explicit producer/consumer analysis for images, SSBOs, indirect
dispatch buffers, and framebuffer operations.

## Ideas that violate the Exact OIT contract

The following patch techniques must not be adopted:

- `OIT_MAX_LAYERS 32` plus a weighted-average tail. The tail is approximate and
  changes both color and ordering.
- Dropping nodes when the fixed pool overflows. Exact OIT must reject the whole
  incomplete capture, render complete vanilla transparency in the same frame,
  and grow safely for later frames.
- Excluding particles or custom/additive blends as a residual screen pass.
  Interleaving those draws outside the common ordered fragment stream loses
  cross-category ordering. Exact OIT stores and applies original blend factors.
- Packing color with `packUnorm4x8`. This quantizes shader output and is not
  lossless relative to the floating-point render target path.
- Treating additive glow as categorically order-independent and outside the
  capture. The active Exact OIT design captures emissive/glow contributions in
  the ordered data where required.
- Resolve-time opaque-depth rejection as a substitute for early fragment tests.
  It allocates occluded nodes, increases overflow risk, and previously proved
  unreliable. Exact OIT correctly uses `layout(early_fragment_tests) in` in
  every capture permutation.
- Always allocating resources with the main render target or enabling the
  feature by default. Exact OIT's setting-driven lazy allocation and complete
  Standard path better preserve vanilla behavior and VRAM use.
- Repeating PPLL declarations and append logic in each upstream shader. The
  current shared/new-module architecture is easier to merge and keeps upstream
  integration narrow.

## Recommended follow-up

No additional patch feature merits immediate implementation. The useful next
work is validation rather than porting:

1. Runtime-test the latest commit while moving the camera in dense alpha scenes
   and confirm that rebuild CPU time falls without image changes.
2. Toggle Standard, Exact OIT, and AVBOIT repeatedly; verify vanilla alpha order
   is rebuilt immediately when returning to Standard.
3. Exercise HUD, impostor, cube snapshot, startup/loading, teleport, and
   minimized/restore paths to cover snapshot freshness.
4. If profiling still shows composite overhead, audit empty-pixel early-out and
   barrier masks in the active Exact OIT shaders, proving identical output and
   synchronization before changing either.
