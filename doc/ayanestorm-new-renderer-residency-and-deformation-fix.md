# Renderer correction: retained deformation memory and work before culling

Runtime result: the user reports continued lag and flashing textures. These changes are not
accepted as a fix. Current investigation: [texture reload regression](ayanestorm-new-renderer-texture-reload-regression.md).

Date: 2026-11-10  
Author: chanayane@firestorm

## User runtime rejection of the initial reclamation patch

The user reports major slowdown with the previous changes. They are not accepted as a
successful performance fix. The latest log ends at 17:16:28 UTC (18:16:28 Paris time).
At 17:16:18 UTC it records:

- GPU frame 73.5701 ms; driver usage/budget 10,609 / 10,431 MB.
- Geometry 4,096 MB versus its 1,564 MB nominal share; chunks 2,880 MB, only 60 MB free.
- Position table 537 MB and cluster table 502 MB.
- Vertex dispatch end/live 566,625 / 145,899 blocks; cluster blocks 77,214 / 21,055.
- 52 of 72 avatars parked; palettes still take 10.242 ms, cluster work 23.0961 ms,
  group work 9.56511 ms and each pyramid about 11–12 ms. Vertex work is 2.78156 ms.

The broad GPU slowdown and budget excess remain consistent with residency pressure.
This is not proof from a GPU paging capture. It establishes that tail-only reclamation
was insufficient: interior holes can pin large physical buffers even after parking.
Skipping more useful vertex arithmetic alone would not directly explain or eliminate
the measured pyramid/palette slowdown.

## Physical position-cache compaction

New module: `indra/asrenderer/asrscenestorememory.cpp`.

When a position cache of at least 16 MB holds no more than half its capacity in live
ranges, rebuild its resident bindings densely, at most once per second:

1. Assign each resident skinned binding a disjoint position span in a new allocator.
2. Rebuild vertex and cluster dispatch maps densely and update each binding's indices.
3. Remove pending retirement records belonging to those old allocator generations.
   Applying their old offsets to the new allocator would free another binding's storage.
4. Require a fresh physical position buffer, even if new arrivals grow its requested
   capacity back to the old size in the same update.
5. Retire the old GPU buffer through the existing frames-in-flight delay.

The backend does not copy or clear old position contents into the new buffer: current
draw consumers regenerate their selected LOD's vertices before they read them. Persistent
source geometry and unchanged morph outputs retain their existing storage/content rules.
Current-frame clusters regenerate their position addresses before culling. Both Vulkan
and Metal use the same store compaction and physical-buffer replacement contract.

This directly addresses the measured position-cache holes. It does not compact the mixed
source/deformed cluster table or relocate partially occupied immutable geometry chunks.

## Deformation demand after occlusion, with same-frame recovery

New shared shader module: `asrSceneSkinDemand.glsl`. A binding's demand state uses a
previously unused word; `GpuBinding` remains 112 bytes and its demand word is at byte 100.

The frame now orders:

1. Predict joints, generate resident palettes, place bindings and update changed morphs.
2. Generate conservative joint-derived cluster/group bounds without reading deformed vertices.
3. Cull against previous depth and append accepted clusters to draw lists. Only a successful
   append requests deformation for its skinned binding.
4. Deform early-admitted bindings, then draw early opaque surfaces and build current depth.
5. Retest occluded candidates against current depth. Newly admitted bindings deform before
   the late draw in that same frame. A binding already deformed early is reused.
6. The transparency passes consume the resulting early/late lists and current positions.

Both culling phases atomically claim a binding only if it has no earlier claim. The bind
pass resets demand each frame. Audit culling does not mutate demand. The frame graph declares
culling's demand writes and the subsequent deformation reads, generating the required
barriers/fences on both backends. Pass registration uses an explicit phase-to-pass mapping;
the skin phases are no longer assumed contiguous in the frame graph.

The optional GPU position audit runs after deformation. In audit mode every chosen binding
is deformed early, including occluded ones, so the no-occlusion reference draw cannot read
uninitialized positions. Normal mode admits only bindings with real draw-list consumers.

This implementation skips whole bindings with no surviving clusters. It does not compact
per-visible-cluster vertex tasks inside a partially visible binding. Conservative cluster
bounds are retained; no unsafe tightness or backface assumption is introduced.

## Parked palettes

A previously unused skeleton word now reports `SKELETON_PARKED` (48-byte layout unchanged,
flags at byte 44). Park/unpark updates it in the same command batch as binding residency.
Palette work skips parked skeletons. Rigid attachments still receive joint prediction;
they do not depend on these skin palettes. Un-parking restores palettes before skin bounds
and vertices are generated in that frame.

## Source LOD admission and retirement

New policy module: `asrmeshlodresidency.h`.

- Refresh demand even when no new network request is needed; cache the asset header's
  actual LOD remapping so a necessary finer fallback remains wanted.
- Admission and eviction use the same window: 30 seconds normally, 5 under pressure.
- Check current users before rejecting a late callback or evicting a supposedly stale LOD.
  A delayed time-sliced sweep must not create disappearance or repeated rebuilds.
- Retire only an obsolete highest built LOD for which every affected face has a usable
  coarser replacement. A weighted face requires a compatible weighted replacement.
- Mark geometry removing immediately; the existing store retains it until all instance
  and finer-LOD references have gone. Unrelated pending updates do not delay retirement.
- Late, unwanted callbacks cannot silently rebuild an evicted level.
- Remove the speculative pressure rule requesting one less LOD for distant scenery.
  Visible quality follows the viewer's own LOD choice; pressure changes retention time.

Eviction traversal has a 0.5 ms scheduling budget, checked between assets, and resumes by
key rather than retaining an iterator across object deletion. One asset with many users
can exceed that budget. A source LOD currently required by a live user is retained even
when its requested quality is expensive; this is not a hidden quality reduction.

## Visual results preserved

The full-precision source/deformed positions, common camera-relative placement, palette
origin ordering, invariant positioning and rigged tie ranks remain. No lip arithmetic was
replaced with quantization. Texture-demand hysteresis remains four seconds. Mesh work still
precedes time-budgeted system-body processing from the earlier patch.

## Validation and current limits

- Khronos `.glslang/bin/glslang.exe` accepted all 19 shader entry points targeting Vulkan 1.3.
  Temporary stage-suffixed copies and SPIR-V outputs are outside the repo in
  `/tmp/asr-validation-cl8k5lk7`. These native shaders already declare GLSL 460; they are
  validated with their existing version and extensions, not the viewer's GLSL 410 prefix.
- Parsed the generated SPIR-V member decorations: deformation demand matches the CPU
  offset 100. CPU static assertions retain the 112-byte binding and 48-byte skeleton layouts.
- C++17 syntax-only checking covers scene/store/frame-graph code, the new compaction module
  and regression-test source. It generates no executable or object files.
- Added checks for source-LOD expiry/re-admission, early/late ordering and synchronization,
  parked palette flags, and an 80-avatar compaction scenario. The latter retains live spans
  near the old buffer end and a pending old-generation retirement while compacting, then
  un-parks another avatar to check that stale retirement cannot free its new allocation.
- No project build, execution of those C++ regression tests, or new viewer/GPU runtime
  validation has been performed. The previous runtime failure is not declared resolved.

Remaining costs include partly occupied immutable chunks, mixed cluster-table holes,
conservative bounds and deformation of a whole binding when only some clusters survive.
New shadow/reflection views must contribute their own demand before using this optimization.
FPS restoration and the actual VRAM reduction need the user's next runtime measurement.
