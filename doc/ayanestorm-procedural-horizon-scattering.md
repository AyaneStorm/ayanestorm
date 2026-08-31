# AyaneStorm procedural horizon scattering

## Purpose

The optional viewer-local horizon layer adds the broad atmospheric illumination
seen around realistic sunrise and sunset without modifying the active EEP asset.
It is independent from the procedural sun disc and its compact billboard halo.

The feature is enabled by default. It is available in the Environment Effects
**Horizon** tab and the standalone **Horizon Lighting** floater.

The 2026-08-31 tuned panel state is now the shipped default: enabled, Additive
mode, band height `19.5`, softness `16`, Mie anisotropy `0.19`, tint mix `1`,
cloud strength `1.15`, water reach/intensity `1.5`/`1`, and sky haze
reach/intensity `1.16`/`0.5`. Controls absent from the saved override file keep
their existing defaults.

**Two separate floaters embed `panel_as_horizon_settings.xml`** —
`floater_as_horizon_settings.xml` (the standalone floater) and
`floater_as_environment_effects.xml` (the tabbed "Environment Effects"
floater's Horizon tab) — and both had to be resized whenever the panel
grew a new control row. The standalone floater was kept in sync
throughout this feature's development, but the Environment Effects
floater's fixed-height, non-resizable outer floater and tab container were
missed for several rows (including the whole Sky horizon haze intensity
slider), silently clipping them with no scrollbar to reveal what was cut
off. Both are now sized to the panel's current height (`680`).

**Temporary debug hook (as of this writing, to be removed):** sky horizon
haze's gating chain (`haze_core * away_from_sun * as_horizon_sun_fade`)
went invisible after adding the azimuth gate, and it isn't yet known which
term collapsed it to zero. Rather than guess a further threshold, the sky
shader has a debug branch triggered by pushing `ASHorizonScatteringSkyHazeStrength`
to its maximum value (`2.0`): the sky's horizon band renders as flat
additive R=`haze_core` (elevation gate), G=`away_from_sun` (azimuth gate),
B=`as_horizon_sun_fade` (activation window) instead of the real haze
effect, so their actual on-screen values can be read off a screenshot.
Once captured, the gating chain should be recalibrated against that data
and the debug branch deleted. (Water horizon fog's equivalent debug hook
has already served its purpose — it revealed that `atten` saturates
almost immediately at normal view distances — and has been replaced by a
real fix; see below.)

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

## Cloud scattering tint

When horizon scattering is enabled, an AS-owned optional fragment shader is
selected for the viewer's normal EEP cloud draw. It
uses the same live sunlight colour, artistic tint, strength, blend opacity, and
solar activation window as the horizon. The EEP cloud vertex lighting supplies
the sun-facing signal, while the original noise and self-shadow fields keep
dense interiors darker and warm exposed regions, following the reference
shader's attenuated ambient plus forward in-scattering principle. A bounded
multiple-scattering approximation lifts and warms dense interiors without
reducing their EEP opacity. An AS-owned derivative of the viewer cloud vertex
stage exports true angular view elevation and sun alignment; the stock altitude
blend is only a visibility fade and cannot classify low versus overhead clouds.
Warm transport is confined to long paths within roughly 5–30 degrees of the
horizon, higher clouds fade back to EEP white/ambient lighting, and normal
self-shadow keeps the overhead ceiling dark. Cloud in-scattering is added
independently of the already-dark EEP base colour; otherwise dense clouds would
incorrectly reject nearly all sunset radiance.

The shader reproduces the standard cloud base colour before tinting and does
not alter cloud density or opacity. It preserves all non-radiance G-buffer
targets. The original Linden cloud shader is selected unchanged when
the Horizon feature is disabled, inactive by elevation, unavailable after a
shader failure, or excluded by HDRI/background-isolation rendering.

## Water horizon fog

### 2026-08-31 corrective handoff

CPU-side water-fog calculations and uniform uploads live in the AS-owned
`aswaterhorizonfog.cpp` module. The upstream-owned draw pool contains only the
module include and a single `ASWaterHorizonFog::uploadUniforms()` call.

The sky-dome dip must not be used to position fog on the water shader.
Water is rendered as a flat, finite plane; in testing, the diagnostic band
around the computed dome horizon did not intersect a single water fragment.
Water fog uses the flat plane's view elevation. Level (`0` radians) is the
planar horizon and `max(-view_elevation, 0)` is angular distance below it.
This is independent of camera far clip and progressive world loading. The
temporary green diagnostic override was removed.

The independent intensity slider alone scales peak opacity. Reach maps
linearly from `0` to `20` degrees; analytic angular subpixel coverage keeps
`0.01` barely visible. Water fog multiplicatively
darkens the existing water color; blending toward atmospheric `additive` was
removed because it made the horizon gray and brighter.

Sky haze no longer uses an azimuth exclusion around the sun. Intersecting that
cone with the elevation falloff bent the gradient into curved lobes; the haze
mask is now elevation-only and therefore horizontal.

Very small water reach values use analytic subpixel coverage derived from
`fwidth(below_horizon)`. A geometrically subpixel interval otherwise still sampled
full intensity on the final water row, creating a harsh bar at `0.01`.
Fog fades from zero near the sun azimuth to full strength by 45 degrees. This
preserves all reflected sun lighting, including reflection-probe/radiance
contributions that cannot be reconstructed from the punctual term alone.

The prior implementation also squared its decreasing smoothstep falloff while
claiming this broadened the gradient. Squaring values in `[0, 1]` reduces the
middle of the curve and visually concentrates the effect at the horizon. Sky
haze now uses the smoothstep result directly; water uses a direct distance
smoothstep. This removes the artificial hard strip seen in the handoff
screenshot.

`class3/environment/waterF.glsl` carries an upstream, commented-out
distance-haze mix (`color = mix(color, additive * water_haze_scale, (1 -
atten))`) intended to restore distant water brightening; it was left
disabled. An AS-owned uniform `as_horizon_fog_strength` (bound from
`ASWaterHorizonFogStrength`, 0.0 default, clamped `[0, 4]`) reuses
`additive` from that same commented-out line to blend `color` toward it at
natural brightness (not `water_haze_scale`, which is tuned for the
unrelated legacy brightening effect and would blow the band out toward
white), gated behind `ASHorizonScatteringEnabled` like the rest of this
feature set.

**Also gated by sun elevation, added after the user reported seeing the
fog at full midday sun.** The geometric horizon-anchoring below only
answers "is this pixel near the horizon," never "is it sunset" — nothing
upstream of it depended on time of day at all, so the effect fired
identically regardless of sun elevation. `ASHorizonScattering::getSunFade()`
already computes exactly this window (fading in as the sun approaches
`ASHorizonScatteringStartElevation` from above and fading out past
`ASHorizonScatteringEndElevation` below the true horizon) for the sky-side
band, but is private to `ashorizonscattering.cpp`; reproduced in
`LLDrawPoolWater::renderPostDeferred` (linear `llclamp` ramps in place of
that file's private `smoothStep` helper — close enough in shape for a
gating multiplier, not worth exporting the helper for) and multiplied
directly into the uploaded `as_horizon_fog_strength` CPU-side, so the
whole effect self-disables outside the sunset/sunrise window exactly like
sky horizon haze already does, without touching the fragment shader's
geometry math at all.

**The falloff is anchored to the TRUE geometric horizon, not a proxy for
it.** The horizon is a fixed angle below level (`as_horizon_water_dip`,
radians), caused by camera height above the domed world — the same
`acos(domeRadius / (domeRadius + cameraHeightAboveWater))` formula
`ASHorizonScattering::getHorizonDip()` already uses for the sky-side band,
reproduced in `LLDrawPoolWater::renderPostDeferred` (not shared as a
function, to avoid coupling the draw pool to that translation unit) and
uploaded as `as_horizon_water_dip`. In the fragment shader, this pixel's
own view-ray elevation (`asin(dot(viewVec, up))`, both already computed
earlier in `main()`) is compared against `-as_horizon_water_dip`:
```glsl
float view_elevation = asin(clamp(dot(viewVec, up), -1.0, 1.0));
float horizon_elevation = -as_horizon_water_dip;
float below_horizon = max(horizon_elevation - view_elevation, 0.0);

float reach_radians = radians(2.0);
float fog_falloff = 1.0 - smoothstep(0.0, reach_radians, below_horizon);
float intensity = clamp(as_horizon_fog_strength / 4.0, 0.0, 1.0);
fog_falloff *= intensity;

float sun_glint = clamp(max(punctual.r, max(punctual.g, punctual.b)), 0.0, 1.0);

vec3 view_horizontal = normalize(viewVec - up * dot(viewVec, up));
vec3 sun_horizontal_dir = normalize(vary_light_dir - up * dot(vary_light_dir, up));
float water_azimuth_angle = acos(clamp(dot(view_horizontal, sun_horizontal_dir), -1.0, 1.0));
float away_from_sun = smoothstep(radians(8.0), radians(35.0), water_azimuth_angle);

fog_falloff *= (1.0 - sun_glint) * away_from_sun;

color = mix(color, additive, fog_falloff * step(0.0001, as_horizon_fog_strength));
```
The transition band's WIDTH (`reach_radians`, fixed at `2°`) and its
OPACITY (`intensity`, driven by strength) are now two separate concepts.
Getting here took three wrong versions that each conflated them:

1. `mix(0.3°, 8.0°, ...)`, scaling the smoothstep *window* linearly with
   strength but clamping its minimum to a fixed `0.3°` floor to avoid a
   hard line. That floor defeated the slider itself: `strength = 0.01`
   and `strength = 1.0` both landed at or barely above that same `0.3°`
   floor, so two screenshots taken at those settings looked nearly
   identical (confirmed by the user) — a thick band regardless of
   strength.
2. A `fwidth(below_horizon)`-based screen-space floor on the same window,
   meant to guarantee a few pixels of softness regardless of camera
   distance without capping the low end of the slider. Wrong in the
   *opposite* direction: `fwidth()` measures how much `below_horizon`
   changes between adjacent screen pixels, and right at the true horizon
   perspective compresses a huge distance range into very few screen
   pixels — so `fwidth(below_horizon)` is actually LARGE there, not small.
   It silently dominated `reach_radians` at every low strength setting,
   so `strength = 0.01` still showed a thick, unchanging band.
3. A cubed linear window (`pow(strength / 4.0, 3.0) * 8°`, no floor of any
   kind), meant to make the window shrink much faster than linearly at low
   strength. This shrank the window too far: at `strength = 0.01` the
   window became smaller than a single pixel's worth of `below_horizon`
   variation, so the smoothstep degenerated into a hard, ~1-pixel step —
   a HARSH band regardless of strength (confirmed by the user: "not even
   a linear scale, it's a harsh band"), the opposite failure mode from
   attempt 1.

All three tried to make the WINDOW itself shrink at low strength, but a
transition window that's allowed to shrink toward zero width will always
eventually hit a size where it renders as a hard edge rather than
disappearing smoothly — there's no width where a `smoothstep` genuinely
looks like "barely visible" rather than either "still thick" or "hard
line." The actual fix keeps the window's width **fixed** at `2°` (wide
enough to always render as visually soft, at any camera distance or
strength) and lets `intensity` fade the whole effect toward fully
transparent instead — `strength = 0.01` now shows the same soft-edged
band shape as `strength = 4.0`, just blended in at a small fraction of
opacity, which is what "barely visible" actually means.

One more issue surfaced on the first real test of the geometric version:

- **Overriding the sun's reflection glint.** The fog was darkening the
  bright specular reflection streak along with the rest of the water,
  which shouldn't happen — the reflection is its own light, not part of
  the ambient water color the fog is meant to mute. `punctual` (the sun's
  specular highlight term, already computed earlier in `main()` via
  `pbrPunctual`) is bright exactly where that glint lands; gating
  `fog_falloff` down wherever `punctual` is already strong protects the
  glint while leaving the surrounding water's fog untouched.
- **No azimuth dependency at all.** The `punctual` gate above only
  protects the thin glint streak itself; it did not keep the broader area
  of water near the sun's azimuth brighter the way the reference does and
  the way sky horizon haze's `away_from_sun` already does. Added the same
  fixed 8°→35° azimuth falloff as the sky shader, computed via vector
  rejection (`viewVec - up * dot(viewVec, up)`, projecting onto the plane
  perpendicular to `up`) rather than dropping a fixed axis the way the sky
  shader's `ray.xz`/`sun.xz` does — that shortcut only works there because
  that shader's directions are already in a space where Y is up, which
  isn't guaranteed for this shader's view-space `up`. Both shaders now
  agree on how wide "near the sun" is (the same 8°/35° constants), so the
  two sides read consistently.

`below_horizon` is `0` exactly at the true horizon and grows with angular
distance below it, so `ASWaterHorizonFogStrength` controls **reach** (how
many degrees below the horizon the gradient extends — `0.05°` at minimum
strength, up to `8°` at the `4.0` ceiling) rather than just intensity:
raising it visibly widens the affected band outward from the horizon
toward the camera, instead of changing how hard a fixed-shape curve pulls
everywhere at once.

Two earlier signals were tried and were structurally wrong, not just
mistuned — worth recording since either could look tempting again:

- **`atten` (distance/depth attenuation).** A debug visualization (water
  rendered as flat grayscale of `1 - atten`) showed the whole visible sea
  reading as ~white (saturated near `1.0`) except a thin strip right next
  to the camera — `atten` collapses to its minimum almost immediately at
  normal SL view distances. No falloff curve on top of it can recover a
  gradient that isn't there; every curve tried (`pow(x, 0.35)`,
  `smoothstep` windows at various thresholds) either affected nearly the
  whole sea or, if narrowed enough to avoid that, went invisible because
  the input barely varies in the relevant range.
- **`dot(viewVec, up)` pinned to a fixed threshold near `0`.** Ignores
  camera height: the true horizon dips below flat-level as the camera
  rises (a real perspective effect on a curved/domed world), so a
  camera-height-blind angular threshold is only correct at exactly water
  level and drifts wrong as soon as the avatar's eye height changes.

### Two independent sliders: reach and intensity

Per explicit user request, water horizon fog is controlled by two
separate sliders rather than one:

- **`ASWaterHorizonFogStrength`** ("Water horizon fog reach", `0..4`,
  default `0.0`) — how far below the horizon the fog gradient extends.
  `0.0` disables the whole effect outright (forced via `fog_falloff *=
  step(0.0001, as_horizon_fog_strength)`), independent of the intensity
  slider's own value.
- **`ASWaterHorizonFogIntensity`** ("Water horizon fog intensity", `0..1`,
  default `0.5`) — peak opacity/darkness of the gradient once reach is
  nonzero. `0.0` also makes the whole effect a no-op regardless of reach.
  Defaults to `0.5` (not `0.0`) so that once a user raises reach off its
  own `0.0` default, the effect is immediately visible at a moderate
  strength rather than needing both sliders touched to see anything.

Both defaulting to values that don't individually guarantee a no-op
(`reach = 0.0` alone would; `intensity = 0.5` alone would not) required an
explicit `step(0.0001, as_horizon_fog_strength)` gate late in the
fragment shader, separate from the `intensity` multiply.

**The gradient's actual shape went through one more wrong version, caught
on the first real screenshot test at `reach = 2.0`.** The first
reach/intensity split used `1.0 - smoothstep(reach, reach + width,
below_horizon)` — a small FIXED transition width whose starting point
moved outward with `reach`. That produces a flat, fully-opaque plateau
from the horizon out to `reach`, fading only in the last `width` degrees
at the far edge — not a gradient peaking at the horizon line as the user
explicitly wants, and not genuinely subtle at low reach either, since even
a small `reach` still yields a fully-opaque (if narrow) band rather than
something barely visible. Fixed by making the smoothstep span the
**entire** reach distance instead of a fixed sliver at its far end:
```glsl
float reach_radians = max(radians(mix(0.0, 10.0, clamp(as_horizon_fog_strength / 4.0, 0.0, 1.0))), radians(0.05));
float fog_falloff = 1.0 - smoothstep(0.0, reach_radians, below_horizon);
```
`fog_falloff` is `1.0` (peak) exactly at the horizon (`below_horizon ==
0`) and smoothly fades to `0.0` by `below_horizon == reach_radians`. This
gets both properties right at once: raising `reach` widens the visible
gradient (as before), AND is what makes a small `reach` genuinely subtle —
the peak-to-zero fade happens almost immediately rather than producing a
narrow-but-still-fully-opaque band. The `radians(0.05)` floor only
prevents the smoothstep's own span from ever hitting exactly zero width;
it is not user-adjustable and has no visible effect given the `step()`
gate above already fully suppresses `reach == 0`.

### Reach at low values: angle vs. screen pixels vs. perceptual gradient

A real test at `reach = 0.01` (the slider's near-minimum) still showed a
harsh, roughly 100-pixel-tall band, not the intended near-single-pixel
sliver — even with `reach_radians`'s floor already as small as `0.05°`.
This went through two more wrong attempts before landing on the current
one:

1. **`fwidth(below_horizon)`-based pixel-space reach.** The idea:
   `fwidth()` measures how much `below_horizon` changes between adjacent
   pixels *at this fragment*, so multiplying a genuine pixel count by that
   gives "reach in radians that corresponds to N screen pixels, here."
   This is unreliable across the screen: `fwidth()` is a true per-fragment
   derivative, not a constant, so "reach = 1 pixel" only held at whichever
   point's own derivative happened to be sampled, and could balloon
   elsewhere on the water where the local derivative was naturally larger
   — reproducing a wide, harsh-looking band through a different
   mechanism than the original bug. Applied to the identical
   `smoothstep(0, reach, elevation)` shape on the sky side too
   (`fwidth(absolute_elevation)`), this additionally introduced visible
   horizontal banding across the *entire* sky (not just near the
   horizon) — `fwidth()` evaluated inconsistently across the sky dome
   mesh's triangle/seam boundaries, and that inconsistency became visible
   as discrete stripes. Confirmed new (not present with the feature
   disabled) by the user, then reverted on both sides.
2. **True screen-space Y distance from the horizon line.** Considered as
   the theoretically correct fix (comparing `gl_FragCoord.y` against the
   horizon's own projected screen Y), but requires passing a projection
   matrix into these shaders and reprojecting per-frame — a larger,
   riskier change not attempted given the pattern of repeated wrong fixes
   already in this session.

**Landed on a plain angular reach again, calibrated by eye against a real
measurement** (an earlier `below_horizon` debug view showed `0.02 rad`
already covering most of the visible sea, so the floor/ceiling here are
set well below that) **plus a perceptual fix for the remaining "still
looks like a hard band" complaint**: the underlying opacity curve
(`1.0 - smoothstep(0, reach, elevation)`) IS a mathematical gradient, but
when the blend target color (`additive` for water, the haze tone for sky)
contrasts strongly with the base color, the eye perceives the blend as
"switched" once opacity crosses roughly 15-20%, well before the math
reaches `1.0` — compressing the *perceived* transition into a narrow
slice near the peak and reading as a hard edge even though the raw curve
is smooth. Squaring the falloff spreads more of the visible transition
across the reach distance instead of concentrating it at the horizon:
```glsl
// water (below_horizon), sky (absolute_elevation) — same shape both sides
float reach_radians = mix(0.001, 0.15, clamp(strength / max_strength, 0.0, 1.0));
float raw_falloff = 1.0 - smoothstep(0.0, reach_radians, elevation_term);
float falloff = raw_falloff * raw_falloff;
```

## Horizon seam blur (attempted and abandoned)

A screen-space post-process pass (`ASHorizonHaze`) was attempted to soften
the hard rasterization edge where the sky dome meets the water plane, by
detecting depth-buffer discontinuities per-pixel and blurring across them —
modelled after `sunset3.shader`'s final compositing step, which blends two
independently rendered sky/sea colours over a narrow `smoothstep` window in
view-ray elevation (`mix(color, seacolor, smoothstep(..., -camera_vector.y))`).

Three real, distinct bugs were found and fixed during development (wrong
reserved uniform name — `LLShaderMgr::DEFERRED_DIFFUSE` resolves to
`"diffuseRect"`, not `"diffuseMap"`; sky-dome VBO segment seams misread as
real depth boundaries because `LLDrawPoolWLSky::endDeferredPass()` clears
depth after the sky pass; and the shader unconditionally forcing alpha to
`1.0`, which corrupted `glowExtractF.glsl`'s `max(col.a, glow_trigger)`
bloom gate). After all three fixes, the pass still could not reliably find
the horizon: **near the actual sky/water line, distant water's depth
converges toward the sky's depth (both heavily compressed by perspective
near the far plane), while nearby water WAVES have much larger depth deltas
between neighboring pixels than the horizon itself does** — so any
depth-delta threshold that's loose enough to catch the horizon also fires
constantly across ordinary choppy water, and any threshold tight enough to
ignore wave noise also misses the horizon entirely. This is a structural
property of perspective depth near the far plane, not a tuning problem.

**The feature was removed.** The reference look (see the Purpose section's
motivating screenshot) does not actually come from blurring the seam — it
comes from colour/haze grading on *both* sides of the line meeting in a
shared band (sky pales and warms toward the horizon; water darkens and
desaturates toward it). Water horizon fog above already does the water
side; sky horizon haze below completes the sky side without the
depth-based seam-detection problem above.

## Sky horizon haze

The sky-side companion to water horizon fog, in `asHorizonScatteringF.glsl`.
Two independent uniforms, mirroring water horizon fog's own reach/intensity
split (see above) per explicit user request: `as_horizon_sky_haze_strength`
(bound from `ASHorizonScatteringSkyHazeStrength`, `0..2`, `0.0` default,
"reach") and `as_horizon_sky_haze_intensity` (bound from
`ASHorizonScatteringSkyHazeIntensity`, `0..1`, `0.5` default, "intensity")
together darken/mute this feature's radiance and the underlying EEP sky's
own extinction near the horizon, gated by three independent `0..1`
"where" factors — elevation, sun-azimuth, and the base feature's own
sun-elevation activation window:
```glsl
float haze_reach = max(radians(mix(0.0, 6.0, clamp(as_horizon_sky_haze_strength / 2.0, 0.0, 1.0))), radians(0.05));
float haze_core = 1.0 - smoothstep(0.0, haze_reach, absolute_elevation);
float haze_intensity = clamp(as_horizon_sky_haze_intensity, 0.0, 1.0);

float haze_azimuth_angle = acos(clamp(dot(ray_horizontal, sun_horizontal), -1.0, 1.0));
float away_from_sun = smoothstep(radians(8.0), radians(35.0), haze_azimuth_angle);

float haze_gate = clamp(haze_core * away_from_sun * as_horizon_sun_fade, 0.0, 1.0);
float haze_amount = haze_gate * haze_intensity;

float haze_luma = dot(base_color, vec3(0.299, 0.587, 0.114));
vec3 haze_color = mix(vec3(haze_luma), base_color, 0.4) * 0.15;
radiance = mix(radiance, haze_color, clamp(haze_amount, 0.0, 1.0));
extinction *= mix(vec3(1.0), vec3(haze_luma * 0.4), clamp(haze_amount, 0.0, 1.0));
```
- **`haze_core`** is `1.0` (peak) exactly at the horizon
  (`absolute_elevation == 0`) and fades to `0.0` by `haze_reach` — a
  gradient peaking at the horizon line, not a flat plateau (see the
  retrospective below for why that distinction mattered). `haze_reach`
  grows with the reach slider (up to `6°` at its `2.0` ceiling);
  `haze_intensity` comes from its own separate slider/uniform, entirely
  independent of reach. Reach defaulting to `0.0` fully disables the whole
  `if (as_horizon_sky_haze_strength > 0.0001)` block regardless of
  intensity's own `0.5` default — unlike water horizon fog, no extra
  `step()` gate was needed here since the block is a real `if`, not always
  executing with a zero-width window.
- **`away_from_sun`** keeps the horizon bright right at the sun's own
  azimuth (its own light/reflection isn't muted) and mutes away from it —
  a fixed `8°→35°` cone, independent of `ASHorizonScatteringAzimuthSpread`
  (that setting shapes the unrelated glow cone, and reusing it directly
  here was an early mistake — see below).
- **Both `radiance` (this shader's own added light) and `extinction` (the
  multiplicative term that darkens the underlying EEP sky) are pulled
  toward the haze tone.** Darkening only `radiance` left EEP's own
  brightness fully visible regardless of haze strength — see below.
- `extinction *=` also runs in **Additive** blend mode as an intentional,
  narrowly-scoped exception to Additive's usual brighten-only behavior,
  per explicit user request (see below).

Applied identically in both of `ASHorizonScattering`'s render passes
(`as_horizon_pass == 0` extinction draw and `== 1` radiance draw), before
either output is finalized, so both compositing draws stay consistent.

### Retrospective: getting here took five real, distinct bugs

1. **Wrong azimuth gate.** An early version derived `away_from_sun` from
   the existing `azimuth` term (`away_from_sun = 1.0 - azimuth`), which is
   shaped by `ASHorizonScatteringAzimuthSpread` — an artistic control for
   the *glow* cone's width (100° default, up to 180°). At that default,
   `azimuth` stayed well above 0 across most or all of the visible
   horizon, so `1.0 - azimuth` was near-zero almost everywhere and
   silently killed the whole haze effect regardless of strength. Found by
   checking the user's actual saved value against the `smoothstep` shape
   (the key was absent, meaning the wide default applied) rather than by
   a debug build — the horizon's location itself was never in question.
   Fixed with the haze's own fixed `8°→35°` cone, independent of the glow
   spread control.
2. **Diluted strength.** `haze_core * away_from_sun * as_horizon_sun_fade`
   is a product of three gates that rarely approaches `1` even at full
   coverage; multiplying strength directly into that same product only
   ever scaled an already-small number, so the strongest slider setting
   still looked weak.
3. **Radiance-only darkening.** `radiance = mix(radiance, haze_color,
   haze_amount)` only darkens this shader's own small ADDED light —
   `extinction` (the multiplicative term that actually attenuates the
   underlying EEP sky) passed through unchanged, so EEP's own brightness
   stayed fully visible regardless of haze strength, and only this
   feature's thin additive contribution ever got muted. Water horizon fog
   never had this problem, since it darkens the whole composited `color`,
   not a delta on top of it. Fixed by also pulling `extinction` toward the
   haze tone.
4. **Additive mode silently a no-op.** The `as_horizon_pass == 0`
   extinction draw that writes `extinction` to screen was only issued in
   **Atmospheric** blend mode (`ashorizonscattering.cpp`); in Additive or
   Replace mode that draw call didn't happen at all, so the extinction fix
   above had no path to reach the screen there. The user's saved
   `ASHorizonScatteringBlendMode` was `0` (Additive), so this made sky
   horizon haze silently a no-op for them regardless of strength — the
   actual root cause of "mostly absent effect," not a remaining tuning
   issue. Per explicit user direction to keep Additive (their preferred
   look) rather than switch modes, the extinction draw is now also issued
   in Additive mode whenever sky horizon haze is active — a consistency
   fix as much as a feature request, since water horizon fog (a separate
   draw pool, unaffected by this mode gating) already worked in every
   blend mode. Enabling this required forcing the *base* scattering
   band's own extinction to a no-op in Additive mode
   (`as_horizon_blend_mode == 0 ? vec3(1.0) : ...`), so turning on sky
   horizon haze doesn't also silently introduce the unrelated base band's
   own darkening into Additive mode as a side effect — only the haze
   block's own `extinction *=` should apply there.
5. **Flat plateau instead of a gradient peaking at the horizon.** The
   reach-scaling fix initially used `1.0 - smoothstep(haze_reach,
   haze_reach + width, absolute_elevation)` — a small fixed transition
   width whose starting point moved outward with strength. That produces
   a flat, fully-opaque plateau from the horizon out to `haze_reach`,
   fading only in the last `width` degrees at the far edge — confirmed
   wrong by a real screenshot at `strength = 2.0` showing uniform darkness
   across the band's whole height rather than a gradient, and not
   genuinely subtle at low reach either (a small reach still produced a
   fully-opaque, if narrow, band). Fixed the same way as the identical bug
   on the water side: a single smoothstep spanning the entire reach
   distance, peaking at the horizon and fading smoothly to zero by
   `haze_reach` — the version shown above.

## Controls

- Enable procedural horizon light
- Additive, atmospheric, or replace compositing
- Light strength and blend opacity
- Band height and upper softness
- Rayleigh and aerosol strengths
- Mie forward concentration and azimuth spread
- Artistic tint and tint mix
- Start and end solar elevations
- Cloud tint strength (independent multiplier on the horizon-scattering
  cloud tint; defaults to 1.0, reproducing the original fixed strength)
- Water horizon fog reach (how far outward from the horizon the water fog
  band extends; defaults to 0.0, disabling the effect regardless of the
  intensity slider)
- Water horizon fog intensity (opacity/darkness of the water fog blend
  toward sky haze color, independent of reach; defaults to 0.5, but has
  no visible effect until reach is raised off its own 0.0 default)
- Sky horizon haze reach (how far outward from the horizon the sky
  darkens/desaturates toward a haze tone, meeting water horizon fog;
  defaults to 0.0, disabling the effect regardless of the intensity
  slider)
- Sky horizon haze intensity (opacity/darkness of the sky haze blend,
  independent of reach; defaults to 0.5, but has no visible effect until
  reach is raised off its own 0.0 default; in Additive blend mode this is
  an intentional, narrowly-scoped exception to Additive's usual
  brighten-only behavior)

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
