# AyaneStorm Star Settings — Plan

## Context
Real night skies (and DoF bokeh of stars) show coloured stars: blue-white hot stars, yellow/orange/red cool ones. The viewer's stars are near-white with a faint green tint, so defocused star bokeh looks flat. Goal: a viewer-local **Stars** panel (Environment Effects tab + standalone floater + toybox command with its own icon) to tweak star colour, density, brightness, size and twinkle. Defaults reproduce stock look.

## Current stock behaviour (verified)
- `llvowlsky.cpp` `initStars()` (L338): 1000 stars, random upper-hemisphere positions on the dome; intensity `min(rand^2+0.1, 1)`; colour `R,B = 0.75+rand*0.25, G = 1` (greenish white). Uses global `ll_frand()` (different every session).
- `updateStarGeometry()` (L513): 6 verts/star quad, size `16 + rand*20`, per-vertex RGBA8 colour (alpha = intensity).
- `drawStars()` (L292) draws `getStarsNumVerts()*4` verts → only the first **666** stars are visible (stock bug; buffer holds 1000×6).
- `updateGeometry()` only runs on rebuild (WLSkyDetail change, GL restore), so stars are static between rebuilds.
- `lldrawpoolwlsky.cpp` `renderStarsDeferred()` (L237): `custom_alpha = EEP star brightness / 500`; star texture = EEP bloom texture; DoF hooks already present (`as_twinkle_mean`, star mask pass, frozen rotation).
- `starsF.glsl`: `col.rgb *= vertex_color.rgb`; twinkle = `fract(screenpos.x+screenpos.y)` (DoF replaces by mean 0.5 while accumulating).

Colour therefore comes entirely from per-star vertex colour → realism can be added on the CPU side with zero shader cost. The DoF star mask and snapshots reuse the same vertex buffer, so coloured bokeh works in live and snapshot DoF with no DoF changes.

## Design

### New module `indra/newview/asstars.h/.cpp` (namespace `ASStars`)
- `registerUICallbacks()`: `ASStars.ResetDefault` (whitelisted control reset, same pattern as `ASAurora::registerUICallbacks` in `asaurora.cpp:38`), `ASStars.RandomizeSeed`; connects commit signals of all catalogue settings → `requestRebuild()`.
- `U32 starCount()`: `round(666 * ASStarsDensity)`, clamped.
- `void generate(F32 radius, std::vector<LLVector3>& pos, std::vector<LLColor4>& col, std::vector<F32>& intensity)`: builds the catalogue and keeps a parallel `sSizes` vector.
  - RNG: `std::mt19937`; seed 0 → random seed per generation (stock behaviour), non-zero → deterministic sky.
  - Position: stock formula; optional Milky Way concentration pulls stars toward a fixed great circle (blend of uniform and band-gaussian sampling, still upper hemisphere).
  - Intensity: `min(rand^spread + 0.1, 1)`, spread default 2 = stock.
  - Colour: `stock = (0.75+r*0.25, 1, 0.75+r*0.25)`; `real = blackbody(T)` with T sampled from a naked-eye spectral-class table (O/B ~12%, A ~20%, F ~15%, G ~15%, K ~30%, M ~8%) shifted by temperature bias in log-T; blackbody→sRGB analytic fit converted to linear, normalised to max channel = 1. Final = `mix(stock, real, ColorAmount)`, then saturation around luminance, clamp.
  - Size: `(16 + rand*20) * ASStarsSize`, stored per star (stable across rebuilds — important for DoF accumulation).
- `F32 starSize(U32 i)`, `F32 brightness()`, `F32 twinkleAmount()`.
- `requestRebuild()`: if `gSky.mVOWLSkyp` exists, call its new rebuild hook.

### Settings (`app_settings/settings.xml`, persisted)
| Control | Type | Default (stock) | Range | UI label |
|---|---|---|---|---|
| ASStarsDensity | F32 | 1.0 | 0–20 | Density |
| ASStarsColorAmount | F32 | 0.0 | 0–1 | Color realism |
| ASStarsSaturation | F32 | 1.0 | 0–3 | Color saturation |
| ASStarsTemperatureBias | F32 | 0.0 | -1–1 | Color temperature (warmer↔cooler) |
| ASStarsBrightness | F32 | 1.0 | 0–4 | Brightness |
| ASStarsBrightnessSpread | F32 | 2.0 | 0.5–6 | Brightness contrast |
| ASStarsSize | F32 | 1.0 | 0.25–4 | Size |
| ASStarsTwinkle | F32 | 1.0 | 0–1 | Twinkle |
| ASStarsMilkyWay | F32 | 0.0 | 0–1 | Milky Way concentration |
| ASStarsSeed | S32 | 0 | 0–999999 | Pattern seed (0 = random) |

Brightness and Twinkle are uniforms (live, no rebuild); others rebuild the catalogue.

### Upstream edits (minimal, `<AS:Chanayane>` tagged, original code kept commented)
- `llvowlsky.h`: add public `void asRebuildStars();`.
- `llvowlsky.cpp`:
  - `getStarsNumVerts()` → `ASStars::starCount()`.
  - `initStars()` body → `ASStars::generate(...)`.
  - `updateStarGeometry()`: `sc = ASStars::starSize(vtx)`; reallocate `mStarsVerts` when the count changed.
  - `drawStars()`: draw `count*6` (all generated stars; default density keeps the stock 666 visible).
  - `asRebuildStars()`: `initStars(); mStarsVerts = nullptr; gPipeline.markRebuild(mDrawable, REBUILD_ALL);`.
- `lldrawpoolwlsky.cpp` `renderStarsDeferred()`: `star_alpha *= ASStars::brightness()` (not in reflection path); set `as_twinkle_amount` uniform.
- `starsF.glsl`: `uniform float as_twinkle_amount;` and `col.a *= mix(1.0, (as_twinkle_mean > 0.0 ? as_twinkle_mean : twinkle()), as_twinkle_amount);` — DoF mean path unchanged. Validate with glslang per AGENTS.md.
- `llviewerfloaterreg.cpp`: `ASStars::registerUICallbacks(); LLFloaterReg::add("as_stars_settings", "floater_as_stars_settings.xml", build<LLFloater>)`.
- `CMakeLists.txt`: `asstars.cpp/.h`.
- `commands.xml`: `as_stars_settings` command (Floater.Toggle / Floater.IsOpen, icon `Command_ASStarsSettings_Icon`, no checkbox_control since there is no on/off).
- `textures.xml`: `Command_ASStarsSettings_Icon` → `toolbar_icons/stars_settings.png`.
- `strings.xml`: `Command_ASStarsSettings_Label` / `_Tooltip`.
- `menu_viewer.xml`: "Star Settings..." next to both "Aurora Settings..." entries.

### New UI files (ours)
- `panel_as_stars_settings.xml`: aurora panel layout (slider + reset button rows, seed spinner + Randomize button, "Reset all" button). Controls bound by `control_name`, no panel class needed.
- `floater_as_stars_settings.xml`: wraps the panel (aurora floater pattern).
- `floater_as_environment_effects.xml`: add `<panel filename="panel_as_stars_settings.xml" label="Stars" name="stars_settings_tab" />` after the Moon tab.

### Icon
- `toolbar_icons/stars_settings.svg`: 18×18, `#d2d2d2`, one 4-point sparkle + two small dots (style of `aurora_settings.svg`).
- Rasterize to `stars_settings.png` with a scratchpad Pillow script (supersample 8×, draw same geometry, downsample LANCZOS, RGBA); match the existing PNG size/format (check `aurora_settings.png` dims first). Script stays out of the repo.

## Verification (user build)
1. Defaults: night sky looks like stock (≈666 greenish-white stars, same size/brightness/twinkle).
2. Color realism 1: mix of blue-white, white, yellow, orange, red stars; temperature bias and saturation shift visibly; seed changes pattern, same seed reproduces.
3. Density 0 → no stars; 20 → ~13k stars, no frame-time regression of note.
4. Live DoF + DoF snapshot with defocused stars: coloured bokeh, no flicker during accumulation (sizes/colours stable; twinkle mean path intact).
5. Panel works both in Environment Effects "Stars" tab and standalone floater; toolbar icon appears in toybox; menu entries toggle the floater; reset buttons work.
6. WLSkyDetail change / GL restore still rebuilds stars correctly.

## Doc
After approval: copy this plan to `doc/ayanestorm-star-settings-plan.md`.

## Follow-ups (implemented after the plan)

### Master toggle
- `ASStarsEnabled` (default on). Off: `generate()` uses stock parameters, `brightness()`/`twinkleAmount()` return 1; panel controls grey out via `enabled_control`; toybox command has `checkbox_control`.
- Pitfall fixed: setting change signals connected in `registerUICallbacks()` fire before `LLCachedControl`s created later (their slots connect after), so `generate()` read stale values. `generate()` now reads `gSavedSettings` directly.

### Brightness above 1.0
- `custom_alpha` goes through `smoothstep(0, 0.9, ...)` in `starsF.glsl`, which saturated the multiplier. Multiplier is now the `as_star_brightness` uniform applied after the smoothstep.

### Milky Way concentration
- Band stars are added on top of the background count (`MILKY_WAY_EXTRA_STARS` = 1.5x at full concentration) instead of being taken from it.

### Real-sky catalogue (`ASStarsMode` = 1)
- Source: Celestial Data (Frohn & Hernangomez 2023, BSD 3-Clause, doi 10.5281/zenodo.7561601, from d3-celestial), `stars.14` set: 118,216 stars, J2000 RA/Dec, `mag`, `bv` (B-V, null for 1,279 stars). No distance/size (not needed: apparent size follows brightness).
- `scripts/content_tools/as_build_star_catalog.py <stars.14.min.geojson> <out.bin> [count]` writes `indra/newview/app_settings/stars/as_star_catalog.bin`: 20,000 brightest stars (faintest mag 7.25), 200 KB. Format: `ASSTAR01`, u32 count, u32 0, then per star int16 x,y,z (equatorial unit vector * 32767), mag*1000, bv*1000 (-32768 unknown), sorted brightest first.
- License: `app_settings/stars/LICENSE-celestial-data.txt` ships next to the binary; `viewer_manifest.py` packages `app_settings/stars`.
- Count = brightest `round(666 * density)` stars (density 30 = all 20,000). Magnitude limit = faintest drawn star.
- Brightness: `energy = (10^(-0.4 (mag - limit)))^contrast * 0.15`; vertex alpha = min(energy, 1), the remainder goes to sprite area (size 18 * sqrt(energy/alpha), max 4x). `ASStarsMagnitudeContrast` default 0.4.
- Color: B-V -> temperature (Ballesteros 2012) -> existing `blackbody()`; unknown B-V = 5800 K. Color realism/saturation/temperature bias apply as in procedural mode.
- Orientation (`ASStars::applySkyTransform`, replaces the stock zenith spin in `renderStarsDeferred`): stars stay in the equatorial frame; GL matrix = translate * Rx(-(90 - latitude)) * Rz(-(LST + 90)), local frame x east / y north / z up. LST (deg) = `starRotationTime * 0.01 + ASStarsSiderealOffset * 15` (free running at the stock rate, DoF-frozen). Verified numerically against the standard alt/az formulas (error ~1e-15, east/west handedness included).
- Horizon: whole sphere is drawn; `starsV.glsl` fades stars below the horizon using `as_star_up` (local zenith in the star frame) when `as_star_horizon_fade` = 1 (0 in procedural mode = stock).
- Sprites: real-sky quads use unit axes with an x-axis fallback near the pole (stock `at % z` collapses at Polaris).
- `ASStarsLatitude` default 45 (Europe / North America).
- Next: realistic Milky Way glow from `mw.geojson` (5 brightness outlines).

### Real-sky brightness fix (all stars looked equally bright)
- Root cause: stock `starsF.glsl` never reads `vertex_color.a` (alpha = texture alpha * smoothstep * 32), so per-star intensity was ignored in every mode; and the x32 would clip any linear intensity above ~0.03 anyway.
- Real sky now stores brightness in vertex alpha as log2 over 8 octaves (`REAL_SKY_LOG_RANGE`), alpha 1 = stock level; `starsF.glsl` decodes `exp2((a - 1) * as_star_log_range)` when `as_star_log_range` > 0 (0 in procedural = stock).
- Level = flux^contrast / 128 (`REAL_SKY_FAINT_LEVEL`): faintest drawn star = 1/128 of the stock level; above the stock level the surplus grows the sprite (sqrt, max 4x).
- `updateStarColors()` (random walk of alpha down to half) is skipped in real sky: it would dim log-encoded stars by up to 4 octaves.
- `ASStarsMagnitudeContrast` default 0.7 (1 = physical linear flux). Decoded level at density 30 (limit mag 7.25), contrast 0.7: mag -1.4 = 32 (x1.5 size), 0 = 27, 2 = 7.5, 4 = 2.0, 6 = 0.56, 7.25 = 0.25.
- Procedural "Brightness contrast" had no visible effect for the same root cause. Replaced `ASStarsBrightnessSpread` (intensity distribution exponent) by `ASStarsBrightnessVariation` (0-1, default 0): 0 keeps the stock path (alpha ignored, all stars equal); > 0 log-encodes `level = intensity^(2 * contrast)` (stock intensity distribution, 1:100 at 1) like the real sky. `ASStars::encodedBrightness()` (real sky or contrast > 0) drives `as_star_log_range` and the `updateStarColors()` skip.

### Real-sky visibility defaults
- At the first real-sky defaults hardly any star was visible (needed density 10+, contrast 1, brightness 4). Now: `REAL_SKY_FAINT_LEVEL` 1/32 (was 1/128, equals brightness 4), real-sky count = 6660 * density (`REAL_SKY_STARS_PER_DENSITY`; 1 = naked-eye limit ~mag 6.3, 3 = all 20000; procedural keeps 666 * density), `ASStarsMagnitudeContrast` default 1.0, `ASStarsColorAmount` default 1.0 (procedural default is no longer the stock tint; master toggle off still gives stock).

### Procedural brightness contrast, second pass
- First version (`level = intensity^(2 * contrast)`) still looked uniform: past the stock x32 gain everything above ~1/32 of the stock level is plain white, and procedural sprite sizes were random, unrelated to brightness.
- Now modelled on the real sky: target level = `(1-u)^(-0.4/0.45) / 32` (synthetic magnitude from the star-count law N(<m) ~ 10^(0.45 m); faintest at the real-sky faint level, median ~2x it, ~2% above the stock level), blended in log space from the stock level by contrast; surplus above stock grows the sprite (sqrt, max 4x); sprite size blends from the random stock size to the brightness-driven size by contrast. Simulated displayed level at contrast 1: p10 1.1, p50 1.85, p90 8, p99 32.

### Mode-dependent panel greying
- `enabled_control` takes one boolean, so `ASStarsProceduralUI` / `ASStarsRealSkyUI` (Persist 0) are derived from `ASStarsEnabled` and `ASStarsMode` by `updatePanelStates()` (signals + once at registration). Procedural-only rows (brightness contrast, Milky Way, seed, randomize) and real-sky rows (latitude, sidereal offset, magnitude contrast) use them; tooltips say why a control is greyed out (LLView tooltips still show on disabled controls).

### Defaults: real sky
- `ASStarsMode` default 1 (real sky), `ASStarsSaturation` default 1.0; density 1.0 and color realism 1.0 unchanged. The stock look is now the master toggle off, not the defaults.
- Tuned defaults: `ASStarsBrightness` 1.2, `ASStarsSize` 2.75, `ASStarsTwinkle` 0.75, `ASStarsLatitude` 45, `ASStarsMagnitudeContrast` 0.75; procedural `ASStarsMilkyWay` 0.15, `ASStarsBrightnessVariation` 0.75.

---

# AyaneStorm Milky Way Glow — Plan

## Context
Real-sky mode now draws catalogue stars, but a real night sky (and the DoF reference photo) also shows the diffuse Milky Way band, the Magellanic Clouds and bright nebulae/galaxies. Goal: a real-sky-only glow layer driven by real data in `.ca` (Celestial Data, BSD 3-Clause, already credited in `app_settings/stars/LICENSE-celestial-data.txt`), rotating exactly with the catalogue stars (same latitude / sidereal transform), fading by day and at the horizon, blurred normally by DoF and present in snapshots.

## Data (verified)
- `mw.min.geojson`: Milky Way in 5 nested brightness outlines `ol1` (faint outer, 10k pts) .. `ol5` (core clouds, 584 pts), MultiPolygons with holes (Great Rift gaps), lon = RA in [-180, 180], lat = Dec.
- `dsos.bright.min.geojson`: 32 hand-picked objects with `type` (s/sd galaxy, sfr nebula, gc, oc...), `mag`, `dim` (arcmin, "a" or "a x b"): Andromeda, Triangulum, Orion Nebula, Lagoon, Pleiades, Southern Pleiades, LMC (PGC 17223)...
- `lg.min.geojson`: SMC (mag 2.7, 320x185') and LMC (mag 0.9, 645x550'); take SMC from here, dedupe LMC.

## Design

### 1. Offline texture — `scripts/content_tools/as_build_milky_way.py` (numpy + Pillow)
- Output `indra/newview/app_settings/stars/as_milky_way.png`: 2048x1024 RGBA, equirectangular, x = (RA_lon + 180) / 360, north at top (PNG rows are flipped on decode, `llpngwrapper.cpp:190`, so GL v = (dec + 90) / 180).
- Milky Way: rasterize each level (outer ring filled, holes cleared) to a mask, blur (radius growing toward the outer levels, ~0.4-1.2 deg), accumulate weighted levels (e.g. 0.15, 0.3, 0.5, 0.75, 1.0 cumulative). Tint: outer cool white -> core warm (~(0.75,0.8,1) -> (1,0.85,0.65)). Light fractal mottling modulated by level (star-cloud texture), seedable, off with a flag.
- Deep-sky: Gaussian ellipses (no position angle in the data: axis-aligned in the local tangent frame, corrected for the equirect stretch 1/cos(dec)), peak from surface brightness (10^(-0.4 mag) / area, normalised so the LMC ~ MW core). Tint by type: galaxies warm white, emission nebulae pink-red, reflection nebulae blue, globular yellow-white, open clusters faint blue-white haze (low weight: their stars are already drawn).
- RGB = combined glow color; A = deep-sky share of the pixel (so the viewer can scale Milky Way and deep-sky separately).
- Writes a scratchpad preview while tuning; the PNG is committed (expected a few hundred KB; check size).
- Shipped via the existing `self.path("stars")` manifest line; license file gets the extra source note (lg/dsos/mw from the same dataset).

### 2. New module `indra/newview/asmilkyway.h/.cpp` (namespace `ASMilkyWay`)
Mirrors `asaurora.cpp`:
- `registerShader / createShader / unloadShader` (program `deferred/asmilkywayV.glsl` + `asmilkywayF.glsl`, `isDeferred`, `SG_SKY`, `HAS_EMISSIVE` permutation as in `ASAurora::createShader`); `unloadShader` also releases the texture (GL restore).
- Texture: `LLImagePNG::load` + `decode` into `LLImageRaw` once, upload lazily at first bind with `glTexImage2D` RGBA8 (unpack-state save/restore and `bindManual` pattern from `ascolorlut.cpp:373-411`); linear filter, wrap S repeat, T clamp, no mips (soft content; avoids the RA seam mip artifact). Missing file: warn once, skip the pass.
- `configureShader()`: returns false unless `ASStarsEnabled`, `ASStars::realSkyActive()`, `ASMilkyWayEnabled`, not HDRI sky, and night factor > 0. Night factor = same as stars: `smoothstep(0, 0.9, EEP star brightness / 500)` times `ASStars::brightness()`-independent own intensity. Sets uniforms: `mw_rot` (mat3 local->equatorial), intensities, saturation, horizon fade, binds the texture.
- Rotation: add `ASStars::localToEquatorial(F32 star_time)` (LLMatrix3 = transpose of Rx(tilt)·Rz(spin)) sharing the tilt/spin math of `ASStars::applySkyTransform`, so glow and stars can never drift apart; use `ASDoFRenderer::starRotationTime(gFrameTimeSeconds)` (DoF freeze).

### 3. Shaders (new, ours)
- `asmilkywayV.glsl`: like `asauroraV.glsl`; direction `normalize(position - camPosLocal)` in the WL dome frame (Y up), converted to agent axes `(d.z, d.x, d.y)` (renderDome's 120 deg rotation about (1,1,1) maps dome x->north, y->up, z->east).
- `asmilkywayF.glsl`: `eq = mw_rot * dir`; `u = atan(eq.y, eq.x) / 2pi + 0.5`, `v = asin(eq.z) / pi + 0.5`; `textureLod(..., 0)`; `color = rgb * mix(mw_intensity, dso_intensity, a)`, saturation around luminance, horizon fade `smoothstep(0, 0.15, dir_up)` (extinction), times night factor; outputs exactly like `asauroraF.glsl` (frag_data[0] or [3] with HAS_EMISSIVE, [1]/[2] zero). Validate both with glslang.

### 4. Render hook — `lldrawpoolwlsky.cpp` `renderDeferred()` (tagged)
After `ASHorizonScattering::render(...)` and before `renderHeavenlyBodies()`, inside `!gCubeSnapshot` (stars are not in reflection probes either): `if (ASMilkyWay::configureShader()) { LLGLSPipelineBlendSkyBox(false,false); BT_ADD; renderDome(origin, camHeightLocal, &ASMilkyWay::getShader()); unbind; BT_ALPHA; }` — same state handling as the aurora block. Behind moon/stars/aurora/clouds, over the sky haze. Not drawn into the DoF star mask (diffuse glow is blurred as normal scene color). Background-isolate mode already returns early.

### 5. Registration (tagged)
- `llviewershadermgr.cpp`: `ASMilkyWay::registerShader / unloadShader / createShader` next to the three `ASAurora::` calls (L495, L1246, L3089).
- `CMakeLists.txt`: `asmilkyway.cpp/.h`.
- `asstars.cpp`: nothing else (realSkyActive already exposed).

### 6. Settings + panel
| Control | Type | Default | Range |
|---|---|---|---|
| ASMilkyWayEnabled | Boolean | 1 | |
| ASMilkyWayIntensity | F32 | 1.0 (tuned in-world) | 0-4 |
| ASMilkyWayDeepSkyIntensity | F32 | 1.0 | 0-4 |
| ASMilkyWaySaturation | F32 | 1.0 | 0-3 |
- New "Milky Way" rows in the Real sky section of `panel_as_stars_settings.xml` (checkbox + 3 sliders + resets, `increment="0.001"`, 3 decimals, `enabled_control="ASStarsRealSkyUI"`, tooltips explaining real-sky-only); panel/floater heights grow by ~120 px (Environment Effects tab container is 675 px: check fit, else grow it). Add the 4 controls to `ASStars` reset-all list (no rebuild needed: live uniforms).

## Files
New: `scripts/content_tools/as_build_milky_way.py`, `app_settings/stars/as_milky_way.png`, `asmilkyway.h/.cpp`, `shaders/class1/deferred/asmilkywayV.glsl`, `asmilkywayF.glsl`.
Edited: `lldrawpoolwlsky.cpp`, `llviewershadermgr.cpp`, `CMakeLists.txt` (tagged); `asstars.h/.cpp`, `settings.xml`, `panel_as_stars_settings.xml`, `floater_as_stars_settings.xml`, `LICENSE-celestial-data.txt`, `doc/ayanestorm-star-settings-plan.md` (append section).

## Verification
- Offline: preview PNG shows the band (Cygnus Rift, Sagittarius core brightest), LMC/SMC, M31, M42 at the right RA/Dec; numeric check that shader math (dome->agent->equatorial->uv) puts a known direction (e.g. Sagittarius core RA 266, Dec -29) where the catalogue star frame puts it (reuse the earlier alt/az verification script).
- glslang on both shaders.
- In-world (user build): at -41 latitude the core passes overhead with the Magellanic Clouds near the south pole; at 45 N the band arcs through Cygnus/Cassiopeia; glow turns with the stars when changing sidereal offset; gone by day and in procedural mode; fades at the horizon; DoF snapshot blurs it softly (no star-mask bokeh from it); no seam at RA 0h / 24h.

## Milky Way glow — implementation notes
- Texture: `python scripts/content_tools/as_build_milky_way.py .ca/data indra/newview/app_settings/stars/as_milky_way.png [--preview p.png] [--no-mottle] [--seed N]` -> 2048x1024 RGBA, 279 KB. Blur is numpy FFT (no scipy): horizontal per row with sigma / cos(dec), wrapping in RA; vertical zero-padded. Final tuning: level blur 3.0/2.0/1.3/0.8/0.5 deg (ol1..ol5), mottle 0.35 * sqrt(glow), DSO sigma = extent / 4, LMC peak 0.5.
- Data fixes in the script: NGC 6121 (M4) has M42's coordinates in dsos.bright (overridden to RA 245.897, Dec -26.526); `GC` (galactic-centre marker) skipped; LMC/SMC taken from lg (PGC 17223 / NGC 292 entries dropped).
- `ASStars::localToEquatorial()` shares `realSkyAngles()` with `applySkyTransform()`; the matrix goes to the shader as three vec3 columns (`mw_rot0..2`; LLGLSLShader has no hashed-name uniformMatrix3fv).
- Verified numerically: renderDome's 120 deg rotation about (1,1,1) maps dome x->north, y->up, z->east (shader swizzle `(d.z, d.x, d.y)`); A == Rx(tilt)*Rz(spin) (the star dome transform); equatorial round trip exact; galactic centre lands at uv (0.24, 0.339) as painted.
- Night factor = the stars' `smoothstep(0, 0.9, EEP star brightness / 500)`; `GLOW_SCALE` 0.25 in `asmilkyway.cpp` sets the base level (tune in-world with the intensity sliders first).
- Environment Effects tab container grown to 715 px (floater 740) for the 692 px stars panel.
- Texture layout changed (replaces the RGBA / deep-sky-share design above): A = deep-sky share made the Milky Way pixels (share ~0) transparent in image viewers and fragile to tools that drop RGB under alpha 0. Now a 2048x2048 RGB PNG (246 KB), two stacked equirectangular maps: top half Milky Way, bottom half deep-sky (file rows; after the decoder's flip GL v in [0.5, 1] = Milky Way, [0, 0.5] = deep-sky, v = (dec + 90) / 180 within each half). Shader samples both and sums `band * mw_intensity + deep_sky * mw_dso_intensity` (exact per-layer scaling). Values are linear, so the file looks dark in image viewers; `--preview` writes a gamma-encoded combined view.

## Milky Way glow — photographic band (NASA SVS Deep Star Maps 2020)
- Source: https://svs.gsfc.nasa.gov/4851, `milkyway_2020_*.exr` (Gaia DR2 stars only; `hiptyc_2020` = Hipparcos/Tycho stars brighter than mag 11.5; `starmap_2020` = both; `_gal` = galactic). Linear half-float, plate carree, ICRF/J2000, RA 0h at the center, RA increasing to the left. No formal license on the page; requested credit "NASA/Goddard Space Flight Center Scientific Visualization Studio. Gaia DR2: ESA/Gaia/DPAC." (in `LICENSE-celestial-data.txt`). Python reader: `pip install OpenEXR` (3.5.2).
- `python scripts/content_tools/as_build_milky_way.py .ca/data indra/newview/app_settings/stars --photo milkyway_2020_8k.exr [--preview p.png]`. The script checks the orientation by circular cross-correlation in RA against the outline band for the 4 flips (8k: flip RA corr 0.784 at +0.9 deg; others 0.51-0.63) and aborts unless it is "flip RA, |shift| < 3 deg"; flips, box-resamples to 4096x2048, white point = luminance p99.99 (0.1866 for the 8k source).
- Outputs: `as_milky_way.jpg` 4096x2048 (JPEG q90 4:4:4, 3.15 MB; PNG was 16.8 MB because of the faint-star texture, mean JPEG error 3/255) and `as_deep_sky.png` 2048x1024 (photo mode: nebulae only, types sfr/en/bn/rn; the Gaia map already shows LMC, SMC, M31, M33 and clusters as stars). Both gamma-encoded (value^(1/2.2)), decoded with `pow(x, 2.2)` in `asmilkywayF.glsl`. Without `--photo`, the outline band is written to the same two files.
- Viewer: `asmilkyway.cpp` `GlowTexture` (file, private sampler/channel, CPU image, GL name) x2, loaded by extension through `LLImageFormatted::createFromExtension`; both must bind or the pass is skipped. Band texture = 4096x2048 RGB8 (~32 MB VRAM once padded).
- Placement check on the final JPEG: plane/|b|=50-70 brightness ratio 12.8x; LMC 0.47 vs its RA-mirrored position 0.02; Sgr star cloud 0.70; north galactic pole 0.002.
- Open: the faint all-sky starlight background is real but may read as haze in-world (script could subtract a black level); the stars between mag 7.25 (catalogue limit) and 11.5 (start of the Gaia-only layer) are in neither layer.
- Tuned defaults (in-world): `ASMilkyWayEnabled` on, `ASMilkyWayIntensity` 2.5, `ASMilkyWayDeepSkyIntensity` 2.0, `ASMilkyWaySaturation` 1.0.
