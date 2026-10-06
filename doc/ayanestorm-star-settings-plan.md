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
