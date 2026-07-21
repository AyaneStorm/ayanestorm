# Exact OIT Transparency Findings

Date: 2026-07-17

## Purpose and quality contract

The active transparency experiment replaces weighted blended order-independent
transparency (WBOIT) with exact per-pixel linked-list transparency (PPLL, also
known as an A-buffer). The feature is exposed as `RenderExactOIT`.

The implementation is governed by the following requirements:

- Capture every eligible transparent fragment once.
- Sort fragments by window-space depth for each pixel.
- Composite farthest-to-nearest with each draw's original color and alpha blend
  factors and equations.
- Include standard alpha, custom blends, regular, rigged, PBR, fullbright,
  legacy material, GLTF, emissive, and glow transparency in the ordered data.
- Never use a fixed layer count, approximate remainder, material heuristic, or
  world/avatar/attachment ordering category.
- Never display an incomplete list. If capacity is exceeded, rerender complete
  vanilla transparency for that frame.
- Never exchange visual correctness for performance without explicit approval.
- With the compatibility setting disabled, preserve the transparency behavior
  of `special-ayanestorm-dev` exactly, apart from independently valid fixes that
  have been explicitly retained.

## Active architecture

### Source ownership

The Exact OIT C++ implementation now resides in `fsexactoit.cpp` and
`fsexactoit.h`. Viewer-owned files retain narrow integration hooks for shader
management, resource lifecycle, alpha and GLTF traversal, validation-result
handling, vanilla fallback traversal, compositing, and diagnostics.

Exact OIT shaders are created whenever the hardware supports the required
OpenGL and GLSL versions, independently of the user setting. This permits the
setting to be enabled without restarting. GPU resources remain setting-driven:
starting disabled allocates none, enabling lazily allocates them at the next
eligible alpha pass, disabling releases them, and re-enabling recreates them.
The complete disabled-enable-disable-re-enable sequence was tested successfully.

### Capture resources

The Exact OIT path currently uses:

- A full-resolution `R32UI` head-pointer texture. `0xFFFFFFFF` is the empty-list
  sentinel.
- A full-resolution `R32UI` fragment-count texture used to avoid redundant sort
  work.
- A shader-storage buffer containing 48-byte fragment nodes.
- A control shader-storage buffer containing allocation count, capacity,
  overflow state, and maximum per-pixel list length.
- An opaque-scene backup render target used by the final composite and by the
  complete vanilla fallback.

Capture shaders allocate nodes atomically and insert them with
`imageAtomicExchange`. Node writes are bounds checked. An allocation beyond the
current capacity sets the overflow state instead of writing outside the buffer.

Every Exact OIT fragment shader uses `layout(early_fragment_tests) in`. This is
essential: the existing opaque depth buffer rejects hidden transparent
fragments before they can allocate and link a node, while depth writes remain
disabled.

### Sorting and compositing

Sorting is an exact bottom-up linked-list merge sort. It does not place the
fragments in a fixed-size local array and therefore has no per-pixel fragment
limit other than the global, checked node-buffer capacity.

The original implementation performed counting, every merge width, and final
blending in one fragment invocation. Dense lists could keep a single GPU
invocation active long enough to trigger the NVIDIA driver watchdog.

The active implementation divides this work into capture metadata and GPU sort
passes:

1. Atomically count each successfully captured node in its pixel and record the
   maximum list length while the fragment is inserted.
2. Read overflow, total-node, and maximum-list metadata together at the one
   mandatory post-capture synchronization point.
3. Execute one merge width per fullscreen draw, with the required image and
   shader-storage barriers between draws.
4. Skip a pixel during a merge pass when its exact stored list count is no
   greater than that pass's merge width.
5. Traverse the sorted list and produce the final composite.

Skipping a completed pixel changes only the amount of work. It does not remove,
approximate, reorder, or alter any fragment.

### Capacity and fallback

The initial target is an average of four fragments per screen pixel when that
fits the safe allocation. The node budget is capped by the smaller of 25% of
reported dedicated VRAM and 2 GiB, without taking memory required by mandatory
scene targets.

After capture the CPU reads the control data before composite:

- A complete capture is sorted and composited.
- An overflowed capture is discarded in its entirety.
- All transparency is rerendered through the complete vanilla path in the same
  frame.
- The node buffer is grown for later frames when the safe budget permits.
- Allocation or shader failure disables Exact OIT for the session and reports a
  diagnostic reason; it never falls back to WBOIT.

## Visual investigation history

### Incomplete shader-set fallback

An early run repeatedly logged that the Exact OIT shader set was incomplete.
This was shader readiness handling, not insufficient VRAM. The renderer now
checks all required regular, rigged, emissive, PBR glow, material, and GLTF
variants before beginning capture. A missing program selects complete vanilla
transparency rather than dereferencing an invalid shader or rendering a partial
Exact OIT frame.

Exact OIT shaders require GLSL 4.30 because they use images, shader-storage
buffers, and atomics. Shader preprocessing now forces the appropriate version
for Exact OIT sources and `EXACT_OIT` permutations.

### Transparent surfaces erasing opaque foreground objects

Initial captures accepted fragments that were behind opaque scene geometry.
The final fullscreen composite then placed those fragments over the opaque
foreground, producing windows through walls and rear transparent objects that
erased foreground objects.

Several attempts to reproduce the depth rejection later in the composite were
not reliable. The correct fix was early fragment testing during capture against
the already populated opaque depth buffer. Once this was active, fragments
behind opaque geometry never entered the linked lists.

### Camera-dependent disappearance

Hair, glass panels, and grass could disappear according to camera orientation.
The disappearance correlated with unrelated transparent content becoming
visible elsewhere on screen. The content looked correct whenever it did render,
which indicated corrupted or inconsistently rejected list contents rather than
an asset-specific shading problem.

Establishing early depth testing consistently across every capture shader
permutation removed the camera-dependent disappearance. It also preserved the
correctly rendered appearance instead of returning to the version that showed
all transparent surfaces through opaque geometry.

### Confirmed visual result

In the successful test session, the following were reported correct together:

- Eyeglasses and eyelashes.
- Layered avatar hair.
- World glass and windows.
- Grass and other alpha geometry.
- Transparent content in front of and behind other transparent content.
- Opaque occlusion of transparent objects.
- Glow and emissive transparency.

No slowdown was initially noticed in ordinary scenes. This is the strongest
visual confirmation so far that exact per-fragment ordering solves the observed
vanilla and WBOIT transparency failures without asset-specific workarounds.

## Crash and performance findings

### Evidence

Three viewer crash dumps created on 2026-07-16 show the same failure class:

- Exception code `0xC0000409` with fail-fast subcode `7`.
- Faulting instruction inside NVIDIA's OpenGL driver, `nvoglv64.dll`.
- The newest dump faulted at `nvoglv64.dll + 0x10c427d`.
- The crashing thread stack was almost entirely NVIDIA OpenGL driver frames.

An earlier incident also produced Windows `LiveKernelEvent 141`, which is a GPU
watchdog/reset event. The newest incident did not produce another event 141;
the NVIDIA OpenGL driver issued its own fail-fast first. This is not the pattern
of a normal viewer-side access violation.

Immediately before the newest crash in a crowded region, frame rate repeatedly
fell into approximately the 8--11 FPS range. The viewer log ended abruptly, but
showed no Exact OIT overflow or allocation failure. System memory was not
exhausted. The machine reported an RTX 3080 Ti with 12 GiB VRAM, OpenGL 4.6, and
NVIDIA driver 596.49.

These observations identify excessive/long-running GPU linked-list sorting as
the leading cause. They do not indicate that fragments should be dropped or
that a smaller visual layer budget is acceptable.

### Optimization progression

1. **Monolithic per-pixel sort:** visually correct, but capable of a long single
   shader invocation and GPU watchdog reset.
2. **One merge width per draw:** bounds each invocation and adds explicit
   synchronization, avoiding the original monolithic workload. It remained
   expensive because every pixel traversed its list during every merge width.
3. **Exact count-guided merge passes:** stores each pixel's list length and skips
   later merge widths for lists already fully sorted. Empty pixels and short
   lists no longer repeat linked-list traversal.
4. **Capture-time exact counting (current, awaiting confirmation):** increments
   the pixel count as each valid node is linked and updates the maximum length
   atomically. This removes the separate fullscreen counting traversal and its
   second synchronous CPU readback. Camera transitions emit at most two
   diagnostic lines: the transition sample and a 30-frame peak summary.
5. **Viewport-resize node-pool retention (transition fix confirmed):**
   first-person mode changes the tested 3440-pixel-wide world view from 1328 to
   1351 pixels high as UI visibility changes. The old path destroyed and
   recreated an approximately 836--851 MiB node buffer on every transition.
   Viewport-only resizes now retain the largest successful node allocation;
   only resolution-dependent textures and targets are recreated. Full release
   still occurs for shutdown, failures, disabling Exact OIT, and graphics-state
   recreation.
6. **Natural-run merge sorting (moving-camera improvement confirmed, remaining
   slowdown):**
   sudden particle and sprite bursts can create hundreds of overlapping
   fragments in a pixel. The earlier all-or-nothing monotonic fast path helped
   one test, but another large burst still slowed down severely specifically
   while the camera was moving. This indicates that camera-relative depth
   changes leave the lists partially ordered rather than fully monotonic.
   Sorting now discovers natural ordered runs, reverses descending runs, merges
   adjacent run pairs, and stores the remaining run count so completed pixels
   skip later passes. Fully unordered data still receives a complete exact
   merge sort. No fragments or blend operations are removed. Retesting while
   moving the camera showed a visible performance improvement, confirming that
   partial ordering was relevant, but the scene remained somewhat slow. This is
   therefore an effective optimization rather than a complete resolution.
7. **Split sort metadata and payload buffers (tested and removed):**
   Tracy frame 12611 showed repeated `Exact OIT natural sort pass` GPU zones
   dominating the frame while the CPU spent 76 ms in the validation readback
   waiting for the GPU backlog. Sorting previously traversed 48-byte nodes even
   though it only uses depth, next index, and sequence. Nodes are now split into
   a 16-byte sort record and a 32-byte color/glow payload. Total storage remains
   48 bytes per fragment, and capture/final blending retain identical data, but
   sort passes no longer fetch or rewrite unused color payloads.

   `trace002.tracy` contained 444 Exact OIT frames, with camera movement during
   the first 222 and a stationary camera during the last 222. Validation-wait
   results were:

   - Moving: 18.78 ms mean, 13.40 ms median, 41.55 ms p90, 50.07 ms p95,
     134.48 ms maximum.
   - Stationary: 17.10 ms mean, 12.66 ms median, 33.83 ms p90, 39.47 ms p95,
     46.79 ms maximum.

   Camera movement clearly increases the heavy tail. However, the moving mean
   did not improve over `trace001.tracy`'s 18.31 ms mean. Scene variation
   prevents treating the small difference as a regression, but the split layout
   has not demonstrated an end-to-end performance benefit.

   The frame 9210 screenshot showed the 134 ms readback waiting on a deep GPU
   queue, with Exact OIT capture itself occupying roughly 30--35 ms immediately
   before the wait released. Splitting the node added a second scattered SSBO
   write to every captured fragment. With no end-to-end benefit and evidence of
   increased capture pressure, the split-buffer experiment was removed. Exact
   OIT again uses one 48-byte node buffer.
8. **Lossless compact single-buffer nodes (performance improvement confirmed):**
   The combined node contained a four-float glow vector although only its first
   component was ever read, plus a stored sequence value that was always equal
   to the node's allocation index. Glow is now one full-precision float and
   equal-depth ordering compares node indices directly. The resulting node is
   32 bytes instead of 48, while retaining identical color, glow precision,
   depth, blend factors, links, and deterministic ordering. This reduces both
   capture writes and linked-sort traffic without adding another buffer access.
   The VRAM budget and growth calculations now use the exact 32-byte stride.

   `trace003.tracy` measured 496 Exact OIT frames. Compared with
   `trace002.tracy`, validation-wait timing improved:

   - Mean: 17.94 ms to 14.44 ms.
   - p90: 38.66 ms to 22.14 ms.
   - p95: 44.92 ms to 26.16 ms.
   - p99: 57.85 ms to 33.88 ms.

   This improvement occurred despite a substantially heavier capture. The new
   session required as many as 58.8 million nodes and grew to the 67.1-million
   safe-node cap, while the earlier split-layout crash occurred around
   29.8 million required nodes. One isolated 123 ms maximum remained during the
   early repeated overflow/growth sequence; outside that outlier the timing
   tail was much lower.

   The trace003 test moved the camera for approximately the first half and held
   it stationary for approximately the second half. The transition was manual
   and not exactly at frame 248, so half-by-half statistics must not use that
   frame as a precise boundary. Aggregate trace002-to-trace003 percentiles are
   the reliable comparison for this capture.
9. **Geometric overflow growth (confirmed):**
   Trace003 overflowed and reallocated four times while rising from 19.3 million
   to the 67.1-million-node safe cap. Growth previously targeted only 25 percent
   above the most recently observed demand. It now retains that demand headroom
   but also grows by at least 2x when the safe VRAM budget permits. For the
   trace003 sequence this should reduce four large reallocations to two:
   approximately 19.3 to 38.7 million, then to the 67.1-million cap. Overflow
   frames still rerender complete vanilla transparency, and allocation remains
   bounded by the existing VRAM limit.

   The latest log confirmed exactly two growth events:

   - 19,337,136 to 38,674,272 nodes after a 24,256,592-node requirement.
   - 38,674,272 to the 67,108,864-node cap after a 38,695,208-node requirement.

   `trace004.tracy` contained 396 Exact OIT frames. Compared with trace003,
   validation-wait timing improved from 14.44 to 13.52 ms mean, 13.09 to
   11.44 ms median, 22.14 to 18.44 ms p90, and 26.16 to 23.86 ms p95. The
   approximate stationary half measured 10.95 ms mean and 12.42 ms p95. A
   135 ms maximum remains from a one-time large allocation; geometric growth
   reduces the number of these allocation events but not the cost of an
   individual allocation.
10. **Bounded exact short-list completion (crashed; removed):**
    Trace005 switched immediately to first person and moved the camera. It
    contained 252 Exact OIT frames with a 17.24 ms mean, 22.91 ms p90,
    34.99 ms p95, and 157.88 ms maximum validation wait. The retained capacity
    was already 67.1 million nodes; camera-transition diagnostics peaked around
    14.7 million nodes and a 69-fragment maximum list, so allocation was not the
    cause.

    After one natural merge pass, lists containing at most 32 fragments and no
    more than four remaining ordered runs now finish with exact stable
    linked-list insertion sorting in that same draw. Monotonic lists still
    finish through the natural-run path, while longer or more disordered lists
    retain the complete multi-pass natural merge sort. This changes scheduling
    only; it does not alter fragment data or final order.

    The test build crashed inside the NVIDIA OpenGL driver while the scene was
    rezzing, immediately after the second geometric buffer growth. The same
    growth path was stable in trace004; bounded insertion sorting was the only
    new GPU algorithm in this build. It was therefore removed rather than
    accepted as a stability risk. Shader cache revision v5 prevents cached v4
    composite binaries from loading.

### Split-layout startup crash

The first split-buffer test build crashed shortly after login. The log showed
that it loaded cached Exact OIT program binaries compiled for the old combined
48-byte node layout. The shader cache key included shader paths and viewer
version, but not shader source contents; the development build retained the
same viewer version, so the incompatible binaries were accepted. Those shaders
then wrote combined nodes into the new 16-byte metadata buffer.

The viewer shader-cache version now includes an AyaneStorm Exact OIT revision
salt. This forces recompilation after incompatible shader-layout changes. The crash log
also showed a 29.8-million-node overflow followed by growth from 19.3 million
to 37.3 million nodes immediately before termination, but stale shader writes
make that run invalid for evaluating the split-buffer allocation itself.

### Test result after capture counting and node-pool retention

The first-/third-person transition became instant after retaining the node pool,
confirming that repeated destruction and allocation of the approximately
836--851 MiB SSBO was the original camera-transition stall. Capacity remained
stable at 19,337,136 nodes during subsequent transitions.

Performance later degraded again as the scene continued loading. This later
slowdown had a different signature:

- Exact OIT did not overflow or grow its node buffer.
- Captured node counts varied from roughly 2.7 million to 12.1 million.
- Maximum per-pixel lists rose as high as 558 fragments, requiring ten exact
  merge widths for the affected frame.
- Texture-downscaling warnings increased rapidly while the viewer was loading
  scene content.
- Reported viewer physical memory rose from approximately 8.4 GiB to 16 GiB.
- Observed frame rate later fell into the 3--12 FPS range.
- NVIDIA reported 12,288 MiB total VRAM, 9,658 MiB used, and 2,427 MiB free at
  the time of inspection.

This evidence does not show the Exact OIT allocation filling up: its capacity
was stable and no overflow occurred. The leading explanation is combined
texture-memory churn and the genuine cost of sorting unusually dense
transparent pixels. Precise GPU timing is required to separate capture,
synchronization, individual merge passes, and final composite before making
another optimization decision.

The later optimizations were not present together in the build that created the
latest analyzed crash dump. The current version needs testing in the same
crowded location and in first-person view before its stability or performance
can be considered confirmed.

## Diagnostics and evidence locations

The proposed relocated runtime log directory was:

```text
S:\as\logs
```

However, the July test builds continued writing the active log to:

```text
C:\Users\[user]\AppData\Roaming\AyaneStorm_x64\logs\AyaneStorm.log
```

`S:\as\logs` still contained an old May log, so the relocation had not taken
effect. Future analysis should use the roaming-profile log unless a newer file
appears on `S:` and correlate its final timestamp with:

```text
C:\Users\[user]\AppData\Local\CrashDumps
```

Available renderer diagnostics include Exact OIT availability, node capacity,
peak allocation count, node memory, overflow count, and status/fallback reason.
`RenderExactOITDebugMode` provides final composite and structural inspection
modes such as fragment count, depth extrema, sorted-depth validation, blend-mode
visualization, and utilization/overflow state.

Profiler instrumentation separates the moving-camera sprite workload into:

- `Exact OIT capture`
- `Exact OIT validation readback` (CPU time, including the GPU wait)
- `Exact OIT opaque copy`
- `Exact OIT natural sort`
- `Exact OIT natural sort pass` (one zone for each submitted pass)
- `Exact OIT final blend`

These zones do not add rendering shortcuts or alter either Exact OIT or vanilla
output. They expose the remaining cost through a profiler capture rather than
adding another synchronous timing readback to every frame.

The Firestorm configure script must be passed `--tracy`, and CMake must also
receive `-DUSE_TRACY_GPU:BOOL=ON`. `--tracy` alone leaves GPU profiling disabled
by default. A configure summary showing `TRACY: false` compiles these profiler
zones out entirely.

## Required validation

The next validation run should preserve the exact same rendering requirements:

- Revisit the crowded location that reproduced the NVIDIA failure.
- Exercise both third-person camera rotation and first-person view.
- Compare frame rate and responsiveness against vanilla without using the
  result to lower transparency quality automatically.
- Recheck hair, eyelashes, eyeglasses, windows, grass, glow, and opaque
  occlusion after every sorting optimization.
- Inspect the new log for capacity, peak-node, overflow, fallback, and shader
  readiness messages.
- If a crash occurs, retain the corresponding viewer dump and check whether it
  repeats `0xC0000409` inside `nvoglv64.dll` or presents a new failure signature.
- Force an undersized buffer separately to verify that overflow produces a
  complete vanilla frame and never a partial composite.
- With `RenderExactOIT` disabled, compare directly against
  `special-ayanestorm-dev` for vanilla parity.

## Open work

- Continue long-session stability testing of count-guided merge passes.
- Validate natural-run sorting with the same sprite burst while moving and
  holding the camera still, and compare it against the earlier monotonic-only
  implementation.
- Use the moving-camera sprite reproduction for those timings; compare capture,
  validation readback, natural-run passes, and final blending before changing
  the sorting algorithm again.
- Complete systematic blend-factor/equation reference tests, including separate
  color and alpha state.
- Complete parity tests for water-adjacent alpha, HUDs, impostors, cube and
  reflection captures, DoF, and transparency highlighting.
- Preserve a one-time, precise diagnostic for every session-level fallback.

## Further exact optimization directions

The remaining performance and watchdog risk is architectural. Candidate work
must preserve every visually relevant fragment, exact ordering, original blend
behavior, and the untouched vanilla-disabled path.

### Exact opaque cutoff inside transparent lists

Opaque scene geometry already rejects hidden transparent fragments through
early depth testing. However, an alpha value of 1 produced by geometry submitted
through the transparent pass does not write depth. Capture therefore still
records fragments behind it because submission order does not establish which
fragment is nearest.

For a node using a blend mode proven to overwrite both destination color and
alpha completely, a list traversal can find the nearest such node before
sorting and discard every deeper node from that pixel's sorting workload. This
is exact: those deeper color and glow contributions cannot survive that
overwriting node. The optimization must not apply merely because source alpha
equals 1; custom destination-dependent blend factors, equations, or other
special modes may still require the destination beneath them.

If a pixel captures `n` fragments and the nearest proven overwrite leaves only
`k` relevant fragments at or in front of it, work changes from approximately
`O(n log n)` sorting plus `O(n)` blending to `O(n)` cutoff discovery followed
by `O(k log k)` sorting and `O(k)` blending. A 100-fragment pixel reduced to
five relevant fragments could eliminate roughly 80--95 percent of its sorting
work. A cutoff leaving only itself can eliminate sorting for that pixel after
the discovery traversal.

Expected benefit varies by content:

- Mostly translucent smoke or glow may have no qualifying cutoff.
- Sprites with solid interiors and soft edges, foliage, fences, and alpha cards
  may reduce affected-pixel sorting by approximately 20--50 percent.
- Pathological stacks with a near opaque-overwrite fragment could improve by
  several times in their sorting stage.

Whole-frame gains will be smaller because initial fragment shading and capture
still occur. The optimization does not reduce the allocation count or prevent
overflow by itself; it reduces sorting and final blending after capture.
Discovery should be integrated into the first natural-sort traversal so scenes
without a qualifying cutoff do not pay a separate extra list scan. Equal-depth
sequence order must be considered when deciding which nodes are behind the
cutoff.

#### Deferred near-opaque exploration

A later, explicitly approximate experiment may evaluate treating source alpha
at or above `0.995` as a cutoff for deeper-list processing. This would include
an 8-bit alpha value of 254 (`254/255`, approximately 0.99608), which commonly
comes from transparent textures rather than an object-opacity value a user can
select directly.

This is not part of the active implementation or approved quality contract.
Texture filtering, mipmaps, vertex/object alpha, HDR backgrounds, and glow can
make the discarded sub-percent destination contribution visible. The experiment
must remain deferred until a deliberate perceptual-quality exception is
approved and validated against filtered texture edges and bright content.
Current Exact OIT cutoff work may use only mathematically complete overwrite
nodes.

### Compute-based parallel sorting

Move linked-list sorting out of one fragment invocation per pixel and into
compute shaders with explicitly bounded workgroups. A parallel merge or exact
key sort could expose more concurrency, improve memory coalescing, and keep an
individual dispatch below watchdog-sensitive execution time.

### GPU-driven work scheduling

Replace repeated fullscreen sort draws with compact work queues or indirect
dispatch. Only pixels with more than one remaining run should generate work.
Completed and empty pixels would stop launching shader invocations instead of
returning early from later fullscreen passes.

### Exact key/payload organization

Investigate a GPU-oriented representation in which sorting touches compact
depth/order/link keys and final blending reads full payloads. The tested simple
two-SSBO split added a second scattered capture write and showed no end-to-end
benefit, so a future design must improve capture locality rather than repeating
that layout.

### GPU-side validation and fallback selection

The synchronous control-buffer readback is where the CPU waits for the entire
GPU backlog. A GPU-driven overflow decision could avoid that CPU stall, but it
must still select either a complete Exact OIT result or a complete vanilla
rerender in the same frame. It may never display partial capture data.

### Capacity reuse across sessions

Persisting or predicting a previously required safe capacity could avoid large
runtime reallocations while a scene rezzes. This trades shorter stalls for
reserving potentially very large amounts of VRAM earlier, so it must remain
bounded by the existing dedicated-VRAM budget and account for texture pressure.

### Watchdog-safe workload subdivision

Further divide expensive sorting into bounded compute dispatches and use GPU
work queues to continue unfinished exact work. This should bound the duration
of any one shader invocation without imposing a fragment or layer limit.

No future optimization may impose a fixed fragment limit per pixel, silently
discard captured data, substitute approximate blending, or knowingly add visual
artifacts without explicit approval.
