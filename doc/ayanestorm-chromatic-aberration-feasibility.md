# AyaneStorm Chromatic Aberration Feasibility

Author: chanayane@firestorm

## Assessment

Feasible as an optional screen-space camera effect. Implemented following the source review. No build or runtime verification has been performed.

## Existing Integration Points

- `indra/newview/skins/default/xui/en/panel_as_camera_effects.xml` contains lens flare and vignette controls. Expand the panel and its containing floater for another section.
- `indra/newview/pipeline.cpp`, `LLPipeline::renderFinalize()`, already uses source and destination post-processing targets around glow, depth of field, and FXAA/SMAA.
- `indra/newview/app_settings/shaders/class1/deferred/rlvF.glsl`, `chromaticAberration()`, demonstrates three texture samples assembling offset red and blue channels with the original green channel. RLVa activation is separate from a standalone camera setting.
- `indra/newview/asvignette.cpp` provides a pattern for optional shader lifecycle, settings, UI reset callbacks, and exclusion checks. Its multiplicative overlay cannot implement chromatic aberration because chromatic aberration needs scene texture sampling.

## Proposed Implementation

Use a dedicated `aschromaticaberration.cpp/.h` module and fragment shader, with minimal ownership-tagged integration where required. Add a pass after antialiasing and before RLVa and snapshot-frame processing, using distinct source and destination targets and swapping only when rendered. Existing later lens flares and vignette would remain after this pass.

Sample red and blue at opposite radial offsets, preserving green. Increase channel separation toward the image edges. Clamp sampling coordinates to valid texture bounds. Suggested controls: enable (default off), strength, and radial falloff, with live updates and reset buttons. No depth buffer or frame history is required for this screen-space approximation.

Skip the pass when disabled, at zero strength, during cube snapshots, or when its shader is unavailable. Follow background-isolation exclusion conventions to avoid color fringes at the isolated subject boundary.

## Cost and Validation

Expected cost is one fullscreen draw with approximately three scene texture samples per pixel. Existing post-processing targets should avoid a dedicated full-resolution allocation; target routing must be verified during implementation. Actual GPU cost requires measurement, especially at high resolutions.

Runtime validation should cover disabled equivalence, edge clamping, aspect ratios, ordinary and high-resolution snapshots, DoF, FXAA/SMAA, RLVa effects, background isolation, and unaffected UI. No shader-version bump is needed for the proposed development work.

## Implemented Controls and Integration

- Dedicated `aschromaticaberration.cpp/.h` and `deferred/aschromaticaberrationF.glsl` implement the effect, shader lifecycle, and reset callbacks.
- `ASChromaticAberrationEnabled`: Boolean, default false.
- `ASChromaticAberrationStrength`: 0–100, default 3. Each red/blue channel moves this many pixels at a corner when the shorter viewport dimension is 1080 pixels. Displacement scales with resolution.
- `ASChromaticAberrationFalloff`: 0.001–4 in 0.001 increments, default 1.25. Higher values concentrate separation near corners; near-zero values make separation nearly uniform across the image (artistic minimum, avoids the divergence at an exact zero exponent).
- Camera Effects contains enable, strength, falloff, center X/Y, beacon toggle, and reset controls. The shared panel is 530 pixels tall and the standalone floater is 560 pixels tall; the existing Environment Effects container already fits it.
- The pass uses the existing spare post-processing target after DoF and antialiasing. When it renders, the previous source becomes the spare for subsequent RLVa/vignette/frame processing. Disabled execution leaves existing routing unchanged.
- Three bilinear scene samples preserve green and alpha, shifting red outward and blue inward. Coordinates clamp to texel centers at the texture boundaries.
- Background isolation, cube snapshots, zero strength, unavailable shaders, invalid target dimensions, and aliased targets skip the pass.

## Center Point (Artistic, Not DoF-Linked)

Chosen over binding to the depth-of-field focus target: lateral chromatic aberration radiates from the lens's optical center (normally screen center), independent of focus depth. Longitudinal chromatic aberration is the DoF-dependent variant and is not implemented here.

- `ASChromaticAberrationCenterX` / `ASChromaticAberrationCenterY`: F32, 0–1, default 0.5 each. Normalized screen position; no Z/depth component, since the effect is screen-space.
- `ASChromaticAberrationShowBeacon`: Boolean, default false, not persisted across sessions (`Persist` 0), matching the transient nature of `ASRenderLightBeacon` in My Lights.
- `ASChromaticAberration::renderCenterBeacon()` draws a 2D UI-space crosshair at the configured center using `gUIProgram` and `gGL` line primitives, called from `render_hud_elements()` in `llviewerdisplay.cpp`. Unlike the My Lights world-space beacons, it is not gated on `RENDER_DEBUG_FEATURE_UI`, since it has its own checkbox and is a 2D overlay, not a 3D world marker.

## Static Validation

XML parsing, unique settings and UI names, control bindings, child layout bounds, and source/destination routing for all six DoF/AA combinations passed. Shader/C++ compilation and runtime appearance remain unverified; no build was attempted. High-resolution snapshot framing and actual GPU cost still need runtime validation. Center/beacon additions are likewise unverified at runtime.
