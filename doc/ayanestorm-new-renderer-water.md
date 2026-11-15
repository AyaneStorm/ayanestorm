# New renderer: water study (P4 step 6)

Status: study agreed (decisions at the end), nothing written. 15 November 2026.

Inputs:
- the GDC 2023 'Photon Water System' notes and the caustics design, in [ayanestorm-new-renderer-p4-plan.md](ayanestorm-new-renderer-p4-plan.md) step 6;
- LL's water code: `LLSettingsWater`, `lldrawpoolwater.cpp`, `lldrawpoolwaterexclusion.cpp`, `class3/environment/waterF.glsl`.

## What SL water is

- **Shape:** a flat plane per region at the region's water height (`LLViewerRegion::getWaterHeight()`, usually 20 m), plus void water: the endless ocean beyond the regions, at the same height. Estates can change the height, so neighbouring regions may differ.
- **EEP water settings** (`LLSettingsWater`):

  | Setting | What it does |
  |---|---|
  | `normal_map` | the wave normal texture |
  | `wave1_direction`, `wave2_direction` | scroll directions of the two layers |
  | `normal_scale` | 3 components, one per wave layer |
  | `fresnel_scale`, `fresnel_offset` | strength and minimum of the reflection |
  | `scale_above`, `scale_below` | refraction distortion above and below the surface |
  | `blur_multiplier` | blur of the reflection; LL uses it as roughness |
  | `water_fog_color`, `water_fog_density`, `underwater_fog_mod` | the underwater medium |
  | `transparent_texture` | not used by today's shaders; to be checked |

- **How LL draws it today:**
  - refraction: the scene behind the water (`mWaterDis`), offset by the normals, with a depth test so things above the water are not pulled in;
  - reflection: reflection probes plus screen-space reflections, using `blur_multiplier` as roughness. LL removed planar water reflections with the PBR viewer.
- **Water exclusion:** faces with the alpha-gradient texture (`IMG_ALPHA_GRAD`), no normal or specular map and 0 transparency hide the water where they are seen, which is how boats keep dry hulls. LL draws the water planes white into a screen mask, then those faces black with the depth test, and the water shader discards masked pixels.

## 1. Reflections

| Method | Quality | Cost |
|---|---|---|
| **Planar reflection** (the scene drawn again, mirrored by the water plane) | **Exact** for flat water, off-screen content included. Ripples and normal maps distort the lookup; rough water blurs it through mips. | A second view: culling, geometry and resolve at the reflection's resolution. Skinned positions are shared (no second skinning); sun shadows are the same results. Its own aerial perspective volume (cheap). |
| Probes + screen-space reflections (LL today) | Approximate: probes are low-resolution and placed in the world, not at the water; screen-space reflections lose everything off screen and behind objects. | Cheap, but screen-space reflections need history to be clean. |
| Probes only | Blurry, misplaced. | Cheapest. |

**Recommended: planar reflection** as the default, the only lossless one.
- It renders every frame, through the same GPU visibility path as the main view.
- **Tiers:** resolution (full, half, quarter) and content (LL's `RenderReflectionDetail` levels: terrain, plus static objects, plus avatars, everything).
- A distorted lookup that leaves the reflected image falls back to the sky view LUT, then probes.
- **One plane per frame:** the water height covering the most of the screen. Other visible heights (rare in SL) use probes, and the doc logs it once.
- **Rough water:** a mip chain of the reflection, sampled by `blur_multiplier` and the distortion.
- **Clip plane:** only what is above the water is drawn into the reflection. In the visibility path this is a per-view plane, and an object crossing it is cut per pixel.

## 2. The water surface

- **World space, around the camera** (needed by caustics, see the P4 plan): a clipmap of 3 levels (about 64, 256 and 1024 m), each holding height and normal.
  - It is built from EEP's two scrolled normal-map layers with their scales. The finest level adds the ripple simulation, if chosen (decision 3).
  - It is made once per frame as `water surface`.
- **Screen-space buffers** for shading are read from it at each pixel's world position; there is no separate screen pass.
- **Displacement:** none by default (EEP has no wave heights). Ripples change normals only. Screen-space tessellation stays possible if ripples ever need to move the surface.

## 3. Shading

- **Fresnel:** LL's form (`fresnel_scale`, `fresnel_offset`) is kept as given by EEP, so creators' water keeps its look. A physical Schlick term would change every region's water.
- **Refraction:** the opaque scene colour and depth from before the water, offset by the normal times `scale_above`. The depth test on the offset sample stays, so nothing above the water is pulled in.
- **Underwater medium** (from EEP fog):
  - extinction comes from `water_fog_density`, the in-scatter colour from `water_fog_color`, lit by the sun's transmittance through the surface and the caustics;
  - it is applied along the refracted path's length under water: Beer-Lambert absorption, with depth giving the colour shift.
  - LL's formula is `exp(-density × distance)` towards the fog colour. The physical form matches it when the light is neutral, and adds caustics and shadowing to it.
- **Lighting:** sun and moon (`sun shadow`, transmittance), ambient (`sky ambient SH`), local lights (cluster lists), all through the shared functions.
- **Shore:** water depth (from terrain heights and the static-plus-moving object map from above) gives shore foam and shallow-water colour.

## 4. Underwater and the water line

- **Underwater medium in the froxel grid:** cells below the water plane get the water's extinction and scattering (times `underwater_fog_mod`) instead of air. Local lights and caustic-lit shafts are then part of the same grid.
- **Exact sun shafts across the surface:** the Exact integral assumes one even medium per ray. A ray crossing the flat water plane is split at it into two even pieces, air and water, each integrated exactly. The light entering the water is lowered by the surface's transmission, and caustics shape the shafts.
- **Camera at the water line:** the near plane crosses the water plane, so each pixel picks its medium from where its near-plane point lies. The surface seen edge-on draws a thin line where the two meet.
- **Surface from below:**
  - inside the critical angle (about 48.6° from straight up) the surface refracts the sky and the world above: Snell's window;
  - outside it, total internal reflection mirrors the underwater scene. That uses probes and the underwater medium's colour; no planar view from below.

## 5. Transparency and water

- Water is one shaded layer after the opaque surfaces, not a Mac OIT layer: Mac OIT cannot refract what is behind a layer.
- **Mac OIT fragments are split by side.** Each blended fragment is tested against the water surface depth at its pixel:
  - **behind the water:** resolved first, into the colour the water refracts;
  - **in front of it:** resolved after the water, over it.

  One set of Mac OIT passes with a side bit per fragment, two resolves. The design is checked against Mac OIT's per-pixel state budget (24 bytes a pixel at K = 4): the split must not double it. If it does, the behind-water side takes a smaller K.
- Glass crossing the water (an aquarium wall, a boat window) then works per fragment.

## 6. Water exclusion

- **The same rule as LL:** alpha-gradient texture, no normal or specular map, 0 transparency.
- These faces become their own instance kind. Per view (main and reflection), they are drawn into an exclusion mask with the depth test, as LL does. Water, caustics and the water-surface lookup skip masked pixels.
- **To check in LL:** whether a camera inside an excluded hull below the water line still gets underwater fog, and match it.

## 7. Void water and region edges

- Void water is the same plane and shader, drawn to the horizon with aerial perspective. It reads the same `water surface` clipmap (the coarsest level tiles) and is reflected by the same planar view.
- **Regions with different water heights:** each is its own plane in the depth pre-pass. The edges between them are kept as LL draws them (edge water tiles, `pushWaterPlanes(1)`).

## Registry results

| Result | Made | Read by |
|---|---|---|
| `water surface` (clipmap: height, normal) | once per frame | water shading, caustics, refraction, foam |
| `water reflection` (colour + mips), per view with water | once per frame, if water is visible | water shading |
| `water caustics` (sun light space) | once per frame | lighting under water, froxel grid |
| `water depth` (map from above: terrain + objects, static cached) | static once, moving every frame | shore foam, shallow colour, ripple boundaries |
| `water exclusion`, per view | per view | water, caustics |
| `ripples` (if chosen) | fixed time steps | `water surface` finest level |

## Tests

- **`asrenderer_test`:** a water scene with a pier, a hull with an exclusion face, glass crossing the surface, a sloped shore, a submerged object; sun at noon and at sunset; camera above, at the water line and below.
- **Viewer:** side by side with the GL viewer on LL's default water and popular EEP water presets; boats; underwater builds.
- **Costs:** the planar reflection at each tier, the clipmap, caustics, the Mac OIT split.

## Decisions for the user

1. **Reflections: planar with tiers** (user, 15 November 2026), as section 1 describes.
2. **Fresnel: LL's EEP form kept** (user, 15 November 2026).
3. **Ripples and wakes: yes** (user, 15 November 2026). The simulation:
   - a tiling domain that follows the camera, run in fixed time steps;
   - sources: avatars' skinned bodies and moving objects crossing the water plane;
   - boundaries from `water depth`;
   - feeds the finest `water surface` level, and so shading and caustics.
