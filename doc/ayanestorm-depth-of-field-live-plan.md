# AyaneStorm Live DoF (mode 3): layered, noise-free screen-space depth of field — Plan

Author: chanayane@firestorm
Date: 2026-10-01

## Context

Mode 1 (Advanced) is glitchy on Windows closeups, and on macOS it is "very noisy and
patterny", with unblurred halos around thin hair strands. Mode 2 (aperture) works and is
not touched. Mode 1 stays as it is, for side-by-side comparison. A new mode 3 "Live" is
added next to it; mode 1 is retired only after the user accepts mode 3.

### Evaluation of mode 1 (code-verified unless marked)

1. **Sparse point sampling.** 32 point taps (default quality) over a radius of 5% of image
   height: about 17 px tap spacing at 1080p and about 31 px on a Retina Mac (1964 px tall).
   The radius scales with resolution and the tap count does not. A per-pixel random phase
   turns this into fixed, screen-locked noise that crawls over moving content. The
   postfilter, tents, pyramids and reach bands all exist to clean this up.
2. **Hash on macOS (likely, not verified).** `fract(sin(dot(pixel, ...)) * 43758)`
   (`asDepthOfFieldFarF.glsl:133`, NearF, TransparentF) takes `sin` of arguments up to about
   1.9e5. Low-precision GPU `sin` (Apple) gives structured patterns instead of noise.
3. **Bilinear CoC** in every gather (`asdepthoffield.cpp:1222/1275/1342`). Silhouettes get
   intermediate radii that belong to no surface: halos and flicker, worst in closeups.
4. **One nearest depth per transparent stratum.** Alpha-blended mesh heads and hair share
   the rigged stratum, so face pixels under a strand take the strand's blur. About 200
   resolve lines of fixes (`opaqueBehind`, `riggedBehind`, `exclusiveBase`) search 64 px at
   most. Resolve line 541 divides by a transmission that can be as low as 0.05, which
   amplifies any mismatch up to 20x.
5. **Cost.** 3 extra replays of all alpha geometry, about 24 fullscreen passes, about 25 mip
   generations and 7 blits per frame. The near gather runs every tap on every pixel (no
   tiles). The resolve uses 16 samplers, the macOS limit.

### Intended outcome

A live DoF that:
- is noise-free and deterministic, and looks the same on Windows and macOS;
- costs the same at any resolution;
- decomposes transparency by real per-fragment depth, reusing Mac OIT's exact
  transmittance weights.

## Design

### Principles

- **Area taps, never sparse point taps.** Layers live at half resolution with mip chains.
  A tap reads the mip level that matches the tap spacing.
  - No random phase, no hash, no postfilter.
  - Rings are fixed and mapped to the aperture polygon (blades, roundness, rotation,
    anamorphic). Orientation is the mode-2 convention (`ASDoFAperture`).
- **CoC is never interpolated across depth edges.**
- **Everything is a premultiplied sum.** Box and mip filtering are then exact, and holes
  fill themselves when the result is normalized.

### Layer decomposition: 4 CoC bins

The bins are, front to back:
- **N2:** strong foreground;
- **N1:** foreground;
- **F:** focus;
- **B:** background.

All content, opaque and transparent, goes into them:
- **Transparent fragments:** each fragment computes its own signed blur radius r from
  `gl_FragCoord.z`. Its weight is split softly between adjacent bins:
  - F gets `1 − smoothstep(0.5, 2, |r|)` (the same 0.5–2 px ramp as today);
  - the rest goes to B, or to N1/N2 split at about 0.5 × the near maximum radius.
- **Opaque pixels:** their color × T goes into their own bins, where T is the transmittance
  of everything in front.

Per bin, the capture adds `S_b = Σ c·w` (rgb) and `W_b = Σ w`. An extra attachment holds:
- for N2 and N1: `Σ w/r²` (exact source energy);
- for B: `Σ w·r` (mean radius).

Visibility in front of bin b: `V_b = 1 − Σ_{k<b} W_k`. The layer "alone" is
`(S_b, W_b)/V_b`. A surface split across two bins composites back to full coverage, which
avoids mode 1's 0.75 loss.

- **Exactness identity:** `Σ_b S_b + T·opaque` equals the compositor's output.
  - Bins are rescaled per pixel by `(1−T)/ΣW` with `T = exp(−Σod)`, exactly as
    `asMacOITResolveF.glsl` normalizes.
  - So an all-in-focus pixel reproduces the transparency composite exactly.

### Transparency capture: reuse Mac OIT

- **Mode 4 (Mac OIT) active: zero extra geometry.**
  - Its COLOR pass already computes the exact per-fragment weight
    `w = α · front_transmittance` (`asMacOITCaptureF.glsl:217`).
  - `avboit_store()` also writes the binned `(c·w, w)` to locations 2..6 (5 RGBA16F),
    on a `colorBinsFBO` with 7 attachments (≤ 8 on Apple).
  - `macoit_store_glow()` writes zeros to those locations, because undefined outputs would
    be added into the bins.
  - The bins need no new samplers (CoC from depth and uniforms), which matters with
    macOS's 16 units.
- **Other modes (Standard, AYAstorm, AVBOIT, Exact OIT): a DoF-only Mac OIT capture.**
  - It runs KEYS, then K−1 PEEL passes, then COLOR, with K = `ASDepthOfFieldLiveExactLayers`
    (default 2, so 3 replays, the same as mode 1 today).
  - There is no resolve and no glow draw, and it does not touch `requested()`, the
    dispatcher's mode or the emissive shader out-params.
  - It is called from `ASOITDispatcher::renderPostDeferredCapture` (owned) before the
    selected mode's capture, so there are no `ll*` edits for it.
  - The probe and allocation follow the existing first-capture path.
- **Fallback** (Mac OIT unsupported, probe failed, allocation failed, or a snapshot whose
  3·H exceeds the max texture size): bins come from the composited `source` with the
  opaque CoC. That is a Firestorm-like single layer, still noise-free.

### Passes (`ASDoFLive::render`, all GL 4.1 fragment)

1. **Opaque bin** (full res, additive into the bin targets): opaque color × T into its bins.
   T is read from Mac OIT's weight/optical-depth target; T = 1 without transparency.
2. **Reduce** (to half res, 6 attachments).
   - 2×2 premultiplied sums of `S_b, W_b`, plus `V_b` and the radius channel.
   - The `(1−T)/ΣW` rescale happens here.
   - Then `glGenerateMipmap` on each attachment (the existing `generateMips()` pattern).
3. **Tiles** (16×16 full-res px): reduce the maximum foreground radius, then dilate by
   `ceil(near_max/16)`. Two tiny passes. In-focus tiles skip the near gathers.
4. **Gathers** (half res).
   - Ring count: `ceil(R_half/spacing)`, capped by `ASDepthOfFieldQuality` at 3/5/7 rings,
     i.e. 37/91/169 taps with 6k taps on ring k.
   - **B (far):** the kernel is the pixel's own mean radius, filled from a coarser mip when
     W is small.
     - Each tap reads `(S,W)/V` at lod = log2(spacing) and passes a scatter-as-gather test
       (its mean radius must reach the distance, soft over one spacing).
     - Holes behind foreground fill by normalization. This replaces push-pull,
       `opaqueBehind` and `riggedBehind`.
   - **N1, N2 (near):** the kernel is the tile's dilated maximum.
     - Coverage += `(W/V)·tapArea/(π r²)·[r ≥ d]`, with `1/r²` from the linear `Σw/r²`
       channel (energy exact), and the reach `r = sqrt(W / Σ(w/r²))`.
     - Color is normalized by the same weights. Output is premultiplied `(rgb, α ≤ 1)`.
5. **Composite** (full res into `hdrOutput`), front to back: N2 over N1 over F over B.
   - F is `(S_F, W_F)/V_F` at full resolution (sharp). Where `V_F < 0.25` it blends toward
     F's mip-filled estimate: the in-focus face under a strand comes from the real face
     behind, not the strand. This removes the halos.
   - Blurred layers are upsampled bilinearly.
   - `final = mix(source, layered, d)`, with d = 0 where the pixel's content is all in F and
     no veil covers it. In-focus content then stays exact in every alpha mode.
   - Glow passes through as `source.a`.
6. **Phase 3:** aperture-shaped highlight sprites (crisp polygon bokeh), ported from
   `asDepthOfFieldHighlightF`/`SpriteV`/`SpriteF`, added into the B and N layers before the
   composite. Until then, bokeh edges are softened by about one ring spacing.

### Cost

- **Geometry:** 0 extra replays with Mac OIT, 3 otherwise.
- **Fullscreen passes:** about 8, most at half res and tile-skipped, plus 6 mip chains.
  Mode 1 had about 24 passes and 25 mip chains.
- **Memory:** the bins add 40 B/px at full res (83 MB at 1080p, 237 MB at 3024×1964). The
  DoF-only capture also allocates Mac OIT's resources in other modes.
- **Profiling:** every pass gets an `LL_PROFILE_GPU_ZONE`.

## Phases

0. **Reference model** (`scripts/testing/dof_live_reference.py`), the gate before any shader
   work:
   - the decomposition identity and the bin-split coverage;
   - the far and near gathers against brute-force scatter: uniform field, solid foreground
     edge, thin lock over in-focus face, mixed radii, isolated light;
   - hole fill;
   - energy (mean error < 2%).
1. **Mode 3, opaque path plus the composite fallback:**
   - plumbing, reduce, tiles, gathers, composite, debug views;
   - works in snapshots first.
   - Runtime: Windows and Mac, against mode 1, mode 2 and Firestorm. This alone removes the
     noise, patterns and bilinear-CoC halos.
2. **Transparency bins via Mac OIT:** the mode-4 piggyback, then the DoF-only capture for
   the other modes. Hair tests.
3. **Highlight sprites.**
4. **Lens extras** (later, user decision): cat's eye and vignette, then axial CA,
   curvature, astigmatism, spherical aberration.

## Files

- **New:**
  - `indra/newview/asdoflive.{h,cpp}`;
  - shaders in `app_settings/shaders/class1/deferred/`: `asDoFLiveOpaqueBinF.glsl`,
    `asDoFLiveReduceF.glsl`, `asDoFLiveTileF.glsl`, `asDoFLiveGatherF.glsl` (a plane
    uniform selects far or near), `asDoFLiveCompositeF.glsl`;
  - `scripts/testing/dof_live_reference.py`.
- **Owned, modified:**
  - `asdepthoffield.{h,cpp}`: mode 3 dispatch in `render()`; `usesScreenSpaceRenderer()`
    (mode 1 or 3); shader registration through the existing `registerShaders()` /
    `createShaders()` / `unloadShaders()` hooks; the reset list.
  - `asmacoit.{h,cpp}`: bin textures and `colorBinsFBO`; `renderDoFCapture()`; the DoF CoC
    uniforms in `configurePass()`; getters.
  - `asMacOITCaptureF.glsl`: bin outputs and zero writes. No cache revision bump.
  - `asoitdispatcher.cpp`: the DoF-only call.
  - `asdofrenderer.cpp`: the UI mirror flag `ASDepthOfFieldUILive`.
  - `floater_as_depth_of_field.xml`: mode entry and Live controls.
  - `CMakeLists.txt`.
- **Tagged edits to non-owned files:**
  - `pipeline.cpp:9238`: `== 1` becomes `ASDepthOfField::usesScreenSpaceRenderer()`.
  - `settings.xml`, inside the AS block: the mode comment, plus `ASDepthOfFieldLiveDebug`,
    `ASDepthOfFieldLiveExactLayers` and `ASDepthOfFieldLiveTransparency`.
  - A combo item in `floater_phototools.xml`, `floater_advanced_phototools.xml` and
    `panel_preferences_graphics1.xml`.
- **Reused as-is:**
  - the CoC formula (`asDepthOfFieldCoCF.glsl` `calculateCoC`);
  - `ASDoFAperture`;
  - the autofocus and focus distance;
  - `hdrOutput()`;
  - the `generateMips()` pattern;
  - Mac OIT's KEYS/PEEL/merge/COLOR and its probe.

Mode 1 and mode 2 code are not changed.

## Debug views (`ASDepthOfFieldLiveDebug`)

1. Bin shares (N2 red, N1 orange, F green, B blue).
2. `ΣW + T`, which should be 1.
3. Tile classes.
4. Far layer.
5. Near layers.
6. Focus layer F.
7. d, the blend toward the layered result.
8. Bins source: Mac OIT piggyback, DoF-only or fallback.

Every debug output writes glow 0.

## Verification

- Every shader validated with `.glslang/bin/glslang.exe` on a scratch copy with
  `#version 410 core` prepended.
- `python scripts/testing/dof_live_reference.py`: all tests pass before shader work.
- **Runtime** (user builds), Windows and macOS, mode 3 against mode 1, mode 2 and Firestorm:
  1. A closeup portrait, thin rigged hair over an in-focus face, blurred background: no
     halos, strokes or noise.
  2. Retina: no pattern. A moving camera: nothing crawls.
  3. A hand near the camera over an in-focus subject: a clean veil, background filled
     behind it.
  4. Glass and windows in the background.
  5. Every alpha mode: Standard, AYAstorm, AVBOIT, Exact OIT, Mac OIT.
  6. An all-in-focus frame equals DoF off (debug 7 black).
  7. Snapshots at window size and 2×; an oversized snapshot falls back cleanly.
  8. GPU zones: mode 3 against mode 1 and DoF off, at each quality.
- Log: shader load, bins source and allocation lines, no GL errors
  (`%APPDATA%\AyaneStorm_x64\logs\AyaneStorm.log`).
- On approval, copy this file verbatim to
  `doc/ayanestorm-depth-of-field-live-plan.md`. Later findings go in that one file.

## Execution record

### Phase 0: reference model (2026-10-01, passed)

`python scripts/testing/dof_live_reference.py` (11 tests pass, ~30 s) and
`... survey [lod_scale]` (error tables). Ground truth: a brute-force scatter of
every source pixel over its own disc, energy-normalized. Results at the
adopted settings (gather pixels, 96x96 scenes, rms):

| scene | 3 rings | 5 rings | 7 rings |
|---|---|---|---|
| uniform layer (coverage) | 0.0000 | 0.0000 | 0.0000 |
| solid foreground edge (coverage / image) | 0.0014 / 0.0005 | 0.0007 / 0.0002 | 0.0006 / 0.0002 |
| 1 px strands, r 14 (coverage / image) | 0.0035 / 0.0016 | 0.0028 / 0.0012 | 0.0050 / 0.0022 |
| lock r 14 over face r 2.5, one bin (image) | 0.046 | 0.044 | 0.046 |
| same, geometric split (r 8 / 2.5) (image) | 0.029 | 0.035 | 0.018 |
| 4 / 20 px surfaces side by side (coverage) | 0.071 | 0.040 | 0.023 |
| isolated light, energy | 1.0000 / 1.0018 | same | same |
| far, in-focus hole filled (rgb) | 0.0028 | 0.0009 | 0.0012 |
| far, radius ramp 2..16 (rgb) | 0.012 | 0.010 | 0.010 |

For comparison, mode 1's point taps measured 0.03-0.05 coverage noise on the
same kind of scenes (`dof_near_gather_sim.py`). The new errors are smooth
biases, not noise.

Design refinements adopted from the model:
- **Exact reach share.** A tap counts the part of its annulus
  `[d - s/2, d + s/2]` inside the source radius,
  `clamp((r^2 - (d - s/2)^2) / (2 d s), 0, 1)` (centre tap: `r^2 / (s/2)^2`).
  The taps then integrate `pi r^2` exactly for any radius. A smoothstep over
  one spacing lost up to 9% coverage for radii between rings (Low quality:
  0.93 on a solid surface).
- **Tap footprint = one spacing** (`lod = log2(s)`). 0.5x aliases thin strands
  (0.079 coverage rms at Low, the "patterny" failure); 1.4x blurs mixed
  radii.
- **Geometric N1/N2 split** at `sqrt(2 * near_max)` instead of
  `0.5 * near_max`. Each near bin then spans a radius ratio of at most about
  `sqrt(near_max / 2)`, which keeps the mixed-radius bias near the
  `lockface_split` row.
- **Far kernel radius** from the finest mip level with `W >= 0.25`, so holes
  take the radius of the background around them.

### Phase 1: mode 3, opaque path and composite fallback (2026-10-01, unbuilt)

Code complete. The shaders link-check with glslang at `#version 410 core` and
`#version 400` (the macOS loader's version). Not built, not run.

**Files**
- New: `asdoflive.{h,cpp}` and five shaders:
  - `asDoFLiveCommonF.glsl`: the library. CoC, bin split and aperture taps.
  - `asDoFLiveReduceF.glsl`
  - `asDoFLiveTileF.glsl`
  - `asDoFLiveGatherF.glsl`
  - `asDoFLiveCompositeF.glsl`
- Owned, modified:
  - `asdepthoffield.{h,cpp}`: mode 3 dispatch, `usesScreenSpaceRenderer()`, registration and the reset list.
  - `asdofrenderer.cpp`: the UI flags `ASDepthOfFieldUIScreenSpace`, `UILive` and `UILens`. The aperture shape and maximum blur controls are now also enabled for mode 3.
  - `floater_as_depth_of_field.xml`
  - `CMakeLists.txt`
- Tagged blocks in non-owned files:
  - `pipeline.cpp`: one condition.
  - `settings.xml`: the mode comment, `ASDepthOfFieldLiveDebug` and the three UI flags.
  - The three other renderer combos.

**Deviations from the plan**
- **No separate opaque-bin pass in phase 1.**
  - The reduce pass bins the composited image directly by the depth buffer. That is the planned fallback: one surface per pixel.
  - The opaque-bin pass arrives with the Mac OIT bins in phase 2.
  - The phase-1 depth is the vanilla DoF depth (opaque plus non-rigged alpha above 0.33), so rigged alpha hair takes the depth behind it, as in Firestorm. Phase 2 fixes this.
- **Two reduce passes of three attachments.** LLRenderTarget allows four attachments at most, so the six bin channels go into two targets:
  - A: N2, N1, radius moments;
  - B: F, B, visibility.
- **Gather resolution is always half resolution.** `CameraDoFResScale` is ignored, because the mips carry the area sampling.
- **The far gather skips fully in-focus pixels.** F hides B there.
- **Taps past the frame edge read the edge texel.** A foreground touching the frame keeps its coverage.

**Runtime checks for the user build** (mode 3 against mode 1 and Firestorm)
1. Log: "Live DoF active at WxH", and no shader errors.
2. Debug 1 (blur bins): red or orange foreground, green focus, blue background. Debug 2 is white everywhere.
3. Large background blur on a still and on a moving camera: no grain, no screen-locked pattern (Windows and Retina Mac).
4. A hand near the camera over an in-focus subject: a smooth veil with the background filled behind it (debug 4 and 5).
5. An in-focus face with a defocused alpha-masked lock over it: no sharp strand halos (debug 6 shows the face filled under the strands).
6. All in focus: debug 7 is black and the image is identical with DoF off.
7. Snapshots at window size and at 2x.
8. GPU zones "Live DoF reduce/tiles/gathers/composite" against mode 1, at Low, High and Cinematic.

Phase 1 runtime (user, Windows): bokt. The blur bins are correct, the bin
weight sum is white, there is no grain or pattern, and the debug views behave
as designed. Snapshots work. Known limit, as expected: rigged alpha hair (the
ponytail) takes the depth behind it, so it blurred like the background.

### Phase 2: transparency bins from Mac OIT (2026-10-01, unbuilt)

Shaders link-check with glslang at `#version 410 core` and `#version 400`:
- the Live programs, with the updated library;
- the Mac OIT capture library, with and without `MACOIT_DOF`.

**Bin outputs in the Mac OIT capture library**
- The bins are written by `asMacOITCaptureF.glsl`, under the `MACOIT_DOF`
  define, at outputs 2..6.
- In the COLOR pass, every fragment adds `(c·w, w)` to the bins of its own
  depth, plus the energies. `w` is Mac OIT's exact weight, so the bins
  partition exactly what the pass accumulates.
- The bin math is a copy of `liveBlurRadius()` / `liveBinWeights()` (noted
  in both files). The glow terminal writes zeros.
- **Trap:** non-blend GLTF variants declare an unlocated `frag_data[4]`.
  - With explicit outputs at 2..6, it would need locations 7..10, beyond 8
    draw buffers. That would fail the link and with it all of Mac OIT.
  - Those clones therefore do not get `MACOIT_DOF`
    (`cloneCaptureProgram(..., dof_outputs)`).

**`ASMacOIT` (owned)**
- `setDoFLens()`, `dofBinsReady()`, `dofBinTexture()`, `dofWeightTexture()`
  (keysOdd: Σw, Σod).
- Five full-resolution RGBA16F bin textures and a 7-attachment
  `colorBinsFBO`. They are allocated only while bins are requested;
  `createFramebuffer()` now takes up to 8 attachments.
- `renderDoFCapture()` runs the same capture with `dof_only`:
  - K from `ASDepthOfFieldLiveExactLayers` (default 2, so 3 geometry
    passes);
  - no glow draws, no resolve;
  - `captureCompleted` and the frame-ready flag are not set;
  - a failed self-test does not reset `ASRenderOITMode`.
- The bin uniforms are set once per pass per program in `configurePass()`.

**Dispatcher (owned)**
- On the post-water pool it asks Live DoF for the lens and hands it to Mac
  OIT.
- In modes other than 4, it runs the DoF-only capture before the selected
  mode's own capture.

**Live**
- `prepareCapture()` keeps the opaque colour and depth before the alpha
  pool. It is called from `ASDepthOfField::prepareTransparentDepthCapture()`
  in mode 3, which still returns false, so the vanilla DoF depth pass is
  kept.
- `liveDecompose()` (library) splits each full-resolution pixel into bins:
  - the Mac OIT bins rescaled by `(1−T)/Σw`, as the Mac OIT resolve does;
  - plus the opaque surface × T, binned by its own depth.

  The reduce and composite passes share it. The composite's focus layer is
  that pixel's F share, plus the fill of what nearer bins hide.
- **Lens timing.** The capture runs before this frame's focus is known, so
  it uses the previous Live frame's lens, stored per image height
  (`tan_pixel_angle·H`, `max_coc/H`). Snapshots at another resolution
  therefore bin correctly. The cost is a one-frame lag in transparent bin
  assignment during focus pulls.
- **Fallback:** the Phase 1 one-layer path. It applies on the first frame,
  with the setting off, after a self-test failure, or on a size mismatch.

**Settings**
- `ASDepthOfFieldLiveTransparency` (on).
- `ASDepthOfFieldLiveExactLayers` (2).
- Floater checkbox "Live: blur transparency by its own depth".
- Debug 8 "Bins source": green = Mac OIT bins, grey = one layer.

**Memory and cost**
- Bins: +40 B/px, full resolution.
- Opaque copies: +8 B/px colour, plus depth.
- With modes other than 4, Mac OIT's own resources are also allocated, and
  the capture adds K+1 alpha geometry passes.
- In mode 4 the bins cost no extra geometry, only 5 more additive outputs in
  the COLOR pass.

**Runtime checks**
1. Log: "Live DoF transparency bins from Mac OIT" after the first frame, and
   no Mac OIT link errors. "Mac OIT shaders loaded" must still list every
   capture program.
2. Debug 8 is green (in mode 4, and in Standard / Exact OIT through the
   DoF-only capture).
3. The ponytail scene: in debug 1 the hair is green where it is in focus; in
   the image the hair stays sharp while the background blurs behind it.
4. Hair in front of the focus over an in-focus face: a blurred veil, with the
   face sharp underneath.
5. Debug 2 (bin weight sum) is white on transparent pixels too.
6. Mode 4 with everything in focus: identical to DoF off.
7. Mac OIT with Live DoF off: unchanged image and performance (no bins
   allocated, `MACOIT_MB` unchanged).
8. Snapshots, and Retina macOS.

### Phase 2 runtime and fixes (2026-10-01)

User run on Windows, Mac OIT mode with Live DoF, bokt. The log shows "transparency bins from Mac OIT" and the same 18 Mac OIT capture programs. Bins source is green everywhere and the bin weight sum is white, so the decomposition is complete. The focus layer is clean: sharp, continuous strands. Two defects were found and fixed (unbuilt):

1. **Sharp rock seen through in-focus hair strands.**
   - **Cause:** the composite blended the whole pixel toward the source, by the focus share: `mix(source, layered, 1 - W_F)`. The background seen through a semi-transparent in-focus strand therefore came back sharp, in proportion to the strand's opacity.
   - **Fix:** `final = layered + W_F (1 - a_N1)(1 - a_N2) (source - sum of bins)`. Only the unveiled focus share takes the source's difference from the bins.
     - With Mac OIT bins that difference is zero, because they add up to its composite.
     - In other transparency modes it is the compositor difference, limited to in-focus content.
     - In the fallback it is zero. This also removes a double-counted sharp share at focus transitions.
2. **Beaded strands and dark outlines.**
   - **Cause:** the far layer was written as normalized colour with coverage, and skipped (fully in-focus) pixels as (0, 0, 0, 0). The composite's bilinear upsampling mixed that black into the colour of neighbouring pixels. It showed through every partly in-focus strand.
   - **Fix:** every gather output is premultiplied, and the composite normalizes the far layer after the read.

Also changed:
- Debug 7 now shows `1 - exact_share` (white: layered result).
- The "Live DoF active" log line is written only when the size changes, instead of every 100 frames.

### Hidden layers: push-pull completed reads (2026-10-01, unbuilt)

**Defect (user, a foreground rock).** The defocused rock had a hard edge exactly where the far layer was black in debug 4.
- The far gather filled holes only within the background's own blur radius: a few pixels for the sea just behind the focus. Inside the rock-wide hole it found nothing, or did not run at all below 0.5 px.
- The composite then fell back to the source pixel: the sharp rock, under its own partly transparent veil.

**Model first.** `dof_live_reference.py` now carries visibility V per bin and a rock scene: a solid foreground blurred 12 px over a background blurred 1.5 px. It scores the fringe against the true layered result, away from the frame borders. The truth leaves off-frame sources out, while the renderer continues the frame's edge content.
- **The old path reproduces the defect.** Fringe rms: 0.057 on a smooth background, 0.047 with a fine checker.
- **A threshold rule ("finest level where V >= 0.5") was tried first and rejected.** Inside a hole as large as the visible part, no level qualifies, so the kernel stayed 0 (0.115 rms).
- **The `far_hole` scene had been mis-specified** (hole visible-and-empty, V = 1). It is now V = 0, as the renderer produces behind a focus pixel.

**Principle adopted.** Every layer read is completed by the push-pull recurrence `c(l) = S(l) + (1 - V(l)) c(l + 1)`, ending with `S / V` at the top level (`liveCompleted()` in `asDoFLiveCommonF.glsl`, `read_completed()` in the model).
- Visible content is read exactly, in one fetch. Hidden content is filled from coarser levels in proportion to what is missing, continuously, stopping at a hidden share below 0.01.
- One function serves every gather tap, the far kernel and the composite's focus fill (which replaces the earlier threshold-based `focusFill`).
- The far gather no longer returns nothing below 0.5 px: it reads such content nearly sharp.

**Results** (fringe rms, all ring counts):
- smooth background: 0.0046 (was 0.057);
- fine checker: 0.021 (was 0.047). The remainder is detail no method can recover behind the rock.
- `far_hole`: 0.003.
- 12 tests pass.

**Debug 3 (foreground tiles)** now normalizes by at least 0.5 px. With Foreground radius 0 it divided by ~0 and showed tiny radii as saturated red. The user's foreground screenshots so far were taken at Foreground radius 0, so the tile widening reach was 0.

### Runtime of push-pull, and the background split (2026-10-01, unbuilt)

**Runtime (user, Windows, Mac OIT).**
- The nose and face patches at Foreground radius 1.0 are gone. The focus layer is smooth.
- The foreground rock edge is soft over a filled background.
- **Defect 1:** the rock behind the hair strands still shows sharp.
- **Defect 2:** a grey patch on the ponytail, which is just behind the focus, in front of the rock.
- In debug 1, the strands are F/B (radius about 1-3 px) and the rock is B (tens of px). Debug 7 is white there, so the exact correction is not involved.

**Cause.** There was one background bin. The far gather blurs each pixel by its own mean radius M / W.
- At gather resolution, a strand and the gap beside it share texels. The rock seen in the gaps was blurred by the strands' small radius.
- Within one bin there is no front-to-back order: the rock's wide blur spread over the nearly sharp hair (defect 2).
- This is the mixed-radius case that N1/N2 already solves for the foreground.

**Model first.** `evaluate_far_strands()` puts 1 px strands (radius 1) over a checker background (radius 8) and scores against the true layered result.
- One bin: rms 0.027-0.036.
- B1 over B2: rms 0.0075 at every ring count.
- 13 tests pass.

**Change: the background is split like the foreground.**
- Bins: N2, N1, F, B1, B2.
- The B1/B2 boundary is `far_split = max(sqrt(2 * far_radius), 2.5)` with the same soft 0.8-1.25 ramp. Each background bin then spans the same radius ratio.
- **B1 (near background)** is gathered like a veil: tile kernel (tile `.z`), coverage from energy, upright aperture.
- **B2 (far background)** keeps the own-kernel, completed, normalized gather. It is filled behind B1 by push-pull.
- **Energies:**
  - (E_N2, E_N1, E_B1, M_B2).
  - B2's energy is `W^3 / M^2` (`liveFarEnergy()`): exact for one radius, and close within the bin's small ratio. It is only used for B2's normalized colour weighting.
- **Mac OIT capture:** one more output, `macoit_dof_back2`, at location 7. `colorBinsFBO` has 8 attachments, OpenGL 4.1's guaranteed maximum and macOS's. If the FBO is incomplete, the existing fallback (one layer) applies. 48 B/px.
- **Samplers:** the weight / optical-depth texture moved from `shadowMap5` to `positionMap`, and the energies to `shadowMap5`.
- **Reduce:** pass 0 writes N2, N1, energies; pass 1 writes F, B1, B2, visibility (V_N1, V_F, V_B1, V_B2). That is 4 attachments, LLRenderTarget's limit.
- **Gathers** run in the order N2; N1, written over N2 (`sNear1` = the whole foreground veil); B2; B1, written over B2 normalized (`sBack` = the whole background).
  - The composite reads these two, not four layers: it stays at 15 texture units (macOS: 16).
  - The background fallback chain is unchanged: background, then focus, then source.
- **Gather skip:** both background gathers skip pixels that F covers fully at gather resolution.
- **Tile dilation reach:** `max(near_radius, min(1.25 * far_split, far_radius))`.
- **Debug views:**
  - 1: B1 purple, B2 blue.
  - 3 ("Veil tiles"): B1 in blue.
  - 4: B1 over B2.
  - 5: the combined veil.

### Completion keeps split surfaces opaque (2026-10-01, unbuilt)

**Runtime of the split (user).** The rock is correctly blurred behind the hair strands.

**Defect.** A grey veil over the whole avatar. Its strength varies, and it is made of squares.

**Cause: the push-pull completion itself.**
- One surface on a blur ramp is split softly between two bins, for example F 0.6 and B1 0.4.
- B1 alone at that pixel is (0.4, V = 0.4). The hidden 0.6 is the same surface, so B1 alone should be opaque.
- Plain push-pull, `c(l) = S + (1 - V) c(l + 1)`, filled that 0.6 from coarser levels. Those levels average the empty space around the avatar, so coverage fell below 1 and the completed far background leaked through as grey-blue.
- The squares are the coarse mip texels under bilinear filtering.
- The focus fill under the foreground veils had the same flaw.
- Before push-pull, these reads were `S / V`. That is exact for split surfaces but empty in holes.

**Model first.** `evaluate_split_surface()` puts a 40x60 object at F 0.6 / B1 0.4 over empty space.
- Plain push-pull leaves an 18% coverage deficit inside the object.

**Principle adopted.** The hidden part takes the visible part's own density `S / V`, blended continuously toward the coarser estimate as less is visible:
`c(l) = S + (1 - V) (S + k c(l + 1)) / (V + k)`, with `k = 0.05` (`LIVE_COMPLETE_PRIOR`).
- Read front to back: `value += t S (1 + k) / (V + k)`, then `t *= k (1 - V) / (V + k)`.
- A hole (V = 0) is plain push-pull. A fully visible bin (V = 1) is one exact fetch.
- There is no threshold.

**Results (5 rings):**
- split surface deficit: 0.0044 (was 0.18);
- rock fringe, smooth background: 0.0005 (was 0.0046);
- rock fringe, fine checker: 0.024 (was 0.021). This is detail no method can recover behind the rock.
- `far_hole`: 0.004.
- far strands: 0.0074.
- 14 tests pass.

The hidden share also falls faster, so fewer mip levels are read.
