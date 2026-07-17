# Exact OIT C++ source extraction plan

## Goal

Move the custom Exact OIT C++ implementation into Firestorm-specific source
files:

- `indra/newview/fsexactoit.cpp`
- `indra/newview/fsexactoit.h`

The purpose is to make future upstream merges easier. Existing viewer files
should contain only the smallest integration hooks necessary to enter the
Firestorm-specific implementation.

This refactor must not change rendering behavior, Exact OIT quality, fallback
behavior, settings, diagnostics, resource limits, or the appearance of the
vanilla renderer when Exact OIT is disabled.

## Intended interface

`FSExactOIT` will be the single owner and entry point for Exact OIT state and
operations. The header should expose only methods needed by existing viewer
integration points.

The initial public interface should cover these responsibilities:

- determine whether Exact OIT is enabled and usable;
- allocate and release viewport-dependent resources;
- optionally retain or release the large node buffer during viewport changes;
- begin a frame and reset capture/fallback state;
- prepare Exact OIT alpha shaders for a post-deferred pass;
- begin and end Exact OIT capture;
- report whether GLTF and ordinary alpha rendering are currently being
  captured;
- choose Exact OIT shader variants for alpha draws;
- upload captured blend factors and glow state;
- validate capture and either composite it or request the complete vanilla
  fallback;
- load, unload, register, and validate the Exact OIT shader family;
- append Exact OIT diagnostics to the viewer debug report.

Prefer methods and a short RAII capture scope over exposing writable global
flags. Existing code should query `FSExactOIT` instead of reading or changing
Exact OIT state directly.

## State and resource ownership

Move the following state out of `LLDrawPoolAlpha`:

- capture completed;
- capture clear required;
- vanilla fallback active;
- capture currently active;
- the per-pool forwarding/capture flag, if it can be represented by the central
  capture scope without changing nested rendering behavior.

Move Exact OIT GPU resources and statistics out of
`LLPipeline::RenderTargetPack` where practical:

- saved opaque render target;
- head-pointer texture;
- per-pixel count texture;
- head/count framebuffer;
- node SSBO;
- control SSBO;
- node capacity;
- peak node count;
- overflow count;
- availability state.

These resources should be members of `FSExactOIT`, with allocation and release
called by the pipeline at the same lifecycle points used today. The large node
buffer must continue to survive viewport-only resizing and must be fully
released on shutdown, disabling Exact OIT, allocation failure, or other
non-viewport teardown.

If moving the saved opaque `LLRenderTarget` out of `RenderTargetPack` creates an
initialization-order problem, it may remain there temporarily behind a narrow
`FSExactOIT` accessor. All lifecycle and compositing logic must still move to
`fsexactoit.cpp`.

## Implementation extraction

### Capture and alpha rendering

Move the following implementation from `lldrawpoolalpha.cpp` into
`fsexactoit.cpp`:

- Exact OIT shader-set validation and the one-time missing-shader diagnostic;
- decision logic for entering capture;
- head/count/control clearing and image/SSBO binding;
- capture frame state;
- capture-scope state transitions;
- packed per-draw blend-factor upload;
- Exact OIT emissive and glow uniform upload;
- selection helpers for deferred alpha, fullbright, material, PBR, emissive,
  and rigged variants.

`LLDrawPoolAlpha` must retain its normal traversal and draw loops. Its Exact OIT
changes should reduce to calls such as:

- ask whether this pass should be captured;
- enter an `FSExactOIT` capture scope around the existing rigged and non-rigged
  draws;
- ask for the appropriate shader instead of embedding selection rules;
- ask `FSExactOIT` to configure a captured draw instead of packing uniforms
  locally;
- skip framebuffer blending and depth writes only when the centralized capture
  state says capture is active.

The original disabled-path expressions should remain structurally recognizable
to simplify upstream conflict resolution.

### GLTF integration

Move GLTF-specific Exact OIT decisions into helpers in `fsexactoit.cpp`:

- choose the ordinary or Exact OIT GLTF program;
- select the correct program variant;
- upload the standard captured blend tuple.

`gltfscenemanager.cpp` should retain only small queries and helper calls at the
points where it already binds programs and submits alpha draws. Its ordinary
GLTF path must remain unchanged when capture is inactive.

### Resource lifecycle and compositing

Move the following implementation from `pipeline.cpp` into
`fsexactoit.cpp`:

- viewport resource creation;
- framebuffer and integer texture setup;
- node/control buffer creation;
- VRAM-bounded initial capacity calculation;
- retained node-pool handling;
- complete resource release;
- per-frame capture-state reset;
- memory barriers and mandatory control-buffer readback;
- camera-transition diagnostics;
- overflow detection;
- geometric node-buffer growth;
- saved opaque-color blit;
- natural-sort fullscreen passes;
- final Exact OIT blend;
- composite shader bindings and debug-mode upload.

The pipeline should retain thin calls at four integration points:

1. allocate Exact OIT resources after the main screen target exists;
2. release resources along with screen buffers, passing whether the node pool
   may be retained;
3. reset Exact OIT state before post-deferred transparency;
4. validate and composite after post-deferred transparency.

The vanilla fallback must still be executed through the existing alpha pool
rendering interfaces. To avoid transferring ownership of pipeline internals,
`FSExactOIT` may return a result such as `COMPOSITED`, `FALLBACK_REQUIRED`, or
`INACTIVE`; the pipeline will perform the existing vanilla rerender when
`FALLBACK_REQUIRED` is returned. Buffer growth and Exact OIT session disabling
remain internal to `FSExactOIT`.

### Shader management

Move definitions and lifecycle code for the Exact OIT shader programs from
`llviewershadermgr.cpp` into `fsexactoit.cpp`:

- composite program;
- alpha and skinned-alpha capture programs;
- PBR and skinned-PBR capture programs;
- fullbright and skinned-fullbright capture programs;
- material capture array;
- emissive and PBR-glow capture programs and rigged variants;
- Exact OIT GLTF program and variants;
- program registration, creation, unloading, and validation;
- the explicit Exact OIT shader-cache revision salt.

`LLViewerShaderMgr` should call `FSExactOIT` helpers from its existing load,
unload, and shader-list construction stages. Generic shader-manager changes
needed to select GLSL 4.30 for Exact OIT shader files may remain in
`llshadermgr.cpp`, since that code controls a lower-level compilation mechanism
and cannot safely be moved into the viewer module.

Existing shader globals should be replaced by `FSExactOIT` accessors where
possible. If shader-manager APIs require stable global objects, keep their
declarations in `fsexactoit.h` but define and manage them exclusively in
`fsexactoit.cpp`.

### Diagnostics

Move crash-report Exact OIT field construction from `llappviewer.cpp` into a
helper that appends the existing fields to the supplied `LLSD` object.

The call site in `llappviewer.cpp` should be one tagged call. Field names and
values must remain unchanged, including availability, capacity, peak demand,
overflow count, memory estimate, and unavailable-reason text.

## Unavoidable changes outside the new files

The extraction cannot make Exact OIT completely invisible to upstream-owned
files. The following minimal changes will remain:

- `indra/newview/CMakeLists.txt`: list `fsexactoit.cpp` and `fsexactoit.h`;
- `pipeline.cpp`: allocation, release, frame reset, and composite/fallback
  hooks;
- `lldrawpoolalpha.cpp`: capture scope, shader-selection, draw-configuration,
  depth/blend, and debug-alpha hooks;
- `gltfscenemanager.cpp`: GLTF program-selection and captured-blend hooks;
- `llviewershadermgr.cpp`: shader registration/load/unload hooks;
- `llappviewer.cpp`: one diagnostics hook;
- relevant headers: only declarations or signature changes that cannot be
  hidden behind `fsexactoit.h`;
- `llshadermgr.cpp`: the GLSL 4.30 Exact OIT compilation exception.

Shader files, settings XML, preferences XML, and documentation naturally remain
separate from the C++ module.

Every retained custom source hook and all new code in `fsexactoit.cpp` and
`fsexactoit.h` must use `<AS:Chanayane>` ownership tags. Markdown files must not
contain ownership-tag comments.

## Refactoring sequence

1. Add `fsexactoit.cpp` and `fsexactoit.h` to the viewer build without changing
   behavior.
2. Move shader object definitions and shader lifecycle helpers first, then
   compile.
3. Move resource ownership, allocation, release, and diagnostics, then compile.
4. Move frame state, capture setup, and alpha/GLTF shader-selection helpers,
   then compile.
5. Move validation, overflow growth, and compositing, then compile.
6. Reduce the old source blocks to thin hooks and remove obsolete Exact OIT
   fields and globals.
7. Compare the resulting diff against `special-ayanestorm-dev` to confirm that
   large custom blocks are concentrated in the new files.

Use mechanical moves wherever possible. Do not combine this extraction with
the planned opaque-cutoff optimization, shader behavior changes, renaming of
legacy shader globals, or unrelated cleanup.

## Verification

### Build and static checks

- Configure and build the same Windows Release configuration currently used for
  AyaneStormSpecial.
- Confirm `fsexactoit.cpp` is compiled and linked exactly once.
- Check for stale Exact OIT globals and direct state mutations outside the new
  module.
- Check the final branch diff for whitespace errors and missing ownership tags.
- Confirm no shader-cache revision bump is needed because this refactor does
  not change shader sources or permutations.

### Rendering behavior

Test with Exact OIT disabled and enabled:

- ordinary alpha, rigged alpha, materials, PBR, GLTF, particles, emissive, and
  glow;
- first-person and third-person camera movement;
- viewport resize and UI scale changes;
- shader reload;
- node overflow and complete same-frame vanilla fallback;
- shutdown and restart;
- debug modes and crash-report diagnostics;
- HUD, impostor, cube snapshot, water-adjacent, and depth-of-field exclusions.

Compare performance counters, output, node capacity, peak demand, and overflow
behavior with the pre-refactor build. This is a source-organization change
only; any rendering or performance difference is a regression.

## Completion criteria

- Nearly all Exact OIT C++ implementation resides in `fsexactoit.cpp`.
- `fsexactoit.h` exposes a narrow interface without writable public state.
- Existing viewer files contain only necessary, short integration hooks.
- The final diff against `special-ayanestorm-dev` is substantially easier to
  review and merge.
- Exact OIT output and performance remain unchanged.
- The vanilla-disabled path remains visually and behaviorally unchanged.
