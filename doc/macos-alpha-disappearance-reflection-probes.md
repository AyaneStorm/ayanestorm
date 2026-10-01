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

## Hair With Sky-Coloured Holes (Standard Mode, DoF Off)

Report (2026-10-01): two macOS users, pre-Mac-OIT build, Standard alpha mode,
DoF off. Rigged alpha-blend hair shows hard-edged holes through which the sky,
not the scalp or head, is visible.

Leading hypothesis: rigged alpha draw order combined with depth writes. Vanilla
`LLDrawPoolAlpha::forwardRender()` sets `write_depth = rigged` and draws rigged
alpha first with depth writes enabled (minimum-alpha discard only). If the head
or scalp is also alpha-blend, or the hair's inner layer is a separate
alpha-blend face, and the outer hair face draws first, its semi-transparent
pixels write depth. The surface behind then fails the depth test and the sky
shows through. The edges are hard because they follow the alpha discard
threshold. Ordering within one avatar's rigged alpha can vary with texture
batching or vertex-buffer splitting, which may explain why only macOS shows it.

Second hypothesis: the upstream Apple Silicon alpha-blend disappearance bug
above, which here affects only some faces.

Discriminating tests:

1. Same Mac and outfit in current Firestorm. If Firestorm shows the same
   defect, it is upstream behaviour.
2. Check whether the head or skin uses alpha-blend mode. Switching the head to
   alpha mode None or Mask should fix the first hypothesis.
3. Switching the hair to alpha Mask should remove the holes in both cases.
4. Look for shader or GL errors in the log.

### Firestorm Comparison (Users: Firestorm Is Clean)

Static diff of v1.0.86 against `.phoenix-firestorm-master`. None of the
following explains the defect in Standard mode with DoF off:

- `lldrawpoolalpha.cpp`: the Standard path (`renderNonOITPostDeferred`) is the
  vanilla rigged-then-world `forwardRender()` pair with the same depth writes.
  `ASAlphaGroupTraversal` only reorders in AYAstorm mode (3). The DoF blocks are
  gated by `RenderDepthOfField`.
- `llspatialpartition.cpp`: the sort skip needs OIT requested (false on Darwin).
- Alpha shaders: the OIT branches compile out. The volumetric atlas is sampled
  only when `asVolumetricEnabled`. `bindTransparencyAtlas` sets only that
  uniform while volumetrics are off.
- `lldrawpoolwlsky.cpp`, `ASBackgroundIsolate::renderBaseLayer`,
  `ASWeather::prepare`: all inactive with their features off.
- `llvovolume`/`llvoavatar`/`llmodel`/`llgl`/`llglslshader`: unrelated.
- Reserved-uniform list insertions match the `llshadermgr.h` enum.

Not yet audited: `llviewershadermgr.cpp` (102 tag lines: alpha program
features/defines), `pipeline.cpp` deferred and post passes, GTAO.
Needed from users: exact version, Mac model, `AyaneStorm.log`, value of
`ASRenderOITMode`, and whether volumetrics, GTAO or background isolate are on.

### Developer Mac Logs (2026-10-01)

Mac build 82326 (`0f285a955a`) on an Apple M4 (16 GB), macOS 26.6.2.

Crash at the minimal graphics preset: `Deferred Soften Shader` link error
`No definition of sampleReflectionProbesBent`, then
`ASSERT (mProgramObject != 0)` in `LLGLSLShader::bind`. Cause: GTAO bent
normals made `class3/deferred/softenLightF.glsl` call
`sampleReflectionProbesBent` and `sampleReflectionProbesLegacyBent`, which
existed only in `class3/deferred/reflectionProbeF.glsl`. Fix: forwarding stubs
in `class2/deferred/reflectionProbeF.glsl`.

Hair lead: 77 occurrences of `readBackRaw : GL Error happens before reading
back texture. Error code: 1282` (GL_INVALID_OPERATION left pending by an
earlier call). The source call is unknown. Use `RenderDebugGL` on the Mac to
find it. Vivian's log (older Special build 82817, M4 Max, macOS 27.0.1) has no
shader or GL errors.
