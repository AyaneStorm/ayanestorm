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
3. **Moon phase (implemented 2026-08-23)**: `ASMoonPhase` drives an analytic
   spherical terminator reconstructed from the moon disc's UV coordinates.
   The 0-to-1 cycle runs New, First quarter, Full, Last quarter, New, with
   0.5 as the compatibility default. The manual slider advances linearly by
   projected illuminated area rather than orbital angle, preventing an
   effectively full-looking plateau around 0.45-0.55 and producing more useful
   crescents. The same illuminated-area fraction scales AyaneStorm moon god
   rays, while general scene moonlight is unchanged. `ASMoonPhaseCurvature`
   defaults to the artistically selected 1.75; 1.0 restores the spherical
   terminator, and the exposed range extends to 5.0 for artistic headroom.
   It applies a nonlinear exponent to reconstructed lunar depth, genuinely
   reshaping the curve instead of merely changing effective phase angle.
   God-ray energy uses a cached numerical integration of the same phase mask.
   `ASMoonPhaseSoftness` adds a short configurable transition across the
   terminator on top of resolution-dependent antialiasing; its artistically
   selected default is 0.09.
   `ASMoonPhaseTilt` rotates the terminator around the disc from -180 to 180
   degrees without changing illuminated area or god-ray energy.

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

## Phase-aware atmospheric moon halo

The dormant `FACE_BLOOM` sky face now carries a fixed-size procedural
moon-centered billboard rendered behind the disc. `ASMoonHaloStrength`,
`ASMoonHaloRadius`, and `ASMoonHaloSoftness` control it independently of global
HDR bloom, with live radius changes performed in the shader. The halo remains
circular like atmospheric scattering in reference photography, while its total
strength follows the integrated visible phase fraction. Its color inherits the
moon's horizon tint. `ASMoonHaloEnabled` provides an explicit master switch.
Directional limb weighting makes it strongest beside the illuminated phase and
reduces the dark-side halo to a faint 15% circular base rather than erasing it.

The halo pass depth-tests against foreground geometry but does not write depth.
Its billboard is coplanar with the moon disc, so writing depth before the disc
causes z-fighting and visible triangular artifacts across the lunar surface.
The procedural halo is explicitly masked using the moon texture's alpha rather
than an assumed circular radius. This prevents both halo leakage through a
transparent phase and a dark annulus when transparent texture padding places
the visible moon edge inside the analytic disc radius. The texture alpha itself
locates the true visible border. A five-tap radial Gaussian filter broadens its
usually one-texel edge into a progressive inward halo reduction. The additional
texture samples execute only near the moon disc, not across the full halo quad.
The filtered 50% coverage contour defines the visible limb: halo remains full
through that contour and is attenuated only inward. Reducing it from the first
nonzero alpha suppresses glow outside the limb and creates a contrast ring.
Fully masked halo fragments are discarded before writing any deferred render
target. A zero-alpha fragment that still writes `GBUFFER_FLAG_SKIP_ATMOS`
darkens the surrounding sky and appears as a ring around the moon.
Both halo and moon passes preserve the already-rendered sky's G-buffer metadata
instead of blending a categorical skip-atmosphere flag through translucent
edges. Such flag blending also creates an outline along the moon texture alpha.

The halo uses alpha-modulated additive blending (`src * alpha + dst`). Ordinary
alpha replacement (`src * alpha + dst * (1 - alpha)`) can subtract brightness
when the HDR sky destination is brighter than the halo source, creating a dark
ring precisely throughout the progressive mask. Normal alpha blending is
restored immediately before the moon disc pass.

Halo energy follows `ASMoonBrightnessMultiplier` through the zero-preserving
saturating response `b / (1 + 0.7b)`. Direct multiplication saturates the
additive halo into a solid white envelope at multipliers 2–4. The calibrated
curve evaluates to approximately 0.59 at brightness 1 and 0.98 at the preferred
3.15 default, then grows slowly toward an asymptote. Halo strength remains the
independent artistic factor, and brightness zero still produces no halo.

Halo phase energy is linear through integrated illumination 0.406, preserving
the preferred phase-0.80 appearance with the default phase controls. Above that
anchor it uses `f / (1 + 1.52 * (f - 0.406)^2)`, reducing full-moon halo energy
from 1.0 to approximately 0.65 without altering crescents or quarter phases.
The squared excess keeps the curve's slope continuous at the anchor.

## Terminator crater relief

`ASMoonTerminatorReliefStrength` adds directional texture contrast in the
incoming-light direction near the phase terminator, approximating the stronger
crater relief produced by grazing illumination. `ASMoonTerminatorReliefWidth`
controls how far that detail band extends from the terminator. The effect is
disabled at strength 0 and naturally vanishes at full and new moon, where the
projected terminator direction has no usable screen-space axis.

The phase softness range extends to 0.30. Its nonlinear smooth transition is
anchored at 50% illumination on the geometric terminator, preventing softness
from shifting the apparent quarter-moon boundary into the dark hemisphere.
Phase illumination controls how strongly the moon texture is blended over the
existing sky, avoiding an opaque black shadowed disc. Halo and god-ray energy
use the same integrated direct-light curve independently of earthshine.

`ASMoonEarthshineStrength` blends a configurable fraction of the moon texture
over the existing sky on the phase-shadowed hemisphere (default 0.05, range
0.00–0.30), approximating Earth-reflected illumination. At zero, fully shadowed
pixels reveal the sky rather than forming an opaque black disc. Earthshine does
not contribute to the direct-light halo or volumetric god rays.

Relevant starting points: `indra/newview/app_settings/shaders/class1/deferred/moonV.glsl`
and `moonF.glsl` for current haze/disc rendering; `llpaneleditsky.h` and
`app_settings/settings.xml` for any phase-related settings already exposed.
