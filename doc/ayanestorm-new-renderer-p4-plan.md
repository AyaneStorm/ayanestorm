# New renderer: P4 work plan (lighting parity)

Status: draft, to be completed with the user. Nothing written. Started 15 November 2026.

Goal (from the main plan): EEP sky with modern atmospherics, sun and moon, stars, clouds, day and night cycle; sun and moon light, shadows, probes, water, terrain lighting, local lights, projectors. Checked by side-by-side captures with the GL viewer, sky and clouds included. Step g (snapshots, scene capture, speed gate) comes after P4.

Studies this plan follows:
- [ayanestorm-new-renderer-sky.md](ayanestorm-new-renderer-sky.md): sky, clouds, volumetric lighting. Agreed.
- [ayanestorm-new-renderer-shadows.md](ayanestorm-new-renderer-shadows.md): shadows. Agreed.

## Rules that apply to every step

- **Computed once:** every result lives in the registry and is read by everyone who needs it. A consumer that is turned off stops nothing else. Results named in the studies keep those names.
- **Real render rule:** correct in every frame, with no history, reprojection or spreading over frames. Lower resolution made in full every frame is allowed.
- **Both backends in one change:** Vulkan and Metal together, tested in `asrenderer_test` on Windows and on the Mac before the viewer.
- **VRAM budget:** every target and table comes from its budget category and has a low tier. The low-tier reference is the GT 1030 and 4 GB cards.
- **Ready for snapshots:** step g's tiled snapshots must work with every effect here. Screen-space passes take a tile overlap, and per-view results (aerial perspective, froxels, masks) are made per tile view. Nothing may assume the view is the screen.
- **Firestorm merges stay reasonable:** logic lives in AS-owned files; LL files get small tagged hooks only.
- **Graphics preferences are honoured (user, 15 November 2026):** where a Firestorm graphics preference applies to an effect, the new renderer reacts to it, live, through the same setting. Where GL's controls do not fit the new renderer, a graphics preferences tab may be added for it, or an existing tab replaced while the new renderer is active. Each step checks its rows in [Graphics preferences](#graphics-preferences) before it is written and updates the table when done.

## Steps

### 1. Atmosphere and sky

- The atmosphere LUTs: transmittance, multiple scattering, sky view, aerial perspective fitted to the draw distance.
- The EEP translation: physical values copied, legacy values converted; units checked.
- Sun disc (limb darkening, EEP texture honoured), moon, stars, EEP 2D clouds lit by the new sky.
- Aerial perspective applied to opaque surfaces and Mac OIT.
- The background replaces today's plain colour.

**Tests:**
- `asrenderer_test` sky scenes: noon, sunset, night, high altitude, a heavy-haze preset.
- Viewer side by side with the GL viewer on LL's library presets and popular region skies.

### 2. HDR, tone mapping, sun, moon and ambient light, materials

- **Lighting in the resolve (decided 15 November 2026, option A).** The opaque resolve rebuilds each visible pixel's surface from the visibility buffer and lights it in the same pass: cluster light lists, shadows, sky results. **No G-buffer.**
  - The effects that need surface data (ambient occlusion, contact shadows, water refraction) read one thin target the resolve also writes: normal and depth, plus roughness if a reader needs it. It is written once and shared through the registry.
  - Mac OIT lights each layer through the same shared functions, so opaque and transparent shading match.
  - **Tiles classified by material kind** (from Nanite, SIGGRAPH 2021): lighting in the resolve makes it heavier, and one shader covering every material kind (legacy, PBR, terrain, fullbright) diverges inside a GPU wave. A small pass first marks which kinds each screen tile holds, then the resolve runs one indirect dispatch per kind over only its tiles, each with its own specialized shader. Mixed tiles are processed once per kind present, each pass taking only its pixels.
  - **Texture gradients:** a compute resolve has no automatic derivatives, so mip selection uses analytic gradients from the triangle's attributes (Nanite measures under 2% overhead). The resolve's current method is checked when step 2 starts, so material maps use the same rule.
  - Rejected, option B: resolve into a G-buffer, then a separate lighting pass. It costs the G-buffer's memory and bandwidth (16 to 28 bytes a pixel, written and read every frame), which the VRAM limit and the GT 1030 tier cannot afford.
- **HDR scene colour and tone mapping: moved to step 1 and done there** (the physical sky cannot be seen without them). Decided 15 November 2026:
  - Tone mappers: LL's two, as GL has them (`RenderTonemapType` 0 Khronos PBR Neutral, 1 ACES Hill), plus AgX as 2. `RenderTonemapMix` and `RenderExposure` work as in GL.
  - Exposure from the sky, not adapted over frames: an incident light meter reading of the frame's sky and sun on a horizontal surface (`exposure` pass), the same for every view and snapshot tile. A floor keeps night dark.
  - Scene colour is linear: texture and vertex colours are decoded from sRGB where they are shaded; the composite exposes, tone maps and encodes sRGB.
- **Direct light:** sun and moon from the transmittance LUT, with EEP sunlight colour.
- **Ambient light:** from `sky ambient SH`.
- **Materials:**
  - LL's legacy materials: diffuse, normal and specular maps, glossiness, environment intensity, shiny, fullbright, emissive mask.
  - PBR (glTF): base colour, normal, ORM, emissive, alpha modes, double-sided, texture transforms.
  - Terrain's normal, ORM and emissive maps (waiting since terrain was written), and the PBR paint map.
- **Avatars and Mac OIT:** lit through the same functions. Mac OIT replaces today's `lit` stand-in with real lighting per layer.
- **Glow/bloom** from emissive materials: also feeds the Live DoF bins later.
- **Open:** ambient occlusion. LL has SSAO; GTAO is the modern choice. Screen-space, so it must handle snapshot tile overlap.
- **Idea, to decide in step 2: skin shading on avatars** (user, 16 November 2026). Pre-integrated skin shading: a soft, reddish falloff across the terminator, done in the shading function itself, with no screen-space pass and no history.
  - **Which faces are skin:** rigged faces whose texture is a Bakes on Mesh ID (`LLAvatarAppearanceDictionary::isBakedImageId()`), plus the system avatar body. This covers every modern body and head (Legacy, Maitreya, Reborn, the men's bodies, LeLutka, Catwa) without relying on attachment names, which users rename and which change between versions. `asrworld.cpp` already detects these faces.
  - **Limits:** the bake also holds tattoos, makeup and painted underwear (fine for the first two; the last to check by eye). Fur and robot avatars use Bakes on Mesh too, so the scatter colour needs a setting and an off switch. Old applier skins are not detected and keep normal shading.

**Tests:** material test scenes in `asrenderer_test` (each map type, legacy and PBR side by side); viewer captures against the GL viewer.

### 3. Local lights

- Point lights (radius, falloff, colour, intensity) and projectors (texture, FOV, focus, ambiance).
- **Light lists per cluster:** the GPU sorts lights into cells of the view once per frame. The cells match the froxel grid of step 5, so lighting and volumetrics read one list.
- Every surface kind reads the same list: opaque, Mac OIT, water, volumetrics.

**Tests:** a light-crowd scene (hundreds of lights) in `asrenderer_test`; club and event places in the viewer.

### 4. Shadows

As the shadow study describes:
- sun or moon cascades with quantized SDSM and a static cache;
- the local light atlas with its static cache;
- PCSS soft edges and Bend-style screen-space shadows;
- `asr_shadow_sun` / `asr_shadow_local` shared by every shader.

**Tests:** the shadow study's measurement list.

### 5. Volumetric lighting

- **Froxel grid,** no history: atmosphere in-scattering with shadows, local fog, local lights, clouds' effect on the haze.
- **Sun and moon shafts exact:** the GL viewer's Exact mode ported (polygonal light volumes on the shared cascades).
- **One result:** `volumetric scattering`, read by the opaque resolve, Mac OIT and water.

**Tests:** side-on and camera-facing shafts, shafts through cloud gaps, a dense-fog club scene; Exact mode against the GL viewer's.

### 6. Water

**Study agreed:** [ayanestorm-new-renderer-water.md](ayanestorm-new-renderer-water.md) (15 November 2026): planar reflections with tiers, LL's EEP Fresnel kept, ripples and wakes added. What follows is the study's scope and its inputs.

It covers:
- SL water planes per region, and EEP water settings (normal map and its scroll, fog colour and density, reflection and refraction scales, Fresnel, blur);
- reflections: planar against probes against screen-space, and their cost with the real render rule;
- underwater fog, and the water line crossing the camera;
- how water reads the sky, shadow, volumetric and light-list results.

**Inputs gathered so far:**

From LightSpeed Studios' GDC 2023 talk 'Photon Water System: open world water rendering and real-time simulation'. SL water is a flat plane per region plus the endless ocean around regions, with EEP's scrolled normal maps; there are no rivers, flow or waves to simulate, so the talk's fluid half (shallow water, flow-map baking, waterfalls) does not apply.

- **Surface work split from shading.** Their pipeline:
  1. a depth-only pre-pass of the flat water surface;
  2. one compute pass that makes screen-space displacement, normal and foam buffers from the world position;
  3. the water shading pass reads those buffers.

  The buffers can be smaller than the screen on low-end hardware. This fits the registry: `water surface` (normal, foam, displacement) is made once and read by shading, reflections, refraction and caustics. Its resolution is a budget tier.
- **Interactive ripples (Kass and Miller 1990 height propagation):** a cheap wave simulation in a tiling domain that follows the camera, giving ripples and wakes from avatars, boats and moving objects in the water. Their setup:
  - three R16F heights (previous, before that, current) plus two R8 masks (moving objects, boundaries);
  - 1024² over 80 × 80 m, under 0.1 ms at 512²;
  - Neumann boundaries (waves bounce off shores).

  They run one step per frame with damping. We use fixed time steps instead, so the waves don't depend on the frame rate. A simulation keeps its own state from frame to frame, which is not reprojection: the real render rule allows it. **A candidate SL feature; the GL viewer has nothing like it.**
- **Water depth map from above:** they draw the terrain and the objects touching water from above, static content once and moving content every frame, to get water depth for the simulation's boundaries. In SL the terrain heights are already known. Objects use the same static-plus-moving cache as the shadow maps. Water depth also gives shore foam and colour absorption by depth.
- **Screen-space tessellation** (Far Cry 5, GDC 2018): a grid of quads a few pixels wide, displaced from the pre-pass depth, gives a fine mesh near the camera at no geometry cost. It is only needed if ripples move the surface itself; with small ripples, normals alone may be enough. To be decided in the study.
- **One shaded water layer:** water is shaded once, after opaque surfaces, reading the opaque colour and depth for refraction and absorption, not inside Mac OIT. Transparent surfaces under and over water then need their order settled in the study.
- **Reflection cost:** Lumen reflections on their water cost 1 ms or more, the largest item in their frame. Reflections are the main question of the water study, together with probes (step 7).
- **Caustics** (user's question, 15 November 2026: ripples' bright lines and dark bands on the underwater ground):
  - each frame a compute pass refracts the sun through the water surface (EEP's scrolled normal maps + ripples), seen from the sun's direction;
  - it measures how much the refracted light bunches or spreads, from the change in area of a refracted grid;
  - result: a small `water caustics` map in sun light space (512² to 1024²).

  Lighting reads it like `sun shadow` for anything under the water (ground, objects, avatars), and the froxel grid reads it for underwater shafts.

  **Design consequence:** the water surface lives in **world space** around the camera. A point on the bottom is lit through ripples that may be off screen. The ripple simulation and EEP's tiled normal maps are world space already. The screen-space `water surface` buffers for shading are derived from it.
- **Not taken:**
  - CDLOD water meshes (SL water is flat; terrain LOD exists);
  - FFT ocean waves (EEP has no wave heights, so they would change every region's look);
  - foam simulation beyond shore foam from water depth.

### 7. Reflection probes

**Needs a short study first.** The main plan requires probes to beat the GL viewer, which updates one probe at a time. To cover:
- LL's probe kinds (manual, automatic) and their sizes;
- how many refresh in one frame, using multiview through the visibility path;
- caching of unchanged probes;
- irradiance from probes;
- screen-space reflections: LL has them; keep, replace or combine with probes.

### 8. Volumetric clouds

- The high tier, following the camera.
- Energy-conserving march, multiple scattering by octaves, dual-lobe phase function.
- Aerial perspective at the transmittance-weighted depth; clouds feeding back into aerial perspective.
- `cloud shadow` read by every `sun shadow` reader.
- EEP 2D clouds stay as the low tier.

**Tests:** the cost on the 3080 Ti and M4 decides whether the tier is kept.

### 9. Measurements and tiers

- Moving sun: options 1 and 2 (shadow study).
- Cloud cost; the low tier on the GT 1030.
- The VRAM of every new result against the budget categories.
- Side-by-side captures of the whole of P4 against the GL viewer.

## Graphics preferences

From Firestorm's graphics preferences panel (`panel_preferences_graphics1.xml`; the advanced floater repeats some of them). Only the ones a P4 step touches are listed. What each setting means for the new renderer is decided in its step, against what the GL viewer does with it.

| Setting | Step |
|---|---|
| `RenderFarClip` (draw distance) | 1: aerial perspective range |
| `RenderQualityPerformance` (quality slider) | 1, 8, 9: sky tier (LUT sizes, volumetric clouds) |
| `RenderDisableVintageMode` (HDR, PBR emissive) | 2: what the off state means here is decided then |
| `RenderExposure`, `RenderTonemapType`, `RenderTonemapMix` | 1: tone mapping (library done; viewer hookup with step 1's viewer work) |
| `RenderHighPrecisionPostProcess` | 2: HDR target format |
| `RenderGlow`, `RenderGlowIterations`, `RenderGlowResolutionPow` | 2: glow |
| `RenderDeferredSSAO` | 2: ambient occlusion |
| `RenderLocalLightCount`, `RenderShaderLightingMaxLevel` | 3: local lights |
| `RenderShadowDetail`, `RenderShadowResolutionScale` | 4: shadows |
| `RenderTransparentWater` | 6: water |
| `RenderScreenSpaceReflections`, `RenderMirrors`, `RenderHeroProbeResolution`, `RenderHeroProbeUpdateRate` | 6, 7: reflections |
| `RenderReflectionProbeDetail`, `RenderReflectionProbeLevel`, `RenderReflectionProbeCount` | 7: reflection probes |

### Read by the new renderer today (15 November 2026)

Only `RenderMaxVRAMBudget`, `RenderTextureVRAMDivisor`, `RenderVSyncEnable` (`asrviewer.cpp`), and `RenderTerrainScale`, `RenderTerrainPBRScale`, `RenderTerrainPBRPlanarSampleCount`, `RenderTerrainPBRTriplanarBlendFactor` (`asrworld.cpp`). Whether `RenderFarClip` reaches it through the viewer camera is not checked yet.

### Settings outside P4, to check (user, 15 November 2026)

Every graphics slider is checked against what GL does with it and replicated where it applies: LOD factors (`RenderVolumeLODFactor`, `RenderTreeLODFactor`, `RenderTerrainLODFactor`, `RenderAvatarLODFactor`, `RenderAvatarPhysicsLODFactor`, `RenderFlexTimeFactor`), `RenderMaxPartCount`, `RenderMaxTextureResolution`, `RenderAnisotropicLevel`, `RenderFSAAType` and `RenderFSAASamples`, `RenderCASSharpness`, avatar complexity and impostors (`RenderAvatarMaxComplexity`, `RenderAvatarMaxNonImpostors`, `IndirectMaxComplexity`, `IndirectMaxNonImpostors`). The new renderer may give a slider's steps a different meaning where its methods differ.

### The quality slider

`RenderQualityPerformance` (notches 1 to 7) applies one list of `featuretable.txt` per notch. Its values (blank: that notch does not set the setting):

| Setting | Low | LowMid | Mid | MidHigh | High | HighUltra | Ultra |
|---|---|---|---|---|---|---|---|
| `RenderFarClip` | 64 | 96 | 128 | 128 | 128 | 128 | 256 |
| `RenderDisableVintageMode` | 0 | 0 | | | | | |
| `RenderHighPrecisionPostProcess` | 0 | 0 | 0 | 0 | 0 | 1 | 1 |
| `RenderExposure` | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| `RenderTonemapType`, `RenderTonemapMix` | 1, 0.7 | same | same | same | same | same | same |
| `RenderGlowResolutionPow` | 8 | 8 | 9 | 9 | 9 | 9 | 9 |
| `RenderDeferredSSAO` | 0 | 0 | 0 | 0 | 1 | 1 | 1 |
| `RenderLocalLightCount` | 8 | 256 | 512 | 1024 | 2048 | 4096 | 8192 |
| `RenderShadowDetail` | 0 | 0 | 0 | 0 | 1 | 2 | 2 |
| `RenderTransparentWater` | 0 | 0 | 0 | 0 | 1 | 1 | 1 |
| `RenderReflectionsEnabled` | 0 | 1 | 1 | 1 | 1 | 1 | 1 |
| `RenderReflectionProbeDetail` | 0 | 0 | 0 | 1 | 1 | 1 | 1 |
| `RenderReflectionProbeLevel` | 0 | 0 | 1 | 1 | 2 | 3 | 3 |
| `RenderReflectionProbeCount` | 1 | 32 | 64 | 64 | 128 | 256 | 256 |
| `RenderScreenSpaceReflections`, `RenderMirrors` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `RenderHeroProbeResolution` | 256 | 256 | 512 | 512 | 512 | 1024 | 2048 |
| `RenderHeroProbeDistance` | 4 | 6 | 6 | 6 | 8 | 16 | 16 |
| `RenderHeroProbeUpdateRate` | 6 | 3 | 3 | 2 | 2 | 2 | 2 |
| `RenderHeroProbeConservativeUpdateMultiplier` | 16 | 16 | 16 | 8 | 8 | 4 | 4 |
| `RenderVolumeLODFactor` | 1.5 | 2 | 2 | 2 | 2 | 2 | 3 |
| `RenderTreeLODFactor` | 0 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 1 |
| `RenderTerrainLODFactor` | 1 | 1 | 2 | 2 | 2 | 2 | 2 |
| `RenderAvatarLODFactor` | 0 | 0.5 | 1 | 1 | 1 | 1 | 1 |
| `RenderAvatarPhysicsLODFactor` | 0 | 0.75 | 1 | 1 | 1 | 1 | 1 |
| `RenderFlexTimeFactor` | 0 | 1 | 1 | 1 | 1 | 1 | 1 |
| `RenderAvatarMaxNonImpostors` | 3 | 5 | 7 | 9 | 11 | 16 | 16 |
| `RenderAvatarMaxComplexity` | 35000 | 100000 | 200000 | 250000 | 300000 | 350000 | |
| `RenderMaxPartCount` | 256 | 2048 | 4096 | 4096 | 4096 | 4096 | 8192 |
| `RenderMaxTextureResolution` | 512 | 1024 | 2048 | 2048 | 2048 | 2048 | 2048 |
| `RenderAnisotropicLevel` | 0 | 2 | 2 | 4 | 4 | 4 | 8 |
| `RenderFSAAType`, `RenderFSAASamples` | 0, 0 | 0, 0 | 1, 1 | 1, 1 | 2, 2 | 2, 2 | 2, 3 |
| `RenderCASSharpness` | 0 | 0 | 0 | 0 | 0.4 | 0.4 | 0.4 |
| `RenderTerrainDetail` | 0 | 1 | 1 | 1 | 1 | 1 | 1 |
| `RenderTerrainPBRDetail` | -4 | -1 | 0 | 0 | 0 | 0 | 0 |
| `RenderTerrainPBRPlanarSampleCount` | 1 | 1 | 1 | 1 | 3 | 3 | 3 |

**The new renderer may redefine the notches (user, 15 November 2026).** Its own tiers (step 9, low tier = GT 1030 and 4 GB cards) set what each notch means for it: which LUT sizes, cloud tier, shadow and probe budgets. A notch keeps setting the shared settings above, so moving the slider still moves the individual controls. **Proposal, not agreed:** the new renderer's own tier settings (`AS` prefix) join the same lists; how they apply only while it is active is checked in step 9.

## Order and dependencies

- 1 before 2: lighting reads the sky's results.
- 2 before 3: local lights use the same material and light functions.
- 4 can start after 2. 5 needs 3 and 4 (light lists, shadows).
- 6 and 7 need their studies; they can be studied while 1 to 3 are built.
- 8 needs 1 and 4 (atmosphere, shadow registry).

## Open items, to complete

- ~~Tone mapper choice (step 2).~~ Decided 15 November 2026: see step 2's HDR entry.
- Ambient occlusion method (step 2).
- Probes study (step 7).
- Whether glow/bloom stays a P4 item or comes with the native DoF.
- **All views in one pass** (from Nanite's multiview): one culling and drawing chain serves an array of views (main, planar reflection, every shadow cascade, every local light map), each element tagged with its view, instead of one chain per view. Nanite saw up to 100 times faster in extreme cases against separate calls. To be designed with step 4, so shadows, reflection and later probes share it from the start.
- **Outside P4, to measure: a software rasterizer for tiny triangles.** Hardware rasterizers work in 2 × 2 pixel quads and waste most of their work on triangles of one pixel or less. In SL that means dense avatar bodies and attachments in a 150-avatar crowd, and far mesh.
  - Nanite rasterizes clusters whose triangles are under about 32 pixels in compute: each pixel does a 64-bit atomic max of depth and ID into the visibility buffer.
  - It needs 64-bit image atomics: on Vulkan an extension (`VK_EXT_shader_image_atomic_int64`); on Metal, support is to be checked and may be limited to newer Apple GPUs.
  - A candidate for the crowd work, once lighting gives a real frame to measure.
