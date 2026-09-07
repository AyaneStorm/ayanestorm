# Exact OIT: ~12.5ms/frame of fixed cost with zero transparency (fence wait + capture zone)

Author: chanayane@firestorm. Date: 2026-09-03.
Companion to `ayanestorm-oit-performance-audit-plan.md` (item E4) and
`ayanestorm-oit-e4-fence-stall-question.md` (the original fence-stall fix).
Not predicted by the plan; found while re-measuring after E4, E7, E9, E11
were all implemented and committed.

## Symptom

With E4 (non-blocking readback), E7 (wave-level capture atomics), E9
(compute-sort removal) and E11 (small CPU cleanups) all in, a scene with
**zero transparent geometry on screen** (looking at a plain wall) still
shows a real FPS gap:

- Vanilla transparency: 45 FPS.
- Exact OIT enabled: 35 FPS.

That is a ~22% frame-time increase with nothing for Exact OIT's capture pass
to actually do. E4's own "Verify" section expected this gap to close
("FPS in the almost-no-transparency scene approaches Standard mode"); it has
not, even after E7/E9/E11.

A Tracy capture in this scene (`03092026_001.tracy`, ~200 frames) shows
**two** separate, comparably large fixed costs — not one. CPU zones, via
`tracy-csvexport.exe -f "Exact OIT" <trace>.tracy`:

```
name,total_ns,counts,mean_ns,min_ns,max_ns,std_ns
Exact OIT fence wait,1463650131,201,7281841,5527940,10038369,820114.78
Exact OIT mapped readback read,43002,201,213,71,772,100.29
```

GPU zones, read from the live Tracy UI's Statistics panel (this
`tracy-csvexport` version does not export GPU-timeline zones — confirmed by
an unfiltered CPU-only export of the same trace):

| Zone | Total (200 calls) | Mean (MTPC) |
|---|---|---|
| Exact OIT capture | 1.06 s (9.41%) | **5.28 ms** |
| Exact OIT composite | 29.81 ms (0.27%) | 149.05 µs |
| Exact OIT final blend | 18.11 ms (0.16%) | 90.54 µs |
| Exact OIT speculative sort pass | 14.56 ms (0.13%) | 72.78 µs |
| Exact OIT opaque copy | 11.14 ms (0.10%) | 55.69 µs |

So: `Exact OIT fence wait` (CPU, mean 7.28ms) and `Exact OIT capture` (GPU,
mean 5.28ms) are each individually large and roughly the same order of
magnitude; everything else Exact OIT does per frame (composite, blend,
speculative sort pass, opaque copy) is under 150µs and negligible by
comparison. Combined, fence wait + capture zone account for **~12.5ms of a
~22ms-longer frame** (35 vs 45 FPS ⟹ 22.2ms vs 28.6ms per frame — a 6.4ms
gap, smaller than the 12.5ms sum, which likely reflects the two zones
partially overlapping CPU/GPU work rather than adding linearly; still, they
are clearly where nearly all of the fixed cost lives).

**This changes which item is actually worth pursuing.** E10c (eliminate the
opaque copy via `ARB_texture_barrier`) targets the smallest zone in the
table (55.69 µs mean) — implementing it would buy almost nothing measured
against this trace. The two zones that matter are the CPU fence wait
(architectural — see Diagnosis below) and the GPU `Exact OIT capture` zone.

### GPU capture zone, broken down (2026-09-03, after adding sub-zones)

`renderPostDeferredCapture()` was instrumented with nested
`LL_PROFILE_GPU_ZONE` markers around each distinct piece of work, then
re-measured in the same wall-only scene. One frame example (`Exact OIT
capture` total 4.67ms that frame — same order of magnitude as the 5.28ms
mean above, sampled from a different capture):

```
Exact OIT capture                          4.67 ms   (outer, 100%)
  Exact OIT capture buffer prep            92.16 µs  (~2.0%)
  Exact OIT capture traversal               8.19 µs  (~0.2%)
    Exact OIT capture traversal (rigged)    1.02 µs
    Exact OIT capture traversal (non-rigged) 5.12 µs
  Exact OIT capture readback copy + fence  4.57 ms   (~97.9%)
```

Buffer prep (`glClearTexImage` clears, after E11) and traversal (both
`pool.forwardRender()` calls, which correctly do almost nothing when there
is no alpha geometry) are both negligible, as expected. **The entire GPU
cost is the last block** — `glMemoryBarrier` + a 16-byte
`glCopyBufferSubData` + `glDeleteSync`/`glFenceSync`:

```cpp
LL_PROFILE_GPU_ZONE("Exact OIT capture readback copy + fence");
if (sResources.readbackMapped)
{
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_COPY_READ_BUFFER, sResources.control);
    glBindBuffer(GL_COPY_WRITE_BUFFER, sResources.readback);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, 4 * sizeof(U32));
    ...
}
if (sResources.captureFence) { glDeleteSync(sResources.captureFence); }
sResources.captureFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
```

A barrier and a 16-byte copy cannot plausibly cost 4.5ms of actual GPU
execution. **This is the same architectural mechanism as the CPU-side
`Exact OIT fence wait` zone, just visible from the other side of the same
fence**: a `LL_PROFILE_GPU_ZONE` measures GPU-timeline time between two
inserted timer queries, and `glFenceSync`'s query can only be satisfied once
everything queued on the GPU timeline *before* it — the entire shadow pass,
opaque geometry, lighting, etc., per the Diagnosis section below — has
retired. So the GPU zone isn't attributing 4.5ms to the barrier/copy/fence
calls themselves; it's attributing to them the GPU-side wait for the rest of
the frame's backlog to drain past that point in the command stream, for the
same reason the CPU-side wait is large.

**Conclusion: there is only one fixed cost here, not two.** The CPU fence
wait and the GPU capture-zone cost are two measurements of the same
underlying issue (a synchronous fence placed after a GPU-heavy pipeline),
not independent costs that both need separate fixes. Splitting the GPU zone
further (e.g. isolating `glFenceSync` from `glCopyBufferSubData`) would not
find a new, actionable sub-cost — the mechanism is already understood via
the CPU-side Diagnosis below, and the only real fix is the frame-boundary
restructuring already described in "What would actually close this gap."

## Diagnosis

`"Exact OIT fence wait"` is a CPU zone (`LL_PROFILE_ZONE_NAMED`), not a GPU
zone: it measures how long the CPU thread blocks inside `glClientWaitSync`
waiting for `sResources.captureFence` to signal. The fence is created in
`renderPostDeferredCapture()` right after the frame's capture draws are
issued (E4's design), so on paper it should be nearly free to wait on when
capture did nothing: no transparent fragments means the capture draws
themselves retire almost instantly.

But `glClientWaitSync` on `GL_SYNC_GPU_COMMANDS_COMPLETE` does not just wait
for the fenced commands — the driver only signals the fence once the GPU's
command stream reaches that point, which means *everything queued on the GPU
timeline before the fence* must also have retired. In a normal frame, the
capture pass runs after the whole deferred-rendering pipeline (opaque
geometry, lighting, shadows, atmospherics, etc.), so the fence sits behind
all of that GPU work regardless of whether capture itself did anything.

In vanilla (non-Exact-OIT) rendering, the CPU never blocks like this: it
keeps issuing next-frame draw calls while the GPU works through the current
frame's backlog, and the two stay pipelined across frame boundaries (typically
2-3 frames of overlap, depending on the driver's swap chain). Exact OIT's
fence wait forces a synchronous CPU/GPU rendezvous once per frame, at the
point capture finishes — which throws away that overlap for every frame Exact
OIT is active, independent of how much (or how little) transparent content
is on screen.

This is consistent with the measured 7.28ms mean: it is not the cost of
*capturing nothing*, it is the cost of *the CPU catching up to wherever the
GPU already was* at that point in the frame, which on this machine (RTX
3080 Ti, i9-11900K) is apparently non-trivial even in a geometrically simple
scene, likely because the deferred pipeline (shadows, reflection probes,
volumetric lighting, etc.) keeps the GPU meaningfully busy before Exact
OIT's own draws ever run.

**Confirmed directly in the Tracy timeline view** (not just aggregate
statistics): in a full-frame capture, the GPU row (`Render`) shows
`generateSunShadow` → multiple `renderShadow` cascades (`shadow alpha`,
`shadow simple`, `shadow alpha material`, `shadow alpha grass`, each its own
sub-pass) → `renderGeomPostDeferred` → `renderDeferredLighting` (this is
where `Exact OIT capture`'s GPU work is nested) → more
`renderGeomPostDeferred` → `render_ui` → `swap`. On the CPU row (`Main
thread`), `Exact OIT fence wait` appears at the point corresponding to
`LLPipeline::renderDeferredLighting` — i.e. chronologically *after* the CPU
has already issued the entire shadow pass and scene traversal
(`updateCull`/`stateSort`/octree `PartiallyIn`/`traverse` calls, all visible
as their own substantial CPU cost earlier in the same frame) to the GPU. By
the time the fence wait is reached, that backlog is already queued on the
GPU timeline; `glClientWaitSync` has no choice but to wait for the driver to
work through it, which is exactly why the wait is large even when Exact
OIT's own capture draws have nothing to do. This is not Exact OIT's own
work being slow — it is Exact OIT being the first synchronous CPU/GPU
rendezvous point that lands after a GPU-heavy shadow pass vanilla rendering
would otherwise leave fully overlapped with the next frame's CPU work.

This same mechanism explains the GPU-side `Exact OIT capture` zone's ~5ms
mean too, now that it has been broken into sub-zones (see above): almost
all of it lands on the `glFenceSync` call, for the identical reason — the
GPU-timeline query behind the zone can't close out until the queued backlog
ahead of the fence has drained. CPU wait and GPU zone attribution are two
symptoms of the same synchronous-fence placement, not two separate costs.

## Why E4 didn't already fix this

E4's stated goal was "the CPU only waits for the *capture* to finish, while
the GPU already works on sort pass 1" — true, and confirmed working (the
fence wait dropped from ~210ms, when the control SSBO was wrongly host-mapped,
to a healthy figure once the mapped-buffer bug was corrected). But "the CPU
only waits for the capture" was never quite accurate: it waits for the
capture *and everything queued ahead of it*, which in the general case is the
whole rest of the frame's GPU work. E4 removed the pathological cost (a fence
wait inflated by atomics running at PCIe latency against a host-mapped
buffer); it did not remove the *architectural* cost of introducing a
synchronous wait point into an otherwise fully pipelined renderer.

## Why this wasn't caught earlier in testing

The user's original Phase 0 measurement (before E4) already found this same
10 FPS gap in a no-transparency scene (31 vs 40 FPS) and it was correctly
attributed to the synchronous `glGetBufferSubData` readback that existed
then. E4 replaced that readback with the fence + mapped-buffer scheme and
was verified to fix the *sprite-heavy* regression (the original ~1 FPS
disaster) and to bring fence-wait time down from ~210ms to a healthy number
in that test. The no-transparency scene was not re-measured at that time
(the "wait ~10ms vs ~210ms" comparison the plan cites was for a sprite
scene, not a wall). The gap in the no-transparency case survived E4 for a
different reason (queued backlog, not host-mapped-atomics latency) that only
became visible now that E7 removed the sprite-side atomic contention and
made this residual, always-present cost proportionally more visible.

## What would actually close this gap

Overlapping the wait across frame boundaries rather than blocking within the
same frame: e.g. waiting on *last* frame's capture fence at the start of
*this* frame (giving the GPU a full frame to catch up before the CPU asks),
or restructuring so Exact OIT's capture pass runs early enough in the frame
that little GPU backlog has accumulated ahead of its fence. Both are
non-trivial restructuring of the render-frame ordering, well beyond a
"small cleanup" — not something to attempt inside the current phase without
deliberate scoping, since a naive one-frame-behind fence risks compositing
stale capture data if not handled carefully (composite must read the SAME
frame's capture, not last frame's).

This is not in the plan as written. Recorded here as a known, real,
architectural cost of the fence-based capture design, to revisit once more
of the plan is implemented and there is a clearer picture of where the
remaining budget should go. No fix attempted yet; this is a findings note,
not a completed item.

## Verify (if revisited)

Same repro: wall-only scene, Tracy capture, `"Exact OIT fence wait"` CPU
zone and `"Exact OIT capture"` GPU zone durations, plus overall FPS with
Exact OIT on vs off. Target: both zones drop to a small fraction of their
current means (7.28ms CPU, 5.28ms GPU), and the on/off FPS gap in this scene
shrinks substantially (not necessarily to zero — the smaller zones,
composite/blend/speculative-sort/opaque-copy, still cost a little,
~150-300µs combined per the table above).

**Do not spend time on E10c for this specific gap** — the opaque copy it
targets measured 55.69 µs mean, under 0.5% of the fence-wait + capture-zone
total. E10c may still be worth doing for its own stated goal (VRAM/bandwidth
at scale), just not as a fix for this finding.

Precise numbers without opening the Tracy UI: `E:\dev\AyaneStorm\tracy\tracy-csvexport.exe -f "Exact OIT" <trace>.tracy`
gives per-zone mean/min/max/count for every Exact OIT CPU zone across the
whole capture in one shot (this is how the CPU numbers above were produced).
It does not export GPU-timeline zones (`LL_PROFILE_GPU_ZONE`) in this Tracy
version; those were read from the live Tracy UI's Statistics panel (toggle
"GPU" instead of "Instrumentation", filter by "OIT").
