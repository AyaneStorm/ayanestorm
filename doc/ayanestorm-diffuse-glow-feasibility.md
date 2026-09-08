yes# Diffuse Glow / Bright-Surface Bloom Feasibility

## Decision

Yes. AyaneStorm can add an optional, live-controlled glow from bright scene
surfaces with a small, low-risk change. The existing glow extraction shader
already implements luminance-based automatic glow; `LLPipeline::generateGlow()`
currently disables that branch by sending `9999` as its luminance threshold.

For user-facing terminology, prefer **Bright-surface bloom** over **Diffuse
glow**. The effect operates on the completed screen image, not specifically on
the renderer's diffuse material channel.

## Implementation Status

Implemented on 2026-09-08, not yet built or runtime-tested. The Camera Effects
panel now provides Enable, Brightness threshold, Softness, and Strength. The
effect feeds the existing glow extraction/blur path without
adding a render pass. Implementation is isolated in `asdiffuseglow.cpp/.h`,
`effects/asDiffuseGlowF.glsl`, and minimal registration/pipeline hooks. The
Second Life-owned `glowExtractF.glsl` retains only a linked-function declaration
and one call that merges the returned mask with normal material glow.

## Existing Path

- `indra/newview/pipeline.cpp:8147-8255` extracts, blurs, and later composites
  glow. No new render target or full-screen pass is required.
- `indra/newview/pipeline.cpp:8161` sends a fixed minimum luminance of `9999`,
  making automatic bright-pixel extraction unreachable.
- `indra/newview/app_settings/shaders/class1/effects/glowExtractF.glsl:45-61`
  already calculates a soft luminance/warmth mask and combines it with the
  existing material-glow alpha using `max()`.
- `RenderGlowMinLuminance` already exists in `settings.xml`, but is neither
  refreshed into `LLPipeline::RenderGlowMinLuminance` nor used by the draw
  path. The static member exists but is currently dead.
- The same blur controls already used for material glow apply afterward:
  `RenderGlowResolutionPow`, `RenderGlowIterations`, `RenderGlowWidth`, and
  `RenderGlowStrength`.
- Extraction happens after tone mapping/gamma conversion and before DoF and
  anti-aliasing (`pipeline.cpp:9090-9160`). It therefore sees display-range
  color in both HDR and non-HDR renderer modes.

## Recommended Controls

Implemented viewer-local persisted settings. Defaults were selected from the
runtime-tuned values shown in the 2026-09-08 UI capture:

1. `ASDiffuseGlowEnabled` (Boolean, default true): enables bright-surface
   extraction without changing normal material glow.
2. `ASDiffuseGlowThreshold` (F32, default `0.67`): visible-brightness cutoff.
3. `ASDiffuseGlowSoftness` (F32, default `0.80`): width of the threshold knee.
4. `ASDiffuseGlowStrength` (F32, default `0.11`): extraction contribution,
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

## Minimal Implementation Shape

1. Add the three `ASDiffuseGlow*` settings to `settings.xml`.
2. In `generateGlow()`, read the settings through `LLCachedControl` and send
   either the selected threshold or `9999` to `GLOW_MIN_LUMINANCE`.
3. Send the new contribution value to `GLOW_MAX_EXTRACT_ALPHA`; use Rec. 709
   luminance weights while the feature is enabled.
4. Add the Camera Effects controls and reset callbacks, following the existing
   AyaneStorm camera-effect patterns.

Only the minimal `pipeline.cpp` integration edit touches an upstream-owned
file and must be enclosed in AyaneStorm ownership tags. The UI can remain in
the existing AyaneStorm-owned panel; no new renderer module is justified for
this small reuse of the existing glow path.

## Limitations and Follow-up Option

- The proposed version blooms visually bright, post-tonemap pixels. It cannot
  distinguish diffuse illumination from specular highlights, sky, UI-like
  in-world surfaces, or emissive color based on material semantics.
- Material glow and bright-surface bloom share the same blur radius and final
  multiplier. Fully independent radii require a second extraction/blur chain,
  additional render targets, and measurable GPU cost.
- Since the shader merges masks with `max()`, the two sources do not add their
  strengths where they overlap. This is safe and avoids runaway brightness,
  but it is not mathematically independent mixing.
- A physically motivated HDR bloom should extract from scene-linear HDR color
  before tone mapping. That is a separate, more invasive design because the
  current post-tonemap alpha also carries authored glow information. Start
  with the existing post-tonemap path and consider pre-tonemap bloom only if
  runtime comparisons show that highlight rolloff makes the result too flat.

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
