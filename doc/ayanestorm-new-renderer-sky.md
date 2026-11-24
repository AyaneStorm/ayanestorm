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

## From Nubis³ (Schneider, Advances 2023)

Horizon Forbidden West's voxel clouds, built so the player can fly through them. Read 16 November 2026.

### What we take

1. **No temporal upscaling, and still fast.** Guerrilla dropped temporal upscaling for these clouds. Instead, the render is split in two passes by depth: **under 200 m at 480×270, beyond 200 m at 960×540**. Close clouds are the costly part of the march and also the blurriest on screen, so a low resolution hides there; far clouds stay sharp, so no aliasing. On PS5: 4 ms → 2.1 ms, the same method at 40 and 60 Hz. Some pixelation shows on sharp, dense edges up close; thinner regions hide it. This replaces the single 'quarter or half resolution' march of the cost note above: two depth ranges, each upsampled with the depth-aware filter.
2. **Distance-guided march.** A signed distance field (SDF: per voxel, the distance to the nearest cloud) sits beside the density. Step size = max(SDF distance, adaptive step) + a jitter, so empty space is crossed in one jump and dense regions get small steps. Their costs: 10 ms base → 6.1 ms with voxel lighting → **2.2 ms** with the SDF and adaptive march.
3. **Light precomputed per voxel.** For every voxel holding cloud, density is summed along a ray toward the sun, up to the grid's edge, and stored. The march reads it instead of marching toward the sun at every sample: about **40 % less** render time, and long-distance self-shadowing for free. It also improves ambient scattering. 0.1 to 0.2 ms.

### What does not carry over as is

- **Their clouds are authored.** Nubis³ grids (512×512×64, about 17 MB for one field in BC6) are built offline in Houdini. EEP gives only a 2D cloud texture, density, scale and wind. So the voxel grid and its SDF would be **built on the GPU from the EEP values**: a registry result made once per EEP change, not per frame. The density shaping already planned (weather map, cloud type, erosion noise) becomes the grid's producer instead of running at every march sample.
- **Camera-following layer** (decision 2): the grid must follow the camera. Updating only the cells that enter at the edges (a wrap-around grid) keeps it from being rebuilt each frame.
- **Wind:** EEP scrolls clouds. If the grid holds the shape and the wind only offsets the lookup, the SDF and the light grid stay valid while the wind moves the clouds.
- **Light amortized over 8 frames:** they spread the light grid's update over 8 frames. That is cached data refreshed over time, like the cached shadow cascades, not pixels reused from earlier frames. **Decided (user, 16 November 2026): amortized over 8 frames.** Clouds and sun move slowly, so 8 frames of lag go unseen, the more so above 60 fps. Jumps are the exception: a new EEP, a teleport or a region change refresh the whole grid in one frame (0.1 to 0.2 ms on PS5, once), so nothing is seen catching up.
- **VRAM:** two to three grids of about 10 to 25 MB each, 30 to 60 MB in all. That fits within the 4 GB floor, but is counted in the budget and dropped with the tier.

### Effect on the tier

The cost note above expected full marching every frame to be too slow for a GT 1030. With the two-pass split, the SDF and the light grid, the PS5 figure is 2.1 ms. That makes the volumetric tier likely on the 3080 Ti and M4, and worth measuring on the GT 1030 instead of assuming it is out. The 2D EEP clouds stay as the low tier.

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
| Sky view | 192 × 108, R11G11B10F | 4 to 14 per texel, growing with ray length (`RayMarchMinMaxSPP`; the 30 in its code is unused when the count varies) |
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

## From the 2020 paper

Source: Hillaire, 'A Scalable and Production Ready Sky and Atmosphere Rendering Technique', Computer Graphics Forum 39(4), 2020.

### Cost

- **GTX 1080, 1280 × 720:** 0.31 ms for everything, every LUT rebuilt every frame. Of that:
  - transmittance LUT (256 × 64, 40 steps): 0.01 ms;
  - sky view LUT (200 × 100, 30 steps): 0.05 ms;
  - final sky and aerial perspective on screen: 0.14 ms.
- **For comparison:** Bruneton's model rebuilt in full costs 250 ms.
- **iPhone 6s:** LUT sizes and step counts are lowered with no difference visible to the eye, about 1 ms in Fortnite. The transmittance LUT is kept the same on every device so the look matches. That is our low tier: smaller multi-scattering, sky view and aerial perspective tables and fewer steps, with the transmittance table identical everywhere.

### Limits to know, because of EEP

- The multiple-scattering shortcut assumes light scatters evenly in all directions after the second bounce. It is accurate for Earth-like air. It drifts in hue when the atmosphere is made very dense, and loses accuracy as Mie g rises.
- EEP creators often push haze and density multipliers far beyond Earth. Heavy-haze presets are part of the side-by-side tests with the GL viewer. If they drift visibly, the translation clamps density or the multi-scattering LUT takes more steps; to be decided on the measurement.

### Shadows in the atmosphere (mountains, buildings, god rays)

- The paper ray marches the view ray with the cascaded shadow maps, 32 samples (1.0 ms total). It jitters per ray and smooths over frames with TAA (temporal anti-aliasing) and reprojection. **We cannot**: the real render rule bans reprojection.
- So the shadowed atmosphere is not a separate march. It belongs to the **volumetric lighting pass**: one froxel grid ('froxel' = a cell of the camera frustum, like a 3D pixel). Each froxel gets the atmosphere's scattering and extinction from the `atmosphere` result, plus local fog, plus `sun shadow` and `local shadow atlas` visibility. The grid is integrated front to back once, with the energy-conserving integration from the Frostbite notes.
- The result is one volume holding the shadowed aerial perspective and the volumetric lighting together, read by the opaque resolve, Mac OIT and water. The unshadowed `aerial perspective` volume stays as the low tier and for views with no volumetrics.
- Smoothness in one frame comes from the froxel grid's own resolution and a fixed per-pixel blue-noise offset, not from history.
- **Sharp sun shafts are not in the grid** (user, 15 November 2026). A froxel grid without history is lossy: blurry or stepped shafts. They come from the GL viewer's **Exact mode**, ported (see `volumetric_lighting_exact_redesign_research.md`):
  - polygonal light volumes (Billeter 2010): each cascade's depth surface is drawn as a mesh;
  - the exact analytic integral along each view ray: lossless, no sampling, no history.

  It reads the shared `sun shadow` cascades and the `atmosphere` coefficients and adds to the grid's result. The grid leaves out the sun's single scattering entirely, so nothing is counted twice. Exact mode assumes the medium is even along the ray; the atmosphere is not (it thins with altitude), so its coefficients are taken at the camera's height, which is accurate within SL's draw distance. A ray climbing steeply from the ground to a skybox is the case to check by eye. The grid keeps everything that varies slowly: atmosphere, local fog, local lights, clouds. Sun and moon shafts are exact (user's requirement). Local lights' shafts come from the grid only (decided 15 November 2026, the user's delegation):
- an Exact volume per shadowed projector would cost a mesh draw per light per frame, growing with the light count;
- local lights sit near the camera, where the grid's depth slices are thinnest, so their shafts lose the least there.

### From Wronski's froxel fog (Assassin's Creed IV, SIGGRAPH 2014)

The paper that introduced the froxel grid our volumetric lighting pass uses. Read 16 November 2026. Its grid uses **no history**: smoothness comes from the grid itself, as decided above.

- **Grid size:** 160×90×64, or ×128 on stronger hardware. That is as many cells as a 720p image, each lit once. The cost is fixed, whatever the screen resolution.
- **Depth slices:** exponential, concentrated near the camera, where aliasing shows most. This matches local lights' shafts coming from the grid (above).
- **No scene depth needed:** the grid is built from the camera, the media and the shadow maps only. So it can run on **async compute** (work running beside the drawing, on the GPU's compute queue), as soon as the shadow maps are ready, overlapping the geometry passes.
- **No edge artifacts:** every pixel reads the grid with 3D filtering at its own depth. This avoids the halos of a 2D low-resolution effect with a bilateral upsample, and every Mac OIT layer gets its own correct fog.
- **Shadows in the grid:** one sampled point per froxel flickers as the camera moves, and wide PCF was slow and still flickered. They fixed it with a **pre-blurred shadow map** for the fog only (an exponential shadow map, cascades downsampled ×4 to 1024×256 R32F, box blurred). Shadow leaks, the reason ESM is rejected for surfaces, did not show in fog.
  - For us, the sun is not in the grid (Exact mode), so this concerns **local lights** in the grid, which read the `local shadow atlas` with one-tap PCF (shadows doc). **To check by eye** on the first grid build: if their shafts flicker when the camera moves, the fix is a small pre-blurred copy of the atlas tiles feeding the grid, made once per frame as its own registry result.
- **Density:** one octave of wind-animated noise and an exponential falloff with height. More octaves cost more for little difference. This is a starting point for local fog.
- **Cost:** 1.1 ms in all on PS4 / Xbox One (density and lighting 0.43 ms), 1.6 ms at double resolution.

### Not needed

- **Space views:** the sky view LUT wastes most of its texels on empty space when seen from orbit, so the paper switches to marching on screen there. SL cameras stay inside the atmosphere, so that switch is not written.

## Registry results (made once per frame)

| Result | Read by |
|---|---|
| `atmosphere`: physical coefficients after the EEP translation, sun and moon directions | every pass below |
| `transmittance LUT`, `multiscatter LUT` | sun and moon colour, lighting, water, volumetrics, clouds |
| `sky view LUT` | background, reflections, probes, ambient |
| `sky ambient SH` | lighting, avatars, Mac OIT |
| `aerial perspective`, per view (unshadowed, low tier and views without volumetrics) | opaque resolve, Mac OIT, water |
| `volumetric scattering`, per view (froxel grid: shadowed atmosphere + fog + local lights) | opaque resolve, Mac OIT, water |
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

## Step 1 batch 1: written (15 November 2026)

Library only, Vulkan and Metal, tested in `asrenderer_test` on Windows (validation clean) and the Mac.

- `asratmosphere.h/.cpp`: `SkyInput` (what a sky is, km and per km, Bruneton's two-layer profiles), `earthSky()` (the sample's Earth), `GpuAtmosphere` (after the scene view in its buffer, `GpuSceneView::atmosphere`).
- Shaders: `asrAtmosphere.glsl` (the sample's functions, MIT notice), passes `transmittance lut`, `multiscatter lut`, `sky view lut`, `exposure`; the resolve draws the background (sky view table, limb-darkened sun disc dimmed by the air).
- Tone mapping and exposure moved here from step 2 (see the P4 plan, step 2's HDR entry).
- `asrenderer_test --sky earth|noon|sunset|night|high|haze` picks a test sky.
- Cost, sky tables and exposure together: about 0.07 ms on the 3080 Ti, 0.24 ms on the M4.
- Below the horizon the planet is black (the sample's ground albedo is 0). In SL terrain and water cover it; what shows past the draw distance is looked at with aerial perspective (batch 2).

Next, batch 2: aerial perspective in the resolve and Mac OIT, moon, stars. Batch 3: viewer (EEP translation, tone settings, 2D clouds).

## Step 1 batch 2: written (15 November 2026)

Library only, Vulkan and Metal. Built and run in `asrenderer_test` on Windows with validation on and 0 errors. Not yet checked by eye, and not yet run on the Mac.

- **Aerial perspective volume** (`asrAtmosphereAerialC.glsl`, pass `aerial lut`): 32 by 32 view rays, with 32 slices spread squared out to the view's draw distance (`GpuAtmosphere::aerialDistance`, or 1024 m with no limit). One thread marches one ray once, two steps a slice. rgb is the light for a sun of brightness 1, a is the mean transmittance. The resolve lays it over opaque pixels, and Mac OIT's colour pass over each layer (`applyAerial()`), so both match. Nearer than the first slice it fades out, as in the sample.
- **Set 1 bindings:** the sky's tables are written at 11 to 14 and sampled at 15 to 18 (18 is the volume, a 3D texture).
- **Background:** stars, then the moon over them, then the sun over both, all dimmed by the air and hidden by the planet. Sun and moon take an image as `SkyInput::sunTexture` / `moonTexture` (a square facing the camera, its top along `sunUp` / `moonUp`, mips from the pixel's size on the sky, with texture feedback). Without an image the sun is the limb-darkened disc and the moon a flat disc. A body's image carries its illuminance over the disc inside the square.
- **Stars: the AyaneStorm stars, ported (user, 15 November 2026).** The viewer keeps `ASStars` as it is: real-sky or procedural catalogue, settings and floater. It hands the library the list of stars when the catalogue changes, then each frame the rotation (`applySkyTransform()`: zenith spin, or sidereal spin and latitude tilt), brightness and twinkle. The library draws them as sprites in their own pass, only where nothing was drawn, each dimmed by the air along its own direction. The Milky Way glow (`ASMilkyWay`, an equirectangular texture) is read in the background under the same rotation. Not written yet. Shader-made stars were tried and removed.
- **Test skies** carry a full moon 40 degrees from the sun, at roughly its real share of the sun's light. `--sky fog` is a thick ground haze (about half the light lost in 250 m). `--sky-view` looks across the city to the horizon, half sky. The older cameras never showed the sky.
- **Rays go through the planet:** SL ground often lies below altitude 0 (the water height). A ray stopped at the planet's surface lost its air after a few tens of metres, and buildings did not fade. Below the surface, points read the air at the surface.
- **Exposure sky guard (user, 15 November 2026):** the white-card meter stays. The sky's mean luminance over the upper hemisphere is also measured, and the exposure is lowered so it stays at or below `SKY_KEY` (0.5) before tone mapping. At sunset the sky keeps its colour, where it had blown out to pale yellow. At noon the card still decides; at night the floor does. Checked by eye on captures: sunset orange to blue, fog fading the city with distance. Noon looks a fairly dark blue: compare with GL at the side-by-side.
- **Cost on the 3080 Ti:** aerial volume 0.02 ms. All sky passes together come to about 0.09 ms.
- **Open:** the brightness of the images' glow areas. The default SL sun image has a soft halo, which this scale makes bright.

**Moon decisions (user, 15 November 2026):**
- The default SL moon becomes a lit sphere, made in the shader with no mesh. Its phase comes from the real sun direction. Its surface colour is `moon_2k.tif` and its normal map is made from `moon_displacement_16.tif` (the user's, in E:\dev\AyaneStorm), both shipped as viewer assets. They are converted into `skins/default/textures/as/as_moon_albedo.png` (2048 x 1024) and `as_moon_normal.png` (2048 x 1024 RGBA since batch 3b, from the 16 pixels per degree displacement, area averaged; tangent normal in rgb, mean slope 4 degrees at this size; height in alpha from -9 to 10.75 km), with `as_moon_CREDITS.txt` and the credit in each PNG's text. Source: NASA's CGI Moon Kit. Every file that ships them, and the viewer's credits, carry this line: "NASA's Scientific Visualization Studio. Visualizer: Ernie Wright (STC). Scientist: Noah Petro (NASA/GSFC)."
- Custom EEP moon textures stay flat images, as OpenGL draws them.
- Moon glow: the moon is a second light in the sky tables, so its halo and the night sky's colour come out of the same scattering as the sun's.
- The phase is the artist's, never the sun's: `moonPhase` and `moonPhaseTilt` as ASMoonPhase and ASMoonPhaseTilt mean them. The moon's light on the sky follows that phase (Lommel-Seeliger's disc-integrated phase law).
- Size: OpenGL's (user, 16 November 2026, replacing the real 0.26 degrees times `moon_scale`, which drew every EEP moon ten times smaller than its author saw it): atan(0.1 x 0.45) x `moon_scale`, about 2.6 degrees at scale 1. OpenGL's horizon stretch (1.3 wide, 1.2 high) is left out, as it would make the sphere oval.
- The sun likewise (user, 16 November 2026): EEP's sun image as a square of half side atan(0.1 x 0.5) x `sun_scale`, about 2.9 degrees at scale 1, its glow included, `sunUp` as llvosky.cpp. No sun image draws no sun, as in OpenGL. The image's light is spread over the whole square (batch 2's open item on the glow's brightness still stands).
- Every AyaneStorm moon panel feature carries over:
  - In the viewer: brightness multiplier, horizon tint, horizon minimum opacity.
  - In the library: phase, tilt, phase softness as the terminator's softness, earthshine, relief strength as the normal map's strength, and halo strength, radius and softness on top of the air's glow.
  - Phase curvature is dropped (user): in the OpenGL moon shader it shaped a made-up surface depth on the flat disc, and the true sphere gives the real curve.

## Step 1 batch 3a: moon (written 15 November 2026)

Library, Vulkan and Metal. Built and validated on Windows, 0 errors. The user saw the textured moon in `asrenderer_test --sky-view --sky night --moon-maps moon`.

- **Sphere moon** (`moonSphere()` in the resolve): each pixel of the disc finds its point on the sphere. Shading is Lommel-Seeliger (2 mu0 / (mu0 + mu), so a full moon is evenly bright). Colour and normals are read by longitude and latitude, with the edge smoothed over a pixel.
- **Disc brightness against the exposure:** `moonBrightness` (EEP `moon_brightness` × ASMoonBrightnessMultiplier). A physically bright moon at night's exposure is thousands of times past white, a blown-out disc, so the disc no longer follows `moonIlluminance`, which now only lights the sky.
- **Moonlight in the air:** the sky view texture holds two tables, sun on top and moon below (`SKY_VIEW_LUT_LIGHTS`). The aerial volume marches both lights and stores light at their own brightness. Exposure meters both.
- **Test:** `--moon-maps DIR` reads `moon_albedo.rgba` (2048 × 1024) and `moon_normal.rgba` (2048 × 1024 since batch 3b), raw RGBA written from the PNGs with Pillow, into `build-asr/moon`.

## Step 1 batch 3b: stars, Milky Way, moon in the viewer (written 15 November 2026)

Library, Vulkan and Metal, and the viewer. `asrenderer_test --scene --sky-view --sky night --validate` runs with 0 validation errors on Windows. Not yet checked by eye; Metal and the viewer not yet built.

- **Stars pass** (`asrSkyStarsV/F.glsl`, pass `stars`, after the resolve): one screen-facing sprite per star, added into the lit image where the depth is still the far plane. A star behind the planet, the moon's disc or the sun's disc is not drawn; the rest are dimmed by the transmittance along their direction. Brightness is OpenGL's chain (EEP star brightness through smoothstep, x32, ASStarsBrightness, the per-star decoded alpha, twinkle), against the exposure as the moon's disc. Alpha adds nothing, so glow is unchanged.
- **At least 2 pixels wide:** a smaller sprite is drawn 2 pixels wide and dimmed by the area it gained, so a star never falls between pixel centres and blinks as the sky turns. OpenGL's horizon fades are dropped: the planet and the air do that job.
- **Star list:** `SkyInput::stars`, a shared list the viewer replaces only when ASStars makes a new catalogue; each frame-in-flight slot keeps its own copy, written only when the list changes.
- **Milky Way** in the resolve's background, under the moon: both maps read in the stars' frame, with mips from the pixel's size on the sky. Levels from `ASMilkyWay::levels()`, which OpenGL now uses too.
- **Crater shadows (user: important for the moon to look right):** `moonShadow()` in the resolve marches from each moon pixel towards the light over the height map in the normal map's alpha. The ray climbs at the light's height above the local horizon while the sphere curves away under it, in steps growing by 1.15 from a texel, up to 0.2 moon radii, each reading the mip its step spans. The edge is softened over the sun's radius as seen from the moon (0.00465 radians), the real penumbra, so shadows are long and sharp at the terminator. ASMoonTerminatorReliefStrength scales shadows with the normals. `SkyInput::moonHeightRange` is the alpha's range in km; 0 casts none.
- **The moon turned for where you stand (user, 16 November 2026):** in ASStars' real-sky mode the moon's north points to the celestial pole of the stars' own frame (ASStarsLatitude, sidereal time), so moon and stars always agree: upside down south of the equator, tipped near it and turning through the night. The artist's phase and ASMoonPhaseTilt stay relative to the moon's face (user's choice over lighting it from the real sun), so a waxing moon lit on the face's right shows lit on the left from the south. Custom flat moons turn the same way. Stock star mode keeps OpenGL's zenith-up. Libration left out.
- **Halo reworked (user, 16 November 2026: 'the halo looks wrong', a bright flat band):** exponential from the limb, exp(-3x / softness) with x 0 at the limb and 1 at ASMoonHaloRadius, faded to nothing over the last third so it has no edge. Its level is against the moon's mean colour (its texture's last mip), not white: the moon reflects about 12%, so a white-referenced halo outshone the disc several times. It follows the phase's light (`moonSkyScale`), so a crescent's halo is faint. Test: `--look-moon` frames the halo, and `--sky night` carries one (strength 1, radius 3, softness 1).
- **Moon settings rescaled (user, 16 November 2026: brightness best at its maximum, softness only at 0, halo strength wanted 0.01, halo radius at its maximum):**
  - The sphere's albedo is scaled to a mean of 1 (its texture's last mip), so `moonBrightness` is what the face shows at on average, as a grey OpenGL moon image does; the real 12% albedo needed x5.
  - Terminator softness is also the light's radius in the shadow march (sun radius + softness), so shadows soften with the wrapped shading instead of cutting it off at the true terminator.
  - Halo strength 1 glows at 5% of the face's mean at the limb (was 100%).
  - Halo radius goes to 12 moon radii in the native renderer (slider max 12; OpenGL still clamps at 4).
- **Moon panel (user, 16 November 2026):** the horizon colour works natively: `ASMoonRendering::horizonTint()` (moonF.glsl's formula, shared with OpenGL's halo) gives `SkyInput::moonTint`, over the disc and its halo, on top of the air's own reddening. Phase curvature, ASMoonHorizonMinOpacity and ASMoonTerminatorReliefWidth have no effect natively but stay for OpenGL, which must keep working (user). New ASMoonSizeMultiplier (0.1 to 10, default 1) multiplies the sky's moon scale in both renderers; OpenGL rebuilds the sky geometry when it changes.
- **Alt-click on the moon (user, 16 November 2026), both renderers:** where a focus pick hits only sky (`lltoolfocus.cpp`), `ASMoonRendering::focusOnMoon()` tests the click ray against the moon's disc (1.25 times its radius, so a small moon is easy to hit) and turns the camera to it, the focus 2 m ahead so orbiting turns the view in place. While there, the scroll wheel zooms the field of view by 1.15 a click, down to the camera's minimum, never past the field of view it started from (`llagentcamera.cpp`). The field of view comes back once the focus target moves elsewhere. The moon's size has one source for both: `ASMoonRendering::angularRadius()`.
- **Earthshine colour:** `SkyInput::moonEarthshineColor`, bluish (0.80, 0.88, 1.0) by default, times ASMoonEarthshineStrength.
- **Viewer:** `fillMoon()` and `fillStars()` in `asrviewer.cpp`. The default moon is the sphere with `as_moon_albedo.png` and `as_moon_normal.png`; a custom moon is EEP's image; no moon image draws no disc. The moon's light on the sky is EEP moonlight x moon brightness x 3e-3, to tune at the side-by-side. Not carried over yet: ASMoonHorizonTint and ASMoonHorizonMinOpacity (the air now reddens and dims the moon near the horizon), and the sun's image.
- **Sun disc (user, 16 November 2026):** with no sun image the viewer zeroed `sunAngularRadius`; it now takes `sunTextureRadius`, OpenGL's size, and the disc shows. Open: it does not shine, a flat white disc. Checked in OpenGL: with a null sun id there is no texture (`llvosky.cpp` setSunTextures) and no disc by day (`lldrawpoolwlsky.cpp` draws one only with an image or the procedural sun, which shows only at sunrise and sunset). The daytime shine is the sky shader's EEP glow (glow size and focus), a bright saturating spot with no edge. Next: revert this disc (no image, no disc, as OpenGL), and make the glow around the sun follow EEP's glow size and focus on top of the Hillaire Mie peak.
- **Was open: the sun's disc did not appear** in the new renderer. Most EEPs have no sun image: OpenGL draws a bright disc. The resolve already draws `sunDisc()` when `sunTexture` is NO_SKY_TEXTURE, so check first whether the viewer passes EEP's default sun texture (taking the image path), then the disc's size against OpenGL's. Seen: the sky's glow around the sun is right, with no disc at its centre at all, not even a small one.
- **Sun shine (16 November 2026, built in asrenderer_test, viewer build pending):** the no-image disc is reverted (no image, no disc, as OpenGL). The sky view texture now holds a third table: the sun's Mie single scattering with no phase (the sun's own table loses it; the phase is one value along each ray, so it splits exactly). `skyRadiance()` applies the Mie phase per pixel, so the sun's peak is as sharp as the screen, not the 192x108 table, times OpenGL's glow on the haze (`skyF.glsl`: max(size (1 - cos), 0.001)^focus + 0.25, capped at 100000) for this EEP against the default EEP's (5, -0.48): the default keeps Hillaire's peak alone, and only the haze is changed, as in OpenGL. EEP glow size and focus reach `SkyInput::sunGlowSize`/`sunGlowFocus`. To check in the viewer: library skies with a tight or wide glow against OpenGL; positive focus (EEP allows -10 to 10) darkens the sun's surroundings, as in OpenGL.
- **Open (user, 23 November 2026): the no-image sun still reads as a glow, not a sun.** OpenGL shows a very bright sun; the new renderer shows only the shine around it. Suspected: OpenGL's glow core is steep enough to clip to white over a disc-sized area after tone mapping, while the sharp Mie peak times the glow ratio clips over a much smaller one. Check against `skyF.glsl`'s numbers (the glow at the sun's centre and at 1 to 3 degrees, against the exposure) before changing anything.

## Step 1 batch 4: EEP 2D clouds (written 23 November 2026)

Library, Vulkan and Metal, and the viewer. `asrenderer_test --scene --sky-view --clouds --validate` runs with 0 validation errors on Windows. Not yet checked by eye; Metal and the viewer not yet built.

- **Shape: OpenGL's, line for line** (`asrSkyClouds.glsl`, `cloudShape()`): cloudsF.glsl's seven noise lookups, its disturbance, density from `cloud_shadow`, cover (alpha1) and self-shadow (alpha2), blended across the two EEP textures as OpenGL does. So every EEP's clouds keep their shape, scale, density and wind.
- **Where: OpenGL's dome, per pixel.** The ray meets a sphere whose top lies `ASCloudLayerHeight` above the camera and whose centre lies `dome_offset` radii below it (`ASWorldClouds::radius()`). Directions from the centre are the same as OpenGL's dome at any radius, so the UVs are llvowlsky.cpp's planar mapping with cloudsV.glsl's flip, scale and `ASWorldClouds` offset. The fade to the horizon is OpenGL's, from `max_y` in EEP's dome units. UV steps come from the pixel's size on the sky, so mips match the screen.
- **Light: this sky's, not OpenGL's haze formula.** Sun and moon after the air above the layer (transmittance table, the planet's shadow), a two-lobe phase (0.7 of g 0.8 forward, 0.3 of g -0.3 back), dimmed by the self-shadow; plus the sky's own light on the layer. The sheet's response is four times the phase, so an even phase gives a Lambert surface's 1/pi. The air in front dims the cloud by the transmittance ratio camera to cloud, and adds the share of the sky's light that lies before it.
- **Albedo:** EEP `cloud_color` against the default EEP's 0.41 grey, times 0.9: the default sky gets white clouds of a real albedo; tinted EEPs keep their tint.
- **Sky irradiance computed once:** the exposure meter already summed the sky's cosine-weighted radiance in rgb and kept only its luminance. `GpuExposure::skyIrradiance` now keeps the rgb: the clouds' ambient, and the ambient step 2 needs.
- **Stars:** the stars' vertex shader reads the cover at each star's centre and dims or hides it, as clouds hide the moon. The bindless textures and samplers are now visible to vertex shaders (Vulkan set 0 stage flags; Metal binds the argument buffer to the stars' vertex stage).
- **Viewer:** `fillClouds()` in asrviewer.cpp takes OpenGL's gates (a cloud texture, the Clouds render type), textures, blend, wind scroll and `lightnorm` self-shadow step.
- **Not yet:** cloud shadows on the world (step 4, shadows), clouds in the aerial perspective under a thick layer, and the exposure meter does not see the clouds.
- **Test:** `--clouds` adds the clouds of the user's 'Sunset' EEP (density 1, 0.53, 0.88; detail 1, 0.53, 0.12; cover 0.27; scale 0.42). `--cloud-map FILE` loads SL's own cloud texture as the viewer's disk cache keeps it (an `.asbc` from `asrtextures`, uploaded encoded); without it, a made-up tiling noise, too smooth to judge the look by. A copy of SL's default cloud texture is at `build-asr/clouds/default_clouds.asbc`.
- **llsettingssky.cpp's code defaults make poor clouds** (detail weight 1 breaks them into small pieces): nobody sees them in OpenGL, where the startup transition leaves them at once. The new renderer showed them at startup only while its frame path skipped `LLEnvironment::update()` (fixed 23 November).
- **Banding near the horizon (user, 23 November):** the air in front was a ratio of two transmittance table reads, kinked at the table's rows. It is now marched, 8 steps over the few km to the cloud.
- **Anisotropic filtering (23 November):** the new renderer had none; the horizon band was the cloud texture blurred flat there. Every linear sampler now follows `RenderAnisotropicLevel`, live (Vulkan: new samplers written into every frame's set after one GPU wait; Metal: per frame copy of the argument buffer). User: bands fixed.
- **Clouds still under the shared environment (user, 23 November): not a renderer bug.** The region's EEP had no X wind; OpenGL agreed after a cache clear (a damaged cache had also made its clouds look wrong). The wind offset is kept across environment changes by design (`updateCloudScroll()`), so the sky before a Reset decides which part of the texture is overhead after it. User: shared environment now correct.
- **Self-shadow moves with the sun's azimuth** (OpenGL's alpha2 lookup, offset towards the light): kept (user). In OpenGL it is hidden, as it only scales the sun's haze glow term.

## Firestorm sky option (design, 24 November 2026)

User: an option to show EEP exactly as vanilla Firestorm's OpenGL does; our sky stays the default. To be designed the best way, not bolted on.

- **Setting:** `ASRenderSkyModel`, 0 = ours (default), 1 = Firestorm. Live, no restart: a change swaps the producers below, and every table is refilled once in the next frame.
- **Principle: same products, other producer.** The Firestorm model fills the same registry slots ours does ('What the Hillaire sky gives the rest of the renderer'), by shadowing their producers. No reader branches on the model: the opaque resolve, Mac OIT, water, reflections, ambient light and clouds read the same things in both modes. No new memory: the legacy model makes neither transmittance nor multiple-scattering tables, and the other tables keep their sizes.
- **Sky:** `skyF.glsl`'s formulas (`blue_horizon`, `blue_density`, `haze_*`, `density_multiplier`, `distance_multiplier`, `max_y`, `glow`, `sun_moon_glow_factor`, `sky_hdr_scale`) are evaluated per pixel in the background, as OpenGL does (its sky is per pixel, not per vertex): the low-resolution sky view table would blur the glow's sharp peak. The same function also fills the sky view table, for reflections and ambient light. That is one function and two products at two resolutions, each made once. EEP's rainbow and halo (`moisture_level`, `droplet_radius`, `ice_level`, `rainbow_map`, `halo_map`) come with it. Our own model lacks them: a gap to close there as well.
- **Haze on the scene:** the legacy atmospheric functions (`calcAtmosphericVars`: additive, attenuation) are evaluated at each froxel centre and fill the aerial perspective volume. The formula is smooth in distance, so the volume's interpolation keeps it, and Mac OIT gets each layer's haze at its own depth for free.
- **Sun, moon and ambient light colours:** `LLSettingsSky`'s legacy values (sunlight, moonlight, ambient), constant across the scene as in OpenGL, take the place of the transmittance lookups.
- **Clouds:** `cloudsF.glsl`'s mapping and shading, in place of our marched 2D clouds. The texture, wind and scroll stay shared (`updateCloudScroll()`).
- **Sun, moon and stars:** as `sunDiscF.glsl`, `moonF.glsl` and `starsF.glsl` draw them. The default moon is SL's flat image, not our 3D sphere. AyaneStorm's OpenGL additions (procedural sun, volumetric lighting) keep their own settings and work in this mode too, as they do over the legacy sky in OpenGL (user); off, the result is vanilla.
- **Exposure and tone mapping:** these are part of the look, so this mode uses Firestorm's own chain (`sky_hdr_scale`, dynamic exposure, tone map type and mix, from the same settings OpenGL reads). Step 2 is designed with both chains from the start; the Firestorm mode is not added afterwards. In our mode the chain is chosen for quality and fidelity (user leaves it to us), decided in step 2's design.
- **Not covered:** materials, shadows and water's own fog are the same in both modes. 'Identical' means the atmosphere and its light, not the whole renderer.
- **Check:** side by side with Firestorm OpenGL on the library EEPs and region skies, same place, same time of day. `asrenderer_test` gets `--sky-model` so the formulas are checked without a viewer build.
- **When:** at the start of step 2, together with the exposure and tone map chain, before materials.

## Step 1 reading pass (15 November 2026)

### Where the sky is today

- No EEP data reaches the native renderer. The background is a constant, `SKY = vec4(0.32, 0.45, 0.62, 0.0)` in `asrSceneResolveC.glsl`, stored where the visibility buffer is empty.
- Per-frame data path: the viewer fills `asr::FrameState` in `asrviewer.cpp` (camera, overlay, world view), the render thread copies it into `asr::FrameInput` (`asrdevice.h`). The sky inputs join both. Both are headers: adding fields rebuilds their includers.
- Viewer source of the values: `LLEnvironment::getCurrentSky()` (an `LLSettingsSky`).

### EEP physical units (checked in `llsettingssky.cpp`)

| Value | EEP default | Unit | Sample unit | Translation |
|---|---|---|---|---|
| Planet, sky bottom radius | 6360 | km | km | copy |
| Sky top radius | 6420 | km | km (sample 6460) | copy |
| Rayleigh layer | `exp_term 1, exp_scale -1/8000` | per metre | per km | `exp_scale` × 1000 |
| Mie layer | `exp_term 1, exp_scale -1/1200`, anisotropy 0.8 | per metre | per km | `exp_scale` × 1000, g copied |
| Ozone layers | widths 25000, linear `-1/25000`, `-1/15000`, constants `-2/3`, `8/3` | metres | km | `width` ÷ 1000, `linear_term` × 1000 |
| Sun arc | 0.00045 | radians | - | copy (disc size) |

- **Mixed units:** radii are in kilometres, density profiles in metres. The sample's profile terms are per kilometre, so `exp_scale` and `linear_term` scale by 1000 and `width` by 1/1000. `exp_term` and `constant_term` have no unit.
- **No scattering coefficients in EEP.** Rayleigh scattering, Mie scattering and extinction, and ozone extinction have no settings fields. The sample's Earth values (table above) are the base, and decision 1's legacy translation tints and scales them.
- **EEP's default ozone is not Bruneton's tent.** Its first layer is `linear -1/25000, constant -2/3`, negative at every height below 25 km, so it clamps to zero there; only the falling half (25 to 40 km) exists. Bruneton's tent rises from 10 km (`linear +1/15 km`). EEP values are copied as they are; a sunset that lacks ozone's blue is checked side by side before anything is changed.
- **Single-layer EEP profiles** store their layer first with `width 0` ('the entire atmosphere'). In Bruneton's form that layer goes second, after a first layer of width 0. The translation moves it.
- **The GL viewer does not use the physical model.** Its sky comes from the legacy values (blue horizon, blue density, haze density and horizon, density and distance multipliers); only a leftover uniform name (`single_mie_scattering_texture`) remains. Matching GL's look therefore rests on the legacy translation, not on the physical fields.
