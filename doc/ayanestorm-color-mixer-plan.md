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
| 8 bands × Hue/Saturation/Luminance | `-100…+100`, `0` |

Use explicit settings for the 24 band values, such as `ASColorGradeRedHue`, rather than storing an opaque array. This keeps debug settings and preset files readable.

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

## Eight-Band Color Mixer

Use these band centers in hue degrees:

| Band | Center |
|---|---:|
| Red | 0 |
| Orange | 30 |
| Yellow | 60 |
| Green | 120 |
| Aqua | 180 |
| Blue | 240 |
| Purple | 280 |
| Magenta | 320 |

For each pixel:

1. Compute circular distance from its OKLCh hue to every center.
2. Generate smooth weights extending to the midpoints between neighboring centers, with a small overlap feather.
3. Multiply weights by `smoothstep(0.01, 0.05, C)` so neutral pixels are protected.
4. Normalize the weights when their sum exceeds one.

Map per-band controls as follows:

- Hue `-100…+100` → `-30…+30°`.
- Saturation → chroma multiplier `max(0, 1 + value/100)`.
- Luminance → `L += value/100 * 0.25 * 4L(1-L)`.

Blend all eight contributions from the original pixel state using normalized weights. Do not apply bands sequentially, which would make results order-dependent.

Upload band values as packed uniform arrays:

```glsl
uniform vec3 color_grade_bands[8];
// x = hue shift radians, y = saturation scale, z = luminance offset
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

- One horizontal row of eight circular color selectors.
- No rainbow/global selector.
- Selected band indicated by an outer ring and its name displayed as text.
- Three full-width Hue, Saturation, and Luminance sliders below.
- Values and slider positions refresh immediately when selecting another band.
- Individual resets affect the selected band and dimension only.
- Keyboard focus and tooltips identify every band so selection does not depend only on color.

Use one white circular mask texture tinted through button `image_color`; provide selected and unselected states without eight duplicate assets.

### Effects

Expose Grain Amount, Size, Roughness, and Color. Disable the subordinate controls when Amount is zero while preserving their values.

### Interaction

Implement the floater as a custom class:

- On band selection, disconnect or guard mixer callbacks, populate the three controls from that band’s settings, then reconnect.
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
version: 1
name: display name
values:
  ASColorGradeExposure: 0.0
  ...
```

Rules:

- **Neutral** is virtual, immutable, always first, and applies all documented defaults.
- A user preset contains all Basic, Mixer, and Grain values.
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
