# 3D Scene Color Mixer Feasibility

Author: chanayane@firestorm

## Conclusion

An Adobe Camera Raw-style color mixer is feasible as a real-time full-screen
post-process. It can change hue, saturation, and luminance for selected color
ranges across the final 3D scene.

## Existing Pipeline

The deferred renderer already applies full-screen tone mapping in
`indra/newview/app_settings/shaders/class1/deferred/postDeferredTonemap.glsl`.
The viewer exposes tone-map and exposure settings through Phototools, providing
an established pattern for live controls.

The older `LLPostProcess` implementation also contains a global color-filter
shader with brightness, contrast, and saturation controls, but it is not the
best foundation for an HDR-aware color mixer.

## Recommended Design

Create an AyaneStorm-owned color-grading module and shader. Apply the mixer to
the tone-mapped scene before final display gamma, or integrate its function at
that exact boundary if an extra full-screen pass is undesirable.

Expose eight color bands similar to Camera Raw: red, orange, yellow, green,
aqua, blue, purple, and magenta. Each band has hue, saturation, and luminance.
Use smooth, overlapping hue masks so adjustments do not produce hard color
boundaries. Include a master enable switch, reset, and optional preset support.

Add global camera controls alongside the per-color mixer:

- Exposure
- Brightness (midtone lightness)
- Contrast
- Temperature and tint
- Shadows (dark tones)
- Highlights (light tones)
- Blacks (black point)
- Whites (white point)
- Saturation
- Vibrance
- Global hue
- Film grain amount, size, and optional colored-grain control

Vibrance should increase saturation selectively, protecting skin-like warm hues
and colors that are already strongly saturated. It should not be implemented as
a second saturation slider. Grain should use stable screen-space noise that
changes between frames without visibly swimming when the camera moves.

Keep the implementation in new `as*` source and shader files. Limit changes to
upstream `ll*` pipeline files to the hooks required to invoke the module, with
AyaneStorm ownership tags.

## Technical Considerations

- Perform selection in a perceptual hue-based color model rather than changing
  RGB channels directly.
- Preserve neutral pixels by reducing hue weighting as saturation approaches
  zero.
- Define luminance adjustment carefully to avoid hue shifts and HDR clipping.
- Decide whether UI, HUD, selection outlines, and snapshots should receive the
  effect. A scene render-target pass naturally excludes most viewer UI.
- A single full-screen shader is expected to have modest GPU cost. Combining it
  with the existing tone-map pass minimizes bandwidth cost.
- The legacy and no-post-processing paths need explicit behavior.

## Suggested Initial Scope

Start with a master enable; global exposure, brightness, contrast, temperature,
tint, shadows, highlights, blacks, whites, saturation, vibrance, hue, and grain
controls; plus eight bands and three controls per band. Apply it to the world
scene and snapshots, exclude viewer UI, use identity defaults, and support live
preview and reset. Presets and targeted color picking can follow.

## Processing Order

A practical initial order is exposure, white-balance temperature and tint,
contrast and tonal-range controls, tone mapping, brightness, global hue and
saturation/vibrance, per-color HSL mixing, then grain and display gamma. The
final shader implementation must keep operations in the correct linear or
display-referred color space. Grain should be applied near the end so tone
mapping does not crush it out of shadows or highlights.

Exposure should multiply linear scene light, preferably in photographic stops.
Brightness should primarily move perceived midtones. Shadows and highlights
should use broad, overlapping tonal masks, while blacks and whites should set or
shape the endpoints. Keeping these controls distinct avoids several sliders
performing the same operation under different names.
