# Motion Blur (Screen-Space, Camera-Reprojection-Driven) — Implementation Plan

Author: chanayane@firestorm
Date: 2026-09-17
Status: implemented and confirmed working at runtime (bokt). Temporary debug
visualization modes (`RenderMotionBlurDebug` 1-3) intentionally kept in place
for now per user request — not yet cleaned up. Self/other avatar exclusion
(backlog's "Note on future motion blur design") investigated 2026-09-17 and
deferred — see "Future work" below for why.

## Context

`doc/rendering-improvements-backlog.md` item #2 identifies that AyaneStorm has
no motion blur at all. A prior feasibility pass
(`doc/ayanestorm-motion-blur-feasibility.md`, 2026-09-06) confirmed: no
velocity buffer or motion blur code exists anywhere in-tree (only inert SMAA
reprojection stubs); the only realistic first scope is **camera motion
blur** — reconstructing per-pixel screen displacement from scene depth plus
current-vs-previous view-projection matrices, not a true per-object velocity
buffer (which would require injecting previous-position output into every
draw pool's vertex shader across the whole scene — a full-object motion
vector pass). That fuller scope is out of scope here; see "Future work"
below, matching the feasibility doc's own boundary.

A survey of other viewer forks in-tree (`.Black-Dragon-Viewer-master`, for
technique ideas only — not copied) shows Black Dragon implements motion blur
via a genuine per-object velocity buffer: an extra full-scene geometry pass
(`renderGeomMotionBlur`) where every draw pool emits a previous-frame screen
position per vertex, resolved into an `RG16F` velocity target, then blurred
with a fixed 32-tap uniform box/triangle filter and **no depth-aware
rejection** — meaning it visibly smears foreground/background edges together.
This is both more invasive (touches every draw pool + adds a real geometry
render pass) and lower quality (no depth discrimination) than what we want.
Our implementation instead:
- Derives displacement purely from depth + matrices (no geometry pass, no
  draw-pool changes, no new persistent render target).
- Adds depth-aware sample rejection to avoid the smearing artifact Black
  Dragon exhibits.
- Follows AyaneStorm's own established self-contained-module precedent
  (`ASChromaticAberration`, `ASVolumetricLighting`) instead of a
  draw-pool-invasive design.

**Outcome:** a new `ASMotionBlur` module providing a default-off, tunable
camera motion blur post-process pass, wired into `LLPipeline::renderFinalize()`
with only small tagged call-outs in `pipeline.cpp`/`llviewershadermgr.cpp`,
matching the `ASExactOIT`/`ASVolumetricLighting`/`ASChromaticAberration`
precedent explicitly called for in the backlog entry.

## Technique

Reprojection-based camera motion blur (no per-object velocity buffer).

**Matrix sourcing (corrected after runtime investigation):** the codebase
tracks previous-frame view/projection matrices as first-class, always-on
infrastructure — `gGLLastModelView`/`gGLLastProjection`
(`indra/llrender/llrender.cpp:56-57`, declared `indra/llrender/llrender.h:542-543`),
exposed via `get_last_modelview()`/`get_last_projection()`
(`indra/llrender/llrender.cpp:2102-2110`). These are captured once per real
frame at the end of the 3D scene render, guarded by `!gCubeSnapshot`
(`pipeline.cpp:10214-10223`), with the existing comment stating they exist
precisely "for use in off-by-one-frame effects in the next frame."

The first implementation assumed `get_current_modelview()`/
`get_current_projection()` would still hold the true current-frame camera
matrices when `ASMotionBlur::render()` runs inside `LLPipeline::renderFinalize()`.
**This assumption was wrong and caused the effect to silently produce zero
velocity everywhere** (enabling it produced no visible change at any camera
speed). The actual call chain is: `render_ui()` (`llviewerdisplay.cpp:1626`)
calls `gPipeline.renderFinalize()` at line 1652, but immediately before that
call it does `set_current_modelview(glm::make_mat4(gGLLastModelView))`
(`llviewerdisplay.cpp:1633-1640`) — deliberately overwriting the "current"
modelview with the *previous* frame's matrix, for HUD-stability purposes.
That substitution is still in effect for the whole of `renderFinalize()`,
including our post-process pass, so `render()` cannot obtain the current
main-view matrices there.

**Fix:** `ASMotionBlur::captureFrameMatrices()` retains consecutive
current-frame `get_current_modelview()`/`get_current_projection()` snapshots
in module-local current and previous state. The call sits beside the upstream
`gGLLastModelView`/`gGLLastProjection` update (`pipeline.cpp:10214-10228`),
inside the `!gCubeSnapshot` guard and before `render_ui()`'s substitution.
Both snapshots must be module-local: the upstream `gGLLast*` arrays have
already been overwritten with the current frame when the capture call runs,
so reading `get_last_*()` later would compare the current frame to itself.

Because the upstream `gGLLastModelView`/`gGLLastProjection` capture (and our
own `captureFrameMatrices()` alongside it) only run for the main scene camera
on non-cubemap-snapshot frames, reflection probes/snapshots/HUD sub-renders
never perturb them — no extra `gCubeSnapshot` guarding is needed in
`render()` beyond what `ASChromaticAberration::render()` already does
(early-out when `gCubeSnapshot`). `render()` also checks a `sHaveFrameMatrices`
flag and bails out until two captures provide a valid adjacent-frame pair.

Per-pixel algorithm:

1. For each pixel, reconstruct its current-frame view-space/world-space
   position from the deferred depth buffer + inverse current
   view-projection matrix (standard deferred-shading depth-reconstruction,
   already done elsewhere in the deferred pipeline — reuse the existing
   depth-reconstruction convention used by other deferred fragment shaders in
   `class1/deferred/*.glsl`, do not reinvent a different one).
2. Reproject that same world position through the **previous frame's**
   module-local captured view-projection matrix
   to get its previous-frame screen-space position.
3. The screen-space delta between current and previous projected position
   (in pixels) is the per-pixel camera-motion velocity vector. This needs no
   separate velocity render target pass — it's computed inline in the
   composite fragment shader from depth + two small `mat4` uniforms (current
   inverse view-projection, previous view-projection), each uploaded once
   per frame via `uniformMatrix4fv`/`glm::value_ptr`, matching the existing
   convention at `pipeline.cpp:9610-9611`. This is a meaningful
   simplification vs. Black Dragon's design (no extra geometry pass, no
   extra G-buffer target).
4. Sample scene color N times along that vector (N from the quality tier),
   weighted (triangle/Gaussian-like weighting, center-biased), accumulating
   into the output pixel.
5. **Depth-aware rejection:** for each sample tap, reconstruct that sample's
   own depth and compare to the center pixel's depth (in view-space linear
   depth, not raw non-linear device depth). If the depth difference exceeds
   a small threshold (scaled by distance/blur radius), attenuate or drop
   that sample's contribution. This prevents a moving foreground silhouette
   from smearing background pixels across it and vice versa — the artifact
   visible in Black Dragon's non-depth-aware implementation.
6. Bound the maximum blur radius in pixels (`RenderMotionBlurMaxLength`) so
   fast camera snaps (teleports, mouselook whips) don't produce unbounded
   streaks; also skip the entire pass below a small velocity epsilon
   (cheap early-out per pixel, matches existing precedent shaders' early-out
   style).

## New module: `indra/newview/asmotionblur.h` / `.cpp`

Follow `ASChromaticAberration`'s shape (free functions in a namespace, not a
static class — it's the simplest fit since this is a single fullscreen pass
with no persistent per-draw state, unlike `ASExactOIT`'s per-pixel-linked-list
state or `ASVolumetricLighting`'s atlas). Reference:
`indra/newview/aschromaticaberration.h`, `indra/newview/aschromaticaberration.cpp`.

```cpp
namespace ASMotionBlur
{
    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();
    // Snapshots the true current-frame matrices; must be called once per frame at
    // the end of the 3D scene render, before render_ui()'s HUD-matrix substitution.
    void captureFrameMatrices();
    // Returns true only when a distinct destination received the processed image.
    bool render(LLRenderTarget& source, LLRenderTarget& destination, LLRenderTarget& depth,
                LLVertexBuffer& screen_triangle);
}
```

- Author header: short 4-line form (matches `aschromaticaberration.h`), author
  `chanayane@firestorm`.
- Internal per-frame matrix state IS needed, unlike what an earlier draft of
  this plan assumed: module-local current and previous modelview/projection
  pairs hold consecutive snapshots taken by `captureFrameMatrices()` (see
  Technique above). Also needs a shader-cache-revision
  string constant (`"as-motion-blur-v1"`) per the `shaderCacheRevision()`
  precedent in `asvolumetriclighting.cpp` / `asexactoit.cpp` — **never bump
  this during dev edits** per `AGENTS.md`.
- No new `LLRenderTarget` needed: the pass reads the current composited
  scene-color source buffer (already ping-ponged by `renderFinalize()`) plus
  the existing deferred depth buffer, and writes to the next ping-pong
  target — exactly like `ASChromaticAberration::render(source, destination, screen_triangle)`.
  This avoids any `allocateResources`/`releaseResources` pipeline hook
  entirely, which is a meaningful reduction in pipeline.cpp surface area
  compared to `ASExactOIT`/`ASVolumetricLighting`.

## Shader

New file: `indra/newview/app_settings/shaders/class1/deferred/asMotionBlurF.glsl`
(fragment only, reuse the existing shared post-process vertex shader
`deferred/postDeferredNoTCV.glsl` exactly as `ASChromaticAberration` does).

Uniforms (looked up via `LLStaticHashedString`, no shared `LLShaderMgr` enum
edit needed — same technique as `aschromaticaberration.cpp`):
- `diffuseRect` (current composited color, bound via `DEFERRED_DIFFUSE`)
- existing deferred depth texture (bound via the existing `DEFERRED_DEPTH`
  slot already used by other deferred shaders — no new enum needed)
- `inv_curr_view_proj` (mat4)
- `prev_view_proj` (mat4)
- `motion_blur_strength`, `motion_blur_max_length`, sample count (from
  quality tier — prefer a uniform loop bound with a sane max array size over
  shader permutations, so we don't need 3 compiled variants)

## Pipeline integration (`indra/newview/pipeline.cpp`)

Single insertion point in `LLPipeline::renderFinalize()`, immediately before
the FXAA/SMAA block, matching the feasibility doc's recommended hook and the
existing ping-pong pattern (around `pipeline.cpp:9167-9199`):

```cpp
// <AS:Chanayane> Apply camera motion blur before AA, matching the existing
// ping-pong post-process chain (see ASChromaticAberration below).
if (ASMotionBlur::render(*sourceBuffer, *targetBuffer, mRT->deferredScreen, *mScreenTriangleVB))
{
    std::swap(sourceBuffer, targetBuffer);
}
// </AS:Chanayane>

if (RenderFSAAType == 1)
{
    applyFXAA(sourceBuffer, targetBuffer);
    ...
```

A second, earlier call site is required: `ASMotionBlur::captureFrameMatrices()`
is called inside the existing `!gCubeSnapshot` block at the end of the 3D
scene render (`pipeline.cpp:10214-10228`, right after the `gGLLastModelView`/
`gGLLastProjection` capture loop) — see the Technique section for why this
separate, earlier capture is required rather than reading
`get_current_modelview()` directly inside `render()`.

`render()` early-outs on `gCubeSnapshot` exactly like
`ASChromaticAberration::render()` already does — no separate discontinuity
handling is needed since `gGLLastModelView`/`gGLLastProjection` (and our own
`captureFrameMatrices()` snapshot alongside them) are only ever updated for
the main scene camera on non-snapshot frames (`pipeline.cpp:10214-10223`).

Shader registration/unload goes in `llviewershadermgr.cpp`, alongside the
existing `ASChromaticAberration`/`ASVolumetricLighting` registration calls
there (same file both modules already use for this).

## Settings (`indra/newview/app_settings/settings.xml`)

Follow the dense `ASChromaticAberration*` block style with `RenderXxx`-style
names to match the closely related `RenderVolumetricLighting` convention:

- `RenderMotionBlur` (Boolean, default `0`) — master enable
- `RenderMotionBlurStrength` (F32) — blur intensity multiplier
- `RenderMotionBlurMaxLength` (F32) — max blur radius in pixels, bounds
  worst-case streak length on fast camera motion
- `RenderMotionBlurQuality` (S32, default matching a "medium" tier) —
  0/1/2 selecting sample count (e.g. 8/16/24), precedented by
  `ASVolumetricLighting`'s own quality-tier settings

All read via function-local `static LLCachedControl<T>` inside the relevant
functions, exactly like `ASVolumetricLighting::isEnabled()` — no per-draw
string-keyed `gSavedSettings` lookups.

## UI

Lives in `panel_preferences_ayanestorm.xml` under a new "Rendering 2" tab
(`tab-as-rendering2`), alongside the existing "Rendering" tab
(`tab-as-rendering`) rather than inline within it: the original Rendering
tab was already visually full (confirmed by a runtime screenshot showing its
last row clipped), so a second tab was added instead of further compressing
row spacing. Controls: enable checkbox, Strength/Max blur length sliders,
Quality combo, and a temporary Debug mode spinner (0 off, 1 velocity
heatmap, 2 raw UV delta, 3 matrix-sanity probe — remove once the effect is
confirmed working end-to-end).

## Verification (user-performed)

- `bok`/`bokt` cycle per `AGENTS.md`: implementation is build-only until the
  user tests at runtime.
- Manual runtime checks once built: camera rotation/translation blur looks
  correct and direction-consistent; moving avatars with a stationary camera
  produce **no** blur (confirms this is camera-only, not object motion, as
  intended for this scope); enabling/disabling live; teleport and camera-cut
  do not produce a garbage streak on the first frame; DoF + motion blur +
  FXAA/SMAA combinations all composite correctly; HUD attachments and the
  snapshot/reflection-probe paths are unaffected (confirms the
  `gCubeSnapshot`/main-view-only guard works); no shader-cache revision was
  bumped during this work.

## Debugging log (runtime investigation, effect not yet confirmed working)

First `bok` build: feature enabled via new UI, camera rotated/moved at
various speeds, **no visible blur at any speed**. `AyaneStorm.log` contains
no shader compile/link warnings for "AyaneStorm Motion Blur Shader" (a real
link failure would show as `LL_WARNS()` via `LL_SHADER_LOADING_WARNS()`,
`indra/llrender/llrender.h:573` — confirmed not debug-gated), so the shader
was compiling and linking successfully; the bug was not a compile failure.

**Fix 1 (still needed, kept):** removed a uniform-driven `for` loop with a
dynamic `break` (`if (i >= motion_blur_samples) break;`) in
`asMotionBlurF.glsl`'s sample loop — this pattern is unprecedented anywhere
else in this codebase (every other GLSL loop here, e.g. the hardcoded
`aoUtil.glsl:89` loop backlog item #7 flags, uses a compile-time constant
bound). Replaced with a fixed-iteration loop that masks unwanted samples to
zero weight instead of breaking early. Kept as a correctness/portability fix
even though it was not the actual root cause of the zero-effect bug (see
below) — dynamic-uniform loop bounds with `break` are still worth avoiding on
principle in this codebase given zero precedent for the pattern.

**Fix 2 (necessary but incomplete; later superseded):** documented in the
Technique section above — `get_current_modelview()`/`get_current_projection()`
are not safe to read inside `ASMotionBlur::render()` because `render_ui()`
(`llviewerdisplay.cpp:1633-1640`) substitutes `gGLLastModelView` into the
"current" modelview slot for HUD purposes, before calling `renderFinalize()`.
Fixed via `ASMotionBlur::captureFrameMatrices()`, called earlier in the frame
from `pipeline.cpp:10214-10228` (end of `LLPipeline::renderDeferredLighting()`,
inside the same `!gCubeSnapshot` block that captures `gGLLastModelView`
itself) — **not** `renderGeomPostDeferred`/end-of-3D-scene-render as an
earlier draft of this doc assumed; the actual capture point is inside
`renderDeferredLighting()`, which still runs late enough in the frame for the
matrices to be final, but the function name in this doc's earlier revisions
was wrong and has been corrected.

**Second `bok` build, after fix 2:** still zero visible blur. Added debug
visualization modes to isolate the problem (`RenderMotionBlurDebug` setting,
`motion_blur_debug` uniform):
- Mode 1 (velocity-magnitude heatmap): **solid black** everywhere, moving or
  still.
- Mode 2 (raw UV delta as red/green, scale `x50`): **solid uniform
  "mustard"** color (`~0.5, 0.5, 0, 1` — i.e. exactly the zero-delta color),
  everywhere, moving or still.
- Both results were suspicious for being *exactly* flat/uniform rather than
  noisy-but-small, which reads more like "always computing zero" than "real
  effect too subtle to see."

**Mode 3 added** (matrix-sanity probe): bypasses depth-buffer reconstruction
entirely, using a fixed mid-depth NDC point per pixel, to isolate whether the
captured matrices themselves are the problem vs. the per-pixel depth
reconstruction math. Result: **same flat color as mode 2**, except **color
artifacts appear when zooming out very far while looking at the sky**. This
is the most informative result so far:
- If the captured current/previous matrices were simply identical or stuck
  at identity, mode 3 zooming should behave the same as rotating/panning
  (all flat) — it does not; zoom specifically produces visible artifacts.
  This suggests the matrices are *not* simply frozen/duplicated, and the
  flatness during ordinary rotation/panning may instead be a **debug-display
  sensitivity problem**, not a computation problem.
- Working theory now favored: ordinary per-frame camera rotation/panning at
  normal framerates produces a genuinely small screen-space reprojection
  delta (often well under 1 pixel, especially away from the image periphery
  and for distant geometry), and the original debug color scales (`speed /
  motion_blur_max_length` for mode 1, `uv_delta * 50.0` for mode 2) were too
  low-sensitivity to visualize deltas that small — reading as "solid flat
  color" even though the underlying math may already be correct. Zooming
  changes the *projection* matrix (FOV), which can produce much larger
  reprojected deltas for distant/sky points specifically, large enough to
  clear the old low-sensitivity thresholds and show as visible artifacts.
- **Not yet confirmed** — this is the leading hypothesis, not a verified
  conclusion. Debug scales have been bumped substantially (mode 1: divide by
  a fixed `4.0` px instead of `motion_blur_max_length`; mode 2 and mode 3:
  `x2000` instead of `x50`) to test this theory on the next `bok` build. If
  ordinary camera rotation/panning still shows completely flat color at
  these much higher sensitivities, the "too subtle to see" theory is wrong
  and there is still a real computation bug to find (most likely candidate
  if so: re-examine whether `renderDeferredLighting()` truly runs with fresh,
  non-stale `gGLModelView`/`gGLProjection` at the exact point
  `captureFrameMatrices()` is called, e.g. by checking what runs between
  `pipeline.cpp:9703`, confirmed still-valid, and the capture point at
  `10214-10228`).

**Third `bok` build, after boosted debug scales:** hypothesis confirmed. With
camera moving: mode 1 (heatmap) visibly reddens on the avatar as motion
increases; mode 2 visibly "flashes" (changing color) as motion continues.
With camera still but only the avatar animating: mode 1 near-black, mode 2
flat/near-zero — correct, since this module is deliberately camera-motion-only
and has no per-object velocity buffer (see "Full object motion blur" in
Context above). This confirms the matrix capture, reprojection math, and
depth-based per-pixel reconstruction are all correct.

**However, the real effect (debug mode 0) still showed no visible blur**,
even at Strength 8 with fast camera motion near the avatar, despite debug
mode 1 clearly showing nonzero, increasing `speed` under the same conditions.
Root cause: `asMotionBlurF.glsl`'s hard early-out `if (speed < 0.5) { return
unblurred; }` — `speed` is `length(vel_pixels)` in **pixels**, and realistic
per-frame camera-rotation deltas at normal-to-high framerates are often
well under 1px even during what feels like "fast" mouselook panning (each
individual frame's incremental camera delta is small when there are many
frames per second). Debug mode 1's boosted heatmap (`speed / 4.0`) was
sensitive enough to show clearly-reddening motion below this 0.5px gate,
but the real blur path silently discarded every one of those frames as
"not moving enough." **Fixed by lowering the threshold from `0.5` to
`0.05`** pixels. At this stage this was incorrectly identified as the final
root cause. Later code inspection showed the measured sub-pixel values were
round-off from comparing two matrices belonging to the same frame.

**Cross-check against Black Dragon's `motionBlurF.glsl`** (technique
comparison only, not copied — Black Dragon uses a true per-object velocity
buffer, a fundamentally different and more invasive architecture we
deliberately did not adopt, see Context above) surfaced one more real
design issue, distinct from the epsilon-value bug: Black Dragon's "strength"
(`RenderMotionBlurStrength`, default `32`) is a *max blur length in pixels*,
applied only to clamp/derive the sampling step *after* their velocity buffer
lookup — it never touches the value compared against their own `speed < 0.5`
gate. Our shader, before this fix, multiplied `motion_blur_strength` into
`vel_pixels`/`speed` *before* the near-zero-motion gate, conflating two
different concepts: "is the camera actually moving" and "how strong should
the blur look." That meant cranking Strength was doing double duty as a
workaround for genuinely small per-frame deltas, rather than behaving like a
true intensity control. **Fixed:** `speed`/`vel_pixels` are now computed from
the raw, unscaled reprojection delta; the `speed < 0.05` gate is evaluated
against that raw value; `motion_blur_strength` is applied afterward, only to
scale the sampling reach for pixels that already passed the real-motion
gate. Debug modes 1/2 also now show the true unscaled camera velocity
regardless of the Strength slider position, which is more useful for
diagnosis than the old strength-entangled version.

**Fourth `bok` build, after both fixes (lowered epsilon + decoupled
strength): still no visible blur in debug mode 0**, at Strength 8, fast
camera motion near the avatar. Re-checked debug mode 1 (now unscaled by
Strength) under the same fast motion: only a **faint/dark red tint**, not
bright/saturated — confirming real per-frame `speed` is genuinely small
(a fraction of the 4px heatmap reference, so likely somewhere around
0.1-1px), not zero, but small enough that the `speed < 0.05` gate is not
the current bottleneck (0.05 is comfortably below the observed faint-red
range) — the gate is passing, yet the resulting blur is still visually
imperceptible.

**Superseded leading theory:** the reprojection math was thought to be
correct end-to-end (confirmed repeatedly via debug modes 1/2/3), and the
per-frame camera-motion delta this technique measures is *genuinely* just a
fraction of a pixel under normal play, even during motion that feels "fast"
to a human. This is architecturally expected for a **single-frame delta**
approach: real motion-blur cameras integrate light over a shutter/exposure
duration spanning many simulated moments, but this implementation only ever
compares "this frame's camera pose" to "last frame's camera pose" — at
60+ FPS, one frame of real camera rotation is a very small angular delta,
so the resulting screen-space reprojection is also small, and a sub-1-pixel
blur radius spread across many samples is essentially invisible regardless
of how "correct" the underlying math is. This would mean the implementation
itself is functioning as designed, but the **design's default sensitivity is
mismatched to what "motion blur" is expected to look like** — Strength as
currently defined (a post-gate sampling-reach multiplier, clamped to
`[0, 8]`) may not be a large enough range to compensate, and/or the
technique needs a frame-rate-independent amplification (e.g. scaling
velocity by an assumed shutter-angle/exposure-time factor rather than by a
flat, small-range Strength multiplier) to produce a "1 frame of camera
motion, blown up to look like a plausible exposure duration" result.

**Paused for a second opinion** on the "is single-frame delta sufficient, or
is amplification needed" question above — that review identified the actual
remaining bug (see next entry), rather than the sub-pixel-delta theory being
the final explanation.

**Root cause found and fixed:** `ASMotionBlur::captureFrameMatrices()`'s
first version snapshotted only a single current-frame matrix pair and relied
on `get_last_modelview()`/`get_last_projection()` for "previous." But the
upstream `gGLLastModelView[i] = gGLModelView[i]` copy
(`pipeline.cpp:10218-10222`) runs *immediately before* the
`captureFrameMatrices()` call in the same `!gCubeSnapshot` block — so by the
time `captureFrameMatrices()` (and later `render()`) read
`get_last_modelview()`, that global had *already* been overwritten with the
**current** frame's matrix, not the true previous one. `render()` was
therefore reprojecting the current frame against itself (via two
independently-derived but numerically near-identical matrices), producing
only tiny floating-point-noise-level "velocity" — not zero, but not real
one-frame motion either. This exactly explains every earlier symptom: debug
modes showed faint, never-strongly-saturated signal that scaled *slightly*
with camera speed (residual floating-point/timing noise, not the real
delta) but never enough for the sample loop to produce a visible blur.

**Fix:** the module now keeps its own two-slot history —
`sCurrModelview`/`sCurrProjection` (this frame) and
`sPrevModelview`/`sPrevProjection` (the actual previous frame) — entirely
independent of the upstream `gGLLastModelView`/`gGLLastProjection` globals.
Each `captureFrameMatrices()` call shifts the current snapshot into the
previous slot *before* reading the new current one via
`get_current_modelview()`/`get_current_projection()` (still valid at that
call site, ahead of `render_ui()`'s HUD-modelview substitution).
`sHaveFrameMatrices` now gates on having completed two full capture cycles,
so the first frame after enabling doesn't reproject against a meaningless
default-identity "previous." `render()` no longer calls `get_last_modelview()`/
`get_last_projection()` at all.

**Confirmed working at runtime (bokt)** after this fix: real, visible motion
blur during camera movement, camera-only as designed (no blur from
avatar-only animation while the camera is still).

**Remaining cleanup (deliberately deferred):** debug modes 1-3,
`RenderMotionBlurDebug`, and all "TEMPORARY development aid" markers in the
shader/XUI are being kept in place for now at the user's request, rather
than stripped immediately after confirmation. Remove them in a follow-up
pass when convenient.

**Second-opinion root cause (code inspection):** the apparent sub-pixel
motion was not real camera velocity. `captureFrameMatrices()` was called
*after* `pipeline.cpp` copied `gGLModelView`/`gGLProjection` into
`gGLLastModelView`/`gGLLastProjection`. The module captured the current
matrices locally, but `render()` then read the just-overwritten `get_last_*()`
matrices as "previous". Current and previous were therefore the same frame;
the faint debug heatmap was floating-point inverse/reprojection round-off.

**Fix:** the module now retains consecutive main-view captures itself. On
each `captureFrameMatrices()` call it moves its former current snapshot to
module-local previous state before capturing the new current matrices.
`render()` uses both module-local snapshots and skips the first frame, when
no valid pair exists. The pipeline call remains after the upstream copy
because the module no longer depends on the already-advanced `get_last_*()`
globals. No multi-frame color history or shutter-time amplification is
needed to make ordinary camera motion visible; adjacent-frame camera
reprojection is the standard input, provided the matrices genuinely come
from adjacent frames.

## Future work (explicitly out of scope now)

- True per-object/avatar motion vectors (would need previous-transform +
  skinning-state tracking across draw pools — substantially more invasive,
  as the feasibility doc notes).
- Per-subject exclusion toggles (self/other avatars) mentioned in the
  backlog's "Note on future motion blur design" — **investigated
  2026-09-17, deferred.** Findings:
  - The current implementation has zero per-object identity (pure
    depth+matrix reprojection), so any exclusion needs a new per-pixel
    avatar-identity signal.
  - `frag_data[1]` (ORM/specular) is unsafe to repurpose: `materialF.glsl`
    (class1+class3, rigged/skinned via `HAS_SKIN`), `pbropaqueF.glsl`,
    `pbralphaF.glsl`, and `impostorF.glsl` all write real data there for
    avatar-attachment paths. `frag_data[2].w` (`gbuffer_flag`) is also
    unsafe — fully occupied at 2 bits by `GBUFFER_FLAG_SKIP_ATMOS/
    HAS_ATMOS/HAS_PBR/HAS_HDRI` (`llshadermgr.cpp:669-672`).
  - A brand-new G-buffer attachment on `mRT->deferredScreen` is blocked by
    a hard cap: `LLRenderTarget::addColorAttachment` refuses more than 4
    color attachments (`llrendertarget.cpp:217-222`), and
    `addDeferredAttachments` already uses all 4 when
    `RenderEnableEmissiveBuffer` is on (diffuse=0, orm=1, norm=2,
    emissive=3). Every deferred fragment shader also hardcodes
    `out vec4 frag_data[4];`. A 5th slot is possible (raise the cap in
    `llrendertarget.cpp` + widen `frag_data[4]` to `[5]` in every deferred
    *F.glsl) but is a wide, mechanical, multi-file change.
  - The alternative — a standalone avatar-only render target populated by
    a duplicate geometry pass — was rejected as too invasive: `LLDrawPool
    Avatar::renderAvatars()` (`lldrawpoolavatar.cpp:674`) branches across
    rigid/rigged-skinned/PBR/impostor draw paths with real skinning state;
    faithfully replicating it for a second tag-only pass is a large,
    high-risk undertaking for a "nice to have" toggle.
  - `.aya-storm-release`'s equivalent feature
    (`RenderMotionBlurSelfAvatar`/`RenderMotionBlurOtherAvatars`) isn't
    reusable as a technique here: theirs is cheap because their motion
    blur is already a full per-object velocity-buffer architecture with
    its own avatar geometry pass to early-return from — a mechanism we
    deliberately don't have.
  - **Conclusion:** revisit only if/when a true per-object velocity buffer
    is ever added (see the point above), since that would need an
    avatar-tagging mechanism anyway and could absorb this at low
    incremental cost. Not worth the standalone complexity today.

