# Moon rendering ideas (backlog)

Loose ideas noted 2026-08-23, not yet scoped or prioritized. Not part of the
volumetric lighting work.

1. **Horizon dimming (implemented 2026-08-23)**: `ASMoonHorizonMinOpacity`
   now places a configurable lower bound on the moon disc's legacy quadratic
   alpha fade. The AyaneStorm Preferences > Rendering 2 slider defaults to
   0.65, while 0.0 reproduces upstream behavior and 1.0 disables dimming.
2. **Horizon reddening (implemented 2026-08-23)**: the configurable
   `ASMoonHorizonTint` is multiplied into the visible moon disc according to
   `ASMoonHorizonTintStrength`. The tint peaks at the horizon and smoothly
   ends near 20 degrees elevation; strength 0.0 disables the effect. The same
   derived tint is applied to AyaneStorm moon god rays, but not to general
   scene moonlight. God rays receive true world-space moon elevation because
   their existing `moon_dir` uniform is transformed into camera space.
3. **Moon phase**: the moon should support a configurable phase (crescent,
   gibbous, etc.) rather than always rendering full moon.

## Moonrise and moonset disc cutoff

`ASRenderPartialMoonBelowHorizon` replaces the original center-based moon-disc
draw cutoff when enabled. AyaneStorm continues drawing while the highest moon
quad corner remains above the camera-relative horizon, automatically accounting
for the environment's moon scale. Disabling it restores Firestorm behavior.
Moonlight, shadows, and god-ray source activation use the same extended lifetime
so the visible below-center sliver continues to illuminate consistently.
While enabled, the below-center portion retains the configured minimum horizon
opacity instead of hitting Firestorm's full-opacity boundary special case.

## Viewer-local moon brightness

`ASMoonBrightnessMultiplier` scales the active sky asset's moon-disc brightness
at draw time without modifying the asset. Its default of 1.0 preserves the sky
value; the Rendering 2 control exposes a 0.0 to 5.0 range.

Relevant starting points: `indra/newview/app_settings/shaders/class1/deferred/moonV.glsl`
and `moonF.glsl` for current haze/disc rendering; `llpaneleditsky.h` and
`app_settings/settings.xml` for any phase-related settings already exposed.
