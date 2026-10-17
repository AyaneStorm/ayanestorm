# Dual-Backend GTAO Implementation

## Summary

Implement XeGTAO-derived scalar ambient occlusion as an optional alternative to existing SSAO:

- `Legacy SSAO` remains the default.
- `GTAO` offers Medium, High, and Ultra quality; High is the GTAO default.
- OpenGL 4.0 fragment/FBO backend supports macOS OpenGL 4.1.
- OpenGL 4.3 compute backend is automatically preferred when available.
- Both backends share the algorithm, settings, formats, and output convention.
- Failure falls back compute → fragment → legacy SSAO without disabling AO.
- GTAO uses a dedicated AO texture and denoiser; it does not pass through the shared shadow blur.
- No temporal accumulation, bent normals, generated normals, auto-tuner, or RTAO in this version.

Before implementation begins, copy this approved plan verbatim to `doc/ayanestorm-gtao-implementation-plan.md`. Append implementation status and test results to that same document as milestones complete; do not create another GTAO document.

## Interfaces, Settings, and UI

### AS-owned module

Add `asambientocclusion.h/.cpp`, authored `chanayane@firestorm`, with this backend-neutral responsibility:

- Define `Technique { LEGACY_SSAO = 0, GTAO = 1 }`.
- Define `Backend { AUTO = 0, FRAGMENT = 1, COMPUTE = 2 }`.
- Register, load, and unload optional GTAO shaders independently of the core deferred shader load result.
- Allocate/release resolution-dependent textures and static Hilbert LUT.
- Render GTAO from the active deferred target and return whether a valid result exists for the current frame.
- Bind/unbind the valid GTAO result for the deferred soften pass.
- Report requested and effective technique/backend for logging and diagnostics.
- Expose a module shader-cache revision and include it in the existing AS cache hash without bumping the global shader version during development.
- Compile cleanly on macOS; only compute loading and dispatch are capability-gated, not the entire module.

Required lifecycle contract:

1. `loadShaders(shader_level)` always attempts the fragment backend on GL 4.0+.
2. It additionally attempts compute on GL 4.3+ when compute entry points are present.
3. `allocateResources(width,height)` is lazy/no-op unless GTAO is selected.
4. `render(deferredScreen, screenTriangle)` verifies dimensions and lazily reallocates if needed.
5. `releaseResources()` is called with screen-buffer and GL-context teardown.
6. Every pass restores framebuffer, viewport, texture/image bindings, color mask, blend, and depth state expected by the deferred pipeline.

### Persistent settings

Add:

| Setting | Type/default | Contract |
|---|---:|---|
| `RenderAOTechnique` | S32 `0` | `0` legacy SSAO, `1` GTAO |
| `RenderGTAOQuality` | S32 `2` | `1` Medium, `2` High; clamp invalid values |
| `RenderGTAORadius` | F32 `0.5` | View-space metres; UI range `0.05–5.0`, increment `0.05` |
| `RenderGTAOFinalValuePower` | F32 `2.2` | Visibility power; range `0.5–5.0`, increment `0.05` |
| `RenderGTAODenoisePasses` | S32 `1` | `0` Disabled, `1` Sharp, `2` Medium, `3` Soft |
| `RenderGTAOBackend` | S32 `0` | Developer setting: Auto, Force Fragment, Force Compute |

Keep `RenderDeferredSSAO` as the master AO enable to preserve feature masks, existing preferences, shader selection, and upstream compatibility. Backend, quality, radius, power, and denoise changes apply live without a core deferred shader reload.

Keep Intel’s remaining tuned constants compiled at their defaults:

- Radius multiplier `1.457`
- Falloff range `0.615`
- Sample distribution power `2.0`
- Thin-occluder compensation `0.0`
- Depth-mip sampling offset `3.30`
- Noise index `0`

### Phototools

Use the screenshot’s current layout:

- Keep the existing AO checkbox.
- Add a `Technique` combo immediately below it.
- Increase the section by one row.
- Use two overlapping seven-row panels whose visibility is switched by `RenderAOTechnique`.
- Disable the technique and its controls when master AO is off.
- Mirror the layout in normal and advanced Phototools so their shared controller remains valid.

Legacy panel remains unchanged:

1. Scale
2. Max Scale
3. Factor
4. Irradiance Max
5. Irradiance Scale
6. Effect
7. AO Soften

GTAO panel:

1. Quality: Medium / High
2. Radius
3. Power
4. Denoise: Disabled / Sharp / Medium / Soft
5. Irradiance Max
6. Irradiance Scale
7. Effect

Reuse existing irradiance and effect settings because they control AO’s lighting contribution after visibility generation. Do not show legacy Factor, Scale, Max Scale, or AO Soften in GTAO mode. Add concise tooltips explaining quality cost, world-space radius, visibility power, and the edge-aware denoiser.

Use optional `findChild` access where necessary during XUI construction, connect the technique setting to panel visibility, and update visibility both on commit and `refreshSettings()`.

## GTAO Rendering Implementation

### Algorithm ownership and licensing

Adapt the vendored `.XeGTAO` algorithm, not its rendering framework:

- Mark substantially derived sources and shaders as derived from Intel XeGTAO and refer to the vendored full MIT notice at `.XeGTAO/LICENSE`; do not reproduce Intel copyright headers in AyaneStorm-owned files.
- Port scalar visibility only.
- Exclude ImGui, DirectX abstractions, reference RTAO, auto-tuning, bent normals, and normal reconstruction.
- Use the existing decoded view-space G-buffer normal.
- Use GLSL `float`; retain bandwidth savings through R16F/R8 storage. Do not introduce vendor-specific 16-bit arithmetic initially.

`vaGTAO_RT.hlsl` was reviewed. It is the DirectX ray-traced reference/ground-truth path used by XeGTAO diagnostics and auto-tuning, so it supplies no raster/compute pass required by this scoped port and remains excluded with RTAO.

Put the horizon integration, edge packing, denoise weighting, Hilbert/R2 noise, fast trigonometric helpers, and tuned constants in one AS-owned GLSL common-function source attached to both fragment and compute programs. Backend entry shaders contain only resource access and dispatch/draw-specific code.

Treat GLSL 4.00/4.10 keywords and future-reserved words as forbidden identifiers in all shared and fragment-backend sources for macOS compatibility. Compute-only GLSL 4.30 keywords must remain confined to compute entry shaders.

Compile separate Medium, High, and Ultra main-pass permutations so slice/step counts are constant at each entry point:

- Medium: 2 slices × 2 steps in both directions.
- High: 3 slices × 3 steps in both directions.
- Ultra: 9 slices × 3 steps in both directions, matching XeGTAO's Ultra preset.

### Shared resources

Allocate at active deferred resolution:

- One five-level `GL_R16F` positive view-depth texture.
- Two full-resolution `GL_R8` visibility textures for main output/denoise ping-pong.
- One full-resolution `GL_R8` packed four-neighbour edge texture.
- One static 64×64 `GL_R16UI` Hilbert-index texture.
- Backend FBOs; attach individual mip levels as needed and verify completeness.

Set point filtering and clamp-to-edge where the reference requires explicit samples. Total resolution-dependent storage is approximately 5.66 bytes/pixel, about 47 MB at 4K, plus an 8 KB LUT.

Resource allocation must be transactional: retain or restore a valid previous allocation if a replacement fails, mark GTAO unavailable for that frame, and log the failure once.

### Coordinate and depth contract

Use positive view depth internally, matching XeGTAO.

For conventional OpenGL perspective projection and raw depth `d`:

```text
ndcZ      = 2*d - 1
viewDepth = P[3][2] / (ndcZ + P[2][2])
```

For orthographic projection:

```text
viewZ     = (ndcZ - P[3][2]) / P[2][2]
viewDepth = -viewZ
```

Detect perspective versus orthographic from the projection matrix. Validate both formulas against the renderer’s inverse-projection reconstruction. Clamp non-finite/background depth to `65504`, the maximum finite R16F value.

Convert viewer view space to XeGTAO convention consistently:

```text
position: (x, y, z) → (x, -y, -z)
normal:   (x, y, z) → (x, -y, -z)
```

For bottom-left OpenGL UVs, use:

```text
NDCToViewMul = ( 2*tanHalfFovX, -2*tanHalfFovY )
NDCToViewAdd = (-tanHalfFovX,     tanHalfFovY )
```

Derive projection constants every frame so FOV, aspect ratio, snapshots, and projection changes cannot leave stale data.

Prefer explicit `texelFetch`/`textureLod` for depth, neighbour, and denoise reads. Do not translate HLSL `GatherRed` until a GLSL gather implementation has passed backend-equivalence tests.

### OpenGL 4.0/4.1 fragment backend

Use the existing fullscreen triangle vertex shader and AS-owned fragment programs:

1. Linearize raw deferred depth into mip 0.
2. Render four weighted 2× downsample passes into mip levels 1–4.
3. Run the GTAO horizon pass with MRT output to visibility ping and packed edges.
4. Run `max(1, configured passes)` edge-aware denoise draws, ping-ponging visibility. With denoise disabled, use the reference high-beta/no-blur behavior needed to finalize and pack visibility correctly.
5. Expose the final ping/pong texture without copying.

Each mip pass binds the exact destination level and viewport. Handle odd dimensions with clamped source coordinates.

### OpenGL 4.3 compute backend

Closely preserve XeGTAO’s optimized organization:

1. An 8×8 workgroup processes a 16×16 source tile and produces all five depth mips using shared memory.
2. An 8×8 main dispatch writes visibility and packed edges.
3. Each denoise invocation processes two horizontal pixels where applicable.
4. Round dispatch dimensions up and bounds-check every store.
5. Insert `GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT` between producer/consumer stages and before fragment sampling.

The compute and fragment implementations must use identical constants, sampling positions, edge encoding, visibility scaling, power application, and final UNORM conversion.

### Deferred-pipeline integration and fallback

Add only small ownership-tagged hooks to shared/upstream files:

- Shader manager: include/register/load/unload the AS module. GTAO shader failure must not fail all deferred shaders.
- Screen-buffer lifecycle: call GTAO allocate/release beside existing AS modules.
- Deferred lighting: render GTAO after the G-buffer is complete and before the sun/light-map pass.
- CMake: list only the new AS C++ header/source.
- Sun and soften shaders: add the minimal conditional integration described below.

When AO is enabled and not rendering a cube snapshot:

1. Resolve requested backend:
   - Auto: compute if loaded and supported, otherwise fragment.
   - Force Fragment: fragment.
   - Force Compute: compute if available, otherwise fragment with one warning.
2. Render GTAO when the requested technique is GTAO.
3. If rendering succeeds, mark effective AO as GTAO for this frame.
4. If it fails, mark effective AO as legacy SSAO and warn once.

Modify `sunLightSSAOF.glsl` with an effective-source uniform:

- Legacy/fallback: execute the existing `calcAmbientOcclusion()` unchanged.
- Valid GTAO: write `1.0` to the light map’s AO channel while preserving all three shadow channels.

Modify `softenLightF.glsl` minimally:

- Add the dedicated GTAO sampler and effective-source uniform under the existing AO permutation.
- Sample GTAO visibility when valid; otherwise use `lightMap.g`.
- Leave `adjustIrradiance()` and material AO behavior unchanged.

The existing shadow blur still processes `deferredLight`; GTAO remains separate and therefore is not blurred by it. A constant green channel of `1.0` remains unchanged through that blur. Off, legacy SSAO, shadows, and cube snapshots retain their existing paths.

## Validation and Acceptance

The repository agent must not build the viewer. After implementation, provide the user with one consolidated build/runtime test request.

### Static and failure-path checks

- All new files have comments, correct author, and Intel MIT attribution where derived.
- Every necessary `ll*`/`fs*` edit is minimal and enclosed in `<AS:Chanayane>` tags, with replaced original code retained as comments.
- No global shader-version bump during development.
- Validate FBO completeness, GL errors, uniform/sampler bindings, mip dimensions, and resource teardown.
- Simulate unavailable compute, compute compilation failure, fragment compilation failure, and allocation failure; verify compute → fragment → SSAO fallback and one-time logging.
- Verify live technique/backend/quality changes do not leak resources or require a viewer restart.

### Image-quality tests

Capture identical static-camera frames for:

- Off, legacy SSAO, GTAO Medium, GTAO High, and GTAO Ultra.
- Forced fragment and forced compute.
- Indoor corners, outdoor terrain, avatars, thin geometry, distant geometry, silhouettes, sky/background pixels, PBR and legacy materials.
- Near/far-plane extremes, several FOVs, resized windows, render-resolution scaling, and ordinary/high-resolution snapshots.
- macOS GL 4.1 fragment, plus Windows/Linux NVIDIA, AMD, and Intel GL 4.3 where available.

Backend parity on a static frame:

- At least 99.9% of final R8 pixels differ by no more than one UNORM step.
- Maximum difference is two steps, excluding explicitly documented edge pixels caused by permitted floating-point ordering.
- Larger structured differences block acceptance.

Legacy mode must show no intentional visual change from the current renderer.

### Performance tests

Use existing GPU profiling zones around depth preparation, main GTAO, each denoise pass, and the total module.

For each backend, measure after 60 warm-up frames over at least 200 frames at 1080p, 1440p, and 4K for Medium, High, and Ultra using the same fixed scene/camera:

- Record median and 95th-percentile GPU time per stage and total.
- Compute must have a lower median total time than fragment at each tested resolution/quality on representative GL 4.3 hardware.
- Compute p95 must not exceed fragment p95 by more than 5%.
- If compute fails this relative criterion, optimize dispatch/access patterns before shipping it as Auto’s preferred backend.
- Confirm Legacy SSAO has no measurable regression when GTAO is not selected.

## Assumptions

- Medium, High, and Ultra are the supported GTAO quality choices; Low is intentionally omitted.
- Ultra with a 1.70 m radius, 2.20 power, and three-pass Soft denoise is GTAO's default.
- Legacy SSAO remains the overall default technique for existing and new installations.
- GTAO requires GL 4.0; lower-capability configurations retain legacy SSAO.
- Temporal index remains zero until the renderer gains a suitable temporal-reprojection consumer.
- The existing deferred normal attachment is authoritative; GTAO does not reconstruct normals from depth.
- GTAO affects ambient/reflection-probe irradiance through the existing AO composite and does not replace per-material PBR AO.

## Approved Scope Addition

- Add live `RenderAODebugWhite` comparison mode in both Phototools views. It renders neutral white materials while retaining the selected AO and existing shadows, making Legacy SSAO/GTAO differences directly visible.
- Add the AO enable control, Legacy SSAO/GTAO selector, white comparison toggle, and a link to Phototools under AyaneStorm Preferences → Rendering 2. Detailed AO tuning remains in Phototools.

## Implementation Status

- 2026-09-22: Added persistent settings, the AS-owned module, transactional fragment and compute backends, XeGTAO-derived shared depth/main/denoise shaders, pipeline composite hooks, the neutral-white AO diagnostic, both Phototools layouts, and AyaneStorm Preferences → Rendering 2 controls. XML validation, ownership-tag checks, and `git diff --check` pass. Build and runtime validation remain for the user.
- 2026-09-22: First runtime shader load exposed NVIDIA GLSL rejection of `packed` as a reserved keyword in `gtao_unpack_edges`. Renamed it to `packed_value`; the reported fragment and compute-depth errors were cascades from that shared-source failure. Rebuild validation pending.
- 2026-09-22: Audited all GTAO shader identifiers against the GLSL 4.10 keyword and future-reserved-word lists; no remaining collisions or double-underscore identifiers were found.
- 2026-09-22: Second runtime shader load showed that this viewer compiles attached helper and entry files as separate GLSL objects. Added explicit shared-function prototypes to every fragment/compute entry shader and redeclared `gtao_radius` in the two depth-mip entry objects; the logged undefined-symbol errors were all instances of this compilation-unit issue. Rebuild validation pending.
- 2026-09-22: Extended neutral-white debug handling to the late legacy alpha, PBR alpha, blended material, fullbright/fullbright-shiny, and GLTF alpha/unlit passes. These paths now retain opacity and directional-shadow silhouettes without restoring texture, emissive, environment, fog, reflection, or local-light color; HUDs and reflection-probe cube captures remain excluded.
- 2026-09-22: Runtime comparison exposed visible screen-fixed Hilbert sampling grain at High quality with a 1.5 m radius even under the three-pass Soft denoiser. Kept Sharp at XeGTAO's reference beta and strengthened only Medium/Soft center weights (0.7/0.35) in both fragment and compute paths; filtering remains depth-edge-aware and spatially stable.
- 2026-09-22: Added Ultra quality using XeGTAO's 9-slice × 3-step preset for both fragment and compute backends. Medium 2×2 and High 3×3 remain available.
- 2026-09-22: Adopted the runtime-tuned GTAO defaults: Ultra, 1.70 m radius, 2.20 power, Soft denoise, irradiance maximum 0.180, irradiance scale 0.60, and effect 0.800. The last three already matched the shared AO defaults. Corrected both Phototools irradiance slider/spinner ranges from effectively integral on/off controls to fractional ranges appropriate to those values.

## Abrupt avatar darkening investigation (2026-10-16)

Author: chanayane@firestorm

### Reproduction evidence

GTAO abruptly darkens an avatar when the camera moves farther away; legacy
SSAO does not. The white-material comparison also shows the change over the
face and opaque clothing. This is a discrete change, not a gradual increase
in contact occlusion. Screenshots inspected:
`AyaneStormOS-Normal_7GCe9eHvL7.png`,
`AyaneStormOS-Normal_adnxyvs9XA.png`,
`AyaneStormOS-Normal_6vXfmIbtQz.jpg`, and
`AyaneStormOS-Normal_njRFkphcBB.png`.

### Initial depth mip hypothesis (runtime test failed)

The user confirmed that the abrupt darkening still occurs with Bent normals
disabled, excluding directional bent-normal lighting as the required cause.

`asGTAOCommonF.glsl` selected depth mips through `textureLod` while the module
bound the texture with `GL_NEAREST_MIPMAP_NEAREST`. Camera distance changes the
projected AO radius and the requested LOD; crossing a half-integer LOD replaced
one occluder representation with another immediately. This is a concrete
scalar visibility discontinuity. Legacy SSAO does not use this depth mip chain.
The port also calculated LOD after rounding the pixel offset, introducing
additional discrete changes absent from the reference ordering.

A GPU regression fixture reproduced a visibility jump of 0.33557045 across
LOD 0.49999 to 0.50001. It tested both an occluder versus a flat surface and a
background sentinel versus an occluder. This establishes the mip-boundary bug;
confirmation that it explains the original avatar reproduction requires the
user's runtime test.

### Initial mip-blending patch and validation (superseded)

The initial patch evaluated the valid, distance-weighted horizon at both
adjacent mip levels and interpolated their occlusion contributions. It retained
point depth sampling within each mip. Interpolating raw depth instead would
invent intermediate surfaces and contaminate valid geometry with the 65504
background sentinel. Clamping each level to its unoccluded horizon before
interpolation also prevents a below-surface sample cancelling real occlusion.
Integer LODs retain their previous result and need only one depth lookup.
LOD is calculated from the unsnapped sample offset; the spatial lookup is then
snapped to pixel centers as before.

The change is shared by fragment and compute backends, all four quality levels,
and scalar/bent-normal modes. No settings, C++, shader versions, or cache
revision were changed. Fractional LOD samples require an additional depth read
and horizon evaluation; runtime performance remains to be measured.

Validation completed:

- Khronos `.glslang/bin/glslang.exe`: the edited shared fragment source combined
  with the main entry passes GLSL 4.10 in scalar and bent-normal modes. Compute
  combinations pass GLSL 4.30, the version required by the compute backend;
  glslang rejects a compute-stage input at 4.10 even with ARB extensions.
- Mesa OpenGL 4.5 software GPU: all 16 main-pass quality/backend/bent-normal
  permutations compile and link with shared and entry sources as separate
  shader objects, matching the viewer's compilation-unit organization.
- GPU regression fixture: both previously abrupt transitions change visibility
  by only 0.00000674 after the fix. Integer-mip equivalence, continuity at
  integer boundaries, and GL error checks pass.
- Earlier scratch numerical checks of an unoccluded tilted plane at 3440x1328
  around 2 m and 4 m did not reproduce broad darkening with either FP16 or
  FP32 depth; depth precision was left unchanged.

No viewer build or original-scene runtime test was performed. Runtime check:
move across the original camera-distance boundary with GTAO Bent normals off,
then on, including the white-material comparison.

### Source references

Reference implementations: online
[XeGTAO shader source](https://github.com/GameTechDev/XeGTAO/blob/master/Source/Rendering/Shaders/XeGTAO.hlsli).
The reference permits nearest or linear selection between depth mip levels.
This fix blends their horizon contributions to handle silhouette and background
discontinuities without interpolating artificial depth.

### Follow-up: cached texture binding and mip-state corruption

The user reported that the initial mip-blending patch did not fix the avatar
switch. New screenshots `AyaneStormOS-Normal_anemK0hTOM.png` and
`AyaneStormOS-Normal_bHpkHfEFW6.png` show the change across opaque clothing and
the skirt, with comparatively stable pavement. The switch also persists with
`RenderGTAOBackend = 1` (Fragment). Bent normals and a compute-only failure are
therefore excluded as necessary causes.

`LLTexUnit::bindManual()` activates the intended texture unit and updates
`mHasMipMaps` only when the texture ID changes. On a binding cache hit it does
neither. `LLTexUnit::setTextureFilteringOption()` does not activate its unit;
it applies `glTexParameteri` to whichever unit is currently active.
`setTextureAddressMode()` activates its unit, but GTAO previously called that
only after changing the filter.

Consequently, if GTAO binds its mipmapped depth texture on unit 0 and the normal
texture is already bound on unit 1, setting unit 1's non-mipmapped point filter
can change the depth texture's minification filter from
`GL_NEAREST_MIPMAP_NEAREST` to `GL_NEAREST`. Even explicit `textureLod()` then
samples the base level, abruptly changing occlusion. Initial texture bindings
can depend on preceding scene draws; camera distance itself is not a condition
inside this defective helper. Establishing which draws trigger this in the
reported scene still requires runtime verification.

A second state defect existed in restoration: GTAO saved texture IDs and types,
but restored them through `bindManual()` without its `hasMips` argument. This
lost the original texture unit's cached mip-map flag.

#### Correction

- `asambientocclusion.cpp::bindRawSampler()` explicitly activates the intended
  unit and sets its mip flag even when `bindManual()` hits the binding cache.
- `bindResult()` uses that same helper, removing its duplicate unsafe sequence.
- The module snapshots the three complete `LLTexUnit` cache objects. It restores
  the actual GL bindings through the existing API, then restores each cache
  object, including its mip flag. `LLTexUnit` contains only its unit index,
  texture ID/type and mip flag; this needs no new shared renderer API.
- The unsuccessful horizon-blending functions and additional depth reads were
  removed. The reference ordering which calculates LOD before snapping the
  sample offset remains. Shader/cache versions are unchanged.

#### Validation and remaining runtime check

A Mesa OpenGL GPU regression reproduces the old cached-bind path: changing only
whether the normal texture is already bound changes the depth minification
filter and lowers visibility from 1.0 to 0.66442955. Explicit activation keeps
the filter and visibility identical for both cached and uncached bindings.
This proves the state-dependent rendering defect, independently of the original
camera-distance reproduction.

All 16 actual main-pass quality/backend/bent-normal permutations compile and
link as separate shared and entry shader objects. Khronos validation passes for
the edited shared shader combined with the main entry in scalar and bent-normal
modes: GLSL 4.10 fragment and GLSL 4.30 compute. No viewer build was attempted.
The user must still verify the original camera transition; do not report that
scene as fixed until this runtime result is available.

### Binding correction runtime result: original issue persists

The user confirmed that the binding correction also did not fix the reported
camera-distance switch. Screenshots `AyaneStormOS-Normal_fV76HEnMN5.png` and
`AyaneStormOS-Normal_ANF02R0ARL.png` show the difference in white-material mode.
The user further described the effect as their own avatar's shadow falling on
another avatar, and said that viewing the other, unshadowed side shows no issue.
Nearby geometry can contribute to GTAO within the configured world-space
radius (default effective search radius: 1.70 * 1.457 = 2.4769 m), but the
screenshots do not establish whether this is scalar AO, directional shadows,
or their final lighting combination. Neither earlier isolated GPU regression
established the cause of this particular scene.

The saved configuration also enables Exact volumetric lighting and cloud
shadows, whose through-cloud fill consumes GTAO. Disabling directional shadows
also disables that volumetric path, so such a test alone cannot distinguish
those two lighting stages. A requested runtime comparison with
`RenderShadowDetail = 0` is pending; GTAO remains enabled for that comparison.

### Raw GTAO visibility diagnostic: RenderBufferVisualization = 7

Added `ASAmbientOcclusion::visualizeResult()` and a minimal tagged pipeline
switch case. After the normal tonemapping/postprocessing, this draws the final
GTAO visibility texture through the existing deferred buffer-view shader.
White is unoccluded (1); black is occluded (0). It selects R for scalar R8 or A
for bent-normal RGBA8 and temporarily replicates that component into RGB via
texture swizzles. The swizzles, texture-unit cache and active unit are restored.
No new shader, shader version, cache revision or persistent setting is needed.
The diagnostic is inactive for the default `RenderBufferVisualization = -1`.

After the user's next build:

1. Keep GTAO enabled and set Debug Setting `RenderBufferVisualization` to `7`.
2. Capture the same camera positions immediately before and after the switch.
3. Restore `RenderBufferVisualization` to `-1`.

If raw AO jumps too, investigate its depth/normal inputs and sampling. If raw AO
stays stable while the lit image jumps, investigate directional/cloud shadows
and the lighting composite. The white-material view still includes lighting
and shadows and cannot make this distinction.

Validation: the actual existing buffer-view fragment shader compiles and links
on the software OpenGL GPU. R8 and RGBA8 cases display the correct black, grey
and white visibility values; original texture swizzles and sampled values are
restored afterward, and GL errors remain zero. No shader file was edited for
this diagnostic. No viewer build was attempted. This is diagnostic work, not a
claimed fix for the user's reproduction.

### Hypothesis: off-screen samples clamped to the screen border (2026-10-17)

`gtao_main_pass()` clamps sample UVs to `[0, 1]`
(`uv0 = clamp(uv + sample_offset, 0, 1)`). Off-screen samples therefore read
the depth of the border pixel instead of being discarded.

The screen-space radius is large at portrait distances. Effective radius is
1.70 * 1.457 = 2.477 m. At 3440x1328 with SL's default 60 degree vertical FOV,
one pixel at depth d covers about d * 0.000869 m, so
`screen_radius ~= 2850 / d` px: about 1425 px at 2 m, against a 664 px half
height. Most outer steps land outside the screen and collapse onto the border.

Any object at the border that is closer to the camera and within 2.477 m of
the shaded surface then raises the horizon for nearly every pixel of the
subject at once: another avatar's hand or body, the user's own avatar, or the
near pavement at the bottom edge. Zooming or rotating moves the frame edge
across such objects, so visibility changes for the whole avatar in one frame.
This matches: broad uniform darkening, camera-angle dependence, a nearby avatar
as the apparent source, no effect from the other side, and no effect with
legacy SSAO, whose kernel stays small. Both screenshots show another avatar's
hand at the right edge.

Proposed fix: give off-screen samples zero falloff weight, so they keep the
unoccluded low horizon. Screen-space AO has no information outside the screen.

Zero-build runtime check: lower `RenderGTAORadius` to about 0.3 m and repeat
the zoom. A smaller screen radius keeps samples on screen, so the switch should
disappear. `RenderBufferVisualization = 7` shows whether raw AO switches.

Runtime result: radius 0.3 m removed the switch, confirming the cause. The
user keeps 1.7 m for its overall look.

Fix applied in `asGTAOCommonF.glsl`: the clamped UV is still used for the
fetch, but `weight0/1 *= float(uv == uv_raw)` zeroes off-screen samples, which
then keep the low horizon. Shared by fragment and compute backends and both
scalar and bent-normal modes. glslang passes (GLSL 4.10 scalar/bent-normal,
4.30). No shader version or cache revision change. Pending build and runtime
test.

Runtime result: the transition is now progressive. Remaining artifact: an
avatar in front of another darkens the one behind (screen-space AO has no
thickness; a foreground surface within 2.477 m acts as an infinite wall).

### Thickness heuristic: ASGTAOThinOccluderCompensation

XeGTAO's thin occluder compensation computes the falloff distance as
`length(vec3(delta.xy, delta.z * (1 + k)))`. AyaneStorm applies the stretch to
foreground samples only (`delta.z < 0`, nearer the camera than the shaded
pixel). Samples behind keep their weight, preserving concave corners such as a
floor meeting a back wall. Example: hair 0.6 m in front, k = 2, gives a 1.8 m
falloff distance instead of 0.6 m, mostly outside the 0.95-2.48 m falloff.

- Setting `ASGTAOThinOccluderCompensation`, F32, default 2.0, clamped 0-10,
  read every frame (live tuning). 0 restores the previous behavior.
- Uniform `gtao_thin_occluder_compensation` set in `applyMainUniforms()`;
  shared by fragment/compute and scalar/bent-normal.
- glslang passes for main fragment (4.10) and compute (4.30) entries, scalar
  and bent-normal. No UI yet. Pending build and runtime test.

Runtime result: raising the setting looks about like lowering the radius on all
objects, not only avatars behind other avatars. This is inherent to the
heuristic. A sample raises the horizon only when it lies toward the camera
(`dot(delta, view_vec) > 0`), so nearly every real occluder is a foreground
sample. Restricting the stretch to `delta.z < 0` therefore changes little, and
the concave-corner argument above is weak. The heuristic is a depth-only radius
cut: lateral reach stays, depth reach shrinks to about
`0.95 m / (1 + k)` full weight and `2.48 m / (1 + k)` zero weight. It cannot
tell a 0.6 m gap behind thin hair from a 0.6 m deep solid occluder.

Proper alternative: visibility bitmask GTAO (Therrien, Levesque, Gilet 2023,
"Screen Space Indirect Lighting with Visibility Bitmask"). Each sample is a
slab of assumed thickness that occludes only its own sector of the hemisphere,
so light passes behind thin foreground objects instead of one max horizon per
side. Requires replacing the horizon loop and visibility integral.

Decision: keep the heuristic, runtime-tuned default 1.0 (full weight below
0.48 m depth gap, none beyond 1.24 m). Bitmask GTAO not pursued for now.

### Follow-up: VBAO

The thickness limitation is addressed by a third technique, VBAO
(`RenderAOTechnique = 2`), sharing the GTAO pipeline. See
`doc/ayanestorm-vbao-implementation-plan.md`.
