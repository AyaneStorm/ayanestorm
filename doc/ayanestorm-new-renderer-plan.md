# AyaneStorm New Renderer: Architecture and Phased Plan

## Context

`doc/ayanestorm-new-renderer-requirements.md` asks for an optional renderer beside OpenGL: at least 5x the OpenGL frame rate in a 16,000-object scene, matching image quality, on Windows and Linux (Vulkan) and Apple Silicon (native), with few `ll*`/`fs*` edits.

**Test machine: RTX 3080 Ti, 12 GB.** The 68 and 340 fps figures in the requirements came from an RTX 4090 and are not valid here. The target is 5x a fresh OpenGL baseline on the 3080 Ti.

**GPU memory is a hard design constraint.** Target users have 12, 8, 6 or 4 GB. Smoothness may drop at 4 GB; failing, crashing or spilling out of video memory may not.

Facts checked in this tree:

| Fact | Consequence |
|---|---|
| `llui` has one direct GL call (`glScissor`, `lllocalcliprect.cpp`). `llrender2dutils`, `llfontgl`, `llfontvertexbuffer`, `lluiimage` have none. The direct GL sits in `llrender.cpp` (42), `llimagegl.cpp` (70), `llvertexbuffer.cpp` (40) | A native backend under those layers can draw LLUI with no LLUI rewrite |
| Of `llfloater*`, `llpanel*`, `fsfloater*`, `fspanel*`, only `llfloaterimagepreview.cpp` uses raw GL, render targets or shaders; 14 use `gGL` | Floaters mostly work through that backend. Other newview UI (minimap, world map) is checked in P1 |
| Previews, morphs and bakes use `LLRenderTarget`, `LLVertexBuffer` and six programs (`gUIProgram`, `gObjectPreviewProgram`, `gPhysicsPreviewProgram`, `gDebugProgram`, `gDeferredPBRAlphaProgram`, `gDeferredPostNoDoFProgram`) | They can run natively, unmodified, through the same backend |
| Direct GL lines: `llrender` 397 in 10 files, `ll*`/`fs*` `newview` 284 (most in GL world paths new mode skips: `pipeline`, draw pools, reflection probes), `llwindow` 6, `llappearance` 5, `llui` 1 | A GL-free new mode needs one seam in `llrender` and a short list of small edits elsewhere |
| Upstream commits from 2025-11-05 to the local ref's tip (2026-04-08), 774 in total: 5 across 8 `llrender` seam files, 13 in `pipeline.cpp`, 3 in `llviewerdisplay.cpp` | `llrender` is a quiet place for a seam. The local ref may be behind live upstream |
| `LLGLManager::initGL` fills vendor, renderer and VRAM used by `LLFeatureManager` | New mode must supply them from the native device |
| `doFrame()` runs `idle()` then `display()` on one thread. `idle()` runs `gObjectList.update` (all `idleUpdate`, motion interpolation, avatars, flexi), `updateParticles`, `gAgentCamera.updateCamera` | Today every frame waits for all of it. None of it needs to run at frame rate, so the main thread must stop being part of the frame |
| Per-object CPU work has clear entry points: `LLViewerObject::interpolateLinearMotion`, `LLVolumeImplFlexible::doFlexibleUpdate`, `LLViewerPartSim::updateSimulation`, `LLPolyMorphTarget::apply`, `LLVOAvatar::idleUpdate` | Each can be replaced in new mode by GPU work with a small early-out |
| `LLPipeline` has `addObject`, `markMoved`, `markRebuild`, `markTextured`, `markShift`, `unlinkDrawable`, `shiftObjects` | Scene changes can be observed with one tagged line per hook |
| `LLVolumeFace` holds object-space positions, normals, tangents, UVs and indices | Geometry can be extracted without the `rebuildGeom` world batches |
| `LLPickInfo::fetchResults` uses `cursorIntersect` | World picking is a CPU raycast and can stay as it is |
| `LLViewerWindow::getPlatformWindow()` exists on all platforms (null on macOS fullscreen) | A native surface can be added from a new module; macOS fullscreen needs handling |
| `.glslang` 16.6: all 63 `class1/deferred/as*` files tested with `-V -R` at `#version 460`: 30 compile alone. The other 33 stop on missing runtime defines, runtime-composed functions, the SPIR-V target version or multi-file linking. No language incompatibility found. `asDoFLiveGatherF` + `Common` links | Existing effect shaders can be reused as source. Not yet proven at runtime or as MSL |

## Architecture

```
Main thread (own pace)          Render thread (free-running)    Workers
network, agent, UI logic        latest camera + input           volume extraction
idle() -> state snapshot ---->  interpolate motion (GPU)        mesh and texture work
UI commands, previews ------->  cull, draw, effects, present    avatar motion, recording
        <---- GPU visibility and LOD feedback
```

### 0. The main thread never limits the frame rate
This is the core rule. No render-thread frame ever waits for the main thread.

- **Two loops:** the main thread runs network, agent logic, `idle()` and UI at its own pace and publishes versioned state snapshots. The render thread renders from the newest snapshot and never blocks on it. A main-thread stall cannot freeze the view.
- **Every frame is new:** each one has a fresh camera and fresh motion, not a repeated image.
- **Camera:** the newest main-thread camera plus prediction from raw input and agent velocity, sampled on the render thread and corrected smoothly. LLAgentCamera logic stays as it is.
- **Motion:** object transforms and joint rotations are interpolated on the GPU between snapshots, so objects and avatars move at render rate.
- **Target:** the 5x measure counts these frames.
- **Main-thread rate:** `ASRendererSimHz` caps it, with a default at or above the server update rate, so interpolation delay stays small. Tuned on real movement in P3b.
- **Fidelity of moved work:** GPU particles, flexi and morphs run against the CPU versions in a fixed-seed comparison mode until they match. The CPU path stays available in new mode for this test.

**Moved off the CPU in new mode, each main-thread path gets a tagged early-out:**

| Work today | New home |
|---|---|
| Culling, occlusion, LOD selection | GPU compute |
| Texture priority inputs | GPU visibility feedback |
| Object motion interpolation | GPU compute |
| Joint palettes between snapshots | GPU compute |
| Skinning, including every shadow and probe view | GPU compute, skinned once per frame |
| System avatar morphs | GPU compute, morph deltas |
| Flexi prims | GPU compute |
| Particle simulation | GPU compute, from existing emitter data |
| Animated textures, wind | Shaders |
| `rebuildGeom`, `stateSort`, draw submission | Removed in new mode |
| Volume extraction, mesh processing | Workers |
| Avatar motion evaluation | Workers, per avatar, joined inside `gObjectList.update` |

Network handling, agent logic and UI logic stay on the main thread: they are not GPU work, and they no longer affect frame rate. Picking stays a CPU raycast on the main thread.

### 1. No GL in new mode
With the new renderer on, the process creates no GL context. OpenGL stays compiled in, complete and default, and is selected at startup as today. Upstream code keeps calling the same `llrender` APIs in both modes.

- **One seam:** `ASRBackend`, in new files, sits under `LLRender`, `LLTexUnit`, `LLImageGL`, `LLCubeMap`, `LLVertexBuffer`, `LLGLSLShader`, `LLRenderTarget`, `LLShaderMgr` and the `llgl` state classes.
- **Merge pattern:** each dispatch is one tagged early-out at the top of a function (`if (ASR::active()) { return ...; }`). GL bodies are not moved or reformatted, so upstream changes inside them merge cleanly. Callers in `newview` and `llui` are not edited for this.
- **Recording:** draws on the main thread record into command lists; nothing talks to a GPU API there. The render thread replays them.
- **UI:** LLUI code does not change. It re-records at a capped rate (`ASRendererUIHz`), and the last list is replayed at world rate.
- **Previews, morphs, bakes, dynamic textures:** run unmodified through `LLRenderTarget` and `LLVertexBuffer` into offscreen native passes. Their programs are compiled at build time and matched by name.
- **Avatar impostors:** rendered by the native world path instead of `generateImpostor`.
- **3D tools:** highlights, beacons, build tools and debug displays use the same backend, depth-testing against world depth.
- **Unmapped programs:** log an error once, so gaps are visible.
- **Stray GL:** direct calls in reachable code get small tagged edits, from the P1 audit. In new mode GL entry points that the viewer loads are stubbed to log once, so a call added by an upstream merge reports itself; P1 confirms how far stubbing can reach on each platform.
- **Window:** `LLWindowWin32`, `LLWindowSDL` and `LLWindowMacOSX` skip context creation in new mode. The swapchain uses the window directly; macOS gets a `CAMetalLayer` view. These are small tagged edits.
- **Startup:** the native device is probed before the window is created. `LLGLManager` fields are filled from it, so `LLFeatureManager` keeps working.

### 2. Thin backend layer: Vulkan and native Metal
- **Vulkan 1.3:** dynamic rendering, synchronization2, timeline semaphores, descriptor indexing, buffer device address. Uses volk and VMA.
- **Metal 3:** Objective-C++ `.mm` files, Apple GPU family 7+ (M1 and later). No translation layer.
- The layer exposes buffers, textures, pipelines, passes, dispatch, timestamps and `executeIndirect` (`vkCmdDrawIndexedIndirectCount` on Vulkan, compute-filled indirect command buffers on Metal). It covers these two backends only, nothing more general.

### 3. Shaders
Native shaders are GLSL 4.60, compiled at build time by glslang to SPIR-V, then SPIRV-Cross to MSL. Nothing compiles at runtime, and GLSL 4.60 never reaches a GL driver, so Apple's GL limit does not apply. The OpenGL renderer keeps its shaders and its GLSL 4.10 rule.

- **World and material shaders:** new, bindless, native only.
- **UI, preview and tool programs:** existing sources compiled by the same build step, with the defines the viewer would inject.
- **Effect shaders:** reuse the `as*.glsl` sources, keeping GL and new paths on one source so they cannot drift. Shared files stay valid GLSL 4.10 because the OpenGL renderer and macOS still compile them; the 460 version line is added only by the native build step. A small host feeds uniforms using reflection data.
- **Exceptions:** effects with a better native form get native-only shaders (see section 7), leaving the 4.10 originals alone.

### 4. Persistent GPU scene
- `LLPipeline` hooks forward changes to `ASRScene`; workers extract from `LLVolumeFace`.
- Geometry is deduplicated by volume and LOD and instanced, in shared arenas. A moved object updates one transform, with no rebuild.
- Tables: instances, materials (from texture entries), joint palettes. Rigged mesh, animesh, Bento and system avatars share them.
- Terrain, trees, grass, clouds, particles, water and sky each get a small extractor.
- World `rebuildGeom` and `stateSort` do not run in new mode. The spatial partition stays for picking and interest lists, updated at main-thread pace. GPU visibility feedback replaces GL occlusion queries and drives LOD and texture priority.

### 5. Frame
Compute culling (frustum, two-phase Hi-Z, LOD) -> indirect visibility buffer -> compute material resolve into G-buffer -> clustered lighting -> transparency -> effects -> composite.

- **Visibility buffer from the start:** geometry writes only triangle and instance IDs, so hidden and tiny triangles cost almost no shading. Each visible pixel is shaded once.
- **Same G-buffer layout:** the resolve writes the GL layout, so reused effects and parity work unchanged.
- **Async compute:** effects run on the async compute queue where the hardware has one, overlapping geometry work.
- **Temporal reuse:** expensive effects (volumetrics, GTAO, DoF gather) may reuse history with reprojection. Each one must pass side-by-side captures in live and snapshot, and snapshots never depend on history they have not converged.
- **Shadows:** culled the same way. Static content is cached at every memory tier, at lower resolution when the budget is tight. Far cascades update on a staggered schedule.
- **Probes:** time-sliced, a few faces per frame, using the visibility path.
- **Alpha:** alpha-masked content uses the visibility path. Blended content is culled against Hi-Z and depth-bounded per tile, so empty pixels cost nothing. OIT layer storage comes from the budget, with a stated per-pixel limit and no silent drop in quality above it.

### 6. Textures
- Decoded images go from `LLViewerFetchedTexture::createTexture` to a transfer queue and a bindless slot, replacing mips in stages. Old images are freed after a fence.
- All textures, UI and world, live in one native store, with no second copy.
- UI images and fonts stay uncompressed.
- **Lossless path:** at 12 GB and above, world textures stay uncompressed, or use BC7 (ASTC on Apple) only if side-by-side captures show no visible difference. Quality is the default.
- **Compression only under pressure:** BC1, BC3 and BC5 are encoded on workers only when the budget needs them, starting with distant and low-detail textures. Skin, faces and alpha edges are compressed last. RGBA8 to BC1 is 8:1 and to BC3 4:1.

### 6a. GPU memory budget
One `ASRMemoryBudget` owns all video memory. Nothing allocates outside it.

- **Source:** `VK_EXT_memory_budget`, or Metal's recommended working set, read at runtime. `ASRendererVRAMBudgetMB` can lower it. The budget reacts to other applications' use.
- **Fixed categories:** render targets, shadows, probes, geometry, textures, OIT, UI, staging. Each has a limit, and textures take what is left.
- **Tiers from the budget:** render-target scale, shadow size and cascade count, probe count and size, OIT buffer size, effect resolution. Defaults fit the device; user settings apply within it.
- **Degrade order:** savings with no visible cost first (aliasing, evicting unused LODs and unseen mips), then compression of distant textures, then mips of distant content, and resolution of effects and shadows last. What is near the camera keeps quality longest.
- **No GL memory:** in new mode there is no GL context, and `LLPipeline` allocates no screen, shadow, probe or effect targets of its own. Offscreen preview targets come from the budget and are released when closed.
- **Render targets:** the frame graph aliases transient targets. Apple uses memoryless attachments where possible.
- **Geometry:** quantized vertices (16-bit positions, octahedral normals), instancing, and chunked arenas that grow only as needed. Inactive LODs are evicted first.
- **Pressure response:** textures start lowering mips at 85% of budget, ordered by distance and GPU visibility feedback. Above 95% new loads wait. Oversubscribing video memory is treated as a bug.
- **Snapshots:** tile size shrinks to fit free budget, and the live view is never evicted. 11500x11500 is always tiled, because a full-size G-buffer needs several GB. On low-memory cards a capture takes longer instead of disturbing the view.
- **Reporting:** per-category use appears in frame statistics.

### 7. AyaneStorm features
New `asr*` modules read the same settings; GL `as*` modules stay untouched.

**Mac OIT is redone, not ported.** Its design came from GL 4.1 limits (no compute, SSBOs, atomics). The same algorithm goal is rebuilt for the new renderer: on Metal with raster order groups or imageblocks, on Vulkan with compute and atomics. Exact OIT and AVBOIT therefore also become possible on macOS.

### 8. Snapshots
The frame path renders to any target from its first day. Snapshots run as a separate render context, so the live view keeps going. Tiling covers 11500x11500, and DoF keeps its time-sliced capture to stay under the 30 s limit.

Tiles overlap by each effect's declared reach (AO radius, DoF blur, volumetric and bloom kernels), and only the inner region is kept, so screen-space effects see across tile edges. Every effect declares its reach when added and is tested for seams.

### 9. Failure and selection
- `ASRendererBackend` selects the renderer, restart required; default stays OpenGL.
- Unsupported hardware falls back to OpenGL with a message, decided by the probe before the window exists.
- A failure after startup logs the cause, stores the OpenGL choice for the next launch, and exits cleanly.
- Device loss logs the cause, then recovers or exits cleanly.
- `display()` gets one tagged early branch.

### Layout
- `indra/asrenderer/`: backends, frame graph, GPU scene. No viewer or GL dependency.
- `indra/newview/asr*.cpp`: viewer bridge and features.
- `indra/asrenderer/shaders/`: new shaders.
- `indra/asrenderer/test/`: `asrenderer_test`, standalone program.

### Build
CMake and autobuild stay. There is no new build system, because upstream changes those files and a replacement would conflict on every merge.

- **Merge rule:** all build logic lives in new files: `indra/cmake/ASRenderer.cmake`, `indra/asrenderer/CMakeLists.txt`, and `indra/newview/asr_sources.cmake` for the `asr*` sources. Upstream's `indra/CMakeLists.txt` and `indra/newview/CMakeLists.txt` get only a few tagged lines (`add_subdirectory`, `include`, link). The long source lists in `newview/CMakeLists.txt` are not edited.
- **Separate library:** `asrenderer` has no viewer dependency, so renderer code compiles apart from the viewer.
- **`asrenderer_test`:** a small standalone program. Its builds never link the viewer, and testing needs no login.
- **Scene capture:** the viewer dumps a scene (geometry, materials, textures, lights, camera, environment) to a file. The test program replays it at a fixed camera for repeatable FPS, memory and side-by-side checks.
- **Shaders:** the build step recompiles only changed shaders, with no C++ rebuild.
- **Optional no-LTO development build:** a separate build directory with `USE_LTO=OFF`, an existing switch. Official builds keep LTO. Link time is measured before relying on it.
- **Limit:** real UI, networking, teleports and live region loading still need the full viewer.

## Phases

Each phase ends with a user build and test. From P1b on, renderer work is tested in `asrenderer_test` with captured scenes wherever possible, and full viewer builds are saved for seam, UI and live-world changes. Edits are batched to keep rebuilds few. A count of changed `ll*`/`fs*` files is reported every phase.

| Phase | Work | Exit gate |
|---|---|---|
| **P0 Baseline** | On the 3080 Ti at the fixed spot: OpenGL fps, VRAM use, Tracy capture of `idle`, `updateCull`, `updateGeom`, render, GPU | 5x target and VRAM baseline known; main-thread costs ranked |
| **P1 Spikes and audit** | Standalone programs, outside the viewer build: native window with no GL on Windows and macOS; all 63 shaders through SPIRV-Cross and the Metal compiler. Audit of every direct GL call reachable in new mode, every shader program it can bind, and `LLVertexBuffer`/`LLImageGL`/`LLRenderTarget` usage | Presenter and shader path proven; complete `ASRBackend` surface and stray-GL list; merge cost estimated from that list against upstream history |
| **P1b Fast iteration** | `asrenderer` library, `ASRenderer.cmake`, shader build step, `asrenderer_test`, scene capture format; time a no-LTO viewer link | Test program builds and opens a window without the viewer; a shader edit rebuilds only that shader; upstream CMake edits are tagged lines only |
| **P2 Skeleton and UI** | Module, setting, probe, device, fallback, free-running render thread, state snapshots, `ASRMemoryBudget`, `ASRBackend`, window without GL | Login, all floaters, resize work with no GL context over an empty world; render rate unaffected by an artificial main-thread stall; OpenGL mode unchanged |
| **P3 Scene and speed proof** | Prims, mesh, textures with the lossless path, compression under pressure, avatars, culling, visibility buffer and material resolve, unlit G-buffer view, tiled snapshot path with overlap | Target scene geometry pass measured; stop and review if 5x is out of reach. VRAM at or below OpenGL at the same spot. Runs in budget at 8, 6 and 4 GB caps |
| **P3b CPU to GPU** | Everything in the section 0 table: camera prediction, GPU motion and palettes, compute skinning, morphs, flexi, particles, feedback-driven LOD and textures, parallel avatar motion, main-thread early-outs | Full target scene with moving avatars and objects holds render rate during main-thread stalls; main-thread cost compared with P0; particles, flexi and morphs match in comparison mode |
| **P4 Lighting parity** | Sun, moon, shadows, probes, EEP sky, clouds, water, terrain, local lights, projectors | Side-by-side captures |
| **P5 Alpha and tools** | Alpha modes, redesigned OIT modes, particles, glow, HUD, highlights, beacons, build tools, debug displays, media, previews, morphs, bakes, impostors | Alpha-heavy avatar test; every preview floater works |
| **P6 Effects** | GTAO/VBAO, colour grading, LUTs, bloom, flares, vignette, chromatic aberration, motion blur, volumetrics, night sky, sun, moon, auroras, snow, My Lights | Each one in live and snapshot, with no tile seams, and GPU cost reported against P0 with all effects on |
| **P7 DoF** | Aperture-sampled, then Live, autofocus, tracking; snapshot first | Snapshot quality matches |
| **P8 Hardening** | Linux pass, device loss, previews, stats, acceptance suite | Requirements acceptance list passes, including at 8, 6 and 4 GB caps |

Metal is built alongside Vulkan from P2, not in a final port. Snapshot support and the memory budget apply to every phase from P3: each new feature declares its memory category and tier behaviour, and is tested at the 4 GB cap.

## Main risks

0. **GPU-limited scenes.** Pixel work costs the same in any API. The visibility buffer, async compute and temporal reuse reduce it, but if the target scene is GPU-limited with all effects on, 5x is not guaranteed. P0 shows where the baseline stands, and P6 reports the result with all effects on.
1. **Prediction and interpolation artefacts.** Camera prediction can overshoot on sudden stops, and interpolation adds up to one snapshot of delay to object motion. Corrections are blended, and `ASRendererSimHz` sets the trade-off. Judged on real movement in P3b.
1a. **Parallel avatar motion.** `llcharacter` and `LLVOAvatar` assume one thread, and upstream edits them. Shared caches need a P3b audit, and it is the most merge-exposed item here.
2. **Merge surface.** About 10 `llrender` files, 3 `llwindow` files and the stray-GL list carry tagged edits, so the `ll*`/`fs*` count is higher than a GL-keeping design. Conflicts should stay low: one-line insertions in files upstream rarely changes. P1 estimates it before work starts; if it is too high, that is the point to stop.
3. **Silent upstream drift.** Upstream can add a GL call or a shader that new mode does not handle. OpenGL mode is unaffected. Stubs and unmapped-program logs expose it; a post-merge check lists new direct GL calls.
4. **VRAM.** Heavy AyaneStorm effects could exceed 4 and 6 GB. Limited by section 6a and tested with budget caps on the 3080 Ti; no real 4 GB card is in the test set, so a cap does not prove the driver's behaviour on one.
5. **`llrender` API semantics.** Some callers may rely on GL behaviour the backend must imitate exactly, such as bind state, sync readback or mapped-buffer timing. Found in the P1 audit and P2.
6. **Upstream changes** to volume or material data can break extraction. The bridge stays narrow, with a check after each merge.
7. **Parity of legacy lighting** across two codebases. Reusing shader source and the G-buffer layout limits it. Temporal reuse and any compression must pass the side-by-side captures. Below 12 GB the budget still costs some quality; the degrade order keeps that away from what is near the camera.

## Critical files (tagged, minimal edits)

`indra/CMakeLists.txt`, `indra/newview/CMakeLists.txt` (tagged lines only), `indra/newview/llviewerdisplay.cpp`, `pipeline.cpp`, `llappviewer.cpp`, `llviewerwindow.cpp`, `llviewertexture.cpp`, `indra/llrender/llrender.cpp`, `llimagegl.cpp`, `llvertexbuffer.cpp`, `llglslshader.cpp`, `llrendertarget.cpp`, `llshadermgr.cpp`, `llcubemap.cpp`, `llcubemaparray.cpp`, `llgl.cpp`, `indra/llui/lllocalcliprect.cpp`, `indra/llwindow/llwindowwin32.cpp`, `llwindowsdl.cpp`, `llwindowmacosx.cpp`, `llwindowmacosx-objc.mm`, plus the P1 stray-GL list. Reused unchanged: `lltexturefetch`, `llmeshrepository`, `llvolume`, spatial partition, all other LLUI.

## Verification

- `glslang` validation of every shader; Tracy and frame statistics on the target scene against a fresh OpenGL baseline on the 3080 Ti.
- Per-category VRAM against OpenGL at the same spot; teleport, snapshot and alpha-heavy tests with `ASRendererVRAMBudgetMB` at 8, 6 and 4 GB.
- Every shared `as*.glsl` still validates at `#version 410 core`.
- Confirmed in new mode: no GL context is created. Stub logs stay empty across the test set.
- Render rate logged while the main thread is deliberately stalled, and during teleport and region arrival. A frame counts only if its camera or motion state is new.
- FPS measured with AyaneStorm effects off and all on, and in an alpha-heavy scene.
- Snapshot tile-seam test on every effect. Live view watched during a capture at the 4 GB cap.
- `ll*`/`fs*` changed-file count, and a trial upstream merge, reported at each phase.
- Fixed-scene side-by-side captures, live and snapshot.
- Login, teleport, snapshot, alpha-heavy avatar and resize tests on all three platforms.
- OpenGL mode regression check each phase.

## Housekeeping after approval

Copy this file to `doc/ayanestorm-new-renderer-plan.md`; later notes go in that one file. Update the two stale lines in `MEMORY.md`, and record the test machine and memory constraint in memory.
