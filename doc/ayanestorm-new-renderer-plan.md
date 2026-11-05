# AyaneStorm New Renderer: Architecture and Phased Plan

## Context

`doc/ayanestorm-new-renderer-requirements.md` asks for an optional renderer beside OpenGL: at least 5x the OpenGL frame rate in a 16,000-object scene, matching image quality, on Windows and Linux (Vulkan) and Apple Silicon (native), with few `ll*`/`fs*` edits.

**Test hardware:**

- **Windows: RTX 3080 Ti, 12 GB.** Main machine. The 68 and 340 fps figures in the requirements came from an RTX 4090 and are not valid here. The target is 5x a fresh OpenGL baseline on the 3080 Ti.
- **macOS: Mac Mini M4, 16 GB unified memory.** The modest-Mac test case. CPU, GPU and the system share that memory, so `ASRMemoryBudget` must use Metal's reported working set, and the 5x target is measured against a separate OpenGL baseline on this machine.
- **Linux: NAS with a GeForce GT 1030.** Use is kept to a minimum: batched smoke tests in P8, plus any earlier check the user agrees to. Its result proves Linux works, not Linux performance.

**Frame rate rules, set by the user:**

- **No built-in cap.** Speed has no ceiling; the 5x figure is a minimum, not a goal. Users can still cap FPS as today: the render thread applies the existing `FSLimitFramerate`, `FramePerSecondLimit` and `RenderVSyncEnable` settings.
- **Floor: 60 fps at all times with everything rendered correctly,** on the 3080 Ti, in the busiest places, with crowds and features on, at full quality. It is judged on worst frames, not on averages. Lowering quality to reach it is not allowed.
- **Goal:** the fastest and most beautiful engine for Second Life. No compromise on either side.
- **Real render only.** See the real render rule in section 0.

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
- **Real render rule:** every presented frame is fully rendered by the GPU: all visible geometry, lighting, shadows and effects. Banned: frame generation, reprojected or interpolated images, skipping frames, and updating any visible pass or effect every 2 or 3 frames to gain speed. Reusing data that has not changed is allowed, such as static geometry or a static shadow layer, as long as the frame's result is correct.
- **Inherited skipping removed in new mode:** distant avatars animate every frame, not every `mUpdatePeriod` frames. Exact OIT's skip-frames on overflow are replaced by the budgeted OIT storage in section 5.
- **Camera:** the newest main-thread camera plus prediction from raw input and agent velocity, sampled on the render thread and corrected smoothly. LLAgentCamera logic stays as it is.
- **Motion:** object transforms and joint rotations are interpolated on the GPU between snapshots, so objects and avatars move at render rate.
- **Target:** the 5x minimum and 60 fps floor count only frames that follow the real render rule.
- **No laggy world:** high FPS must mean a live world, never the same state drawn many times. Camera, animation and motion must feel at least as responsive as a high-FPS OpenGL session.
- **Main-thread rate:** not artificially low. It runs as fast as it can by default, with `ASRendererSimHz` only as an optional cap, because each main-thread step sets how fresh animation and motion are.
- **Camera:** input is sampled on the render thread right before each frame, so camera lag is lower than today.
- **Animation and motion:** use the newest state and extrapolate forward from velocity, instead of waiting for the next snapshot and interpolating behind. Interpolation is used only to smooth corrections. If the main thread stalls, the camera keeps moving, and motion extrapolates briefly before holding.
- **Freshness gate:** each frame records the age of its camera input and its animation state. Reported as worst values in the frame statistics, and judged together with FPS.
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
- **Avatar impostors:** not used in new mode; impostors are redrawn only every so many frames. Every visible avatar is real geometry with a GPU-selected LOD. `generateImpostor` output needed by UI, such as profile and preview images, comes from the native world path.
- **3D tools:** highlights, beacons, build tools and debug displays use the same backend, depth-testing against world depth.
- **Unmapped programs:** log an error once, so gaps are visible.
- **Stray GL:** direct `gl*` call sites are not edited. On Windows and Linux they already go through the 652 function pointers in `llgl.cpp` (`LL_GL_FUNC_POINTER`). In new mode those pointers are filled by `ASRGLTable`: state and readback calls that reachable code uses go to `ASRBackend`, and everything else is a stub that logs once, so a call added by an upstream merge reports itself. macOS links `<OpenGL/gl3.h>` directly, so one tagged block in `llglheaders.h` remaps the same names to the same table there.
- **GL object names:** `getTexName()` and similar keep returning `GLuint` values. In new mode they are native handles, so callers such as `bindManual` need no edits.
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
- **No temporal shortcuts:** every effect is computed every frame. Effect cost is cut by better algorithms: compute instead of full-screen fragment work, tile classification that skips pixels with nothing to do, and native features.
- **Shadows:** culled the same way, with every cascade correct every frame. Unchanged static content is cached at every memory tier, at lower resolution when the budget is tight, and moving content is drawn over it each frame.
- **Probes:** must beat the OpenGL renderer, which updates one at a time. Unchanged content is cached. Probes affected by a change in sun, environment or nearby moving objects refresh in the same frame, using multi-view rendering through the visibility path. The cost is measured in P4.
- **No quality scaling for speed:** nothing lowers samples or resolution at runtime to hold the frame rate. If a scene misses the floor at full quality, that is a defect to fix in the engine.
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

## P0 baseline results

Busy place, RTX 3080 Ti, OpenGL renderer, frame cap off. First informal reading; resolution, VRAM and the CPU/GPU split are still to be recorded.

| Setup | OpenGL | Frame time | 5x target | Target frame time |
|---|---|---|---|---|
| Most AyaneStorm features on | 23 fps | 43.5 ms | 115 fps | 8.7 ms |
| AyaneStorm features off | 30 fps | 33.3 ms | 150 fps | 6.7 ms |

The features add about 10.2 ms per frame, more than the whole 8.7 ms target frame. Hitting 5x with them on means reducing their own cost, not only removing the base overhead.

A later reading at the same place, features on, was about 15 fps (66.7 ms), and frame rate depends heavily on the people nearby. The real floor is 60 fps in that condition: 16.7 ms, a 4x gain over that reading, with no cap above it. A live busy place is therefore not a stable baseline. Every reading must record the nearby avatar count, and the comparison above is only indicative because the crowd may have changed between readings.

Same place and camera, avatars hidden with Rendering Types > Avatars: features on 42 fps (23.8 ms), features off 60 fps (16.7 ms). Features cost about 7 ms without avatars. Avatars cost about 17 ms with features off (30 to 60 fps), and 20 to 43 ms with features on (from 23 or 15 fps to 42), though those readings were not taken together. The 60 fps reading is not a cap: much higher rates appear with the camera pointed at the sky, at the same place. Camera on the sky at the same place: features on 96 fps (10.4 ms), features off 106 fps (9.4 ms). So work independent of the view, including `idle()`, UI and present, costs at most about 9.4 ms, which alone would cap OpenGL at about 106 fps and rules out the 150 fps target without the section 0 decoupling. Features cost about 1 ms with nothing in view, so almost all their cost scales with scene content. With features off, the rough split is about 9 ms view-independent, 7 ms world, 17 ms avatars. Hiding avatars may leave their CPU animation running, so their full cost may be higher.

## P1 audit results

Read-only audit of this tree. The window and SPIRV-Cross spikes in P1 are still to do.

### Stray GL reachable in new mode
Excluding `as*` GL modules (619 lines in 11 files) and GL world paths, which do not run in new mode, direct calls fall into these groups:

| Group | Calls | Files |
|---|---|---|
| Frame and target state | `glClear`, `glClearColor`, `glViewport`, `glColorMask` | `llviewerdisplay`, `lldynamictexture`, `llviewercamera`, previews |
| Raster state | `glPolygonMode`, `glPolygonOffset`, `glCullFace`, `glPointSize`, `glScissor` | `llselectmgr`, `llviewerwindow`, `llmodelpreview`, `llmanipscale`, `llmaniptranslate`, `llviewerjoint`, `lllocalcliprect` |
| Stencil | `glStencilFunc`, `glStencilOp`, `glStencilMask` | `llmaniptranslate` |
| Readback | `glReadPixels`, `glGetTexImage`, `glGetTexLevelParameteriv` | `llviewerwindow`, `llviewertexlayer`, `llfasttimerview`, `lltexlayer` |
| Info | `glGetString`, `glGetFloatv` | `llappviewer`, `llsnapshotlivepreview` |
| Scene monitor | Queries, framebuffer copies | `llscenemonitor` needs its own path |

All are simple state or readback calls that `ASRGLTable` can serve, so call-site edits are not needed. `llscenemonitor` (the interest-list scene-loading monitor) is the exception and gets a small native replacement.

### Programs reachable in new mode
Of 125 programs, outside world code: `gUIProgram`, `gDebugProgram`, `gObjectPreviewProgram`, `gPhysicsPreviewProgram`, `gCopyProgram`, `gOneTextureFilterProgram`, `gTwoTextureCompareProgram`, `gDeferredPBRAlphaProgram`, `gDeferredPostNoDoFProgram`, `gPBRTerrainBakeProgram`, `gClipProgram`, `gBenchmarkProgram`, `gOcclusionProgram`, `gNormalDebugProgram`, `gFXAAProgram`, `gSMAAEdgeDetectProgram`. The last few are likely debug or GL-world only and are confirmed in P2. This list is a text search; the unmapped-program log is the real check.

### `llrender` seam size
About 340 declared functions across the 10 headers by a rough count, including many trivial getters. Most need no dispatch. Estimate: 100 to 200 one-line tagged early-outs.

Semantics to imitate exactly: `LLGLState::sStateMap` tracking, `LLGLSLShader` uniform caching, `LLVertexBuffer` map and flush timing, `setSubImageFromFrameBuffer`, `LLGLSyncFence`. Blocking readbacks (`glReadPixels` in bakes) must not stall the render thread: the main thread waits, the render thread never does.

### Third-party GL
`llmodelpreview` calls GLOD (`glodInit`, `glodBuildObject`), a prebuilt library that may need a GL context. To check on real mesh upload in P5. If it does, its LOD generation needs a replacement in new mode, with meshoptimizer as the candidate. `llplugin`, `media_plugins`, `llwebrtc` and `llkdu` contain no texture GL calls.

### Why avatars are expensive
Confirmed from code; the cost split still needs Tracy.

- **Every shadow view repeats the work:** `LLPipeline::renderShadow` runs `updateCull` and `stateSort` per shadow camera, and rigged passes are drawn in each. That is up to 4 sun cascades plus 2 projector maps.
- **Per-skin palette upload:** `pushRiggedBatches` calls `uploadMatrixPalette` when avatar or skin changes, with `glUniformMatrix3x4fv` each time. Mesh avatars have many attachments, each with its own skin.
- **Many fragmented draws:** rigged content has its own pass per material type (simple, bump, fullbright, alpha mask and more), so each avatar is drawn in many small batches.
- **Alpha through OIT:** with OIT on, rigged alpha goes through capture (`configureCapturedDrawIfActive`, merged traversal), and Exact OIT adds `ceil(log2(max list))` full-screen sort passes set by the deepest pixel on screen. Layered avatars deepen that, which fits avatars costing more with features on.

The plan targets all four: one skin pass per frame shared by all views, one palette buffer, indirect draws with no per-material batches, and a redesigned native OIT.

### Merge estimate
About 29 upstream-owned files get tagged edits: 11 `llrender`, 4 `llwindow`, 2 `llappearance`, 10 `newview`, 2 CMake. `lllocalcliprect.cpp` drops off the list thanks to `ASRGLTable`.

Upstream commits touching them, 2025-11-05 to 2026-04-08 (674 non-merge commits in total): `llappviewer` 50, `newview/CMakeLists.txt` 33, `llviewerwindow` 20, `llvovolume` 14, `pipeline` 13, `llviewerobject` 8, `llvoavatar` 7, `llwindowwin32` 6, `llviewerdisplay` 3, all others 0 to 2. `llimagegl`, `llvertexbuffer`, `llrendertarget`, `llgl.cpp`, `llflexibleobject`, `llpolymorph` and `llwindowsdl` had none.

The seam sits almost entirely in files with no upstream changes in that window. Busy files such as `llappviewer` get only a few tagged lines, so a conflict needs upstream to change those exact lines. The local upstream ref ends 2026-04-08, so later churn is not counted. A trial merge is still required in each phase.

## Phases

Each phase ends with a user build and test. From P1b on, renderer work is tested in `asrenderer_test` with captured scenes wherever possible, and full viewer builds are saved for seam, UI and live-world changes. Edits are batched to keep rebuilds few. A count of changed `ll*`/`fs*` files is reported every phase.

| Phase | Work | Exit gate |
|---|---|---|
| **P0 Baseline** | On the 3080 Ti at the fixed spot: OpenGL fps, VRAM use, Tracy capture of `idle`, `updateCull`, `updateGeom`, render, GPU | 5x target and VRAM baseline known; main-thread costs ranked |
| **P1 Spikes and audit** | Standalone programs, outside the viewer build: native window with no GL on Windows and macOS; all 63 shaders through SPIRV-Cross and the Metal compiler. Audit of every direct GL call reachable in new mode, every shader program it can bind, and `LLVertexBuffer`/`LLImageGL`/`LLRenderTarget` usage | Presenter and shader path proven; complete `ASRBackend` surface and stray-GL list; merge cost estimated from that list against upstream history |
| **P1b Fast iteration** | `asrenderer` library, `ASRenderer.cmake`, shader build step, `asrenderer_test`, scene capture format; time a no-LTO viewer link | Test program builds and opens a window without the viewer; a shader edit rebuilds only that shader; upstream CMake edits are tagged lines only |
| **P2 Skeleton and UI** | Module, setting, probe, device, fallback, free-running render thread, state snapshots, `ASRMemoryBudget`, `ASRBackend`, window without GL | Login, all floaters, resize work with no GL context over an empty world; render rate unaffected by an artificial main-thread stall; OpenGL mode unchanged |
| **P3 Scene and speed proof** | Prims, mesh, textures with the lossless path, compression under pressure, avatars, culling, visibility buffer and material resolve, unlit G-buffer view, tiled snapshot path with overlap | Target scene geometry pass measured; stop and review if 5x is out of reach. VRAM at or below OpenGL at the same spot. Runs in budget at 8, 6 and 4 GB caps |
| **P3b CPU to GPU** | Everything in the section 0 table: camera prediction, GPU motion and palettes, compute skinning, morphs, flexi, particles, feedback-driven LOD and textures, parallel avatar motion, main-thread early-outs | Busy place with a crowd holds at least 60 fps on worst frames, including during main-thread stalls; main-thread cost compared with P0; particles, flexi and morphs match in comparison mode |
| **P4 Lighting parity** | Sun, moon, shadows, probes, EEP sky, clouds, water, terrain, local lights, projectors | Side-by-side captures |
| **P5 Alpha and tools** | Alpha modes, redesigned OIT modes, particles, glow, HUD, highlights, beacons, build tools, debug displays, media, previews, morphs, bakes, impostors | Alpha-heavy avatar test; every preview floater works |
| **P6 Effects** | GTAO/VBAO, colour grading, LUTs, bloom, flares, vignette, chromatic aberration, motion blur, volumetrics, night sky, sun, moon, auroras, snow, My Lights | Each one in live and snapshot, computed every frame, with no tile seams, and GPU cost reported against P0 with all effects on. Busy place holds 60 fps at full quality |
| **P7 DoF** | Aperture-sampled, then Live, autofocus, tracking; snapshot first | Snapshot quality matches |
| **P8 Hardening** | Linux pass, device loss, previews, stats, acceptance suite | Requirements acceptance list passes, including at 8, 6 and 4 GB caps |

Metal is built alongside Vulkan from P2, not in a final port. Snapshot support and the memory budget apply to every phase from P3: each new feature declares its memory category and tier behaviour, and is tested at the 4 GB cap.

## Main risks

0. **GPU-limited scenes.** Pixel work costs the same in any API, and the real render rule removes temporal shortcuts. The visibility buffer, async compute and better effect algorithms reduce it, but the 60 fps floor at full quality is unproven until measured. A miss is reported to the user with the cause and a proposed engine fix, and quality is never lowered instead. P3, P3b and P6 are the checkpoints.
1. **Prediction artefacts.** Extrapolated camera, joints and objects can overshoot on sudden stops or direction changes. Corrections are blended, and the freshness numbers and real movement are judged in P3b.
1a. **Parallel avatar motion.** `llcharacter` and `LLVOAvatar` assume one thread, and upstream edits them. Shared caches need a P3b audit, and it is the most merge-exposed item here.
2. **Merge surface.** About 29 upstream-owned files carry tagged edits (see the P1 audit), most of them in files upstream rarely changes. Conflicts should stay low because edits are one-line insertions, and stray GL needs no call-site edits. This is an estimate until trial merges confirm it.
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
- FPS measured with AyaneStorm effects off and all on, and in an alpha-heavy scene, each reading with its nearby avatar count.
- Worst camera-input age and animation-state age logged with FPS, in crowds and during main-thread stalls.
- Worst-frame log (minimum, 1% low) against the 60 fps floor, in crowds, on teleport and on region arrival, with presented frames matched one to one against fully rendered frames.
- Snapshot tile-seam test on every effect. Live view watched during a capture at the 4 GB cap.
- `ll*`/`fs*` changed-file count, and a trial upstream merge, reported at each phase.
- Fixed-scene side-by-side captures, live and snapshot.
- Login, teleport, snapshot, alpha-heavy avatar and resize tests on all three platforms.
- OpenGL mode regression check each phase.

## Status and next steps

This file is the single living document for the project. It supersedes the approved plan file, and later notes go here. No renderer code exists yet.

Status at end of 2026-11-05:

- **P0:** closed. Windows FPS readings are recorded above.
- **P1:** read-only audit done. Spikes not started.
- **Answered by the user:** quality is never lowered to hold FPS; interpolation and prediction are acceptable only if the world feels live, not laggy.
- **Confirmed by default, not stated outright:** the 60 fps floor is judged on the 3080 Ti; the Mac and smaller cards get as close as they can.

Waiting on the user:

1. Choose how P1 gets its libraries: install the Vulkan SDK on the Windows machine, or vendor volk, VMA and SPIRV-Cross in `indra/asrenderer`. `VULKAN_SDK` is not set in the shell today.
2. Approve writing the P1 spikes: a native window with no GL on Windows and on the M4, and all 63 shaders through SPIRV-Cross and the Metal compiler.

P0 is closed for design purposes: the readings above set the priorities, and no further measurement blocks work. Further numbers are taken only when a specific decision needs them, or once for the final comparison in P8, and no more are requested before then.

Then, in order: P1 spikes, P1b fast-iteration setup, P2.
