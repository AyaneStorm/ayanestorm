# Renderer performance follow-up: crowd scaling, freshness, and memory

Date: 2026-11-10  
Author: chanayane@firestorm

## Scope and confidence

This is a source and architecture audit of this project, including its renderer requirements, design, status history, avatar handoff, and retained crowd-performance analysis. No source or documentation outside this project was used. No application build, new GPU capture, or runtime benchmark was performed. Historical timings below are observations recorded in the project's documentation, not measurements of the current checkout made for this report.

The report deliberately describes algorithms and data flow without source filenames, line numbers, application names, or implementation-location references. Statements marked **confirmed** describe behavior visible in the current source. **Recorded** means a historical runtime observation. **Proposal** and **risk** distinguish recommendations and deductions from measured bottlenecks.

An independently verified performance comparison against another implementation is outside this evidence scope. The relevant comparison is with the work-reduction principles described by the earlier crowd analysis retained in this project. Those principles are useful, but their numerical results do not establish a performance target for this renderer under different quality requirements.

## 1. Diagnosis

The renderer has successfully reduced draw-submission overhead, but it still admits too much recurring work before determining whether that work contributes to the image. Moving work to compute does not bound its amount. A crowd can therefore be expensive despite a small number of indirect draws and a fast static-world path.

There are four separate scaling problems:

1. **Deformation demand:** every admitted skinned face owns output storage, and in-view bindings deform their selected geometry every render frame, including much geometry later rejected by occlusion.
2. **GPU scheduling:** several passes dispatch over historical allocation extents rather than compact sets of active work. The same block allocation drives vertex, cluster, and group phases despite their different workloads.
3. **CPU state production:** animation remains in a serial active-object update loop, followed by additional skeleton extraction, spatial maintenance, and body-state polling. Pose throttling reduces some evaluation, but not all downstream work.
4. **Memory and streaming:** derived geometry, oversized lists, replacement overlap, and unrestricted required-buffer reservations compete with textures. A successful allocation is not proof of a sustainable working set.

The current full-rate-avatar count is an **animation-rate control**. It is not a limit on full-geometry avatars, skinned vertices, transparent layers, or deformation memory. Treating it as an equivalent crowd-rendering budget would misdiagnose the implementation.

The earlier crowd strategy gets substantial savings by reducing the number of detailed avatars rendered repeatedly and coordinating animation work with that representation. This design explicitly retains real geometry for every visible avatar. Its workload is consequently different. Equivalent performance cannot be assumed from adopting the same graphics API, a similar nearest-avatar count, or GPU skinning.

## 2. What the recorded timings actually establish

| Recorded situation | Observation | Valid conclusion |
|---|---|---|
| About 79 avatars after correcting an erroneous LOD cap | Roughly 20 FPS; GPU 48–54 ms; cluster deformation 31–34 ms, vertex deformation 8–9 ms, palette work 3–5 ms | Deformation dominated that run. Submission and body morphs were not the primary cause. |
| Earlier apparent crowd success | About 150 FPS while crude lowest LODs were incorrectly selected up close | This is not a valid full-quality baseline. It demonstrates sensitivity to admitted geometry. |
| Frustum rejection added before skinning | Busy-place rate reported around 30 FPS | Avoiding offscreen deformation helped, but did not bound visible or occluded crowd work. |
| Cluster bounds changed from skinned-triangle reduction to joint-derived bounds | GPU 14–19 ms; cluster phase about 1.1 ms; about 45 FPS; active-object CPU work 8–11 ms | The former cluster algorithm was a real bottleneck and has already been replaced. The next bottleneck shifted. |
| Earlier crowded memory run | Geometry about 3.3 GB, textures about 4.4 GB, discard bias 4, visible texture cycling | Derived geometry and texture demand interacted badly. Later policy changes require a fresh assessment. |

These runs are not a controlled series with identical geometry, camera, residency, and animation state. Do not subtract their timings to claim an isolated optimization gain. Do not describe the old triangle-reduction cluster algorithm as the current implementation: current bounds use joint transforms.

At 14–19 ms of GPU work, a 60 FPS floor is already marginal or exceeded before adding the remaining lighting, shadows, reflections, and effects. Improving CPU animation alone cannot eliminate a 19 ms GPU critical path. Conversely, a fast GPU cannot make a serial state producer deliver low-latency animation updates.

## 3. Preserve the improvements that are already present

The implementation already has persistent scene storage, worker-built geometry, bindless textures, GPU LOD choice, hierarchical cluster culling, two-phase depth-pyramid rejection, shared palettes per skeleton/skin pair, GPU attachment placement, GPU morph results updated from changed inputs, and texture demand feedback.

It also already rejects skinned bindings outside a conservative frustum sphere and computes animated cluster bounds from joint influence metadata rather than scanning their deformed triangles. Attachment movement generally avoids rewriting every face when only its wearer moved. Render-thread camera prediction and raw mouse following exist; claiming the camera always waits for the main loop would be outdated.

These are the right foundations. The corrective work should change eligibility, scheduling, invalidation, and memory ownership around them. Replacing the whole renderer or abandoning indirect drawing is not justified by the available evidence.

## 4. Skin only the geometry that a current consumer requires

### 4.1 Occlusion currently arrives after most deformation cost

**Confirmed:** the graph performs joint prediction, palette generation, binding placement, morph work, vertex deformation, cluster bounds, and group bounds before ordinary scene occlusion culling. The pre-skin binding test is frustum-based. It does not establish that the binding survives occlusion.

An avatar behind an opaque wall can therefore avoid its final draw yet still pay for palette entries, vertex deformation, and bounds generation. Fine cluster rejection saves raster and shading work after much of the avatar cost has already happened. This matters especially in crowded interiors with many attachments and layered bodies.

**Proposal:** separate determining visibility from producing deformed vertices:

1. Predict required joint transforms.
2. Update conservative owner/binding bounds and choose a resident LOD.
3. Generate inexpensive group or cluster bounds from joint metadata.
4. Classify deformation demand against current consumers.
5. Compact required vertex work and deform it.
6. Draw using the resulting positions.

The recent joint-based bounds are valuable because bounds no longer mathematically require a prior full vertex-deformation pass. The current graph still orders those passes as though they did.

This is an architectural opportunity, not a claim that moving two existing passes automatically works. Current geometry descriptors, group reductions, audit reads, and synchronization must be separated into bounds-only and position-consuming dependencies.

### 4.2 Previous-frame occlusion needs a same-frame recovery path

A previous-frame depth pyramid can identify candidates, but cannot safely suppress their deformation for the entire current frame. Camera motion, an opening door, or a moving avatar can invalidate the decision.

Use a two-stage design analogous to the existing visibility solution: draw reliable occluders first, build current depth, retest uncertain animated bounds, then skin and draw newly exposed geometry in the same frame. Newly resident content and invalid bounds must take a conservative path. If a candidate cannot be proved hidden, it must remain eligible.

Measure whether avoided deformation exceeds the additional bounds, compaction, depth, and synchronization cost. The existing scene culling is already two-phase; introducing another complete unrelated culling system would duplicate work. Extend the same classification results and make their dependencies explicit.

### 4.3 A main-camera test is not a future universal visibility test

The current renderer has not yet implemented the full shadow and reflection workload. A main-frustum skinning shortcut cannot be copied unchanged into those features: an avatar outside the main camera can cast a visible shadow or appear in a mirror.

Represent deformation demand as the union of active consumers. A useful key includes owner, geometry, LOD, pose/morph generation, and evaluation time. Share the result when consumers need the same deformation. If views require different LODs or capture times, record that distinction explicitly rather than either recomputing everything or sharing an incorrect result.

At binding granularity, offscreen rejection is already present. The next meaningful gain is demand reduction inside that broad eligibility region, particularly opaque occlusion and unused layers. Avoid spending a development cycle merely reimplementing the existing frustum shortcut.

## 5. Replace allocation-sized dispatches with active work queues

### 5.1 Historical high-water marks are not active counts

**Confirmed:** joint and palette dispatch sizes use allocator high-water extents. Binding dispatch uses the size of the binding container. Vertex, cluster, and group dispatches derive from the high-water extent of the skin-block allocator. Released ranges are reusable, but returning a range does not lower that allocator's historical bump end.

This creates a recurring cost that can depend on where the application has been. After a large crowd leaves, a smaller scene can still launch work over many free entries. Shader early returns reduce useful execution but do not remove scheduling, buffer reads, and dispatch overhead.

**Proposal:** retain stable resource handles while maintaining dense execution lists. Stable identity and compact execution order are separate requirements. Use generation-tagged handles for lifetime safety, and GPU/CPU-maintained active lists for joints, palette jobs, bindings, and deformation blocks. Retire old mappings only after in-flight consumers complete.

Record high-water entries, live entries, admitted entries, and actual executed items independently. A high historical/live ratio should be visible as a diagnostic rather than hidden inside one aggregate pass time.

### 5.2 One block count is used for three different algorithms

**Confirmed:** a binding reserves blocks based on the maximum of its vertex-block count and cluster-block count. Both vertex and cluster phases launch over this common block space. The group phase also launches through it, but only block zero of each binding performs useful group processing.

For a vertex-heavy mesh, most group workgroups immediately return. Cluster work can likewise be dominated by empty blocks. Selecting a coarser LOD reduces useful vertex work but does not proportionally reduce the preallocated dispatch extent.

**Proposal:** generate separate indirect workloads:

- Vertex tasks sized from the actually chosen LOD's vertices.
- Cluster tasks sized from that LOD's clusters.
- Group tasks sized from its groups, or one task per binding when that is demonstrably efficient.
- Palette jobs restricted to required skeleton/skin pairs.

Build those lists after demand classification, using prefix sums or bounded append queues. Capacity and overflow behavior must preserve correctness. Compare the queue construction cost with the empty-work cost; a small scene may benefit from a direct-dispatch fast path.

### 5.3 Joint-derived cluster bounds still contain redundant work

**Confirmed:** each cluster visits its joint list twice, transforms its rest-sphere center, and computes conservative stretch from palette matrices. Multiple clusters commonly use the same palette entries. Each cluster also atomically expands a shared geometry radius. Group spheres are rebuilt from cluster spheres afterward.

The replacement algorithm is already much cheaper than its predecessor, so optimize it according to fresh timings. Candidates are cached per-palette stretch metadata, a reduction for per-binding aggregate radius, and group-level bounds derived directly from suitable joint metadata. Precompute a transform property once per palette evaluation when many clusters consume it.

Bound tightness matters: a cheap loose bound can admit enough extra geometry to cost more overall. Compare rejected clusters, deformed vertices, and final raster work, not only the bounds-pass timer. Preserve the treatment of morph displacement, nonuniform scale, weight normalization error, and conservative rounding.

## 6. The compute cache needs a measured cost model

### 6.1 Once per frame can still be too much

**Confirmed:** each skinned binding owns a derived position range at 16 bytes per reserved vertex plus derived cluster/group entries. Capacity covers the largest valid LOD reachable from its selected source. The host now caps that source using its chosen rigged LOD, so describing every binding as always reserving the absolute finest asset LOD would be inaccurate. There is still per-binding storage and growth/rebinding cost when that source changes.

Compute deformation writes positions that later rendering reads again. This can be excellent when several passes reuse them. In the current incomplete lighting pipeline, fewer consumers exist, so that bandwidth cost is amortized less effectively. A small indirect draw count does not establish that compute deformation is cheaper than vertex-stage deformation for a particular binding.

Use an explicit comparison:

`compute cache cost = demanded deformation + output writes + downstream reads + work construction + synchronization`

`vertex-stage cost = deformation repeated for vertices actually consumed by each pass`

These are decision models, not timings. Measure vertex reuse, pass count, occlusion, visible pixel coverage, and bandwidth before selecting a strategy. A hybrid producer can preserve shared result ownership while choosing compute for multi-consumer meshes and an uncached path where that is cheaper. This would require an explicit architectural decision if the existing shared-result contract is interpreted as mandating a materialized position buffer.

Do not reintroduce lossy deformed-position packing as a quick memory fix. Prior correctness work established the importance of a common origin, exact weight sums, and float positions for shared boundaries. Reduce the number and lifetime of cached vertices first.

### 6.2 Normals are not part of the same deformation cache

**Confirmed:** the stored deformation result primarily contains positions. Opaque material resolve reads the triangle's three source normals and skins them using the palette for each covered pixel. Adjacent pixels can repeat the same triangle-normal deformation work.

This is not necessarily the dominant cost today; the recorded resolve time does not establish that. However, large close-up triangles and additional material inputs can make per-pixel reconstruction expensive.

Compare skinning and packing normals/tangents with the vertex output against the current reconstruction path. Caching adds writes and residency; reconstruction saves attributes for tiny or hidden triangles. Preserve per-vertex normalization, interpolation order, morph normals, and tangent handedness. A position audit alone cannot validate this change. The existing decision to postpone a normal audit should not be mistaken for evidence that normals have already passed one.

## 7. Animation throttling currently trades freshness for CPU time

### 7.1 Frame counts create an unstable time policy

**Confirmed:** the nearest group receives full-rate animation while other skeletons use periods of 8, 16, 32, or 64 main-loop frames, with a shorter special period for moving seats. Far skeletons use interpolation from a previous sample toward the newest sample, effectively introducing a sample interval of delay during steady updates.

At 45 main loops per second, 64 frames are about 1.42 seconds. At 10 loops per second, they are 6.4 seconds. A busy main thread therefore makes remote motion much staler exactly when the crowd is already expensive. Render-rate interpolation can make the transitions continuous while still showing old motion. Smoothness and freshness are different measurements.

The interpolation gap allowance is derived from the current loop interval multiplied by the frame period. Under uneven producer intervals, that is only an estimate of the actual sample spacing. Scheduling the settled-pose resend by unchanged frame count adds another frame-dependent policy.

**Proposal:** use monotonic time deadlines, explicit maximum pose age, and importance-based error tolerances. Stagger deadlines by stable owner identity. Carry sample timestamps and validity horizons through the pipeline. Distinguish root/seat motion, joint pose, facial morphs, attachment-local changes, and animation events: they do not need identical update rates.

Maintain animation start/stop and protocol events even when expensive pose evaluation is deferred. Sample current animation time when evaluation resumes rather than replaying a backlog of missed display steps. Moving-seat exceptions should consider inherited parent motion, not just nonzero velocity on the immediate seat object.

### 7.2 The count does not select the nearest visible full-detail group

**Confirmed:** ranking uses skeleton distance with self priority, without a visibility condition in that ranking. Nearby offscreen owners can occupy full-rate places. The same ranking does not lower geometry complexity for the remaining crowd.

If the intended policy is nearest visible avatars, implement that policy explicitly using reliable visibility and hysteresis. Account for mirrors and shadow demand where relevant. Selection churn needs stable ties and a minimum residence interval, otherwise camera movement can cause repeated promotion, allocation, and pose spikes.

A geometry budget is a separate decision. It should estimate skinned vertices, clusters, material faces, transparency coverage, and texture demand. An avatar with many elaborate attachments cannot be assigned the same cost as a simple body solely because both count as one owner.

### 7.3 Strict freshness requires more than sampling less frequently

The documented design originally requires current visible animation and rejects stale image reuse. The recent interpolation-based throttling is a deliberate tradeoff relative to that ambition, even though every frame still rasterizes geometry.

For a strict high-freshness mode, parallelize pose evaluation or move suitable evaluation work off the producer's critical path. Use immutable animation assets, per-owner evaluation state, and explicit publication of completed poses. Audit shared caches and callbacks before running existing character methods concurrently. Blindly parallelizing the active-object loop is unsafe.

If results are joined within the main loop, measure the slowest-job barrier: parallel work can still stall publication. Prefer bounded completion and reuse of a still-valid pose when allowed, with separate handling for self, interactions, seats, appearance changes, and animation resets. Any relaxed freshness mode should expose its maximum latency as part of its quality contract.

## 8. CPU maintenance still scales with skeletons and objects

**Confirmed:** every avatar-maintenance pass discovers live characters, visits retained skeletons, updates CPU avatar extents and spatial placement, extracts joint matrices, and updates system-body state. Joint extraction allocates a temporary matrix vector and compares the full result to its previous value before deciding whether to publish it.

Consequently, skipped animation evaluation does not skip skeleton extraction. The system first reconstructs all joint matrices to discover that they did not change. Body morph polling similarly constructs temporary weight arrays and checks them, including morphs not currently used by shown parts.

**Proposal:** make publication depend on explicit generations:

- Pose generation from completed animation evaluation.
- Root transform generation, independent of relative joint pose.
- Appearance/rest-skeleton generation.
- Attachment membership and local-transform generations.
- Morph-weight and mask generations.
- Bounds and LOD-policy generations.

Reuse per-owner scratch storage. Resolve joint pointers when skeleton topology changes. Maintain membership incrementally instead of constructing a new alive set each loop where lifecycle notifications can provide the same safety. Retain a staggered recovery sweep for missed notifications.

Refresh spatial structures only when the relevant bounds or placement change. Preserve correct picking and object-cache behavior: earlier interaction regressions show that removing inherited maintenance without replacing its consumers is a correctness failure.

Rigid attachment placement is already shared on the GPU. Further CPU savings should target redundant host attachment traversal, dirty propagation, LOD scheduling, and special-case transforms. For ordinary moving linksets, shared root/mover transforms could also avoid recursively rewriting every child face. Instrument these costs before attributing the entire active-object timer to animation.

The renderer's individual maintenance slices already have several fixed time limits. Their sum, plus unbounded phases and ordinary host work, can still exceed the desired producer interval. Give the producer one explicit scheduling budget with priority for input and pose publication. Monitor backlog age and starvation; a short budget that indefinitely postpones a newly visible asset is not success.

## 9. Scene command ordering can hold fresh poses behind bulk work

**Confirmed:** scene commands are transferred as batches. A batch's instance updates are consumed under an upload allowance. The next batch is not taken until those instance commands finish. Skeleton, material, and other commands in later batches therefore cannot overtake a large older instance batch.

This preserves order but couples animation freshness to loading volume. During arrival or a large geometry change, a later pose can wait even though the render loop itself keeps running. Command production appends updates; there is no general latest-state coalescing for repeated sets of the same resource.

**Proposal:** separate ordered lifecycle events from replaceable state:

1. Preserve create/destroy, dependency, and generation ordering.
2. Coalesce repeated transform, material, and pose state where semantics permit it.
3. Keep the recent pose samples required by prediction/interpolation, not every obsolete pose and not only a timestamp-free latest matrix.
4. Give camera, pose, and interaction state independent admission priority over bulk instance creation.
5. Admit bulk assets under bounded bytes and CPU time without holding unrelated live-state channels behind them.

Resource generations and publication epochs must prevent a pose referencing a skeleton not yet created, a material referencing an unavailable image, or an ID being reused before earlier readers retire. Track oldest pending command age, state samples superseded, and dependency waits by category.

The geometry upload queue also stops at a front item that cannot upload. This is a potential head-of-line blocker for smaller ready items. A bounded scheduler can select other independent work while respecting dependencies and preserving progress for large assets. Verify whether this occurs in real loading traces before implementing a more complex policy.

## 10. Memory accounting must become memory admission

### 10.1 Required allocations bypass the budget

**Confirmed:** required scene tables and lists can fall back to an unconditional reservation when ordinary reservation fails. This records the bytes but does not enforce a limit. Their subsequent driver allocation can still fail or push the process toward an unsustainable resident set.

Geometry borrowing is better than crashing at a fixed category share, but unrestricted required allocation is not a completed solution. In addition, the texture limit calculation retains a fixed minimum share even when nontexture usage has consumed enough memory that this share no longer fits the global budget with the desired reserve.

**Proposal:** enforce one physical working-set constraint across live data, staging, replacement overlap, and retired resources. Clamp texture entitlement to actual remaining room. If mandatory nontexture state cannot fit, stop admitting additional detailed content and use the explicitly permitted fallback policy. Category targets may be elastic; the global bound must remain real.

### 10.2 Growth and mutually exclusive LODs inflate capacity

**Confirmed:** table/range capacities grow geometrically, backend table replacement allocates a new buffer while retaining the old one for in-flight frames, and old content is copied during growth. Large table growth therefore combines a memory peak, a copy, and clearing of replacement storage on a render-frame path.

Draw-list accounting reserves capacity for each LOD in an instance's source chain even though only one draws at a time. Power-of-two rounding adds further slack. Safe sizing can instead sum the per-instance maximum over selectable LODs for each independent bucket, while accounting for all early/late/group/retest uses and publication transitions. A single global maximum across buckets is insufficient because each bucket has its own capacity.

Use measured live demand and bounded replacement headroom. Consider paged or segmented tables with stable addressing, or planned background migration, where full replacement is too expensive. Distinguish live bytes, capacity slack, fragmented free ranges, pending retirement, and migration overlap in diagnostics. Reclaiming historical allocation space should not require another large stall.

### 10.3 Size deformation residency by demand and amortize LOD changes

Per-binding outputs remain allocated regardless of whether they contribute to the current image. An owner who is out of all relevant views need not retain the same derived cache as an immediately visible owner, provided regeneration can occur before the next required draw.

Keep immutable source geometry shared. Share derived deformation when geometry, skeleton, skin, morph state, LOD, and time genuinely match; a material difference alone should not require another copy of identical positions. Do not assume different material faces necessarily share source vertices—identify valid reuse from actual vertex domains.

Use LOD hysteresis and delayed shrinking to avoid allocate/retire cycles near thresholds. A replacement must remain correct while uploads complete. The earlier coarse-LOD regression is evidence that intentionally wrong LOD selection can look like a dramatic performance improvement; visual parity must accompany every memory optimization.

### 10.4 Texture stability depends on geometry stability

GPU texture feedback and stable texture slots already exist. Their presence does not prevent geometry growth from repeatedly reducing texture headroom. A reserve-percentage adjustment treats a symptom if derived geometry continues growing without admission control.

Use hysteresis for shrinking and promotion, prioritize absent visible images before sharper replacements, and cancel obsolete demand. Preserve offscreen predictive demand and independent snapshot demand: current onscreen feedback alone cannot describe a camera turn or a high-resolution capture. Measure decode, staging, image replacement, and retirement peaks together.

This renderer's compression/mip-pressure phase is still incomplete according to project status. Do not claim that the intended pressure-management design has already been delivered merely because the budget interfaces exist.

## 11. Quality constraints determine which crowd strategy is possible

There are two different routes to the earlier crowd strategy's reduced recurring work.

**Quality-preserving route within the current real-geometry requirement:** reject hidden work before deformation; compact dispatches; share actual duplicate results; publish poses incrementally; parallelize animation safely; remove inherited host maintenance; manage residency and replacement peaks; and use the existing correct authored LOD policy efficiently. These should be the first implementation priorities.

**Additional policy route:** allocate a limited detailed-avatar budget, use coarser geometry outside it, reduce animation sampling by bounded time, and optionally use cached avatar representations at sufficient distance. These can provide larger scaling changes, but they change quality or freshness. They must be treated as explicit policy choices, not silent performance fixes.

Cached avatar representations are currently excluded by the design's real-render rule. This report does not authorize adding them. If that constraint is later relaxed, representation, pose scheduling, attachment suppression, texture demand, refresh admission, and overload fallback must be designed together. Merely drawing a cached card while leaving all full-geometry animation and skinning active would miss much of the benefit.

No engine can guarantee an arbitrary fixed frame-time bound for unbounded visible user-generated geometry at unchanged fidelity. A performance requirement needs either a bounded accepted workload or an explicit overload policy. A fixed owner count alone does not bound triangles, overlapping transparent surfaces, texture bytes, or attachment count.

## 12. Verification that will distinguish improvements from apparent gains

### Required observations

Record render/present rate, CPU publication rate, input-to-present age, per-owner pose sample age, and visual interpolation delay separately. A fresh camera does not prove a fresh world; a newly consumed snapshot does not prove that its poses were evaluated recently.

Collect distributions, including median, p95, p99, and worst spikes, for:

- CPU animation evaluation, skeleton extraction, bounds/spatial updates, body polling, dirty propagation, and scene application.
- GPU pose, palette, binding, morph, vertex, cluster, group, culling, resolve, and transparency work.
- Live/high-water/admitted joints, palettes, bindings, vertices, clusters, groups, and dispatched workgroups.
- Frustum rejection, opaque-occlusion rejection before skinning, and geometry rejected only after deformation.
- Command age and bytes pending by category; buffer-growth copy/clear bytes and retirement peaks.
- Texture misses, residency changes, promotion/demotion cycles, and global working-set headroom.

Pass timestamps identify candidate bottlenecks; they should be interpreted alongside GPU queue waits and CPU submission intervals. Report time in milliseconds as well as FPS. Keep expensive whole-scene diagnostic walks off latency-sensitive paths or time-budget them; otherwise the measurement code can create periodic hitches.

### Representative comparisons

Use the same resolution, camera, resident assets, avatar outfits, selected LODs, transparency, and animation state. Include a quiet world, dense opaque world, alpha-heavy crowd, a crowd mostly behind opaque walls, a large visible crowd, rapid camera turns, arrival/teleport, and a long session after a large crowd has left.

The last case is essential for detecting high-water dispatch and residency problems. A synthetic city with simple moving objects is insufficient to validate crowd skinning. Add representative many-face rigs with realistic joint counts, layered clothing, varied LOD vertex counts, and streaming bursts.

Useful isolated experiments are compact versus historical-range dispatch, bounds-before-skinning versus current order, CPU pose evaluation versus extraction/publication, and compute-cache versus direct deformation for selected workloads. Disabling a pass is only a diagnostic upper bound on its removable cost; it is not a valid final benchmark if it changes the image.

Preserve position and visibility audits, and add targeted normal/tangent checks when those representations change. Test disocclusion, camera cuts, moving seats, appearance changes, attachment edits, LOD transitions, resource-ID reuse, and in-flight retirement. Exercise both native backends; shader portability does not establish identical bandwidth or scheduling behavior.

## 13. Recommended implementation order

| Priority | Change | Acceptance evidence |
|---|---|---|
| 1 | Establish a current, quality-correct crowd baseline and per-owner freshness data | Timings tied to actual selected geometry; historical coarse-LOD results excluded |
| 2 | Separate vertex, cluster, and group task lists; compact active palette/joint work where justified | Workgroup count follows live admitted work and drops after a crowd leaves |
| 3 | Use generations to skip unchanged CPU pose extraction, body polling, and spatial maintenance | Lower producer cost without stale picking, appearance, or attachment state |
| 4 | Replace frame-count pose policy with time/error bounds and audit parallel evaluation | Pose age stays bounded as main-loop rate falls; event semantics preserved |
| 5 | Decouple fresh state admission from bulk scene batches | Pose publication latency remains stable during arrival and mass instance updates |
| 6 | Reorder joint-derived bounds and introduce conservative pre-deformation demand classification | Occluded crowd deformation falls; same-frame disocclusion remains correct |
| 7 | Enforce the global memory bound; reduce list slack and bound replacement overlap | Stable textures and no uncontrolled required-buffer growth under pressure |
| 8 | Compare deformation-cache and attribute strategies using actual consumer counts | Lower total GPU time and residency at the same rendered result |
| 9 | Add further lighting and effects with explicit deformation-demand reuse | Multi-view costs remain visible and do not restore duplicated or hidden work |

The most actionable next change is the separation of deformation task sizes: current group and cluster scheduling demonstrably inherits vertex-sized historical allocations. In parallel conceptually, the most important correctness-of-performance change is replacing uncontrolled pose latency with an explicit time policy. After those changes are measured, pre-deformation occlusion and strict memory admission address the larger scaling limits. All proposed gains require runtime verification; this audit establishes mechanisms and priorities, not an unmeasured FPS promise.
