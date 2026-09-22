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
