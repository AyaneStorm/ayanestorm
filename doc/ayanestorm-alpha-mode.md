# AYAstorm Unified Alpha Mode

## AYAstorm History

AYAstorm commit `d1749048c7` attempted to fix a cross-viewer alpha ordering
defect. A
rigged foreground such as hair can depth-reject alpha-blended world geometry
behind it, while simply reversing the two broad passes causes attached static
alpha prims such as eyelashes to be overdrawn by the rigged hair.

That older traversal was:

1. Rezzed non-rigged alpha blend.
2. All rigged alpha blend.
3. Attached non-rigged alpha blend.

It classifies each `LLDrawInfo` from `LLViewerObject::getAvatar()`. AYAstorm
reports Linux runtime validation for both ordering cases and identifies the
defect as common to Second Life viewers, not macOS-only.

Commit `7d6f7797f3` from July 6, 2026 supersedes that heuristic with a unified
depth merge of rigged and non-rigged spatial groups. It assigns each rigged
avatar run the near depth of `getLastAnimExtents()` along the camera axis,
sorts avatar runs back-to-front while preserving `mRenderOrder` within an
avatar, and merge-walks that list with the already sorted non-rigged list.
Equal-depth entries draw non-rigged first.

The newer change exists on AYAstorm's Vulkan-development lineage, including
`origin/feature/ayastorm-r41-vk-canonical` and later r42 branches. Its ordering
algorithm is CPU-side and does not depend on Vulkan.

## AyaneStorm Integration

AyaneStorm exposes the newer unified traversal as dispatcher mode 3,
`AYAstorm (mayatonton)`. It is opt-in on every platform and is available on
macOS, where Exact OIT and AVBOIT remain unavailable.

The implementation uses only CPU sorting and the viewer's existing OpenGL
depth-state wrapper. It imports no Vulkan renderer code. The merged list is a
temporary dispatcher-owned view; it does not modify the cull lists used by
other modes.

Only the main-world, post-water, non-impostor, non-reflection render uses the
merged traversal. Rigged groups write depth and non-rigged groups do not, as in
AYAstorm's implementation. Its GLTF depth prepass explicitly writes depth.

Standard retains its original rigged-then-non-rigged calls and list ordering.
Exact OIT and AVBOIT retain their renderer-owned, unfiltered capture
traversals. Pre-water, HUD, impostor, reflection, cube snapshot, and DoF
auxiliary alpha paths retain their existing traversal.

This mode is unrelated to the graphics-preset-dependent disappearance tracked
in `macos-alpha-disappearance-reflection-probes.md`.

## Verification

No build was run, per repository policy. Runtime coverage should compare:

- AYAstorm mode with world glass or foliage viewed through rigged hair.
- AYAstorm mode with attached alpha prims and hair cards intersecting rigged
  hair.
- Standard against the pre-change rendering baseline.
- Standard pre-water transparency, HUDs, impostors, reflections, and DoF.
- AVBOIT and Exact OIT capture, composite, and alpha debug behavior on
  supported non-macOS systems.

The screenshot comparison motivating the update showed the older three-pass
heuristic producing a hard rectangular hair-card overlap where Exact OIT
resolved the local surface order. The unified merge addresses the broad
rigged/non-rigged ordering error, but remains conventional sorted alpha rather
than per-pixel OIT; intersecting geometry can still have cases no group-level
ordering can resolve perfectly.
