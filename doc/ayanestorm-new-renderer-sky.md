# New renderer: sky study (P4)

Status: study agreed (decisions at the end), nothing written.

User's goals, 15 November 2026:
- support EEP (windlight) skies;
- a modern sky from the start, especially at sunrise and sunset;
- volumetric clouds if they make sense.

## What decides the method

1. **EEP drives everything.** Regions, parcels and users set skies and day cycles, and day cycles blend between keyframes continuously. The sky's inputs can therefore change every frame, so nothing may need a long precompute after a change.
2. **EEP has two sets of sky values:**
   - **Legacy windlight values:** blue horizon and density, haze horizon and density, density and distance multipliers, max altitude, sunlight and ambient colours, glow, gamma. LL's sky shaders use these. Every existing EEP asset was tuned by its creator to look right with them.
   - **Physical values:** a Rayleigh layer (air), a Mie layer (haze) with its anisotropy, an absorption layer (ozone), each with an altitude profile; planet radius and sky top radius; droplet radius, moisture and ice levels. These are stored in every asset but LL's renderer barely uses them. They are exactly the inputs of the modern physical sky models below.
3. **SL altitudes.** Avatars build and live from 0 to 4096 m and above. Skyboxes at 2000 to 4000 m are common. The sky must look right from any altitude, and anything placed in the air (clouds) must not cut through builds.
4. **One result, many readers.** The sky feeds:
   - the background;
   - aerial perspective (haze on distant objects; LL's fog);
   - sun and moon light colour after the atmosphere;
   - ambient light and probes, water reflections, Mac OIT, volumetric lighting.

   As with shadows, it is computed once and shared through the registry.
5. **Real render rule.** Correct every frame. Nothing from earlier frames is blended in, so no temporal reprojection. Lower-resolution effects made in full every frame are allowed.
6. **The AyaneStorm GL viewer's add-ons** (procedural sun disc and halo, horizon scattering band, cloud scattering tint, water horizon fog; see `ayanestorm-procedural-horizon-scattering.md` and `sunset3-procedural-sun-reuse-review.md`) were added because LL's legacy model cannot make real sunsets. A physical model makes these effects from the atmosphere itself.

## Sky models compared

| Model | How it works | Fit |
|---|---|---|
| **LL legacy windlight** | Hand-tuned formulas from the 2007 windlight shaders, evaluated per pixel. | Matches every EEP asset exactly. But sunsets are flat: no ozone, a single scattering bounce, no change with altitude, no real red sun at the horizon. That is why AyaneStorm needed its GL add-ons. |
| **Preetham / Hosek-Wilkie** (1999 / 2012) | Curves fitted to measured clear skies, controlled by sun angle and turbidity. | Seen from the ground only, from no altitude, no planet curvature, no night or twilight, and none of EEP's physical values used. **Rejected.** |
| **Bruneton precomputed scattering** (2008, revised 2017) | Full multiple scattering stored in 4D tables. | Very accurate, but the tables take far too long to rebuild when inputs change, and EEP day cycles change them every frame. **Rejected.** |
| **Hillaire LUT sky** (EGSR 2020, 'A Scalable and Production Ready Sky and Atmosphere Rendering Technique'; used in UE4/UE5 and Frostbite) | Every frame, small lookup tables (LUTs), each made by a quick compute pass: <br>1. transmittance (how much light gets through the air, per altitude and angle);<br>2. multiple scattering (light bounced more than once);<br>3. sky view (the whole sky around the camera, in a low-resolution latitude/longitude map);<br>4. aerial perspective (a small 3D volume of haze and transmittance, fitted to the camera's view). | Its inputs are exactly EEP's physical values (Rayleigh, Mie, absorption profiles and radii). Rebuilt every frame, so day cycles blend freely. Works from any altitude, up to space. Gives real sunsets: a red, dimmed sun at the horizon, ozone's blue hour, Earth's shadow rising at dusk. The paper reports well under a millisecond on current GPUs. **Chosen.** |

### What the Hillaire sky gives the rest of the renderer

- **Sun and moon colour** at any point: the transmittance LUT. Lighting, water and volumetrics take the same value, so a sunset's light on the ground matches the sky.
- **Aerial perspective volume:** the fog on objects, read by the opaque resolve, Mac OIT (each layer at its own depth) and water. This replaces LL's haze formulas.
- **Sky view LUT:** read by the background, by reflections (water, probes) and by ambient light. Ambient light gets a small set of numbers (spherical harmonics) reduced from it once per frame.
- **Volumetric lighting** reads the same scattering coefficients, so god rays match the haze.

### EEP's legacy values

Two ways to honour them:

- **A. Translated:** legacy values are converted into physical inputs (blue density → Rayleigh scale and tint, haze density → Mie density, density multiplier → overall density, distance multiplier → aerial perspective scale, sunlight and ambient colours → light tints). Every sky goes through the physical model. Measured side by side against the GL viewer on the common EEP presets (LL's library, Firestorm's defaults, popular region skies). The look is modern, close to the creator's colours but not identical.
- **B. Translated + legacy mode:** A, plus LL's exact legacy formulas as a second sky model, picked by a setting ('as the creator saw it'). It is a full second path: another set of passes behind the same registry results, so every reader still reads one result.

Assets whose physical values were edited by their creator, rather than left at the defaults, use those values directly.

### Sun, moon and stars

- **Sun disc:** brightness from the transmittance LUT, with limb darkening (the edge of the disc is darker than its centre). An EEP asset's own sun texture is honoured: it is tinted the same way, not replaced.
- **Moon:** a second light with the moon's texture, phase and brightness from EEP. It goes through the same transmittance, so a rising moon turns orange. It drives the night sky's faint scattering.
- **Stars:** EEP's star brightness. A procedural star field (point stars, magnitudes, colour) or the EEP texture, faded by the sky's own brightness, so stars appear as twilight darkens rather than on a timer.
- **Tone mapping:** a real sunset is a big range of brightness. It needs the HDR path and a good tone mapper, which belongs to P4 lighting anyway.

## Clouds

### What EEP gives

A 2D cloud noise texture (custom per asset), cloud colour, two density and position sets, scale, scroll rate (wind), variance, and cloud shadow strength. LL draws it as a scrolling textured dome.

### Volumetric clouds

Modern volumetric clouds (Horizon Zero Dawn's Nubis 2015 to 2017 and its voxel version Nubis³ 2023; Frostbite 2016) march rays through a layer of 3D noise shaped by a 2D weather map. EEP maps onto that naturally:

- the EEP cloud texture becomes the **weather map** (coverage and type);
- cloud density, scale and colour shape it;
- scroll rate is the wind;
- cloud shadow strength scales a real **cloud shadow map**: a 2D texture of light through the clouds, put in the shadow registry and multiplied into `sun shadow` for every reader.

Volumetric lighting through cloud gaps then comes for free.

Problems:

- **Cost:** games keep clouds cheap by marching 1/16 of the pixels each frame and filling in the rest from earlier frames. That is temporal reprojection, which the real render rule bans. Without it, clouds are marched in full every frame at quarter or half resolution, with per-pixel blue noise and a depth-aware upsample. This runs fast on the 3080 Ti and M4, but likely too slow on a GT 1030. So it is a tier: low budgets get the EEP 2D clouds, lit by the same sky.
- **Altitude:** real cloud layers sit at 1500 to 4000 m, which is exactly where SL skyboxes are. A cloud layer there would cut through builds and fill skyboxes with fog. Choices:
  1. the layer sits above SL's build heights (from about 4500 m), seen from below as distant clouds;
  2. the layer follows the camera's altitude, always a set height above it, like LL's dome;
  3. the layer's height comes from the region (EEP has no field for it, so it would be a viewer setting).

  A camera above the layer (flying high, skyboxes at 5000 m and more) sees the clouds from the top, which makes high skyboxes look striking.
- **EEP's 2D clouds stay** as the low tier and as the fallback when the volumetric layer is off. They are lit by the sky view and sun transmittance, so they stay consistent with the new sky.

### Verdict

Volumetric clouds are relevant: sunsets are mostly the clouds lighting up, and the cloud shadow map gives sunlight and volumetrics their real occlusion. Recommended as the high tier, after the sky itself, measured on the M4 and 3080 Ti before being kept.

## From Frostbite's 2016 course notes

Source: Hillaire, 'Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite', SIGGRAPH 2016 course notes.

### Confirms the choices above

- **Its 2016 sky is the one the 2020 paper replaced.** It uses a 3D scattering table that drops the angle between view and sun around the vertical, so Earth's shadow at dusk is lost. Rebuilding that table after a change in the atmosphere costs 3.5 ms, so Frostbite spread the rebuild over 19 frames and blended between results. We cannot do that, since EEP changes the values every frame and the real render rule bans spreading. The 2020 LUT sky rebuilds every frame. The choice stands.
- **Ozone is essential:** without it the sky turns too yellow overall, and twilight loses its blue. EEP's absorption layer is that.
- **Aerial perspective volume:** 32 × 32 × 16 (width, height and depth slices), fitted to the camera, costs 0.05 ms on Xbox One. It is the same as ours.
- **Cloud layout:** clouds sit in one slab of constant height, shaped by a 2D weather texture (density, cloud type) and a cloud-type texture (density and erosion by height within the layer). EEP's cloud texture maps onto the weather texture as planned.
- **Cloud shadow:** a 2D transmittance texture projected onto the world and assumed flat around the camera. It is applied to opaque and transparent surfaces and to particles, and fed to global lighting. The same as our `cloud shadow` result.

### Taken from it

1. **Energy-conserving step integration** (its section 5.6.3, appendix C): each ray-march step integrates scattering against the step's own transmittance analytically. The result stays right with few, large steps, where the usual order over- or under-darkens. Used for clouds and volumetric lighting.
2. **Multiple scattering in clouds by octaves** (Wrenninge 2013): a few octaves of scattering are summed during the same march, each with less scattering, less extinction and a flatter phase function. Without it, clouds look like dark smoke. N = 2 or 3; scattering is reduced at least as fast as extinction, so no light is created.
3. **Dual-lobe phase function** for clouds: one forward lobe and one back lobe (Henyey-Greenstein), giving the bright silver lining around the sun and lit cloud fronts away from it.
4. **Cloud ambient light from the sky's spherical harmonics**, weighted by a gradient from dark at the bottom of the layer to bright at the top, since the sky is the main ambient source. That is our `sky ambient SH` result.
5. **Aerial perspective on clouds, cheaply:** the march keeps a depth averaged by transmittance (empty samples skipped). The aerial perspective is then read once at that depth and applied to the cloud result, instead of at every step. This fixes the seam between clouds and ground at the horizon.
6. **Clouds affecting aerial perspective:** under a thick layer, the haze must darken and take the clouds' colour. The clouds' average transmittance and scattered light, seen from the camera, scale the aerial perspective's sun term and add to it. One more small pass on the cloud result.
7. **Sun and moon in physical units:**
   - the sun disc's brightness comes from its illuminance and angular size, after transmittance, with limb darkening from measured data;
   - the moon is 0.568° wide with a luminance of about 3658 cd/m², times its texture;
   - stars are coloured by temperature (Planck's law).

   The moon's lit and dark sides are left to artists in Frostbite. EEP gives the moon texture only, so we do the same.
8. **Sky, moon and stars in reflections and probes:** they come through the sky view LUT and the moon and star passes, so they are part of what probes capture and blur.

### Where we differ

- **No temporal integration.** Frostbite's clouds take 16 samples per pixel at half resolution and blend each frame with the last (an exponential moving average). Without that they are noisy (its figure 34). Ours must be smooth in one frame:
  - more samples where the layer is dense, guided by a coarse density mip, empty space skipped;
  - blue-noise jitter per pixel, fixed over time;
  - a depth-aware spatial filter at half resolution.

  Expect more than Frostbite's 1.6 ms (1080p on Xbox One, worst case at the horizon). To be measured on the 3080 Ti and M4 before the tier is kept.
- **The cloud layer follows the camera** (decision 2), where Frostbite places it at a fixed height over the planet.
- **Cloud views:** Frostbite renders clouds in the main view, planar reflections, environment maps and shadows. Our probes and water read the main view's sky and cloud results where they can, and only views the main one cannot serve render their own, at lower resolution.

## From Hillaire's 2020 sample code

Source: the paper's companion code from Epic Games, 2020, DX11 and HLSL, MIT licence. Code taken from it keeps that licence's notice in the files that carry it.

### EEP's physical values are its inputs, byte for byte

Its atmosphere uses the same density-layer format as Bruneton's code: `width, exp_term, exp_scale, linear_term, constant_term`, two layers per profile. EEP's `rayleigh_config`, `mie_config` and `absorption_config` use exactly those keys (`LLSettingsSky::SETTING_DENSITY_PROFILE_*`). The translation for physical values is therefore a copy; only units need care (the sample works in kilometres, so EEP's units must be checked when it is written). The legacy-value translation (decision 1) writes into these same fields.

Earth defaults in the sample:

| Value | Default |
|---|---|
| Bottom radius | 6360 km |
| Top radius | 6460 km |
| Rayleigh scale height | 8 km |
| Rayleigh scattering | (0.005802, 0.013558, 0.033100) /km |
| Mie scale height | 1.2 km |
| Mie scattering | 0.003996 /km |
| Mie extinction | 0.00444 /km |
| Mie g (phase) | 0.8 |
| Ozone | tent profile peaking at 25 km, extinction (0.00065, 0.001881, 0.000085) /km |

### Sizes and sample counts it ships with

| LUT | Size, format | Samples |
|---|---|---|
| Transmittance | 256 × 64, RGBA16F | 40 per texel (as low as 10 loses visible energy) |
| Multiple scattering | 32 × 32 | 20 per ray, 64 directions, summed as a power series |
| Sky view | 192 × 108, R11G11B10F | 30 per texel |
| Aerial perspective | 32 × 32 × 32, RGBA16F | grows with slice (2 per slice index) |

The sky view LUT's mapping is **non-linear in latitude**: more texels near the horizon, where sunsets happen. We keep that.

### Taken from it

1. **LUTs at unit sun brightness.** The tables store how much light the atmosphere passes, for a sun of brightness 1. The sun's real brightness and colour are multiplied in when they are read. EEP's sunlight colour and brightness can then change at no cost, and the tables are rebuilt only for the atmosphere itself. They still get rebuilt every frame (EEP blends), but nothing else depends on the sun's value.
2. **Shadowed atmosphere.** Its ray march samples the sun shadow map at each step (`getShadow`), so mountains and buildings cast shadows into the haze. That is the `sun shadow` result from the shadow study, read by the aerial perspective pass: another reader, nothing new made.
3. **Sun angle range:** the tables cover the sun down to 120° from the zenith, 30° below the horizon, so twilight and the Earth's shadow fade correctly after sunset.

### SL adaptation: aerial perspective slices

The sample spreads its 32 slices at 4 km each (with a squared distribution), up to 128 km: it was made for flight-sim distances. SL's draw distance is at most 1024 m, so with those slices the whole scene would sit in the first slice. Our slices follow the view's draw distance:
- the far edge is the draw distance, plus the terrain beyond it up to the horizon if distant terrain is drawn;
- the squared distribution stays, so near slices are thin.

The sky view LUT keeps doing the background beyond, as in the sample.

**Altitude:** SL heights in metres become altitude above the planet's bottom radius, with SL's sea level (water height) as 0. From the ground up to 4096 m and above, the camera stays inside the atmosphere, as the sample assumes.

## Registry results (made once per frame)

| Result | Read by |
|---|---|
| `atmosphere`: physical coefficients after the EEP translation, sun and moon directions | every pass below |
| `transmittance LUT`, `multiscatter LUT` | sun and moon colour, lighting, water, volumetrics, clouds |
| `sky view LUT` | background, reflections, probes, ambient |
| `sky ambient SH` | lighting, avatars, Mac OIT |
| `aerial perspective`, per view | opaque resolve, Mac OIT, water, volumetric lighting |
| `cloud shadow` (2D, sun space) | `sun shadow` readers, volumetrics |
| `clouds`, per view (colour + transmittance, low resolution) | composite, reflections |

## Order

1. Atmosphere and its LUTs, the EEP translation, sun, moon and stars, aerial perspective on everything. Measured side by side with the GL viewer.
2. Ambient and sun lighting from these results (P4's lighting).
3. Volumetric clouds and the cloud shadow map, as the high tier.

## Decisions (user, 15 November 2026)

1. **EEP legacy values: A, translated only.** One sky model; there is no legacy mode.
2. **Volumetric clouds: yes, as the high tier, with the layer following the camera (choice 2).** It is always a set height above the camera, so it never cuts through builds. The camera never gets above it: the 'seen from the top' case does not exist.
3. **The AyaneStorm GL add-ons are not ported.** The physical model makes their effects; their sliders do not exist in the new renderer.
