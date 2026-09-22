# Diffuse Glow / Bright-Surface Bloom Feasibility

## Decision

Yes. AyaneStorm now has an optional, live-controlled glow from bright scene
surfaces implemented as a small, low-risk extension of the existing glow
extraction and blur path.

For user-facing terminology, prefer **Bright-surface bloom** over **Diffuse
glow**. The effect operates on the completed screen image, not specifically on
the renderer's diffuse material channel.

## Implementation Status

Initial version implemented on 2026-09-08. The Camera Effects
panel now provides Enable, Brightness threshold, Softness, and Strength. The
effect feeds the existing glow extraction/blur path without
adding a render pass. Implementation is isolated in `asdiffuseglow.cpp/.h`,
`effects/asDiffuseGlowF.glsl`, and minimal registration/pipeline hooks. The
Second Life-owned `glowExtractF.glsl` retains only a linked-function declaration
and one call that merges the returned mask with normal material glow.

The scene-linear HDR improvement was implemented on 2026-09-21 and has not
been built or runtime-tested. When HDR and bright-surface bloom are enabled,
automatic bright-pixel extraction, blur, and RGB composition now happen before
tone mapping in isolated floating-point targets. Authored material-glow alpha
remains on the unchanged compatibility path. Shader/allocation failure falls
back to the former post-tonemap path.

## Existing Path

- `indra/newview/pipeline.cpp:8147-8255` extracts, blurs, and later composites
  glow. No new render target or full-screen pass is required.
- `LLPipeline::generateGlow()` now sends the live
  `RenderGlowMinLuminance` value to the legacy automatic-glow branch and binds
  the independent AyaneStorm bright-surface controls.
- `indra/newview/app_settings/shaders/class1/effects/glowExtractF.glsl:45-61`
  already calculates a soft luminance/warmth mask and combines it with the
  existing material-glow alpha using `max()`.
- `RenderGlowMinLuminance` is live again. It controls the legacy automatic
  extraction branch; `ASDiffuseGlowThreshold`, `ASDiffuseGlowSoftness`, and
  `ASDiffuseGlowStrength` control the viewer-local branch.
- The same blur controls already used for material glow apply afterward:
  `RenderGlowResolutionPow`, `RenderGlowIterations`, `RenderGlowWidth`, and
  `RenderGlowStrength`.
- The compatibility extraction happens after tone mapping/gamma conversion and
  before DoF and anti-aliasing. With HDR bright-surface bloom active, automatic
  bright-pixel extraction instead runs on scene-linear color before tone
  mapping; only authored material glow remains in the compatibility pass.

## Recommended Controls

Implemented viewer-local persisted settings. Defaults were initially selected
from the 2026-09-08 UI capture, then retuned for scene-linear HDR bloom from
the 2026-09-21 runtime comparison:

1. `ASDiffuseGlowEnabled` (Boolean, default false): enables bright-surface
   extraction without changing normal material glow.
2. `ASDiffuseGlowThreshold` (F32, default `0.15`): exposure-adjusted brightness cutoff.
3. `ASDiffuseGlowSoftness` (F32, default `0.80`): width of the threshold knee.
4. `ASDiffuseGlowStrength` (F32, default `0.15`): extraction contribution,
   separate from the shared final `RenderGlowStrength`.

The implementation uses fixed Rec. 709 luminance weights
`(0.2126, 0.7152, 0.0722)` for this new
mode. Do not use the current `RenderGlowLumWeights` default `(1, 0, 0)`, which
would make selection depend only on red.

The implemented mask uses a symmetric soft knee centered on Threshold, with
Softness controlling its width. Softness zero selects a hard step.

`RenderGlow` remains the master glow switch. If it is off, the new controls
should be visibly disabled and explain that both authored material glow and
bright-surface bloom are unavailable.

## UI Placement

Recommended: add a **Bright-surface bloom** section at the bottom of the
existing **Camera Effects** panel.

Reasons:

- This is a screen-space optical/post-processing effect, like lens flare,
  vignette, and chromatic aberration; it does not modify EEP/environment assets.
- `panel_as_camera_effects.xml` remains 530 pixels tall. Every section now
  places its title and master switch on one header row. All master switches
  share one horizontal position, internal controls use a 27-pixel pitch, and
  sections have a measured 20-pixel gap. All controls fit without clipping or
  scrolling.
- A ninth top-level tab would make the already crowded 488-pixel tab bar less
  usable.
- Advanced shared glow quality/width/iteration controls already exist in
  Phototools and do not need duplication in the initial Environment Effects UI.

If all global glow controls are later moved into Environment Effects, use a
dedicated **Glow / Bloom** tab instead; at that point the control group is large
enough to justify another tab.

## Implemented Shape

1. Four persisted `ASDiffuseGlow*` settings provide enable, threshold,
   softness, and strength controls.
2. `ASDiffuseGlow::bindExtractionUniforms()` binds clamped live values and owns
   the one-shot automatic-extraction suppression that prevents duplicate HDR
   bloom without changing the configured luminance threshold.
3. The linked `asDiffuseGlowF.glsl` utility calculates a scalar Rec. 709 mask;
   `glowExtractF.glsl` merges it with authored/legacy glow using `max()`.
4. Camera Effects controls and reset callbacks follow the other viewer-local
   camera-effect patterns.

The initial path required only a minimal `pipeline.cpp` integration edit. The
HDR follow-up adds small ownership-tagged hooks in `pipeline.cpp` and shader
lifecycle registration while keeping extraction, targets, fallback logic, and
the new shaders in the existing AyaneStorm-owned `asdiffuseglow` module. The UI
remains in the existing AyaneStorm-owned panel.

## Limitations and Follow-up Option

- The compatibility version blooms visually bright, post-tonemap pixels. It cannot
  distinguish diffuse illumination from specular highlights, sky, UI-like
  in-world surfaces, or emissive color based on material semantics.
- Material glow and bright-surface bloom share the same blur radius and final
  multiplier. Fully independent radii require a second extraction/blur chain,
  additional render targets, and measurable GPU cost.
- Since the shader merges masks with `max()`, the two sources do not add their
  strengths where they overlap. This is safe and avoids runaway brightness,
  but it is not mathematically independent mixing.
- The HDR path adds one full-resolution `GL_RGBA16F` composite and three lazy
  low-resolution `GL_RGBA16F` glow targets while enabled. This is the principal
  memory and bandwidth cost and requires runtime measurement.

## Hue-Preserving HDR Bloom Research Review (2026-09-21)

Sources: the repository copy in `doc/hdrbloomresearch.md` and
[ledmapper issue #493, "hue-preserving HDR bloom"](https://github.com/zackees/ledmapper/issues/493).
The assessment below reads the opening revised directive first. That directive
supersedes the issue's older problem framing, candidate ranking, and acceptance
criteria where they conflict.

### Conclusion

The research is useful, but its authoritative revised target is a frosted LED
sign rather than a general 3D scene. Its diagnosis of additive overflow,
per-channel clipping, and hue loss applies directly to AyaneStorm. Its desired
white-out, loss of bright-region detail, and brightness-dependent wide
diffusion do not. The implemented HDR path follows the transferable result:
bright-surface bloom operates on scene-linear HDR color before tone mapping and
is tone-mapped only once with the scene.

The original implementation remains the compatibility/fallback path. The HDR
implementation adopts the transferable color-pipeline findings without
transplanting the issue's final LED-specific spatial behavior.

### What Transfers to AyaneStorm

- Bloom should spread the source RGB energy. Extraction must use one scalar
  gate for the entire RGB triplet, and every blur tap must use identical
  weights for R, G, and B. AyaneStorm already satisfies this locally:
  `asDiffuseGlowMask()` returns one Rec. 709 luminance mask, and `glowF.glsl`
  applies the same kernel to all channels.
- Thresholding, compositing, and tone mapping should remain in one linear
  color space. Per-channel thresholds or clamps create hue shifts; adding in
  display-encoded space is not energy-correct.
- Highlight compression should act on a luminance/lightness or RGB-norm axis
  while preserving RGB ratios. Any path toward white should be explicit and
  tunable rather than an accidental result of independent channel clipping.
- White pixels need no hue protection. A hue-preserving operation naturally
  becomes a no-op for achromatic input, so a special detector should not
  suppress white bloom.
- Tests must include bright white and saturated red, green, and blue sources;
  judging white bloom alone cannot reveal colored-highlight failure.

These points agree with the literature surveyed by the issue, including
linear HDR bloom practice, norm/luminance-based tone mapping, Oklab gamut
mapping, and ACES chroma compression. The issue correctly notes that applying
this literature to its sparse LED-panel content is an inference rather than a
directly measured result. The same caution applies to AyaneStorm.

### Previous AyaneStorm Gap and Implemented Correction

Previously, bright-surface extraction ran only after `tonemap()` or
`gammaCorrect()` in `renderFinalize()`. `generateGlow()` therefore received
display-range color, not the original `GL_RGBA16F` scene radiance. The blur
preserved the RGB values it received, but could not recover highlight energy
or chromaticity already altered by the tone mapper.

`glowcombineF.glsl` then performed a plain addition:
`scene + blurredGlow`. In this late display-space position, a bright saturated
surface has little or no headroom in its dominant channel. Addition can exceed
the target range and later clipping can change channel ratios or push the
result toward white. Enabling `RenderGlowHDR` only changes the glow render
target to `GL_RGBA16F`; it does not move extraction or composition into the
scene-linear HDR pipeline, so it does not solve this structural problem.

The new HDR path corrects that placement. Its exposure-aware selection uses
one scalar for the RGB triplet, blur remains channel-neutral in floating-point
targets, and `asHDRDiffuseGlowCombineF.glsl` adds RGB before the existing tone
map while preserving the original material-glow alpha. The later compatibility
pass excludes automatic bright-pixel extraction to prevent double bloom.

### Implemented HDR Direction

The implementation follows this structure:

1. Extract bright-surface bloom from `mRT->screen` while it is still
   scene-linear `GL_RGBA16F`. Use exposure-aware units and a scalar soft knee
   applied uniformly to RGB.
2. Blur the extracted linear RGB in a floating-point target with identical
   per-channel weights. A multi-resolution downsample/upsample pyramid is a
   better long-term radius model than repeatedly widening one low-resolution
   separable kernel, but it is not required for the first HDR experiment.
3. Add the blurred light to the linear scene before `tonemap()`, then run the
   existing display transform once over the combined result. Evaluate whether
   the current tone mapper desaturates intense colored bloom.
4. If runtime tests show that colored highlights still whiten, add an explicit
   ratio-preserving highlight/gamut treatment at the display-transform
   boundary. Start with a max-RGB or luminance norm scale; consider a
   perceptual-space path-to-white control only if runtime images justify the
   extra complexity.
5. Keep authored material glow on its existing compatibility path initially.
   Its alpha is already embedded in the post-tonemap screen target, so moving
   it safely requires a separate material-glow signal or another render
   target. Do not let that coupling force bright-surface bloom to remain late.

This is isolated in the AyaneStorm module with minimal ownership-tagged hooks
to shader lifecycle, buffer release, extraction binding, and `renderFinalize()`;
`pipeline.h` retains its original API.

### Ideas Not to Copy Directly

- Do not target the issue's frosted-acrylic white-out. In a Second Life scene,
  destroying detail inside every bright white region would make sunlit walls,
  clouds, avatars, and specular surfaces look fogged or overexposed.
- Do not copy the revised directive's main prescription literally: retaining
  legacy bloom energy while rerouting locally limited overflow into a wider
  lobe is the correct response for its acrylic target, not a general camera
  bloom model. In AyaneStorm it would make bloom radius depend on saturation
  pressure and could spread energy far beyond the source.
- Do not add a neutral far-veil merely because ocular-glare models permit one.
  It conflicts with the viewer's need to preserve dark scene regions and is
  not required for hue preservation.
- Do not use a corrective saturation boost as the primary fix. It can amplify
  noise and merely hides the late-composite problem.
- Do not replace AyaneStorm's whole display transform solely to improve bloom.
  That has a much larger regression surface than moving this effect into the
  existing linear-HDR stage.

### Validation for an HDR Prototype

- Compare current and HDR paths at identical exposure, EEP, camera, resolution,
  glow radius, and perceived strength.
- Use white, near-white, and saturated RGB emissive surfaces plus sunlit diffuse
  surfaces, specular reflections, sky, water, avatars, and legacy prim glow.
- Check halo hue from core to edge, highlight texture, dark-region lift,
  overlapping differently colored halos, temporal exposure changes, and
  snapshots.
- Test HDR renderer on/off. Define the new mode's fallback explicitly when HDR
  is unavailable.
- Measure GPU time and bandwidth separately for extraction, blur, and composite.
  Do not promote it based on theory alone; paired runtime captures are the
  deciding evidence.

### Reference Entry Points

- Research issue: <https://github.com/zackees/ledmapper/issues/493>
- Physically based bloom overview cited there:
  <https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom>
- Oklab hue-preserving gamut mapping cited there:
  <https://bottosson.github.io/posts/gamutclipping/>
- Khronos PBR Neutral tone mapper cited there:
  <https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral>
- ACES 2.0 chroma compression cited there:
  <https://docs.acescentral.com/system-components/output-transforms/technical-details/chroma-compression/>

## Test Matrix

- Feature off: pixel-identical behavior to current rendering.
- Feature on with `RenderGlow` on/off.
- HDR renderer on/off and `RenderGlowHDR` on/off.
- Bright white, saturated red/green/blue, sky, water/specular highlights, PBR
  emissive surfaces, and legacy prim glow.
- Exact OIT, AVBOIT, and normal transparency paths.
- Strength zero, threshold endpoints, multiple glow resolutions/iterations,
  DoF, FXAA/SMAA, snapshots, and color grading.
- GPU timing should remain essentially unchanged because no pass is added;
  verify that higher glow coverage does not expose precision/banding issues in
  the non-HDR glow target.
