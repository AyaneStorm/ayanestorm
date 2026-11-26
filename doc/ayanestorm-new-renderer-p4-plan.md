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
- **Batch 1, lighting core: written 26 November 2026, tested in `asrenderer_test` on Vulkan (0 validation errors); Metal and viewer unbuilt.**
  - The exposure pass, which already samples the sky, also leaves the sun's and moon's light after the air at the camera, their directions, and the ambient as order-2 spherical harmonics, already convolved to irradiance (Ramamoorthi and Hanrahan 2001): made once per frame in `GpuExposure` (now 256 bytes).
  - The ground below reflects `BOUNCE_ALBEDO` 0.2 of all the light falling on it into the harmonics (constant and z terms only). The atmosphere's own ground albedo is 0, as EEP skies are fitted without it, so it cannot serve.
  - `lightSurface()` in `asrSceneCommon.glsl` replaces the placeholder in the resolve and in every Mac OIT layer: diffuse only, until materials bring specular.
  - The albedo target is gone: nothing read it (4 bytes a pixel). Binding 1 of set 1 stays free, so no other binding moved.
  - **Batch 2b, LL's legacy materials: written 26 November 2026; tested in `asrenderer_test` on Vulkan and Metal (0 validation errors, same images); viewer unbuilt.**
    - `GpuMaterial` grows to 208 bytes: its first 80 unchanged, then three more maps (normal; specular, later PBR's ORM; emissive), each with its own texture and UV transform, legacy specular colour and glossiness, environment intensity, and flags (normal map, specular map, fullbright, emissive alpha).
    - `asrSceneLighting.glsl` holds `shadeLegacy()`, shared by the resolve's surface pass and Mac OIT's colour pass: OpenGL's normalized Blinn-Phong (exponent glossiness squared times 368, its Fresnel and shadowing terms) for sun and moon, its glossy and legacy environment terms, and the emissive mix. OpenGL's light is radiance-like, ours irradiance, so its sunlight is ours over pi. Fullbright and emissive colours are divided by the exposure, so they show as OpenGL shows them in any light.
    - Environment until reflection probes (step 7): the sky in the reflected direction, the ambient's lower half below the horizon, and the ambient for low glossiness.
    - Mac OIT now lights only in its colour pass; the first two need alpha alone.
    - Viewer (`fillLegacyLighting()` in `asrworld.cpp`), as OpenGL: every face starts from the shiny level as specular colour, glossiness and environment (none for alpha-masked faces without a material), and only a material with a specular map replaces them with its colour, exponent and environment. A first version gave materials without a specular map `LLDrawInfo`'s default white at 0.5, which OpenGL always overwrites: normal-mapped bark, bushes and hair shone like metal at sunset (user's capture). Normal and specular maps take the material's own repeats, offset and rotation, or the face's under texture animation. A map is used once its texture has an image: the viewer's default image would make a wrong normal map.
    - A mesh without tangents keeps its normal under a normal map (a zero tangent made NaN, which bloom spread over the whole frame).
    - **LL's built-in bump maps: written 26 November 2026, `asrenderer_test` on Vulkan and Metal (0 validation errors); viewer unbuilt.** As OpenGL: bump codes 1 to 17 where no material normal map replaces them, read with the colour's UVs, the map's alpha not taken as glossiness (`bumpF.glsl`). The 15 standard images are the viewer's own, turned into normal maps when they load (`onSourceStandardLoaded()`, which runs in native mode too: `sRenderDeferred` is always true). Brightness and darkness: OpenGL renders a mipmapped normal map per source texture (`normgenF.glsl`); here the same normal is worked out where the face is shaded, from five reads of its colour texture at the mip being sampled, its steps scaled back to full-size texels. No texture per bumped image, so no VRAM for it.
  - **Batches 1 and 2a on Metal: tested in `asrenderer_test` on the M4 the same day**, 0 validation errors; mean colours match Vulkan in every scene (sun +30, -4, and +13 looking at the sun). Metal has no atomics on a vector's element, so the dispatch header is plain words.
  - **Batch 2a, tiles by material kind: written 26 November 2026, tested in `asrenderer_test` on Vulkan (0 validation errors).** `asrSceneClassifyC` ORs the kinds of each 8 by 8 tile's pixels (sky, surface, terrain; PBR to come) and appends the tile to each kind's list, counting an indirect dispatch per kind (`GpuResolveTiles`, its address in the view's former padding). The resolve body is `asrSceneResolve.glsl`, built once per kind (`asrSceneResolve{Sky,Surface,Terrain}C`); each pass takes only its own pixels of a mixed tile. Pixels the resolve cannot place go to the surface pass, which alone reports them. Per-kind shaders are 70 to 106 KB of SPIR-V against 163 KB for the old one. Check: a fixed camera's sky matches the old resolve within 4/255.
  - Measured (starley, city view, ground mean 0-255): sun +60: 84, +30: 92, +5: 94, 0: 91 warm, -4: 50 pink-violet, -10: 20, -25 without moon: 2. A white surface facing up reflects exactly the sky's mean brightness, so physical lighting alone cannot make a moonless ground brighter than its sky.
  - **Moonless night ground: kept physical (user, 26 November 2026).** With the sun at -25 degrees and no moon, the ground reads 2/255 at `ASRendererExposureAdaptation` 0.7. Raising adaptation lifts the sky with it (0.8: sky 21, ground 11) and greys the night sky: rejected. EEP ambient as a minimum light and a night-vision lift were offered and declined: moonless nights stay dark, and in-world lights (step 3) light the scene. To revisit only if that does not work.
  - **Dull sky from sun +9 degrees (open from step 1, revisit after this step):** looking at the sun, the sky's mean stays about (72, 92, 131) from +9 up, greyish rather than dark, while the unlit ground sits near 30/255. Judge again once the ground is lit; no sky-mean guard before that.
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

- **Batch 1, written 26 November 2026; Vulkan `asrenderer_test` shows shadows, 0 validation errors; Metal and viewer unbuilt.** Timings, 3080 Ti, city scene, sun +30: GPU total 0.86 ms without shadows, 2.33 ms with (shadow draw 1.31, early cull +0.06, shadow groups 0.06, resolve +0.02); stall test passes in both.
  - `shadow fit` (`asrShadowFitC`): 4 cascades centred on the camera, each out to its split (a fixed area whichever way the camera looks, so batch 2's cache survives turning), centre snapped to whole texels. Reach as OpenGL's: twice the farthest depth seen last frame (pyramid's 1 by 1 level), 16 to 512 m, within the draw distance. Light: the sun when above the horizon, else the moon.
  - The early cull tests each instance against the main view, then every shadow view: one load, size bound and LOD per instance. Bounds are transformed once and tested against each view left in a mask; groups go to `shadow groups` with that mask. No facing test and culling off when drawn, as OpenGL's shadow pass. Shadow casters ask for skinning before `skin vertices`, first request wins.
  - `shadow draw`: one indirect draw per list for all cascades; the vertex shader writes `gl_Layer` from the entry's view tag. Depth clamp keeps casters beyond the light side. D16.
  - Alpha as OpenGL's `shadowAlphaMaskF.glsl`: cut at the material cutoff, then times the colour alpha (not fullbright); under 0.05 none, under 0.88 every other column.
  - `asr_shadow_sun()` / `sunMoonShadow()` in `asrShadow.glsl`: finest cascade holding the point, normal offset 1.5 texels, depth bias 1 texel, 3 by 3 tent PCF from 4 gathers (no comparison sampler), fade over the last 10% of the reach. Shadows the sun or moon term and its highlight in the resolve (surface, terrain) and Mac OIT.
  - Preferences: `RenderShadowDetail` 0 turns shadows off (a 1 texel map stays bound); `RenderShadowResolutionScale` scales the tier's size (2048, 1024 under a 6 GB budget), 512 to 4096, the map remade when it changes.
  - Debug view 8: each pixel's cascade as a colour, darker where shadowed. Test flags `--debug-view`, `--shadow-detail`, `--shadow-scale`.
  - First count, city scene: 267k shadow entries against 49k drawn on screen (no occlusion in shadow views). Batch 2's static cache is what brings this down.
- **Batch 2a, static cache: written 26 November 2026; Vulkan `asrenderer_test` 0 validation errors, cascades checked in debug view 8 with the camera orbiting; Metal and viewer unbuilt.** As the shadow study's item 2 (shift, static into dirty tiles, store, moving on top). Store: `INSTANCE_MOVING` while a transform changed in the last 30 frames, world spheres of stale cached shadows (`GpuSceneView::shadowDirty`), a full redraw when an LOD in use arrives. Static casters take each cascade's LOD from its texel. `ASRendererShadowSunStep` (0.25 degrees; 0 follows the light) and test flag `--shadow-sun-step`.
  - City scene, 3080 Ti, sun +30, camera orbiting: GPU total 2.33 ms uncached, 1.37 ms cached (shift 0.18, store 0.10, draws 0.05; 17.5k moving clusters, the test moves objects every frame); 0.87 ms without shadows. Half resolution: 1.00 ms.
  - **Metal, same day: batches 1 and 2a tested in `asrenderer_test` on the M4, 0 validation errors**; the city image's mean matches Vulkan's within 0.1/255, shadows darken it by 13/255. GPU total 5.30 ms without shadows, 6.59 ms with (shift 0.66, static redraw 0.84 while the camera orbits). `asr_spirv2msl` keeps set 1 bindings 20 and 21 at their numbers.
  - **Viewer feedback, same day:** a window's light patch came and went (per-cascade LOD closing SL low-LOD holes): replaced by one LOD per instance chosen in a `lod select` cull job, which marks a changed static instance's tiles dirty. Leaf shadows striped when zoomed (OpenGL's every-other-column dither under a 3 by 3 tent): the filter is now two bilinear taps a texel apart each way, which weighs odd and even columns alike. Avatars cast no visible shadow and `shadow moving` costs 2.7 to 3.1 ms in a club: under investigation, static/moving counts added to the viewer's log line.
  - **Viewer log, busy region (26 November 2026):** 56 to 76 moving instances but about 15,800 rigged faces in reach; 1.9 to 2.2 million moving shadow entries, `shadow moving` 7.0 to 7.5 ms of a 13.5 ms frame. The cost is avatars, redrawn into every cascade they touch (nested around the camera, up to 4), every frame. Also fixed that day: moving draws had a vertex count of 0 (no avatar shadows at all); fixed reach and log splits from 1 m (4.8, 23, 108, 512 m) after flicker on glossy earrings.
  - **Next, agreed (user, 26 November 2026): receiver culling for moving casters (cut 1).** After this frame's depth: mark, per cascade, the dirty-tile-sized tiles holding a visible pixel that reads that cascade; cull moving casters into a view only where their footprint touches such a tile. Moving casters then move after the main depth (cull after pyramid final, skinning for off-screen casters in the late skin pass, then the moving draw). Plus: skip a caster in a view when it is under a texel there. Exact in every frame. **Exception to the real render rule (user, 26 November 2026):** shadows are recomputed only every 30 ms (`ASRendererShadowInterval`, 0: every frame), "shadows at 30 fps would be invisible". Between updates every frame reads the last maps through the views they were drawn with (world-anchored, so the camera can move); dirty spheres and LOD marks gathered meanwhile are applied at the next update. Built before receiver culling. **Narrowed the same day:** a 30 ms old map made turning avatars sit inside their own older shadow (dark flicker in full sun, gone at interval 0), so moving casters are drawn every frame; the interval now covers only the static cache (cascade placement, strips, changed objects). Hair over skin flickered once the nearest cascade reached 4.6 mm texels: the PCF is now 4 bilinear taps a texel apart each way (3 by 3 gathers, texels weighted 1-f, 1, 1, 1, f), still even over odd and even columns; resolve 0.10 to 0.18 ms in the city test.
  - **Receiver culling written the same evening** (tested Vulkan + Metal, 0 validation errors): city test moving entries 17,290 to 6,683. **Open, blocking: one-frame flicker at static cache updates.** Measured with `--calm --frames DIR N --camera-speed S`: pixels that jump and return within three frames stay at the scene's own texture shimmer (about 3,300 a frame) at `--shadow-interval 0`, but burst to 10,000 to 21,000 on update frames at 30 or 500 ms: sunlit faces go dark, or lose their shadow, for one frame. Update frames and the frames between them read different maps; to be found before any viewer build.
  - Open: on the M4 the shift pass, a fragment depth write over every texel, costs 0.66 ms; to look at.
  - Open: the street scene redraws about 2k static clusters a frame while walking (0.34 ms, noisy); clusters are rasterized over the whole layer, not only their dirty tiles. The stall check fails in the street scene with shadows off too: not from shadows.

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

- **Reordered 26 November 2026 (user):** after step 2's legacy materials, LL's built-in bump maps, then shadows (step 4), then the probes study and probes (step 7), and only then PBR (step 2's batch 3). Legacy shading follows OpenGL line for line, but OpenGL multiplies its sun highlight by the shadow and takes both environment terms from reflection probes and screen-space reflections. With the sky alone (OpenGL's own path with probes off), tiles, bricks and hair shone like metal at sunset. PBR depends on the environment even more, so it is built once against real shadows and probes. Cost: PBR faces keep their base colour only until then. Debug views 6 (no environment terms) and 7 (no highlights) compare the two terms meanwhile.
- **Local lights placed 26 November 2026 (user):** sun and moon cascades, then local lights (step 3) with the shadow atlas, then probes, then PBR. Shadows are finished in one go, while the shadow view array is fresh.

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
