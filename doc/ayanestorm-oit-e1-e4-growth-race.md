# Exact OIT: E1 proactive/overflow growth raced E4's speculative pass, crash on zoom

Author: chanayane@firestorm. Date: 2026-09-03.
Companion to `ayanestorm-oit-performance-audit-plan.md` (items E1 and E4) and
`ayanestorm-oit-e4-fence-stall-question.md` (the earlier E4 fence-stall fix).
Not predicted by the plan; found and fixed during E6 testing.

## Symptom

After E4, E5, E6 were all implemented and built, the user zoomed the camera
into a dense water-splash particle scene (Exact OIT enabled). The viewer
crashed with no fatal/error line logged — consistent with a driver-level
crash rather than a caught C++ exception. The log's last lines before the
gap:

```
WARNING #ExactOIT# ... FSExactOIT::captureOverflowed : ONCE: Exact OIT node
    capacity exceeded (required 28715435, capacity 19337136); rendering
    complete vanilla transparency for this frame.
INFO  #ExactOIT# ... FSExactOIT::growNodePool : Grew exact OIT node capacity
    to 38674272
INFO  #ExactOIT# ... FSExactOIT::growNodePool : Grew exact OIT node capacity
    to 67108864
```

Two node-pool growths in immediate succession, the second landing at
67,108,864 nodes × 32 bytes ≈ 2.1 GB — right at `safeNodeCapacity()`'s cap
(`min(VRAM/4, 2 GB) / 32 bytes`). GPU: NVIDIA RTX 3080 Ti, driver 596.49,
GL 4.6 (same machine as the earlier fence-stall investigation).

## Root cause: growNodePool() reallocated a buffer with GPU work still queued against it

`FSExactOIT::growNodePool()` calls `glBufferData()` on `sResources.nodes`
(the SSBO every capture fragment writes to via atomics, and every sort pass
reads/rewrites). Before this fix, it was called from two places, both
**mid-frame, after this frame's own capture had already written data into
the buffer**:

1. `captureOverflowed()` — called synchronously from `waitValidation()`,
   which by then has already:
   - run this frame's capture draws (writing `sResources.nodes`), and
   - (E4) issued the speculative pass-1 sort draw against `sResources.nodes`,
     with only a `glMemoryBarrier` — no fence, no wait — separating it from
     the capture that preceded it.
2. The proactive-growth branch at the end of `waitValidation()` (E1's
   "grow now rather than waiting for an overflow frame later") — same
   timing, just on the non-overflow path.

`glBufferData()` on a buffer object *is* well-defined while older commands
that reference its previous storage are still queued (the GL spec's
orphaning contract: the driver is required to keep the old allocation alive
until those commands retire). But relying on that contract here is fragile
in practice, for two reasons specific to this code path:

- The reallocation is *itself* still followed, later in the same
  `waitValidation()`/`composite()` call chain, by code that **rebinds the
  same buffer object** (via `bindCompositeResources()` /
  `glBindBufferBase(..., sResources.nodes)`) and issues **more sort passes
  and the final blend pass against it** — all in the same frame, all
  expecting to see this frame's capture data. If the driver's orphaning
  swaps in a *new, uninitialized* allocation for the buffer object (which is
  the whole point of `glBufferData` re-specification), every draw issued
  after the reallocation binds and reads the **new empty buffer**, not the
  one the speculative pass and capture had just populated. This is not a
  hypothetical GPU-side memory race so much as a straightforward CPU-side
  logic bug: the buffer's *identity* stays the same GL name, but its
  *storage* was swapped out from under a frame that was mid-flight using it.
- Doing this at ~2 GB, near the safe-capacity ceiling, is exactly where a
  driver is most likely to take a slow/unusual path for the reallocation
  (defragmentation, eviction, a synchronous stall) — consistent with a crash
  appearing specifically at the second, larger growth (19.3M → 38.6M was
  fine; 38.6M → 67.1M crashed).

## Why the plan didn't predict this

E1 ("Overflow: proactive growth and predictive skip") was designed and
written *before* E4 existed in the phase plan. At E1's design time,
`validateCapture()` (E4's later split into `beginValidation()` +
`waitValidation()`) was a single function that ran to completion, including
its `glGetBufferSubData` readback, *before* `composite()`'s sort-pass loop
ever touched `sResources.nodes` for reading. E1's own text says: "Proactive
growth: **at the end of `validateCapture()`**... Growing here costs one
`glBufferData` but avoids a double-render frame later" — a true and safe
claim in the pre-E4 pipeline, where nothing was yet reading the buffer for
sorting at that point in the frame.

E4 moved sort pass 1 to run **before** the equivalent of `validateCapture()`
(speculatively, to overlap the capture-completion fence wait). E4's plan
text and traps list do not mention E1's proactive-growth or
`captureOverflowed()`-triggered growth calls at all — they were not
re-examined for the new call ordering. Nothing in
`ayanestorm-oit-performance-audit-plan.md` flags this interaction.

## Fix

Deferred both growth paths to `beginFrame()`, before any of *that* frame's
capture draws are issued — the previous frame's GPU work (capture, sort
passes, composite) is by then already fully submitted in order ahead of the
reallocation, and this frame hasn't touched the buffer yet, so there is no
window where in-flight GPU work references the buffer across a
reallocation.

`fsexactoit.h`, `Resources`: added
```cpp
U32 pendingGrowthNodes = 0;   // 0 = no growth pending
```

`fsexactoit.cpp`:
- Extracted the target-capacity math out of `growNodePool()` into a pure,
  file-scope free function `computeGrownCapacity(required_nodes,
  current_capacity)` (no GL calls, no access to `sResources`, so it is safe
  to call from anywhere including the skip-frame prediction below).
  `growNodePool()` itself keeps doing the actual `glBufferData` reallocation
  and remains a private `FSExactOIT` member function (an earlier draft tried
  to split the reallocation itself into a second free function too, but that
  needs `sResources`, a private static member — MSVC rejected it with
  C2065/C2198; reverted, the reallocation stays inside `growNodePool()`).
  `growNodePool()` is unchanged in signature/behavior — still does an
  immediate reallocation — but is now only called from the safe point,
  `beginFrame()`.
- `captureOverflowed()`: instead of calling `growNodePool()` immediately, it
  predicts success with `computeGrownCapacity()` alone (no GL call) to
  decide the skip-frame policy, and records
  `sResources.pendingGrowthNodes = max(pendingGrowthNodes, required_nodes)`
  when growth would help.
- `waitValidation()`'s proactive-growth branch: same change — records
  `pendingGrowthNodes` instead of calling `growNodePool()` inline.
- `beginFrame()`: if `pendingGrowthNodes > 0`, calls
  `growNodePool(pendingGrowthNodes)` (the real reallocation) and clears the
  field, before resetting per-frame capture state.
- `releaseResources(bool)`: clears `pendingGrowthNodes` alongside the other
  transient counters, so a stale request can't outlive a resource
  release/reallocate cycle (mode toggle, resize).

## Consequence for the overflow/skip-frame frame

Growth is now requested one frame later than before, but the *effect* for
the user is the same or better: the overflow frame already renders vanilla
transparency (unchanged — `discardCapture()` still runs immediately). The
grown capacity takes effect starting the *next* frame's capture instead of
being available (unsafely) partway through the *current* one. No behavior
regresses; `E1`'s stated goal ("never pay capture + full vanilla re-render
more than once per demand increase") still holds, since growth still lands
before the next capture attempt.

## Verify

Same crash repro: zoom into a dense sprite/particle scene that pushes demand
from well under capacity to a large multi-step growth in one or two frames.
Expect: no crash, `"Grew exact OIT node capacity to..."` log lines appear
one frame later than the demand spike (visible as one extra vanilla-fallback
frame at most, same as before this fix), debug mode 4 stays green, image
correct once growth lands.

**Confirmed 2026-09-03 (bokt):** same repro (zoom into the water-splash
scene) that crashed before the fix. Log:
```
WARNING #ExactOIT# ... FSExactOIT::captureOverflowed : ONCE: Exact OIT node
    capacity exceeded (required 27643825, capacity 19337136); rendering
    complete vanilla transparency for this frame.
INFO  #ExactOIT# ... FSExactOIT::growNodePool : Grew exact OIT node capacity
    to 38674272
INFO  #ExactOIT# ... FSExactOIT::growNodePool : Grew exact OIT node capacity
    to 67108864
```
No crash. FPS did not collapse during the demand spike (unlike the pre-fix
repro, which crashed outright). Both growth steps landed back-to-back across
two consecutive frames — demand outran a single growth step, which is
expected (each `beginFrame()` only applies one pending request; a second
overflow on the very next frame queues another) and is exactly the "one
extra vanilla-fallback frame at most" cost described above, just paid twice
in quick succession for this particular spike.

---

# Review (reviewing model, 2026-09-03)

## Verdict: the fix is correct and is what I would have done. Keep it.

Deferring every `glBufferData` on `sResources.nodes` to `beginFrame()` is the
right design: at that point the previous frame's commands are all queued
ahead of the reallocation (orphaning keeps their storage alive), and nothing
of the new frame has touched the buffer yet. `computeGrownCapacity()` as a
pure function for the skip prediction is also right. Verified in the code:
`captureOverflowed()`, the proactive branch in `waitValidation()`,
`beginFrame()`, and `releaseResources()` all do what this document says.

## Two corrections to the diagnosis (they do not change the fix)

1. **The bug was not created by E4; it was latent in E1 and is the plan's
   fault.** E1's proactive growth ran at the end of `validateCapture()`, and
   `composite()` ran *after* `validateCapture()* in the same frame, reading
   `sResources.nodes`. So even before E4, a proactive-growth frame issued the
   sort passes and the blend against the freshly orphaned, uninitialised
   storage. The claim in "Why the plan didn't predict this" that nothing read
   the buffer after that point pre-E4 is wrong. E4's speculative pass is
   issued *before* the growth and therefore used the old storage; it was
   never the racing party. The overflow-path growth (the original code) was
   always safe for the opposite reason: after an overflow no composite runs.
   The plan text for E1 said "grow now, at the end of validateCapture" —
   that instruction was the defect. I have corrected the plan.
2. **The likely crash mechanism is a GPU hang, not the allocation size.**
   The composite reads garbage `next` indices from uninitialised memory;
   the linked-list traversals in pass 1 and the blend loop have no bound, so
   a garbage cycle loops forever, the driver's watchdog (TDR) kills the
   context, and the viewer dies with nothing logged. Read the log this way:
   the crash followed the *second* growth because that one was the proactive
   (non-overflow) growth, the only path that composites after reallocating.
   The first growth was an overflow-path growth and was safe. Size was not
   the trigger.

## Small follow-ups (not blocking; do them with the next build)

- Guard `beginFrame()`'s growth: `if (pendingGrowthNodes > 0 && isEnabled() && sResources.nodes)`.
  Today a pending request can trigger a multi-GB `glBufferData` on the frame
  the user disables Exact OIT, immediately before `captureEligible()`
  releases everything.
- The proactive request records `control[0] * 2`, and
  `computeGrownCapacity()` then applies `max(×1.25, capacity×2)` on top, so
  a 29 M-node demand jumped straight to the 2 GB cap (67.1 M nodes). Record
  `control[0]` (not ×2); the doubling policy already lives in
  `computeGrownCapacity()`. This keeps VRAM closer to demand until E10b
  (shrink) exists.
- `growNodePool()` returns a bool nobody uses now; either drop the return or
  log when `beginFrame()` growth still leaves `capacity < required`.

## Rule for all remaining items

Any reallocation of a buffer that shaders read or write (node pool growth,
E10b shrink, the E8 SoA split if it is ever done, the compute queues if E9
keeps them) happens only in `beginFrame()`, before that frame's capture, via
the same pending-request pattern. Never mid-frame. This is now stated in the
plan under E1, E10b and the E4 trap list.

## Verification you already did is sufficient

Same repro, no crash, debug mode 4 green, two growth lines one frame apart:
that is the expected signature. No further test is needed for this fix.
