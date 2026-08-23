# Moon rendering ideas (backlog)

Loose ideas noted 2026-08-23, not yet scoped or prioritized. Not part of the
volumetric lighting work.

1. **Horizon dimming (implemented 2026-08-23)**: `ASMoonHorizonMinOpacity`
   now places a configurable lower bound on the moon disc's legacy quadratic
   alpha fade. The AyaneStorm Preferences > Rendering 2 slider defaults to
   0.65, while 0.0 reproduces upstream behavior and 1.0 disables dimming.
2. **Horizon reddening**: the moon should optionally gradually tint
   orange/red as it approaches the horizon, mimicking real-world atmospheric
   reddening (similar to sunset coloring of the sun).
3. **Moon phase**: the moon should support a configurable phase (crescent,
   gibbous, etc.) rather than always rendering full moon.

Relevant starting points: `indra/newview/app_settings/shaders/class1/deferred/moonV.glsl`
and `moonF.glsl` for current haze/disc rendering; `llpaneleditsky.h` and
`app_settings/settings.xml` for any phase-related settings already exposed.
