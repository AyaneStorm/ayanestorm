<!-- <AS:Chanayane> Exact OIT findings and investigation record -->
# Exact OIT Transparency Findings

Date: 2026-07-16

## Purpose and quality contract

The active transparency experiment replaces weighted blended order-independent
transparency (WBOIT) with exact per-pixel linked-list transparency (PPLL, also
known as an A-buffer). `RenderWBOIT` remains the compatibility setting name, but
enabling it selects Exact OIT.

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
- With `RenderWBOIT` disabled, compare directly against
  `special-ayanestorm-dev` for vanilla parity.

## Open work

- Continue long-session stability testing of count-guided merge passes.
- Add GPU timings before addressing the later memory-pressure/dense-list
  slowdown; do not infer the expensive stage from aggregate FPS alone.
- Add sufficiently precise GPU timing around capture, count/readback, each sort
  stage, and composite so future optimization targets measured cost.
- Complete systematic blend-factor/equation reference tests, including separate
  color and alpha state.
- Complete parity tests for water-adjacent alpha, HUDs, impostors, cube and
  reflection captures, DoF, and transparency highlighting.
- Preserve a one-time, precise diagnostic for every session-level fallback.

No future optimization may impose a fixed fragment limit per pixel, silently
discard captured data, substitute approximate blending, or knowingly add visual
artifacts without explicit approval.
<!-- </AS:Chanayane> -->
