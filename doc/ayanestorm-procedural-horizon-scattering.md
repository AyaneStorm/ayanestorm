# AyaneStorm procedural horizon scattering

## Purpose

The optional viewer-local horizon layer adds the broad atmospheric illumination
seen around realistic sunrise and sunset without modifying the active EEP asset.
It is independent from the procedural sun disc and its compact billboard halo.

The feature is disabled by default. It is available in the Environment Effects
**Horizon** tab and the standalone **Horizon Lighting** floater.

## Render design

The AS-owned sky-dome pass renders immediately after EEP sky haze and before the
sun, moon, stars, aurora, and clouds. This order keeps celestial discs sharp and
lets clouds naturally occlude the generated horizon field. HDRI preview and the
background-isolation mode do not receive the effect. Main-view, radiance, and
irradiance cube renders do, so enabled horizon light participates in both
specular reflections and image-based ambient lighting.

The shader is analytic and has no ray-marching loop or texture lookup. For each
sky direction it calculates:

1. a soft vertical band around the apparent horizon;
2. an approximate slant optical depth;
3. RGB Beer-Lambert transmittance with Earth-like relative Rayleigh
   coefficients;
4. the warm light removed from the direct spectrum;
5. a broad Rayleigh phase term;
6. a normalized Henyey-Greenstein forward Mie lobe;
7. sun-relative azimuth falloff;
8. a smooth sun-elevation activation window.

Camera height lowers the apparent horizon using a clamped curvature angle based
on the live EEP dome radius. The same angle is added to solar elevation for the
activation window, so a sun revealed by a lower horizon behaves like a higher
sun. The band is soft on both sides and has no lower cutoff that can bisect the
solar disc.

The live EEP sunlight colour remains the source chroma. The artistic tint is
mixed multiplicatively, so unusual EEP colours remain influential.

The AS horizon and procedural sun use the zero-offset EEP direction rather than
WindLight's legacy 50-metre celestial displacement, keeping both viewer-local
effects in the same coordinate convention.

## Compositing and G-buffer behavior

**Additive (brighten EEP)** adds generated radiance without attenuating the
existing sky. It is retained as an artistic option and can only brighten.

**Atmospheric blend** is the default and uses two dome draws. The first applies
RGB Beer-Lambert extinction as `EEP * transmittance`; the second adds Rayleigh
and Mie in-scattering. Blend opacity scales both terms. Separate alpha factors
preserve the post-process glow mask, and white/zero writes preserve every
non-radiance G-buffer target during the multiply/add passes.

**Replace EEP horizon** alpha-blends the generated band over EEP. It is retained
as a stronger artistic override and uses blend opacity as its coverage limit.

The generated field does not change EEP direct lighting, shadows, scene fog,
water-light selection, or environmental asset data.

## Stars

The SL-owned star shaders remain unchanged. In additive mode the brighter
horizon reduces star contrast naturally. The layer intentionally does not add
spatial star attenuation, avoiding an upstream shader modification and keeping
the established celestial render order intact.

## Controls

- Enable procedural horizon light
- Additive, atmospheric, or replace compositing
- Light strength and blend opacity
- Band height and upper softness
- Rayleigh and aerosol strengths
- Mie forward concentration and azimuth spread
- Artistic tint and tint mix
- Start and end solar elevations

Every value is viewer-local, applies live, and is clamped again at shader
upload. Adjustable values have individual reset buttons; the enable checkbox
does not need one. Blend opacity is disabled in additive mode.

## Runtime validation

The user performs builds. After a successful build, validate:

- disabled mode against the unmodified EEP image;
- additive, atmospheric, and replace modes with bright, dark, saturated, legacy-haze, and
  current-atmosphere EEP skies;
- solar elevations `15`, `12`, `5`, `2`, `0`, `-6`, `-12`, and `-15` degrees;
- multiple solar azimuths and camera pitches;
- HDR and non-HDR, above and below water;
- opaque and transparent EEP sun textures;
- cloud occlusion and acceptable star contrast along the bright horizon;
- mirrors plus radiance and irradiance reflection captures;
- day-cycle, parcel, and region EEP transitions;
- HDRI preview and background-isolation exclusion;
- graceful fallback if the optional shader fails to compile.

Look specifically for sky-dome seams, a hard upper band, horizon clipping, HDR
clamping, altered glow-mask alpha, or invalid categorical G-buffer metadata.
