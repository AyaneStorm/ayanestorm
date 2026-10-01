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

### Tile kernels from the completed bins (2026-10-01, unbuilt)

**Runtime of the completion fix (user).** Better, but faint blocky patches remain on the hair, hairline and hand.
- "Veil tiles" shares their grid, but nothing in its values stands out.
- "Background layer" shows the cause. Behind the in-focus torso and hand, the background is either the near-background fill (B1, brown) or the far-background fill (B2, flat grey). The switch between the two follows a tile staircase.
- The tiles where B1 is grey match the black tiles inside the silhouette in "Veil tiles": kernel 0.

**Cause.** The tile pass reduced each bin's raw level 0, but the gathers read the completed bin.
- Where nearer bins hide all of B1 (behind opaque in-focus skin), a tile had a kernel of 0 and skipped B1. Its neighbour's gather ran and read B1's fill.
- The fill therefore stopped at tile edges.
- It shows wherever F is not fully opaque: hair edges, translucent strands, and surfaces on the F/B1 ramp. Hence faint, patchy, and on the 16 px grid.
- In debug 3 the difference is 0 against about 0.6 px: both look black.

**Model first.** In `evaluate_hidden_veil_tiles()`, a veil bin (radius 2) is visible on the left and hidden with V = 0 on the right.
- The model now mirrors the shader's skip of kernels under 0.5.
- Raw tile kernels: a coverage step of 1.0 at a tile edge inside the hidden part.
- Completed tile kernels: largest step 0.0015.
- Also measured and ruled out: the tap spacing's dependence on the kernel size below the ring cap. It changes near-sharp content by under 0.007.

**Change.** Tile pass 0 reads every veil bin with `liveCompleted()` at lod 0, exactly as the gathers do. The tile program links the common library and binds the bins trilinear plus the visibility (`lightMap`). The tile kernel then covers whatever the gather reads.
- 15 tests pass.

### Gathers skip only empty bins (2026-10-01, unbuilt)

**Runtime of the tile fix (user).** Blocks remain on the chest and arm: a lighter, washed staircase on skin just behind the focus. "Background layer" shows it; the skin there is B1.

**Cause.** The veil gathers skipped kernels under 0.5 gather px, but 0.5 is the smallest radius anything can have: `liveDecompose()` and the capture clamp r to it.
- Content just off the focus (under 1 full px, up to about 16% of the pixel in B1) has a tile kernel of exactly sqrt(W / E) = 0.5.
- 16-bit storage and texture filtering put it a hair above or below 0.5, tile by tile.
- In skipped tiles that share was replaced by B2's fill behind the body (sand, rock): blocks.
- The model ran in double precision, and numpy's half rounding keeps E = 4 W exact. It cannot show the hardware's rounding, so the invariant is tested instead.

**Change.** Every gather (N2, N1, B1, B2) skips only where its bin holds nothing (kernel 0), then reads with at least 0.5. This is the rule the B2 path already used. One code path now replaces the two.
- `test_minimum_radius_is_gathered`: a kernel a hair under 0.5 gathers the full coverage. 16 tests pass.
- Debug 3 ("Veil tiles") shows every gathered tile at least at a quarter brightness. A 0.5 px kernel is no longer black like a skipped tile, which hid both this defect and the previous one.

### Polygon aperture tap areas: reproduced tile staircase (2026-10-01, unbuilt)

**Runtime evidence.** Screenshots in `C:\Users\gabri\Documents\ShareX\Screenshots\2026-10`:
- `AyaneStormOS-Normal_O7Pc3aB6sD.png`: normal output, washed grey squares on the hair and jaw.
- `AyaneStormOS-Normal_NPVKFceH8v.png`: debug 9, B1 alone over magenta, carries the same staircase.
- `AyaneStormOS-Normal_mveopfjCg7.png`: debug 10, normalized B2 alone, has no corresponding staircase there.
- The user confirmed the current aperture is 6 blades, roundness 0. Temporarily setting roundness to 1 makes the squares disappear in debug 9, without rebuilding.

These observations isolate the visible defect to B1 before the B1-over-B2 combine and link it to the polygon aperture. The previous three changes did not resolve this defect; their earlier cause descriptions are not proof of its root cause.

**Root cause, reproduced in the reference model.** The gather estimated a ring tap's polygon area as `anamorphic * pi*s^2/3 * boundary(angle)^2`. That is a midpoint approximation to the angular integral, while the energy denominator `unit_area` is the exact aperture area from `ASDoFAperture::unitArea()`.
- With a sharp hexagon, all six first-ring angles lie at side midpoints: `boundary^2 = 3/4`. The estimated area is `pi*3/4`, versus the true `3*sqrt(3)/2`. Their ratio is `pi/(2*sqrt(3)) = 0.906900`.
- The centre tap already uses the exact area. At source radius = kernel = 0.5, the total coverage of an opaque uniform layer is `1/9 + 8/9 * pi/(2*sqrt(3)) = 0.917244`.
- Which annuli a source reaches depends on the tile kernel. Consequently this area error changes at tile boundaries, even when all tiles gather and the B1 colour is constant. B2 fills the missing coverage with its lighter colour.
- The earlier reference model used circular taps only. The circle has constant boundary, so the midpoint approximation is exact; that model could not expose this error. GPU precision, mip behaviour and Mac OIT are not needed to reproduce it.

**Model reproduction.** `evaluate_polygon_tiles()` puts constant dark B1 over a hidden light B2. All B1 sources have radius 0.5 except one radius-1.5 source, which changes the conservative tile maxima. The measured strip is outside that source's reach. It uses the shader's adaptive ring count, polygon offsets and per-tap mip footprints.
- Previous midpoint areas: minimum B1 coverage `0.917244`, maximum tile coverage step `0.082756`, maximum output colour error `0.047171`.
- Integrated sector areas: coverage `1.0`, tile step `0.0`, colour error `0.0` (double precision).

**Fix.** Each tap integrates `boundary^2/2` over its entire angular sector. Its area is `2*d*s * sector_area`, the radial squared-width times that sector integral. The centre keeps `unit_area*s^2/4`.
- `liveBladeAreaPrimitive()` uses the same analytic primitive as the existing CPU aperture area. `liveApertureAreaTo()` continues it across blade boundaries, including negative angles; `liveTapSectorArea()` subtracts the two angular endpoints.
- Every ring now partitions its own annulus exactly for all blade counts, roundness values and anamorphic ratios. This preserves coverage for every constant source radius up to the kernel, including partial coverage. Whole-kernel renormalization would not establish that identity for smaller sources that reach only inner rings.
- The shared area function applies to all four gathers. Tile maxima, binning, completion, reach tests and compositing keep their existing rules. No tile smoothing, opacity override or hexagon-specific correction is needed.

**Validation.** The reference model includes polygon/rounded aperture taps and three new regression tests: annular area conservation for blades 0 and 3–12, uniform coverage including partial alpha and smaller source radii, and the reproduced B1 tile staircase with old versus corrected areas. A float32 sweep of the shader's sector primitive across blades 3–12, roundness 0/0.35/0.8/1, squeeze 0.1/1/2 and kernels 0.5–20 measured a maximum uniform coverage error of `3.05e-7`.
- Khronos validator: Reduce, Tile, Gather and Composite each linked with the common library under both GLSL 410 core and 400 core: all eight pass.
- Reference model: all 19 tests pass. Edited files checked for LF line endings.
- Viewer build and runtime verification remain with the user; shader revision unchanged.

### Known residual, and Mac preparation (2026-10-01)

**Aperture fix (user, commit "new live dof fix 4").** The tile-aligned blocks came from the ring-tap areas.
- Taps used `boundary^2` sampled at the tap angle instead of each tap's sector integral.
- Error on the first ring: triangle -19%, hexagon -9.3% (worst), square +8.5% and octagon +1.3% (clamped coverage, no visible veil), odd blade counts under 1.1%, circle exact. This matches the user's ranking of shapes.
- The model had used a circular aperture only, so it could not reproduce the defect.
- `liveTapSectorArea()` now integrates each sector exactly.

**Known residual (minor, deferred).** A thin line along a sharp silhouette in front of B1 hair (neck against ponytail) shows sky. View 9 confirms it as a thin magenta line: B1's coverage dips there. The likely cause is the fill of B1's hidden part averaging in the gaps between strands. Not modelled yet.

**Mac preparation.** Mac OIT's COLOR pass with bins writes 8 colour attachments, 2 x 16 B + 6 x 8 B = 80 B per pixel. Apple's tile-based GPUs may cap the bytes a pass writes per pixel (unverified).
- Before, a rejected `colorBinsFBO` failed the whole Mac OIT allocation: Mac OIT itself would have been lost.
- Now a rejected bins framebuffer sets `sDoFBinsUnsupported` once per session and logs `Live DoF transparency bins unsupported by this driver`.
- Live DoF then stays one layer, and Mac OIT is unaffected. No reallocation every frame (`dofBinsWanted()`).

**Mac runtime (user, 2026-10-01).** A portrait closeup with thin hair over a blurred background looks flawless on macOS: no noise, no pattern, no strand halos. That was the original complaint about mode 1.

---

# AyaneStorm Live DoF (mode 3), phase 4: lens effects — Plan

Author: chanayane@firestorm
Date: 2026-10-01

## Context

Live DoF (mode 3) is accepted on Windows and macOS (noise-free, transparent hair per
depth, correct for every aperture shape). It ignores the lens-effect settings that modes 1
and 2 honour. The user wants them back, in this order, astigmatism skipped for now:

1. field curvature;
2. spherical aberration;
3. cat's eye (mechanical vignetting) and its corner darkening;
4. axial chromatic aberration.

Lessons that shape this plan:
- **Point-sampled areas.** The last defect came from point-sampling the aperture area per
  tap. Every effect that changes a tap's weight must integrate it exactly over the tap's
  region (or be normalized by the same discretization), so a uniform field keeps
  coverage 1.
- **Model the user's settings.** The reference model must include the user's actual
  settings: blade count, roundness, anamorphic ratio, quality.
- Each effect is its own step: model first, then shader, then the user's runtime check
  against mode 2 (the aperture-sampled reference, which models the real lens).

Settings are shared with modes 1 and 2, with the same meanings and scales; there are no new
settings. Mode 1 and mode 2 rendering stay unchanged.

## Shared groundwork

- **Lens field values.** Extract the settings math of `updateLensField()`
  (`asdepthoffield.cpp`) into a public helper on `ASDepthOfField`. It returns the existing
  `LensField` struct: field scale, cat eye, vignette, curvature, axial CA, spherical. Mode 1
  calls it with identical results. Live calls it with its own lens values.
- **Field position.** It is `(uv - 0.5) * field_scale`, length 1 at the frame corner, as in
  `asDepthOfFieldFarF.glsl` (`fieldPosition()`). It is added to `asDoFLiveCommonF.glsl`.
- **Live lens uniforms** (`setLensUniforms()` in `asdoflive.cpp`): `field_scale`,
  `field_curvature`, `sa_strength`, `cat_eye`, `vignette_shift`, `ca_shift`.
- **UI** (`asdofrenderer.cpp` `syncModeFlags()`): `lens_modes` includes Live for axial CA,
  cat's eye and spherical aberration. The field-curvature flag becomes mode 1 or 3.
  Astigmatism stays mode 1 only.
- **Reference model.** `dof_live_reference.py` gains a field position per pixel, and
  brute-force truths for each effect. Every gather test runs for circle, 5 and 6 blades,
  roundness 0 and 0.5, anamorphic 1 and 1.33, and all three ring counts.

## Step 1: field curvature

- A signed shift of every pixel's normalized CoC: `coc += field_curvature * field^2`
  before the clamp, as `normalizedCoC()` in `asDepthOfFieldCoCF.glsl` does.
- **Live library.** Applied in `liveBlurRadius()`, which takes the pixel position. The bins
  then re-sort by themselves; the gathers are unchanged.
- **Mac OIT capture.** Applied in `macoit_dof_radius()` (`asMacOITCaptureF.glsl`): one
  `vec4` uniform carries the field scale and curvature, and the fragment position comes
  from `gl_FragCoord` over the capture size.
  - `ASMacOIT::DoFLens` gains the fields.
  - `transparencyLens()` fills them per image height, like the other lens values.
- **Model gate.** The decomposition still sums to the source. `macoit_dof_radius` equals
  `liveBlurRadius` at the same pixel.

## Step 2: spherical aberration

- **Profile.** As in modes 1 and 2: light at normalized pupil radius ρ of a source's disc
  weighs `1 - a σ (2ρ² - 1)`, with `σ = clamp(signed_radius / 3, -1, 1)` in full-res px.
  It averages to 1 over the disc.
- **Exact per tap.** `liveReach()` becomes, when `sa_strength != 0`, the exact integral of
  the profile over the part of the tap's annulus `[d - s/2, d + s/2]` (centre tap: the
  disc of radius `s/2`) that lies inside the source radius r. This is a closed-form
  polynomial in ρ, divided by the annulus area.
  - A uniform field then still sums exactly to W (partition).
  - Polygonal and anamorphic apertures keep `liveTapSectorArea()`: the profile is radial in
    aperture space, as in mode 2.
- **Sign.** The source's sign comes from the layer: N layers in front, B layers behind.
  r is converted to full-res px through `gather_scale`.
- **Model gate:**
  - uniform-field coverage error under 0.1% for every shape and ring count;
  - the bokeh profile of an isolated light within 2% rms of a brute-force splat with the
    same weight.

## Step 3: cat's eye and corner darkening

- **Clip.** Toward the corners, the aperture is clipped by the lens barrel: a unit circle
  centred at `cat_eye * field` in unit aperture coordinates, capped at 1.6 radii
  (`barrelCenter()` in `asDepthOfFieldFarF.glsl`).
- **Open fraction per tap.** Each tap gets the open fraction of its own region (its angular
  sector times its annulus, in unit aperture coordinates). It is computed from a fixed
  sub-pattern of points inside the region. The point count is set by the model gate.
- **Without darkening.** Coverage and colour weights are divided by the clipped fraction of
  the aperture, computed over the same tap regions with the same sub-pattern. A uniform
  field then gives coverage 1 by construction. This is normalization by the same
  discretization, never an analytic fraction mixed with a sampled one, which would bring
  the tile-aligned veil back.
- **With darkening** (`ApertureCatEyeDarken`): no renormalization. The composite multiplies
  the final colour by the vesica fraction, as `vignette()` in
  `asDepthOfFieldResolveF.glsl` does for mode 1. In-focus content darkens toward the
  corners too, as with a real lens.
- **Field position.** It is taken at the gathering pixel; the barrel varies slowly across
  the frame.
- **Model gate:**
  - uniform-field coverage within 0.2% at any field position, shape, ring count and kernel
    size (no tile dependence);
  - bokeh shape against a brute-force clipped splat.

## Step 4: axial chromatic aberration

- **Strata.** Mode 1's spectral model (`channelCover()`, `CA_STRATA` and the channel
  weights in `asDepthOfFieldFarF.glsl`): four wavelength strata, whose radii are
  `r - σ δ s_k` (δ = `ca_shift` times the maximum radius, σ = ±1 by layer).
- **Per tap.** The exact reach (with Step 2's profile when it is on) is evaluated per
  stratum. Energy is rescaled per stratum, `E (r / r_k)²`, so each stratum's disc keeps
  its energy. Each channel takes its weighted sum of strata.
  - Colour sums are per channel. Each channel's coverage partitions exactly (its strata
    weights sum to 1).
- **Kernels.** Tile kernels and the B2 kernel grow by δ, so the widest stratum is covered.
- **Layering.** The layers keep one alpha (green coverage) for over-compositing, with
  premultiplied per-channel colour.
  - The error is `(a_green - a_channel) * behind`, only at blur edges.
  - Per-channel alphas would need two more composite samplers (17 of macOS's 16).
  - The model reports this error. It must stay under 2% rms on a hard-edge test at the
    strongest setting. Otherwise the step stops and comes back to the user.
- **In-focus content (F)** is not split by wavelength (the shift there is under a pixel).
- **Model gate:**
  - per-channel coverage of a uniform field exact (under 0.1%);
  - fringe colour against a per-channel brute-force splat.

## Files

- **Owned:**
  - `asdoflive.cpp`;
  - `asdepthoffield.{h,cpp}`: lens-field helper extracted, mode 1 behaviour identical;
  - `asdofrenderer.cpp`: UI flags;
  - `asmacoit.{h,cpp}`: DoFLens field values, one uniform;
  - shaders `asDoFLiveCommonF.glsl`, `asDoFLiveGatherF.glsl`, `asDoFLiveTileF.glsl`,
    `asDoFLiveCompositeF.glsl`, `asMacOITCaptureF.glsl`;
  - `scripts/testing/dof_live_reference.py`;
  - `doc/ayanestorm-depth-of-field-live-plan.md`.
- **Non-owned:** none expected. The floater controls exist already and only their
  enable flags change, in `asdofrenderer.cpp`.
- **Reused:**
  - `updateLensField()` math;
  - `fieldPosition()`, `barrelCenter()`, `sphericalWeight()` profile, `channelCover()`
    strata and weights;
  - `vignette()` vesica fraction;
  - `liveTapSectorArea()`.

## Verification

- **Per step:** `python scripts/testing/dof_live_reference.py` passes, with the new gates,
  before any shader edit. glslang validates and links every changed shader at
  `#version 410 core` and `#version 400`. `git diff --check` is clean, and every file is LF.
- **Runtime (user builds), Windows then macOS, after each step:**
  1. Effect off: identical to the current build (debug views and images).
  2. Effect on, Live against mode 2, same scene: comparable look (curvature shifting the
     focus toward the corners; SA rim or centre bokeh; cat's-eye corner bokeh and
     optional darkening; colour fringes on blur edges).
  3. Debug view 9 "Near background alone" stays free of magenta blocks for every aperture
     shape (the regression guard for coverage).
  4. Snapshots at window size and 2×.
  5. FPS at Low against the current 45 FPS reference scene, with the effect on and off.
- Findings go in the plan doc's execution record. Astigmatism stays deferred.

## Phase 4 execution record

### Groundwork and step 1: field curvature (2026-10-01, unbuilt)

- **`ASDepthOfField::lensField()`** (`asdepthoffield.{h,cpp}`) returns the shared `LensField`.
  - Mode 1's former `updateLensField()` body is now the pure `computeLensField()` in the same anonymous namespace. Mode 1 assigns its result to `sLensField`, so its behaviour is identical.
  - The struct moved to the header.
- **Live** (`asdoflive.cpp`) fills `Lens::mField` from it every frame and uploads `field_scale` and `field_curvature`.
- **Live library** (`asDoFLiveCommonF.glsl`): `liveBlurRadius(device_depth, uv)` adds `field_curvature * field^2` to the normalized CoC before the clamp, as mode 1's `normalizedCoC()` does. `liveDecompose()` passes the pixel centre's uv.
- **Mac OIT capture**:
  - `transparencyLens(width, height, lens)` calls the same `lensField()` with the capture's size and lens values.
  - `DoFLens` carries the field scale and curvature.
  - `configurePass()` uploads `macoitDofField = (scale / size, scale / 2)` and `macoitDofCurvature`.
  - `macoit_dof_radius()` uses `gl_FragCoord.xy * xy - zw`.
- **UI.** The field-curvature checkbox is enabled for modes 1 and 3 (`ASDepthOfFieldUIScreenSpace`), and so is its slider (`syncModeFlags()`). Astigmatism stays mode 1 only.
- **Model gate.** `test_field_curvature_capture_matches_library`: the capture's and the library's field positions and normalized CoC agree at every pixel centre to 1e-12. The gathers already handle arbitrary per-pixel radii (the ramp scenes). 20 tests pass.
- glslang at 410 core and 400, `git diff --check` clean, LF throughout.
- **Build fix.** The first build failed: `LensField` was declared twice in `asdepthoffield.h` (the redone refactor added it again after the first attempt was reverted in the `.cpp` only). The duplicate is removed.

### Step 2: spherical aberration (2026-10-01, unbuilt)

- **Profile.** Light at pupil radius rho of a source's disc weighs `1 - sa (2 rho^2 - 1)`, with `sa = a sigma` and `sigma = min(r_fullres / 3, 1)`, negative for the N layers (`liveSphericalProduct()`), as in modes 1 and 2.
- **Exact per tap.** `liveReach(r, d, s, sa)` integrates the profile over the part of the tap's annulus inside r. With `u = t^2`, `G(u) = (1 + sa) u - sa u^2 / r^2`, and `G(r^2) = r^2`, so a whole disc keeps its energy for any strength, aperture shape, ring count and kernel. The difference is factored, `(hi - lo)((1 + sa) - sa (hi + lo) / r^2)`, for 32-bit precision. With `sa == 0` the previous code runs unchanged.
- **Wiring.** `sa_strength` is uploaded with the lens uniforms (`LensField::mSpherical`). The gathers pass the layer's side: N2 and N1 in front, B1 and B2 behind. The tiles, kernels and composite are unchanged.
- **UI.** The lens checkboxes (`ASDepthOfFieldUILens`) are enabled in Live, and the spherical strength slider with them. The axial CA and cat's-eye checkboxes are enabled ahead of steps 3 and 4 but do nothing in Live yet; their sliders stay disabled there.
- **Model gate.**
  - `test_spherical_partitions_any_kernel`: uniform coverage exact to 1e-11 for the circle, 5 and 6 blades, roundness 0 and 0.5, anamorphic 1 and 1.33, every ring count, kernel 0.5 to 20, source radius and strength.
  - `test_spherical_bokeh_profile`: an isolated light's energy is exact. Its azimuthal profile is within 1.5x the same gather's error without spherical aberration, and the direction is right (a > 0: bright rim in front, bright centre behind).
  - **Gate change.** The planned "2% rms against brute force" is out of reach even without spherical aberration. An isolated single-pixel light's rim is softened by the tap spacing: 9.2 / 5.1 / 3.8% at 3 / 5 / 7 rings. With spherical aberration the error is 13 / 6.0 / 2.0% (bright rim) and 6.5 / 2.3 / 3.4% (bright centre).
  - 22 tests pass.
- glslang links Reduce, Tile, Gather and Composite with the library at 410 core and 400. `git diff --check` is clean, LF throughout.
- **Runtime (2026-10-01).** The user confirmed step 2 works.

### Step 3: cat's eye and corner darkening (2026-10-01, unbuilt)

- **Clip.** A pupil point p of a source's unit aperture (rotation, polygon and squeeze included) passes when `|p - barrel| <= 1`. The barrel is `cat_eye * field` at the gathering pixel, capped at 1.6, as in modes 1 and 2. For a tap reading a source of radius r, `p = tap offset / r`. The clip follows each source's own disc, not the kernel, so small sources in a large tile kernel are clipped correctly.
- **Why not the planned sub-pattern.** The model measured it first.
  - Sampling the clip at a few angles per tap, normalized by a per-pixel open fraction, left uniform-coverage errors of 2 to 4% at kernels of 6 and more, and up to 35% at tiny kernels. Clipping each tap's sector to the barrel's cone of directions analytically did not fix it.
  - The error changed with the tile's kernel and the ring count, which is the tile-step mechanism of the grey blocks.
- **Barrel band (`asDoFLiveGatherF.glsl`, model `BarrelBand`).**
  - In the area-uniform angle `A = liveApertureAreaTo(theta)` and `u = tau^2`, every aperture shape is the rectangle `[0, unit_area] x [0, 1]` and every tap an exact sub-rectangle (the existing sector areas).
  - The open part is a band `u1(A) <= u <= u2(A)` along each ray (the exact ray and circle intersection). It is sampled at 24 node angles per pixel (`setupBarrel()`) and is linear in A between them.
  - Every tap (`barrelReach()`) and the aperture's open fraction integrate that same band exactly, spherical profile included (`rampMean()`: the moments of `u + sa (u - u^2)` along a clamped linear ramp). A uniform field therefore keeps coverage 1 by construction for any kernel, ring count, source radius, shape and spherical aberration. The only approximation left is the barrel's outline.
- **Compensation and darkening.** The gathers always renormalize a source to its open aperture (open fraction `f0 + sa f1`, floor 0.01). With darkening (`ApertureCatEyeDarken`), the composite multiplies the final colour by the vesica fraction, as mode 1's resolve does (`vignette()`, 5% floor), so in-focus content darkens too. With cat's eye off, the previous code runs unchanged: the tap area is the same expression as `liveTapSectorArea()`.
- **Wiring.** `cat_eye` and `vignette_shift` are uploaded with the lens uniforms (`LensField::mCatEye`, `mVignette`). The cat's-eye strength slider and darken checkbox are enabled in Live. Only axial CA's checkbox is still ahead of its effect.
- **Model gate.**
  - `test_cat_eye_uniform_coverage_is_exact`: coverage 1 to 1e-9 for 5 shapes, 5 barrel shifts up to the cap, kernels 0.5 to 20, every ring count, radii 0.3 to 1 times the kernel, and spherical aberration -1, 0 and 1.
  - `test_cat_eye_open_fraction`: the band's open fraction is within 3% of the exact vesica (circle) at every shift: 0.1 / 0.8 / 1.4 / 2.5% at 0.3 / 0.8 / 1.2 / 1.6. At shift 0 it is exactly 1.
  - `test_cat_eye_bokeh`: against a brute-force clipped splat, an isolated light's energy is exact. Its error is within 1.05x the unclipped gather's error (it is lower in every case measured), and its centroid moves toward the truth's at least 60% of the way (the gate). Measured: 94 to 98% of the truth's shift, net of the unclipped gather's own offset.
  - 25 tests pass. 16 and 32 nodes gave nearly the same bokeh error as 24.
- **Cost.** It applies only with cat's eye on: 24 node rays per gather pixel and layer, then one to five band segments per ring tap and 25 for the centre tap. This is ALU only, with no extra texture reads.
- glslang links Reduce, Tile, Gather and Composite with the library at 410 core and 400. `git diff --check` is clean, LF throughout.
- **Runtime (2026-10-01).** The user confirmed cat's eye works.

### Fix: B1 self-occlusion, the jaw line (2026-10-01, unbuilt)

- **Defect.** With max blur above about 2.4%, a light line ran along the jaw, with or without cat's eye. Debug view 9 showed it as thin magenta, like the earlier silhouette line in the hair.
- **User settings, modelled first.** Max blur 3% of a 1398 px high window, physical blur, multipliers 1, Cinematic (7 rings), hexagon with roundness 0, f/15, cat's eye and spherical aberration off.
- **Cause (model `evaluate_self_occlusion()`).**
  - Behind the focus, sharper content is nearer. The jaw edge, blurred about 1.2 px, is split between F (0.55) and B1 (0.45) on the focus ramp. Above about 2.4%, `far_split_radius` lets the neck below, blurred about 6.5 px, into B1 as well.
  - Taps from the neck toward the jaw read the jaw edge, which is too sharp to reach. The neck hidden behind the jaw is stored nowhere: completion runs only for nearer bins. B1 coverage fell to 0.79 for about 6 px below the jaw, and the far background showed through.
  - The deficit is 11 to 20% at any max blur whenever B1 holds content blurrier than a sharp edge next to it. At 1% the user's neck was in B2, which is normalized.
- **Fix (`asDoFLiveGatherF.glsl`, B1 only).**
  - The pixel's own completed B1 gives a density `W_p`, radius `r_p` and colour.
  - A tap whose B1 is sharper (`r < r_p`) adds a hidden share: `area / unit_area * W_tap * W_p / r_p^2 * max(reach(r_p) - reach(r), 0)`. That is what content like the pixel's own would add over the part of the tap its own content does not reach, with the same reach function (spherical aberration and barrel included).
  - The coverage deficit is filled with the pixel's colour, up to that sum.
  - Equal radii give exactly zero: the reach difference is continuous. The first prototype tested "sharper" alone, and float rounding then filled the neck's real see-through at the background edge. Smooth blur ramps already have coverage 1, and taps without B1 (a true edge over B2) add nothing.
- **Model.** `gather(..., self_fill=True)` for every B1 gather. `test_self_occlusion_fills_behind_sharper_b1` checks three things:
  - On the jaw, the colour error drops from 0.106 to under 0.01.
  - The neck and background edge see-through is unchanged to 1e-9.
  - A receding textured surface keeps coverage 1 and its colours within 0.01.
  - The strands-over-rock, hidden-veil-tile, polygon-tile and minimum-radius gates pass with the fill on. 26 tests pass.
- **Cost.** One completed read per B1 gather pixel. The extra reach runs only for taps sharper than the pixel's own B1. With cat's eye on, that is a second barrel integration for those taps.
- glslang links Gather and Composite with the library at 410 core and 400. `git diff --check` is clean, LF throughout.
- **Runtime (2026-10-01).** The user confirmed the jaw line is gone ("looks perfect so far") and committed steps 1 to 3 with this fix.

### Step 4: axial chromatic aberration (2026-10-01, unbuilt)

- **Gate result: single alpha rejected (the plan's stop point).**
  - The model measured compositing the layers with one (green) alpha against per channel, on a hard blurred edge (`evaluate_ca_edge()`).
  - On bright over dark the error is under 0.2%. On dark over bright it is as large as the fringe itself: in the fringe band, rms 1.85% against a 1.75% fringe at delta = 0.4 R (about the strongest setting at 2 m and 50 mm), and 3.4% against 3.2% at delta = R.
  - A dark foreground has no colour to fringe. Its fringe comes from the per-channel coverage of what lies behind, so one alpha loses it.
  - The user chose the per-channel veil.
- **Strata (`addTap()`).** As in modes 1 and 2:
  - Four strata of wavelength s, with radius `|r - sigma delta s|` (floored at half a gather pixel), sigma +1 behind and -1 in front.
  - `delta = ca_shift * (near or far radius)` in gather pixels.
  - Channel weights red 1 + s, green 1.5 (1 - s^2), blue 1 - s.
  - Each stratum has its own exact reach (spherical aberration and the barrel included) and keeps the source's energy (`W / r_k^2`), so each channel's weights partition exactly.
  - B1 self-occlusion runs per stratum.
- **Kernels.** The tile pass adds the widest stratum (`ca_reach`, 0.75 delta per side) to each non-empty tile radius before dilation, and C++ widens `tile_reach` likewise. B2's own kernel grows in the gather.
- **Per-channel compositing.**
  - N2 and N1 write their per-channel coverage to a second attachment (`frag_alpha`, `sNear2` and `sNear1`). N1 is written over N2 per channel.
  - B1 is written over normalized B2 per channel inside the B1 pass, which needs no storage.
  - B2 stays normalized: colour per channel, one alpha.
  - The composite lays the veil per channel (`normalMap`). `exact_share` keeps the green alpha.
- **Texture units.** Debug view 3 (tile kernels) is now drawn by the B1 gather into the background target. The composite dropped `noiseMap` and has 15 active samplers with the veil alpha, under macOS's 16 (glslang reflection).
- **CA off.** Every changed operation keeps its previous code in a uniform branch (`ca_on`, `ca_shift > 0`), and the tile radius adds 0. The only visible difference is debug view 3, now upsampled from the gather target.
- **UI.** The axial CA strength slider is enabled in Live (`ASDepthOfFieldUIAxialCA`). Every lens effect except astigmatism now works in Live.
- **Model.**
  - The gather became a stratum loop: one neutral stratum when off, and the existing 26 tests pass unchanged.
  - `test_axial_ca_uniform_coverage_per_channel`: coverage 1 to 1e-9 in every channel for 3 shapes, with and without the barrel, both sides, delta 0.3 to 6, radii 0.5 to 8, and spherical aberration 0 and 1.
  - `test_axial_ca_edge`: a hard edge composited per channel is within 1% rms of a per-channel brute-force splat for every quality, side, shift up to delta = R and colour pair. It also documents the single-alpha loss.
  - 28 tests pass.
- **Cost (CA on).** Four reach evaluations per tap (four barrel integrations with cat's eye), one more RGBA16F half-resolution attachment written by each foreground pass, and one more composite fetch. CA off adds only the second attachments' writes in the two foreground passes.
- glslang links Reduce, Tile, Gather and Composite with the library at 410 core and 400. `git diff --check` is clean, LF throughout.

### Wider lens-effect limits: axial CA 10%, spherical aberration ±5 (2026-10-01, unbuilt)

- **Why.** At the user's f/15, axial CA at the 2% limit shifts the colours by about 2 px, and spherical aberration at ±1 was too subtle. Both effects scale with the aperture like the blur itself. The user asked for 10% and ±5, in all three modes. Within the old ranges, every mode is unchanged.
- **Limits.** The slider ranges, the clamps in `computeLensField()` (modes 1 and 3) and in the aperture-sampled key (`asdofrenderer.cpp`, mode 2), the `LensField` comment, and the `settings.xml` comments, which now name Live. Each tooltip explains the stylised range.
- **Spherical profile beyond 1.** `1 - c (2 u - 1)` (c = a sigma, u = rho^2) turns negative near the rim (c > 1) or the centre (c < -1). It is cut at zero and divided by its mean, which is `(1 + c)^2 / (4 c)` for c > 1 and `1 + (1 + c)^2 / (4 |c|)` for c < -1. The result is a hard bright core or a thin ring, and the source keeps its energy.
  - Mode 1: `sphericalWeight()` in Far, Near, Transparent and Sprite divides by `sphericalNorm()`.
  - Mode 2: `asDoFAccumulateF.glsl` clamps the weight at zero and divides likewise. Before, a weight could turn negative and subtract colour.
  - Live: `liveReach()` cuts the integration interval at `u0 = (1 + c) / (2 c)` and divides by `liveSphericalNorm()`. `barrelReach()` cuts the band's u range the same way, and its open fraction is the cut profile's (one whole-aperture band integral, kept for the last strength).
  - For |c| <= 1 the norm is exactly 1 and the cut is outside [0, 1], so every mode keeps its previous results: mode 2's pupil radius is in [0, 1), where the weight is already >= 0.
- **Model.** `spherical_norm()` and `spherical_cut()` feed `spherical_reach()` and `BarrelBand.reach()`. The truth splat clamps at zero before renormalizing.
  - Uniform coverage is exact to 1e-11 for strengths down to -5 and up to 5, and with the barrel at 3 and -5.
  - A light's bokeh at 3 and -5 keeps its energy and stays within 2x the unclipped gather's error. Measured: 1.74x at 3 rings for the thin ring, under 1x at 7 rings.
  - 28 tests pass.
- glslang compiles the Live set (linked), and mode 1's Far, Near, Transparent and Sprite and mode 2's Accumulate (alone), at 410 core and 400. `git diff --check` is clean, LF throughout.

# AyaneStorm Live DoF (mode 3), phase 5: speed and cleanup — Plan

Author: chanayane@firestorm
Date: 2026-10-01

## Context

Phase 4 (lens effects) is committed. The user wants Live faster with plain settings (no lens
effects), and Live's own code cleaned up. Modes 1 and 2 are not touched. Astigmatism stays
deferred; macOS is checked after.

Where the plain gather spends its time (`asDoFLiveGatherF.glsl`, `asDoFLiveCommonF.glsl`):
- **Completion per tap.** Every tap of N1, B1 and B2 runs `liveCompleted()`, the push-pull
  recurrence, itself: per mip level, three trilinear reads (visibility, sums, energies).
  - It stops once the hidden share is under 1%. Where the bin shows, that is one level, so
    3 reads.
  - Behind the avatar (B1, B2), it is up to about 10 levels, so up to about 30 reads per tap.
  - The tile pass reads every gather pixel this way too, three bins each, and the composite
    once per hidden focus pixel.
- **Tap geometry per tap.** `liveTapOffset()` and two `liveApertureAreaTo()` calls cost
  cos, sin and the boundary, plus for polygons tan, log and cos twice. This is identical
  for every pixel.
- **Reduce twice.** Two passes each decompose all four full-resolution pixels. Each
  decomposition reads up to 9 textures plus the depth unprojection. The only reason is
  LLRenderTarget's 4-attachment limit.

**Feasibility, already checked in the model** (scratch prototype): building the completion
once per mip level, then reading it with one trilinear fetch, passes all 28 tests. The
survey errors against brute force are equal or better:
- far hole: 0.0041 → 0.0030 rms;
- checker rock: 0.024 → 0.020;
- split surface: 0.0044 → 0.0013;
- hidden-veil step: 0.0011 → 0.

The one regression is the smooth rock fringe, 0.0005 → 0.0028, still 20× under the old
defect's 0.057.

Each step below is its own build, check and commit, in this order.

## Step 1: completion pyramid

- **Recurrence, once per texel** of each level, top-down:
  - `c(top) = S / V`;
  - `c(l) = S·(1+k)/(V+k) + k(1−V)/(V+k) · c(l+1)`, with `c(l+1)` read bilinearly at the
    texel centre;
  - k = 0.05, as now.
  - This is exactly `liveCompleted()` unrolled, without its 1% early stop.
- **Targets.** Two new mipmapped RGBA16F targets at gather resolution (`TMG_MANUAL`, as
  `allocateMipmapped()`):
  - `sDoneA`: completed N1, F, B1, B2 sums `(S.rgb, W)`;
  - `sDoneB`: completed energies `(E_N1, E_B1, M_B2, 0)`, each completed with its own
    bin's visibility.
  - N2 needs none: it is the front bin (V = 1), so its read is the raw mip, as today.
- **Passes.** A new small program, `asDoFLiveCompleteF.glsl` (linked with the library),
  with 5 outputs. It renders level by level from the top:
  - one raw FBO, with the five textures' level l attached by `glFramebufferTexture2D`,
    and the viewport set to the level size;
  - the precedent is `checkAttachment()` in `asambientocclusion.cpp`;
  - during level l's draw, the `sDone` textures' `GL_TEXTURE_BASE_LEVEL` is l+1, so
    reading `c(l+1)` while writing level l is no feedback loop under GL 4.1. It is reset
    to 0 after.
  - The raw sums and visibility come from `sBinsA`/`sBinsB` by `texelFetch` at level l.
  - About 10 levels at 1080p, so about 10 tiny draws.
- **Readers.**
  - Gathers: one `textureLod` of the completed sums and one of the energies per tap, with
    no loop and no visibility read.
  - `farKernel()`, the B1 self read, tile pass 0 and the composite's `focusFill()` read the
    same textures at lod 0.
  - The composite drops `emissiveRect` (14 samplers).
  - `liveCompleted()` and its constants are removed from the library.
- **Memory.** 5 more half-resolution RGBA16F mip chains: about +28 MB at 1080p and +79 MB
  at 3024×1964.
- **Model.** `read_completed()` becomes the pyramid: `build_completed(mips)`, then
  `sample_trilinear`. Gates:
  - all tests pass;
  - the survey matches the prototype numbers above (recorded in the doc).

## Step 2: one reduce pass

- One raw FBO over the 7 textures of `sBinsA` (3) and `sBinsB` (4), with 7 draw buffers.
  OpenGL 4.1 guarantees 8, and Mac OIT's `colorBinsFBO` already uses 8 on Apple.
- `asDoFLiveReduceF.glsl` writes all seven outputs. `reduce_pass` and the second draw are
  removed, which halves the full-resolution decomposition.
- If the FBO is incomplete, Live logs once and returns false, the same as a failed target
  allocation.
- **Model:** unchanged (same sums).

## Step 3: tap table

- **Per frame on the CPU**, for the 169 taps of 7 rings, in the gather's order (the table
  is the same for every ring count): `vec4(unit offset x, y, boundary · squeeze, sector
  area span)`.
  - Rotation, anamorphic ratio and polygon boundary are included.
  - The span is `liveApertureAreaTo(θ+h) − liveApertureAreaTo(θ−h)`, computed with
    `ASDoFAperture`'s blade primitive.
- **Upload.** `uniform4fv(U_TAPS, 169, ...)` to the gather program: 676 components, within
  GL 4.1's 1024 fragment minimum together with the existing uniforms.
- **Per tap in the gather:**
  - `offset = taps[i].xy · d`;
  - `spacing = s · taps[i].z`;
  - `area = 2 d s · taps[i].w`.
  - The barrel path (cat's eye on) still computes its sector bounds itself, so it is
    unchanged.
- **Model gate:** a Python mirror of the C++ table equals `aperture_taps()` within 1e-6 for
  circle, 5 and 6 blades, roundness 0 and 0.5, anamorphic 1 and 1.33, rotation 0 and 15°.

## Step 4: cleanup (Live only)

- **Remove:** the unused `liveTapSectorArea()`, with comments pointing at
  `liveApertureAreaTo()`; whatever steps 1 to 3 leave unused (uniforms, samplers,
  `bindRaw` paths).
- **Fix stale comments:**
  - `sa_strength` "-1..1" becomes -5..5 (common library);
  - the gather header's "selfOcclusion()", a function that does not exist;
  - `asdoflive.cpp`'s pass list (reduce, complete, tiles, gathers, composite).
- **Doc:** an execution record per step. The original "Passes" section is marked as
  superseded by phase 5.
- **No behaviour change.** Images are identical to step 3.

## Files (all owned)

- `indra/newview/asdoflive.cpp`;
- shaders `asDoFLiveCommonF.glsl`, `asDoFLiveReduceF.glsl`, `asDoFLiveTileF.glsl`,
  `asDoFLiveGatherF.glsl`, `asDoFLiveCompositeF.glsl`, and the new
  `asDoFLiveCompleteF.glsl`;
- `scripts/testing/dof_live_reference.py`;
- `doc/ayanestorm-depth-of-field-live-plan.md`.

The new shader is registered in `ASDoFLive::registerShaders()`/`createShaders()`, which are
already wired into `ASDepthOfField`. There are no non-owned edits and no cache revision bump.

## Verification

- **Per step:**
  - `python scripts/testing/dof_live_reference.py` passes, and the survey numbers are
    recorded;
  - glslang links every Live program with the library at `#version 410 core` and `400`;
  - `git diff --check` is clean, and LF throughout.
- **Runtime (user builds), Windows:**
  1. FPS in the 45 FPS reference scene at Low, Medium and High, Live with lens effects off,
     before (HEAD) and after each step.
  2. Debug views 4, 6, 9 and 10 look as before: no magenta blocks in 9, and holes behind
     the avatar filled.
  3. The jaw and neck scene, hair over the beach: unchanged.
  4. Lens effects on (cat's eye, CA, spherical): unchanged.
  5. Snapshots at window size and 2×.
- macOS afterwards, as agreed.

## Phase 5 execution record

### Step 1: completion pyramid (2026-10-01, unbuilt)

- **New pass** `asDoFLiveCompleteF.glsl` ("Live DoF complete"), between the reduce and the tiles. It runs one draw per mip level, top down, into `sDoneA` (completed N1, F, B1, B2) and `sDoneB` (completed E_N1, E_B1, M_B2) through a raw FBO (`sCompleteFBO`, 5 draw buffers).
  - During level l's draw, the completed textures' base level is l + 1. They are reset to 0 after.
  - Their mip storage comes from one `glGenerateMipmap` at allocation.
  - The program is created without the common library, so the library's samplers never count against macOS's 16 units.
- **Readers.**
  - Gathers: one `textureLod` of sums and one of energies per tap. N2 reads its raw mips, as before.
  - The tile pass reads per texel with `texelFetch`.
  - The composite's `focusFill()` reads completed F at level 0, and drops `emissiveRect` (14 samplers).
  - `liveCompleted()`, `LIVE_COMPLETE_EPSILON` and the `max_level` uniforms are removed.
- **Model.** `read_completed()` now reads `build_completed()`, the same recurrence once per texel. All 28 tests pass. The survey is identical to the feasibility prototype. The changes against the per-tap reads:
  - smooth rock fringe: 0.0005 to 0.0028 (old defect 0.057);
  - checker rock: 0.024 to 0.020;
  - split surface: 0.0044 to 0.0013;
  - hidden-veil step: 0.0011 to 0;
  - far hole: 0.0041 to 0.0030 rms;
  - everything else is equal within 0.0002.
- glslang links Reduce, Tile, Gather and Composite with the library, and compiles Complete alone, at 410 core and 400. `git diff --check` is clean, LF throughout.
- Also in this step: the `sa_strength` comment range in the common library is fixed to -5..5.

### Step 1: runtime (2026-10-01)

- The user saw no defect. In the same area (camera and scene not identical): DoF off 46 FPS, Live 41 FPS, so Live costs about 2.65 ms, against about 6.7 ms measured earlier (56.5 / 41 FPS). Committed.

### Step 2: one reduce pass (2026-10-01, unbuilt)

- `asDoFLiveReduceF.glsl` writes all seven bin outputs: N2, N1, radius, F, B1, B2, visibility. `reduce_pass` is removed.
- `sReduceFBO` attaches level 0 of `sBinsA` (3) and `sBinsB` (4) with 7 draw buffers. It is allocated and checked in `ensureTargets()`. If it is incomplete, the allocation fails and Live returns false, as for any target.
- Each full-resolution pixel is now decomposed once instead of twice. The model is unchanged: same sums.
- glslang links Reduce with the library at 410 core and 400. `git diff --check` is clean, LF throughout.
