# Exact OIT Readback Investigation

Investigation into the mandatory per-frame control-buffer readback in
`FSExactOIT::validateCapture()`, carried out to determine whether it could be
removed or reduced.

**Conclusion: it should not be changed.** The apparent cost is a symptom of being
GPU-bound, not a cost the synchronization creates. Two candidate designs were
evaluated and both rejected on measured evidence.

**Scope limit:** this closes the *readback* question only. It does not establish
that Exact OIT performance cannot be improved substantially -- the GPU side was
never profiled. See "What has NOT been established" below for the prerequisite
measurement and the structural ideas it would inform.

All measurements were taken at 3440x1440 with Exact OIT active.

## The readback under investigation

`validateCapture()` issues a memory barrier and then reads four control words
with `glGetBufferSubData`:

| Word | Purpose |
| --- | --- |
| `control[0]` | captured node count; drives the overflow test and pool growth |
| `control[2]` | overflow flag |
| `control[3]` | maximum per-pixel list length; sets the sort pass count |

The read is synchronous because the result decides between exact compositing and
the same-frame complete vanilla fallback. That decision must happen before the
CPU records the composite or fallback draw calls.

## Measurement 1: the readback appears to cost 9-14 ms per frame

Temporary instrumentation timed the readback call over 120-frame windows.

```
average  9.07 ms, peak 40.2 ms, nodes 9,972,466, max list 144
average 11.45 ms, peak 34.1 ms, nodes   342,360, max list  40
average 10.36 ms, peak 19.4 ms, nodes    59,408, max list  20
average 12.01 ms, peak 37.6 ms, nodes    70,132, max list  19
average  9.78 ms, peak 23.4 ms, nodes   127,205, max list   8
average 14.24 ms, peak 87.3 ms, nodes   197,692, max list  13
average 12.60 ms, peak 40.5 ms, nodes 2,271,066, max list  93
average 14.48 ms, peak 42.6 ms, nodes   229,118, max list  12
```

Against a 16.7 ms budget at 60 fps this looked severe.

The cost is **uncorrelated with the workload**: 9.97M nodes with 144-deep lists
stalls less (9.07 ms) than 59K nodes with 20-deep lists (10.36 ms), a 168x
difference in captured fragments with no effect on stall time. This ruled out the
GPU finishing capture work as the explanation and suggested pipeline drain.

That reading was correct as far as it went, but incomplete. See Measurement 3.

## Rejected design 1: defer the overflow decision by one frame

Composite whatever was captured, detect overflow later through a non-blocking
fence, then grow the pool and correct on a subsequent frame.

**Rejected on design review, before implementation.** It would display a
partially captured list. Worse, in a scene that keeps overflowing while the pool
is still growing, the sequence wrong / vanilla / wrong / vanilla alternates at
frame rate. The frame rendered through the vanilla fallback also looks visibly
different from an Exact OIT frame, so even the "correct" frames in that sequence
are a visual discontinuity.

Trading a guaranteed-correct failure path for a flickering one is the wrong
direction for a renderer whose purpose is exactness.

## Rejected design 2: skip the readback below a usage threshold

Keep the same-frame decision, but skip the readback entirely while node usage
sits far enough below capacity that overflow is arithmetically impossible before
the next check. Re-arm the blocking readback only near the limit. Both states
would be lossless.

This required a bound on how far usage can climb between consecutive frames.
Instrumentation was extended to record consecutive-frame growth:

```
peak 53,405,370 of 66,756,713 (79%), largest single-frame rise 53,222,116 (79% of capacity, 183,254 -> 53,405,370)
peak 33,155,914 of 66,756,713 (49%), largest single-frame rise  1,853,112 ( 2% of capacity)
peak 21,960,225 of 66,756,713 (32%), largest single-frame rise 10,651,313 (15% of capacity)
peak 19,052,692 of 66,756,713 (28%), largest single-frame rise  7,613,096 (11% of capacity)
peak 12,333,688 of 66,756,713 (18%), largest single-frame rise  9,508,479 (14% of capacity)
```

**Rejected on measured evidence.** A single frame moved from 183,254 nodes to
53,405,370 -- a 291x increase between consecutive frames, 79% of capacity in one
step, most likely a teleport arrival or a cam onto a dense scene. Rises of
11-15% appear routinely.

To be safe against a 79% jump the threshold would have to sit near 20% of
capacity, while typical peak usage is 26-49%. The fast path would almost never
engage: all of the complexity, none of the benefit.

### Incidental finding: overflow does occur

Capacity was observed at **66,756,713 nodes**, well above the initial
`width * height * 4` sizing (19,814,400 at this resolution). The pool had already
grown, which means overflow has fired and `captureOverflowed()`'s growth policy
has run.

An earlier session log showed no overflow events, which led to an incorrect
assumption that overflow was unreachable. It is not. The same-frame fallback
guarantee is doing real work, which independently reinforces the rejection of
design 1.

## Measurement 3: the stall is GPU latency, not synchronization overhead

A fence was inserted at the readback point and polled with
`glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 0)` to ask whether the GPU had
already finished when the CPU arrived.

`GL_SYNC_FLUSH_COMMANDS_BIT` matters here: a zero-timeout poll does not flush, so
without it an unsignalled result can mean the commands were never pushed rather
than that the GPU is still busy.

**Result: 0 of 120 frames signalled, in every window sampled.**

The CPU is never ahead of the GPU at this point. The renderer is **GPU-bound**,
and the readback exposes queue latency rather than creating it.

The practical consequence is decisive: removing the synchronization would not
recover 9-14 ms. It would relocate the wait to `SwapBuffers`, because the CPU has
no useful work to do with the reclaimed time -- it is already outrunning the GPU.

The 9-14 ms figure is therefore best understood as a *measurement of how far
behind the GPU is*, not as a recoverable cost.

## Consequences for future optimization work

Because the bottleneck is GPU throughput:

- Removing or deferring the readback is not worth pursuing. Both designs above
  are closed.
- Optimizations must reduce **GPU work**, not CPU stalls.

### What has NOT been established

This investigation measured the CPU side only. **No measurement exists of how GPU
frame time divides between capture, sort, and blend.** Any statement about which
of the ideas below is worthwhile is therefore a hypothesis, not a finding.

That measurement is the prerequisite for all further work here, and it is cheap:
`LL_PROFILE_GPU_ZONE` markers already exist around the relevant stages
(`"Exact OIT capture"`, `"Exact OIT compute classify"`,
`"Exact OIT compute deep merge"`, `"Exact OIT natural sort pass"`,
`"Exact OIT final blend"`, `"Exact OIT opaque copy"`). Timing them with GPU timer
queries over a session in dense content would show where the time actually goes.

Do that before implementing anything below.

### Structural ideas (potentially substantial, unvalidated)

1. **Reject fragments before allocation rather than after capture.**
   A frame was observed allocating 9,972,466 nodes. A significant fraction of
   those very likely cannot affect the final image -- occluded by an opaque
   surface, or sitting behind a fully opaque alpha-blended texel. The existing
   `RenderExactOITOpaqueCutoff` optimization already identifies the latter class,
   but it prunes during the first sort pass, i.e. *after* those nodes have been
   allocated, written, and had their memory traffic paid for.

   Moving that rejection earlier -- into capture -- would reduce node count,
   node-pool bandwidth, and per-pixel sort depth simultaneously. Those three
   costs compound, so this is the idea with the largest plausible upside.

   The obstacle is ordering: a capture shader cannot know a nearer opaque
   fragment exists until it has been captured, and capture order is arbitrary.
   Possible angles: a cheap conservative depth prepass over alpha-blended
   geometry that writes a per-pixel "nearest fully opaque alpha" depth, used to
   reject in `exact_oit_store()`. Correctness requires that the prepass never
   rejects a fragment that could contribute, so it must be conservative in the
   safe direction.

2. **Reduce sort cost at depth.**
   `maximum list 270` was observed, meaning some pixels sort 270 entries. Natural
   merge sort is O(n) on ordered input and O(n log n) otherwise, so deep pixels
   are disproportionately expensive. If GPU profiling shows sort dominating,
   options include sorting a compact `(depth, index)` array and touching full
   32-byte nodes only during the final blend, which would cut the memory traffic
   the comparisons drag through cache.

3. **Question full-resolution capture for all fragments.**
   Every transparent fragment currently gets full-resolution treatment. Whether
   that is necessary for all content -- as opposed to, say, distant or
   low-contrast layers -- has not been examined. This one carries real fidelity
   risk and would need careful framing to stay lossless where it matters; it is
   listed for completeness, not as a recommendation.

### Incremental ideas (small, well understood)

4. **Per-frame clear elimination.** The heads and counts images are cleared at
   full resolution every frame (~66 MB of writes at 3440x1440). A frame-stamp in
   the head value's high bits would remove the clear entirely; node indices need
   about 26 bits at this resolution and capacity, leaving room. Worth roughly
   0.1-0.2 ms of GPU time.
5. **Node size reduction.** `OITNode` is 32 bytes. `depth` and `blend` carry more
   precision than they need and could plausibly reach 24 bytes, a 25% bandwidth
   reduction on the structure the sort traverses most. The `color` field must not
   be narrowed: HDR range and blend-factor reconstruction depend on it. Note the
   equal-depth tie-break depends on exact depth comparison, so narrowing `depth`
   needs care rather than a blind cast.
6. **Sorter selection.** `RenderExactOITComputeSort` defaults off. Both sorters
   are lossless, so A/B comparison on target hardware is safe and cheap.

Items 4-6 are incremental and unlikely to change how the renderer feels. Item 1
is the one worth real effort if the GPU profile supports it.

## Method note

Two instrumentation errors occurred during this investigation and are recorded so
the numbers above are read with appropriate confidence:

- The first fence probe omitted `GL_SYNC_FLUSH_COMMANDS_BIT`, making its 0%
  result unreliable. It was corrected and re-run; the corrected result agreed.
- An accumulator (`accumulated_fence_ms`) was not reset per window, producing
  negative "transfer" figures. The GPU-already-finished counts were unaffected,
  and those are what the conclusion rests on. The fence/transfer *split* was never
  measured cleanly and no claim here depends on it.

All temporary instrumentation has been removed; `fsexactoit.cpp` is unchanged
from its committed state.
