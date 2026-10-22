# AyaneStorm Optional Vulkan Renderer: Feasibility (Slipstream Viewer study)

Source studied: Slipstream Viewer checkout in `.sm/` (HEAD 2026-10-16, branch `vulkan-port-tb`).
Goal: an **optional** Vulkan renderer in AyaneStorm, OpenGL renderer kept as default.

**Decision 2026-10-17: deferred.** Slipstream is still in very active development; revisit once it stabilizes (commit rate drops, tagged releases, no more large refactors). Re-check the facts below against their tree at that time.

## 1. What Slipstream did

- Base: Alchemy (LL viewer + SDL3 windowing on all platforms). Linux-first; Windows is cross-compiled with clang-cl + xwin.
- Renderer rewritten as a **GPU-driven Vulkan 1.3 renderer**. OpenGL fully removed (no fallback).
- Claimed: busy scene 24 FPS (GL viewer) -> 90 FPS (RTX 5090). Indicative only.

### Architecture ("clean pipeline")

- **Persistent GPU scene**: every face is an instance in a persistent instance SSBO (transform, material, bounds, flags) + one shared vertex/index "mega buffer". Uploaded on change, not per frame.
- **Ingest** (`newview/llcleaningest.cpp`, `llcleanmeshasset.cpp`, `llcleaninstancestore.cpp`, `llcleanmaterialstore.cpp`, `llcleanskinstore.cpp`, ...): reads `LLViewerObject` / `LLDrawable` / `LLVolumeFace` directly and fills CPU stores. Change tracking via a few hooks: `noteObjectMoved` (`lldrawable.cpp`, `llviewerobject.cpp`), `noteObjectChanged` (`llvoavatar.cpp`), `noteTerrainDirty` (`llvosurfacepatch.cpp`), `shiftOrigin` (`llviewerobjectlist.cpp`), plus `vkIngestTick` per frame from `llappviewer.cpp`. Terrain mesh is generated from the heightfield directly (no `LLVOSurfacePatch::updateGeometry`).
- **Bindless**: one big descriptor array of all resident textures; materials index into it. No per-draw binds.
- **Compute cull + multi-draw indirect**: compute pass does frustum/HiZ cull, LOD select, bucket sort (opaque, mask, blend, rigged...), writes indirect commands; world drawn with a few `vkCmdDrawIndexedIndirectCount`.
- **Depth pre-pass** then lit pass with depth-equal. Deferred lighting on async compute queue. Clustered local lights.
- **GPU skinning** (pre-skin compute), GPU avatar animation, GPU flexi, GPU particles.
- Mesh-shader cluster path (`clean_cluster_draw.task/.mesh`, `llcleancluster*vk.cpp`), cluster LOD DAG.
- Extras: HW ray-traced shadows (`VK_KHR_ray_query`, optional), SSR, probes, GTAO/SSAO, DoF, TAA/FSR3, motion blur, god rays, ReShade-FX runtime (`third_party/reshadefx`), VMA allocator, VRAM budget, auto-tuner (`llvkautotune.cpp`).
- **UI**: kept `LLRender` (gGL) API; added `llrender/llrenderbackend.h`, a pure-virtual seam (begin/end/vertex/matrix/blend/scissor/texture). `vkgfx/llrendervkbackend.cpp` implements it on `LLRenderVK`. Fonts/UI images route through it.
- **Textures**: `LLImageGL` kept as CPU container; a VK branch keeps a CPU copy (`mVKCpuData`) used as upload/re-residency source.

### Where the speedup comes from

Mostly the **GPU-driven architecture** (no per-frame CPU scene walk, no `rebuildGeom`/`genDrawInfo`/draw pools, no per-face state binds), not Vulkan by itself. Vulkan adds: clean bindless, low driver overhead, async compute, ray query, mesh shaders, explicit sync.

Note: they first built the same GPU-driven design **on OpenGL** (2026-06-15: bindless textures, mega-VBO, compute cull, MDI). It hit a months-long intermittent geometry-loss bug (#29, GL driver state / indirect drawcount issues), which led to the Vulkan move on 2026-08-11.

### Size and pace

| Item | Size |
|---|---|
| `newview/vkgfx/` handwritten C++ | ~99k lines (`llcleanrenderervk.cpp` alone 2.4 MB) |
| `newview/llclean*` + `llvk*` | ~27k lines |
| `vkclean/shaders/` (GLSL -> SPIR-V) | ~41k lines, 170 files |
| Generated SPIR-V headers | `vk_world_shaders_spv.h` 40 MB, `vk_fsr3_shaders_spv.h` 1.6 MB |
| `pipeline.cpp` | ~12k -> 2k lines after GL removal |

Timeline: GL GPU-driven 2026-06; first VK frame 2026-08-11 (behind `RenderUseVulkan`, GL default); Vulkan made mandatory 2026-09-17 (commit `94ab64c2fb`); GL deleted 2026-09-27 -> 2026-10-10. ~2,400 commits in Oct alone, Claude-agent driven. Not a stable upstream to track.

### How VK mode bypassed the GL viewer (dual-mode era, Aug-Sep)

- Window: SDL3 forbids a window being both GL and Vulkan, so VK mode created an `SDL_WINDOW_VULKAN` window and no GL context (`gWindowUseVulkan`). Switching requires restart.
- `LLAppViewer::initWindow()` diverted **before** `gPipeline.init()`; `display()` returned into `displayVulkanUI()`. So `updateCull`, `stateSort`, `updateGeom`, draw pools, `LLViewerShaderMgr` never ran.
- Many crash fixes for unguarded GL reached under VK (GL PFNs null). Their own map (`research/gl_removal_map.md` in history, commit `32a0165f72`) lists side effects of skipping `gPipeline`: undrained `LLGLUpdate::sGLQ`, `mGroupQ1`, `mPartitionQ`, `mMovedBridge` (picking staleness), reflection-probe leak, terrain dirty-patch churn, dead dynamic textures (previews), frozen flexis / no trees+grass at that time. Plus latent crashes (`LLRenderSphere`, GLTF scene manager, `glReadPixels` debug, terrain paint map, fast timer export, bake aux masks).

## 2. Feasibility for AyaneStorm (optional backend, GL kept)

### Positives

- License compatible: Slipstream LGPL 2.1 (same as FS/LL). VMA MIT, FidelityFX MIT, ReShade FX BSD-3. Keep headers and credits.
- Their dual-mode period proves the coexistence pattern: divert at `initWindow` + `display`, UI via `LLRenderBackend` seam, ingest hooks are few.
- Renderer core is self-contained in `vkgfx/` + `llclean*`; can live as new modules (fits AGENTS.md "substantial functionality in new modules").

### Blockers / costs

1. **Windowing**: AyaneStorm Windows uses `LLWindowWin32` + WGL, not SDL. Need a VK surface from the HWND (`VK_KHR_win32_surface`) and a path that skips WGL context creation when VK is selected. Restart to switch (`ASRenderUseVulkan`).
2. **GL coexistence must be re-added**: Slipstream HEAD has GL deleted and assumes VK everywhere (llrender gutted: `llgl.cpp` 2.6 KB, no `LLVertexBuffer`, no `LLGLSLShader`, `LLRender` always forwards to backend). Importing means making every VK change conditional, and guarding every GL path reachable in VK mode.
3. **AyaneStorm GL features absent in VK mode**: 80 `as*.glsl` shaders and ~46 `as*.cpp` modules (DoF suite, exact OIT/AVBOIT/macOIT, VBAO/GTAO, volumetric lighting/clouds, weather/snow, lens flare, color grading/LUT, sun/moon/stars/aurora, light rigs, motion blur, vignette...). All would need VK reimplementation; Slipstream has its own equivalents for some (DoF, GTAO/SSAO, god rays, motion blur, TAA, tone mapping).
4. **Integration edits in FS/LL files**: Slipstream touches `llvovolume.cpp` (~105 refs), `llappviewer.cpp` (~91), `llviewerwindow.cpp`, `llvoavatar.cpp`, `llviewerpartsim.cpp`, `llrender`, `llimagegl`, etc. Each needs AS ownership tags; merge burden with LL/FS upstream grows.
5. **Toolchain**: built with clang-cl; MSVC compile issues expected. 40 MB SPIR-V header is heavy for MSVC; prefer shipping `.spv` files or a build-time `glslc` step.
6. **Snapshot only**: Slipstream churn (~60+ commits/day) makes ongoing merges impossible; import once, then own it.
7. **Snapshots**: VK path must support high-res snapshots before AS features move over (see project rule: DoF works in snapshots first).

### Effort

Large. Slipstream: ~2 months from first VK frame to GL removal, ~5 months total, agent-driven, full-time. For AyaneStorm, even reusing their code: months.

## 3. Recommended plan (if pursued)

- **P0 Decide import base**: take Slipstream HEAD `vkgfx/` + `llclean*` + `vkclean/shaders` (most mature), not the 2026-09-17 dual-mode snapshot (immature). Re-add GL coexistence ourselves.
- **P1 Bring-up**: `ASRenderUseVulkan` (restart required). Win32 VK surface, no WGL context in VK mode. Add an `LLRenderBackend` seam in `llrender` with GL as default implementation (tagged). VK login screen/UI.
- **P2 World**: divert `initWindow`/`display` in VK mode; ingest hooks (tagged); world renderer; picking (`llcleancursorpick`); handle the undrained-queue side effects listed above.
- **P3 Parity**: snapshots, previews (dynamic textures), HUDs, selection, beacons, media textures, EEP sky/water.
- **P4 AS features on VK**: DoF first (snapshot first), then OIT, AO, volumetrics, post stack.

Cheaper alternative: none really equivalent. A GL GPU-driven path (Slipstream June attempt) was abandoned by them due to GL driver-state bugs.
