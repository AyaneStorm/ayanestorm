# New renderer: memory and avatar loading regression audit

Current follow-up: [residency and deformation correction](ayanestorm-new-renderer-residency-and-deformation-fix.md).
The user rejected the initial patch's runtime performance; its tail reclamation was insufficient.

Date: 2026-11-10  
Author: chanayane@firestorm

## Scope and verification status

Reviewed the last five commits, renderer documentation, current allocation/retirement code,
viewer texture loading, system-body scheduling, and the latest local viewer log.
The user confirms only the lips-flicker and texture-cycling fixes. Earlier documents saying
other optimizations worked are historical reports, not acceptance of this checkout.

No project build or new viewer/GPU run was performed. Prepared changes need the user's
Windows build and runtime validation; a return to 150 FPS is not established by this audit.

## Documentation inventory

- `ayanestorm-new-renderer-requirements.md`: renderer parity, responsiveness and memory requirements.
- `ayanestorm-new-renderer-plan.md`: persistent scene, independent render thread, Vulkan/Metal,
  memory constraints, and real geometry rather than avatar pictures.
- `ayanestorm-new-renderer-status.md`: implementation history; old test and performance results
  describe particular checkpoints, not the present changes.
- `ayanestorm-new-renderer-avatar-body-plan.md`: shared system meshes and per-avatar morphs;
  the later attachment-wait gate was not part of its original visibility rules.
- `ayanestorm-new-renderer-avatars-handoff.md`: avatar implementation, fault/precision fixes,
  later crowd changes, and the introduction of system-body polling.
- `ayanestorm-new-renderer-crowd-performance.md`: comparative design research, including
  picture-based impostors. Its measurements are not measurements of this renderer.
- `ayanestorm-new-renderer-performance-followup.md`: audit and proposals. It correctly separates
  animation throttling from deformation work and identifies unreclaimed high-water storage.
- `ayanestorm-new-renderer-crowd-work-plan.md`: recent experiments and reversals. Its own final
  notes acknowledge that chunks/tables did not give released memory back to the driver.

## Last five commits

| Commit | Review |
|---|---|
| `b31f028a5e` | Adds texture demand hysteresis, memory diagnostics and more aggressive animation periods. Preserve the user-verified texture hysteresis. Animation changes are not evidence of reduced GPU geometry work. |
| `b65d09eba0` | Adds parking, smaller growth steps, driver-aware texture limits and LOD eviction. Parking releases allocator ranges but neither physical tables nor chunks; dispatch bounds also retain their historical ends. The eviction implementation was subsequently removed. |
| `d9bfff3288` | Removes LOD eviction, changes VMA allocation policy, adds driver/allocator diagnostics and corrects avatar position usage. Allocation policy alone cannot free retained application buffers. |
| `989da8f08c` | Reverts the allocator changes and changes texture steering/bootstrap behavior. The bootstrap fix is incomplete: a visible, never-sampled texture still falls through to expiration without refreshing its lifetime. |
| `d1d51c7000` | Restores the allocator policy and enables parking again. No new physical-buffer reclamation accompanies parking. Enabling it is not proof that memory is returned or avatar recovery works. |

## Extended review: system-body introduction through checkpoints 6–9

The previously unreviewed interval is `b543e55f..b31f028a^`: four commits.
Also checked the integration changes in `b543e55f` itself. This follow-up is a source
review, not a runtime comparison; no further renderer source changes were made here.

| Commit | Added behavior | Assessment |
|---|---|---|
| `b543e55f24` — avatars 5 | System-body module, eager skeleton creation for all eligible avatars, body updates inside the avatar phase, shared-material bridge, morph-mask notification. | Confirms the origin of system-body work competing with mesh arrival. The prior corrections in this working tree move that work behind mesh processing and defer new bodies behind upload backlog. |
| `31d84b5141` — avatars 6 | Separate vertex/cluster dispatch tables; one group workgroup per binding; joint-derived cluster bounds replacing triangle-derived bounds; frustum rejection before skinning; distance-ranked animation periods and delayed pose interpolation; diagnostics. | Largest additional performance-sensitive change. Dispatch separation removes empty work, but does not bound useful vertex work. Bounds and facing rejection changed materially; see below. |
| `dcc4d0fe05` — avatars 7 | Animation notification hook, conditional joint extraction, a 64-frame safety sweep, reusable scratch vectors, body-weight polling conditional on pose reads. | CPU-side reduction, not a GPU working-set limit. Does not prioritize mesh loading or release hidden body morphs. Those lifetime/scheduling issues are handled by the earlier working-tree corrections. |
| `e315237e7` — avatars 8 | Normals/albedo/alpha debug views; inverse-transpose/cofactor normal handling for rigged meshes and corrected normal transformation for rotated instances. | A shading/correctness change with additional per-pixel normal arithmetic. No measured evidence here establishes it as the cause of the frame-rate collapse. |
| `feb9e33992` — avatars 9 | Float rigged source positions, revised rigged depth ranks, shared invariant camera-relative positioning, palette-origin ordering, Metal invariance flag; also blended normal/placeholder lighting and more debug views. | Contains the user-verified lip-flicker fixes mixed with separate shading changes. A blanket rollback would remove those verified fixes. |

### Checkpoint 6: looser bounds and weaker facing rejection

Before this commit, animated cluster bounds and facing cones were reconstructed from
the deformed triangles. It replaces those with the union of the source cluster sphere
carried through every influencing joint. The union ignores each joint's contribution
magnitude when bounding the cluster: a joint with a tiny weight still contributes its
full transformed sphere. This can be substantially looser than the actual skinned surface.

It also disables cluster-facing cones unless the cluster has exactly one joint, exact
weight sum and no morph, with a suitable rigid/uniform-scale transform. Group-facing
cones remain disabled. Consequently, reducing the bounds-pass cost can increase the
number of clusters surviving downstream culling and increase raster/OIT work.

These are confirmed algorithm changes, not proof that the bounds are invalid. Their
net performance needs a same-scene comparison of surviving clusters and draw/OIT time.
The current skin audit checks a sampled stored position against direct skinning; it does
not establish that cluster bounds are tight or that this tradeoff improved frame time.

The commit does not add occlusion rejection before vertex deformation. In-view geometry
can still be skinned before the ordinary scene culler later rejects it behind a wall.

### Checkpoints 6–7: CPU animation savings do not imply GPU savings

`ASRendererFullRateAvatars` controls CPU pose frequency. The GPU still skins each selected
visible binding every render frame, using the interpolated/predicted pose. It neither caps
the number of fully drawn avatars nor limits their resident geometry.

The scheduling policy converts time gaps to frame periods using a smoothed loop interval,
then tests a frame-count modulo. For native-controlled avatars it disables the old
four-second impostor timeout. There is no replacement hard wall-clock maximum pose-age
check. Thus the comments describing time gaps are targets rather than hard guarantees
during stalls or changing periods. Interpolation deliberately shows poses one sample late.
The 64-frame extraction sweep in checkpoint 7 likewise has no fixed duration in seconds.

These changes can affect motion freshness. They do not explain 35 ms of GPU vertex
skinning as a direct CPU scheduling cost, nor do they free the GPU deformation cache.

### Checkpoints 8–9: normal cost and verified precision cost

The opaque resolve skins three vertex normals for each covered pixel. Checkpoint 8 adds
cofactor construction and per-joint normalization there. Checkpoint 9 shares this normal
routine with the blended vertex path used by the OIT pipelines. Shader source reuse alone
does not prove the actual cost in each pipeline: unused outputs/arithmetic may be removed
by the shader compiler. A GPU capture is needed to isolate the added normal cost.

The available slow-session reading attributes about 2.26 ms to opaque resolve, versus
35.02 ms to skin vertices and 54.37 ms to early drawing. It does not support blaming the
entire 131 ms frame on opaque normal correction.

Rigged source positions increase from 8 to 16 bytes per vertex in checkpoint 9. With the
existing 16-byte attributes and 12-byte skin weights, those vertex arrays grow from
36 to 44 bytes per vertex (about 22%, excluding indices/cluster metadata). This is a real
memory cost of the verified lip precision fix. Deformed positions were already float4
before checkpoint 9; this commit does not newly double that output table. Preserve the
precision fix and reduce unnecessary residency/lifetimes instead of restoring lossy packing.

### Follow-up conclusion

The system-body introduction adds eager CPU/resource work before mesh processing; that
is addressed by the prior working-tree changes. Checkpoint 6 is the strongest additional
candidate for a downstream culling/drawing regression because it changes bounds quality
and facing rejection, not merely scheduling. This review does not establish a new leak
or a measured performance win/loss for the four intervening commits. Do not treat their
historical standalone-test results as viewer performance validation.

## Viewer log evidence: VRAM pressure and GPU time

Source: `%APPDATA%\AyaneStorm_x64\logs\AyaneStorm.log`, last session ending at
16:18:58 UTC (17:18:58 Paris time).

At 16:18:44 UTC:

- GPU frame: 130.942 ms, consistent with the reported roughly 7 FPS.
- Driver usage/budget: 10,680 / 10,634 MB.
- VMA blocks/allocations: 10,528 / 8,687 MB. About 1.8 GB is block overhead/free space,
  not available VRAM merely because the application's live allocations are smaller.
- Geometry chunks: 2,304 MB, only 14 MB free inside them.
- Scene tables: 720 MB, including 376 MB of deformed positions and 286 MB of clusters.
- Skin vertices: 35.0181 ms; early draw: 54.3694 ms. Main-thread avatar maintenance was
  about 0.85 ms in the preceding readings, so it cannot alone explain this GPU collapse.

Driver-budget excess and the broad slowdown are consistent with residency pressure.
This is not a new GPU residency capture. Most chunk bytes in this reading are live;
empty-chunk reclamation alone cannot remove that working set.

## Prepared corrections: allocation lifetime and driver memory

- `RangeAllocator::release()` now collapses a free tail, including adjacent free ranges.
  Retired blocks/joints/palettes no longer permanently extend dispatch bounds.
- Empty geometry chunks are destroyed after their ranges pass the existing in-flight delay.
  Stable chunk slots are reused without relocating live geometry addresses.
- Range-backed tables can shrink with two-to-one hysteresis after retirement. Paired
  palette-job and morph-attribute tables follow their owning allocator. New allocations size
  physical tables by their used end rather than restoring old logical allocator capacity.
- Both backends preserve only the surviving prefix when resizing a table and retire its old
  buffer through the existing in-flight mechanism. Transient visibility lists can shrink too.
- Large Vulkan scene buffers use dedicated memory, so destroying them releases that allocation
  instead of leaving their space inside a shared VMA block.
- Table capacities stay word aligned for Vulkan fill/copy operations.
- Driver-budget excess is subtracted from affordable texture memory. Previously excess was
  clamped to zero room, incorrectly treating all current texture usage as affordable.
- The decoded-texture admission check permits a smaller replacement even at the hard limit.
  GPU mip-copy shrinking already has its own path; this fixes the decoded replacement path.

## Prepared corrections: mesh bodies before system bodies

- Mesh arrivals, shape completion, LOD requests, texture visits and object application run
  before system-body processing. System-body discovery no longer eagerly creates and poses
  a skeleton for every built avatar before worn meshes are applied.
- New system bodies wait while object application or GPU geometry uploads are backlogged,
  or that avatar's bake/attachment-count readiness is incomplete. Rezzed status alone does
  not establish that mesh uploads finished. There is no ten-second fallback timer that starts
  system work while the outfit is still arriving. System-only avatars remain eligible once
  their bakes are ready and primary work has cleared.
- System bodies receive a round-robin 0.5 ms scheduling budget, checked between avatars.
  A single update/shared-mesh initialization can exceed that budget; it is not preemptible.
  Mesh skeletal updates stay in the main avatar phase. Pending system morph changes are
  remembered until that body receives a turn.
- The current camera is published before system-body work, so that work does not delay its
  publication to the render thread.
- Hidden system parts release their private morph output/weights/masks rather than polling
  and retaining them indefinitely. Returning parts initialize from current appearance values.
- A visible texture with no image or no valid feedback stays wanted and refreshes its lifetime.
  The previous bootstrap branch could expire it immediately and repeatedly cancel loading.
- An evicted texture discards obsolete feedback. Mip replacements retain the existing demand
  window, preserving the verified texture-cycling correction.
- Before a worn texture has feedback, its live wearer provides a conservative visibility
  bound and a small initial demand. Native rendering skips GL face batching, which normally
  marks faces rigged and maintains deformed bounds; bind-space face bounds cannot reliably
  prioritize a nearby avatar. GPU feedback takes over resolution selection once it draws.

## Preserved fixes and remaining limits

The initial patch made no shader or shader-version changes. The lip precision/ranking/invariance fixes and the
four-second texture-demand hysteresis remain in place. No impostors or additional LOD
quality reductions were introduced. The removed LOD-eviction experiment was not reinstated.

Live fine source LODs, interior holes in partly occupied chunks/tables, and per-frame
skinning of geometry later rejected by occlusion remain costs. This patch reclaims empty
chunks and sufficiently large free table tails; it does not compact live device addresses
or establish a hard bound on an arbitrarily large visible crowd. VMA may retain shared
blocks containing smaller live allocations.

## Validation performed and pending

- Syntax-only C++17 checking of `asrscenestore.cpp` and `asrtestmain.cpp` passed using
  `g++ -std=c++17 -Wall -Wextra -fsyntax-only`, with `-Iindra/asrenderer` and
  `-include asrrenderthread.h`. The forced include supplies existing portable-test dependencies
  normally included under `ASR_HAS_DEVICE`; no object files or binaries were generated.
- One existing portable-path warning: `seconds` is set but unused when there is no device.
- Added regression checks for aligned free-tail collapse, in-flight chunk retention/destruction,
  stable slot reuse, delayed table shrinking and small allocations after shrinking.
  These checks have not been executed; no test build was performed.
- Vulkan/Metal backend and viewer changes have not been compiled or runtime-tested here.

For the user's next runtime check: compare mesh outfit arrival before any system-body work,
turn away/back to exercise parking, then leave a crowd. Driver usage and actual chunk/table
bytes should fall after retirement. Check complete avatars, stable lips/textures, absence of
GPU validation errors, and FPS at the same camera. Source inspection does not establish those
runtime results.
