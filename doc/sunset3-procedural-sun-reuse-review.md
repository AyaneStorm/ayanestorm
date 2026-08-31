# `sunset3.shader` reuse review for the procedural sunset sun

## Scope

This review compares the reference full-screen ShaderToy-style renderer in
`/sunset3.shader` with AyaneStorm's billboard-based procedural sun in
`indra/newview/asproceduralsun.cpp` and
`indra/newview/app_settings/shaders/class1/deferred/sunDiscF.glsl`.

## Main finding

The realistic sunset light in `sunset3.shader` does not primarily come from
its sun-disc shape. It comes from wavelength-dependent atmospheric extinction,
forward Mie scattering, and HDR compression. Reusing the complete ray marcher
would duplicate and potentially contradict the active EEP atmosphere. The best
portable improvement is a cheap, elevation-dependent approximation of the
reference shader's transmittance applied to the procedural disc and halo.

## Reusable ideas

### 1. Physically motivated spectral extinction

The reference uses Earth-like Rayleigh coefficients
`vec3(5.8e-6, 13.5e-6, 33.1e-6)` (`sunset3.shader:256-259`) and evaluates
Beer-Lambert transmittance with `exp(-(betaR * depthR + betaM * depthM))`
(`sunset3.shader:973-983`). The much larger blue coefficient naturally removes
blue, then green, as the low sun's optical path grows. This is the strongest
idea to reuse.

For AyaneStorm, use a normalized optical-depth curve driven by solar elevation
or lower-limb elevation, then calculate a three-channel transmittance. Blend
that result with the live EEP sunlight colour so the viewer enhancement remains
compatible with authored skies. This could replace or augment the current
hand-shaped `mixSunsetColor()` progression in `asproceduralsun.cpp:144-154`.

### 2. Separate aerosol glow from the solar surface

The reference uses a Henyey-Greenstein-like forward Mie phase term
(`sunset3.shader:1017-1028`) and treats the highly concentrated solar component
separately (`sunset3.shader:1111-1113`). This agrees with AyaneStorm's existing
separate disc and additive halo architecture. Keep that architecture, but make
the halo profile resemble forward scattering: calculate it from the angular
dot product to the sun direction, or use a billboard-space approximation of
the same phase curve. The current rational profile
`1 / (1 + 4 r^2)` (`sunDiscF.glsl:58-65`) is convenient but not tied to a
scattering anisotropy.

A fitted phase-like profile can provide a bright compact aureole with a long,
soft tail. It should retain the existing explicit outer fade so the finite
billboard cannot reveal an edge.

### 3. Preserve radiance and compress highlights late

The reference keeps a very bright analytic sun (`getSunPoint()` multiplies by
100) and compresses it with a Jodie/Reinhard operator
(`sunset3.shader:411-442`). AyaneStorm should likewise keep the disc emissive
and let the viewer's existing HDR/exposure pipeline perform final compression.
Do not copy the reference shader's local tone mapper into `sunDiscF.glsl`,
because that would tone-map the sun twice and differ between HDR modes.

### 4. Elevation-dependent aerosol character

The reference varies Mie anisotropy, intensity, Rayleigh intensity, and scale
height as the sun approaches the horizon (`sunset3.shader:2055-2081`). A small
subset is useful: broaden and warm the procedural halo progressively with
optical depth instead of merely fading a fixed profile in below two degrees.
The present `halo_factor` is only an opacity gate
(`asproceduralsun.cpp:140-143`). Driving radius/profile concentration as well
would better mimic the reference sunset without ray marching.

## Ideas not worth copying

- The 8 view samples times 4 light samples in `scatter()` are too expensive for
  a small sun billboard and would duplicate EEP atmospheric scattering.
- `getAtmosphericScattering()` is explicitly labelled as the "simple sun" and
  uses screen-space coordinates, empirical absorption, and a local tone mapper.
  Its appearance may inspire tuning, but the function is not portable to a
  world-direction billboard.
- The reference clamps the below-horizon light to an artificial minimum
  direction (`sunset3.shader:997` and `1072-1084`). AyaneStorm should retain the
  true EEP sun direction and its current scale-aware cutoff.
- Its horizon desaturation and channel-specific gamma operations
  (`sunset3.shader:2245-2257`, `2298-2318`) are scene-wide grading hacks. Applying
  them only to the sun would be inconsistent; applying them globally would
  override EEP intent.
- Lens flare, water reflection, clouds, and gamma conversion are outside the
  procedural-disc feature and should stay separate.

## Suggested implementation order

1. Add a cheap spectral-transmittance function to the AyaneStorm-owned
   procedural-sun module, parameterized by normalized elevation optical depth.
   Use it to derive disc and halo colours from the live EEP sunlight colour.
2. Replace the halo's rational radial falloff with a fitted forward-scattering
   profile while retaining its edge fade, additive blend, depth test, and
   zero glow-mask alpha.
3. Optionally make halo radius/concentration vary with optical depth. Avoid new
   controls initially; validate sensible fixed physical coefficients first.
4. Compare HDR and non-HDR at elevations around 5, 2, 0, and the scale-aware
   cutoff, including opaque and transparent EEP sun textures.

The first item is the highest-value, lowest-risk reuse. It can improve the
sun's realistic white-yellow-orange-red progression without importing the
reference renderer or changing EEP scene lighting.

## Proposed extinction-light improvement

The current implementation is not physical extinction. `mixSunsetColor()`
linearly blends red and green, while blue uses `pow(warmth, 0.25)` to disappear
earlier. This avoids pink but cannot respond naturally to atmospheric path
length or EEP atmospheric character.

`LLSettingsSky::getLightTransmittance()` already evaluates Beer-Lambert
transmittance, but its extinction coefficients are the legacy `blue_density +
haze_density` values and its input is a linear viewer-space distance. It does
not calculate the much longer slant path through the atmosphere when the sun is
near the horizon. It should therefore not be called with an invented distance
as a shortcut for the procedural sun.

A better low-cost model is:

1. Derive relative optical air mass from sun elevation. The Kasten-Young
   approximation is suitable from ordinary daylight down to the apparent
   horizon and remains finite there (approximately 38 air masses at zero
   degrees).
2. Evaluate RGB Beer-Lambert transmittance, `T = exp(-tau_rgb * air_mass)`,
   using fixed Earth-like Rayleigh optical depths plus a neutral or mildly warm
   aerosol term.
3. Normalize `T` by its largest component when deriving chroma, so extinction
   changes colour without unintentionally extinguishing the already
   independently controlled emissive disc. Keep radiance under the existing
   brightness and visibility controls.
4. Combine the transmittance chroma multiplicatively with normalized live EEP
   sunlight colour. Treat `ASProceduralSunFinalColor` as an artistic bias or
   strength endpoint rather than the primary generator of sunset colour.
5. Derive halo colour from the light removed from the direct beam (or a bounded
   approximation of `1 - T`) and its profile from forward Mie scattering. This
   links a warmer/dimmer direct disc to a stronger aerosol aureole.

This calculation belongs in the AyaneStorm-owned `asproceduralsun` module and
costs only a few scalar powers/exponentials once per render-parameter update,
not per scene pixel. The shader continues receiving final disc and halo colours.

Important safeguards:

- clamp the air-mass input at the apparent horizon; do not extrapolate the
  approximation below its valid range;
- retain the existing scale-aware lower-edge visibility cutoff;
- do not alter the EEP sunlight colour used for world lighting, shadows, or
  other viewers;
- do not apply a second tone mapper in the sun shader;
- validate unusual fantasy EEP colours, because strict physical extinction
  should assist rather than erase authored intent.

## Broad horizon-light layer

The visually strong light along the horizon in `sunset3.shader` is distinct
from both its sun disc and a small sun-centred halo. It is a wide atmospheric
scattering field formed by high optical depth near the horizon, wavelength
dependent Rayleigh extinction, and forward Mie scattering around the solar
azimuth. The present procedural-sun halo is limited to at most eight disc radii
and cannot reproduce this sky-wide band.

The appropriate AyaneStorm implementation is a new AS-owned sky-dome module and
shader. Insert its draw between `renderSkyHazeDeferred()` and
`renderHeavenlyBodies()` in `LLDrawPoolWLSky::renderDeferred()`. At that point:

- the EEP sky already supplies the base colour;
- the procedural horizon field can blend over it;
- the sun and moon remain sharp foreground celestial elements;
- clouds render later and therefore occlude the horizon field naturally;
- no scene-depth or post-process replacement is required.

The fragment calculation should use world/view directions, never screen-space
mouse coordinates. Inputs are the live EEP sun direction and sunlight colour,
camera-relative sky direction, sun elevation, and viewer-local controls. A
compact model can combine:

1. a horizon optical-depth profile based on view elevation;
2. RGB Beer-Lambert extinction for the broad yellow/orange/red band;
3. a Henyey-Greenstein-like Mie phase lobe from `dot(view_dir, sun_dir)`;
4. a broader Rayleigh/twilight term spanning much of the horizon;
5. smooth fades above the configured band and as the sun descends past the
   useful twilight range.

Two compositing modes are feasible:

- **Additive:** premultiplied radiance is added to the EEP sky. This is the
  safest default, preserves authored skies, and most closely describes extra
  scattered light. Strength must be exposure-aware and bounded to avoid HDR
  clipping.
- **Replace horizon:** ordinary alpha blending uses a vertically and angularly
  varying coverage. Coverage reaches one only in the intended low band and
  fades to zero above it, allowing the generated field to override an
  unattractive EEP horizon without replacing the complete sky. This is an
  artistic override, not physical radiative transfer.

Do not implement replacement by editing EEP settings or globally disabling EEP
atmospherics. Those approaches would also change scene fog, object lighting,
water, clouds, and parcel/region intent. The isolated dome layer provides the
desired visual override only where the sky background is drawn.

Star visibility may need to be attenuated by the procedural horizon coverage,
because stars are currently drawn after heavenly bodies and before clouds.
Reflection-probe and cube-snapshot behavior should initially follow the EEP sky
policy deliberately: additive horizon light can participate in radiance
captures, while an explicitly viewer-local replacement may be excluded until
its reflections have been validated.
