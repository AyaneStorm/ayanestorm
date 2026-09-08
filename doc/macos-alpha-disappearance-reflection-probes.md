# macOS Alpha-Blended Objects Disappearing With Graphics Presets

## Symptom

On one Apple M3 system, sheer or transparent legacy-material objects disappear
starting exactly at the third graphics-slider tick, Mid, but render at the
second tick, LowMid. The same content renders normally on an Apple M4 system.

## Preset Boundary

`indra/newview/featuretable_mac.txt` disables `RenderReflectionsEnabled` only for
Low. LowMid and every higher preset enable it. `RenderReflectionProbeDetail` and
`RenderReflectionProbeLevel` are both zero on LowMid and Mid. Consequently,
reflection enablement or detail does not explain the precisely reported
LowMid-to-Mid boundary. Shadows also remain disabled through Mid and MidHigh.

Reflection-probe count increases from 32 to 64, but this is a resource/count
limit rather than a distinct alpha material shader variant. It is a secondary
candidate only if the stronger Mid-boundary candidates are excluded.

## Matching Upstream Report

Second Life feedback reports legacy alpha-blended textures disappearing on
macOS PBR viewers, including a confirmed Apple M3 reproduction. Reported content
includes clothes, walls, handrails, windows, foliage, banners, and prim water.
Reported content-side workarounds include adding a blank normal or specular map,
or enabling fullbright/glow. Alpha masking can also avoid the affected blend
variant where suitable.

Source:
<https://feedback.secondlife.com/bug-reports/p/pbr-some-textures-with-transparency-do-not-render-in-alphablend-macos-sonoma-m1>

## LowMid-to-Mid Candidates and Isolation Tests

The most relevant settings that change exactly at Mid are:

- `RenderFSAAType`: Off (0) to FXAA (1).
- `RenderMaxTextureResolution`: 1024 to 2048.
- `RenderAnisotropic`: Off to On.
- `RenderReflectionProbeCount`: 32 to 64.

Test them independently without moving the main slider:

1. Select Mid, turn antialiasing off, and recheck the object.
2. If unchanged, leave antialiasing off and set `RenderAnisotropic` false.
3. If unchanged, set `RenderMaxTextureResolution` to 1024 and allow the affected
   texture to refetch (or relog if necessary).
4. Only then test `RenderReflectionProbeCount` at 32.

For stronger confirmation, reverse the successful test on LowMid: enable FXAA,
use 2048 texture resolution, or enable anisotropic filtering one at a time. If
one change makes the content disappear, that setting isolates the path.

Capture Help > About information and `AyaneStorm.log` from the affected Mac,
including shader compile/link warnings and OpenGL errors, before changing code.
The matching upstream M3 alpha-blend report still makes a shader/driver issue
plausible, but the exact tick boundary does not implicate reflection detail.

The macOS log is `~/Library/Application Support/AyaneStorm/logs/AyaneStorm.log`.
The preceding run is retained as `AyaneStorm.old` in the same directory.

## Firestorm Comparison

The reporter states that the same machine/content does not exhibit the problem
in regular Firestorm. A direct comparison against the repository's unchanged
`.phoenix-firestorm-master` copy shows:

- The Mac graphics presets are identical except for AyaneStorm adding the
  disabled-by-default `RenderVolumetricLighting` setting. The LowMid-to-Mid
  differences therefore come from Firestorm and do not by themselves explain
  why only AyaneStorm fails.
- AyaneStorm materially changes all relevant transparency paths, including
  `class2/deferred/alphaF.glsl`, `class2/deferred/pbralphaF.glsl`,
  `class3/deferred/materialF.glsl`, `class1/deferred/fullbrightF.glsl`, and
  `lldrawpoolalpha.cpp`.
- Exact OIT and AVBOIT use inert stubs on Darwin, but their dispatcher remains
  in the alpha draw path. More importantly, the ordinary macOS alpha shaders
  still contain AyaneStorm volumetric-atlas uniforms/functions and output
  routing conditionals. OIT being unavailable does not make those shader
  source changes disappear.
- AyaneStorm also changes the post-processing sequence around FXAA with optional
  chromatic aberration, color grading, lens flare, vignette, and background
  isolation passes. FXAA first enables exactly at Mid.

Pending a controlled comparison using the exact same settings and viewer
versions, this should be treated as a probable AyaneStorm regression. The first
high-value test is Mid with FXAA disabled. If that does not restore the content,
the next test should use an instrumented or minimally reverted alpha shader,
starting with the volumetric-atlas additions while leaving OIT in Standard mode.

## Darwin OIT Fallback Audit

On Darwin, both OIT implementations are compiled as inert stubs:

- support/enabled/capture queries always return false;
- shader selectors return the ordinary Firestorm shader pointer;
- capture entry points return false, causing `LLDrawPoolAlpha` to execute its
  ordinary rigged and non-rigged `forwardRender()` calls;
- captured-draw configuration returns false, causing the ordinary Firestorm
  `gGL.blendFunc()` call;
- captured-emissive handling returns false, causing ordinary emissive draws;
- OIT allocation, loading, compositing, and finishing do nothing.

The culling snapshot also remains false because both support-aware enablement
queries return false. Therefore the Exact OIT and AVBOIT algorithms themselves
cannot execute on macOS and are not the direct rendering path.

The result is functionally intended to be Firestorm fallback, but it is not
source-identical or control-flow-identical. Draws pass through the dispatcher;
the ordinary alpha shaders contain OIT preprocessor branches; and the same
shaders contain live AyaneStorm volumetric-atlas additions. With neither OIT
macro defined, the shader output branch preprocesses back to ordinary
`frag_color`, and with volumetrics disabled its uniform is set to zero. Those
facts make the OIT wrappers a lower-probability direct cause, but their shared
integration changes remain legitimate regression candidates.

Neither OIT availability nor mode changes at the LowMid-to-Mid preset boundary.
FXAA does. Therefore test Mid with FXAA off before preparing a Darwin-only build
that bypasses the shared OIT/volumetric alpha integration entirely.

## Ranked AyaneStorm-Specific Candidates

After comparing the rendering changes against `.phoenix-firestorm-master`, the
leading code candidate is the volumetric-transparency integration in the
ordinary alpha path, not either unavailable OIT algorithm. It modifies every
ordinary transparency shader used by macOS (`alphaF.glsl`, `pbralphaF.glsl`,
`materialF.glsl`, and `fullbrightF.glsl`) and calls
`ASVolumetricLighting::bindTransparencyAtlas()` from ordinary Firestorm alpha
draws. Even while volumetrics are disabled, the compiled programs retain a
runtime enable uniform and potentially an additional live atlas sampler. This
changes shader resource layout and compiler input on Apple's OpenGL driver.

Second is the shared OIT dispatcher integration in `lldrawpoolalpha.cpp` and
`gltfscenemanager.cpp`. Its Darwin stubs produce Firestorm-equivalent shader
selection, capture rejection, blending, and emissive behavior, so it is less
likely, but only a Darwin compile-time bypass can prove equivalence on the
affected driver.

Third is AyaneStorm's optional post-processing integration around Firestorm's
FXAA presentation. All optional calls return without changing GL state when
disabled, making this less likely from static inspection, but FXAA is the only
AyaneStorm-adjacent render-path boundary that activates exactly at Mid. The
Mid-with-antialiasing-disabled user test remains decisive.

There is also a separate Darwin settings defect: `ASRenderOITMode` defaults to
the migration value `-1`, while legacy `ASRenderExactOIT` defaults true. The
dispatcher consequently persists mode 1 (Exact OIT) even though the macOS UI is
hidden. Support-aware queries still return false, so OIT does not execute and
vanilla alpha sorting remains selected; this does not explain the Mid boundary.
Darwin should nevertheless force/migrate the stored mode to Standard to make
state match the UI.
