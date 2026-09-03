# AVBOIT: hair flicker + tile-range corruption (handoff -- two fixes tried, both still broken)

**Current status (2026-09-03, after round 10): read "Round 10: remaining
cell popping -> implement A9" and its "Round 10 implementation status" near
the end of this file first.** Rounds 1-3 fixed the flicker, hair colour,
clothing-showing-through, black dashes, and coarse 8px-block moire, in that
order. Round 3 exposed fine periodic striping; rounds 4-5's raster-grid
theories were ruled out by live testing. Round 6 correctly identified the
underlying limitation as pre-existing **A9** (no per-pixel front-layer
knowledge -- overlapping near-coplanar cards blend into a beat pattern),
deferred as a separate follow-up at the time. Round 7 found a second, 2px-
period artifact; round 8 root-caused and fixed it (cell-centre depth
projection so a surface can't occlude its own sub-samples) -- confirmed by
rebuild, periodic lines gone. Round 9 found and fixed residual speckle on
flat/camera-facing surfaces (a minimum tile span, since near-flat tiles'
real depth range could collapse below what the two rasterizers agree on) --
confirmed by rebuild, flat fabric clean. What remained after round 9 (dark
squares of under-garment popping through the dress under camera motion;
faint squarish patches in dense hair vs. Exact OIT) is exactly the round-6
A9 limitation, not a new bug and not fixable by further tile-range tuning.
**Round 10 implements A9**: a full-resolution front-key raster pass (pass
3) records the two nearest distinct transparent depths/alphas per pixel via
32-bit image atomics; pass 2 gives the front fragment exact weight
`alpha_F` (transmittance 1), the second fragment exact source-over weight
`alpha_S(1-alpha_F)`, and only the third-and-deeper layers still fall back
to the volume, bounded by `(1-alpha_F)(1-alpha_S)`. Implemented per the
plan's design with one structural deviation (a bind-stack ordering fix,
required because the plan's literal pass-3 placement predates rounds 1-9's
restructuring) and one deliberately skipped part (debug modes 16/17, an
image-unit conflict in the resolve shader) -- both documented in the
round-10 implementation-status section. Not yet built or tested. Everything
above this note is earlier rounds' history, kept for the record; read
bottom-up for the current picture, or jump straight to the round-10
sections near the end.

Author: chanayane@firestorm. Date: 2026-09-03. Original status (round 1,
superseded by the note above): **unresolved after three build/test cycles.
Handed off for a fresh (higher-effort) read. Two real, confirmed
pre-existing bugs were found and fixed along the way (see "Confirmed fixes
kept" below) but the top-level symptom -- flicker and tile-shaped
corruption on hair -- is still present after the second fix attempt, now
asymmetric (right side of the avatar only). Do not implement a third fix
without new evidence; this doc exists so the next pass does not repeat the
same reasoning.**

## Symptom, current state (after fix attempt 2, rebuilt and tested)

- Corruption on the right side of the avatar's hair; left side clean. See
  the screenshot the user attached to their report (not stored in-repo):
  dark/black blocky patches eaten out of hair strands, sitting in a grid
  pattern, right side of the head/ponytail only.
- Flicker (frame-to-frame instability at the crown of the hair, modes 0/12/
  14) is back too, after seeming address by earlier work.
- This is a regression from the pre-A5 visual state, which had neither of
  these problems (per the user's own commit bisection, see history below).

## How we got here (chronological, condensed -- full blow-by-blow was in
this file's previous revision; git history has it if needed)

1. **User discovered the flicker** on the crown of the avatar's hair,
   absent on a pre-session build. Two rounds of wrong-theory investigation
   (A2 depth-test flip, A8 full-res pass-1 rewrite) produced real,
   independently-useful fixes for *other* problems but did not touch the
   actual cause. Both fully reverted; not relevant to what remains broken
   now except as ruled-out territory.
2. **User's own commit bisection** (rebuilding the viewer at specific
   historical commits and visually comparing -- not code analysis) found
   the actual introduction point: commit `55928e97da` (A5), which flipped
   `RenderAVBOITTileRange`'s default from `1` to `0`. The commit
   immediately before it looked *better* than anything since ("beautiful"
   hair) but is where the flicker starts.
3. A5's own plan entry had already named the deferred fix ("Option A": wire
   per-tile ranging to all alpha geometry, not just GLTF). User approved
   implementing it.
4. **Fix attempt 1**: fed the per-tile range from
   `FSAVBOIT::rasterizeConservativeBounds()`'s exact-proxy pass
   (`avboitBoundsF.glsl`, raw untextured geometry, no alpha test). First
   build showed severe static (non-flickering) corruption -- a jagged,
   blocky black region carved out of the hair.
5. **Bug found and fixed (confirmed real, kept in all later attempts):**
   `avboitVolumeC.glsl` had two unrelated compute passes both gated on
   `if (avboitPass == 12)`, dispatched from the same program
   (`gAVBOITVolumeProgram`). The first (a transmittance-validity
   diagnostic, dispatched from `finishDirectExtinction()`) always matched
   first and `return`s, making the second (the per-tile range's reset-to-
   empty-interval pass, dispatched from `rasterizeConservativeBounds()`)
   permanently unreachable dead code. This predates the whole session --
   confirmed present at A7, HEAD before any of this investigation started.
   Consequence: the tile-range buffer's `(minimum, maximum)` words were
   never reset each frame; `atomicMin`/`atomicMax` only ever narrow a
   stored range, so once written the range degrades monotonically across
   arbitrarily many frames (or starts from uninitialized buffer contents on
   the first frame) and never recovers. Fixed by renumbering the second
   block to `avboitPass == 13` and updating its one CPU dispatch site.
   Rebuilt: **corruption reduced but not eliminated** -- same blocky tile-
   grid character, less of it.
6. **Fix attempt 2** (current code state, described in full below):
   diagnosed the residual corruption as attempt 1 feeding the range from
   architecturally the wrong pass -- the exact-proxy pass draws raw,
   untextured geometry with no alpha test, which does not cover the same
   pixels as the alpha-tested hair fragments the later capture pass
   actually draws. Reverted attempt 1's `avboitBoundsF.glsl` changes
   entirely, and instead made the **material-tested occupancy pass**
   (`render_pass(true)` in `renderPostDeferredCapture()`, which runs the
   real alpha/PBR/fullbright shaders with real textures and alpha testing)
   run unconditionally every frame instead of only in debug mode 6. This
   pass already contained a correct, unmodified call to
   `avboit_reduce_tile_range()` (in `avboitCaptureF.glsl`'s pass-0 branch)
   that simply never used to execute for ordinary content.
   **Rebuilt and tested: still broken, now asymmetric (right side only),
   and the flicker -- which seemed to be a separate axis from the blocky
   corruption -- is back too.**

At this point the user asked for investigation to stop and this handoff
doc to be written instead of a third blind attempt.

## Current code state (uncommitted, on top of A7)

Four files carry uncommitted changes implementing fix attempt 2 (attempt
1's `avboitBoundsF.glsl` changes are fully reverted -- confirmed via
`git diff` showing zero delta there):

- `indra/newview/app_settings/shaders/class1/deferred/avboitVolumeC.glsl`
  -- the pass-12/13 renumbering (kept from attempt 1, still believed
  correct) plus its comment.
- `indra/newview/fsavboit.cpp` -- `tileRange()`'s default flipped back to
  `true`; `renderPostDeferredCapture()`'s occupancy block restructured so
  `render_pass(true)` (material-tested occupancy) and the GLTF occupancy
  block both run unconditionally instead of being gated behind
  `debugMode() == 6`; the pass-12/13 CPU dispatch site updated to match.
- `indra/newview/app_settings/settings.xml` -- `RenderAVBOITTileRange`
  default `1`, comment rewritten to describe the corrected feed.
- `doc/ayanestorm-oit-performance-audit-plan.md` -- status log rows
  documenting both fix attempts (see that file directly for exact wording).

Full diff of the code changes (doc/settings.xml diffs omitted, prose above
covers them):

```diff
diff --git a/indra/newview/app_settings/shaders/class1/deferred/avboitVolumeC.glsl b/indra/newview/app_settings/shaders/class1/deferred/avboitVolumeC.glsl
@@ (inside main(), after the avboitPass == 12 diagnostic block, before it used to be avboitPass == 12 again)
     // Resets the per-tile depth range to an empty interval. atomicMin and
-    // atomicMax in raster pass 0 close it around the transparency actually
-    // present, and an untouched tile keeps minimum > maximum so the capture
-    // shader falls back to the global curve.
-    if (avboitPass == 12)
+    // atomicMax in the material-tested occupancy pass (raster pass 0) close
+    // it around the transparency actually present, and an untouched tile
+    // keeps minimum > maximum so the capture shader falls back to the
+    // global curve.
+    //
+    // Pass number 13, not 12: pass 12 above (the transmittance-validity
+    // diagnostic) already claims that value on the same program
+    // (gAVBOITVolumeProgram dispatches both from fsavboit.cpp), so the two
+    // blocks collided -- this reset was unreachable dead code ...
+    if (avboitPass == 13)
     {
         ivec2 tile_count = avboit_range_tile_count();
         if (all(lessThan(pixel, tile_count)))
         {
             uint range = avboit_tile_range_offset() +
                 (uint(pixel.y) * uint(tile_count.x) + uint(pixel.x)) * 2u;
             avboitWork[range] = 0xffffffffu;
             avboitWork[range + 1u] = 0u;
         }
         return;
     }

diff --git a/indra/newview/fsavboit.cpp b/indra/newview/fsavboit.cpp
@@ bool tileRange()
-    // Off by default (A5): pass 0 only feeds this in debug mode 6 or for
-    // GLTF alpha geometry, so it is currently inert for ordinary content.
+    // A5 revised: the material-tested occupancy pass in
+    // renderPostDeferredCapture() now runs unconditionally (was debug mode
+    // 6 only), feeding this from real alpha-tested fragments every frame
+    // instead of only for GLTF content. Back on by default.
     static LLCachedControl<bool> tile_range(
-        gSavedSettings, "RenderAVBOITTileRange", false);
+        gSavedSettings, "RenderAVBOITTileRange", true);

@@ FSAVBOIT::renderPostDeferredCapture(), occupancy-raster block
         rasterizeConservativeBounds();
-        const bool compare_static_proxy = debugMode() == 6;
-        if (compare_static_proxy)
-        {
-            render_pass(true);
-        }
-        else
+        render_pass(true);
+        // GLTF scene geometry is traversed outside the alpha spatial-group
+        // draw maps used by rasterizeConservativeBounds() and render_pass(),
+        // so it needs its own occupancy pass regardless of debug mode.
         {
-            // GLTF scene geometry is traversed outside the alpha spatial-group
-            // draw maps used by rasterizeConservativeBounds(). Give it the
-            // same material-tested occupancy pass before building the warp.
             LLGLDepthTest depth_test(GL_TRUE, GL_FALSE, GL_LEQUAL);
             LLGLDisable blend(GL_BLEND);
             sCaptureActive = true;
             LL::GLTFSceneManager::instance().render(false, false);
             LL::GLTFSceneManager::instance().render(false, true);
             LL::GLTFSceneManager::instance().render(false, false, true);
             LL::GLTFSceneManager::instance().render(false, true, true);
             sCaptureActive = false;
         }

@@ FSAVBOIT::rasterizeConservativeBounds()
     gAVBOITVolumeProgram.uniform1i(pass, 9);
     glDispatchCompute(groups_x, groups_y, 1u);
-    // Reset the per-tile depth range before any capture pass reduces into it.
+    // Reset the per-tile depth range before any capture pass reduces into
+    // it. Pass 13, not 12: pass 12 is a different, unrelated compute step
+    // (transmittance-validity diagnostic) dispatched later in
+    // finishDirectExtinction() on this same program -- the two collided ...
     const U32 range_groups_x =
         ((sResources.viewportWidth + 15u) / 16u + 15u) / 16u;
     const U32 range_groups_y =
         ((sResources.viewportHeight + 15u) / 16u + 15u) / 16u;
-    gAVBOITVolumeProgram.uniform1i(pass, 12);
+    gAVBOITVolumeProgram.uniform1i(pass, 13);
     glDispatchCompute(range_groups_x, range_groups_y, 1u);
```

## Confirmed fixes kept (both real, both necessary, neither sufficient alone)

1. Pass 12/13 dead-code collision (item 5 above). Confirmed by direct code
   reading of pre-A5/pre-A8/A7 shader source: the collision predates this
   session's work entirely. Grepped after the fix: every `avboitPass ==`
   literal in `avboitVolumeC.glsl` (1 through 13) is now unique.
2. Feeding the range from the material-tested occupancy pass instead of the
   exact-proxy pass (item 6 above). This is the architecturally correct
   design -- it matches the pre-existing, unmodified call site in
   `avboitCaptureF.glsl`'s pass-0 branch, which the original (pre-A5)
   authors clearly intended this to be fed from, given it was already
   there and simply gated to a debug-only code path.

Neither of these is now suspected as the remaining bug's location, but
both are new, real changes to when/how a full compute+raster pipeline
touches this buffer every frame, so they are not ruled out as *interacting*
with something else either.

## What has been independently re-checked this round and ruled out

Before writing this handoff, the following were checked line-by-line and
found internally consistent (i.e., NOT independently corroborated by a
build/test, but no logic bug found on inspection):

- **Tile-count and buffer-offset formulas.** `avboit_range_tile_count()` in
  both `avboitVolumeC.glsl` (compute) and `avboitCaptureF.glsl` (fragment)
  use the identical formula `max((avboitViewport + 15) / 16, 1)`, driven by
  the same `avboitViewport` uniform. `avboit_tile_range_offset()` in both
  files reduces to the same numeric byte offset (`avboit_bounds_offset()` /
  `avboit_proxy_bounds_offset()` are two independently-hand-written
  functions with different names but algebraically identical formulas --
  confirmed by expanding both; this duplication is a maintenance risk but
  not currently a bug).
- **Reset dispatch sizing.** `rasterizeConservativeBounds()`'s pass-13
  dispatch (`range_groups_x/y = ceil(ceil(viewportWidth/16)/16)`) always
  launches at least as many 16x16-thread workgroups as there are tiles,
  given the compute shader's `local_size_x/y = 16`; the in-shader
  `if (all(lessThan(pixel, tile_count)))` guard discards any excess
  threads. No off-by-one found.
- **Dispatch ordering and barriers.** `rasterizeConservativeBounds()` does
  pass 9 (proxy-bounds reset) then pass 13 (tile-range reset), then one
  `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)`, then unbinds -- this
  happens before `render_pass(true)`'s pass-0 raster in
  `renderPostDeferredCapture()`, so the reset is correctly sequenced before
  any write.
- **SSBO binding persistence.** `sResources.work` is bound once to SSBO
  binding point 3 (inside `rasterizeConservativeBounds()`, which runs every
  frame before any raster pass) and nothing else rebinds point 3 to a
  different buffer; GL SSBO bindings persist across program switches, so
  the raster (fragment-shader) programs see the same buffer the compute
  reset just wrote to, without needing their own explicit bind.
- **Write path (`avboit_reduce_tile_range()`) itself.** Reads
  `gl_FragCoord.xy`, computes a tile index via `avboit_range_index()`
  (shared formula with the reader), does a plain `atomicMin`/`atomicMax`
  pair. No asymmetry (no operation that would behave differently for large
  vs. small `gl_FragCoord.x`) found in this function.
- **Resize/reallocation race.** `allocateResources(width, height)` is only
  called from `beginDirectFrame()`, gated on
  `sResources.viewportWidth != width || sResources.viewportHeight != height`
  -- so `sResources.viewportWidth` and the `work` buffer's `tile_count`-
  based sizing are always updated together, atomically, before any frame
  renders at a new size. A stale/undersized buffer after a resize was
  considered and ruled out.
- **finishDirectOccupancy() (runs immediately after the pass-0 occupancy
  raster).** Its compute passes (1, 11, 2, 4, 3) and the A2 cell-depth bake
  it performs do not touch the tile-range buffer region at all -- confirmed
  by reading the function in full. Not a factor.

None of the above produced a concrete bug. This is a list of *ruled-out*
territory, not a confirmed-safe certification -- treat "no logic bug found
on inspection" as weak evidence at best; this investigation has already
been wrong twice on reasoning-only conclusions (see "Lessons" below).

## Open questions / where to look next

1. **Why right side only?** This is the most important new data point and
   was not present (or not reported) after fix attempt 1's corruption,
   which read as more uniform/symmetric ("blocky tile grid" generally, not
   called out as one-sided). Candidates not yet checked:
   - Is the avatar's hair geometry itself asymmetric in a way that changes
     which spatial groups/draw calls touch which screen tiles (e.g., camera
     angle in the test shot puts more hair strand geometry on the right)?
     If so this might not be a bug at all but a coincidence of what's
     visible -- worth asking the user to test with the camera/avatar
     rotated 180 degrees to see if the corruption follows the avatar's
     screen-space position or a fixed side of the viewport.
   - Frustum culling or spatial-partition traversal order differences
     between `rasterizeConservativeBounds()`'s exact-proxy draw and
     `render_pass(true)`'s material-tested draw -- if one pass's spatial
     group iteration skips/reorders draws differently near the edge of the
     view frustum, coverage could differ asymmetrically. Not checked this
     round.
   - Multi-threaded/multi-GPU-context draw submission order: is there any
     tiled/multi-viewport rendering path (VR, multi-monitor) active in the
     user's test that could split left/right processing? Unlikely but not
     ruled out -- ask the user about their display setup.
2. **Is the flicker now the same mechanism as the corruption, or two bugs?**
   Fix attempt 2's rebuild reintroduced both symptoms together. Earlier
   (pre-attempt-1) the flicker was investigated in isolation and pointed
   at A5 as sole cause; the corruption is new (only appeared once tile-
   ranging was made genuinely active by attempt 1). It's possible the
   flicker is inherent to *any* correctly-functioning tile-range feed
   (i.e., the "coarser global curve causes more frequent slice-boundary
   crossings" mechanism theorized in the previous revision of this doc,
   never independently confirmed by a controlled experiment) and the
   corruption is a second, independent, still-unfound bug. Recommend
   designing a discriminating experiment: force `RenderAVBOITTileRange` on
   but somehow bypass just the corruption-causing mechanism (if it can be
   isolated) to see if flicker alone remains.
3. **Debug mode 6 comparison, revisited.** Debug mode 6 used to run
   `render_pass(true)` as a proxy-vs-material *comparison* -- implying
   there's already a diagnostic path (`avboit_compare_proxy_coverage()` in
   `avboitCaptureF.glsl`, writes to `avboitDiagnostic[4..]`) that measures
   divergence between the exact-proxy bounds and the material-tested
   occupancy. This diagnostic counter was not read/reported this round.
   Reading `avboitDiagnostic[4]` (total comparisons) and whatever failure
   counters mode 6 accumulates, on the current broken build, might
   directly quantify how much coverage mismatch exists between the two
   passes without needing to reason about it -- worth trying before further
   code changes.
4. **Was fix attempt 1's more-uniform corruption pattern actually different
   in kind from fix attempt 2's right-side pattern, or just different in
   degree?** Not confirmed either way -- rely on the user's direct
   comparison if they still have both builds' screenshots, since the
   description alone ("blocky tile grid" vs "right side corrupted") is not
   precise enough to say whether these are the same bug manifesting
   differently under different feed passes, or two unrelated bugs.

## Lessons for whoever picks this up

- **Reasoning about GLSL/GPU code from source alone has been wrong twice
  in this investigation** (A2 depth-test theory, A8 pass-1 density theory)
  and both times a real, plausible-sounding mechanism was proposed,
  partially fixed something else real along the way, and did not touch the
  actual cause. The user's own direct commit bisection is what actually
  found the real regression point (A5). Prefer build-and-compare
  experiments over code-reasoning-only conclusions when a theory needs
  confirming, especially for anything involving GPU-side synchronization,
  atomics, or per-pixel coverage.
- **Two "fix it properly" attempts have each individually seemed complete
  and well-reasoned and both left real, visible corruption on rebuild.**
  Don't trust "I traced the offsets/dispatch math and it's consistent" (this
  doc's own "ruled out" section) as equivalent to "verified working" --
  it isn't, and this investigation has the receipts to prove it.
- Standing repo rules apply: read `AGENTS.md` before any work here; only
  read-only git commands are permitted (no `checkout`, `reset`, `stash`,
  etc., even for investigation); never bump
  `FSAVBOIT::shaderCacheRevision()` mid-session; the user builds and tests,
  the assistant does not.

## Repro

Avatar with alpha hair, scene containing GLTF/PBR content, camera zoomed on
the crown, `RenderAVBOITTileRange` at its current default (`1`, i.e. fix
attempt 2's code as currently uncommitted in the tree). Right side of the
avatar's hair shows blocky dark corruption; left side does not. Flicker
present at the crown under camera motion or avatar animation with the
other held still (matches the original, pre-fix-attempt-1 symptom
description).

---

# Fresh diagnosis (Fable 5.1, 2026-09-03) and implementation instructions

Read this section in full before touching code. It supersedes the theories
above. Everything here was verified by reading the current tree
(uncommitted state on top of A7), not by build. Three concrete defects were
found: two hard bugs in the tile-range path and one design gap. Fix all
three in one build.

## What the pre-A5 state actually was (changes the goal)

User timeline: hair looked fine before A2, looked weird from A2 to A4, A5
fixed the weird look but introduced the crown flicker. One mechanism
explains all three:

Pre-A5: `RenderAVBOITTileRange=1`, the pass-13 reset was dead (pass-12
collision) and `beginDirectFrame()` clears the whole `work` buffer to 0
every frame (`glClearBufferData`, `fsavboit.cpp` ~line 1205). So every
tile's range words were `(min=0, max=0)`. `avboit_virtual_depth()` treats
that as a *written* tile (the unwritten test is `min > max`), span 0, pad
`1/16777215`, so every fragment's rescaled depth clamped to `1.0`. Every
alpha fragment in passes 1 and 2, in every tile, landed in the same last
physical slice: no ordering at all, each fragment attenuated by everything
else in its cell (unordered weighted blend).

- Before A2 that was hidden: pass 1's bogus depth test (full-res pixel
  sampled at a cell coordinate) culled most hair cells, so little extinction
  accumulated and hair looked acceptable by accident.
- A2 made pass 1 admit the right cells, so the one-slice mode showed its
  real result: every strand darkened by every other strand -> the "weird"
  look from A2 to A4.
- A5 turned ranging off -> global curve -> real per-slice ordering ->
  correct look, but strands now cross slice boundaries under sub-pixel
  motion on a coarse frame-wide curve -> crown flicker.

So the flicker is ordering engaging, not a bug introduced by A5, and the
pre-A2 look was not a reference. Goal: correct per-tile ranging, which
gives ~100x finer slice spacing over a hair band than the frame-wide curve
and should remove most boundary crossings.

## Bug 1 (hard): pass 1 looks up the wrong tile

Pass 1 rasterizes at volume resolution (`beginDirectRasterPass(1)` sets
`glViewport(volumeWidth, volumeHeight)`), so in pass 1 `gl_FragCoord.xy`
is an 8x8 *cell* index, not a pixel (see `avboit_direct_store()`:
`cell = avboitRasterPass == 0 ? pixel / 8 : pixel`). But
`avboit_virtual_depth()` calls
`avboit_range_index(ivec2(gl_FragCoord.xy))`, which divides by 16 assuming
full-res pixels. Pass 1 therefore reads the range of tile `cell/16`, i.e.
the tile at pixel `pixel/8`: every cell on screen reads a tile from the
top-left 1/8 x 1/8 of the screen. Pass 2 (full res) reads the correct
tile. Extinction is written with one depth mapping and read with another.
Result: blocky (8 px cell / 16 px tile) garbage, black where all of a
pixel's fragments read past the cell's zero-transmittance slice
(`weight == 0` -> `transparent = 0` while `aggregate_alpha` still darkens
the opaque). Which part of the head is hit depends on which top-left tiles
happen to be written, hence "one side only" (screen-left in the latest
screenshot) and camera-dependent. Present in attempt 1 and attempt 2
alike; explains both "corruption reduced but not eliminated" and the
asymmetry. Pre-A5 it was masked because every tile held the same `(0,0)`.

## Bug 2 (hard): tile-rescaled coordinate is pushed through the global warp

`avboit_warped_slice()` feeds `avboit_virtual_depth()` (tile-rescaled
0..1) into `avboitWarp[]`, which compute pass 1 built from *global-curve*
occupancy. The warp is a piecewise map global->physical with empty global
ranges collapsed. A tile whose contents sit at global depth 0.30..0.31 gets
rescaled to 0..1; then 0..0.30 of that lands on the warp's leading empty
range (one slice), 0.30..0.31 spreads across the slices, 0.31..1 collapses
onto the trailing range. So per-tile ranging as written is an arbitrary
monotonic map with heavy collapses: worse than the global curve, never
finer. Same code in `avboitEmissiveF.glsl` and `avboitPbrGlowF.glsl`.
Also compute pass 6: the early-depth quads take
`avboitWork[8 + zero_depth]`, a *global* slice->depth table, meaningless
for a tile with its own mapping (can cull visible fragments -> holes).

## Bug 3 (design): trilinear read across tiles with different mappings

`avboitTransmittance` is sampled with `GL_LINEAR` in xy. A 16 px range
tile is 2x2 cells, so about half of all pixels blend cells from two tiles
whose z axes mean different depths once ranging is on. Not the cause of
the black blocks, but it will show as seams after bugs 1 and 2 are fixed.

## Fix specification

Keep: pass 12/13 renumbering (correct and necessary). Keep
`RenderAVBOITTileRange` default `1` and `tileRange()` default `true`.

### Step 0. Feed: revert attempt 2's occupancy change, use attempt 1's feed

In `renderPostDeferredCapture()` restore the original structure:
`render_pass(true)` only when `debugMode() == 6`, else the GLTF-only
block. Attempt 2 adds a full extra alpha-material pass every frame for
nothing. Attempt 1's feed from `avboitBoundsF.glsl`'s exact-proxy path is
fine: it draws the same geometry (static and rigged, full resolution;
`gl_FragCoord.xy` is a pixel there, see `cell = gl_FragCoord.xy / 8`) with
the same opaque-depth clamp, only without alpha test, so its range is a
superset of the material-tested one. A superset range costs a little
resolution, never correctness. Attempt 1 "failed" because of bugs 1 and 2,
not because of the feed.

Re-implement attempt 1 exactly as row `AVBOIT A8 superseded` of
`ayanestorm-oit-performance-audit-plan.md` describes:

- `avboitBoundsF.glsl`: add `uniform int avboitTileRange;`, port
  `avboit_global_normalized_depth()` (byte-identical to
  `avboitCaptureF.glsl`), `AVBOIT_RANGE_TILE`, `avboit_tile_range_offset()`
  (= `avboit_bounds_offset() + volumeSize.x*volumeSize.y*5u`),
  `avboit_range_index()`, `avboit_reduce_tile_range()`. Call
  `avboit_reduce_tile_range(bounded_window_depth)` inside the
  `if (avboitExactProxy != 0)` branch, next to the existing
  `atomicOr(avboitOccupancy[exact_bin], 1u)`.
- `fsavboit.cpp` `rasterizeConservativeBounds()`: upload `avboitTileRange`
  (`tileRange() ? 1 : 0`) to both `gAVBOITBoundsProgram` and
  `gAVBOITSkinnedBoundsProgram` after each bind.
- Leave the `avboit_reduce_tile_range()` call in `avboitCaptureF.glsl`'s
  pass-0 branch (harmless; keeps GLTF alpha and mode 6 fed).

### Step 1. Fix bug 1: tile lookup by full-res pixel

`avboitCaptureF.glsl` (and the two glow shaders for symmetry):

```glsl
// Pass 1 rasterizes at volume (8x8-cell) resolution, so gl_FragCoord is a
// cell index there. Every cell lies inside one 16-pixel range tile.
ivec2 avboit_full_res_pixel()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    return avboitRasterPass == 1 ? pixel * 8 : pixel;
}
```

Replace every `avboit_range_index(ivec2(gl_FragCoord.xy))` with
`avboit_range_index(avboit_full_res_pixel())`. `avboitRasterPass` is
already a uniform in all three files.

### Step 2. Fix bug 2: tile mode bypasses the warp, linear slices

`avboitCaptureF.glsl`:

- Factor the range read into one helper used everywhere:

```glsl
// True, with the padded [minimum, minimum+span] global-normalized range,
// when ranging is on and pass 0 wrote the tile containing full-res `pixel`.
// Padding must stay identical to avboitVolumeC.glsl pass 6.
bool avboit_tile_range(ivec2 pixel, out float minimum_depth, out float span)
{
    if (avboitTileRange == 0) return false;
    uint range = avboit_range_index(pixel);
    uint stored_minimum = avboitWork[range];
    uint stored_maximum = avboitWork[range + 1u];
    if (stored_minimum > stored_maximum) return false;
    minimum_depth = float(stored_minimum) / 16777215.0;
    float maximum_depth = float(stored_maximum) / 16777215.0;
    float pad = max((maximum_depth - minimum_depth) * 0.0625, 1.0 / 16777215.0);
    minimum_depth -= pad;
    maximum_depth += pad;
    span = max(maximum_depth - minimum_depth, 1.0 / 16777215.0);
    return true;
}

// Physical slice coordinate of a window depth for the tile containing
// `pixel`: linear across the tile's own range (the range already removed
// the empty depth, so the global warp is bypassed), else the global warp.
float avboit_slice_for_pixel(ivec2 pixel, float window_depth)
{
    float minimum_depth, span;
    if (avboit_tile_range(pixel, minimum_depth, span))
    {
        float global_depth = avboit_global_normalized_depth(window_depth);
        return clamp((global_depth - minimum_depth) / span, 0.0, 1.0) *
            float(AVBOIT_DIRECT_SLICES - 1u);
    }
    return avboit_warped_slice_global(window_depth);
}
```

- Rename the existing `avboit_warped_slice()` to
  `avboit_warped_slice_global()` and make it use
  `avboit_global_normalized_depth()` directly. Delete
  `avboit_virtual_depth()`; the rescale now lives only in
  `avboit_slice_for_pixel()`.
- Pass 1: `slice_coordinate = avboit_slice_for_pixel(avboit_full_res_pixel(), gl_FragCoord.z)`.
- Pass 2 (own cell): same call with `ivec2(gl_FragCoord.xy)`. In tile mode
  the sampling bias is applied in physical slices, not through
  `biased_window_depth`: `sample_slice = max(slice_coordinate - avboitSamplingBias, 0.0)`.
  In global mode keep the existing `biased_window_depth` ->
  `avboit_warped_slice_global()` path. Own-share correction unchanged (it
  only uses `slice_coordinate` and `sample_slice`).
- `avboit_reduce_tile_range()` unchanged except for Step 1.

### Step 3. Fix bug 3: manual 4-cell filtering in pass 2 (tile mode)

Replace the single `texture(avboitTransmittanceSampler, vec3(sample_xy, ...))`
in pass 2, when `avboitTileRange != 0`, with a manual bilinear read so each
cell is sampled in its *own* tile's mapping of this fragment's depth:

```glsl
float avboit_cell_transmittance(ivec2 cell, float sample_slice)
{
    sample_slice = clamp(sample_slice, 0.0, float(AVBOIT_DIRECT_SLICES - 1u));
    int lower = int(floor(sample_slice));
    int upper = min(lower + 1, int(AVBOIT_DIRECT_SLICES) - 1);
    float a = texelFetch(avboitTransmittanceSampler, ivec3(cell, lower), 0).r;
    float b = texelFetch(avboitTransmittanceSampler, ivec3(cell, upper), 0).r;
    return mix(a, b, fract(sample_slice));
}

// Bilinear over the 2x2 cells around this pixel, each cell read in the
// mapping its own tile used in pass 1 (cells of different tiles have
// different z meanings, so the hardware trilinear read is wrong across a
// tile edge). `biased_window_depth` is only used for cells whose tile is
// unwritten (global warp path).
float avboit_front_transmittance(float window_depth, float biased_window_depth)
{
    vec2 cell_coordinate = gl_FragCoord.xy / 8.0 - 0.5;
    ivec2 base = ivec2(floor(cell_coordinate));
    vec2 f = fract(cell_coordinate);
    float result = 0.0;
    for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x)
    {
        ivec2 cell = clamp(base + ivec2(x, y), ivec2(0), avboitVolumeSize - ivec2(1));
        float minimum_depth, span, slice;
        if (avboit_tile_range(cell * 8, minimum_depth, span))
        {
            float global_depth = avboit_global_normalized_depth(window_depth);
            slice = clamp((global_depth - minimum_depth) / span, 0.0, 1.0) *
                float(AVBOIT_DIRECT_SLICES - 1u) - avboitSamplingBias;
        }
        else
        {
            slice = avboit_warped_slice_global(biased_window_depth);
        }
        float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
        result += w * avboit_cell_transmittance(cell, slice);
    }
    return result;
}
```

Keep the existing `texture()` read for the global path so global mode is
byte-for-byte unchanged. A neighbour tile that does not cover this depth
clamps to slice 0 or 127, which is exactly "in front of all" or "behind
all" of that cell's content: correct.

Glow shaders (`avboitEmissiveF.glsl`, `avboitPbrGlowF.glsl`): in tile mode
read own cell only, `avboit_cell_transmittance(pixel / 8, slice)` with
`slice = avboit_slice_for_pixel(pixel, gl_FragCoord.z)` (no bias, as now).
Port the helpers; these files already duplicate the capture helpers.
Global path unchanged.

### Step 4. Pass 6 early-depth quads: per-tile depth (`avboitVolumeC.glsl`)

Pass 6 already iterates the same 16 px tile grid (`pixel` is the tile
index). After computing `zero_depth`, when the tile's range is written,
compute the quad depth from the tile mapping instead of
`avboitWork[8u + zero_depth]`:

```glsl
uint range = avboit_tile_range_offset() +
    (uint(pixel.y) * uint(tile_count.x) + uint(pixel.x)) * 2u;
uint stored_minimum = avboitWork[range];
uint stored_maximum = avboitWork[range + 1u];
uint depth_bits;
if (stored_minimum <= stored_maximum)
{
    // Same padding as avboit_tile_range() in avboitCaptureF.glsl.
    float minimum_depth = float(stored_minimum) / 16777215.0;
    float maximum_depth = float(stored_maximum) / 16777215.0;
    float pad = max((maximum_depth - minimum_depth) * 0.0625, 1.0 / 16777215.0);
    minimum_depth -= pad; maximum_depth += pad;
    float span = max(maximum_depth - minimum_depth, 1.0 / 16777215.0);
    // End of the saturating slice: everything at or past it is invisible.
    float normalized = clamp(minimum_depth + span *
        float(zero_depth + 1u) / float(AVBOIT_SLICES - 1u), 0.0, 1.0);
    depth_bits = floatBitsToUint(avboit_window_depth(
        avboit_high_virtual_depth(normalized * float(AVBOIT_VIRTUAL_SLICES))));
}
else
{
    depth_bits = avboitWork[8u + zero_depth];
}
```

No new uniform: with ranging off no tile is ever written, so the global
branch is taken. `avboitDepthRange`/`avboitLinearization` are program
uniforms already set on `gAVBOITVolumeProgram` earlier in the frame by
`finishDirectOccupancy()` and persist.

### Step 5. Docs/comments

- `settings.xml` `RenderAVBOITTileRange` comment: fed by the exact-proxy
  bounds pass; slices are linear across the tile's range and bypass the
  frame-wide warp.
- One short status-log row in `ayanestorm-oit-performance-audit-plan.md`
  when implemented (the diagnosis row is already there).
- No shader cache revision bump. User builds.

### Self-checks before handing back

- grep: no remaining `avboit_range_index(ivec2(gl_FragCoord.xy))` in any
  shader; `avboit_virtual_depth` gone from all three raster shaders.
- Both `gAVBOITBoundsProgram` and `gAVBOITSkinnedBoundsProgram` get
  `avboitTileRange`.
- With `RenderAVBOITTileRange=0` the behaviour is unchanged (renames and
  comments only on that path).

### Expected result and next step if flicker remains

Corruption and asymmetry gone in one build; tile edges seam-free (Step 3).
If crown flicker persists with ranging on (compare by toggling
`RenderAVBOITTileRange` live), the remaining candidate is the one A8
targeted: one fragment per 8x8 cell in pass 1 sampling thin strands
unstably. Revisit A8 with the bounded CAS only after that comparison.

---

# Round 2 (Fable 5.1, 2026-09-03): cell-sized dark dashes after the fix

Build result of the spec above: flicker gone, hair colour exact, clothing
no longer shows through. Remaining: small dark rectangles/dashes in a
regular (8 px cell) grid on hair, most visible where the hair is dense.
Implementation was checked against the spec and is faithful; the dashes
are a different mechanism that the fine per-tile slices expose.

## Cause

Pass 1 rasterizes at cell resolution: one fragment per cell per triangle,
sampled at one point of the 8x8 block. A hair card whose opaque core
(alpha ~1, optical depth 11.09 in the wide layout) lands on that sample
saturates its lane in one go, so `avboit_add_extinction()` records
`avboitExtinctionOverflowDepth = slice`, and compute pass 5 then treats
every slice from there on as fully extinct: it stores `exp(-11.09)` at the
overflow slice, breaks, and every later slice keeps the `0.0` that pass 3
cleared it to. So the whole 8x8 cell says "nothing behind slice k is
visible", even though that opaque core covers only some of the cell's 64
pixels.

- Global curve: a hair mass in one cell occupies one or two coarse slices,
  so k is the slice of essentially every strand in the cell. Fragments read
  one slice in front (bias) or the overflow slice itself (`1.5e-5`, not
  zero). Never exactly zero, so the weighted average still yields the hair
  colour.
- Per-tile linear slices: the same hair mass now spans ~100 slices. Every
  strand more than a slice behind the sampled opaque core reads `0.0`.
  For pixels of that cell whose own strands are all behind k, every
  fragment gets `weight = alpha * 0 = 0`; resolve then outputs
  `transparent = 0` while `aggregate_alpha` (exact, per pixel, from pass
  2's accumulated extinction) still darkens the opaque: black dash the
  size of a cell, following opaque strand cores. Additionally pass 6 now
  emits an early-depth quad at the end of slice k in the tile mapping, so
  fragments beyond it are culled in pass 2 and drop their extinction too:
  those pixels show the background through the hair instead (holes).

This is not a bug in the spec; it is the single-sample-per-cell extinction
(the thing A8 targeted) being treated as hard occlusion once slices are
fine. Two cheap changes make it harmless; the real fix (A8-style full-res
pass 1 with 1/64 contributions) stays a later option.

## Fix

### A. Floor the front transmittance in tile mode (`avboitCaptureF.glsl`)

In pass 2, tile path only (`avboitTileRange != 0`), after the own-share
correction:

```glsl
// A cell's extinction is one sample per 8x8 block, so a saturated slice
// says "everything behind is hidden" for pixels that opaque core never
// covered. Keep a floor so such fragments still average into the colour
// with equal weights instead of vanishing (weight 0 -> black with the
// exact per-pixel opacity still applied). 1/16384 is the smallest normal
// fp16 value, so the R16F weight sum stays exact.
front_transmittance = max(front_transmittance, 1.0 / 16384.0);
```

Ordering is unaffected: a genuinely front fragment keeps weight ~alpha,
occluded ones get ~alpha/16384. Global path unchanged.

Same floor in the two glow shaders' tile branch (`front = max(front, 1.0 / 16384.0)`).

### B. No early-depth quads for ranged tiles (`avboitVolumeC.glsl` pass 6)

The quad culls pass-2 fragments beyond the cell's saturation slice, and a
culled fragment adds neither weight nor extinction, so the pixel goes
transparent (hole) where the cell said "hidden" but the pixel was not
covered. With coarse global slices the culled fragments were always behind
a saturated slice at pixel level too; with fine tile slices they are not.

In the pass-6 block added by the spec: when `stored_minimum <= stored_maximum`
(written tile), do **not** emit a quad for that tile (skip the
`atomicAdd(avboitWork[5], ...)` append entirely). Keep the global path
(`avboitWork[8u + zero_depth]`) for unwritten tiles. The per-tile depth
computation added last round can be deleted. Cost: ranged tiles lose the
early-Z rejection for pass 2; measure later, correctness first.

### C. Nit (optional, same build)

`avboit_front_transmittance()`: hardware bilinear for a pixel is centred at
`(pixel + 0.5) / 8 - 0.5` in cell units, the code uses `pixel / 8 - 0.5`
(half a pixel off). Use `(vec2(pixel) + 0.5) / 8.0 - 0.5`.

### Self-check

- `RenderAVBOITTileRange=0`: no behavioural diff (floor and quad skip are
  inside tile-mode branches / written-tile branches only).
- grep pass 6: no remaining use of the tile-mapped `depth_bits` path.

## If dashes persist after A+B

Then the remaining darkness is not from `weight == 0`. Next discriminator:
debug mode 10 (normalized colour, blue where `weight <= 0`) and mode 12
(weight bands) on the affected area; report which one shows the dash
pattern before changing anything else.

## Round 2 implementation status (2026-09-03)

A, B, and the C nit all implemented exactly as specified:

- **A**: `avboitCaptureF.glsl` floors `front_transmittance` to `1/16384`
  right after the own-share correction, inside a new `avboitTileRange != 0`
  guard (global path untouched). Same floor added in both
  `avboitEmissiveF.glsl` and `avboitPbrGlowF.glsl`'s tile branch (`front =
  max(front, 1.0 / 16384.0)`, right after the manual bilinear read).
- **B**: `avboitVolumeC.glsl` pass 6's per-tile depth computation (the block
  this doc's Round 1 spec added) is deleted entirely. A written (ranged)
  tile is now detected with a plain `avboitWork[range] <=
  avboitWork[range + 1u]` check and, when true, no early-depth quad is
  emitted for that tile at all -- the `if (zero_depth < AVBOIT_SLICES)`
  block only runs (using the original global `avboitWork[8u + zero_depth]`
  path) for unranged tiles.
- **C**: `avboit_front_transmittance()`'s cell-space coordinate changed from
  `vec2(pixel) / 8.0 - 0.5` to `(vec2(pixel) + 0.5) / 8.0 - 0.5`.

Self-checks: grepped pass 6 for the deleted tile-mapped `depth_bits` path --
zero remaining references (the one remaining `floatBitsToUint(` hit in the
file is an unrelated, pre-existing site elsewhere). With
`RenderAVBOITTileRange=0` the floor's guard and the `tile_ranged` check both
evaluate to the pre-Round-2 behaviour (`tile_ranged` is always false, since
the write path that could set `avboitWork[range] <= avboitWork[range+1]`
never fires when ranging is off).

Not yet built/tested. If cell-sized dashes/holes persist after this round,
follow this doc's "If dashes persist after A+B" section above (debug modes
10 and 12) before making further changes.

---

# Round 3 (Fable 5.1, 2026-09-03): cell-grid moire on hair and fabric

After round 2: black dashes gone. Remaining, versus Exact OIT: a fine
8 px grid / moire across dense hair, and faint blocky shading on the
dress fabric. Same cause as round 2, now without the black: pass 1 samples
each 8x8 cell at one point, so which strand or garment layer is "front"
for a whole cell is decided by one pixel's worth of geometry. Coarse
global slices hid this (everything in a cell shared a slice); per-tile
slices resolve the layers, so each cell now shows the ordering its one
sample happened to pick: a block pattern. The transmittance floor keeps
it from going black but cannot make the per-cell decision smooth.

Fix: supersample pass 1 so a cell's extinction is the average over the
block, not one sample. This is A8's idea, done with a tunable factor and
the bounded CAS loop (A8's perf regression came from the unbounded loop
and going straight to 64 samples per cell).

## Spec

Add `constexpr U32 AVBOIT_PASS1_SUBSAMPLE = 4;` in `fsavboit.cpp` next to
`AVBOIT_SCALE` (S = samples per axis per cell: 4 -> 16 samples/cell,
16x more pass-1 fragments than now, 4x fewer than A8's full-res; 8 is
full-res and equals A8). Start at 4; the user can try 8 after measuring.

1. `fsavboit.cpp`
   - `allocateVolume()`: `gAVBOITCellDepthTarget.allocate(volumeWidth * S, volumeHeight * S, GL_R8, true)`.
   - `beginDirectRasterPass(1)`: `glViewport(0, 0, volumeWidth * S, volumeHeight * S)`.
   - `configureDirectRasterShader()`: upload new int uniform
     `avboitPass1Subsample` = S to every raster program (same pattern as
     `avboitTileRange`). Also upload it to `gAVBOITCellDepthProgram` in
     `finishDirectOccupancy()` before its draw.
   - `finishDirectOccupancy()` cell-depth bake: unchanged otherwise (the
     target is now S times larger per axis, the full-screen triangle
     covers it).

2. `avboitCellDepthF.glsl`: each output texel now covers an
   `(8/S) x (8/S)` pixel block:
   ```glsl
   uniform int avboitPass1Subsample;
   ...
   int block = 8 / avboitPass1Subsample;
   ivec2 base = ivec2(gl_FragCoord.xy) * block;
   for (int y = 0; y < block; ++y)
   for (int x = 0; x < block; ++x) { ... max over base + (x, y) ... }
   ```
   Still the farthest depth of the block (conservative, as A2 requires).

3. `avboitCaptureF.glsl`
   - `uniform int avboitPass1Subsample;`
   - Cell mapping in `avboit_direct_store()`:
     ```glsl
     ivec2 cell = avboitRasterPass == 0 ? pixel / 8 :
                  avboitRasterPass == 1 ? pixel / avboitPass1Subsample : pixel;
     ```
     (then the existing clamp).
   - `avboit_full_res_pixel()`: pass 1 -> `pixel * (8 / avboitPass1Subsample)`.
   - Pass-1 branch: after computing `optical_depth`, scale it:
     `optical_depth /= float(avboitPass1Subsample * avboitPass1Subsample);`
     before the lower/upper split, so the S*S sub-samples of a cell sum to
     the block's mean optical depth. Wide layout quantum is 11.09/65535 =
     1.7e-4; alpha 0.5 at S=4 gives 0.043 -> 254 quanta, fine. Narrow
     layout is not viable with this; leave `RenderAVBOITWideExtinction`
     default on and say so in its settings comment.
   - `avboit_add_extinction()` CAS loop: raise the cap from 64 to 256
     attempts, keep the give-up-as-saturated fallback. Do NOT make it
     unbounded (that was A8's perf regression).
   - Keep round 2's transmittance floor; it is still needed for cells
     where an opaque core dominates, just far less often.

4. Glow shaders (`avboitEmissiveF.glsl`, `avboitPbrGlowF.glsl`): same
   cell mapping change and uniform for consistency (they return early in
   pass 1, so this is bookkeeping only). `avboit_full_res_pixel()` there
   likewise.

5. Pass gates in the material shaders (`fullbrightF.glsl`, class3
   `materialF.glsl`, alpha/pbr/GLTF) are unchanged: pass 1 still runs the
   same shaders, only more fragments.

## Self-check

- With S = 8 the code must equal A8's mapping (`pixel / 8` in pass 1,
  `optical_depth / 64`).
- `gl_FragCoord` in pass 1 is now in S-per-cell units everywhere it is
  used: `cell`, `avboit_full_res_pixel()`; grep `avboitRasterPass == 1`
  to confirm no other consumer.
- Global mode (`RenderAVBOITTileRange=0`) also gets the smoother
  extinction; that is intended.

## Expected

Grid/moire on hair and fabric blocks gone or reduced to the residual
8-px cell resolution of the volume itself (soft, not blocky). Report FPS
at S=4; if acceptable try S=8. If moire remains at S=8, the remaining
difference to Exact OIT is the volume's 8-px xy resolution, which is a
design limit (A9 territory), not a bug.

## Round 3 implementation status (2026-09-03)

Implemented exactly as specified, S = `AVBOIT_PASS1_SUBSAMPLE = 4` (constant
next to `AVBOIT_SCALE` in `fsavboit.cpp`, not a runtime setting):

- `fsavboit.cpp`: `allocateVolume()` sizes `gAVBOITCellDepthTarget` to
  `volumeWidth/Height * AVBOIT_PASS1_SUBSAMPLE`; `beginDirectRasterPass(1)`'s
  `glViewport` scaled to match. `avboitPass1Subsample` uploaded through
  `configureDirectRasterShader()`'s existing `getUniformLocation`-guarded
  pattern (every raster program that declares the uniform gets it; the ones
  that don't -- none currently -- are silently skipped, same as
  `avboitTileRange`) and separately to `gAVBOITCellDepthProgram` in
  `finishDirectOccupancy()`'s cell-depth bake block. CAS loop cap in
  `avboit_add_extinction()` raised from 64 to 256 attempts, fallback kept,
  still bounded (not unconditional -- that was A8's actual perf regression).
- `avboitCellDepthF.glsl`: block size is now `8 / avboitPass1Subsample`
  pixels per axis instead of a fixed 8; still takes the block's farthest
  (conservative) opaque depth.
- `avboitCaptureF.glsl`: new `avboitPass1Subsample` uniform.
  `avboit_full_res_pixel()` and the `cell` mapping in `avboit_direct_store()`
  both gained a third branch for `avboitRasterPass == 1` that scales by
  `avboitPass1Subsample` instead of the old fixed `* 8` / implicit `pixel`
  passthrough. Pass 1's `optical_depth` divided by `avboitPass1Subsample^2`
  before the slice split, so a cell's `avboitPass1Subsample^2` sub-cell
  fragments sum to the block's mean instead of one sample's value. Round 2's
  transmittance floor is unchanged/untouched, still needed for the rarer
  remaining single-sample-dominated cells.
- Both glow shaders: gained the `avboitPass1Subsample` uniform declaration
  only (bookkeeping/uniform-set consistency) -- their pass-1 branch returns
  immediately without using `cell` or `pixel`, so no mapping logic changes
  there, matching the spec's expectation.
- `settings.xml`: `RenderAVBOITWideExtinction`'s comment now also notes that
  pass 1's per-sample scaling shrinks the narrow layout's already-marginal
  quantum further, reinforcing why it should stay on.

Self-checks passed: with `AVBOIT_PASS1_SUBSAMPLE` set to 8, `cell = pixel /
avboitPass1Subsample` reduces to the old fixed `pixel / 8`,
`avboit_full_res_pixel()`'s `pixel * (8 / avboitPass1Subsample)` reduces to
`pixel * 1`, and `optical_depth /= (8*8)` reduces to A8's `/64` -- confirmed
algebraically equivalent to full-resolution pass 1. Grepped every
`avboitRasterPass == 1` consumer across all three raster shaders; the only
two that read `gl_FragCoord`-derived pixel/cell coordinates in that branch
(`avboit_full_res_pixel()`, the `cell` ternary) both scale by the new
factor, the glow shaders' pass-1 branch reads neither. Not yet built/tested
-- shader source, uniform set, and render-target/viewport sizing changed;
needs a cache bump, held per standing guidance.

**Rebuilt and tested at S=4.** Round 3 did resolve the coarse 8px-block
moire from round 2 (confirmed by the user via mode 5 and mode 0
comparison). But a new, different-looking artifact appeared on top: a fine,
very regular, periodic striping pattern across the hair mass, following the
hair's curvature -- visibly finer-grained than the 8px cell grid or the
16px tile grid (compare the two full-resolution screenshots the user
attached: Exact OIT is smooth with no banding at this zoom level; AVBOIT
shows tight, evenly-spaced horizontal-ish stripes across the whole visible
hair mass). Not the same symptom as round 2/3's cell-block moire -- this
looks like true periodic banding, not per-cell sampling noise.

**Discriminating test performed:** toggled `RenderAVBOITTileRange` to `0`
live (no rebuild) on the same camera position/hair area. Result: the fine
stripes disappear entirely, but doing so reintroduces the original A5-era
bug this whole per-tile-ranging effort exists to fix (underlying clothing
layers show through the dress again -- the "hair colour exact, clothing no
longer shows through" result from round 1 depends on ranging being on).
**This conclusively ties the striping to the per-tile ranging mechanism
itself**, not to anything specific to round 3's pass-1 supersampling (S=4
vs the old S=1 doesn't matter for this test) and not to an unrelated,
coincidental cause -- ranging on, both before and after round 3, produces
stripes; ranging off, neither round's code produces them.

## Round 4 candidate diagnosis (not yet confirmed, offered as a starting
point only)

Per-tile ranging spreads a narrow depth band (the hair's own occupied depth
in that tile, potentially just a centimetre or less) across most or all of
the 128 physical slices (`AVBOIT_DIRECT_SLICES`) -- that is deliberately the
whole point (it is what fixed the exact-hair-colour and clothing-showing-
through bugs in round 1). The stored transmittance volume
(`sResources.transmittance`, `avboitTransmittanceSampler`) is linearly
filtered along all three axes (`GL_LINEAR` on the 3D texture, confirmed via
`fsavboit.cpp`'s `glTexParameteri` calls), so a smoothly-varying real depth
should read back smoothly in principle. Two things not yet checked that
could still produce fine, regular banding under that setup:

- Each physical slice's stored value is `exp(-cumulative_extinction)`,
  which is not linear in depth even when the extinction itself varies
  smoothly -- combined with `avboit_add_extinction()`'s saturating,
  quantized (8 or 16-bit lane) accumulation and the round-2/3 mechanisms
  that make one slice's extinction depend on which of a cell's samples
  happened to saturate, a genuinely smooth depth gradient across many
  adjacent pixels could still read back as a staircase if each pixel's
  fragment lands in a different physical slice under a per-tile curve
  that is only ~100 slices wide across the tile's whole depth range --
  i.e., quantization along the depth axis, not the xy axes rounds 2/3
  targeted. Not measured or confirmed; the fineness and high regularity
  of the visible stripe period (much finer than the 8px cell or 16px tile
  grid) is the main reason to suspect this over a resolution-grid cause.
- `avboit_front_transmittance()`'s manual 2x2-cell bilinear filter (added
  in the original per-tile-ranging fix spec, "Bug 3") operates at 8px cell
  granularity, coarser than the visible stripe period in the screenshots
  -- probably not the direct cause of *this* pattern, but not ruled out,
  since it is the one place per-tile-ranging deliberately bypasses the
  hardware's xy-trilinear read in favour of a manual one.

Neither of these has been independently confirmed; they are offered as
starting candidates, not conclusions, given this investigation's repeated
history of reasoning-only theories turning out wrong or incomplete (see
"Lessons for whoever picks this up" above). A build-and-compare experiment
(e.g., a debug mode that visualizes the raw physical-slice index a pixel
reads, at full resolution, next to the smooth Exact OIT reference) would be
far more conclusive than further code reading alone.

## Status after round 3

Round 3's fix (pass-1 supersampling) is real, working, and should be kept
-- it did fix the coarse block moire it targeted. The newly-visible fine
striping is a different, likely pre-existing mechanism that round 3's fix
did not introduce and cannot fix (tied to per-tile ranging itself, per the
toggle-off test above), not yet root-caused. Handed off again for a fresh
round rather than a further blind fix attempt, per the same discipline
used after round 1's asymmetric-corruption handoff.

---

# Round 4 (Fable 5.1, 2026-09-03): fine periodic stripes with ranging on

The round-4 candidates above are close but not it. The stripes are not
depth-axis quantization of the stored transmittance and not the manual
xy filter. They are the fragment reading a share of **its own
extinction**, with a strength that cycles once per physical slice of
depth. Period = one slice. With ranging on a slice is a fraction of a
millimetre, so the cycle repeats every few pixels along the depth
gradient of a curved hair sheet: tight, regular stripes following the
curvature. With ranging off a slice is centimetres thick, so the same
cycle spans the whole hair and reads as smooth shading. Round 3 did not
introduce it; round 1 did, by making slices thin.

## Mechanism (pass 2, `avboit_direct_store()`)

Let `c` be the fragment's slice coordinate, `f = fract(c)`, `k = floor(c)`.

- Pass 1 splats the surface's optical depth `od` as `(1 - f) * od` into
  slice `k` and `f * od` into slice `k + 1`.
- Pass 5 stores post-slice transmittance, so `T[k]` includes
  `(1 - f) * od` of this very surface; `T[k - 1]` includes none of it.
- Pass 2 reads at `sample = c - avboitSamplingBias` with bias `1.0`, i.e.
  `mix(T[k - 1], T[k], f)`. The `T[k]` term carries the surface's own
  share, weighted by `f`. Own pollution is
  `f * (1 - exp(-(1 - f) * od))`: zero at `f = 0`, zero at `f = 1`, a
  bump in the middle. That is a periodic darkening of `weight` with
  period exactly one slice of depth.
- The existing own-share correction never fires:
  `read_overlap = 1 - (floor(c) - floor(c - 1)) = 0` for every fragment
  when bias is `1.0`, so it is dead code in both modes.

Trying to subtract the own share exactly is not possible any more: since
round 3 a cell's extinction from this surface is `od / S^2` per covering
sub-sample and the fragment does not know how many sub-samples its
surface covered.

## Fix: bias of two slices in tile mode

Read `mix(T[k - 2], T[k - 1], f)` instead. Neither texel contains the
surface's own splat (that lives in `k` and `k + 1`), and the read is
continuous in `c`: at `f -> 1` it tends to `T[k - 1]`, and when `k`
increments `f` restarts at 0 with `T[(k + 1) - 2] = T[k - 1]`. Surfaces in
front in slices `< k - 1` are fully included, slice `k - 1` proportionally.
Surfaces sharing slice `k` are ignored; with ranging on a slice is well
under a millimetre, so that is nothing. This is the PDF's original 2.0
bias, which the code backed off to 1.0 only because compaction made
global slices thick. Tile slices are thin, so 2.0 is the right value
there. Global path stays at `avboitSamplingBias` (unchanged).

### Zero-cost confirmation first (no rebuild)

The tile path already uses the live `RenderAVBOITSamplingBias` uniform.
Set it to `2.0` in the debug settings on the striped shot. Stripes should
vanish immediately. Then set it back to `1.0` (the global path also reads
it) and implement the code change below so tile mode does not depend on
the setting.

### Code (`avboitCaptureF.glsl`)

```glsl
// Tile mode reads two whole slices in front so neither sampled texel can
// contain the fragment's own splat (slices floor(c) and floor(c)+1). With
// bias 1.0 the read mixed in T[floor(c)], which carries (1-fract(c)) of
// this surface's own extinction, producing a darkening that cycles once
// per slice of depth -- fine stripes along the depth gradient once slices
// are sub-millimetre. Tile slices are thin enough that ignoring content
// less than one slice in front costs nothing.
const float AVBOIT_TILE_SAMPLING_BIAS = 2.0;
```

- Own-cell read in pass 2, tile branch:
  `sample_slice = max(slice_coordinate - AVBOIT_TILE_SAMPLING_BIAS, 0.0);`
- `avboit_front_transmittance()`, tile branch of each of the 4 cells:
  replace `- avboitSamplingBias` with `- AVBOIT_TILE_SAMPLING_BIAS`.
- Leave the `own_share` / `read_overlap` block as is (it evaluates to
  zero in the tile path; not worth touching the global path now).
- Glow shaders: they read at `slice_coordinate` with no bias in tile
  mode, so glow includes its own extinction; also subtract
  `AVBOIT_TILE_SAMPLING_BIAS` there for consistency (`max(..., 0.0)`).

### Self-check

- Global mode diff: none.
- Fragments in slices 0..1 clamp to `T[0]`; the range padding puts the
  nearest content at slice ~7, so no fragment is affected.

### If stripes remain after the live test at bias 2.0

Then the cause is elsewhere; report that result before any code change.
Next candidate would be `fract`-dependent splitting in pass 1 interacting
with the wide-lane rounding (`+ 0.5` in `avboit_add_extinction()`) at
`od / S^2` scale, which would show as noise rather than clean stripes,
so it is unlikely.

## Round 4 result: live test failed, own-splat theory ruled out

**Live test performed exactly as specified** (`RenderAVBOITTileRange = 1`,
`RenderAVBOITSamplingBias` raised from `1.0` to `2.0`, no rebuild, same
striped shot): **the stripes did not vanish.** This is a clean, falsifiable
negative result against round 4's specific mechanism -- own-splat
pollution at `T[floor(c)]` from a bias-1.0 read predicts the stripes go
away at bias 2.0 (both sampled texels move strictly in front of the
fragment's own pass-1 splat, per the mechanism section above), and they did
not, on the same live, no-rebuild code path the mechanism itself is
described against. This rules out the own-splat-pollution theory as *the*
cause of the visible stripes -- not merely "unconfirmed" but actively
falsified by the one test the round-4 spec proposed as sufficient to
confirm it. `RenderAVBOITSamplingBias` was restored to `1.0` after the
test, per the round-4 instructions.

This does not by itself rule out that the described mechanism exists at
all (it may still be real and simply too small relative to whatever the
dominant cause actually is), only that it is not the explanation for what
is visible in the screenshots. The search for the striping's actual cause
continues; round 3's supersampling fix and rounds 1-2's fixes are
unaffected and should be kept regardless of how the striping is eventually
resolved.

---

# Round 5 (Fable 5.1, 2026-09-03): stripes = tilted-sheet self-occlusion at cell resolution

Round 4's negative result is informative: any per-slice-period mechanism
(own splat, slice quantization) is gone at bias 2.0, so the stripe period
is not one slice. The remaining periodic structure in the pipeline is the
8 px cell, and there is a mechanism that produces a sawtooth with exactly
that period once slices are thin.

## Mechanism

A hair sheet or garment surface is tilted relative to the view. Across
one 8 px cell its depth changes by some amount `D`. Since round 3, pass 1
splats the sheet's 16 sub-samples of that cell into the slices covering
`D`; with per-tile ranging `D` is many slices (a 2 mm change across a
cell at 0.2 mm per slice is 10 slices). The cell's transmittance volume
therefore says: "this surface starts at slice c_min and is fully in place
by slice c_max". A pixel of that same surface at slice `c_p` reads
`T[c_p - bias]`, which already contains every sub-sample of its own sheet
that is nearer than `c_p - bias`. So along the depth gradient, the first
pixel of a cell sees the sheet unoccluded and the last pixel sees itself
occluded by most of its own sheet. At the next cell it resets: a sawtooth
with period one cell (8 px) along the gradient, smeared but not removed
by the 2x2-cell bilinear (the neighbour cell holds the same sheet shifted
by one cell's worth of depth). Stripes follow the surface's depth
contours, are regular, and go away with ranging off because there a cell's
whole sheet fits inside one thick slice and can never occlude itself.
Bias 2.0 cannot fix it because the self-occlusion spans `D`, not two
slices. Round 3 made it worse (16 sub-samples per cell fill the band
densely) but S = 1 already had it from neighbouring cells.

General statement: a cell-resolution volume cannot resolve ordering finer
than the depth spread of a surface across the filter footprint. Sampling
closer than that spread reads the surface's own occlusion. Global mode
satisfied this by accident (thick slices); tile mode must enforce it.

## Live test first (no rebuild)

`RenderAVBOITTileRange = 1`. Raise `RenderAVBOITSamplingBias` in steps:
`4`, `8`, `16`, `32`. Prediction: stripes fade progressively and are gone
once the bias exceeds the sheet's per-footprint spread in slices; the
spacing of the stripes, measured on a 100 % screenshot along the
direction they repeat, is 8 px (one cell). If stripes are unchanged even
at 32, this theory is wrong too: stop and report. Restore `1.0` after.

## Fix: derivative-based bias in tile mode (`avboitCaptureF.glsl`)

Bias by the surface's own depth spread across the bilinear footprint
(own cell plus one neighbour each side, 16 px), measured with screen-space
derivatives of the slice coordinate, plus one slice of margin:

```glsl
// Tile mode: a cell-resolution volume cannot separate a surface from its
// own sub-samples elsewhere in the filter footprint, whose depths differ
// by the surface's tilt. Read from in front of the whole footprint's
// worth of this surface (16 px = own cell plus bilinear neighbours),
// otherwise a tilted sheet occludes itself with a sawtooth of one cell
// period along its depth gradient. fwidth() is per pixel; clamp so a
// silhouette edge between two distant strands does not push the read to
// slice 0.
const float AVBOIT_TILE_FOOTPRINT_PIXELS = 16.0;
const float AVBOIT_TILE_MAX_SPREAD_SLICES = 32.0;

float avboit_tile_sampling_bias(float slice_coordinate)
{
    float spread = fwidth(slice_coordinate) * AVBOIT_TILE_FOOTPRINT_PIXELS;
    return 1.0 + min(spread, AVBOIT_TILE_MAX_SPREAD_SLICES);
}
```

- Compute `slice_coordinate` before any divergent control flow in
  `avboit_direct_store()` (it already is: computed right after the pass-0
  early return, which is uniform per draw). Compute
  `float tile_bias = avboit_tile_sampling_bias(slice_coordinate);` right
  after it, still before the `if (avboitRasterPass == 1)` branch, so the
  derivative is taken in uniform control flow.
- Pass 2 tile branch: `sample_slice = max(slice_coordinate - tile_bias, 0.0);`
- `avboit_front_transmittance()`: pass `tile_bias` in and use it instead
  of the constant for all four cells (same bias everywhere, applied in
  each cell's own mapping).
- Round 4's `AVBOIT_TILE_SAMPLING_BIAS` constant goes away (the `1.0`
  margin inside the function replaces it).
- Glow shaders: same function and use; their `slice_coordinate` is
  computed inside the pass-2 branch today, move the computation and the
  fwidth before the branch as above.
- Global path unchanged.

Trade-off, stated plainly: ordering between two different surfaces closer
in depth than one surface's own 16 px tilt spread is lost. That is the
volume's real resolution limit, and global mode was already far coarser.
Hair strands crossing at steep angles keep correct ordering when their
depth separation exceeds the spread; near-coplanar strands average, which
is what Exact OIT shows as smooth anyway.

## Self-check

- With `fwidth == 0` (surface facing the camera) the bias is exactly
  `1.0`: same as the code before round 4.
- `RenderAVBOITTileRange = 0`: no diff.
- No `discard` between the fwidth and the shader's start within
  `avboit_direct_store()` (the caller's alpha discard happens before the
  call, which is fine: helper invocations still supply derivatives).

## If the live test fails (unchanged at bias 32)

Report the measured stripe spacing in pixels at 100 % and whether the
stripes move with the camera (attached to screen pixels) or with the hair
(attached to the surface). Screen-attached with a fixed spacing points to
a raster-grid source (cells or tiles); surface-attached points to a
depth-domain source. Do not change code before that.

## Round 5 result: live test failed, tilted-sheet theory ruled out; new constraint found

**Live test performed exactly as specified** (`RenderAVBOITTileRange = 1`,
`RenderAVBOITSamplingBias` stepped through `4`, `8`, `16`, `32`, no
rebuild): the visual pattern of the stripes *changes* at each step (which
specific stripes are visible/where shifts), but they never fade, thin out,
or improve/worsen in overall severity -- present and equally strong at
every bias value tested, including 32. This rules out round 5's tilted-
sheet self-occlusion mechanism the same way round 4's own-splat mechanism
was ruled out: a real falsification, not just "unconfirmed". The fact that
the pattern *shifts* rather than staying frozen as bias changes is itself
informative -- it means changing the read depth does change *which* slice
range gets sampled (the bias mechanism itself works as coded), it just
doesn't touch whatever is actually causing the banding.

**New, decisive data point (per the "if the live test fails" instructions
above): zoom and pan behavior.** The user reports:

- Stripes get **thinner when zooming in, wider when zooming out** -- they
  scale with screen magnification.
- **Panning** the camera causes some stripes to disappear and others to
  appear, rather than a fixed pattern sliding across the screen.

**This is the opposite of what a screen-space/raster-grid artifact would
do.** An artifact tied to the 8px cell grid, the 16px tile grid, or any
other fixed-pixel-count structure keeps a constant *pixel* size regardless
of zoom (zooming the camera changes the field of view / world-to-screen
mapping, not the pixel dimensions of the render targets or the cell/tile
grids, which are allocated in screen pixels). A pattern that shrinks in
screen pixels as you zoom in is a pattern that is fixed in *world space* or
*depth space* and only appears at a roughly constant *angular* or *world*
size -- consistent with something tied to actual scene/surface geometry
(individual hair strand geometry, UV/texture-space structure, or a
depth-domain quantity that maps to world units) rather than anything in
AVBOIT's screen-aligned cell/tile/slice grids. The panning behavior (stripes
appearing/disappearing at specific locations rather than translating with
the view) further supports this being tied to specific surface locations,
not a screen-locked overlay.

**This rules out both round 4 and round 5's entire category of theory**
(both assumed a fixed-pixel-count raster structure -- one slice's worth of
self-pollution, one cell's worth of tilt spread -- as the periodic driver).
Neither survives this test: a raster-grid-locked cause cannot explain
zoom-dependent stripe width or content-dependent appear/disappear under
panning. `RenderAVBOITSamplingBias` restored to `1.0` after the test.

**Open again, more constrained this round:** whatever is producing the
striping scales with the scene/geometry, not with AVBOIT's fixed pixel
grids. Worth considering before the next round: whether this could be
present in the *source hair texture/geometry itself* (e.g., normal-mapped
strand card texture detail that Exact OIT's per-pixel exact ordering
resolves correctly but AVBOIT's approximate, coarser-than-exact ordering
does not, independent of any of AVBOIT's OWN grids) rather than a bug
introduced by any of rounds 1-5's mechanisms at all. That would explain
both the geometry-locked scaling behavior and why four consecutive raster-
grid-based theories have all been cleanly falsified by direct, specified
tests rather than just narrowly missed.

---

# Round 6 (Fable 5.1, 2026-09-03): stripes are card-texture interference, the A9 limit

Rounds 4 and 5 were both falsified by their own tests, and the zoom/pan
data says the stripes are locked to the surface, not to any AVBOIT grid.
Combined with "bias changes which stripes show but never their strength",
one explanation is left, and the plan already names it: A9.

## Mechanism

Hair is stacked strand cards, each with a fine strand alpha texture.
Where two near-coplanar cards A (front) and B (behind) overlap, correct
compositing is per pixel: B shows only through A's alpha gaps. AVBOIT
attenuates B by the **cell-averaged** extinction of A in the slices in
front of B's read point, not by A's alpha at this pixel. Two effects:

- Cards a few slices apart with a tilt spread larger than their
  separation interleave in depth across the cell, so B's read lands in
  front of most of A's extinction. B keeps nearly full weight.
- Even when A's extinction is fully in front, it is a cell mean: B is
  dimmed uniformly, not masked by A's strands.

So at every pixel the normalized average shows A's texture plus B's
texture at comparable weight. Two fine, slightly different strand
patterns added together produce a beat pattern: regular stripes, spacing
set by the textures and their projected scale (thinner when zooming in,
changing with the camera as different card pairs overlap), independent of
the read bias (bias only changes *which* pair is merged). Global mode
looked smooth because it merged every card in a cell into one slice:
ten textures averaged is mush, two textures averaged is moire. Per-tile
ranging is working as designed; it exposed that the volume has no
per-pixel front-layer knowledge. Exact OIT has it, hence smooth.

This is exactly the A9 finding in the plan ("the summed integral cannot
express order inside a slice; only per-pixel knowledge of which fragment
is in front can"). No bias, slice count, sub-sampling or filter change
can fix it; rounds 1 to 3 remain correct and necessary and stay.

## One confirming test (no rebuild), then stop tuning tile ranging

Same striped shot, ranging on:

- Debug mode 9 (aggregate alpha, per-pixel exact): stripes must be
  **absent**. If they show here, the pattern is in the geometry/alpha
  itself and Exact OIT would show it too; report that.
- Debug mode 10 (normalized colour): stripes **present**.
- Debug mode 14 (front transmittance bands): expect the striped region
  mostly yellow/red (rear cards not suppressed).

That combination confirms the colour average is mixing rear cards and
closes this line of investigation.

## Decision

Implement **A9** (per-pixel front key: two nearest transparent depths and
alphas per pixel, front fragment gets weight `alpha_F` with transmittance
1, second gets exact source-over weight, deeper layers keep the volume
weight). Its design is already written in
`ayanestorm-oit-performance-audit-plan.md` section A9, with the source-over
proof. It is also the fix for the hair-lighter-than-vanilla and lash
density findings logged there. Do not implement further tile-range
changes before A9 is in and re-evaluated.

Status of tile ranging to carry forward: keep on by default; keep rounds
1 to 3 (feed from bounds pass, full-res tile lookup, linear per-tile
slices, manual 4-cell filter, transmittance floor, no early-depth quads on
ranged tiles, S = 4 pass-1 supersampling); bias constant back to the
`avboitSamplingBias` uniform in the tile path (rounds 4 and 5's constants
and fwidth bias bought nothing; remove them to keep the path simple).

## Round 6 confirmation: blocks match debug mode 5 (per-cell zero-transmittance depth)

User observation: the square artefacts line up with debug mode 5's
yellow/orange cells (cells whose summed extinction reached the effective-
zero cutoff, `avboit_zero_extinction()`, at some slice; blue = never).

What that means, precisely:

- Pass 5 integrates per cell. Once the cumulative optical depth reaches
  11.09 it records `zero_depth`, breaks, and every later slice keeps the
  `0.0` pass 3 cleared it to. With S = 4, one fully opaque card covering
  the cell contributes 16 x 0.69 = 11.09, so any cell under two
  opaque-ish hair cards saturates.
- The cutoff is a **cell** property, but the pixel's front strand is a
  **pixel** property. In a yellow cell whose cutoff comes from other
  pixels' nearer cards, this pixel's own front strand reads `T = 0`
  (floored to 1/16384) exactly like everything behind it: every fragment
  at the pixel gets the same negligible weight and the colour becomes the
  flat average of all strands, back-facing ones included. In the
  neighbouring blue cell the front strand keeps weight ~alpha and
  dominates. Different colour per cell -> blocks that follow the mode-5
  map. The card-texture interference in the round-6 note is the same
  effect seen inside cells where several cards share the cutoff.

Why no bias, slice, filter or floor change can remove it: the missing
information is per-pixel ("which fragment is in front here"), and the
volume only holds per-cell sums. Continuing the integration past the
cutoff instead of zeroing the tail would only move the cliff from
exp(-11) to R16F underflow (~exp(-17)); not worth a build.

Decision unchanged and now evidence-backed: **A9** (per-pixel front key).
With it the front fragment at every pixel gets weight `alpha_F` with
transmittance exactly 1 regardless of the cell's cutoff, the second layer
gets the exact source-over weight, and only deeper layers depend on the
volume. That removes the per-cell cliff for the layers that decide the
colour. Keep rounds 1 to 3 as they are.

Further confirmation from the user: debug mode 12 (weight sum bands) shows
the stripes, mode 14 (average front transmittance bands) shows them to
some extent. Both diagnostics expose `alpha * T_front`, which is exactly
the quantity that collapses to the floor wherever the pixel's front strand
sits behind its cell's cutoff. Mode 9 (per-pixel exact aggregate alpha)
is expected to be stripe-free; if it is not, report before A9 work.

## Round 7: mode 12 / mode 14 screenshots show a 2 px horizontal raster pattern

New evidence (user screenshots, modes 12 and 14): on the dress sleeve, a
single surface, both modes show perfectly horizontal, screen-aligned lines
with a period of about 2 px, identical on the curved top and on the
vertical sides of the sleeve. Hair shows the same overlaid on strand
structure. Horizontal-only and orientation-independent means a raster
mechanism with a 2-row period, not geometry and not the 8 px per-cell
cutoff (that one is real too, it matched mode 5, but it makes 8 px
blocks, not 2 px lines).

The only 2 px structure in the pipeline is pass 1 since round 3: S = 4
rasterizes at half resolution, one sub-sample per 2x2 screen pixels. Why
that would change pass 2's weight per *row* only is not established by
reading; do these live tests on the dress sleeve in mode 12 and report
each result before any code change:

1. `RenderAVBOITTileRange = 0`. Lines still there? (yes -> not tile
   related at all; no -> tile path.)
2. `RenderAVBOITTileRange = 1`, `RenderAVBOITSamplingBias = 32`. Lines on
   the sleeve still there? (yes -> not a read-position effect.)
3. Take a 100 % screenshot of the sleeve in mode 12 and measure the line
   period in pixels exactly (2 px? 4 px?), and whether it is locked to the
   screen (resize the window by a few pixels in height and see if the
   lines move relative to the sleeve).
4. State which code is in the build: are round 4/5's tile-bias constant
   and `fwidth` bias still present, or removed as round 6 asked?

Do not implement anything from this round until those four answers are in
the doc.

### Round 7 answers (user, 2026-09-03)

1. Ranging off: no stripes. 2. Ranging on, bias 32: stripes unchanged.
3. Zoomed sleeve in mode 12: period 2 px; horizontal over most of the
   sleeve, vertical in patches where the surface's depth gradient is
   horizontal; solid cells where the surface faces the camera.
4. Checked by grep: rounds 4/5 bias code is NOT in the build; the tile
   path uses the `avboitSamplingBias` uniform, so the bias-32 test was
   valid.

# Round 8 (Fable 5.1, 2026-09-03): cell-centre depth projection

## Cause (final)

Per-tile ranging gives a tile's slices to the depth present in that tile.
A single tilted surface therefore spans the whole slice range of its tile
(1 cm of depth across a 16 px tile -> 127 slices, ~8 slices per pixel).
Pass 1 at S = 4 splats the surface's 16 sub-samples per cell at their own
depths, 2 px and therefore ~16 slices apart. A pass-2 pixel reads in front
of its own depth and finds some of its own surface's nearer sub-samples
there; how many flips with row (or column) parity along the gradient. One
near-opaque sub-sample is 11.09/16 = 0.69 optical depth, so each extra
one halves the weight: mode 12's blue/grey alternation. A constant bias
only changes which sub-samples count (bias 32 = 2 sub-samples), so the
pattern persists at any bias short of the tile span; ranging off puts the
whole cell in one slice so nothing of the surface is ever in front of
itself. This is round 5's mechanism made discrete by the sub-sample grid;
round 5's clamped derivative bias could not cover a 127-slice spread and
was the wrong shape of fix anyway (it throws away ordering).

## Fix: evaluate the surface at the cell centre in both passes

A cell-resolution volume stores one depth profile per cell. Give each
surface one depth per cell: extrapolate the fragment's window depth to
the cell centre with screen-space derivatives. All of a planar surface's
sub-samples in a cell then land in the same slice (pass 1), and every
pass-2 pixel of that surface in that cell reads the same canonical depth,
so a surface can never occlude itself, at any tilt. Two different
surfaces keep their separation (both projected to the same point).

`avboitCaptureF.glsl` (tile mode only; global path unchanged):

```glsl
// Window depth of this fragment's surface extrapolated to the centre of
// the volume cell it belongs to. `pixel_scale` is screen pixels per
// gl_FragCoord unit (pass 1 rasterizes at 8/S screen pixels per unit).
// Derivatives are only valid inside one primitive; at a silhouette quad
// they are garbage, so clamp the extrapolation to a few slices' worth of
// the fragment's own gradient (fwidth) to keep such pixels near their true
// depth instead of flying off.
float avboit_cell_centre_depth(vec2 cell_centre_fragcoord)
{
    float z = gl_FragCoord.z;
    vec2 d = cell_centre_fragcoord - gl_FragCoord.xy;
    float dz = dFdx(z) * d.x + dFdy(z) * d.y;
    float limit = fwidth(z) * 8.0; // at most one cell's worth of slope
    return clamp(z + clamp(dz, -limit, limit), 0.0, 1.0);
}
```

Call sites (all before any divergent branch, derivatives need uniform
control flow within the function; `avboit_direct_store()` is entered from
uniform code in every raster shader):

- Pass 1 (viewport = volume * S, one unit = 8/S screen px): cell centre in
  fragcoord units = `vec2(cell) * S + S * 0.5` where
  `cell = ivec2(gl_FragCoord.xy) / avboitPass1Subsample`. Use the
  projected depth for `slice_coordinate` only when the own tile is ranged
  (`avboit_tile_range()` true); otherwise keep `gl_FragCoord.z`.
- Pass 2 (full res): cell centre = `vec2(pixel / 8) * 8.0 + 4.0`. Use the
  projected depth for the own-cell `slice_coordinate` when the own tile is
  ranged. In `avboit_front_transmittance()`, for each of the 4 cells use
  that cell's own centre (`vec2(cell) * 8.0 + 4.0`) with the same
  extrapolation (pass `gl_FragCoord.z`, `dFdx`, `dFdy`, `fwidth` values
  computed once at the top and handed in as arguments; never call the
  derivative functions inside the loop). Neighbour extrapolation is up to
  12 px, still one primitive for a sleeve; for hair the clamp bounds it.
- Bias in tile mode: back to a constant 2.0 in physical slices (now that
  no sub-sample spread exists, `mix(T[k-2], T[k-1], f)` excludes the
  surface's own splat exactly and is continuous; see round 4). Keep
  `avboitSamplingBias` for the global path.
- Glow shaders: same projection for their tile-mode read (own cell only,
  as now), bias 2.0.
- `avboit_reduce_tile_range()` in the bounds pass: unchanged (range only
  needs to cover the surface; projection stays inside the padded range
  except at the clamp, which `clamp(..., 0, 1)` in the rescale handles).

## Self-check

- Camera-facing surface: `dz = 0`, identical to today.
- Global mode: no diff.
- No derivative call inside a loop or after a data-dependent branch.
- Pass-1 cell centre arithmetic: with S = 4 and cell (10, 3), centre is
  (42, 14) in pass-1 fragcoord units = screen (84, 28) = centre of the
  8x8 cell at (80..87, 24..31). Verify the same for pass 2: cell (10, 3)
  -> (84, 28).

## Expected

Mode 12 on the sleeve: uniform blue, no lines at any zoom. Hair: strand
structure only. The 8 px cell cutoff blocks (mode 5 correlation) may still
remain in dense hair; that is the A9 item and is next if visible.

## Round 8 implementation status (2026-09-03)

Implemented exactly as specified in `avboitCaptureF.glsl` and both glow
shaders (`avboitEmissiveF.glsl`, `avboitPbrGlowF.glsl`):

- `avboit_cell_centre_depth(cell_centre_fragcoord, z, dz_dx, dz_dy,
  slope_limit)`: new helper, one copy per file (the two glow shaders
  already duplicate several capture-shader helpers rather than sharing a
  file). Ported identically to the spec's snippet, restructured to take
  `z`/`dz_dx`/`dz_dy`/`slope_limit` as parameters instead of calling
  `dFdx`/`dFdy`/`fwidth` internally, so both call sites -- the single
  own-cell computation and the four-cell loop inside
  `avboit_front_transmittance()` -- can share one derivative evaluation
  taken once in uniform control flow, satisfying the spec's "never call
  derivative functions inside the loop" instruction structurally rather
  than by convention.
- `avboitCaptureF.glsl`: derivatives (`fragment_z`, `dz_dx`, `dz_dy`,
  `slope_limit`) and `cell_centre_fragcoord` (branched on
  `avboitRasterPass == 1` per the spec's two formulas) computed once right
  after `avboit_full_res_pixel()`, before the pass-1/pass-2 branch. The
  own-cell `slice_coordinate` computation was inlined from the former
  `avboit_slice_for_pixel()` helper (removed -- see below) so the tile
  range only needs reading once per fragment instead of twice; uses the
  cell-centre-projected depth when the fragment's own tile is ranged,
  falls back to `avboit_warped_slice_global(fragment_z)` (raw depth,
  unprojected) otherwise, matching pre-round-8 global-mode behaviour
  exactly. `avboit_front_transmittance()` gained the four derivative
  parameters and now computes each of its four cells' own centre depth
  before mapping to a slice, replacing the old shared `window_depth`
  parameter. Bias source changed from the `avboitSamplingBias` uniform to
  the new `AVBOIT_TILE_BIAS_SLICES = 2.0` constant in both the own-cell
  read and the four-cell filter, exactly as specified; `avboitSamplingBias`
  is now read only in the global-mode `else` branch. The old
  `avboit_slice_for_pixel()` helper is deleted (dead code once its call
  site was inlined); confirmed via grep it has no remaining references in
  this file.
- Both glow shaders: gained their own `avboit_cell_centre_depth()` and
  `AVBOIT_TILE_BIAS_SLICES`. Their existing `avboit_slice_for_pixel()`
  helper (an `-1.0`-sentinel variant distinct from the capture shader's
  former one) is kept, since these shaders' single call site doesn't
  benefit from inlining it the way the capture shader's two call sites
  did; its doc comment updated to note the caller must pass the projected
  depth. Pass 2's read (their only branch that reaches the tile/global
  split, since pass 1 returns immediately) now computes the cell centre
  and derivatives, passes the projected depth into
  `avboit_slice_for_pixel()`, and -- new -- subtracts
  `AVBOIT_TILE_BIAS_SLICES` from the result before the two-tap read, where
  previously this pass used the raw, un-biased own-cell slice.

Self-checks: grepped `avboitSamplingBias` in `avboitCaptureF.glsl` -- its
only remaining use is inside the global-mode branch, confirming the tile
path is fully switched to the new constant. Grepped `dFdx`/`dFdy`/`fwidth`
in all three files -- every call site is in each function's uniform-control-
flow prologue (before any pass or tile-range branch), never inside
`avboit_front_transmittance()`'s loop. No `discard` between shader entry
and the derivative calls in any of the three files (the capture shader's
only early `return` is the uniform-branch pass-0 block, which precedes the
derivative calls entirely; the glow shaders call `avboit_store_glow()`
unconditionally with no alpha test before it). `RenderAVBOITTileRange = 0`:
every projection is computed unconditionally but only ever read inside an
`avboit_tile_range()`-true branch, so global mode's output is unchanged
(some now-wasted ALU work computing an unused projection, not a behaviour
difference). Not yet built/tested -- shader source changed; needs a cache
bump, held per standing guidance.

# Round 9 (Fable 5.1, 2026-09-03): residual speckle = tile span below depth precision

Round 8 built and tested; implementation checked line by line against the
spec and found faithful (units consistent in both passes, derivatives in
uniform control flow, own tile gating correct). Periodic lines gone.
Remaining: grey speckle in mode 12, **heaviest on the flat, camera-facing
skirt in the front view, much lighter in the side view where the same
dress is tilted**; some along hair strand edges.

## Cause

Flat-facing tiles have almost no depth range. The padded span then
collapses to the key quantum (`pad = max(span * 0.0625, 1/16777215)`, so
a padded span of ~3 units of the 24-bit key). 127 slices over that is a
few tens of nanometres per slice, while fp32 window depth at two metres
resolves about 2 um. The half-res pass-1 rasterizer and the full-res
pass-2 rasterizer interpolate the same plane with different rounding, so
their depths disagree by many slices from precision alone, and the
surface's own splat lands in front of the 2-slice-biased read at random:
speckle. Tilted tiles have real spans (0.1 mm/slice), where 2 um is
nothing, hence smooth. Curvature (round 9's first draft) is not the
driver; that draft is withdrawn.

## Fix: minimum tile span (all shader copies must match)

In `avboit_tile_range()` (`avboitCaptureF.glsl`, both glow shaders) and
the pass-6 copy in `avboitVolumeC.glsl` (kept consistent even though
ranged tiles emit no quads), after the existing pad:

```glsl
// Never spread the 127 slices over less depth than the rasterizer can
// resolve. 6e-4 in normalized log depth is ~1 cm at 2 m and ~7 cm at
// 20 m (tolerable: distant tiles need less), giving >= 80 um per slice
// against ~2 um of fp32 window-depth precision. Widened symmetrically so
// the tile's content stays centred.
const float AVBOIT_TILE_MIN_SPAN = 6.0e-4;
...
if (span < AVBOIT_TILE_MIN_SPAN)
{
    minimum_depth -= (AVBOIT_TILE_MIN_SPAN - span) * 0.5;
    span = AVBOIT_TILE_MIN_SPAN;
}
```

Placed after `span = max(maximum_depth - minimum_depth, 1.0 / 16777215.0);`.
`minimum_depth` may go slightly negative near the camera; the rescale
already clamps to [0, 1], no other change needed. Global path untouched.

## Optional, same build (cheap insurance for folds)

Slope-proportional margin on the tile-mode read, own cell and the four
filter cells, and the glow shaders:
```glsl
float margin = AVBOIT_TILE_BIAS_SLICES + min(fwidth(slice_coordinate) * 2.0, 16.0);
```
with `fwidth(slice_coordinate)` computed once next to the other
derivatives (uniform control flow), replacing the bare
`AVBOIT_TILE_BIAS_SLICES`. Covers the pass-1 (4 px) vs pass-2 (2 px)
derivative baseline disagreement on curved surfaces.

## Expected

Mode 12: uniform blue on the flat skirt in the front view; folds and
side view unchanged or better. If flat fabric is still speckled with the
minimum span in, report the value tried; raising it to 2e-3 is the next
step before anything else.

## Round 9 implementation status (2026-09-03)

Required fix implemented in all three fragment shaders that compute a tile
span (`avboitCaptureF.glsl`, `avboitEmissiveF.glsl`, `avboitPbrGlowF.glsl`):
`AVBOIT_TILE_MIN_SPAN = 6.0e-4` added next to each file's `avboit_tile_range()`
(or, in the capture shader, right before it, since round 8's edits moved
some code around); the span-widen check placed exactly where specified,
right after `span = max(maximum_depth - minimum_depth, 1.0 / 16777215.0)`,
symmetric around the tile's real content.

**`avboitVolumeC.glsl`'s pass-6 copy was not touched -- there is nothing
there to change.** Checked directly: pass 6's tile-ranged check (added in
round 2's Fix B, further simplified in round 3) is now just
`tile_ranged = avboitWork[range] <= avboitWork[range + 1u]` -- a raw
sentinel comparison with no span/padding computation of its own (the
earlier per-tile depth-mapping computation this file once had was deleted
back in round 2's fix, once ranged tiles stopped emitting early-depth
quads at all). Round 9's instruction to keep this file "consistent" appears
to assume a span computation still exists here; it doesn't, confirmed via
grep (`avboit_tile_range`, `minimum_depth`, `maximum_depth`, `float span`
all return zero matches in this file). Nothing to change for the required
fix.

**Optional slope-proportional margin (Round 9's "cheap insurance for
folds" section) was deliberately skipped**, not merely deferred. The
proposed snippet calls `fwidth(slice_coordinate)`, but `slice_coordinate`
in the current code is only computed inside the `avboit_tile_range()`
branch (whether a fragment's own tile is ranged is itself a per-fragment,
data-dependent condition read from the tile-range buffer) -- two
neighbouring fragments in the same 2x2 derivative quad could take
different branches (one ranged, one falling back to the global curve), at
which point `fwidth()` on a value only one of them actually computed is
not well-defined by GLSL's derivative rules. Implementing this correctly
would need restructuring so a `slice_coordinate` estimate exists in
uniform control flow for every fragment regardless of branch outcome --
real added complexity for an enhancement the round-9 spec itself labels
optional and unconfirmed. Left for a future round if the required fix
alone (minimum span) doesn't fully clear the fold case the margin was
meant to help with.

Not yet built/tested -- shader source changed; needs a cache bump, held
per standing guidance.

# Round 10 (Fable 5.1, 2026-09-03): remaining cell popping -> implement A9

Round 9 built and tested: flat fabric now clean; remaining (a) dark 8 px
squares of the under-garment popping through the dress under camera
motion, (b) slightly darker squarish patches in dense hair versus Exact
OIT.

Both are one thing: an entire cell where the pixel's front layer reads a
transmittance below its own splat, so the layer behind it takes the
normalized average. Residual sources, all cell-granular and motion
dependent: the projection clamp engaging at silhouettes and folds,
neighbour-cell extrapolation over 12 px on curved geometry, and cells that
straddle two surfaces whose derivatives are unrelated. Each further
tile-range tweak shrinks this a little and can never close it, because
the volume has no per-pixel knowledge of which fragment is in front.

Decision: stop tuning tile ranging. Keep everything from rounds 1, 2, 3,
8 and 9 as is (ranging on by default). Implement **A9** exactly as
specified in `ayanestorm-oit-performance-audit-plan.md` section A9
(full-resolution front-key pass 3: two nearest transparent depths and
alphas per pixel via 32-bit image atomics; in pass 2 the front fragment
gets weight `alpha_F` with transmittance 1, the second gets the exact
source-over weight `alpha_S * (1 - alpha_F)`, deeper layers keep the
volume weight bounded by `(1 - alpha_F)(1 - alpha_S)`). That makes the
dress/under-garment pair and the two front hair strands exact per pixel,
independent of cells, tiles, slices and projections, which is precisely
the two symptoms left. Interaction with tile mode: none; the key pass
does not read the volume, and the volume path stays as the fallback for
layers three and deeper.

Verification after A9: (1) dress over under-garment under camera motion,
no popping; (2) hair crop versus Exact OIT, no squarish patches; (3) mode
12 unchanged on flat fabric; (4) ranging off still renders as before.

## Round 10 implementation status (2026-09-03)

Implemented per the plan's A9 design in full, with one deliberate
deviation from the plan's literal pass-3 insertion snippet (a structural
bind-stack conflict, described below) and one deliberately skipped part
(debug modes 16/17, an image-unit conflict), both flagged clearly rather
than silently worked around.

**CPU side (`fsavboit.h`/`fsavboit.cpp`):**
- `Resources` gained `frontKey0`/`frontKey1` (full-resolution `GL_R32UI`
  images, allocated via the existing `allocateAccumulationTexture()`
  helper -- it is format-agnostic and integer images are always nearest-
  filtered by spec, so no new allocator needed) and `frontKeyFBO` (an FBO
  wrapping both as color attachments 0/1, used only by the
  `glClearTexImage` fallback below).
- `frontLayers()` accessor added next to `tileRange()`/`debugMode()`,
  reading a new `RenderAVBOITFrontLayers` boolean setting (default on).
- **Bind-stack conflict with the plan's literal pass-3 placement:** the
  plan's snippet calls `gAVBOITOpaqueTarget.bindTarget()` before pass 3,
  but by the time `finishDirectOccupancy()` returns (as currently
  structured, after rounds 1-9's changes), `gAVBOITOpaqueTarget` is
  already the current bound target -- `finishDirectOccupancy()`'s own
  trailing code left `gAVBOITCellDepthTarget` bound instead, unflushed,
  as pass 1's render target. Calling `bindTarget()` on an already-current
  target a second time would push a self-referential stack entry, exactly
  the corruption several existing comments in this file warn against.
  Fixed by splitting `finishDirectOccupancy()`: its last two lines
  (`gAVBOITCellDepthTarget.bindTarget(); beginDirectRasterPass(1);`) moved
  into a new `FSAVBOIT::beginPass1()` (declared in the header, stubbed for
  `LL_DARWIN` alongside every other public function), called explicitly
  by `renderPostDeferredCapture()` after the new pass-3 block. Pass 3 now
  runs while `gAVBOITOpaqueTarget` is already current, matching how pass
  0's occupancy raster works, and does not rebind it.
- `beginDirectRasterPass()` gained a `pass == 3` branch: full viewport,
  clears both front-key images to the `0xffffffffu` sentinel, binds them
  read-write at image units 0/1. The clear uses the same
  `glClearTexImage`-with-fallback pattern `fsexactoit.cpp`'s E11 already
  established for its own R32UI head/count images (`if (glClearTexImage)`
  direct clear, else bind `frontKeyFBO` and `glClearBufferuiv` per
  attachment) rather than assuming GL 4.4's function is always present on
  an AVBOIT-capable (GL 4.3 baseline) driver.
- `renderPostDeferredCapture()`: inserted the pass-3 block between
  `finishDirectOccupancy()` and the extinction raster (pass 1), per the
  plan's ordering requirement (pass 3 must run before
  `finishDirectExtinction()`'s conservative early-depth-tile raster, or a
  tile could reject a legitimate second layer). Runs `render_pass(true)`
  plus GLTF's own draw (GLTF is traversed outside the alpha spatial-group
  draw maps `render_pass()` covers, same as pass 0's occupancy raster
  handles it), then calls the new `beginPass1()`.
- `configureDirectRasterShader()`: uploads `avboitFrontLayers` alongside
  the other per-pass uniforms, guarded by `getUniformLocation()` same as
  every other uniform there.
- `handleCapturedEmissives()`'s pass-skip guard widened from
  `sDirectRasterPass != 1` to `!= 1 && != 3` -- pass 3 has no glow store at
  all (it only needs alpha, via the shared shaders' own `avboitRasterPass
  != 2` early return), so emissive draws there would be pure waste.
- `beginDirectFrame()`'s resource-completeness gate checks
  `frontKey0`/`frontKey1` alongside the existing accumulation textures.

**Shader side:**
- `avboitCaptureF.glsl`: `avboitFrontLayers` uniform, `avboitFrontKey0`/
  `avboitFrontKey1` image declarations (bindings 0/1, `coherent
  uimage2D`), `avboit_front_key()`, `avboit_store_front_key()` (the exact
  two-key atomic insertion from the plan, with its correctness argument
  preserved verbatim in comments), and the pass-3 early-return branch in
  `avboit_direct_store()`. Pass 2's `weight` computation replaced with the
  plan's `front_factor` logic exactly: front layer gets 1.0, second layer
  gets `1.0 - alpha0`, everything else gets
  `min(front_transmittance, (1-alpha0)(1-alpha1))`, and the whole thing
  falls back to plain `front_transmittance` when `avboitFrontLayers == 0`
  for the live A/B comparison. Debug mode 14 left untouched (per the
  plan: it becomes less meaningful for exact F/S pixels now, but its code
  doesn't need to change since it never reads `front_factor`).
- Both glow shaders (`avboitEmissiveF.glsl`, `avboitPbrGlowF.glsl`):
  identical `avboitFrontLayers`/image declarations and `front_factor`
  computation in their pass-2 branch, replacing `max(glow, 0.0) * front`
  with `max(glow, 0.0) * front_factor`. A front-surface glow texel shares
  its depth with the colour fragment at the same surface, so it matches
  `key0` and gets `front_factor = 1.0`, unattenuated, matching vanilla.
- Five shared/upstream shaders (`alphaF.glsl`, `fullbrightF.glsl`,
  `pbralphaF.glsl`, `materialF.glsl`, `pbrmetallicroughnessF.glsl`): each
  had exactly one `avboitRasterPass < 2` early-store gate, changed to
  `!= 2` so the same cheap early exit (alpha only, no lighting) also
  covers pass 3, per the plan. All five already used `< 2` (the plan's
  note that only `materialF.glsl` had been updated to `< 2` while others
  were still `< 1` did not match the current tree -- confirmed via grep
  before editing). Each edit wrapped in the required `<AS:Chanayane>` tags
  per this repo's ownership rules, with the original `< 2` line preserved
  as a comment inside the tag.

**Deliberately not implemented: debug modes 16/17.** The plan's own
diagnostics section already flags the resolve compute shader
(`avboitVolumeC.glsl`) is at GL's 8-image-unit limit during resolve
(units 0,1,2,5 actively rebound by `finishDirectFrame()`; 3,4,6,7
declared but left holding whatever the raster passes bound). Unit 7
happens to already be `r32ui` (`avboitExtinctionOverflowDepth`) and could
in principle share format with `frontKey1`, but unit 6 is `r8ui`
(`avboitZeroTransmittanceDepth`), format-mismatched against `frontKey0`'s
`r32ui`. Two ways to resolve this were considered: (a) a second,
differently-formatted GLSL declaration aliased onto the same binding
index 6, relying on only one declaration ever being read per branch --
legal-looking but not something the GLSL/GL spec guarantees, and
unverifiable without a build across multiple drivers; (b) skip the
diagnostics entirely for this round. Chose (b) on the user's explicit
direction. `debugMode()`'s clamp left at its pre-A9 range (0-15); modes
16/17 are simply unavailable. Verification should use mode 0 (visual) and
the existing mode 9/10/14 diagnostics instead. If modes 16/17 are wanted
later, the cleanest fix is probably freeing a unit during resolve (e.g.
folding `avboitExtinctionOverflowDepth`'s read into a buffer instead of
an image) rather than aliasing.

**Not yet built/tested.** Shader source, uniform set, and CPU-side raster
pass sequencing all changed; needs a cache bump, held per standing
guidance. Verification should follow Round 10's own four-item list above
once built.
