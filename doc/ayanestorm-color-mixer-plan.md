# AyaneStorm Color Grading Implementation Plan

## Summary

Create `doc/ayanestorm-color-grading-implementation-plan.md` with this plan and author `chanayane@firestorm`.

Implement color grading as an AyaneStorm-owned module with two shader stages:

1. Scene-linear exposure and white balance before tone mapping.
2. Display-referred tonal, color-mixer, and grain processing after camera effects.

Normal snapshots include grading. Viewer UI, HUD, snapshot guides, cube captures, buffer visualization, and “No post-processing” snapshots exclude it. Background-isolate mode retains its current immunity.

## Module and Interfaces

Add these self-contained components:

- `ascolorgrading.h/.cpp`
  - Own shader programs, settings access, uniform packing, preview bypass state, preset serialization, and final presentation.
  - Public interface:
    - `registerShaders(std::vector<LLGLSLShader*>&)`
    - `createShaders(S32 shader_level)`
    - `unloadShaders()`
    - `appendLinearShader(LLGLSLShader&)`
    - `bindLinearUniforms(LLGLSLShader&, bool bypass)`
    - `present(LLRenderTarget& color, LLRenderTarget& depth, LLVertexBuffer&)`
    - `isActive()`
    - `setPreviewBypass(bool)`
    - `resetAll()`
    - `savePreset()`, `loadPreset()`, `deletePreset()`, and `listPresets()`
- `asfloatercolorgrading.h/.cpp`
  - Derive `ASFloaterColorGrading` from `LLFloater`.
  - Own band selection, dynamic mixer-slider binding, preset controls, reset actions, and press-and-hold Before behavior.
- New shaders:
  - `ascolorgradingLinearF.glsl`: exposure and white-balance function linked into existing tone-map/gamma programs.
  - `ascolorgradingF.glsl`: final tonal, perceptual color, mixer, presentation dither, and grain shader.
- New XUI:
  - Reusable `panel_as_color_grading.xml`.
  - Standalone `floater_as_color_grading.xml`.
  - New Color Grading tab in Environment Effects.
- Register all new files in `indra/newview/CMakeLists.txt`.

New files use normal explanatory comments and `@author chanayane@firestorm`. Every edit to an existing `ll*` or `fs*` file is minimal and wrapped in `<AS:Chanayane>` tags. If existing code is moved or replaced, retain it commented inside the ownership block.

## Settings and Runtime State

Add persistent settings with identity defaults:

| Setting group | Range/default |
|---|---|
| `ASColorGradingEnabled` | Boolean, `false` |
| `ASColorGradeExposure` | `-5.00…+5.00 EV`, `0` |
| Brightness, Contrast | `-100…+100`, `0` |
| Temperature, Tint | `-100…+100`, `0` |
| Shadows, Highlights | `-100…+100`, `0` |
| Blacks, Whites | `-100…+100`, `0` |
| Saturation, Vibrance | `-100…+100`, `0` |
| Global Hue | `-180…+180°`, `0` |
| Grain Amount, Size, Roughness, Color | `0…100`; Amount default `0`, remaining neutral defaults `50` |
| 24 bands × Hue/Saturation/Lightness | `-100…+100`, `0` |
| 24 bands × Selection Color | Per-band palette color; independently customizable |
| 24 bands × Strength/Hue Range/Chroma Range/Lightness Range/Softness | `0…100`; Strength `100`, Hue and Lightness Range `50`, Chroma Range `100`, Softness `50` |

Use explicit settings for the 192 numeric band values and 24 selection colors,
such as `ASColorGradeRedHue` and `ASColorGradeRedSelectionColor`, rather than
storing opaque arrays. This keeps debug settings and preset files readable.

Keep these values transient and out of settings/presets:

- Preview-bypass state.
- Selected main tab.
- Selected mixer band.
- Snapshot grain seed.

`isActive()` returns true only when master enable is on, preview bypass is false, rendering is not a cube capture or buffer visualization, and the current snapshot does not request no post-processing.

## Linear HDR Stage

Attach `ascolorgradingLinearF.glsl` to every relevant HDR tone-map and legacy gamma-correction shader variant. Add one tagged function call in `postDeferredTonemap.glsl` and `postDeferredGammaCorrect.glsl` before tone mapping or sRGB conversion.

The exported shader function receives linear Rec.709 color and performs:

```glsl
color = white_balance_matrix * color;
color *= exp2(exposure_ev);
color = max(color, vec3(0.0));
```

Compute the white-balance matrix once on the CPU whenever Temperature or Tint changes:

- Map Temperature `-100…+100` to a bounded reciprocal-temperature shift around D65.
- Map Tint to a bounded green–magenta displacement.
- Convert the source and target white points through Bradford cone space.
- Upload the resulting `mat3`; identity is used at zero.
- Clamp extreme chromatic adaptation coefficients to prevent negative or unstable output.

`ASColorGradeExposure` is independent of existing `RenderExposure`. It adds creative exposure in photographic stops without changing dynamic-exposure configuration or EEP state.

`LLPipeline::tonemap()` and `gammaCorrect()` call `ASColorGrading::bindLinearUniforms()`. Disabled and bypassed states upload identity values; no shader reload or permutation change occurs while sliders move.

## Final Display-Referred Stage

Run the final stage after tone mapping, glow, DoF, anti-aliasing, chromatic aberration, lens flare, vignette, RLVa effects, and snapshot-frame compositing, but before presentation, background isolate, snapshot guides, focus markers, HUD, and viewer UI.

Refactor the existing AyaneStorm lens-flare and vignette render functions to accept and bind the active color render target:

- Lens flare blends additively into the active target while sampling the preserved deferred depth.
- Vignette blends multiplicatively into that same target.
- Neither shader samples the color target, so in-place blending is valid.
- Preserve their current cube-snapshot and background-isolate guards.
- Comment the original direct-framebuffer calls inside ownership tags when moving them.

Replace the normal final presentation draw with:

```cpp
if (!ASColorGrading::present(*sourceBuffer, mRT->deferredScreen, *mScreenTriangleVB))
{
    // Original presentation shader path.
}
```

Keep the original path as the fallback when grading is disabled or its shader is incomplete. The grading presentation shader must:

- Sample the final color target.
- Preserve alpha.
- Sample and write the existing deferred depth.
- Preserve the existing `HAS_NOISE` presentation dither.
- Apply artistic grain separately.
- Draw directly to the current framebuffer, avoiding an additional render-target copy.

Background isolate continues drawing after presentation so its configured solid color remains ungraded.

## Tonal Mathematics

Decode the tone-mapped sRGB input to linear RGB, convert it to OKLab, and perform tonal adjustments on OKLab `L`. Normalize UI sliders to `v = setting / 100`.

Use bounded endpoint-aware operations:

- Brightness:
  - `L += v * 0.25 * (1.0 - abs(2.0 * L - 1.0))`
  - Maximum influence is at the midtone; black and white remain anchored.
- Contrast:
  - Pivot at `L = 0.5`.
  - Multiply distance from the pivot by `exp2(v * 2.0)`.
- Shadows:
  - Weight `1.0 - smoothstep(0.15, 0.55, L)`.
  - Add `v * 0.20 * weight`.
- Highlights:
  - Weight `smoothstep(0.45, 0.85, L)`.
  - Add `v * 0.20 * weight`.
- Blacks:
  - Weight `1.0 - smoothstep(0.00, 0.25, L)`.
  - Add `v * 0.15 * weight`.
- Whites:
  - Weight `smoothstep(0.75, 1.00, L)`.
  - Add `v * 0.15 * weight`.

Recompute each tonal weight after the preceding operation and clamp `L` only after the complete tonal stack. Process in this order:

`Brightness → Contrast → Highlights → Shadows → Whites → Blacks`

This matches the UI order while allowing broad masks to shape the image before endpoint controls.

## Global Color and Vibrance

Convert adjusted OKLab to OKLCh:

- Global Hue adds the configured angle and wraps it around `2π`.
- Saturation scales chroma with `C *= max(0, 1 + saturation)`.
  - `-100` produces grayscale.
  - `+100` doubles chroma before gamut limiting.
- Vibrance uses chroma headroom:
  - Positive values mainly affect low and medium chroma.
  - Already saturated colors receive progressively less gain.
  - Warm hues receive a mild protection factor to reduce skin-tone overprocessing.
  - Negative vibrance reduces chroma smoothly across all hues.
- Achromatic pixels with very small chroma retain their hue state but receive no hue rotation.

After all color operations, convert back through OKLab to linear RGB and apply a hue-preserving gamut compressor before sRGB encoding. Avoid independent RGB clipping until the final safety clamp.

## 24-Band Color Mixer

Use three rows of eight default selection colors:

1. Red, Orange, Yellow, Green, Aqua, Blue, Purple, and Magenta.
2. Eight equal-OKLab-lightness grays from `#f2f2f2` to `#181818`.
3. `RedSkin2`, `RedSkin4`, `RedSkin6`, `RedSkin8`, `Skin2`, `Skin4`,
   `Skin6`, and `Skin8` from the generated skin palettes.

Each default can be replaced with an exact per-band selection color through the color
picker. For each pixel:

1. Use each band's customizable Selection Color as its exact selection center.
2. For chromatic bands, multiply a circular hue gate, a relative-chroma gate,
   and an independent OKLab-lightness gate.
3. Compare relative chroma as `C/L`, clamped near black. This remains
   approximately stable when illumination changes a surface's lightness.
4. For gray bands, omit hue and compare relative chroma and lightness.
5. Apply Softness independently to each non-maximum gate, then multiply their weights.
6. Exclude bands whose Hue, Saturation, and Lightness adjustments are all zero,
   then normalize active weights when their sum exceeds one. Neutral overlapping
   bands must not weaken the selected band's effect.

Map per-band controls as follows:

- Hue `-180…+180°` in `0.1°` increments, covering the complete hue circle without scaling.
- Saturation → chroma multiplier `max(0, 1 + value/100)`.
- Lightness → `L += value/100 * 0.25 * 4L(1-L)`.
- Strength scales the band's full contribution; zero disables it.
- Hue Range radius runs from exact at zero through `±30°` at 50 to
  `±60°` at 100; nearly neutral pixels are maximally hue-separated.
- Chroma Range controls relative-chroma difference independently. Its cubic
  response gives radius `0.125` at 50; 100 ignores relative chroma.
- Lightness Range controls OKLab-lightness difference independently. Its quadratic
  response gives radius `0.25` at 50; 100 ignores lightness and accepts all shades.
- Neutral selection colors omit the undefined hue gate, including default gray bands.
- Softness progressively attenuates farther selected colors. At zero the
  selection boundary is hard; values above 50 increasingly suppress distant
  colors, reaching a fourth-power falloff at 100.

Blend all 24 contributions from the original pixel state using normalized weights. Do not apply bands sequentially, which would make results order-dependent.
Apply Hue by blending the original and fully rotated OKLab chroma vectors, not
by multiplying a signed angle by selection weight. This keeps the `-180°` and
`+180°` endpoints identical under soft or overlapping selections.

Upload band values as packed uniform arrays:

```glsl
uniform vec3 color_grade_bands[24];
// x = hue shift radians, y = saturation scale, z = lightness offset
uniform vec3 color_grade_band_selection_colors[24];
// customized sRGB selection colors converted to OKLab by the viewer
uniform vec3 color_grade_band_parameters[24];
// x = strength, y = hue range, z = softness
uniform vec2 color_grade_band_ranges[24];
// x = relative-chroma range, y = lightness range
```

## Grain

Apply grain after color grading and immediately before final sRGB output.

- Amount maps `0…100` to a maximum amplitude of `0.08`.
- Size maps to approximately `1…8` output pixels per grain cell.
- Roughness blends fine noise with a lower-frequency component.
- Color blends between one monochrome noise sample and independent RGB samples.
- Modulate grain by luminance so it remains visible in midtones without overwhelming clipped blacks or whites.
- Use output-pixel coordinates rather than world coordinates, preventing camera-motion swimming.
- Advance the seed once per displayed frame for film-like temporal variation.
- For tiled snapshots, reconstruct full-image pixel coordinates from zoom/subregion data and latch one snapshot seed from the first tile until snapshot rendering ends. This prevents seams and tile-to-tile pattern changes.

## User Interface

Create a fixed-width Color Grading floater consistent with existing AyaneStorm panels. The top area remains visible while switching tabs:

- Master enable checkbox.
- Preset combo.
- Save and Delete buttons.
- Press-and-hold **Before** button.
- **Reset All** button.

Use three tabs:

### Basic

Use a scroll container with these groups and order:

- White Balance: Temperature, Tint.
- Light: Exposure, Brightness, Contrast, Highlights, Shadows, Whites, Blacks.
- Color: Saturation, Vibrance, Hue.

Every row has:

- Descriptive label.
- Slider.
- Editable numeric field.
- Individual reset icon.
- Tooltip stating the range and visual effect.

Temperature is explicitly a creative `-100…+100` cool-to-warm adjustment, not Kelvin.

### Color Mixer

Match the compact Photoshop reference:

- Three horizontal rows of eight circular color selectors: spectrum, gray, and consolidated skin.
- No rainbow/global selector.
- Selected band indicated by an outer ring and its name displayed as text.
- A compact selection-color swatch beside the band name opens the color picker.
- A white top-right dot marks every band whose selection color or numeric values differ
  from defaults.
- Eight full-width Hue, Saturation, Lightness, Strength, Hue Range, Chroma Range,
  Lightness Range, and Softness sliders below, using compact spacing.
- Values and slider positions refresh immediately when selecting another band.
- Individual resets affect the selected band and dimension only.
- The bottom Reset button restores the selected band's selection color and every
  numeric setting.
- Keyboard focus and tooltips identify every band so selection does not depend only on color.

Use one white circular mask texture tinted through button `image_color`; provide selected and unselected states without eight duplicate assets.

### Effects

Expose Grain Amount, Size, Roughness, and Color. Disable the subordinate controls when Amount is zero while preserving their values.

### Interaction

Implement the floater as a custom class:

- On band selection, disconnect or guard mixer callbacks, populate the seven controls from that band’s settings, then reconnect.
- Slider commits write directly to the appropriate persistent setting for live rendering.
- Mouse-down on Before sets transient bypass.
- Mouse-up, mouse capture loss, floater close, app focus loss, and floater destruction clear bypass.
- Reset All asks for confirmation, restores every grading value to identity/default, and leaves master enable unchanged.
- Add the floater to the relevant viewer menu, AyaneStorm preferences, and as a reusable Environment Effects tab. Do not add a toolbar icon in v1.

## Presets

Implement presets inside `ASColorGrading`; do not extend `LLPresetsManager`.

Store each preset at:

```text
%APPDATA%\AyaneStorm_x64\user_settings\presets\color_grading\<escaped-name>.xml
```

Use versioned LLSD:

```text
version: 2
name: display name
values:
  ASColorGradeExposure: 0.0
  ...
```

Rules:

- **Neutral** is virtual, immutable, always first, and applies all documented defaults.
- A user preset contains all Basic, Mixer, and Grain values.
- Version 1 presets migrate `Luminance` to `Lightness`, `Tolerance` to
  `HueRange`, `ShadeRange` to `LightnessRange`, and `TargetColor` to
  `SelectionColor` while loading. Missing newer bands retain neutral defaults.
- Loading never overwrites the source preset; newly saved presets use version 2.
- Exclude master enable and transient UI state.
- Loading first parses into temporary storage, verifies scalar types and finite values, fills missing known fields from neutral defaults, clamps ranges, ignores unknown fields, and then applies all settings together.
- Parse or validation failure leaves current settings unchanged and displays a notification.
- Any value change marks the preset combo as **Custom**.
- Saving trims names, rejects empty/reserved/path-invalid names, escapes the filename, and confirms overwrite.
- Deleting requires confirmation and cannot target Neutral.
- Refresh the combo after save/delete and retain the active selection when possible.

## Integration Points

Make minimal ownership-tagged changes to:

- `llviewershadermgr.cpp/.h`
  - Register, configure, and unload the new shaders.
  - Attach the linear utility to existing tone-map/gamma variants.
- `pipeline.cpp`
  - Bind linear uniforms.
  - Move AyaneStorm lens-flare/vignette target compositing.
  - Invoke the grading presentation path with the original presentation code as fallback.
- `llviewerfloaterreg.cpp`
  - Register `ASFloaterColorGrading`.
- Existing menu, preferences, settings, notifications, texture registry, and Environment Effects XML files.
  - Wrap each upstream/shared modification in ownership tags.

Do not enlarge `LLPostProcess`; it is a legacy SDR color-filter system and does not provide the required HDR split or modern grading behavior.

## Validation

Do not build the viewer; the user performs builds.

Before handoff:

- Parse all modified XML.
- Verify each XUI setting and callback exists.
- Confirm all source/header/shader files are registered.
- Confirm shader uniforms match CPU names, types, and array lengths.
- Confirm every affected `ll*`/`fs*` edit has ownership tags and preserves replaced original code.
- Review render-target parity for every combination of DoF, FXAA, SMAA, chromatic aberration, RLVa effects, snapshot frame, lens flare, and vignette.

User runtime acceptance:

- Disabled grading and enabled Neutral are pixel-equivalent to the prior renderer apart from unavoidable floating-point roundoff.
- Exposure behaves in stops and remains independent from `RenderExposure`.
- Temperature/tint remain neutral at zero and do not generate negative, NaN, or infinite values.
- Tonal controls affect their intended ranges with smooth transitions.
- Saturation `-100` produces grayscale; vibrance differs visibly from saturation and protects already saturated warm colors.
- Every mixer band selects, adjusts, resets, saves, and reloads independently without hue-boundary seams.
- Grain has no camera-motion swimming, no tiled-snapshot seams, and becomes monochrome at Color `0`.
- Before bypasses both stages only while held and always restores.
- Normal snapshots include grading; no-post snapshots, cube captures, UI/HUD, and guides exclude it.
- Lens flare, vignette, chromatic aberration, and snapshot frames receive final grading.
- Background-isolate color remains immune.
- Verify HDR enabled/disabled, ACES/Khronos tone mapping, CAS enabled/disabled, glow, DoF, FXAA/SMAA, resized windows, and high-resolution tiled snapshots.
- Record `bok` after a successful user build and `bokt` after runtime validation.

## Negative Effect

- Effects tab: `Negative`, bound to persistent Boolean `ASColorGradeNegativeEnabled`, disabled by default.
- The final presentation shader applies `1 - clamp(RGB, 0, 1)` after grading, grain, and dither; alpha and depth remain unchanged. UI is drawn afterward. Existing grading bypasses (including Before) still apply.
- Presets save and validate the Boolean; older presets without it disable Negative. Reset All and built-in presets restore the disabled default. Changes mark the preset Custom.
- Validation: XML parsing and source wiring checked; build and runtime verification remain with the developer.

## Existing LLImageFilter LUTs

- `indra/llimage/llimagefilter.cpp:312`: `colorCorrect()` processes raw image pixels on the CPU using independent red, green, and blue lookup tables, blended through a stencil.
- Gamma, brightness, contrast, colorize, linearize, and equalize generate 256-entry byte tables internally. These are per-channel 1D mappings.
- Filter descriptions are LLSD XML, loaded by the constructor; this code does not implement imported `.cube` 3D LUTs.
- `indra/newview/llsnapshotlivepreview.cpp` calls `executeFilter()` for snapshot images and previews. This is separate from the live ASColorGrading presentation shader.

## GPU 3D LUT Performance

A hardware-trilinear 3D LUT can evaluate a color transform with one filtered 3D texture lookup per pixel plus coordinate mapping. Adding it to the existing grading presentation shader avoids an additional fullscreen pass. Load and upload the LUT when selected, not every frame. Expected incremental cost is small, but FPS impact requires measurement on target GPUs and resolutions; no viewer benchmark has been performed. Larger LUTs increase memory/cache pressure.

Source: [NVIDIA GPU Gems 2, Chapter 24](https://developer.nvidia.com/gpugems/gpugems2/part-iii-high-quality-rendering/chapter-24-using-lookup-tables-accelerate-color).

## Cube LUT Implementation

Author: chanayane@firestorm

### Controls and File Locations

The reusable color-grading panel has a LUT tab: Enable LUT (off by default), Previous and Next buttons around the file selector, strength (0–100%, default 100%), Import, Open folder, and Refresh. Previous and Next follow the displayed alphabetical order and are disabled at the first and last entries. The master color-grading switch and Before preview still apply.

Discovery scans the runtime application directory's app_settings/lut and LL_PATH_USER_SETTINGS/luts. The latter resolves to %APPDATA%\AyaneStorm_x64\user_settings\luts on Windows. User files with the same basename take precedence. The dropdown displays the first nonempty name in this order: quoted TITLE, Preset: header comment, filename. Entries are sorted case-insensitively by that displayed name, with filename as the tie-breaker. Discovery reads headers only; only the selected table is parsed and uploaded. Filenames remain the item values and preset references, including when titles repeat. Refresh rescans and reloads the selection.

Import validates a single .cube file before copying it into the user folder, selects it, and enables LUT processing. Existing filenames receive a numeric suffix rather than being overwritten. Open folder opens the user directory. Source-tree examples are not added to viewer_manifest.py or bundled; use Import if running an installed viewer whose application settings are elsewhere.

### Format and Rendering

ascolorlut.h/.cpp owns parsing, discovery, import, UI callbacks, errors, and the selected texture. Supported files contain one LUT_3D_SIZE from 2 through 128 or one LUT_1D_SIZE from 2 through 65,536, optional quoted or unquoted TITLE, optional DOMAIN_MIN/DOMAIN_MAX (default 0–1), and the exact number of RGB rows. Red varies fastest in 3D tables. Blank lines, comments, CRLF, scientific notation, and a UTF-8 BOM are accepted. Finite half-float-representable output values are required. Invalid domains, incomplete/extra rows, duplicate headers, combined shapers, unsupported directives, lines over 4096 bytes, and files over 128 MiB are rejected.

The shader samples RGB16F textures with hardware linear filtering, mapping input-domain endpoints to texel centers. A 3D LUT uses one 3D sample; a 1D LUT uses independent samples for red, green, and blue. The LUT follows Basic, Mixer/Colorize, and Split Toning, before Grain and Negative. Strength blends the original graded RGB with the LUT result. No additional fullscreen pass is added; alpha, depth, UI exclusion, and existing rendering bypasses are unchanged.

LLGLSLShader only assigns texture channels to reserved samplers. The private LUT sampler is explicitly assigned mActiveTextureChannels after shader creation to avoid aliasing the scene/depth samplers, including when the LUT is disabled. Upload preserves pixel-unpack state. Only the selected GPU texture is retained; changing the file releases it, and shader unload releases GPU resources for recreation.

These are creative display-referred sRGB looks, without camera-log or ICC conversion. The 20 supplied 32-cubed examples reference AdobeRGB1998.icc in comments; their tables are structurally compatible, but this implementation does not reproduce that profile's color management.

### Presets and Failure Handling

ASColorGradeLUTEnabled, ASColorGradeLUTFile, and ASColorGradeLUTStrength persist with viewer settings and grading presets. Presets store a basename, not a path or embedded table. Older presets and Reset All restore LUT disabled, empty filename, and 100% strength. Changes mark the preset Custom.

A missing or invalid selection is bypassed with a notification and status text, without retaining the previous LUT effect. Parsing and header discovery contain allocation, standard-library, and unknown exceptions so a malformed file cannot unwind into the viewer. Failed imports preserve the current selection. Refresh retries failed files. File-picker callbacks use a weak panel handle.

### Validation and Runtime Checks

Independent Python checks verified all 20 supplied files have 32,768 finite RGB rows and usable domains. Edited XML parses; settings defaults, control bindings, callback names, shader uniform names/order, and CMake entries were checked. Coordinate checks cover domain endpoints and red-fastest indexing for sizes 2, 17, 32, 33, 65, and 128. Git diff whitespace checks passed.

No project build, C++ execution, GPU runtime test, or FPS measurement was performed. Runtime checks for the developer: import BW3.cube; toggle LUT and vary strength; confirm Before/UI exclusion; save/load a grading preset and Reset All; import an invalid file; remove a selected LUT and Refresh; verify normal snapshots and shader reload.

### Format Reference

[Colour's Iridas .cube reader](https://github.com/colour-science/colour/blob/develop/colour/io/luts/iridas_cube.py) documents the red-fastest ordering and optional domains, with a reference to Adobe's Cube specification.

### Additional Sample: Presetpro Kodachrome 64

`Presetpro  - Kodacrome 64.cube` was read without copying or modifying it. It declares `LUT_3D_SIZE 32`, a quoted TITLE, and DOMAIN_MIN/MAX of 0/1. Independent data checks found exactly 32,768 finite RGB rows, all within 0–1. Its format fits the current loader; the filename’s 64 is part of the film name, not the LUT dimension. No code change or runtime test was needed/performed.
