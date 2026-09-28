# AyaneStorm Aperture-Sampled Depth of Field — Implementation Plan

Author: chanayane@firestorm
Date: 2026-09-24
Status: implementation started after user direction to proceed; milestone 1 in progress.
Priority selected by user: highest quality first; performance target later.

## Execution record — 2026-09-24

Copied the original proposed plan from /tmp using cp before adding this record.
Added scripts/testing/dof_reference.py, an independent standard-library Python
ray/card reference. Eleven tests pass: focus-plane registration, off-axis
projection/ray agreement, blur sign and scale, pinhole equivalence, ordered
transparency, hidden-background visibility, correlated aperture occlusion,
64 layers, additive energy without opacity, focused thin-card coverage, and
disk-quadrature agreement with analytic strip coverage.

Milestone 1 is not complete: general blend factors, textured/angled geometry,
pixel-footprint integration, viewer coordinate/FOV calibration and the
once-per-frame lifecycle inventory remain. No viewer renderer changes or
project build in this step. These tests establish a reference foundation,
not evidence that the viewer's hairline problem is already fixed.

## Execution record — 2026-09-24 (continued): once-per-frame lifecycle inventory

Read-only investigation of `llviewerdisplay.cpp`/`pipeline.cpp` to satisfy
milestone 1's lifecycle-inventory gate before any camera-math code is written.
No files modified in this step.

**Simulation/animation advance happens before `display()`, not inside it.**
`LLAppViewer::idle()` (`llappviewer.cpp:1758`) runs immediately before
`display()` (`llappviewer.cpp:1795`) in the same main-loop iteration, and
drives `gObjectList.update()` → `LLVOAvatar::updateCharacter()`/`updateMotions()`
(skinning/animation blending), `gPipeline.updateMove()`, `LLWorld::updateParticles()`,
and `gAgentCamera.updateCamera()`. A DoF coordinator that repeats only the
back half of `display()` per lens sample and never re-enters `idle()` will not
re-advance animation/skinning/particles/camera-position by construction.

**Once-per-frame state inside `display()` (must run exactly once, not per sample):**
- `LLEnvironment::instance().update(&camera)` (`llviewerdisplay.cpp:877`) — sky/sun/cloud
  clock keyed to a `static LLFrameTimer` real-time delta; already skipped when
  `gCubeSnapshot` (`llenvironment.cpp:1763`), the existing precedent for "internal
  re-render, don't advance."
- DoF's own focus-rack smoothing: `static F32 current_distance/start_distance/transition_time`
  inside `LLPipeline::renderDoF()` (`pipeline.cpp:8927-8929`), keyed to `gFrameIntervalSeconds`.
- Auto-exposure history: `generateExposure(..., use_history=true)` (`pipeline.cpp:7938`)
  reads/writes `mExposureMap`/`mLastExposure`; already skipped when `gSnapshot`
  (`pipeline.cpp:9211-9216`) — reuse this exact precedent for intermediate samples.
- Motion-blur matrix history: `gGLLastModelView`/`gGLLastProjection` capture and
  `ASMotionBlur::captureFrameMatrices()` (`pipeline.cpp:10336-10343`), guarded by
  `!gCubeSnapshot`. Must fire at most once per displayed frame, not per sample,
  or intermediate lens positions become spurious "previous frames" for velocity.
- Global frame counters (`gFrameCount`, `gRecentFrameCount`, `gForegroundFrameCount`,
  `llviewerdisplay.cpp:677-682`) and `LLDrawable::incrementVisible()` (`:904`) —
  incrementing per sample desyncs every LOD/texture-budget/"seen this frame" consumer.
- `gPipeline.resetFrameStats()` (`:844`), `LLSceneMonitor::fetchQueryResult()`/`capture()`
  (`:1042`, `:1187`), `LLVOAvatar::updateImpostors()` (`:962`, its own 512x512
  viewport + matrix save/restore) — all single-shot per displayed frame.
- No TAA/motion-vector reprojection history exists in this codebase today (confirms
  backlog's prior claim); SSR has no cross-frame history, only same-frame
  `mSceneMap` capture (`pipeline.cpp:8157`) that must be captured per sample if
  SSR is to stay sample-consistent.

**Per-sample-shaped work already exists but is currently called once:** view
culling (`gPipeline.updateCull`, `pipeline.cpp:2734`), draw-order bucketing
(`gPipeline.stateSort`, `:3385`), and OIT reset/resolve
(`ASOITDispatcher::beginFrame()`/`finishFrame()`, `asoitdispatcher.cpp:201`,
called `pipeline.cpp:10274`/`10317`) are exactly the per-sample operations
section 5 of this plan calls for; `beginFrame()`'s own
`static TransparencyMode previous_mode` transition-edge detection needs
auditing so N calls per displayed frame aren't misread as N mode-change events.

**Camera/matrix state and existing save/restore precedent:** `LLViewerCamera`
is a singleton (`llviewercamera.h:39`) holding the projection/modelview caches;
per-frame FOV/near/far is set via `setZoomParameters`/`setNear`/`setFar`
(`llviewerdisplay.cpp:789-790`, `:262`), and the actual perspective push happens
in `LLViewerWindow::setup3DRender()`/`setup3DViewport()` (`llviewerwindow.cpp:6918-6932`),
which writes the global `gGLViewport` array and calls `glViewport` directly.
`display()` already contains a full save/restore pattern worth copying for
per-lens-sample overrides: around `LLVOAvatar::updateImpostors()`
(`llviewerdisplay.cpp:958-970`) it snapshots projection+modelview via
`get_current_projection()`/`get_current_modelview()`, temporarily changes
viewport, then restores both matrices and the viewport via
`gViewerWindow->setup3DViewport()`. `LLViewerCamera::sCurCameraID` also needs
an explicit decision (reuse `CAMERA_WORLD` vs. a new camera-ID slot) since
other systems branch on it.

**Post-render/pre-swap sequence (must run exactly once, after all samples):**
`render_ui()` (`llviewerdisplay.cpp:1626`) is the single per-displayed-frame
call that invokes `gPipeline.renderFinalize()` (`pipeline.cpp:9159`, the entire
tonemap/bloom/DoF/AA/vignette/present chain) and then HUD/UI rendering, followed
by `swap()` (`llviewerdisplay.cpp:1724`). The natural seam for the coordinator:
repeat culling → state-sort → opaque/transparent render → OIT resolve
(`llviewerdisplay.cpp:918-1187`, minus the once-per-frame items above) once per
lens sample while accumulating HDR output per section 6, then call the
`render_ui()`/`renderFinalize()`/`swap()` sequence exactly once on the
normalized accumulation, feeding it in place of `mRT->screen`.

**Existing integration point confirmed:** `LLPipeline::renderDoF()` is called
twice from `renderFinalize()` — once early (`pipeline.cpp:9192`, HDR/advanced,
gated `ASDepthOfFieldMode==1`, before SSR copy/exposure/tonemap) and once late
(`:9275`, post-tonemap legacy fallback, only if the advanced pass didn't already
return true). The aperture-sampled renderer's per-frame-once composite belongs
at the advanced call site, before tonemap/bloom, per section 3 of this plan.

## Execution record — 2026-09-24 (continued): off-axis camera math derivation

Added `Camera.off_axis_view_and_projection()` to `scripts/testing/dof_reference.py`,
a matrix-form (view translation + asymmetric-frustum projection) counterpart to
the existing ray-based `Camera.ray()`, derived and cross-checked independently
rather than transcribed from a textbook formula. First derivation attempt (a
projection-only frustum shift, no paired view translation) was proven wrong by
its own cross-check test before being corrected — recorded here because it is
exactly the kind of matrix error milestone 1's "derive and unit-test matrices
against independently generated lens rays" requirement exists to catch.

Three new tests, 14 total (all pass):
- `test_off_axis_projection_matrix_matches_ray_model_at_focal_plane`: a world
  point on the focal plane, reached via any lens-ray for a given pinhole image
  coordinate, projects to that same NDC coordinate through the view+projection
  pair regardless of lens offset. Off-focal-plane agreement is deliberately not
  asserted — that disagreement across lens positions is the blur itself.
- `test_off_axis_projection_reduces_to_symmetric_at_zero_lens`: at zero lens
  offset the pair reduces to an identity view and the standard symmetric
  perspective matrix.
- `test_off_axis_projection_depth_mapping_matches_symmetric`: near/far NDC-z
  mapping (-1/+1) is unaffected by lens offset, matching
  `LLViewerCamera::calcProjection`'s existing symmetric near/far handling.

Convention note for the future C++ port: this reference keeps the file's
+Z-forward convention throughout (`w == z`), not OpenGL's raw eye-space
`w == -z_eye`. The depth-row and off-diagonal signs are therefore the mirror
of the textbook GL form; `LLViewerCamera::calcProjection`'s actual GL matrix
(`indra/newview/llviewercamera.cpp:182-197`, standard `glm::perspective`,
row-major `mMatrix[col][row]`, `mMatrix[2][3] = -1`) is the real target
convention for `asdofcamera`, confirmed by reading that function directly
before this derivation. The C++ port must re-derive/re-verify against
`calcProjection`'s actual sign convention, not copy this file's matrix
verbatim — this file's contribution is the algebraic derivation and the
cross-check methodology, not a drop-in matrix.

Not yet done: the C++ `asdofcamera` module itself, calibration against
`LLViewerCamera`'s live FOV/aspect/zoom/handedness at runtime, and the
textured/angled-geometry and pixel-footprint integration items milestone 1
still lists as outstanding. No viewer files changed in this step; no build.

## Execution record — 2026-09-24 (continued): asdofcamera module

Added owned `indra/newview/asdofcamera.cpp/.h` (namespace `ASDoFCamera`,
registered in `indra/newview/CMakeLists.txt` inside the existing camera
post-effect ownership block). Not yet called by any render path.

- `makeLens()`: zoom-adjusted focal length uses the same fixed-sensor mapping
  as `LLPipeline::renderDoF` (`CameraFocalLength`/`CameraFieldOfView` define
  sensor height). Aperture radius = focal length / (2 × f-number), metres.
  Rejects focus ≤ focal length and invalid FOV/f-number via `mValid = false`;
  no silent clamping.
- `lensModelview()`: `T(-offset) * modelview`, offset in GL eye space
  (right, up).
- `lensProjection()`: shears the existing perspective projection,
  `P[2][0] -= P[0][0]·dx/focus`, `P[2][1] -= P[1][1]·dy/focus` (glm
  `[column][row]`). Applied to the live projection rather than rebuilding
  one, so it inherits the viewer's FOV/aspect/near/far and any snapshot tiling.
  Depth rows are untouched.
- `cocRadiusPixels()`: `R·|1/S − 1/d|·H / (2·tan(fovY/2))`, for diagnostics
  and sample-count planning only.

Equivalence to the physical thin lens: sensor CoC in normalized image units is
`A·|1/S − 1/d|` (diameter A = f/N), which equals the translated-pinhole
disparity with lens radius R = A/2. Focus breathing is ignored, as the viewer's
FOV mapping already does.

`scripts/testing/dof_reference.py` gained `viewer_*` mirrors of each function
in the viewer's GL convention (-Z forward, `m[column][row]`, built from
`LLViewerCamera::calcProjection`). 18 tests pass. New:
- focal-plane reprojection error < 0.01 px (the frozen camera-math gate) over
  FOV 10/60/120°, aspect 0.5/1.78/3, focus 0.3/2/200 m, heights 720/2160/8640,
  lens samples across a large f/1 aperture, and off-centre image points out
  to 95% of the frame. A sign-flipped mutant of the shear fails this test.
- depth mapping unchanged and zero offset is the identity.
- pixel CoC formula equals projected rim disparity; far points shift with the
  eye offset, near points against it.
- zoom mapping: default FOV gives the default focal length; half tan(FOV/2)
  doubles it.

Fixed: `test_blur_radius_and_sign` compared the CoC formula with itself. It now
derives the image offset from lens→point/focal-plane geometry and checks it
against `Camera.ray()`.

Still open in milestone 1 at this point: runtime calibration against live
`LLViewerCamera` state (needs the coordinator; checked at milestone 2's
zero-aperture equivalence). The sample-state contract, blend factors,
angled/textured geometry and pixel footprint were closed in later records.
`asdofaperture` followed; see the next record.

## Execution record — 2026-09-24 (continued): asdofaperture module

Added owned `indra/newview/asdofaperture.cpp/.h` (namespace `ASDoFAperture`,
registered in CMake next to `asdofcamera`). Not yet called by any render path.
- `generate(shape, count, out)`: unit-circumradius, equal-weight lens
  positions. R2 sequence, so an N-sample set is a prefix of the 2N set (for
  nested convergence sweeps) and sampling is deterministic across frames. The
  angle is inverted through the per-blade analytic area CDF
  `½(a²·tan x + 2ab·ln(sec x + tan x) + b²·x)` with
  `a = (1−roundness)·cos(π/n)`, `b = roundness`, and the radius is
  `sqrt(v)·boundary`. Blades < 3 or roundness 1 gives a circle. Rotation
  follows the shape; the anamorphic ratio scales x.
- `shapeFromSettings()`: reads the existing `ASDepthOfFieldAperture*` and
  `ASDepthOfFieldAnamorphicRatio` settings with the legacy clamps. Saved values
  are preserved.

Python mirror `viewer_aperture_samples`. 21 tests pass. New:
- samples inside the shape, deterministic, nested prefixes;
- uniform area density: sample fraction equals area fraction on an independent
  dense grid (tolerance 0.01 at 4096 samples) for half-planes and the outer
  radial band, across circle, sharp hexagon, rounded rotated pentagon, sharp
  triangle and anamorphic 9-blade shapes. A mutant using a linear-radius angle
  CDF (too dense at blade centres) fails this test;
- the circular sampler reproduces the analytic strip coverage within 0.002.

## Execution record — 2026-09-24 (continued): reference completion

`scripts/testing/dof_reference.py`, 28 tests pass (about 4 s):
- **Viewer blend equations:** `viewer_blend_node()` mirrors
  `asExactOITCompositeF.glsl` `blend_node()`: factor codes 0–9, separate
  color/alpha factors, glow-only nodes (`0xffffffff`), and
  `glow = node.glow + glow·(1 − a)`. `trace_viewer()` resolves per ray
  far-to-near with submission-index ties (`comes_first()`). Tests: the standard
  tuple matches `trace()` (viewer alpha is transmittance); a multiply/standard
  pair gives the exact order-dependent values; glow order is covered.
- **Angled/textured geometry:** `Surface` is an arbitrary parallelogram with a
  `(u, v)` RGBA texture, which also models alpha masks. Test: on a tilted,
  striped alpha-mask plane, every lens sample reproduces the pinhole value
  exactly where the plane crosses the focal plane, and the plane blurs where it
  is off focus.
- **Pixel footprint:** `integrate_pixel()` samples lens and pixel jointly
  (`viewer_pixel_jitter`, box filter). Test: a focused 0.3 px strand gives
  `alpha·0.3 ± 0.01` at every sub-pixel position at 256 samples. Centre-point
  sampling flips between 0 and alpha, which is the hairline appear/disappear
  failure.
- **`viewer_jitter_projection()`:** gives an exact pixel shift at all depths
  and commutes with the lens shear.

Sequence change: the aperture sampler moved from R2 to R4 (root of
`x⁵ = x + 1`). Dimensions 0/1 drive the lens and 2/3 the jitter, under one
nested index. The aperture density tests still pass. C++ was updated to match:
`ASDoFAperture::generate(shape, count, lens, pixel_jitter)` and
`ASDoFCamera::jitterProjection()`.

**Measured convergence finding (report before integration, per section 10).**
Integrated energy of a defocused 0.3 px strand (blur about 6 px), relative to
alpha·width:

| samples | R4 | R2 + Halton(2) | Halton(2,3) + R2 |
|---|---|---|---|
| 256 | 0.885 | 0.977 | 1.146 |
| 1024 | 0.928 | 1.038 | 1.012 |
| 4096 | 0.975 | 1.032 | 0.995 |
| 16384 | 0.991 | — | — |

No sequence pair reaches the 1% strand gate below a few thousand samples. The
integrand, a thin discontinuous coverage function, dominates the error, not the
choice of sequence. The test asserts the 1% gate at 16384 samples as an
unbiasedness check. Consequence: defocused sub-pixel hair is the
sample-count-limiting case. Live presets will not meet the 1% strand gate by
lens/pixel sampling alone, and must be reported honestly (section 6). Whether
per-sample MSAA/coverage (analytic sub-pixel coverage per lens sample) reduces
this is a milestone-4 question, validated against this reference.

## Sample-state contract (milestone 1 deliverable)

Identities: a **presented frame** (simulation/animation state, one `idle()` plus
one `display()`) contains 1..N **lens samples**. Sample `i` uses
`ASDoFAperture` index `i` (lens offset plus pixel jitter). The sequence is
fixed across frames.

Once per presented frame, before sample 0 (existing order kept):
`LLEnvironment::update`, focus distance and rack smoothing, frame counters,
`LLDrawable::incrementVisible`, `resetFrameStats`, impostor updates, scene
monitor, LOD selection. `ASDoFCamera::makeLens` is evaluated from the smoothed
focus. If the result is invalid, the frame renders pinhole (N = 1, zero offset);
it is never clamped.

Per sample (repeatable, no simulation side effects):
1. Install `lensModelview`, `lensProjection` and `jitterProjection` through the
   impostor-style save/restore precedent. `LLViewerCamera` state stays central
   for all non-render consumers.
2. Cull for this sample's frustum without central-view occlusion rejection.
   LOD stays frozen from the once-per-frame pass.
3. Render the opaque G-buffer and lighting, including sample-dependent lighting
   and SSR `mSceneMap` capture.
4. Run the selected transparency compositor's begin/capture/resolve. Mode
   transition detection in `ASOITDispatcher::beginFrame()` keys on presented
   frames, not samples.
5. Add the resolved linear-HDR color and glow to the accumulation with weight
   1/N.

Once after the last sample: normalize, then exposure (history only once),
motion-blur matrix capture (central camera), `renderFinalize`
tonemap/bloom/effects chain, UI/HUD, swap. Picking and any
central-depth consumer use a separate central (zero-offset) depth, never the
last sample's. Any sample failure (OIT overflow, shader or allocation failure)
discards the whole frame's accumulation and follows section 5.

**GL 4.1 split (user direction, 2026-09-24).** The coordinator, per-sample
camera installation, culling, opaque/lighting, accumulation (fragment shader
plus additive blending into an RGBA32F/RGBA16F target, format chosen by the
section 6 precision tests) and the Standard and AYAstorm compositors are the
common path and must be GL 4.1. Exact OIT keeps only its node capture/resolve
behind its existing GLSL 4.30 gate; AVBOIT keeps its own gate. Both plug into
step 4 through the dispatcher and hand back a resolved HDR sample. No
DoF-specific OIT representation is added. The screen-space gather's
DoF/OIT-shared captures (rigged/world depth, coverage strata) are not used by
this path.

## Execution record — 2026-09-24 (continued): milestone 2, single-sample path

New owned coordinator `indra/newview/asdofrenderer.cpp/.h` (`ASDoFRenderer`),
active for `ASDepthOfFieldMode` 2 under the same gate as `renderDoF`
(RenderDepthOfField, edit-mode rule, not cube snapshots). Normal snapshots keep
the ordinary path for now. Mode 2 is listed only in the owned
`floater_as_depth_of_field.xml` as "Aperture-sampled (experimental)"; the
Firestorm/preferences combos are unchanged until acceptance.

Upstream hooks (all tagged, logic kept in the AS module):
- `llviewerdisplay.cpp`: `ASDoFRenderer::beginSample(for_snapshot)` right after
  `display_update_camera()`. It saves the central matrices and installs the
  lens projection/modelview through the same state `setPerspective()` sets (GL
  stack, `set_current_*`, `LLViewerCamera::updateFrustumPlanes`), so culling
  uses the sample frustum. Impostor updates and sun shadows already
  save/restore the current matrices, so the sample survives them.
- `pipeline.cpp` end of `renderDeferredLighting()`: `ASDoFRenderer::endSample()`
  just before the `gGLLastModelView` / `ASMotionBlur::captureFrameMatrices()`
  capture. Motion-blur history, render_ui/HUD/UI 3D, picking and `renderDoF`'s
  focus raycast therefore see the central camera.
- `pipeline.cpp` `renderDoF()`: in mode 2 the early (advanced-only) call
  publishes the smoothed `current_distance` via `setFocusDistance()` and returns
  before any image pass. `renderFinalize()` makes that call in mode 2 and
  marks the frame handled, so the late legacy blur never runs. The lens
  therefore uses the previous frame's focus: one frame of latency, with rack
  smoothing still advanced once per frame.
- `settings.xml` (inside the existing AS block): mode comment updated, plus
  `ASDepthOfFieldApertureDebugSample` (S32, not persisted, default -1).

Runtime tests for the user build (milestone-2 gate):
1. Mode 2 with debug sample -1 must match DoF off pixel for pixel, apart from
   the absence of the legacy blur. The central matrices are reinstalled
   unchanged, so any difference is a lifecycle or state bug.
2. Debug sample 0, 1, 2… (large aperture, e.g. f/1.4, focus on an avatar):
   each index shows one unaveraged view from a different lens position. The
   focused subject must stay fixed while nearer and farther content shifts in
   opposite directions. This is the live `LLViewerCamera` calibration check.
3. Toggle modes and sample indices, resize, take a snapshot, enter
   mouselook/build mode: no stuck offset in UI, selection outlines or picking.

**Result (user, bokt, 2026-09-24): milestone-2 gate passed.**
1. Sample -1 is pixel identical to DoF off.
2. f/1.4, 200 mm, 60° FOV, focus-follows-pointer on an avatar face at about
   1 m, samples 0/1/2: the face stays registered while the window, bookshelf
   and crates behind it shift by tens of pixels between samples, the expected
   orbit-around-focus parallax. Magnitude check: R = 200/(2·1.4) ≈ 71 mm, and
   a background at about 4 m gives ≈ 0.053 image units ≈ 50 px at 1100 px
   height, consistent with the screenshots.
3. Selection and picking work after resize.

Note: f/0.01 (the slider minimum) gives R = 10 m and views from inside or
behind geometry. That is correct for that lens, not a defect. No aperture
limit was added (section 3: no silent clamping).

Known single-sample limitations (expected, resolved by milestone 3 or later):
- Occlusion-query results carry across frames; the sample index is constant
  per frame, so this is consistent except for one frame after changing it.
- Post effects that reconstruct position from depth (SSR's frame-lagged scene
  copy, motion blur) use the central matrices on depth rendered from the lens
  sample. Irrelevant at sample -1; minor for non-zero samples.
- World-space camera-origin uniforms (sky, water, atmospherics) remain
  central; the lens offset is millimetres to centimetres.
- If `renderDeferredLighting()` returns early (no cull result, startup only),
  `endSample()` is skipped for that frame; the next `beginSample()` resets.

## Execution record — 2026-09-24 (continued): milestone 3, multi-sample accumulation (first cut)

`ASDoFRenderer` now renders `ASDepthOfFieldApertureSamples` (new persisted
S32, default 8, clamped to 1..4096) lens samples per presented frame and
averages them before `renderFinalize()`.
- Sample 0 is the ordinary `display()` pass. Samples 1..N-1 run from one new
  tagged `display()` hook (`renderRemainingSamples(result)`, right after
  `renderDeferredLighting()`). Each sample repeats `updateCull`, `stateSort`,
  G-buffer clear/`renderGeomDeferred(camera, false)` and
  `renderDeferredLighting()`, which includes `renderGeomPostDeferred`, the
  selected OIT compositor (`ASOITDispatcher::beginFrame`/`finishFrame` per
  sample) and weather.
- `endSample()` (end of `renderDeferredLighting`) adds `mRT->screen` (linear
  HDR, glow in alpha) × 1/N into an RGBA32F accumulator through the owned
  shader `asDoFAccumulateF.glsl` (`copyV` vertex, additive ONE/ONE), then
  restores the central camera. After the last sample the normalized sum
  overwrites `mRT->screen`, so exposure, tonemap, glow extraction, bloom and
  the rest of `renderFinalize` run once on the average.
- Once-per-frame guard: the `gGLLastModelView` / `ASMotionBlur` capture
  condition becomes `!gCubeSnapshot && !ASDoFRenderer::isRepeatSample()`
  (tagged, original kept). `display()`'s sim/env/texture/impostor/shadow work
  stays single; the new loop does not re-enter it.
- Occlusion culling is disabled for multi-sample frames (`sUseOcclusion = 0`,
  restored after the last sample), so central-view queries never reject
  surfaces other lens positions see. Section 5 requires this.
- LOD and alpha-sort distances use `LLViewerCamera`'s origin, which is never
  moved (only matrices and frustum planes change), so LOD is frozen across
  the aperture.
- Shaders register through `ASDepthOfField`'s existing hooks (owned file); no
  new upstream shader-manager edit. `ASDepthOfFieldApertureSamples = 1` or an
  invalid lens renders one central sample. The debug sample still renders one
  unaveraged sample. Leaving mode 2 releases the accumulator.
- Failure rule: if the accumulator cannot be allocated, the frame keeps the
  last full sample and logs once. It never presents a partial average.

Open items for the milestone-3 gate:
- **Depth consumers:** `deferredScreen` depth after the loop is the last
  sample's, not central. SSR's frame-lagged copy and motion blur therefore
  reconstruct from a lens-offset depth. A central-depth pass or sample
  ordering is needed; measure the visual impact first.
- **Exact OIT/AVBOIT:** `beginFrame()` now runs once per sample. The Exact
  predictive-skip counter (`skipFramesRemaining`) decrements per sample, not
  per frame. Per-sample vanilla fallback on overflow is not yet detected, so a
  frame could average fallback and Exact samples (violates section 5). The
  fix is to query the fallback state after each sample and apply the
  whole-frame rule.
- **Sun shadow cascades** are fitted once to sample 0's frustum. Lens offsets
  are centimetres, so coverage at the frustum edges needs checking.
- **`RenderDepthPrePass`** (default off) is not repeated for samples ≥ 1.
- **Cost:** about N × (cull + sort + G-buffer + lighting + transparency) per
  frame. Measure per-sample GPU/CPU time before choosing presets (section 10).

### Milestone 3 first runtime result (user, bokt, 2026-09-24)

f/1.4, 200 mm, 60° FOV, avatar focus at about 1 m, background a few metres
away. DoF off 44 FPS; mode 2 with 8 / 16 / 32 / 64 samples gives
6.8 / 6.7 / 6.5 / 3.5 FPS.

Quality: the focused face is sharp and correctly registered. Out-of-focus
regions show discrete ghost copies (8 samples) or streaks (64 samples), not a
smooth blur. This is the expected finite-sample behaviour, not a registration
bug. Copy spacing is about (CoC diameter)/√N. With a CoC radius of about
60–100 px, a smooth result needs spacing near 1 px, i.e. on the order of 10⁴
samples. Pure aperture sampling therefore cannot deliver smooth large blur
live (section 10 "report measured limits").

Performance follow-up: 1 / 2 / 4 samples give 44 / 24 / 13 FPS; 1 sample is
pixel identical to DoF off. Frame time is 22.7 ms + about 18 ms per extra
sample, an exact fit through 8 samples. Each lens sample costs about 80% of a
full frame, with no fixed overhead. The 16/32/64 readings exceed the linear
prediction (3.4/1.7/0.9 FPS) and are attributed to FPS-counter averaging or
the setting not yet being applied.

Conclusion: the live budget is 2–4 samples, orders of magnitude short of the
roughly 10⁴ needed for smooth large blur. Milestone-4 reuse (union culling,
shared lighting inputs) plausibly saves 20–40% per sample, not orders of
magnitude. Revised recommendation to the user: mode 2 becomes a
static/converged photographic mode (progressive accumulation while camera and
scene are unchanged, reset on change; fixed high counts for snapshots), and
live DoF stays screen-space.

Decision pending (proposed to user): (1) hybrid, where N lens samples each get
a residual screen-space gather of radius ≈ CoC/√N; (2) progressive
accumulation while the camera and scene are static (converged capture,
section 6); (3) both. Recommended: (3), starting with (1).

## Execution record — 2026-09-24 (continued): progressive photographic mode

User decision: (1) mode 2 becomes progressive/converged (photographic);
(2) live DoF stays screen-space (mode 1, no code change). The user is not yet
satisfied with the visual results.

`ASDoFRenderer` frame plans:
- **PINHOLE** (first frame after any change, or invalid lens): one central
  pass at ordinary cost; the running average restarts.
- **ACCUMULATE** (still): `ASDepthOfFieldApertureSamples` (now per frame,
  default 4) new lens samples, sequence indices continuing from the running
  count, added with unit weight to the RGBA32F sum. The screen shows
  sum/count. Occlusion is off for these frames.
- **CONVERGED** (count ≥ `ASDepthOfFieldApertureMaxSamples`, new, default
  512): one central pass with occlusion restored, displaying the stored
  average, so frame time returns to about the DoF-off cost.
- **DEBUG** (debug sample ≥ 0): unchanged single unaveraged sample.

Restart key: central projection/modelview (relative 1e-6), view angle,
f-number, focal length, default FOV, aperture shape, viewport size. Focus is
frozen for the average and restarts only on a >0.5% change, so rack smoothing
drift does not keep resetting it. Lens samples for the whole nested prefix are
generated once per restart.

Known limits:
- Scene animation is not detected. Moving avatars and particles smear into
  the average like a long exposure, and a converged frame shows the frozen
  average while the scene keeps animating underneath. The plan's "never
  average consecutive animation states" rule is therefore not met in this
  mode; frozen poses are the intended use.
- Snapshots, corrected the same day (the first cut captured the sharp
  restart frame). Deferred `display()` clears `for_snapshot` before the hook
  (sky hack), so snapshots already reach mode 2 and are detected with
  `gSnapshot`. A snapshot always converges inside its capture: it renders all
  remaining samples up to `ASDepthOfFieldApertureMaxSamples` in that
  `display()` call, reusing a finished live average when key and size match
  (instant), otherwise restarting. High-res tiles are separate keys, so each
  tile converges fully; lens shear and pixel jitter compose correctly with the
  tile's zoom/offset projection. The main-loop watchdog is pinged per sample.
  Capture time ≈ samples × per-sample cost at snapshot resolution (512 ×
  18 ms ≈ 9 s at window size), and the snapshot floater's preview refreshes
  pay it too.
- Earlier open items (last-sample depth, Exact OIT per-sample fallback, shadow
  cascades, depth pre-pass) still apply to ACCUMULATE frames.

### Progressive mode: user result and UI (2026-09-24)

User (bokt): a converged result at 4096 samples (f/2.35 149 mm; f/4.88
62 mm) "looks rather nice". Aperture shape controls (blades, roundness,
rotation, anamorphic) are read live by mode 2, as confirmed by the user. The
quality, radius, highlight and diagnostic controls apply to the Advanced
renderer only.

Added:
- `ASDoFRenderer::drawProgress()`: an optional (`ASDepthOfFieldApertureShowProgress`,
  default on) yellow "DoF n / max" counter with a progress bar, top right of
  the 3D view, drawn like the viewer debug text. Never drawn when `gSnapshot`
  is set, in DEBUG mode, or outside mode 2. One tagged `render_ui()` hook after
  `drawDebugText()`.
- Owned `floater_as_depth_of_field.xml`: an "Aperture-sampled renderer"
  section with samples per frame, total samples and a show-counter checkbox,
  including reset buttons (added to `ASDepthOfField.ResetDefault`). The header
  tooltip names the controls that apply to Advanced only.

### Residual per-sample softening ("dot smoothing", 2026-09-24)

User observation: at 4096 samples, out-of-focus point lights show triangle
bokeh made of distinct dots (one per lens sample). User asked for a very mild
fill that keeps the shape.

Implementation: `asDoFAccumulateF.glsl` softens each sample before
accumulation with a 12-tap equal-area Vogel disk (plus centre). The tap
pattern is rotated by the golden angle per sequence index. The radius comes
from the sample's own depth:
`strength · R · (P[1][1]·H/2) · sqrt(π/N) · |1/S − 1/d|` px. That is exactly
the mean spacing of N equal-area sample images of a point with CoC radius
`R·|1/S−1/d|`: about 1.1 px for a 40 px bokeh at N = 4096. It is zero at the
focal plane, capped at 6 px (low N is not rescued by rounding the shape), and
uses P[1][1] so zoomed snapshot tiles scale correctly. The depth-to-distance
formula `P32/(ndc + P22)` was verified numerically against
`viewer_projection`.

Setting `ASDepthOfFieldApertureResidualBlur` (F32, default 1 = spacing,
0 = off) is part of the restart key and exposed as "Dot smoothing" in the
owned floater. Limits: gathers use the centre pixel's opaque depth, so
transparent hair over a far background takes the background's radius (at most
a few px); a halo of about one spacing may appear at focused edges.

Simulation (scripts/testing, same sequence and taps; point light, triangle
bokeh with R = 40 px, N = 4096, measured over the inner 80% of the shape):
- strength 0: 56% pixel-to-pixel noise, 8% empty pixels;
- strength 1: 16% noise, 0% empty;
- strength 2: 13% noise, 0% empty.
The effect is visible only on small bright defocused highlights. Suggested
runtime check: 256 total samples (≈4 px spacing), smoothing 0 vs 2.

Counter placement fix: the world-view rect runs under the menu, navigation
and favourites bars, so the counter is now drawn top centre of
`gFloaterView->getSnapRect()` (converted to screen), clear of toolbars and the
top-right chiclets.

### Snapshot capture UX (2026-09-24)

User report: a 4096-sample snapshot blocks the viewer for about 80 s and
looks frozen. User chose all four measures:
- **Reuse live:** the snapshot target is compared with the live running
  average. For the same key and size, an average already at or above the
  target is used instantly; a shorter one is continued, not restarted.
- **Separate snapshot target:** `ASDepthOfFieldApertureSnapshotSamples` (S32,
  default 512, "Snapshot samples" in the owned floater, mode 2 only). Live
  keeps `ASDepthOfFieldApertureMaxSamples`. The residual smoothing uses the
  target in force as N.
- **Progress screen:** during a snapshot's sample loop, every 0.5 s the
  default framebuffer is cleared and shows "Capturing depth of field… n /
  total" with a bar, then swapped; the FBO binding and viewport are restored
  and the central camera is reinstalled after the loop. The UI scale is not
  applied (raw pixels).
- **Esc to stop (Windows only):** `GetAsyncKeyState(VK_ESCAPE)` while the
  viewer window is in front, because the main loop and its input are
  suspended during the capture. It keeps the samples done so far; a later
  capture of the same view continues them. Not available on macOS/Linux.
  The Esc key press may still reach the viewer afterwards.

**Disconnect finding (user, 2026-09-24):** an ~80 s blocking 4096-sample
snapshot logged the user out of Second Life. During a capture the main loop,
including simulator networking, is suspended; the progress screen does not
change that. Fix: `ASDepthOfFieldApertureSnapshotMaxSeconds` (F32, default
15, clamped 1–30, "Snapshot time limit" slider) bounds one snapshot's sample
loop. The budget is split across high-res tiles (`ceil(zoom)²` display calls
each get total / tiles), so a whole snapshot stays within it. On reaching it,
the capture stops like Esc and keeps its samples; repeating the same capture
continues them. Pumping networking inside the render loop was rejected: object
updates would mutate the scene mid-frame.

### Still-image labelling and animation freeze (2026-09-24)

- Mode 2 is labelled "Aperture-sampled (still images only)" in all four
  renderer combos. The owned floater section header reads "Aperture-sampled
  renderer: for perfectly still images", with a note that the blur builds up
  while nothing moves and that motion smears or restarts it.
- "Freeze all animations" checkbox in the owned floater: non-persisted
  `ASDepthOfFieldFreezeAnimations`, whose control listener (registered via
  `ASDepthOfField::registerUICallbacks`) calls the existing
  `set_all_animation_time_factors(0 / 1)`, the same global freeze as
  Advanced > Animation > Freeze Animations (Ctrl+Alt+N) and the My Lights
  floater. No new upstream edit. Limit: freezing from the menu does not tick
  the checkbox, and unticking restores normal speed (1.0).
  Superseded 2026-09-27 by the toolbar "Toggle animations" button (see
  "DoF floater toolbar").

### DoF floater toolbar (2026-09-27)

Row at the top of `floater_as_depth_of_field.xml`: the "Enable Depth of
Field" checkbox, then buttons, all bound to
`ASDepthOfField.Toolbar` (registered in `ASDoFRenderer::registerUICallbacks`).
Both toggles only invert the current state, which can also change elsewhere.

- "Toggle animations" (`animations`): freezes when
  `LLMotionController::getCurrentTimeFactor() != 0`, else resumes at 1.0,
  via `set_all_animation_time_factors`; sets `ASDepthOfFieldFreezeAnimations`
  to match. `isSceneFrozen()` holds sky/snow only while that setting is on
  and the time factor is 0, so Ctrl+J also releases the sky.
- No focus lock button: with `FSFocusPointFollowsPointer`, the focus
  follows the pointer to the button before the click, so locking (or
  unlocking) from the floater always focuses behind the button. Alt+Shift+X
  stays the way to lock.
- "Refresh" (`refresh`, enabled in mode 2 only): clears `sLive.mHaveKey`, so
  the next live frame restarts the average.

### Mode-dependent control enabling (2026-09-24)

The owned floater greys out controls that have no effect in the selected
renderer, using XUI `enabled_control` bound to non-persisted flags that
`ASDoFRenderer::syncModeFlags()` derives from `ASDepthOfFieldMode`. The flags
are updated by a control listener and re-checked each frame in `beginSample`
(written only on change).
- `ASDepthOfFieldUIAdvanced` (mode 1): backend, bokeh quality,
  foreground/background radius, bokeh highlights, diagnostic view, and their
  labels and reset buttons.
- `ASDepthOfFieldUIShape` (modes 1 and 2): aperture shape, blade rounding,
  rotation, anamorphic ratio.
- `ASDepthOfFieldUIAperture` (mode 2): samples per frame, total samples, dot
  smoothing, sample counter.
- Always enabled: DoF enable, renderer, freeze animations, reset tuning.

### Time-sliced snapshot capture (2026-09-24)

Goal: a long DoF snapshot must neither freeze the viewer nor stop
networking. Threads were rejected: the GL context, scene data and networking
all belong to the main thread. Pausing inside `rawSnapshot()` was also
rejected: running the main loop there would mutate the scene mid-frame.

Mechanism: the snapshot floater's `LLSnapshotLivePreview::onIdle` (idle
callback) calls `ASDoFRenderer::requestCaptureSlice()` before
`rawSnapshot()`; both are tagged hooks. The snapshot `display()` that
follows is a *slice*:
1. It renders samples for about 0.3 s (`CAPTURE_SLICE_SECONDS`, measured
   from `beginSample`, sample 0 included).
2. It shows the partial average, and `rawSnapshot` reads it back.
3. If `isCapturePending()` is then true, the hook leaves the snapshot out of
   date and returns.
4. The main loop runs (networking, UI, one live frame), and the next idle
   call renders the next slice.

With deferred rendering, snapshots larger than the window render
single-tile in reallocated full-size buffers, so the common case is
sliceable. Tiled (zoom > 1) snapshots, and other callers such as File > Take
Snapshot to Disk and reports, keep the blocking path with its 30 s cap.

State: two running averages.
- `sLive`: the window-size live view.
- `sCapture`: snapshot size. It is released when a capture finishes or is
  abandoned: live frames free it when no slice arrives for 2 s, for example
  after the floater closes.
- A new capture whose key equals the live key is seeded by copying the live
  sum, so window-size snapshots of a developed view stay instant. Live
  frames between slices render no new samples: they show the live average
  if there is one, else the pinhole view.

Stop conditions (keep the samples rendered so far):
- **Esc:** `GetAsyncKeyState & 0x8001`, which also catches a press between
  slices. A stale press is discarded when a capture starts.
- **Time limit:** `ASDepthOfFieldApertureSnapshotMaxSeconds`, now 1–600 s
  with a default of 120, counting slice render time only.
- **View change between slices:** the camera moved, or Esc reset the view.
  The slice then shows the old average (`CONVERGED` plan) instead of
  restarting on the new view.

The counter reads "Snapshot DoF n / m" during a capture, even when the
optional counter is off.

Counter placement, revised after user feedback: top centre of the snap rect
still overlapped the favorites bar, which the snap rect does not cover. The
counter is now at the top right of the snap rect. Its top is placed below
the lowest visible bottom edge among the views named `navigation_bar`,
`favorite` (favorites bar) and `chiclet_bar` (notifications). The views are
found by name, with the search retried at most every 60 frames until found,
and are held as handles.

Cost per slice: the snapshot buffer reallocation, the scratch target and the
readback in `rawSnapshot`. At 0.3 s slices this is expected to be a small
fraction, but it has not been measured.

Non-DoF snapshots: nothing to slice. They are one `display()` pass, a
readback and encoding. What blocks at large sizes is likely the single
large render, buffer allocation and image encoding or scaling, which is not
measured yet. Encoding on a worker thread would be the candidate; it is
recorded as an idea, not implemented.

First runtime results (user, 2026-09-24):
- Slicing works: the log shows the 4000×4000 and window buffers
  alternating about three times a second.
- A 4000×4000 capture ran at about 5 samples/s (92 in 19 s). This is
  intrinsic: about 7.7 times the pixels of the window.
- **Thumbnail stall:** after a capture, the floater's thumbnail went through
  `thumbnailSnapshot()` → `rawSnapshot()`. That is a blocking capture at
  window size that re-rendered the whole snapshot target, which showed the
  full-screen progress screen for many seconds. Fix: a tagged
  `ASDoFRenderer::requestPreviewCapture()` hook before that call. The
  thumbnail continues the live average with at most 16 new samples
  (`PREVIEW_SAMPLES`), shows no progress screen and leaves a pending
  capture alone. Scaling the preview image instead was rejected, because
  the thumbnail has the window's aspect and a crop frame.
- **Stuck at 2 / N:** every second slice saw a slightly different key,
  from camera drift and/or the focus published by the live frame in
  between. The capture then stopped as a "view change" (not logged at the
  time), the floater's auto-refresh started a new one, and the cycle
  repeated. Fix: a continuing capture ignores focus changes and drift
  within 1e-4 (projection) and 1e-3 (modelview), about 1 mm / 0.06°. It
  reinstalls the requested view's frozen matrices, and only a larger move
  stops it. Capture start and view-change stops are now logged.
- **Blocking progress screen misaligned:** the text used the font's UI
  scale and the bar an inherited UI offset. The screen now loads UI
  identity, zeroes the font origin and divides the text position by
  `LLFontGL::sScaleX/Y`.
- Live view during a capture: adds no samples (confirmed by design).
- **"UI blocked, counter frozen at 14 / 2048":** the log showed the main
  loop running at 3–5 FPS with slices progressing (353 samples in 83 s at
  4000×4000), but nothing was presented. `rawSnapshot()` ends with
  `gDisplaySwapBuffers = false`, which suppresses the next frame's swap by
  upstream design. With a slice on every main-loop pass, no live frame was
  ever shown. Fix: `ASDoFRenderer::resumeLiveView()` sets it back to true
  in the pending branch of the snapshot preview hook. The live frame
  renders a complete back buffer before its swap.
- **Snapshot bokeh wrong, screenshot fine:** in a converged 2048-sample
  4000×4000 capture (about 3 minutes), star bokeh appeared as several
  copies side by side ("crowns"), and the moon was smeared sideways.
  - Cause: the day cycle (sun, moon, star rotation) keeps advancing
    between slices. The sky blender updates in steps
    (`DEFAULT_UPDATE_THRESHOLD`), so each step leaves one copy.
  - The live view averages over a much shorter time and shows it only as
    slightly widened shapes. The old blocking capture ran no main loop, so
    it never saw this.
  - Fix: `ASDoFRenderer::isSceneFrozen()`, true while a capture is pending
    or while animations are frozen by the DoF floater's "Toggle animations"
    (`ASDepthOfFieldFreezeAnimations` and time factor 0). When it is
    true, `LLEnvironment::update` skips `applyTimeDelta` and
    `updateCloudScroll`, in a tagged block; the camera-yaw cache still
    updates.
  - Snow simulation pauses the same way. My first diagnosis blamed snow,
    which was wrong: there was no snow in the scene. The snow pause is
    kept for the same reason.
  - After unfreezing, the sky catches up to the current time in one step.
  - Other moving content between slices still smears, as it does in live
    accumulation: avatar animation, particles, water, wind-driven trees.
    Freezing is recommended.
- **Star "crowns" persisted after the environment freeze (user, CA off:
  still fan/lotus shapes).** A simulation of star bokeh with the viewer's
  sample mirror (6 blades, 2048 samples, 90 px radius, 1 px dots) gave
  clean hexagons for R4, R2 and Halton alike, with interior variation
  of 3.6–6%. So the sample set is not the cause, and the sequence stays
  R4. The real causes are in `lldrawpoolwlsky.cpp`:
  - **Twinkle:** `starsV.glsl` sets each star's brightness from
    `mod(time, 1.25)`, with time from `LLFrameTimer`. That value is
    constant within a frame, so all samples of a snapshot slice share one
    twinkle. Consecutive Kronecker samples trace arcs and rays across the
    aperture, so each slice painted one arc at one brightness. Live frames
    (2 samples each) average it out.
  - **Rotation:** the star dome rotates by `gFrameTimeSeconds × 0.01°`,
    about 1.8° over a 3-minute capture, and it also widens live hexagons
    slightly.
  - Fix, in tagged edits: `ASDoFRenderer::starRotationTime()` returns the
    frame time recorded when the running average (re)started, carried
    over by live seeding. `starTwinkleTime()` returned
    `fract(index × 0.618034) × 1.25` per lens sample while accumulating.
    Both passed the time through when not accumulating. The twinkle part
    was superseded by the mean twinkle; see "Star bokeh grain" below.
- **Two captures per floater open (user saw a quick progress bar, then a
  slow one):** in the log the floater opened at 12:02:42 and a 3440×1328
  capture started, then 20 s later a 4000×4000 one.
  - Cause: `LLFloaterSnapshot::onOpen` calls `updateSnapshot(true)` and
    `updateControls` before selecting the last-used panel, so the preview
    keeps its window size. The saved resolution only applies when the
    first snapshot's "snapshot-updated" runs `updateControls` again.
  - Harmless without DoF (one render); with DoF, a whole wasted capture.
  - Fix: a tagged extra `impl->updateControls(this)` right after the panel
    `onOpen`, so the size is set before the first idle capture.
- A size or DoF-setting change during a pending capture now restarts it,
  with a fresh time limit. Previously it counted as a view change and
  finished with the old-size average. Only camera movement ends a capture
  early (`sameSettings()` check).
- **Progress preview (user request):** the floater's preview area stayed
  gray during a capture.
  - Each slice already reads the partial full-resolution average into
    `mPreviewImage`.
  - In the pending branch of the tagged hook,
    `ASDoFRenderer::isCapturePreviewDue()` fires first at 8 samples, then
    every 32. When it does, the hook sets `mThumbnailSubsampled`
    temporarily and calls `generateThumbnailImage(true)`, which is a CPU
    scale of the partial image to the thumbnail at the snapshot's own
    aspect, with no render. It then restores the flag.
  - The final thumbnail keeps the normal forced path when the capture
    completes.
  - No "snapshot-updated" notify mid-capture, so the save buttons stay
    disabled.
  - **Follow-up (user: correct ratio for an instant, then stretched):**
    `updateLayout()` runs `setThumbnailImageSize()` on every draw from
    `mThumbnailSubsampled`, so restoring the flag right after generation
    re-sized the square thumbnail to window proportions.
    - The flag now stays set for the whole capture.
    - `ASDoFRenderer::setProgressThumbnail()` records that the hook set it,
      so previews that are subsampled by design (the share floaters) are
      untouched.
    - It is cleared before the final thumbnail, which gets the normal
      window framing.
- Counter visibility (user request): the counter is shown only while
  samples are being added, meaning a live frame with the ACCUMULATE plan
  or a pending snapshot capture. It disappears once converged, while
  moving (PINHOLE) and in the debug view.

Dot smoothing (user, 2026-09-24): 2 looked ideal. The default is now 2.0, the
slider goes to 10, and `RESIDUAL_MAX_PIXELS` rose from 6 to 24 px so higher
values are not silently clamped.

### Axial chromatic aberration (mode 2, optional, 2026-09-24)

User request: optional and off by default. Lateral CA stays the existing
post effect after DoF, which is the physically right order, because the
per-channel magnification applies to the whole image, bokeh included. Axial
(longitudinal) CA is a per-wavelength focus distance, so it belongs inside
the lens integration.

Model: each lens sample also stands for a wavelength.
- **Spectral coordinate:** `ASDoFAperture::spectralCoordinate(i)` is a
  base-2 radical inverse rotated by 1/2, mapped to s ∈ [−1, 1): blue −1,
  green 0, red +1. It is nested and deterministic, decorrelated from the R4
  lens and jitter dimensions, and sample 0 is green.
- **Colour weights:** `spectralWeights(s)` = (1 + s, 1.5(1 − s²), 1 − s).
  Each averages to exactly 1 over uniform s, so in-focus content stays
  neutral and brightness is preserved.
- **Focus:** per sample, 1/S′ = 1/S − s·α/(2f). α is the red-to-blue focal
  shift over the focal length, from the thin lens with a fixed sensor: red
  focuses farther, blue nearer. The inverse focus drives the projection
  shear; a negative value (beyond infinity) is valid, and |1/S′| is kept at
  least 1e−6. The same value drives the residual smoothing's `inv_focus`.

Accumulation:
- `sample_weight` is now a `vec4`: RGB are the colour weights, and alpha
  (glow) stays 1 per sample.
- Normalization and seeding use a uniform `vec4`.
- There is no extra render pass. Colour noise converges with the sample
  count.

Settings and UI:
- `ASDepthOfFieldApertureAxialCA` (bool, off) and
  `ASDepthOfFieldApertureAxialCAStrength` (percent of f, 0–2, default 0.2,
  typical of real fast lenses).
- Both are part of the accumulation key and `sameSettings()`, so a change
  restarts the average.
- Floater: a checkbox (mode 2) and an "Aberration (% of f)" slider, enabled
  only with mode 2 and the checkbox on, via the non-persisted
  `ASDepthOfFieldUIAxialCA` flag synced by `syncModeFlags()`. That function
  now listens to the checkbox too.
- Both settings are in Reset tuning defaults. The floater height is now 736.

Magnitude with defaults: at f = 50 mm and focus 2 m, the red-blue spread
is 0.04 in inverse focus. Against a background blur scale of 0.5, the rims
are about 8% of the bokeh radius.

Reference (`dof_reference.py`): the mirrors are
`viewer_spectral_coordinate`, `viewer_spectral_weights` and
`viewer_axial_ca_inv_focus`. Two tests were added:
- `test_axial_ca_weights_neutral_and_nested`: channel means within the
  Koksma–Hlawka bound (log₂N + 3)/N for N = 16…2048, including N = 1000.
- `test_axial_ca_focus_shift_orders_channels`.

All 30 tests pass.

### Cat's-eye bokeh and spherical aberration (mode 2, optional, 2026-09-24)

User request: both optional and off by default. They are per-pixel
per-sample weights in `asDoFAccumulateF.glsl` (new `lens_mode`), with no
extra render.

**Cat's eye (mechanical vignetting):**
- Weighting: a sample reaches a pixel only when its unit-aperture
  position lies within a unit circle (the projected barrel) centred at
  `cat_eye × field`. Field is the aspect-correct image position, length 1
  at the frame corner.
- Shape: the overlap of the two circles is the lemon, with its long axis
  tangential.
- Normalization (`lens_mode` 2): the average is divided by the analytic
  open fraction (vesica area / π), clamped to at least 0.05, which keeps
  brightness. The option `ASDepthOfFieldApertureCatEyeDarken` keeps the
  physical corner light loss instead.
- The analytic fraction assumes a circular aperture; polygon apertures
  differ slightly.
- Limitation: tiled (zoom > 1) snapshots compute the field position per
  tile, so it is not yet mapped to the full image.
- Settings: `ASDepthOfFieldApertureCatEye` (off),
  `ASDepthOfFieldApertureCatEyeStrength` (0–2 aperture radii at the
  corner, default 0.6), and `ASDepthOfFieldApertureCatEyeDarken` (off).

**Spherical aberration:**
- Weight: 1 − a·σ·(2ρ² − 1), where:
  - ρ² is `ASDoFAperture::pupilRadius2(i)`, the uniform R4 dimension, so
    the weight averages to 1;
  - σ = clamp(signed full-aperture CoC in pixels / 3, −1, 1), computed
    from this sample's depth and inverse focus (axial CA included), so it
    is 0 at the focal plane.
- a > 0 (typical under-corrected lens): centre-bright, soft background
  bokeh and a bright-rimmed foreground.
- a < 0 (over-corrected): soap-bubble background.
- Settings: `ASDepthOfFieldApertureSpherical` (off) and
  `ASDepthOfFieldApertureSphericalStrength` (−1…1, default 0.5).

Common to both:
- All three values are in the accumulation key and `sameSettings()`.
- The UI flags `ASDepthOfFieldUICatEye` and `ASDepthOfFieldUISpherical`
  are synced by `syncModeFlags()`, which listens to both checkboxes.
- All settings are in Reset tuning defaults.
- The floater height is now 866.

Reference: the mirrors are `viewer_pupil_radius2`,
`viewer_spherical_weight`, `viewer_cat_eye_open` and
`viewer_cat_eye_fraction`. Two tests were added:
- `test_spherical_weights_average_to_one`: within 4/N for N = 64…2048,
  both signs; non-negative weights; orientation of the effect.
- `test_cat_eye_fraction_matches_samples_and_is_tangential`: the share of
  the viewer's 4096 samples within 0.01 of the analytic fraction for
  d = 0…1.5, and the open pupil taller than wide for a horizontal field.

All 32 tests pass.

### Bright bokeh highlights (mode 2, artistic, optional, 2026-09-24)

User observation: typical night photos show bright bokeh discs, while mode
2's are faint.
- Mode 2 is energy-correct: a point spread over a disc of radius r
  loses brightness as 1/r². Real point lights are orders of magnitude
  brighter than lit surfaces and saturate even when spread. Viewer lights
  and stars are close to surface brightness.
- User decision: an artistic control, optional and off by default, so the
  exact simulation remains available. Mode 1's "Bokeh highlights" slider
  is unchanged.

Per-sample gain in `asDoFAccumulateF.glsl` (`lens_mode` 1):
1 + strength · bright · isolated · min((CoC/4 px)², 1024), where:
- **bright:** a smoothstep of luminance from 0.5T to 1.5T (T = threshold),
  measured before residual smoothing;
- **isolated:** a smoothstep from 2 to 8 on the ratio of the pixel's
  luminance to the mean of an 8-tap ring 6 px away, so point lights
  qualify, while sky, the moon and white walls do not (surfaces keep
  their brightness when defocused, so boosting them would blow them out);
- **area:** the full-aperture CoC radius in pixels from this sample's
  depth and inverse focus, so in-focus pixels (CoC ≤ 1 px) are unchanged
  and the gain compensates the spread with the disc area.

Settings and UI:
- `ASDepthOfFieldApertureHighlights` (off),
  `ASDepthOfFieldApertureHighlightStrength` (0–1, default 0.3) and
  `ASDepthOfFieldApertureHighlightThreshold` (0.05–8, default 1.0).
- All are in the key and `sameSettings()`. The UI flag is
  `ASDepthOfFieldUIHighlights`.
- All are in Reset tuning defaults. The floater height is now 944.

Reference: `viewer_highlight_gain` and
`test_highlight_gain_only_for_defocused_isolated_bright_points` check that
the gain is 1 in focus, when off, below the threshold and for large bright
areas, that it grows with disc area, and that it is capped. All 33 tests
pass.

Correction to an earlier explanation: stars are not fixed-pixel sprites.
`LLVOWLSky::updateStarGeometry` builds 16–36 m world quads on the sky
dome, so they scale with snapshot resolution, like the bokeh and the
residual smoothing. The dotted star bokeh in 4000×4000 snapshots is not a
resolution effect. The open suspects are image-viewer downscaling, the
`fract()` twinkle striping within each star quad, and dot smoothing at 1.

### Sample sequence: Owen-scrambled Sobol (2026-09-24)

User observation: point-light bokeh showed regular patterns and needed many
samples. The user asked whether a tiny random offset would help, subtle
enough not to break the bokeh shape.

Analysis:
- Randomness cannot fill the gaps between sample images: each render is one
  lens point. Dot smoothing approximates the continuous aperture, spreading
  each sample over its share of it.
- Randomness can turn lattice structure into fine grain. The patterns come
  from Kronecker sequences (R4 dimensions 0/1) passing through the polar
  aperture mapping: six-petal "lotus" shapes with R4, spiral arms with R2.

Simulation (6 blades, 90 px radius, 1 px dots, dot smoothing 2; interior
coefficient of variation and FFT peak ratio, where lower is better):

| Sequence | N=512 CV / peak | N=2048 CV / peak |
|---|---|---|
| R4 (dims 0/1) | 0.034 / 40.3 | 0.026 / 37.1 (petals) |
| R2 | 0.023 / 40.7 | 0.007 / 20.3 (faint spirals) |
| R2 + index-decaying jitter λ=0.5 / 1 | 0.029 / 0.036 | 0.020 / 0.031 (grain) |
| Pure random | — | worst: blotchy (0.256 at smoothing 1) |
| **Owen-scrambled Sobol** | **0.019 / 31.4** | **0.005 / 21.1 (no structure)** |

Thin-strand energy error (hair gate: lens and pixel jointly, mean / max
over 8 strand offsets):

| Sequence | 4096 | 16384 |
|---|---|---|
| R4 dims 0–3 (previous) | 1.14 / 2.51% | 0.45 / 0.86% |
| R2 lens + R4 jitter | 1.22 / 2.67% | 0.89 / 1.37% (fails the gate) |
| R2 lens + Halton(5,7) jitter | 1.77 / 3.03% | 0.58 / 0.97% |
| **Owen-scrambled Sobol dims 0–3** | 1.44 / 3.32% | **0.39 / 0.91%** |

Decision: an Owen-scrambled Sobol sequence (`asdofaperture.cpp`).
- **Dimensions:** 0/1 lens, 2/3 pixel jitter, 4 axial-CA wavelength.
- **Direction numbers:** Joe–Kuo polynomials for dimensions 1–4. They were
  hand-checked against the published Gray-code table (for example point 4
  = 0.375 0.375 0.625 0.875 0.375).
- **Scrambling:** hash-based Owen scrambling, a Laine–Karras permutation on
  the reversed bits with fixed per-dimension seeds. It randomizes within
  strata only, so the bokeh shape is never broken.
- **Nets:** power-of-two prefixes are (t, m, 2)-nets, with t = 0 for the
  lens pair and t = 1 for the jitter pair (measured).
- **Wavelength dimension:** the wavelength moved off van der Corput, which
  is Sobol dimension 0 (the lens angle), because reusing it would tie
  colours to aperture sectors.
- **Pupil radius:** `pupilRadius2` = dimension 1.

Verification:
- Python mirror: `sobol_owen_bits`, with the R4/R2 helpers removed.
- New test `test_sobol_owen_stratified_and_decorrelated`: lens t = 0,
  jitter t ≤ 1 for N = 16…1024, and wavelength/lens-angle covariance
  < 5%.
- The existing strand, aperture-density and axial-CA tests pass. The
  axial-CA "sample 0 is green" assertion was dropped, because scrambling
  moves it. 34/34 pass.
- The C++ Sobol block, extracted verbatim from `asdofaperture.cpp` and
  compiled with g++, matches the Python mirror on 50 values (indices up to
  65535, all 5 dimensions).

### Star bokeh grain: twinkle replaced by its mean (2026-09-24)

User report: star bokeh stay "dotted" at any dot smoothing, unlike real
bokeh.
- Geometric coverage was not the limit. At about 55 px bokeh and
  N = 2048, the dots are about 1.1 px apart, and smoothing 2 spreads each
  by about 2 px.
- The texture came from the star shader. Twinkle is
  `fract(screenpos.x + screenpos.y)` with `screenpos = position.xy ×
  mod(time, 1.25)`, which is uniform in [0, 1) and varies per sample and
  per pixel across each star quad. The earlier per-sample twinkle time
  (which removed the per-slice arcs) left every dot a randomly bright,
  speckled copy.
- Simulation (6 blades, Sobol–Owen, N = 2048), interior coefficient of
  variation at smoothing 1 / 2: constant brightness 0.034 / 0.005; random
  twinkle 0.132 / 0.070, 14× worse at smoothing 2 and visibly blotchy.
- Fix:
  - `starsF.glsl` (deferred) gets a tagged `as_twinkle_mean` uniform: when
    it is > 0 it replaces `twinkle()`.
  - `lldrawpoolwlsky.cpp` sets it from `ASDoFRenderer::starTwinkleMean()`:
    0.5 (the mean) while lens samples accumulate, 0 otherwise.
  - The star time is restored to vanilla, and `starTwinkleTime()` is
    removed.
  - A still exposure averages twinkle anyway, so brightness is unchanged
    on average.
- The underwater star shader (`environment/starsF.glsl`) is unchanged.

### Dot smoothing no longer softens sharp hair (2026-09-24)

User report: at 2048 samples, star bokeh still show holes. The user runs
dot smoothing at 0 because smoothing softened thin in-focus hair.
- **Holes at smoothing 0 are expected:** N lens samples image a point as N
  dots, and filling between them is what the residual smoothing is for.
- **The softening was a bug:** the residual disk was a plain gather sized
  by the centre pixel's own defocus. A defocused background pixel next to
  a sharp strand averaged in the strand's colour, so the strand bled into
  its surroundings.
- **Fix, `asDoFAccumulateF.glsl` (scatter-as-gather rule):** each tap
  reads its own depth and contributes only when its own residual radius
  reaches the centre (tap radius ≥ tap distance). The result is the mean of
  the contributing taps, centre included. Sharp content (radius ≈ 0) never
  leaks outward, and defocused content still fills its own gaps. The cost
  is 12 extra depth fetches per pixel per sample while smoothing is on.
- **Follow-up (user, dot smoothing 6: hair outline soft against the sky):**
  the reach rule alone let the heavily blurred sky, whose radius reaches
  far, spread onto the nearer hair edge pixels. Physically, a farther
  surface cannot blur over a nearer one, because the nearer surface
  occludes it in every lens position. Taps now also have to satisfy
  tap distance ≤ centre distance × 1.05 (same-surface slack). The rest of
  the softening at 6 is by design: at N = 2048, strength 6 widens every
  defocused point by 6·√(π/N) ≈ 23% of its CoC, far beyond the dot
  spacing. 1.5–2 only fills the gaps.
- **Depth used for hair:** in mode 2 the mode-1 private capture declines
  (`prepareTransparentDepthCapture` requires mode 1), so
  `LLDrawPoolAlpha`'s fallback runs the vanilla depth replay (alpha ≥
  0.33) into the scene depth. Hair has correct depth except the faintest
  strand tips, which read the depth behind them.

### Final smoothing of the average (2026-09-24)

User report: bokeh keep a dotted look at any dot smoothing, unlike real
bokeh.
- **Why dot smoothing cannot remove it:** it gathers 12 point taps over a
  disk of up to 24 px. A star dot of 1–2 px is picked up only where a tap
  lands on it, so each dot becomes 13 scattered copies instead of being
  spread. Raising the strength widens the disk, which softens edges and
  hair without covering it.
- **New pass, `ASDepthOfFieldApertureSmoothing` (default 2, 0 off, up to
  4):** it runs on the shown average (`presentAverage()`), in live and
  snapshot views.
  1. `Draw::MASKED` (shader `lens_mode` 3) writes the normalized average
     premultiplied by a defocus mask (smoothstep of the pixel's radius over
     0.5–1.5 px) into `Accumulator::mSmooth`. This is RGBA16F with automatic
     mips, released with its slot.
  2. `Draw::SMOOTHED` (`lens_mode` 4) gathers 16 Vogel taps over radius
     `strength · R · ppu · |1/S_f − 1/S| · √(π / max(N, 4))`, capped at
     32 px. N is the samples averaged so far, so the pass fades as the image
     develops. Each tap reads the mip level about twice the spacing between
     taps (`log2(r·√(π/16)) + 1`), so a tap covers an area instead of a
     point. The centre also reads the mips, because one point tap on a dot
     keeps it visible. The mask keeps sharp pixels out of the mips (no hair
     colour halo). The reach and occlusion rules of the residual apply per
     tap. Glow (alpha) is not smoothed.
- **Depth:** the pass uses the depth of the last view rendered. That is
  the central view for converged frames and the last lens sample while
  accumulating. At the focal plane every sample agrees. Elsewhere the error
  is below the CoC the radius follows.
- **Simulation (disk R = 100 px, stratified dots, interior std / mean):**

  | | N = 256 | N = 2048 |
  |---|---|---|
  | raw dots | — | 3.57 |
  | point taps, strength 1 | — | 0.50 |
  | mip taps + raw centre, strength 1 | — | 0.32 |
  | mip taps, strength 1 | 0.22 | 0.24 |
  | mip taps, strength 2, LOD + 1 (shipped) | 0.05 | 0.06 |

  Dot smoothing (per sample) still applies before this pass and only helps.
  The shape edge softens by about 2 dot spacings (about 8 px for a 100 px
  CoC at 2048 samples).
- **First runtime result (user):** a bright flash at the start, then the
  whole image darkened progressively. Two causes, both fixed:
  - **The pass ran from the first sample.** A few samples meant a radius
    at the 32 px cap, a heavily blurred image that then faded. It now runs
    only on a finished average: a blocking snapshot, the last slice of a
    capture, or a converged or complete live average. Developing frames
    and progress previews show the plain average.
  - **Invalid values were spread by the mips.** The smoothing input is
    half float. HDR beyond its range (sun) overflowed to Inf, and any stray
    NaN/Inf, which is harmless as a single pixel, reached whole regions
    through the mips and fed the auto-exposure meter, which then drifted.
    MASKED now writes 0 for non-finite pixels and clamps to 60000.
    SMOOTHED keeps the plain pixel when its result is not finite.
  - Mip generation is now explicit (`TMG_MANUAL`, texture unit 0 activated
    before `glGenerateMipmap`) instead of relying on `flush()`, which only
    activates the unit when the bound texture changes.
- **Second runtime result (user): hair blurred after smoothing.** The pass
  smooths every pixel whose depth is defocused, not only bokeh. Faint hair
  strands (alpha < 0.33) wrote no depth in mode 2's fallback depth replay
  (`lldrawpoolalpha.cpp`), so they carried the background's depth: they
  were smoothed and spread into the background. With mode 2 on, the replay
  now uses `ASDoFRenderer::SHARP_DEPTH_MIN_ALPHA` (0.1). This also helps dot
  smoothing, and autofocus may now pick faint strands. The trade-off is
  that bokeh seen through very faint glass (alpha 0.1–0.33) is no longer
  smoothed there.
- **Bokeh shapes only, located exactly (user requirement):** a first
  version guessed bokeh from brightness contrast in the average. The user
  pointed out that the shapes are known, which is correct. Each lens sample
  is a sharp view from one aperture point, so each small light is a dot
  exactly where that sample's ray lands inside its bokeh. The union of the
  dots over the samples is the bokeh shape, with the aperture shape, cat's
  eye and partial occlusion included.
  - **Per sample** (`Draw::SOURCES`, `lens_mode` 5, after the sample is
    added): an out-of-focus pixel (CoC ≥ 1–2 px) whose luminance exceeds
    the mean of an 8-tap ring by 1.05–1.2× is marked. The ring radius is
    the dot spacing `coc·√(π/N)` (clamped 2–24 px), because a source smaller
    than the spacing is what leaves separate dots. The test is relative, so
    faint stars on a dark sky count. The marks are summed additively into
    `Accumulator::mSources` (R32F, manual mips). The map is copied when a
    capture is seeded from the live average, and invalidated
    (`mSourcesFailed`) if an allocation fails.
  - **At the end** (`lens_mode` 4): coverage = mean of the map over about
    two smoothing radii × π·CoC² / N. Inside a small source's bokeh this is
    its dot area (≥ 1 px), and it is exactly 0 where no source dot ever
    landed. The smoothed value is blended in by
    `smoothstep(0.05, 0.3, coverage)`. Everything else, including plain
    blurred background, skin and hair, keeps the sampled image.
  - **Runtime result (user): the whole background was still smoothed.**
    The first source test (1.05× the ring mean) was wrong. In a sharp lens
    sample, half of any texture is brighter than its ring mean, so the whole
    textured background was marked. Any mark is also enough, because
    coverage multiplies by π·CoC². Every point of a blurred background has
    its own bokeh, so "bokeh shapes only" must mean isolated point lights.
    A source must now exceed the brightest of its 8 ring taps by 1.3–1.5×.
  - **Checking it:** `ASDepthOfFieldApertureShowSmoothing` (checkbox "Show
    smoothed area (red)", not persisted) tints the smoothed area red, so
    the selection is visible rather than assumed.
- **Sources from what the renderer drew (user: "do not guess"):** all
  brightness tests are removed. Mode 5 now marks a pixel in a lens sample
  only when:
  - **Glow:** the screen alpha (glow) is above 0.01. Glow is set by the
    content on bulbs, neon and lamps, and is written by the opaque and alpha
    passes alike.
  - **Stars:** the sky pool drew a star there. `lldrawpoolwlsky.cpp` draws
    the stars a second time right after the real draw, into
    `ASDoFRenderer`'s private R16F star mask. That target shares the scene
    depth buffer (`shareDepthBuffer`, test only), so exactly the visible
    star pixels are marked. `starsF.glsl` gained `as_star_mask`, which
    writes the colour to `frag_data[0]` even with the emissive buffer. The
    mask is drawn only for accumulating lens samples, and not in
    reflection or shadow passes.

  Either way the pixel must be out of focus (CoC ≥ 1 px).
  - **Fix (user: red smoothed hexagons where no star is visible):**
    `POOL_WL_SKY` renders before the opaque pools, so the mask's depth
    test ran before buildings wrote depth, and stars behind them were
    marked. Mode 5 now counts a star pixel only where the finished
    sample's depth is still 1.0 (stars are drawn at the far plane,
    `starsV`: z = w). Stars behind clouds are still marked: clouds blend
    over them without writing depth.
  - **Known gap:** fullbright or PBR-emissive lights without glow are not
    marked. They render after lighting (or through material paths) with no
    per-pixel flag in the buffers mode 2 reads. Marking them means tagging
    those shaders the way the stars are tagged.

### Floater reorganized into tabs (2026-09-24)

User: the floater (996 px) was too tall. `floater_as_depth_of_field.xml` is
now 425 px:
- **Always visible:** Enable, Renderer, and Reset tuning defaults.
- **Tabs:**
  - Aperture: shape, rounding, rotation, anamorphic.
  - Sampling: note, freeze animations, sample counts, snapshot
    samples/time, dot and final smoothing, counter.
  - Lens effects: axial CA, cat's eye, spherical, bright highlights.
  - Advanced: the mode-1 controls.
- **Commented out (settings unchanged):** "Diagnostic view" and "Show
  smoothed area (red)".

The floater is plain `LLFloater` with no C++ child lookups, and
`enabled_control` greying is unchanged.

### Renderer choice not persisted (2026-09-24)

User: logging in with the Aperture-sampled renderer already selected is
confusing. `ASDepthOfFieldMode` is now `Persist 0`, so every session starts
with Standard. A value in an old settings file is ignored
(`LLControlGroup::loadFromFile` applies saved values only to persisted
controls). The mode is still listed in `graphic_preset_controls.xml`, so
loading a preset saved with mode 2 selects it again.

### Still-picture info dialog (2026-09-24)

When `ASDepthOfFieldMode` changes to 2 from any combo, a control listener
in `ASDoFRenderer::registerUICallbacks()` shows the `ASApertureDoFInfo`
alert (tagged block at the end of `notifications.xml`). The alert says
the mode is for still pictures, not live use. It advises freezing
animations first (Ctrl+Alt+N freezes, Ctrl+J restores normal speed) and
locking the focus with Alt+Shift+X, with "Depth of Field focus follows
pointer" enabled. The `okignore` template gives it "Do not show this
again".

### Live accumulation sometimes never starts (2026-09-24, open)

User report: sometimes the live view never starts accumulating. The log
shows no allocation failure, so the live key keeps failing to match every
frame: camera matrices (1e-6 relative), focus (0.5%) or settings. Suspects:
focus-follows-pointer raycast flicker (hair, rigged mesh), which also
retriggers the 1% rack transition, and an alt-zoom focus on an animated
avatar. Diagnostic added: after 10 consecutive restarts, `logLiveRestart()`
logs (at most every 2 s) the old and new focus, the projection and
modelview deltas, and whether settings changed.

Follow-up clue (user): toggling focus lock twice (Alt+Shift+X) fixes it,
which re-captures `sLastFocusPoint`. That points to the silent
invalid-lens path: `makeLens()` rejects a focus at or below the
zoom-adjusted focal length, including a negative distance from a locked
point that is now behind the camera. The frame then renders pinhole
indefinitely. That state is no longer silent: the counter shows "DoF:
focus X m is behind the camera or too close", and it is logged every 5 s
with the focus, view angle, focal length and f-number. Nothing is clamped
(section 3).

### Snapshot preview keeps the finished picture (2026-09-24)

User report: when a capture finished, the snapshot preview was replaced by
the current screen, and a new sampling started. At the end of a capture,
`LLSnapshotLivePreview::onIdle` reset `mThumbnailSubsampled` to false, so
`generateThumbnailImage(true)` took a screen-grab thumbnail
(`thumbnailSnapshot`). That rendered the live view through
`requestPreviewCapture()` and restarted the live average when its key
differed from the capture's. With mode 2 on, the thumbnail now stays
subsampled: the finished picture, scaled to the thumbnail. It remains so
until the user refreshes. Other modes keep vanilla framing
(`mThumbnailSubsampled` false; nothing else sets it).

### Frozen world and UI-only screen during a sliced capture (2026-09-26)

User request: during snapshot sampling the world kept updating between
slices. Avatars walked (in T-pose with "Freeze animations", which only sets
the animation time factor to 0) and could push the user's avatar. Each live
frame also re-rendered the whole scene, taking time from sampling. Wanted:
freeze the world and the screen, keep the snapshot preview progress and chat
typing working.

Why server movement showed: messages must still be processed (circuit acks,
object state; dropping them would disconnect the viewer or lose state), but
applying them to what is drawn can wait. Object messages only change the
`LLViewerObject` position and put the drawable on the moved list. The drawn
transform changes in `LLPipeline::updateMove()`, and for avatars in
`LLVOAvatar::updateCharacter()` → `updateMoveDampedAsync()`. Avatar roots and
the agent camera follow the *render* (drawable) position
(`LLVOAvatar::updateCharacter`, `LLAgent::getPositionAgent`). The upstream
freeze-frame mechanism (`LLPipeline::FreezeTime`, used by the snapshot
floater's freeze frame and the 360 capture) blocks exactly those, so the
server's pushes wait until the capture ends.

Mechanism (`ASDoFRenderer::isWorldFrozen()`, on while `isCapturePending()`):
- `setWorldFrozen()` pauses every `LLCharacter` (`requestPause()`, same as
  the snapshot floater's freeze frame) and sets `LLPipeline::FreezeTime`.
  Tagged hooks: `LLPipeline::refreshCachedSettings()` ORs the freeze in so a
  settings refresh cannot clear it; `LLViewerObjectList::update()` takes its
  avatar-only idle branch on `LLPipeline::FreezeTime` too (the vanilla branch
  reads the setting); `LLAppViewer::idle()` skips particle simulation.
- Thaw: when the last slice ends the capture, when a live frame finds no
  pending capture (abandoned after 2 s, or replaced by a blocking snapshot),
  on mode change and in `releaseResources()`. The moved list then applies
  every position received meanwhile; avatars resync on their next idle
  update. The thaw also drops the live average's key (user request): the
  world has moved on, so the live view restarts from a pinhole frame instead
  of continuing a sum from before the capture.
- Live frames show the developing capture, not the world (user request,
  same day: see whether enough samples accumulated). Each slice's
  `render_ui()` calls `keepFrozenView()` right after `renderFinalize()`
  (post-processed, before any UI). It blits the world-view rect of the
  bound framebuffer (back buffer, or `rawSnapshot()`'s full-size scratch
  target) with `GL_LINEAR` into `sFrozenView`: RGBA8, at most twice the
  window fit, manual mips. `display()` skips the whole 3D block while
  `isLiveViewFrozen()`: it calls `display_update_camera()` (3D matrices for
  name tags and selection), then `render_ui()`, where `presentFrozenView()`
  clears the window and draws the image in place of `renderFinalize()`. The
  image is centred in the world view and fitted, never above 1:1, with
  trilinear sampling (the accumulate shader's `COPY` mode). HUD
  attachments, floaters, chat, the snapshot preview and the "Snapshot DoF
  n / m" counter still draw. No world frame is rendered during a capture.
  When a snapshot smaller than the window is cropped to another aspect,
  the screen shows the uncropped render.
- Slices shortened from 0.3 to 0.15 s (`CAPTURE_SLICE_SECONDS`): a UI-only
  frame is cheap, and UI refreshes about 6 times a second instead of 3.
  Per-slice readback overhead grows accordingly; to be measured.

Rezzing during a capture (user report, 2026-09-26: new people appeared in
the snapshot). The slices' own `display()` created drawables, rebuilt
geometry and uploaded textures. While frozen, all of it waits (tagged
hooks), and it catches up on thaw:
- `LLPipeline::addObject()` queues new objects in `mCreateQ` (the upstream
  `RenderDelayCreation` path, so code already handles an object without a
  drawable for a while), and `createObjects()` returns early. New
  avatars and objects stay invisible until the capture ends.
- `LLPipeline::updateGeom()` returns early (like for cube snapshots):
  no rebuilds from server shape/colour changes or LOD switches.
- `rebuildPriorityGroups()` skips `gMeshRepo.notifyLoadedMeshes()`: loaded
  meshes wait (fetch dispatch also runs there, so fetching pauses too).
- `display()` skips `gBumpImageList` / `gTextureList.updateImages()`: no
  texture sharpening or bake arrival, so avatars already present do not
  finish rezzing mid-capture. `LLViewerTexture::updateClass()` still runs.

Still not frozen: objects killed by the server (people leaving) disappear
at once; deferring kills would keep dead objects referenced. Avatar
appearance messages that change visual parameters of an already-rezzed
avatar can still apply.

Esc on Linux and macOS: `captureCancelRequested()` read only Windows'
`GetAsyncKeyState`, so Esc did nothing elsewhere. A tagged hook at the top
of `LLViewerWindow::handleKey()` (every key-down, all platforms) calls
`ASDoFRenderer::noteEscapeKey()`, which records Esc while a sliced capture
is pending; the next slice consumes it. An edge event, so a press that
starts and ends during a slice is not missed (the key level would be).
Blocking captures (tiled, File > Take Snapshot to Disk; main loop
suspended, no input events) read Esc directly on every platform, per sample
and at capture start (stale press discarded), only while the viewer has the
keyboard:
- Windows: `GetAsyncKeyState(VK_ESCAPE) & 0x8001` (down now or since the
  last query), foreground window only.
- Linux (SDL2, the default build; SDL1 builds only get the sliced path):
  `SDL_PumpEvents()` then `SDL_PeepEvents(SDL_PEEKEVENT, SDL_KEYDOWN)`. It
  peeks, removing nothing, so the viewer handles the events afterwards. An
  Esc key-down newer than the last one seen (timestamp) counts once. The
  only SDL event filter answers X11 clipboard requests, which is safe
  mid-render. SDL sends keyboard events to the focused window only.
- macOS: `CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState,
  kVK_Escape 0x35)` while `gFocusMgr.getAppHasFocus()`. It reads the level
  only (no press history outside the event loop), so a tap shorter than
  one sample can be missed; holding Esc stops the capture.

The progress screen and the counter show "(Esc to stop)" on every platform.
Linux and macOS paths are not built or tested by us (Windows-only builds).

Esc and the final smoothing: the log showed recent stops as "stopped by a
view change". Esc also resets the camera, and the capture sees that first.
In that branch `planSnapshot()` kept the *new* camera for the last slice,
so its sample-0 depth, which `presentAverage()` smoothing reads (defocus
mask, source coverage), was another view's. The last slice now always
renders the capture's matrices, like the drift branch, so the finished
average gets its final smoothing of bright lights.

### Autofocus: area and eyes (2026-09-27, all renderers)

Module `asdofautofocus.{h,cpp}`, shader `deferred/asDoFAutofocusF.glsl`,
setting `ASDepthOfFieldFocusMode` (0 Firestorm focus point, 1 area, 2 eyes),
floater tab "Focus". Hook: `LLPipeline::renderDoF` calls
`ASDoFAutofocus::update()` after the Firestorm focus point; a result
replaces `current_distance` and skips the Firestorm cosine transition, so
modes 0, 1 and 2 all use it (mode 2 through `setFocusDistance`).

- Area sampling: a 64 x 33 R32F pass reads `mRT->deferredScreen` depth at
  2048 points (golden-ratio angle, Box-Muller radius, sigma = 1/3 of the
  half area, clamped to the area), linearized with `inv_proj`. Row 32 is
  the distance at the tracked eyes.
- Readback: 3 PBO slots, `glReadPixels` into a PBO + fence; collected with a
  zero-timeout `glClientWaitSync` 1-2 frames later. No stall.
- Focus: weights 1/distance, sorted, weighted quantile
  `0.5 - 0.45 * NearPriority`. Sky and far background barely count.
- Eyes: `LLCharacter::sInstances`, non-control avatars with a visible
  drawable (self skipped in mouselook); the avatar whose eyes are inside
  the area, nearest the camera (was: nearest the area centre, which in a
  crowd picked people behind the subject; user report 2026-09-27). One
  whose eyes are hidden (seen from behind) fails the occlusion probe and
  area autofocus takes over. Focus point: the camera-facing surface of the nearer eye
  (`mEyeLeftp`/`mEyeRightp` are eyeball centres: centre + 12 mm toward the
  camera), as photographers focus on the near eye, not the eyeball centre.
- Eye focus point from the mesh eyeballs (`measureEyes()`,
  `collectEyeVertices()`): on every prim (root and children: mesh eyes are
  linksets) of every world attachment rigged to an eye joint
  (`mEyeLeft/Right`, Bento `mFaceEyeAltLeft/Right`; skin joint names
  resolved with `LLVOAvatar::getJoint`), the vertices weighted >= 90% to
  one eye joint are the eyeball (it rotates with the joint; lids, lashes
  and skin follow other bones). Weights decoded as `LLSkinningUtil` does
  (integer part = mesh joint index, fraction = weight). Faces with alpha 0
  skipped (HUD-hidden alternate eyes). Only faces holding eye vertices are
  skinned (`updateRiggedVolume(true, face, false)`, current pose, agent
  space). Vertices are grouped per layer (face) and classified opaque or
  alpha-blended (`LLFace::isInAlphaPool`): mesh eyes stack opaque layers
  (sclera, iris, pupil) and alpha shells (cornea, shine, wetness). The
  opaque layers are used (all layers if they hold < 8 vertices), stored in
  the joint frame. Focus point each frame: the stored vertex nearest the
  camera along the view axis (`eyesPosition()`: one dot product per
  vertex with the view axis rotated into the joint frame), i.e. the
  iris/sclera facing the camera. No shape assumed. Joint set: the one with
  more eyeball vertices.
- Runtime history: rays (root prims only, then per-prim picking gates) hit
  nothing; then a sphere model (bounding-box centre + farthest vertex
  toward the camera) measured "10.4 mm, centre 8.3 mm from joint" on a
  LeLutka setup and a hi-res snapshot showed neither eye sharp: an eye
  rotates about its joint, so an 8 mm centre offset meant the box covered
  more than the eyeball (extra layers), and the focus landed millimetres
  in front of the iris. Hence the per-layer, shape-free front vertex.
  Result (user, bokt, 2026-09-27, LeLutka setup): eyes sharp. Layer log per
  eye: iris/pupil disc (opaque, 2.5 x 15.5 x 15.5 mm, front 18.2 mm before
  the joint), sclera front hemisphere (opaque, 16.9 x 34 x 34 mm: radius
  ~17 mm, centred on the joint), cornea/wet shell (alpha, ignored, front
  18.2 mm). So the joint is the eyeball centre; mesh eyes can be much
  larger than a real eye (12 mm); only the front half is modelled, which
  put the old box centre 8-9 mm forward. Manual radius sweet spot on the
  old build was 8-9 mm from that box centre.
- Log (`ASDoFAutofocus`): "Eyeball measure" per eye and layer (vertices,
  opaque/alpha, used/ignored, extents in the joint frame, centre and
  front distances from the joint); "Eye lock" at a subject lock (view-axis
  distances of the joint, the focus point and the current focus).
- `ASDepthOfFieldAutofocusEyeRadius` (mm) > 0: focus at the used layers'
  box centre plus that radius toward the camera (manual override).
  Unmeasurable (system eyes): joint + 12 mm toward the camera.
- Measured once per avatar (the chosen one, first time it is focused on)
  and cached; failures (meshes loading) retry every 5 s, 3 times. No
  automatic re-measure (a slightly different point could restart a
  converging mode 2 image): the Focus tab's "Redetect" button
  (`ASDepthOfField.RedetectEyes`) clears the cache after an eye or head
  change. Snapshots and captures never measure (`update()` holds first).
  The helper label shows "eye x.x mm measured" (live distance from the eye
  joint, the eyeball centre, to the focus point: the iris front),
  "eye 12.0 mm default" or "eye x.x mm set". The helper is hidden under the
  teleport/login progress screen and with the UI hidden (Ctrl+Alt+F1).
- Eye occlusion: eyes count as visible unless the depth probe is nearer
  than 0.9 x eye distance - 5 cm; otherwise area focus.
- Smoothing: in 1/distance, `alpha = 1 - 0.01^(dt / AutofocusTime)`.
  Dead band: starts moving above 1% relative change, snaps below 0.3%, so
  idle animation noise does not restart mode 2 accumulation (its own
  restart tolerance is 0.5%).
- Snapshots (`gSnapshot`) and sliced captures (`isWorldFrozen()`) hold the
  current value: a snapshot uses exactly the live autofocus distance.
- Toggle: menu "Depth of Field Autofocus" (Alt+Shift+Z, free in
  `menu_viewer.xml`; Alt+Shift+F is Joystick Flycam) switches between point
  focus and `ASDepthOfFieldAutofocusLastMode`.
- Focus lock (`FSFocusPointLocked`, Alt+Shift+X) in autofocus,
  `ASDepthOfFieldAutofocusLockMode`: 0 freezes the distance (classic);
  1 locks the subject at the lock press: the tracked eyes (eye mode), else a
  `lineSegmentIntersectInWorld` ray through the area centre (rigged
  picking on). Avatar hits (body, rigged or attached mesh) store the offset
  in the nearest skeleton joint's frame (a mesh face follows the head);
  other objects in the object's frame (`getPositionAgent`,
  `getRotationRegion`). Subjects are re-found by UUID each frame; gone,
  behind the camera, or outside the area with
  `ASDepthOfFieldAutofocusTrackOutside` off: the distance holds. No hit:
  classic freeze. Point mode keeps Firestorm's lock unchanged.
- Overlay (`drawOverlay()` after `drawProgress()` in `render_ui()`): area
  outline (yellow, green on eyes, red locked), subject/eye marker and
  "AF x.xx m (locked: name)", while autofocus is on, with
  `ASDepthOfFieldAutofocusShowArea` (menu "Show DoF Autofocus Area",
  Alt+Shift+V, free in `menu_viewer.xml`; was: only while the DoF floater
  is open) or "Draw DoF Focus crosshair" (`FSFocusPointRender`), whose 3D crosshair
  `renderFocusPoint()` skips in autofocus.
- Known limit: alpha-blended surfaces do not write scene depth; area
  autofocus sees behind them.

### Advanced renderer (mode 1): light shapes and foreground layers (2026-09-28)

User decision: resume the live screen-space renderer (mode 1) with roadmap
items 2 and 3 of `rendering-improvements-backlog.md`. Plan:
[ayanestorm-depth-of-field-advanced-sprites-layers-plan.md](ayanestorm-depth-of-field-advanced-sprites-layers-plan.md).
One correction to it: `unitArea` = anamorphic × blades × `bladeCdf(half)`
(`bladeCdf` already spans a whole blade), not × 2. Owned files only, no build.

**Shape convention aligned with mode 2.**
- The thin-lens algebra of `ASDoFCamera` gives an image shift of
  `P00·dx·(1/f − 1/d)`. Far points therefore image as the upright aperture,
  near points as the inverted one.
- Mode 1 had both planes reversed, and it put an edge centre where mode 2
  has a blade vertex.
- The far, near and transparent gathers now sample `uv − disk` (far) and
  `uv + disk` (near), with a vertex at the rotation angle. The difference
  was visible only with odd blade counts.

**Light shapes (item 2)**, `asDepthOfFieldHighlightF.glsl` and
`asDepthOfFieldSpriteV/F.glsl`:
- **Detection:** a defocused pixel (radius smoothstep 2–4 px) that is
  `isolation`–2×`isolation` times brighter than the brightest of 8 ring taps
  at 6 px gives up its excess over the ring mean.
- **Cells:** 8×8 cells sum that energy with its centroid and CoC. A mip of
  the occupancy flag counts the occupied cells for the budget
  (`ASDepthOfFieldHighlightMaxSprites`). Over budget, a stable per-cell hash
  drops cells, and dropped highlights stay in the gather.
- **Gather input:** a full-resolution copy of the opaque colour minus the
  kept extraction. It feeds the far, near and background passes. The
  resolve and the transparent gathers keep the originals.
- **Sprites:** one attribute-free instanced draw (2 triangles per cell) per
  plane, following the `asAVBOITEarlyDepthV` precedent. They are drawn
  additively into the far target and into the near front layer. Radiance is
  E / (`ASDoFAperture::unitArea` · R²), with alpha 0, so the resolve needs
  no new sampler and hides far sprites behind in-focus content.
- **Antialiasing:** perpendicular edge distance. Sprites under 12 target
  pixels use a 4-sample rotated grid.
- **Limit:** highlights that exist only in transparent layers are not
  extracted.

**Foreground layers and background completion (item 3):**
- **Two near layers:** the near gather writes a back and a front
  premultiplied layer (MRT), split softly at half the foreground radius. The
  resolve composites back, then front. Resolve samplers: 16, the GL 4.1
  minimum.
- **Background completion:** push-pull at gather resolution, in
  `asDepthOfFieldBackgroundF.glsl`.
  - Non-foreground texels are weighted by exp2(3·CoC), so farther surfaces
    win over in-focus mid-ground.
  - Holes take the finest mip with at least 5% valid texels, blended toward
    the next coarser level.
- **Far pass:** foreground-centre pixels now blur the completion with its
  own CoC; this replaces the old average of non-foreground taps. Far taps
  that land on foreground read the completion instead of being rejected.

Settings (persisted): `ASDepthOfFieldHighlightSprites` (on),
`ASDepthOfFieldHighlightIsolation` (2.0), `ASDepthOfFieldHighlightMaxSprites`
(4096), all in the Advanced tab and in Reset tuning. Debug views 16–19:
energy moved to shapes, near back layer, near front layer, background
completion.

**Reference tests** (`dof_reference.py`, 39 pass):
- mode-1 boundary = aperture sampler boundary;
- far shift parallel to and along the lens offset, near shift against it;
- `unitArea` against a dense grid (< 0.5%);
- extraction conserves energy exactly, with 1 cell, or 2 cells when the
  light straddles a boundary;
- sprite splat energy within 1% for radii ≥ 4 target pixels at gather
  scale 1 and 0.5.
- Measured, not gated: 1–2 target-pixel sprites err 1–10% (sharp triangle
  worst).

First runtime results (user):
- The Highlight shader failed to link: `centroid` is a reserved GLSL word.
  It was renamed, and mode 1 had fallen back to Firestorm DoF until then.
- Some star bokeh stayed dotted. Stars are 16–36 m quads on the
  15000 m dome (`llvowlsky.cpp`), 0.06–0.14°, so on a zoomed portrait view
  the larger ones exceed the fixed 6 px ring, fail the isolation test and
  stay in the gather.
  - The ring is now half the blur radius (6–32 px): a light qualifies when
    it is smaller than about half its bokeh.
  - A multi-cell light gets one sprite per cell, and these merge at such
    radii.
  - New test case: a 10 px light with a 40 px blur is fully extracted.
  - Faint stars, between 1× and 2× `isolation` brighter than their ring,
    are still only partly extracted by design,
    to avoid popping; debug 16 shows which are.

- **Mesh pattern on a defocused hair lock over an in-focus cheek.** Debug 7
  (opaque CoC) showed the lock in opaque depth: alpha-masked hair, which
  goes through the opaque near layers, not the rigged stratum. Two
  regressions from this item:
  - **Coverage leak from the soft layer split.** A surface split between
    the back and front near layers, composited over, loses coverage
    (0.5 / 0.5 gives 0.75), and the plate shows through it noisily.
    - The resolve now keeps front-over-back for colour, but uses the
      clamped sum of both coverages: the old single-layer estimate.
    - The rescale is at most 4/3, and only where both layers are
      partial.
  - **Plate favoured the distant background.** The exp2(3·CoC) farness
    weight filled the lock with the sky past the head instead of the
    cheek. It was removed; push-pull is now unweighted, so the nearest
    valid pixels on screen win.

- After both fixes (user): "much better", a fine pattern remains on the
  lock (open; see below).
  - Debug 19 was burnt out: it dimmed the context to 25%, and
    auto-exposure brightened the whole frame. The context now shows as
    grey at its own luminance.
- **Autofocus overlay drawn over HUDs and UI panels.**
  `ASDoFAutofocus::drawOverlay()` moved from after `render_ui_2d()` to
  just before `render_hud_elements()` (tagged, `llviewerdisplay.cpp`).
  - It now sets up 2D state itself and restores the projection and
    modelview stacks and `gGLViewport` for the HUD elements.
  - The mode-2 progress counter stays on top.

- **Debug views bloomed:** every debug output wrote alpha 1, which is
  glow, so the whole frame bloomed. That caused the soft blobs in debug
  16, the burnt debug 19 and the blurry edges of debug 10. All debug
  outputs now write glow 0.
- **Remaining fine pattern on the lock: near-gather sampling noise.**
  - Findings:
    - Bokeh quality changes it directly (Low worst, Cinematic best);
      `CameraDoFResScale` hardly does.
    - Debug 18 (front layer) is empty, and light shapes off changes
      nothing.
    - Point taps see a thin defocused strand only when they land on it,
      so its spread coverage flickers from pixel to pixel. This predates
      today (backlog: "stippled foreground hair").
  - **Fix: area taps.**
    - A near source pyramid at gather resolution (`sNearSourceTarget`,
      4 RGBA16F attachments, manual mips, built by `near_pass` 0 of
      `asDepthOfFieldNearF.glsl`) holds colour and 11 reach bands.
    - Each band stores the area average of each source's reach,
      w·[d < r] with w = R²/r².
    - Each tap reads the mip level that matches the local tap spacing,
      √(2π·d·R/N).
    - This is linear in the sources, so mips stay exact for any radius
      mix, and the integrated coverage is exact.
    - The front layer uses min(band, front weight). Sources under 1 px
      of blur stay point taps.
  - **Measured** (`scripts/testing/dof_near_gather_sim.py`, 96 taps):
    per-pixel coverage error 3.5–7× lower, mean within 2% on uniform,
    1:5 mixed, ramped and face scenes.
  - Rejected by the same simulation:
    - one mean radius per texel: −33% on mixed radii;
    - point values of the reach at 7 or 11 distances: +6–16%.
  - Cost: one pass, 4 mip chains, and 2–4 fetches per tap instead of 2.
    The tap count is unchanged. Below 2 px of maximum foreground blur,
    the gather keeps point taps.
  - New test (`dof_reference.py`, 40 pass): the bands hold every tap
    distance, and band averages integrate to the exact splat area.

- **Area taps made no visible difference (user).**
  - The log shows mode 1 active with no shader errors.
  - Debug 12 (rigged coverage) is about 1 over the whole hair, lock
    included. The lock renders through the rigged transparent stratum,
    which the resolve composites over the opaque result. Its alpha also
    passes the depth replay, which is why debug 7 showed it.
  - Debug 15 (rigged near colour) is smooth, so the pattern is in that
    gather's coverage, which still used point taps.
  - The blocks in debug 4 are only colour ÷ coverage over sparse mip
    texels, and do not reach the final image.
  - **Change: `asDepthOfFieldTransparentF.glsl` gets the same area taps**,
    in all four transparent gathers (world and rigged, far and near).
    - A `gather_pass` 0 rebuilds the shared `sNearSourceTarget` for each
      gather (layer and plane). No extra memory: the opaque near gather
      is done with it.
    - Attachment 0 holds colour per unit coverage (Σ rgb·hw/r²,
      Σ a/r²); 1–3 hold the 11 reach bands of a·R²/r².
    - Cost: 4 extra build passes and their mips per frame.
  - New debug views: 20 = rigged near coverage, 21 = world near coverage.

- **Transparent area taps: the pattern remains (user: "bad, more or
  less").** Dark speckles on the lock over the cheek.
  - Debug 20 (rigged near coverage) is smooth, so the rigged gather is not
    the source.
  - Debug 13 shows world coverage only on the windows, and debug 21 is
    black. That rules out the suspected world-layer division by
    (1 − rigged coverage).
  - Rigged depth is captured with minimum alpha 0.004, so the front lock
    should own the nearest rigged depth.
  - Unexplained so far. New stage views to locate it:
    - 22: opaque result before the transparent layers;
    - 23: the rigged layer as composited;
    - 24: the final blend toward the blurred result (white) versus the
      sharp compositor output (black).

- **Stage views located it (user captures).**
  - 23 (rigged layer as composited) is smooth on the lock.
  - 24 (blend) is white on the lock: blurred result everywhere. Pixels
    without transparency are white by design.
  - 22 (opaque result) shows the lock's alpha-masked strands sharp and
    dark with flecked edges, though they are defocused (debug 7).
  - **Cause:** the near gather's ownership rule gave every foreground
    pixel full coverage at its own position. It was meant for point taps,
    which under-estimate interiors and would tear holes. On a thin strand
    it keeps the strand opaque and sharp, and the blurred rigged layer
    above leaves it visible as dark flecks.
  - **Fix:** with area taps (which estimate interiors correctly) the rule
    applies only to point-tapped sources under 1 px of blur.
    - A defocused strand now becomes a veil over the background plate.
    - A solid foreground area still reaches coverage 1 inside and 0.5 at
      its edge.

- **Worse after removing ownership (user).** The flecks are gone only at
  foreground radius 0.
  - Debug 22 still shows sharp dark lock strands.
  - Debug 2 (near coverage) is about 1 over the whole side of the head,
    which is foreground with a small blur.
  - Debug 4 (near colour) carries the sharp dark strands.
  - **Cause, in the area taps:** one pyramid colour per texel, weighted by
    pixel count. The face beside the lock (r ≈ 2 px, weight 1/4) dominates
    coverage, while the lock (r ≈ 15, weight 1/225) supplies half the
    colour, so the face's contribution took the lock's dark colour.
  - **Fix:** split by source radius, at 8πR/N, clamped to at least 1 px.
    - Only sources above it enter the pyramid.
    - Smaller ones stay point taps (tap spacing at distance r is under
      r/2 there), with their own colour and their ownership rule.
    - The pyramid is used only when the split is below R.
  - Simulated (`dof_near_gather_sim.py lockface`, 96 taps, R = 24):
    - colour error 0.011, against 0.033 for point taps and 0.044 for one
      pyramid;
    - colour weighted 1/r²: 0.109, rejected;
    - trade-off: strands blurred less than the split keep point-tap
      coverage noise (mixed: 0.042 against 0.014).

- **Still bad after the split (user: "you're not fixing the issue").**
  Debug 22 kept the sharp dark strands through three different near
  gathers, so they never came from the near layer.
  - **Actual cause, in the resolve (pre-existing):**
    - At a defocused foreground pixel, the base colour starts as the
      pixel itself (`opaque_source`, since `far_blend` is 0 for
      foreground).
    - It was replaced by the plate only by the far target's alpha, and the
      far pass classifies foreground at gather resolution (0.7) from a
      bilinear CoC.
    - A 1–2 px strand blends with the in-focus cheek, is not classified
      as foreground, and gets alpha ≈ 0. So the sharp strand stayed as the
      base and showed through the near layer's partial coverage.
    - The old ownership rule hid this by painting the strand's own dark
      colour there.
  - **Fix:** under a pixel with near blur > 1 px, the base is always the
    background completion (`sBackgroundTarget`, which covers every
    foreground pixel). It is blended toward the far target's blurred plate
    by far alpha × smoothstep(0.5, 2, completion blur).
  - The completion takes the resolve's 16th sampler (`brdfLut`): debug 16
    now shows it, and the sprite-energy view is dropped.
  - I should have traced the resolve after the first unchanged debug 22,
    instead of reworking the gathers.

- **Compared with Firestorm (user):** Firestorm's single blur of the
  composited image renders the lock as a smooth veil; ours still showed
  thin dark strokes along it.
  - Remaining cause: the ownership rule, still active for point-tapped
    sources under the split (≈6 px at Cinematic). It painted such strands
    opaque in their own sharp colour.
  - **Fix:** ownership only below 1 px of blur, for every path.
  - Point-tap simulation of a solid foreground half-plane without
    ownership, 200 random phases per case (N 16/32/96, R 12/24/60,
    r 1.5–0.75R):
    - one radius or more inside the edge, coverage 1.000 (one case 0.96:
      N 32, R 60, r 6);
    - at the edge, coverage 0.5–0.77.
    So solid interiors need no ownership, and the old "torn holes" should
    not return. Runtime check: a hand close to the camera.
- **Still bad (user); full read of the C++ and every shader.** The real
  cause is in the transparent composite, not the gathers.
  - The mode-23 capture (rigged layer over black) contains the face: the
    face is alpha-blended and rigged, like the hair. The rigged replay
    keeps one depth per pixel (the nearest), so a strand pixel carries
    the face with the strand's blur. Nothing of the in-focus face remains
    behind it.
  - The resolve took `mix(raw, near, blend)` per stratum. An in-focus
    pixel kept only its raw content, so the strand's blurred spread was
    dropped at every face pixel and reached the image only inside the
    strand's own pixels, over a base without the face layer. The final
    `mix(source, color, visible_blur)` also ignored the transparent near
    coverage. Result: strand-shaped dark strokes. Debug 22 (before
    transparency) was already smooth.
  - **Fix** (`composeStratum()`, `riggedBehind()` in
    `asDepthOfFieldResolveF.glsl`; `spreadShare()` in
    `asDepthOfFieldTransparentF.glsl`):
    - every stratum (rigged, world, single-layer) is composited as near
      spread over its own pixel content;
    - the transparent near gathers leave sources under 0.5–2 px of blur
      to that raw content (same smoothstep as the resolve blend), so
      in-focus surfaces stay full-resolution and are not counted twice;
    - where the rigged surface itself is a defocused foreground, the
      in-focus rigged content behind it comes from rings of 8
      full-resolution taps at 2–64 px (nearest rings with in-focus or
      empty rigged pixels); no new sampler;
    - `visible_blur` includes the rigged and world near coverage.
- **Not much better (user: mode 2 correct, mode 1 debug 0 and 23).** In that
  view the face is not in the rigged layer. Debug 23 showed the rigged hair
  smooth, with sharp black strand-shaped holes: pixels where alpha-masked
  (opaque) lock strands hide the rigged hair behind them.
  - **Cause: composite order.** The opaque near veil went over the opaque
    base, then the transparent strata went over it. So the in-focus alpha
    hair behind the defocused opaque strands was painted over their blur.
  - **Fix** (`stratumBase()`, `over()` in the resolve):
    - order is opaque base, then each transparent stratum's base (raw in
      focus, far blur, or the rigged fill behind a defocused surface) in
      depth order, then the opaque near veil, then the transparent near
      veils;
    - the opaque near gather now leaves sources under 0.5–2 px of blur to
      the sharp base (`spreadShare()`, same ramp as the transparent
      gathers), and the resolve base fades from the sharp pixel to the
      completion over the same 0.5–2 px. The ownership rule is gone.
      Otherwise a nearly focused skin pixel, spread at full coverage,
      would now wash out in-focus alpha brows and lashes in front of it.
- **Still jagged strand-shaped strokes (user).** The jagged edges are those
  of the alpha-masked strands. Under an opaque foreground strand the rigged
  hair behind is zeroed (hidden), so the base there showed the face while
  its neighbours showed in-focus rigged hair; the thin veil did not hide
  the difference.
  - **Fix:** `riggedBehind()` also fills rigged content hidden by a
    defocused opaque foreground (blend over the same 0.5–2 px ramp).
    Neighbours whose front surface (rigged if present, else opaque) is a
    defocused foreground count as unknown and are skipped, instead of as
    empty.
- **Diagnosed with captures before changing code.** Debug 23 was clean where
  the strokes were; debug 22 showed the dark lock strands perfectly sharp in
  the opaque result; debug 7 showed them red (depth correct) and the whole
  head red too, since cheek, ear and hair cap lie slightly in front of the
  focus point.
  - **Cause:** the background completion counts only non-foreground pixels.
    Around the lock everything is foreground, so the pull found nothing and
    fell back to the pixel itself (the sharp strand), or reached the sky past
    the head. That sharp strand was the base under the thin veil. The rigged
    fill had the same absolute test, which left the remaining black hole in
    debug 23.
  - **Fix:** "behind" is relative. `opaqueBehind()` (resolve) fills a
    defocused opaque pixel from nearby pixels blurred at least ~2 px less
    (rings of 8 taps, 2–64 px; background neighbours give their blurred
    plate). The completion is used only where nothing is found.
    `riggedBehind()` uses the same relative test.
- **Debug 22 still showed crisp strands (user). Full audit of the opaque
  path: every way a strand pixel can reach debug 22 sharp.**
  1. The base's sharp share under 2 px of blur: correct (a nearly focused
     strand is sharp).
  2. The completion's pull fallback returned the pixel itself when no
     background lay within reach, so the sharp strand was its own "behind".
     The whole-screen background average tried instead showed grey patches
     inside the slightly defocused hair cap (user). **Kept the self
     fallback:** thin strands never reach it, since `opaqueBehind()` fills
     them first from nearby less-blurred surfaces. Deep inside a large
     region the pixel itself is the best estimate.
  3. The near gather's separate centre term added the centre pixel's colour
     at weight 1/r². One source covering the whole disc totals N/R² over all
     taps, so the centre counted R²/(N r²) of a full disc: about 30 % of the
     veil colour for R 20, N 96, r 3. Each thin strand redrew itself into
     the veil at its own pixels. **Fixed:** term removed; the taps near
     d = 0 sample the centre with its true area share.
     `dof_near_gather_sim.py` never modelled this term, which is why the
     simulations did not reproduce the strokes.
  - **Hair showing through a nearly focused arm (user).** Two leaks:
    - the base mixed self (1 − s) + behind · s under a veil that covers
      about s on a solid surface, so behind leaked at s (1 − s), up to 25 %
      (the rigged fill copied ponytail hair behind the arm).
      **Fixed:** `exclusiveBase()`/`exclusiveFill()`: behind fills only
      max(s − veil, 0) / (1 − veil), for the opaque base, the rigged fill
      under opaque surfaces and every stratum base;
    - a transparent stratum's far spread was drawn over any opaque pixel
      without a surface of that stratum, even an in-focus one in front of
      it (the ponytail's blur over the back); this predates the session.
      **Fixed:** scaled by the opaque far blend, as for the opaque plate.
  - **Status (user, 2026-09-28): better, not yet as good as mode 2 or
    Firestorm; paused.** Open leads: some lock strands stay crisp in
    debug 22 (possibly too little mode-1 CoC compared with mode 2); the
    rigged fill still fails where the lock is wider than 64 px.
  - Transparent stage, checked for the same class of defect: no centre term
    in the transparent gathers; the final `mix(source, …, visible_blur)`
    cannot return a sharp opaque strand under defocused alpha hair
    (`rigged_blend` is 1 there), and the coverage capture excludes rigged
    hair hidden behind the strand.

Runtime checks pending (user build): triangle aperture orientation in mode 1
vs mode 2; clean light polygons with blades, rotation and anamorphic visible;
no exposure change when toggling shapes; neon strips and windows stay
gathered (debug 16); foreground edges over far background without mid-ground
leak (debug 19); nearer foreground over farther (17/18); FPS against the
previous mode 1.

## Design notes — screen-space gather comparison (2026-09-24)

User decision: keep aperture re-rendering. Screen-space gather designs remain
comparison points only; no code is taken from them.

Own design choices for the remaining modules:
- Aperture shape (`asdofaperture`): polygon edge radius `cos(π/n)/cos(θ_local)`
  blended to a circle by roundness (standard regular-polygon geometry).
  Equal-weight lens samples come from a nested low-discrepancy sequence
  (originally R4; now Owen-scrambled Sobol, see "Sample sequence" above). The angle is inverted through the per-blade area CDF
  (∝ boundary²) and the radius is `sqrt(v)·boundary`, which gives uniform area
  density and normalized brightness for any shape.
- Autofocus candidates to evaluate later: a robust weighted depth median over a
  focus window, smoothed in reciprocal depth, versus Firestorm's cosine rack.
  Must run once per presented frame.
- CoC model: standard thin lens, `R·|1/S − 1/d|` (`ASDoFCamera::cocRadiusPixels`),
  normalized by the vertical sensor size derived from the default FOV.

Rejected for this plan (section 2 invariant): foreground layer accumulation
with hole fill infers background hidden behind the foreground from neighbouring
pixels. That is the central-view hole filling this plan excludes for hair and
Exact OIT. Highlight sprites and highlight gamma/color shaping are artistic
effects outside scope. Compute and variable-rate passes are unavailable on the
GL 4.1 baseline.

## 1. Decision and delivery contract

Replace the advanced screen-space transparent DoF architecture with aperture-sampled scene rendering. Render real geometry from different positions on a virtual lens, resolve the selected transparency compositor independently for each position, and average complete linear-HDR samples before display processing.

This is the production architecture, not merely an expensive oracle followed by another aggregate-layer replacement. A small independent numerical reference validates it; the same scene-rendering implementation delivers the final images. All milestones below are internal implementation/validation steps, not intermediate products presented as a finished fix.

Quality means converging to the selected viewer renderer evaluated through a thin lens. It does not mean path-traced lighting, physical glass refraction absent from the viewer, or keeping an entire three-dimensional face sharp at an arbitrarily shallow depth of field. A point on the focal plane must remain registered and sharp; points outside it must blur consistently with the lens model.

Finite samples have integration error. No promise of flawless output at every sample count, fixed real-time FPS, unlimited geometry or unlimited transparency memory. Failures must be visible as failed acceptance gates, not concealed with hair-specific thresholds or silent changes to the algorithm.

## 2. Why this replaces the current approach

The current rigged/world capture compresses several depths into one representative depth per class. The failed Exact-node experiment also separated focused and defocused sets before proving their visibility/composition. Neither representation solves aperture-dependent occlusion.

New invariant: resolve visibility and blending for each lens sample BEFORE averaging samples. Do not average layer opacity and then use it to attenuate independently averaged background. Do not split all fragments into three global background/focus/foreground buckets.

Rerasterization exposes geometry hidden from the central camera, including background behind foreground hair. Center-view fragments, depth peeling at the central view, and hole filling cannot in general supply that information.

Exact OIT keeps its existing per-sample ordering, blend factors, glow and shallow-list behavior. We do not require its linked lists to remain globally sorted for a later DoF gather. AVBOIT remains an approximate compositor; DoF must not claim to turn it into Exact OIT. Standard and AYAstorm retain their own transparency behavior. Exact OIT is the principal high-quality acceptance baseline.

## 3. Optical model and controls

Implement an explicit thin-lens camera in a new owned camera-math module. Define positive focus distance, world/metre conversion, effective sensor dimensions, focal length, f-number, aperture orientation and pixel/aspect scaling in one place. Calibrate against the existing camera/FOV behavior; eliminate ambiguous millimetre/metre and CoC radius/diameter conversions.

For each lens position, translate the camera parallel to its image plane and use an off-axis projection that keeps the chosen focal plane fixed. Do not toe-in/rotate the camera toward the focus point. Derive and unit-test matrices against independently generated lens rays, including the viewer's handedness and depth conventions.

Use deterministic, nested, well-distributed aperture samples with correct probability weights. Support circular and rounded polygonal apertures, blade count, rotation and anamorphic shape. Maintain normalized brightness when aperture shape or sample count changes. Jointly sample the pixel footprint for subpixel hair; lens sampling alone does not cure raster edge aliasing. Bokeh follows the sampled aperture, not an added highlight sprite approximation.

Keep focus selection, focal length, f-number and aperture UI after correcting their mapping. New quality controls describe actual samples/convergence. Independent near/far radius, per-depth maximum-CoC clipping, gather resolution and highlight boost remain legacy-only controls with saved values preserved. They must not silently affect the physical renderer. If an aperture limit is needed, constrain the whole lens explicitly rather than clipping blur independently at each depth. No extra artistic lens effects in this project.

## 4. Frame and sample lifecycle

Introduce an owned aperture-render coordinator; do not recursively call display(). Separate once-per-presented-frame work from repeatable scene rendering with minimal upstream hooks.

Once per output frame: update scene state, animations, skinning, particles, texture state, camera/focus and lighting clocks. Freeze the render-visible state and draw inputs for all lens samples of that frame. A rendering pause is not a network/simulator pause. Do not advance simulation, focus smoothing, exposure adaptation or temporal histories once per sample.

Per sample: install scoped camera/projection state, establish valid culling, render opaque and transparent geometry and sample-dependent lighting, finish that sample's selected alpha compositor, and accumulate the complete HDR result plus any separate glow channel. Every sample gets correct depth and fresh OIT capture/resolve state. Central-view depth and occlusion queries must not reject surfaces visible from another lens position.

After all samples: normalize HDR accumulation, apply exposure/tone mapping, bloom and display effects in the established compatible order, render HUD/UI once and present once. Disable both legacy and current advanced DoF inside the new path. Restore ordinary camera, matrices, viewport, FBO stack, depth/blend state and renderer flags on every success, cancellation and failure path.

Keep an unshifted camera/depth result for picking and consumers whose contract requires it. Do not average depth or publish the last lens sample as the main camera history. Audit screen-space reflections, AO, volumetrics, water, reflection probes, motion blur, AA and snapshots individually. View-dependent effects must use sample-consistent inputs; shared caches are allowed only where their camera coverage and semantics remain valid. Temporal consumers update once or use explicit isolated sample state. Never silently disable an effect to pass acceptance.

## 5. Culling, transparency and hidden geometry

Initially evaluate culling for each lens sample without reusing central-view occlusion rejection. Freeze LOD policy consistently over the aperture to avoid geometry switching between samples. Later use a conservative union of lens frusta where equivalent. Include sky, far clip, water/pre-post-water ordering, attachments, alpha masks, PBR/legacy/fullbright content, emissives and particles.

Exact OIT must clear/resolve per sample and keep fences, node ownership, validation and pool growth correct even though samples share one presented frame. Stream samples through a reusable node pool rather than retaining all sample lists. Audit existing beginFrame assumptions: simulation frame and render sample are distinct identities.

On node overflow, allocation or shader failure, discard the incomplete output; do not average failed and successful compositors together. Retry the same frozen state safely if possible, otherwise report failure or a clearly identified whole-frame fallback. Never silently use truncated nodes or fall back to the broken aggregate DoF. Apply the same whole-output consistency rule to AVBOIT failures.

## 6. Accumulation, motion and responsiveness

Use numerically stable full-precision HDR accumulation and normalized weights. Keep source-over coverage and additive/glow semantics from the resolved sample. Do not clamp signed contributions or highlights arbitrarily. Test HDR range and cancellation before selecting a lower precision format.

Provide one renderer with two scheduling policies:

- Live: complete each output from one frozen scene state. Never average consecutive animation states merely to obtain more lens samples. Keep a deterministic sample sequence across camera motion to avoid random per-frame sparkle. Quality remains sample-count limited and is reported honestly.
- Converged capture: progressively complete one frozen render state, allowing cancellation and UI responsiveness between bounded GPU submissions. Restart on invalidated state; do not mix frames. No ghosting-prone history reuse in the correctness path.

Support nested sample-count sweeps and convergence diagnostics. Determine production presets from measurements, not an assumed 16/32/96 budget. Bright small lights and moving thin hair must both converge; a global average error alone is insufficient. Reaching a sample/time cap is not automatically convergence.

## 7. Salvage and code organization

Reuse camera-control plumbing, UI framework, HDR target management patterns, shader registration, diagnostics, known scenes and existing OIT compositors. The current opaque blur is useful for comparisons, but it is not combined into aperture-rendered production output.

Add owned modules such as asdofcamera, asdofaperture and asdofaccumulation (final file split kept minimal). ASDepthOfField remains the selection/settings boundary. Isolate substantial orchestration outside upstream display/pipeline modules.

Expected integration boundaries: llviewerdisplay.cpp scene submission, pipeline.cpp/.h HDR finalization and sample boundaries, llviewercamera projection/state, asoitdispatcher and renderer lifecycle, shader registration and settings/UI. Existing asExactOIT capture/composite shaders should need no new DoF layer representation. File names must follow current asexactoit ownership, not stale fsexactoit references in historical notes.

All required upstream edits retain original code and ownership tags. Author chanayane@firestorm. No Git mutations, no shader-version bump, and no agent-run project builds. Keep the current experimental renderer intact until the new one passes acceptance; then remove its obsolete auxiliary captures/gathers from the new path and clean up only proven-unused resources. DoF-off must acquire no new resources or passes.

## 8. Independent reference and acceptance tests

Before viewer integration, build a small CPU thin-lens reference for analytically intersectable opaque planes, alpha-textured cards and ordered transparent stacks. It must calculate visibility independently of the GPU implementation and reproduce the viewer blend equations for nonstandard blends. Include a dense double-precision integration and analytic pinhole/focal-plane cases.

Freeze the following initial acceptance gates before implementation; do not relax them after observing a regression:

- Camera math: focal-plane reprojection error below 0.01 output pixel across lens positions, zoom/FOV, aspect ratio and resolution.
- Pinhole limit: matches the same compositor without DoF within established render-target precision; no face/color/coverage substitution.
- Synthetic normalized linear-color cases: RMS error at most 0.001 and 99th-percentile absolute error at most 0.01 against a converged independent reference. Measure alpha/coverage separately in synthetic tests.
- Isolated unoccluded strand/highlight tests: integrated contribution within 1% of reference, with adequate image guard band. Do not demand energy invariance where real occlusion changes it.
- At least N/2, N and 2N convergence comparisons; reference itself must converge before judging the candidate. Use local hairline, silhouette and highlight metrics, not only whole-image metrics.
- Slow zoom, focus rack, camera translation and animated strands: no systematic disappearing/reappearing hair, stale ghosts or discontinuities beyond measured finite-sample reference error.
- Scene suite: baby hair against scalp, rear hair, overlapping near/focused/far hair, glass before/behind hair, lamp panes/frame, foliage, bright small lights, custom blend/glow jewelry, water, screen edges, thin alpha masks, large apertures, close camera and equal-depth overlays. Compare opaque/transparent intersections explicitly.
- More than 4/8/16 overlapping surfaces, resource overflow, resize, shader reload, toggles, snapshots, camera cuts and interrupted captures. No arbitrary layer-count acceptance ceiling.
- All four transparency modes tested against their own pinhole and aperture reference. Exact OIT quality is not inferred from AVBOIT's appearance or vice versa.

Record lens settings, camera, animation time, compositor, GPU, resolution and sample count with each comparison. Save failure images and metrics. User reports remain evidence; synthetic success does not override a visible regression.

## 9. Implementation milestones and gates

1. Freeze baseline and specification. Record the existing 30 FPS off / 18 FPS experimental result, current failures and exact test settings. Inventory once-per-frame side effects and write the sample-state contract. Complete camera math/reference tests. No viewer rendering rewrite before this gate passes.
2. Implement isolated single-sample rendering. A zero-aperture sample must reproduce ordinary output and leave all state restored. Audit GL 4.1 baseline and newer OIT backends. No multiple-lens debugging until single-sample equivalence passes.
3. Implement complete aperture integration. Re-render visibility and transparency for each lens sample, accumulate HDR/glow, integrate post effects and lifecycle handling. Pass the synthetic and known-scene quality gates; quantify sample convergence. No release as a finished renderer before this gate passes.
4. Optimize that implementation. Measure CPU/GPU stage timing, memory high-water mark, sample count and frame-time distributions. Reuse invariant animation/skinning, safe lighting caches and conservative visibility work first. Every optimization must pass the same images/metrics. No layer collapse, silent lower resolution, missed lens views, unsafe tile skips or unvalidated temporal denoising.
5. Final acceptance and cleanup. Run the full quality, motion, compositor, state-failure and performance matrix. Publish measured speed/quality tradeoffs and select honest presets. Replace the experimental advanced path only after acceptance; update the single project plan with measured results and retained limitations.

Each implementation milestone batches changes into a reviewable unit before asking the user for a build. Do not request a build per small shader edit. The user performs all viewer builds and runtime checks.

## 10. Performance and resource contract

Quality-first is not a promise of 25 FPS. Repeated scene rendering can be substantially slower than today's 18 FPS before optimization, especially in alpha-heavy scenes. Measure one lens sample and estimate N-sample cost early; report it before a lengthy integration phase, without replacing the approved optical model with a shortcut.

Stream samples so accumulation memory is O(pixels), not O(samples × fragments). Explicitly budget extra HDR accumulation, glow, convergence storage, retained central-view data and any frozen-state resources at 1080p, 1440p and 4K. Reuse OIT storage with correct fences. No synchronous per-sample CPU readback, unbounded single GPU invocation, or repeated scene updates. Keep rendering cancellable and avoid driver watchdog stalls.

The selected requirement defers a fixed FPS floor; it does not waive reporting unusable latency. If the required quality remains too expensive for live use, report measured limits and retain the converged-capture capability. Do not call the complete product realtime until measurements support it. A later strict performance requirement may require an explicit scope decision; this plan cannot guarantee mutually incompatible quality/time limits.

## 11. Approval and source record

Primary optics reference: PBRT 4th edition, Projective Camera Models, thin-lens section: https://www.pbr-book.org/4ed/Cameras_and_Film/Projective_Camera_Models . It defines aperture sampling and focus-plane ray construction. The proposed raster multi-view orchestration is our application of that camera model to this repository, not a claim that PBRT supplies the viewer integration.

Repository evidence: current asdepthoffield.cpp/.h and asDepthOfField shaders; asexactoit.h and asExactOITCompositeF.glsl (per-node depth/color/blend/glow and shallow paths); asavboit.h; asoitdispatcher.h; llviewerdisplay.cpp; pipeline.cpp; existing DoF research notes and rendering backlog. Previous aggregate and selected-node failures are superseded as implementation directions, retained as historical evidence.

On approval, copy this exact raw plan file into doc/ayanestorm-depth-of-field-final-implementation-plan.md using cp; do not regenerate it. That becomes the single authoritative implementation plan. Historical research/backlog documents receive a short pointer instead of another competing roadmap. Internal validation gates are part of this plan, not requests to ship temporary half-solutions.
