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

### 0a. Compute once: the result registry
Rule set by the user: nothing is computed twice, and nothing is duplicated. Any computed thing, expensive or cheap, that more than one consumer can use lives in one registry. Shadow maps, depth pyramid, skinned vertices, AO and light lists are examples.

- **One producer:** each result has one producer. Consumers ask for the result by ID and never know who made it. No feature keeps a private copy or redoes the work. A result nothing needs is not computed.
- **Scope chosen per result:** each result declares what suits it: per frame and view, per frame and shared by all views, or kept across frames until its inputs change. Reuse across frames follows the real render rule in section 0: the frame's result must stay correct. A snapshot tile is its own view.
- **Overrides:** a new feature changes a result by overriding its producer, like a subclass overriding a virtual method, and consumers need no edits. An override either replaces the producer, or modifies the previous output.
- **Modifying never recomputes from zero:** a modifying override receives the existing output and only adds its own work to it. Overrides of one result stack in a declared order. Replacing a producer must also stop the replaced one running, so its cost is not paid for nothing.
- **Very fast lookup:** IDs are dense integers fixed at registration. Producers, consumers and scopes are declared at startup, and the frame graph resolves them when it compiles, so passes hold direct references and a normal frame does no lookups. Any lookup left at frame time is one array index and one stamp compare: no strings, hashing or locks.
- **Recompile only on change:** the frame graph recompiles only when an override, setting or tier changes, never per frame.
- **Reporting:** frame statistics show each result's cost, including every override, and flag any result computed more than once in its scope as a bug.

### 1. No GL in new mode
With the new renderer on, the process creates no GL context. OpenGL stays compiled in, complete and default, and is selected at startup as today. Upstream code keeps calling the same `llrender` APIs in both modes.

- **One seam:** `ASRBackend`, in new files, sits under `LLRender`, `LLTexUnit`, `LLImageGL`, `LLCubeMap`, `LLVertexBuffer`, `LLGLSLShader`, `LLRenderTarget`, `LLShaderMgr` and the `llgl` state classes.
- **Merge pattern:** each dispatch is one tagged early-out at the top of a function (`if (ASR::active()) { return ...; }`). GL bodies are not moved or reformatted, so upstream changes inside them merge cleanly. Callers in `newview` and `llui` are not edited for this.
- **Recording:** draws on the main thread record into command lists; nothing talks to a GPU API there. The render thread replays them.
- **UI:** native, not translated. LLUI code does not change. A tagged early-out in `LLRender::flush` copies the finished vertices into one per-list buffer, with no vertex buffer, no GL program and no hashing. The cached replays in `LLVertexBufferData::draw` and `drawWithMatrix` get the same treatment, because they bypass `flush`. One native UI shader covers `gUIProgram` and `gSolidColorProgram`. Textures are bindless, so a texture change does not split a draw; only blend, scissor and target changes do. It re-records at a capped rate (`ASRendererUIHz`), and the last list is replayed at world rate.
- **Speed decides:** the goal is the fastest renderer. Merge cost is weighed only between designs of equal speed.
- **`ASRGLTable` is never a drawing path.** It answers queries, catches stray calls and hands over state such as scissor and blend. World drawing never goes through it.
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
- The layer exposes buffers, textures, pipelines, passes, dispatch, timestamps and indirect draws. It covers these two backends only, nothing more general.
- **Geometry draws:** no per-object draws and no command lists. Compute culling appends visible clusters to lists, and each list is one instanced indirect draw (`vkCmdDrawIndirect`, `drawPrimitives:indirectBuffer:`) that reads its vertices from memory. See section 5. This replaces the earlier plan of `vkCmdDrawIndexedIndirectCount` and compute-filled indirect command buffers, which would have needed a hand-written Metal shader and could only cull whole faces.

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

- **Clusters are the unit of culling and drawing:** each face is cut into clusters of up to 64 triangles on a worker. The GPU rejects whole clusters that are outside the view, facing away or, from step b, hidden. So a body of 60,000 triangles only pays for the parts in sight, which face-level culling cannot do. Clusters of 2, 8, 32 and 64 triangles have their own list and draw, so a two-triangle prim face does not pay for 64.
- **The known cost:** with no index buffer each triangle runs three vertices, against about 1.2 with reuse. Vertex work here is a few memory reads and a matrix, and fewer triangles reach the rasterizer, so this pays for itself on dense content. Mesh shaders could later replace the draw using the same cluster data, as an override, where a GPU has them.
- **Visibility buffer from the start:** geometry writes only a 32-bit visible cluster and triangle number, so hidden and tiny triangles cost almost no shading. Each visible pixel is shaded once.
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

This file is the single living document for the project. It supersedes the approved plan file, and later notes go here. No renderer code is in the viewer yet.

Status at end of 2026-11-05:

- **P0:** closed. Windows FPS readings are recorded above.
- **P1:** closed. Read-only audit, shader spike and window spike are all done, see below.
- **P1b:** done except the no-LTO viewer link timing, which needs a viewer build. `indra/asrenderer` builds alone with `cmake -S indra/asrenderer -B <dir>`, and `asrenderer_test` draws a shader-driven triangle in a window on the 3080 Ti (Vulkan, 0 validation errors) and the M4 (Metal), with no GL context. `--selftest` checks the scene file format, including damaged files. A shader edit rebuilds only that shader, with no C++ compiled: under a second on Windows. `asr_add_shader` handles native shaders only: reusing the `as*` sources needs the header injection and libraries proven by the shader spike, which is P2 work. Only the camera chunk of the scene format has a fixed layout; the others are defined in P3. The viewer has one tagged line, `include(ASRenderer)` in `indra/CMakeLists.txt`, with `AS_RENDERER` off by default, and the viewer has not been reconfigured with it yet. Linux has not been built: `asrenderer` has no Linux device code until the P8 pass.
- **P1 window spike result:** a native window was cleared and resized with no GL context on both platforms, and the user saw the Windows window on screen. Windows: Vulkan 1.3 through volk on the RTX 3080 Ti, 0 validation errors, about 10,000 fps with validation off and 5,000 with it on. M4: native Metal 3, Apple7 family, 11.8 GB working set, 119 fps. Code is in `E:\Dev\AyaneStorm\Vulkan\window-spike`, outside the repo.
- **'No GL' means no GL context, not no GL library:** on Windows `opengl32.dll` appears in the process after device creation although the program never uses it, and NVIDIA's Vulkan driver is itself `nvoglv64.dll`. On macOS `OpenGL.framework` is loaded before `main` although the program does not link it. No context existed at any check point. The verification test must check for a context (`wglGetCurrentContext`, `CGLGetCurrentContext`), not for loaded modules.
- **The Mac render loop is not capped at the display refresh:** with the screen active and low power mode off, the Metal window, seen working by the user, submitted 546 fps (10,911 frames in 20 s, `displaySyncEnabled = NO`). The display is set to 100 Hz in macOS, so it can only show 100 of those per second. The earlier 119 fps was with the screen inactive. With low power mode on, the user read 571 fps, so that mode makes no real difference here. What limits a bare clear at 546 fps is not known; it does not block anything.
- **Metal validation is part of the Mac test:** run `asrenderer_test` with `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`. On the test triangle, API and GPU validation both reported 0 error lines and exit code 0, and the log confirmed each was enabled. The `no validation errors` line in the program counts Vulkan errors only, so on the Mac the proof is the log and the exit code. Whether a deliberate error is caught has not been tested. The run was capped at about 120 fps by the inactive screen, so it says nothing about speed.
- **Viewer configure on Windows:** the first viewer build after the `include(ASRenderer)` line configured and linked, with `AS_RENDERER` off and no `asrenderer` build folder. The Mac and Linux viewer configures are untested.
- **No-LTO link timing, P1b closed:** on Windows, after a one-file change (`llviewerdisplay.cpp` touched), the no-LTO viewer rebuilt in 31 s of MSBuild time. A full no-LTO rebuild took 15:01. `bn_nolto.cmd` builds in `build-vc170-64-nolto` and sets `AS_SKIP_INSTALLER`, so `viewer_manifest.py` staged the whole viewer but built no installer. That switch is the only repo change for it, and normal builds are unaffected. Use this build for P2 viewer tests. The user confirmed this build reaches the login screen; logging in has not been tested.
- **P1 shader spike result:** all 63 `as*.glsl` files went through glslang 16.6, SPIRV-Cross (`--msl-argument-buffers`, MSL 3.0) and Apple's `metal` compiler on the M4 with no failures and no shader source edits: 53 programs, plus 5 libraries linked through host shaders, and the other 5 libraries linked inside those programs. This proves compilation only: nothing has run yet, and only one variant per file was tested (GTAO High, AVBOIT 8192 slices). The build step must reproduce what `LLShaderMgr` injects: the stage define, G-buffer defines, `GBufferInfo`, permutations and shared libraries. It must also replace `diffuseLookup`, which the viewer generates at runtime. Test scripts are in `E:\Dev\AyaneStorm\Vulkan\shader-spike`, outside the repo.
- **Answered by the user:** quality is never lowered to hold FPS; interpolation and prediction are acceptable only if the world feels live, not laggy.
- **Confirmed by default, not stated outright:** the 60 fps floor is judged on the 3080 Ti; the Mac and smaller cards get as close as they can.

Waiting on the user: nothing at present. The Vulkan SDK 1.4.363.0 is installed on Windows, and volk, VMA and SPIRV-Cross are vendored in `indra/asrenderer/third_party`. Test work goes in `E:\Dev\AyaneStorm\Vulkan` or a dotted project folder.

Test machine toolchains checked 2026-11-05, in login shells (a plain SSH command on the Mac has a shorter PATH and misses cmake):

| Machine | OS | Present | Missing |
|---|---|---|---|
| Mac Mini M4 | macOS 26.6.2, Xcode | git, clang++, cmake 3.31.2, Metal Toolchain, ninja 1.13.2, glslang 16.6.0, SPIRV-Cross | Nothing |
| Linux NAS, GT 1030 | Debian 13 | cmake, git, g++, NVIDIA driver, Vulkan loader 1.4.309, ninja 1.12.1, glslang 15.2.0, SPIRV-Cross | NVIDIA Vulkan ICD file, vulkaninfo, clang++ |

Windows `.glslang` is 16.6, the same as the Mac. The NAS's 15.2.0 is older, so official SPIR-V output must not come from it.

The NAS has no `nvidia_icd.json` in `/etc/vulkan/icd.d` or `/usr/share/vulkan/icd.d`, so Vulkan cannot reach the GT 1030 until that is fixed. This matters from the P8 Linux pass, not for the P1 spikes.

P0 is closed for design purposes: the readings above set the priorities, and no further measurement blocks work. Further numbers are taken only when a specific decision needs them, or once for the final comparison in P8, and no more are requested before then.

Next: P3, ordered below so that the viewer is built as rarely as possible.

| P2 step | Work | State |
|---|---|---|
| **a. Library** | `LatestValue`, `RenderThread`, `MemoryBudget`, `Device::probe` | Done on Windows and Mac |
| **b. Viewer start** | `AS_RENDERER` in the viewer, `ASRendererBackend`, probe before the window, window without GL, `ASRGLTable` stubs, Windows first | Done on Windows |
| **c. Native UI, library** | Textures, one UI vertex buffer, merged draws, bindless UI shader, replay on the render thread, tested in `asrenderer_test` | Done on Windows and Mac |
| **c. Native UI, viewer** | Tagged `LLRender::flush` and `LLVertexBufferData` early-outs, texture hand-over, `createShader` skipped so the 10 s load goes | Done on Windows: login screen drawn with no GL context |
| **d. UI complete** | Capped UI rate, last list replayed at world rate; logged in over an empty world; login, floaters, resize; viewer stall test; explain the one-row difference below | Done on Windows |

P2 is done on Windows. The Mac and Linux viewer sides of P2 are still open: the user chose to start P3 first.

| P3 step | Work | State |
|---|---|---|
| **a. Scene core, library** | Cluster builder, scene queue and store, chunks and tables, compute culling, visibility buffer, resolve to albedo and normals, unlit view, tested in `asrenderer_test --scene` | Done on Windows and Mac |
| **b. Hidden geometry, library** | Frame graph and result registry (section 0a), depth pyramid, two-phase Hi-Z culling, LOD selection on the GPU, alpha-masked and double-sided lists | To do |
| **c. Viewer: prims** | Render camera from the viewer, `LLPipeline` hooks, workers extract `LLVolumeFace`, materials from texture entries, world textures through the same queue, viewer FPS reads the render thread | To do |
| **d. Viewer: mesh and scene capture** | Mesh, sculpts, terrain as plain geometry, capture file for repeatable readings | To do |
| **e. Avatars** | Joint palettes, compute skinning, system avatars, attachments, rigged mesh | To do |
| **f. Textures under pressure** | Lossless path, compression and mip lowering from the budget, tests at 8, 6 and 4 GB | To do |
| **g. Snapshots and the speed gate** | Tiled snapshot path with overlap, target scene measured against P0 | To do |

- **P3a result on the 3080 Ti:** a made-up city of 16,000 objects, 80,332 face instances and 17.1 M triangles, with no occlusion culling yet, about 390 objects moving at 45 Hz and a 10,000-triangle mesh replaced twice a second. At 2560x1440 it ran at about 470 fps (2.1 ms), 467 fps with the main thread stopped, and worst frame 7.6 ms. About 200,000 clusters were drawn. Scene, UI and resize together gave 0 validation errors. A screenshot showed shapes, facing, textures and mips to be right.
- **P3a result on the M4:** the same scene with Metal API and GPU validation on passed with no errors, and about the same number of clusters was drawn (202,000 against 201,000). The user saw the picture and it looks the same as on Windows. With the machine idle, at 1280x720: 84 fps (11.9 ms), 84 fps with the main thread stopped for 2 s, worst frame 20.3 ms. Metal has no pass timers yet, so where those 11.9 ms go is not known.
- **Pass timers:** on Windows they show cull 0.16 ms and visibility 1.64 ms, and resolve 0.08 ms. The resolve figure is not believable at 3.7 M pixels, so the split between visibility and resolve must not be used yet. The total frame time is real. To be solved in step b, where passes get timed by the frame graph.
- **Why addresses, not bindings:** tables and geometry are reached by device address from push constants, so a growing table needs no rebinding, and one GLSL source became valid Metal through SPIRV-Cross on the first try, including atomics and `textureGrad` in compute. Metal cannot see which buffers a shader reaches this way, so one `MTLFence` orders the passes by hand.
- **Geometry memory:** 64 MB chunks that never move, so clusters can hold addresses. The city takes 77 MB of geometry. A moved object writes one 64-byte entry, and several moves between two frames are merged into one write.
- **Uploads cannot stop the renderer:** the first version could: 64 MB of textures plus 64 MB of scene data per frame slot exceeds the staging budget even at 12 GB. Now one frame's upload share is fixed at startup from the staging budget, so every frame in flight fits. Instances that do not fit wait for the next frame, in order, and a large texture goes up in bands of rows over several frames. At a 4 GB cap the city and a 4096x4096 texture load normally. At 300 MB, far below the floor, geometry waits and nothing fails.
- **Test options:** `--size W H` sets the window and `--keep-size` skips the resize the test does on purpose, so speed readings stay at one size.
- **Mac viewer build:** `/Users/ayane/ayanestorm/vulkan/bn_nolto.sh` is `bn.sh` without LTO, with `AS_RENDERER` on and its own build folder, `build-darwin-universal-nolto`. `bn.sh` is untouched.
- **P2a result:** `asrenderer_test` now renders on its own thread, and the test freezes the main thread for 2 s. On the 3080 Ti: 4852 fps running against 4937 fps stalled, with Vulkan validation on and 0 errors. The worst stalled frame took 1.7 ms, and the newest state was 2002 ms old, which proves nothing from the main thread reached those frames. On the M4: 120 fps in both, against the inactive-screen cap. The test triangle is the only load, so this proves the thread design, not world rendering.
- **Memory budget:** the 11.05 GB on the 3080 Ti comes from `VK_EXT_memory_budget`, against 11.8 GB of heap. Metal's figure is fixed per device, so on the Mac the budget does not react to other applications, as section 6a assumed it would. Category shares are starting values until the tiers exist.
- **Swapchain size:** Vulkan now compares the requested size with the last requested one. Comparing with the real size would rebuild the swapchain every frame when the main thread's size lags behind the window.
- **`ASRGLTable` needs no list of names:** on Windows and Linux all 652 pointers load by name through `LL_GET_PROC_ADDRESS`, so one tagged macro line in `llgl.cpp` routes them all to the table. A function upstream adds later is covered with no edit. A few functions reply like a driver, so `LLGLManager::initGL()` runs unchanged; the rest are stubs that log their first call.
- **P2b result:** on the 3080 Ti, `--set ASRendererBackend Native` started the viewer with no GL context at either check, reached `STATE_LOGIN_WAIT` with all UI code running against the table, and closed cleanly with 0 validation errors. The user saw the test triangle in the viewer's window. The render thread drew 85,886 frames in about 13 s. The newest state reached 246 ms old from ordinary main-thread pauses, and the worst frame was 15.5 ms. The user also confirmed OpenGL mode still works in this build. `--set` lasts one session, so testing does not change the saved setting. Logging in, the Mac and Linux are not tested.
- **GL functions reached up to the login screen:** 48 stubs, plus the functions that reply like a driver. Texture and buffer upload (`glTexImage2D`, `glTexSubImage2D`, `glTexImage3D`, `glBufferData`, `glBufferSubData`, `glGenerateMipmap`), shaders (`glShaderSource`, `glCompileShader`, `glAttachShader`, `glBindAttribLocation`, `glLinkProgram`, `glUseProgram`, `glGetAttachedShaders`), binding (`glBindTexture`, `glActiveTexture`, `glBindVertexArray`, `glBindBuffer`, `glBindFramebuffer`, `glFramebufferTexture2D`), state (blend, cull, depth, scissor, viewport, draw and read buffers, clear) and draws (`glDrawArrays`, with no `glDrawElements` yet). This list covers the login screen only: floaters and a logged-in session will add to it.
- **Native UI library result:** `asrenderer_test --ui` rebuilds a UI of 7,602 flushes at 60 Hz on the main thread, in 1.3 ms at worst, and those flushes merge into 4 draws. That is 4,237 fps on the 3080 Ti with Vulkan validation on and 0 errors, and 120 fps on the M4 (inactive-screen cap) with Metal API and GPU validation on and 0 errors. The picture was checked by eye on both, and matches.
- **How draws merge:** vertices are transformed as they are copied, strips and fans become plain triangles, and each vertex carries its texture slot, sampler and solid-color bit. So matrix and texture changes never split a draw.
- **Metal needs macOS 15:** residency sets make a bindless texture cost nothing per frame, so the probe refuses older systems and the viewer stays on OpenGL there.
- **How the viewer reaches the renderer:** `llrender` is built with or without `AS_RENDERER`, so it talks to a small interface in `asrbridge.h`, which `asrviewer.cpp` fills in. Only `.cpp` files include it, so `llrender.h` and `llvertexbuffer.h` stay untouched and a change here never rebuilds the viewer. One virtual call per flush costs nothing next to the vertex work behind it.
- **Textures are handed over in `LLImageGL`, not in `glTexImage2D`:** `setImage()` and `setSubImage()` pass the pixels straight on, and the GPU makes the mips, which also skips the CPU mip loop. A name maps to its slot and sampler through one array read at draw time. `LLTexUnit` reports wrap and filter per texture. `ASRGLTable` only keeps what GL state it is the natural owner of: blend, scissor, and texture names. `setManualImage()` does nothing in this mode: what still reaches it is render targets and world data.
- **Colour of `gSolidColorProgram`:** that shader reads a uniform, but every native program reports a colour attribute, so `gGL.color4f()` fills the vertex and the one native shader reads it there.
- **Login screen result:** compared with OpenGL pixel by pixel outside the web page, which changes on its own, the two matched except for one row. The Log In button was 68 rows tall in OpenGL and 67 in native, and the selection in the username field lost its bottom row.
- **The one-row difference is a rasterization tie:** the UI scale here is 1.25, so a dump of the UI list showed the bottoms of those two shapes at exactly y = 1228.5 and 1258.5, which is a pixel centre. GL keeps such a row at the bottom of a shape, Vulkan at the top. The UI viewport now sits 1/128 pixel lower (`UI_EDGE_NUDGE`), which decides every tie as GL does. The same tie exists wherever the UI is scaled to a fraction.
- **A list keeps its own size:** the render thread can show a list after the window has changed, as during a resize in a main-thread stall. Stretching it to the new size also left its pixel-sized scissors behind, and hid most of the UI. Each list now records its size and is drawn at that size from the top left corner. The rest of the window stays black until the next list. `asrenderer_test --ui` draws lists larger and smaller than the window in turn.
- **P2 logged-in result on Windows:** logged in to a real region over a black world, opened floaters and logged out. UI looked normal to the user, no program was left out, and no texture lacked a native path. Normal render rate about 7,600 fps. Validation was off in this run, so its 0 errors prove nothing, and the same goes for the viewer's earlier readings. A later run with `ASRendererValidation` on covered the login screen, a logged-in session and four stalls with 0 errors. The render rate fell to about 1,500 fps there, which shows the layers really ran.
- **Login row after the fix:** rows 1257 and 1287 now match OpenGL exactly, and the largest difference in the row fell from 203 to 14. The menu bar is exact. The shift has a small cost: UV coordinates move with it, so about 7,400 more pixels of scaled images differ by 1 to 3 levels out of 255, and a few at the side edges of the Log In button by up to 14. None of this shows to the eye, so it stays.
- **Resize:** a size change now rebuilds the UI at once, ignoring `ASRendererUIHz`, so a list of the wrong size stays on screen as briefly as possible. The user saw a slight flicker while resizing, before this change. OpenGL's `display()` clears to black and skips its frame on every resize, so it should flicker too, but the two have not been compared side by side.
- **Viewer stall test result:** five 5-second stalls held 6,058 to 8,104 fps, against about 7,600 normally, with a worst frame of 4.5 ms and 5,000 ms state age. The window could also be dragged and maximized during a stall, so the window thread stayed live. A list too small for the maximized window stays intact in the corner, with black around it, until the main thread returns.
- **Two FPS figures now:** the viewer's FPS display counts main-thread loops, 700 to 800 here, not the frames the render thread presents. Its statistics must read the render thread's count once the world draws, in P3.
- **Empty world until P3:** after `STATE_STARTED`, `display()` runs its usual early part (teleport screen, logout progress, frame counters), then `display_native()` instead of the world. That function keeps alive what the minimized branch of `display()` keeps alive: pools, HUD object timers, texture updates and GLTF material cleanup. That branch already runs for hours in real sessions with nothing culled or built, so the session stays healthy. `rawSnapshot()` returns false in this mode, so the logout snapshot cannot replace the saved login image with an empty one, and the snapshot floater shows nothing until P3.
- **No rebuild-when-unchanged:** dropped, because `LLView::sIsRectDirty` cannot say the UI is unchanged. Only `dirtyRect()` sets it, and that is called for views being added or removed, buttons, floaters and menus. Typed text, new chat and stat bars never touch it, so skipping on it would show stale text. `ASRendererUIHz`, 60 by default, limits the main-thread cost instead.
- **No fullscreen work needed:** `switchContext()` is called only from the window constructor, and `restoreGL()` is marked deprecated in the code, so the native window never changes after startup. The surface rebuild in `publishFrame()` could not run, and is removed.
- **Test setting:** `ASRendererStallTestAt` stops the main thread for 5 s and logs the render rate. It takes seconds since the renderer started, goes back to 0 once done, and fires at once if set later than that. Frame statistics are also logged every minute.
- **The renderer starts inside the `LLViewerWindow` constructor:** that constructor loads the UI images, so the device has to exist before it goes on. Started after it, those images were dropped and drew white.
- **Recorded buffers:** `flush_vbo()` keeps vertices in the mapped copy in this mode, because no GL buffer exists, and that is what `LLVertexBufferData` replays from.
- **Startup:** about 10 s passed between the probe and the first frame, because `setShaders()` read every GL shader the new renderer does not use. `createShader()` and `loadShaderFile()` now return at once in this mode.
- **Empty stubs run first:** with the table in place, the viewer runs its normal code with no GL context and draws nothing of its own. Its log then lists every GL function really reached, which replaces the P1 text search as the input for P2c. That list decides how much of the seam can live in the table and how much needs early-outs.
- **Settings protected from the fake GL:** the new renderer keeps the saved `LastGPUString`, because a different string would reset the user's graphics settings at each switch of renderer. It also skips the GPU benchmark and turns off the shader binary cache, so OpenGL mode's cache stays untouched.
- **Viewer build cost:** only `asrviewer.cpp` gets the `AS_RENDERER` define, and `AS_RENDERER` is on in `build_nolto.cmd` only. Turning it on the first time recompiled all of `newview` (12:31): `$<LINK_ONLY:asrenderer>` does not keep the renderer's include folder out of a directly linked target, so the folder reached the whole project and forced a new precompiled header. The extra folder is harmless, and removing it would cost that rebuild again, so it stays. Later one-file rebuilds are expected to return to about 30 s.
- **Window thread:** the viewer already handles Windows messages on its own window thread, so a stalled main thread does not make Windows report the window as not responding. This is from reading the code; the viewer stall test in P2d confirms it.
- **Mac and Linux viewer:** they stay on OpenGL for now, and asking for `Native` there falls back with a message. macOS links GL directly, so it still needs the `llglheaders.h` remap of section 1.
