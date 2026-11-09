# New renderer, step e viewer side: handoff (9 November)

Read this first when resuming. The 9 November work below is committed in checkpoint `dc6ba427b5` (10 November). Later changes are listed at the end.

## Done in the checkpoint
- **Viewer, `indra/newview/asrworld.cpp`:**
  - Blended faces go to Mac OIT (`INSTANCE_BLENDED`) through `setBlended()`.
  - Each avatar has one skeleton (`AvatarSkeleton`), posed in `updateAvatars()` only when it changes, plus one repeat so the GPU stops leading.
  - Rigged mesh (worn or animesh) is skinned through `SharedSkin`, one per mesh ID and hash.
  - Attachments are bound to their attachment point's joint (`attachedMatrix()`).
  - Mesh shapes carry skin weights (`ShapeFace::skinJoints`, `unweighted`).
  - HUD attachment points stand at the root, so they don't inflate the box.
  - A rigged object waits while its skin info is still to come ("skin to come").
- **Debug setting `ASRendererAvatarSkip`** (U32, not persisted), added up: 1 no rigged mesh, 2 no attachments, 4 blended drawn as cut-outs.
- **Vulkan library changes:**
  - `drawIndirectFirstInstance` is now required and enabled. GPU-AV flagged it.
  - Validation turns on GPU-assisted checks. `DeviceOptions::report` sends messages to the viewer log.
  - `VK_EXT_device_fault`: a lost device logs the faulting address and names the buffer (`describeDeviceFault`, `mLiveBuffers`).
- **Per-binding skinned boxes,** the fix for the squares and pixelated face:
  - Positions pack in the skeleton's joint cube (`GpuSkeleton.boxScale`, joints only) widened by the binding's own `GpuBinding.reach` times `GpuSkeleton.stretch`. See `bindingBox()` in `asrSceneSkinC.glsl`.
  - Reach comes from `GeometryData::jointSpheres`: for each joint, a sphere around the vertices really weighted to it, built in `buildGeometry`.
  - The Frobenius bound now includes the lead.
- **Test, `asrtestscene.h`:**
  - Creatures arrive gradually, 8 at first.
  - Blended creatures are double-sided, and their attached boxes are blended.

## Status
- **Skinning works in the viewer:** the head, hair, dress and shoes are placed correctly, as on the user's screenshot.
- **The user's avatar:** 355 skins, 361 rigged objects, 18 attachments.
- **10 November update:** the user confirmed that the helper/viewport guard stopped the crash. Mesh body textures now appear; the earlier claim that mesh BOM could not work was incorrect. Body seams and a missing dress section are the current visual issues; see the latest section.

## Open
1. **Crash: VkResult -4 (device lost), with transparency only: resolved in the user's 10 November run after the helper/viewport guard.** Earlier investigation notes follow:
   - With `ASRendererAvatarSkip 4` there's no crash.
   - Earlier diagnostics discarded partial device-fault information on `VK_INCOMPLETE`. Neither a TDR nor a page fault is established. The latest checkpoint evidence is recorded below.
   - It comes about 2 to 10 s after a second skeleton appears. Both crashed runs show the same counts: 2 skeletons, 356 skins, 18 attached.
   - Earlier handoff reported as ruled out (pixel-access safety is now reopened by the GPU read fault):
     - Mac OIT state size and pixel indexing.
     - The resolve.
     - Texture-feedback bounds.
     - Shader loops.
     - Palette sharing.
     - Table growth.
     - Full lists (no drops logged).
   - Not yet checked:
     - The second skeleton was identified as another avatar; a later crash with only self shows that a second skeleton is not required.
     - Mac OIT draws with huge indirect counts.
     - Do not change Windows TdrDelay or GPU drivers: the user ruled out system changes.
2. **Test stall check fails:** "render rate holds during the stall" drops to about 350 to 510 fps (normally 1,100 to 1,400) since the blended double-sided creatures and attachments were added. It may be linked to item 1.
3. **Test audit at 2560x990:** "scene staging buffer does not fit in its memory budget". At the default size it passes, with 0 pixels lost.
4. **Mesh BOM correction, 10 November:** the existing viewer resolves `IMG_USE_BAKED_*` before `getTEImage()` returns the texture used by `asrworld.cpp`. The user now sees the mesh body. Native system-avatar geometry/morphs remain separate unfinished work.
5. **Not verified by eye:** whether per-binding boxes fixed the pixelated face. The user should build and run with `--set ASRendererBackend Native --set ASRendererAvatarSkip 4`.
6. **Next:**
   - The system body: morphs, baked textures, alpha masks.
   - Mouselook head hiding.
   - Flexible prims.
   - Mac (Metal) testing of all of the above.
7. **The user's run with per-binding boxes and `ASRendererAvatarSkip 4`:** the face is sharp now, so that fix worked. Three new findings:
   - **The right eye is missing.** Suspects, none checked:
     - Bake on Mesh eyes (`IMG_USE_BAKED_EYES`).
     - A blended eye layer cut out at 0.5 because of skip 4.
     - The eye-joint number or skin joints.
   - **Hand and finger edges are ragged in a still image.** Likely skip 4 drawing alpha-edged textures as cut-outs at 0.5. Check again once transparency works.
   - **Hair and hands wobble while the avatar moves.** Suspect: the GPU lead extrapolates joint matrices linearly (`predict()` in `asrSceneSkinC.glsl`), which shears rotations, so the farther from the joint, the larger the wobble. Fix idea: interpolate rotation and translation properly (quaternion or polar decomposition), or lead only translation. Also check whether poses arrive at an uneven rate.
8. **The status doc's step e row** was updated on 10 November from this handoff; older narrative entries remain historical.

## Transparency investigation, 10 November

### Vulkan feature enablement and Mac OIT atomics

First fixes written, not built or runtime-tested:

- `asrdevicevulkan.cpp` now requires and enables `fragmentStoresAndAtomics` for Mac OIT's fragment storage writes and atomics, and `shaderStorageImageExtendedFormats` for the resolve's RG32F weights image. Both were previously left disabled. The resolve's generated SPIR-V explicitly declares `StorageImageExtendedFormats` (capability 49).
- `asrSceneMacOIT.glsl` now sets the shared tail-seen flag with `atomicOr`. Multiple pixels could previously write the same flag non-atomically.
- `CMakeLists.txt` now tracks `asrSceneMacOIT.glsl` as a dependency of its three fragment programs; previously editing this include alone did not trigger their recompilation.
- `asrworld.cpp` logs each skeleton's creation with its GPU ID, avatar UUID, self/avatar/animesh kind, joint count and originating object UUID. This identifies the second skeleton in the next run.

These are concrete defects, **not a proven explanation of the device loss**. The user reconfirmed that `ASRendererAvatarSkip 4` avoids the crash. That setting converts blended surfaces to cutouts, leaving no blended draws for Mac OIT; its other passes still run. No Windows TDR settings were changed.

### Validation and next runtime check

- Khronos glslang 16.6 validated the three fragment entry points and the resolve with the project's native target: GLSL 4.60, `-V --target-env vulkan1.3`. No errors. SPIR-V inspection confirms the keys shader now contains `OpAtomicOr`.
- The requested scratch-copy GLSL 4.10 check was attempted. It rejects existing native-only `std430` and `early_fragment_tests` constructs; these shaders have their own `#version 460` and compile to SPIR-V, as the renderer plan's section 3 specifies. No OpenGL shader was changed.
- No project build or runtime test was performed. The available viewer log is the successful skip-4 run, not a crashing run.
- Next user test: one build, then Native with `ASRendererAvatarSkip 0`, at the same location/camera. Check whether device loss remains and identify the second skeleton from `created skeleton` log lines. If it remains, inspect blended draw workload and synchronization next; absence of GPU-AV or device-fault details does not prove a timeout.

### Specification references

- [Vulkan physical-device features](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html): supported features must be enabled at device creation; fragment storage stores/atomics and extended storage-image formats have separate feature bits.
- [Vulkan SPIR-V requirements](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html#VUID-RuntimeSpirv-NonWritable-06340): fragment storage writes require `fragmentStoresAndAtomics`; the `StorageImageExtendedFormats` capability requires `shaderStorageImageExtendedFormats`.

## Device loss reproduced after the first fixes, 10 November

### Crash log evidence

The user built and ran Native with `ASRendererAvatarSkip 0`. The log confirms the new skeleton logging is present. Both skeletons were created at `2026-11-09T23:12:07Z`: skeleton 1 is self, skeleton 2 is another **avatar**, not animesh; both have 214 joints. At `23:12:14Z` the renderer stopped with `wait for frame failed: VkResult -4 (no fault details)`. The screenshot shows that same error and the fallback message. The first patch did **not** resolve the crash.

The run ends before the next periodic GPU profile. Its only GPU profile is the initial empty scene, so it provides no evidence about the failing frame's workload or pass. `no fault details` is also used when the device-fault query fails; it does not establish a TDR. The crashing log was preserved in `/tmp/asr-avatar-validation/crash-after-feature-fixes.log` for this session.

### Visual Studio GPU debugging, Nsight Graphics and Aftermath

The user proposed GPU debugging rather than more speculative changes. Researched tools:

- Microsoft's [Debugging GPU Code](https://learn.microsoft.com/en-us/visualstudio/debugger/debugging-gpu-code?view=visualstudio) describes the C++ AMP `GPU Only` workflow. The renderer's GPU programs are GLSL compiled to Vulkan SPIR-V. A CPU debugger can inspect C++ submission/state, but breaking at the failed fence wait identifies where loss was detected, not necessarily the GPU operation that caused it.
- NVIDIA's [Nsight Graphics Shader Debugger](https://docs.nvidia.com/nsight-graphics/UserGuide/shader-debugger-overview.html) supports live Vulkan shader breakpoints, stepping, variables and warp selection on Ampere or newer hardware. The RTX 3080 Ti is eligible. [Setup requirements](https://docs.nvidia.com/nsight-graphics/UserGuide/shader-debugger-setup.html): two local GPUs, with a separate GPU for the desktop/debugger, or two computers; source stepping also needs shader debug information. The current shader CMake commands do not request debug information. The viewer log lists only the 3080 Ti, so a second usable GPU is unconfirmed.
- For the current device-loss failure, [Nsight Aftermath Crash Dump Monitor](https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-crash-dump-setting-up.html) can collect a dump from the existing Vulkan executable without integrating its SDK or rebuilding the viewer. Enable collection for this executable, with shader debug information, resource tracking and shader error reporting, then reproduce and inspect the dump in Nsight Graphics. Driver shader debug information and source-level SPIR-V debug information are separate. This route does not require live breakpoints or a second GPU.
- Standard install directories contain Nsight Visual Studio Edition, Compute and Systems, but no Nsight Graphics or Aftermath Monitor was found there. These products have different capabilities; their presence does not establish that the Vulkan Shader Debugger is installed.
- The user has a Debian 13 NAS with a GT 1030. The Ampere requirement applies to the GPU executing the debugged shaders (the Windows RTX 3080 Ti), so the NAS GPU's architecture alone does not rule it out as a debugger host. However, a Debian-host-to-Windows-target session is not confirmed by the documentation checked; do not promise that setup works. The current [connection guide](https://docs.nvidia.com/nsight-graphics/UserGuide/launch-application-overview.html) describes Windows targets using Remote Monitor and Linux targets using SSH, without clearly confirming this cross-OS pairing. Aftermath capture on Windows avoids that dependency.
- Optional in-code fallback researched, not implemented: [NV device diagnostic checkpoints](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_device_diagnostic_checkpoints.html), queried only after device loss, can name markers crossing supported GPU stages even when EXT device-fault details are unavailable. They narrow the commands involved, not necessarily the exact faulting instruction.

No further renderer source changes or builds were made during this follow-up. Pending: choose the available debugging setup and collect GPU evidence before the next proposed fix.

## Existing-driver GPU diagnostics, 10 November

The user does not want to change GPU drivers. Do not make driver, TDR or system-setting changes to obtain external-tool compatibility.

Diagnostics added, not built or runtime-tested:

- New `indra/asrenderer/asrdiagnosticsvulkan.h` keeps device-loss evidence separate from the backend. `asrdevicevulkan.cpp` optionally enables `VK_NV_device_diagnostic_checkpoints` if the installed driver exposes it. Startup logs whether checkpoint recording is available; unsupported drivers still run.
- Checkpoints bracket texture commands, each frame-graph pass, and composite/UI. Opaque marker values encode frame number, stable pass ID and before/after. No marker points into reused frame-slot memory. Recording uses fixed CPU storage, no allocations, added GPU readback or synchronization.
- On `VK_ERROR_DEVICE_LOST` only, checkpoint queries report the latest markers at the driver's supported pipeline stages. They narrow the command range; they do not prove which shader instruction faulted. Detailed evidence goes to the log, not the error dialog.
- The existing completed-frame readback is preserved with its matching view. On loss, the log includes blended early/late counts, list capacities, submitted vertices per pass, tail flag and dropped entries. Inconsistent indirect arguments or tail-gated copies are marked `INVALID DRAW`. This describes the last completed scene, **not** necessarily the frame that failed.
- Device-fault query failures now include the failing query and its VkResult instead of only `no fault details`.
- `asrviewer.cpp` labels backend report messages `renderer:` because the shared callback serves Vulkan and Metal and now includes diagnostics as well as validation errors.

No shaders, rendering settings, drivers or Windows settings were changed by this diagnostic addition. No build was attempted. On the next user build/run with Native and `ASRendererAvatarSkip 0`, the useful log lines are `GPU diagnostic checkpoints enabled/unavailable`, `GPU checkpoints`, `last completed scene frame`, and any `INVALID DRAW`. Keep this development instrumentation until the transparency fault is resolved, then review its removal.

## Vulkan/Metal scope audit, 10 November

The user caught a backend-specific `Vulkan:` label in the shared viewer report callback. Corrected to `renderer:`. Audited all changes made during this session for the same mistake:

- Vulkan feature enablement and checkpoints stay in `asrdevicevulkan.cpp` and its new diagnostic header. Only that backend includes the header; CMake compiles that backend and exposes Vulkan include directories under `WIN32`, while `APPLE` selects `asrdevicemetal.mm`.
- The shared shader change is the tail flag's `atomicOr`; it intentionally applies to both backends. The installed Vulkan SDK SPIRV-Cross translated all three validated fragment programs to MSL 3.0 successfully with argument buffers and discrete set 1. The keys output contains `atomic_fetch_or_explicit` on a `device atomic_uint` with relaxed ordering. The repository's vendored MSL translator also explicitly handles `OpAtomicOr` through its atomic-operation emitter. This checks translation support, not Apple's compiler or the project's complete binding remapping.
- The shader include dependency is attached to the common SPIR-V output; the existing Apple dependency chain continues from SPIR-V to MSL to metallib. No Apple build branch was removed or redirected.
- Other shared viewer changes are the neutral report prefix and skeleton-identity logging. No shared GPU struct, public backend interface, or Metal backend implementation was changed in this session.

No further Vulkan/Metal scope error was found. No Mac build, Apple Metal compilation, or Mac runtime test was performed; Metal correctness remains unverified at those stages.

## Checkpoints around the tail pass; close-up zoom, 10 November

### Latest crash evidence

The user reproduced the crash while zooming in on their avatar. This is a useful reproduction trigger, not proof that zoom itself is faulty. A close-up can increase transparent fragment work without a proportional change in submitted triangles.

The checkpoint-enabled run (`2026-11-09T23:32:33Z`) reported:

- Top-of-pipe: frame 947548, **before `macoit color`**.
- Bottom-of-pipe: frame 947548, **before `macoit tail`**.
- Last completed scene: frame 947547, 2,697 blended clusters, 378,342 submitted vertices per transparency pass, tail present, 0 dropped entries, no `INVALID DRAW`.
- Nonempty blended lists (early/late, capacity): 16 = 421/0 of 16,384; 17 = 243/0 of 1,024; 18 = 212/0 of 1,024; 19 = 1,821/0 of 4,096.
- Only skeleton 1 (self, 214 joints) was created in this run. Another avatar is **not necessary** to trigger the loss.

Inference: the tail pass or the transition leading into color is the current area to investigate. Checkpoints describe pipeline progress, not an exact faulting instruction. Counts are from the previous completed frame and do not establish the failing frame's counts or its pixel overdraw.

### Partial device-fault information was being discarded

The dialog reported `fault info query failed: VkResult 5`. `5` is `VK_INCOMPLETE`, a success status indicating that only part of the requested information was returned. The code intentionally supplies no vendor binary buffer (`vendorBinarySize = 0`), which can produce this result when binary data exists, while the description and address/vendor arrays are still returned.

Fixed `describeDeviceFault()` to retain `VK_INCOMPLETE` results, bound the arrays to their actual returned counts, and report how many available binary bytes were omitted. Only Vulkan C++ diagnostic code changed; no shader, Metal implementation, driver or Windows-setting change. This correction is not built or runtime-tested and does not itself fix device loss. The next reproduction should expose any available textual fault description and addresses that the old path discarded.

Reference: [vkGetDeviceFaultInfoEXT](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceFaultInfoEXT.html), particularly output-count semantics and `VK_INCOMPLETE` when capacities are insufficient.

The log is preserved for this session at `/tmp/asr-avatar-validation/crash-with-checkpoints.log`. Windows' protected LiveKernelReports directory could not be read, including outside the sandbox; no permissions were changed.

## Alt-click reproduction and Mac OIT helper-read guard, 10 November

### Exact trigger and newly visible fault

The user clarified: the viewer stayed stable, mousewheel zoom did not crash it, and **Alt-click/Alt-click-zoom on the avatar triggered the crash immediately**. Use that exact operation for the next runtime check; ordinary wheel zoom is not an adequate reproduction.

The latest log, preserved at `/tmp/asr-avatar-validation/crash-alt-click.log`, now exposes partial fault data:

- Instruction-pointer fault at `0x200034a20`.
- Invalid GPU read at `0x306660000`, reported by the buffer tracker as 3,712 bytes past the nearest tracked `Mac OIT state` allocation. This labels the nearest allocation; it is not by itself a shader-variable attribution. Driver address precision was not included in that run.
- 42,500 bytes of vendor binary fault data were available but deliberately not requested; this explains the partial-query result.
- Checkpoints again span the tail pass: top before color, bottom before tail, frame 198740. Prior completed frame: 3,720 blended clusters, 407,418 submitted vertices per pass, no drops or invalid draws.
- Before the click, the full GPU frame averaged about 1.18 ms and tail about 0.03 ms in the periodic profiles. This is actual invalid-read evidence; do not treat the crash as an established overload/timeout.

### Shared shader correction

Found an unguarded path in `asrSceneMacOIT.glsl`: fragment invocations compute `macoitPixel()` and physically address the per-pixel buffer without checking their framebuffer coordinates. Helper invocations generated for derivatives may execute ordinary loads even though their storage writes and atomics have no effect. Helpers at the viewport edge, particularly an odd-sized edge, can therefore address beyond the per-pixel storage. The keys pass predominantly uses atomics; tail and color perform ordinary state loads. This is consistent with the observed fault range; its role in this exact reproduction still needs runtime verification.

After `surfaceColor()` has evaluated all texture derivatives and implicit-LOD sampling, the shader now returns for `gl_HelperInvocation` and rejects coordinates outside `[0,width) x [0,height)` before forming any OIT buffer address. No later operation needs a derivative. This shared fix applies to Vulkan **and Metal**. No camera path was modified.

Diagnostics additionally report the previous completed view's dimensions. Fault messages now include `addressPrecision` and only associate read/write addresses with scene buffers; instruction pointers are no longer misleadingly described as far beyond the texture-feedback buffer.

### Validation and next check

- All three fragment programs pass Khronos glslang native GLSL 4.60/Vulkan 1.3 validation and `spirv-val --target-env vulkan1.3`.
- All three translate successfully to MSL 3.0 with the installed SPIRV-Cross. Inspected generated tail code: derivatives/sampling precede the helper return; coordinate checks precede OIT state reads. Metal uses `simd_is_helper_thread()`, also supported by the project's vendored translator at its configured MSL version.
- This is shader/SPIR-V/MSL translation validation, not an Apple compiler, viewer build, or runtime test. Those were not run by the agent. The earlier GLSL 4.10 scratch-check limitation for native-only shaders still applies.
- Next user build/run: Native, `ASRendererAvatarSkip 0`, then Alt-click/Alt-click-zoom on the avatar. Confirm it remains stable; if not, preserve the fault, checkpoints, address precision and view dimensions before further changes.

References: Vulkan [helper-invocation and derivative rules](https://docs.vulkan.org/spec/latest/chapters/shaders.html#shaders-helper-invocations), and [fault-address precision](https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceFaultAddressInfoKHR.html).

## Standalone tests of the guarded shaders, 10 November

The user requested testing with `E:\dev\AyaneStorm\Vulkan\build-asr\test\Release\asrenderer_test.exe`. Ran that existing executable without rebuilding. It predates the newer C++ diagnostics (their log strings are absent), so these runs verify its existing CPU tests and the updated shader path, not the latest C++ diagnostic implementation.

Created an isolated complete shader set under `/tmp/asr-avatar-validation/standalone-shaders`: copied the existing standalone shaders and replaced only Keys/Tail/Color with the freshly validated guarded SPIR-V. Passed that directory through `--shaders` using its Windows UNC path. No built artifacts in the standalone or viewer build folders were overwritten.

| Run | Result |
|---|---|
| `--selftest` | 257 checks passed, exit 0 |
| `--scene --ui --audit --validate --seconds 10` with the isolated shaders | All checks passed, exit 0; 564 audited frames, 0 pixels lost, 0 validation errors, 0 GPU-work errors; resize and texture replacement passed |
| `--scene --street --ui --audit --validate --seconds 10 --size 1281 721 --keep-size --memory-cap-mb 4096` with the isolated shaders | All checks passed, exit 0; odd viewport, 4 GB cap; 670 audited frames, 0 pixels lost, 0 validation errors, 0 GPU-work errors; staging 80.0 of 81.9 MB |

The main-thread stall checks passed in both GPU runs: normal/stalled 52/50 fps and 66/70 fps respectively, no stalled frame reached 100 ms. These are heavily instrumented validation runs, not performance benchmarks. Both logged a validation-settings warning about GPU-assisted and core validation running together and their overhead; neither logged a validation error.

Full logs for this session: `/tmp/asr-avatar-validation/standalone-selftest.log`, `standalone-scene-guard.log`, and `standalone-street-odd-guard.log`. The standalone scene does not exercise viewer Alt-click picking/camera behavior, so the user's exact viewer reproduction remains the runtime acceptance check.

## Crash resolved; mesh body seams and dress investigation, 10 November

### User observations and BOM source trace

The user reports no further crash after the helper/viewport shader guard. The new session log has about six minutes of scene profiles with no device-loss stop. It is preserved at `/tmp/asr-avatar-validation/viewer-after-helper-guard.log`.

The mesh body appeared, contradicting the earlier blanket BOM limitation. Checked `LLViewerObject::setTETextureCore()` and `getBakedTextureForMagicId()` in `llviewerobject.cpp`: the viewer already maps baked magic IDs to the avatar's real baked image in `mTEImages`, and `getTEImage()` returns it. `asrworld.cpp::fillMaterial()` reads that resolved image. This proves an existing mesh-texture route; it does not complete native system-avatar meshes, morphs, or local baking.

The provided crops show thin background/black cracks along body face boundaries. The user also reports that a dress section disappeared after the body loaded, remains absent at every camera angle, and some previously missing body parts appeared when zoomed in. The user exited the viewer and cannot currently guarantee reproduction of the dress issue. No additional or uncropped screenshot is needed.

### Shared packing grid per skeleton/skin, prepared

`GpuBinding.reach` previously came from each face independently. All faces use the skeleton's center, but a different reach gives a different 16-bit position grid; copies of a shared boundary vertex can therefore decode to different positions. This is a concrete crack-producing path consistent with the crops, not yet runtime-confirmed as their only cause.

Updated the common `SceneStore` code (`asrscenestore.h/.cpp`):

- Each existing skeleton/skin palette tracks its bound faces and the largest reach it has needed. Every face using that palette receives the same GPU reach.
- A new face, finer LOD binding or larger morph bound widens the grid for all those faces in the same update. Removed bindings leave the palette's list; the reach never shrinks while the palette lives.
- Other skins retain their own bounds, preserving the earlier precision improvement over an avatar-wide box. The grouping covers faces sharing a skeleton/skin; seams across different skin IDs are not yet established or solved by this change.
- No GPU layout, shader, public API, Vulkan implementation or Metal implementation change is needed: both backends use this shared store and their existing skinning shader.

Added two regression checks to `testSkinStore()`: a second face with a smaller morph bound must retain the common grid, and a larger morph on the first face must republish the grid to the existing second face. The existing binding-preservation and removal/reuse checks remain in place. These changes were subsequently built and tested by the agent with the user's explicit authorization; see the rebuilt standalone results below. Viewer visual confirmation remains pending.

### Dress: diagnosis remains open

Angle independence makes a simple one-sided/back-face explanation less likely. Zoom-related body appearance also warrants checking LOD arrival and weighted-face acceptance; the code currently rejects a face if its accumulated LODs report missing weights or joints beyond the skin. No specific dress asset/face has been identified, so do not claim an LOD or material diagnosis yet.

Expanded the existing `logObjectFaces()` inspector in `asrworld.cpp` to include every face, including cleared faces that the former texture-only loop omitted. Its `face state` line reports CPU submission, geometry, missing weights, weighted/available joint counts, material flags, alpha and cutoff. This is CPU submission metadata, not proof of GPU visibility. The existing `ASRendererLocalShift` tool triggers the inspection when a visible piece of the object is clicked and the setting changes. For a rigged object, `moveObject()` returns without applying a world shift; reset the setting to 0 afterward. Leave this inspection pending until the user can revisit the outfit; no forced reproduction now.

## Rigged surface wobble: joint prediction and rebuilt standalone, 10 November

### Concrete deformation in the old predictor

The user reports animated surface wobble, especially on hands, toes and hair. `asrSceneSkinC.glsl::predict()` extrapolated all twelve affine matrix components independently: `newest + (newest - previous) * lead`. This does not preserve a rotating joint's scale or shear. For example, extrapolating identity to a 60-degree rotation by one more step gives a planar scale of sqrt(3), instead of a pure 120-degree rotation. Each newly uploaded pose resets this extra deformation. This proves a deformation source in the code; it does not prove that every visible wobble in the user's viewer came from it.

### Shape-preserving prediction on both backends

- New portable `asrjointpose.h/.cpp` extracts the shortest world-space rotation delta once per uploaded joint pose. A scaled polar decomposition of `newest * inverse(previous)` removes scale/shear changes; singular or reflected changes use identity rotation. The newest affine shape is retained, including legitimate nonuniform scale and shear.
- Shared `asrSceneJointPose.glsl` raises the rotation delta quaternion to the frame's fractional lead and rotates the newest linear transform. Translation retains the existing linear extrapolation and time/lead limits. Main-thread pose delivery and the independent render loop are unchanged.
- A new `skin pose` compute pass caches one predicted matrix per joint per GPU frame. Every palette and rigid attachment reuses it. Added the pass to the shared frame graph and to both Vulkan and Metal pass dispatch; no Vulkan-specific prediction path.
- `GpuJoint` and its GLSL mirror grow from 96 to 176 bytes to hold the rotation delta, cached prediction and owning skeleton. The existing view padding holds the joint dispatch limit, preserving the 672-byte view size. Retired/free joints are skipped by skeleton/range checks. No shader version bump.
- Predicted unit rotation preserves the newest Frobenius norm. The skeleton bound therefore no longer adds the old matrix-difference stretch term, avoiding unnecessary growth of the packed-position grid. Translation bounds still cover the entire permitted lead.
- CMake builds the portable module and tracks the new shader include as a dependency. The earlier common-grid seam fix remains in the same standalone build.

### Build authorization and measured validation

The repository normally reserves builds for the user. The user's explicit request to rebuild the standalone overrides that restriction for the standalone; viewer builds remain the user's responsibility. Agent ran:

`C:\CMake\bin\cmake.exe --build E:/dev/AyaneStorm/Vulkan/build-asr --config Release --target asrenderer_test`

Build succeeded. All runs below use this freshly rebuilt executable and its freshly built shader directory `E:\dev\AyaneStorm\Vulkan\build-asr\shaders`, with no substituted old binary or isolated shader set.

| Run | Result |
|---|---|
| `--selftest` | 271 checks passed, exit 0; includes both common-grid regressions and 12 new rotation/translation/shape checks |
| `--scene --ui --audit --validate --seconds 10` | Passed, exit 0; 606 audited frames, 0 pixels lost, 0 validation/GPU-work errors; resize and live replacement checks passed |
| `--scene --street --ui --audit --validate --seconds 10 --size 1281 721 --keep-size --memory-cap-mb 4096` | Passed, exit 0; 671 audited frames, 0 pixels lost, 0 validation/GPU-work errors; staging 80.0 of 81.9 MB |

The motion checks compare predictions with independent analytic rotations around each axis, at 5, 60, 170 and -170 degrees and leads 0, 0.25, 0.5 and 1.1. They check translation and preservation of column lengths/angles for a nonuniformly scaled, sheared transform. Singular, reflection-change and stopped-pose cases are also checked. The existing skinned-vertex bound check now uses the new predictor.

Main-thread stall checks passed: normal/stalled 56/56 fps in the scene run, 66/70 fps at street level. Worst stalled frames were 20.5 and 17.2 ms. These instrumented validation runs are not performance benchmarks. Both runs emitted the existing warning about simultaneous core and GPU-assisted validation overhead, without validation errors.

All native shaders compiled through Khronos glslang in the build, and all 15 scene SPIR-V programs passed `spirv-val --target-env vulkan1.3`. The new skin program translated to MSL 3.0 with the installed SPIRV-Cross; inspected the generated per-joint prediction writes and cached reads. Native shaders use their existing GLSL 4.60 build path; the viewer-injected GLSL 4.10 scratch procedure does not apply. Apple compiler and Mac runtime verification remain pending.

Session logs: `/tmp/asr-avatar-validation/standalone-selftest-rebuilt.log`, `standalone-scene-rebuilt.log`, `standalone-street-odd-rebuilt.log`; translated shader: `skin-prediction.metal` in the same directory. Next acceptance check: one user viewer build, Native with `ASRendererAvatarSkip 0`, inspect hands/toes/hair during motion and the body face boundaries. The standalone checks do not establish the user's final visual result or solve the still-unidentified dress section.

## Viewer rejected first visual fixes; position fidelity, LODs and picking, 10 November

### User runtime result

The common packing grid and quaternion predictor did **not** resolve the reported seams or wobble. The user still sees mangled/pixelated fingertips, wobbling hair/hands/toes, an incomplete dress, body sections appearing only after zooming, and Alt-click camera focus passing through the avatar. Preserve this as the runtime result; earlier standalone success was not evidence of visual correctness. Session log snapshot: `/tmp/asr-avatar-validation/viewer-unresolved-seams-wobble.log`.

### Precision and weight normalization changes

- Removed final SNORM16 quantization of deformed positions. The old grid moved with the skeleton and differed across skins, even after sharing it between faces of one skin. Skinned positions now use float4 storage relative to one skeleton centre, with an identity linear instance transform. Static source geometry retains its existing packed format. The otherwise-zero low address bit tags float positions; the common reader masks it before any buffer access. Visibility, transparency, resolve and cluster bounds therefore consume the same positions on both APIs.
- Skin palette translations now subtract the common centre **before** weighted vertex accumulation. This avoids repeatedly accumulating large world translations and then subtracting them at each vertex. Rigid attachments still use world-space joint predictions. Cluster/group bounds use unrestricted float minima/maxima instead of assumptions that coordinates fit within a unit box. The skinned position table costs 16 rather than 8 bytes per vertex, accounted through its normal table budget.
- Found a separate normalization bug in viewer extraction. Rounding slots 1–3 and clamping slot zero's residual can produce a total of 65,536: `{0, .5, .5, 0}` is one example. Added portable `asrskinweights.h::packSkinAmounts()`, which assigns the signed rounding residual to the largest share, preserving a total of 65,535 without activating empty slots. Five regressions cover this, a three-share case, general/single-joint weights and the zero-total fallback. The viewer now uses the tested helper.
- The quaternion predictor remains, but it is no longer described as having resolved the user's wobble. Visual acceptance remains pending.

### Missing surfaces: isolate each LOD

The CPU store previously rejected a skinned binding if **any** LOD lacked weights. The viewer also accumulated an `unweighted` flag over the whole face. One unusable coarse face could therefore suppress valid finer surfaces. `LLVolume` represents a missing face with one vertex and three zero indices; the former `hasGeometry()` accepted that sentinel.

- Require at least three vertices for a face to contain geometry; the missing-face sentinel no longer poisons a rigged face.
- Track weighted joint requirements per face **per LOD** in `asrworld.cpp`. Accept a face if any available LOD can use its skin.
- Store each geometry's highest weighted palette index in the existing `GpuGeometry` padding word. A skinned binding supplies its palette count in the matrix field, which is otherwise only used by rigid attachments. CPU allocation and GPU LOD selection skip unweighted/out-of-range LODs and retain an available valid weighted surface. No geometry/binding layout growth.
- Added three CPU binding regressions: valid fine/unweighted coarse, unweighted fine/valid coarse, and valid fine/out-of-range coarse. A fourth checks uploaded per-LOD requirements. The standalone animated tubes also have an unweighted coarsest neighbour, exercising the GPU fallback.

These are verified code defects, but the specific missing dress asset/face is still unidentified. Do not claim the dress is conclusively diagnosed.

### Native avatar CPU state and camera picking

`LLVolumeGeometryManager` normally sets `LLDrawable::RIGGED` during GL batching. Native rendering does not run that batching, yet `LLVOVolume::calcLOD()` and `lineSegmentIntersect()` depend on the flag. Native binding now sets it (and the root's rigged-child flag). The native avatar update refreshes CPU spatial bounds, partition placement and camera distance before LOD requests. It sets the distance directly, avoiding `LLDrawable::updateDistance()`'s system-avatar GL mesh/VBO LOD rebuild. Rigged mesh LOD scheduling uses the avatar distance and animated extents rather than the small attachment's size.

`LLToolCamera::handleMouseDown()` explicitly passed `pick_rigged=false`. Native mode has no rendered system-avatar body proxy yet, so the camera could pick the background instead. A minimal ownership-tagged change in `lltoolfocus.cpp` passes `ASR::active()` for this argument; OpenGL retains its original false value. Actual CPU mesh intersection remains the viewer's existing implementation, with no new approximate picking surface.

These viewer-side changes are source-reviewed but are **not compiled or runtime-tested by the agent**; the standalone does not compile `asrworld.cpp` or `lltoolfocus.cpp`.

### Stronger GPU regression and final standalone results

Added `asrSceneSkinAudit.glsl`: with `--audit`, the cluster pass samples one actual stored vertex per skinned cluster, including hidden clusters, and compares it with direct float skinning. It records sample/error counts and maximum error through the existing counts readback. Both backends expose those statistics. Frame-graph access to the counts buffer is declared. There is no per-vertex audit work when auditing is disabled, no new readback, and no shader version bump.

The negative control copies the built shader set into `/tmp/asr-avatar-validation/quantized-control` and replaces only its scratch skin shader with one that restores the old moving SNORM16 quantizer. No repository shader or built shader was overwritten. The control **passes visibility auditing** (258 frames, zero lost pixels) but fails the new fidelity assertion: 614,836 errors among 614,844 samples, maximum 0.000172537533 m. Exit 1 is expected; Vulkan validation and GPU-work checks remain clean. This demonstrates why the previous visibility-only audit could pass despite deformation.

The final normal shaders and renderer built successfully with the authorized standalone target. Results:

| Run | Result |
|---|---|
| `--selftest` | 280 checks passed, exit 0 |
| `--scene --ui --audit --validate --seconds 10` | Exit 0; 488 audited frames, zero pixels lost; 1,312,200 position samples, zero errors, maximum error 0 m; no validation/GPU-work errors |
| `--scene --street --ui --audit --validate --seconds 10 --size 1281 721 --keep-size --memory-cap-mb 4096` | Exit 0; 675 audited frames, zero pixels lost; 2,891,184 position samples, zero errors, maximum error 0 m; staging 80.0/81.9 MB; no validation/GPU-work errors |

Stall checks passed: normal/stalled 50/52 fps and 66/70 fps, worst stalled frames 21.7 and 17.7 ms. Instrumented runs are not performance benchmarks. Native GLSL compiled through glslang; all 15 scene SPIR-V programs passed Khronos `spirv-val`. Skin, visibility, masked visibility, Mac OIT vertex and resolve programs translated to MSL 3.0. Apple compilation/runtime remain untested. The final weight-helper addition only rebuilt the standalone test executable; the GPU runs above use the final renderer/shaders.

Logs under `/tmp/asr-avatar-validation`: `build-position-fidelity-final.log`, `build-skin-weights.log`, `selftest-position-fidelity-final.log`, `scene-position-fidelity-final.log`, `street-position-fidelity-final.log`, `scene-quantized-control.log`. Next: one user viewer build/run to check all five reported symptoms; retain the face inspector for any dress section that remains absent.

## Busy-region budget failures: investigation paused by user, 10 November

The user tested a place with about 75 avatars. Earlier build stopped at 00:41:35 UTC with `scene table does not fit in its memory budget`. Immediately preceding statistics: 64 skeletons, 5,750 skins, 13,407 rigged objects, geometry 1,296/1,581 MB and textures about 4,968 MB. The screenshot confirms this budget rejection, not a device-loss message.

After the user finished building the float-position/picking/LOD changes, the same place stopped at 00:59:02 UTC with `retest list does not fit in its memory budget`. At 00:59:00: 35 skeletons, 3,067 skins, 7,618 rigged objects, geometry 1,551/1,697 MB. This is the same geometry-category allocation failure in another buffer. The geometry category currently receives a hard 15% of the driver budget; the log does not establish exhaustion of all VRAM.

Read-only findings for the next session:

- `asrdevicevulkan.cpp::createBufferObject()` and `asrdevicemetal.mm::createSceneBuffer()` reject a geometry reservation at its category limit. Errors currently omit the requested bytes and exact table name.
- Both backends' `growTable()` allocate the full new buffer while retaining the old one for frames in flight. Old retired buffers remain correctly charged, but there is no separate bounded allowance for replacement overlap. Vulkan/Metal `growList()` retire the old list before requesting the new one and can hit the same limit.
- `SceneStore::ensureTable()` doubles byte capacity; `allocateRange()` independently doubles entry capacity. `listCapacity()` also rounds up by powers of two. These layers can increase both steady spare capacity and transient growth peaks.
- `countInstance()` reserves cluster/group list room for every LOD in the source chain, although only one LOD draws. Retest bytes are `(visibleCapacity + groupCapacity + instanceEnd) * sizeof(GpuListEntry)`. Investigate conservative maximum-per-instance sizing rather than summing mutually exclusive LODs, while preserving per-bucket capacity and zero dropped geometry.
- Scene chunks already wait when allocation fails; table/list growth currently exits the renderer. A robust fix must preserve old-buffer lifetime and count all physical memory, without hiding over-budget use or lowering visual quality.

No budget fix has been implemented, built or tested. No renderer source changed after the user's latest viewer build; only this handoff note was added when the user requested wrapping up for usage limits. Next work: reproduce table/list replacement pressure in portable/standalone tests, distinguish steady demand from transient overlap, implement a bounded solution shared by Vulkan and Metal, and improve allocation diagnostics. Do not claim that a particular table or overlap size caused these runs: the existing log lacks those allocation details.

## Quiet-place runtime confirmation, 10 November

After teleporting to a quiet place with the latest viewer build, the user explicitly confirms that rigged surface wobble, disappearing dress sections and the missing right eye are fixed, then confirms there are no visible seams. This supersedes their pending status above. No individual change is isolated as the cause of each fix. Alt-click camera behavior has not yet been explicitly confirmed; the busy-region geometry-budget failures remain open. No renderer code changed in response to this confirmation.

## Alt-click and busy-region budget fixes, 10 November (Claude)

### Alt-click through avatars
- Cause: octree group bounds are refreshed only by `rebound()` inside `LLSpatialPartition::cull()`. Native mode returns before `updateCull()`, so the pick traversal tested rays against stale group bounds and skipped moved drawables (avatars), hitting what was behind.
- Fix (`llspatialpartition.cpp`, tagged): in native mode, `LLSpatialPartition::lineSegmentIntersect()` and the bridge branch of `LLOctreeIntersect::check()` call the root group's `rebound()` first. It is a no-op when not dirty.
- Codex's `pick_rigged = ASR::active()` in `lltoolfocus.cpp` was reverted to the original file: the system avatar is picked through its collision volumes (`LLVOAvatar::lineSegmentIntersect`), independent of rendering, as in OpenGL. The "no body proxy" premise was wrong, and rigged picking added CPU skinning per click.

### Budget
- `MemoryBudget::reserve()`: Geometry may borrow past its 15% share from memory that is actually free, never into the textures' share (`canBorrow()`). `textureLimit()` already counts it, so textures shrink to restore their reserve. Selftests added.
- Scene tables and lists (Vulkan `createBufferObject(..., required)`, Metal `createSceneBuffer(..., required)`) are counted past the limit with `reserveAnyway()` instead of stopping the renderer. Geometry chunks still wait for room.
- Refusals now report asked MB, category used/limit, total and textures (`MemoryBudget::refusal()`).
- Standalone: `--selftest` all passed; street audit at 1281x721 with 4 GB cap passed, no validation errors. Viewer not built.

### Open: per-avatar scaling for 100-150 avatars
Each skinned binding owns `SkinnedPositions` sized for the finest LOD of its chain (`addBinding`, max vertices over LODs) at 16 B/vertex, plus its own cluster entries, whatever LOD it draws. This memory grows linearly with the crowd and is the next design point.

## Crowd follow-up, 10 November (Claude)

User run: Alt-click fixed, no budget errors. Log at about 61 avatars: geometry 3331 MB (share 1635), textures 4.4 GB over a limit near 4.1 GB, discard bias 4, eye/lip textures cycling, GPU 20 ms of which **skin clusters 14.9 ms**, skin vertices 1.8 ms, draw early 0.6 ms.

- **Texture reserve 25% -> 5%** (`asrmemorybudget.cpp`). `pressure()` has no users; textures follow the viewer's discard bias, which aims at 85% of `textureLimit()`. The 25% reserve plus borrowed geometry squeezed textures to bias 4, and running at the limit made them shrink and reload in turn.
- **Skin clusters pass** (`asrSceneSkinC.glsl`): 8 threads per cluster (`CLUSTER_LANES`), strided triangles, workgroup shared-memory reductions, 3 barriers per round of 8 clusters. Same bounds and cones. No subgroup operations, so it does not depend on the GPU's SIMD width; no CPU dispatch change. Standalone scenes are too light to time it (both versions under 0.1 ms); the viewer run is the measurement.
- **Skinned copies sized to the drawn LOD** (`asrworld.cpp`): a rigged face names the finest loaded LOD at or below its object's `getLOD()` (`skinnedGeometry()`), not the shape's finest. A LOD change from `updateLods()` re-applies the object, which rebinds at the new size. A move keeps `WorldObject::skinnedLod`. The GPU still chooses within the chain from there, never finer.
- Standalone: `--selftest`, scene audit, street audit at 1281x721 with 4 GB cap: all passed, 0 pixels lost, positions exact. Viewer not built.

## LOD-cap regression and fix, 10 November (Claude)

User run: about **150 fps** in the busy place (was 60) after the skin clusters change. After a teleport home their own avatar stayed broken: spiky, torn surfaces (rigged meshes' crude lowest LODs drawn up close).

- Cause: `bindToAvatar()` set `LLDrawable::RIGGED` without what LL's batching does at that point (`llvovolume.cpp`, "first time this drawable is being marked as rigged, do another LoD update to use avatar bounding box"). `calcLOD()` had measured the attachment's own small size, so `getLOD()` was the lowest LOD. Before the cap the GPU chose from distance and hid it; with `skinnedGeometry()` it stuck.
- Fix: `bindToAvatar()` calls `volume->updateLOD()` the first time it sets `RIGGED`.
- Second gap: LOD rechecks are scheduled by camera travel only, which suits still objects. `updateAvatars()` now reschedules an avatar's objects (`AvatarSkeleton::lodDistance`) when its distance to the camera changes by more than 10%: avatars walking up, teleports.
- Not built.

## Note for next session (user, 10 November)
- Dirty shadows on avatars, source unknown. Investigate (the renderer has no shadow pass yet; check shading, normals and Mac OIT on avatar surfaces).

## Dirty avatar shading: placeholder light, 10 November (Claude)
- Source review found no defect. Skinned normals (`skinNormal()` in `asrSceneResolveC.glsl`) use the same palette, weights and vertex indices as positions; bind shape then inverse bind as `llmodel.cpp`; skinned instance rows are identity, so the cofactor leaves normals unchanged; morph normals are softened and normalized as the viewer does.
- User test: dark patches vanish when facing the fixed placeholder light (`unlit()`, `0.65 + 0.35 * dot(n, L)`, no ambient), so it is the placeholder light. The far side drops to 0.3 and looks darker than the mostly upward-facing environment. P4 lighting replaces it.
- Open: the far side is also 'not smooth'. The placeholder shows normal errors more than real lighting would, but it does not prove the normals are right. The skin audit checks positions only, not normals.
- User decision: leave the normal audit for P4.

## System avatar body, 10 November (Claude)
Plan: `doc/ayanestorm-new-renderer-avatar-body-plan.md`. Written, not built.
- New `asravatarbody.cpp/.h` (`ASRAvatarBody`). Per reference body mesh, once per session: a morph set (base vertices and every morph target; masked targets from the first avatar's params), a skin (the `mJointRenderData` walk, pivots as inverse binds, 15 entries at most) and a geometry per LOD over the shared vertices. Eyeballs have no weights and are attached to their skeleton joint.
- Per avatar: a morph per weighted mesh, polled from `getLastWeight()` each frame and sent only on change; masks sent when `LLVOAvatar::applyMorphMask()` calls `ASRWorld::avatarMasksChanged()`; an instance per shown part. Rules, LOD (`updateJointLODs()`, then the first valid LOD), texture and colour follow `renderSkinned()`, `renderTransparent()`, `renderRigid()` and `drawShape()`. Body and head are masked at 0.2; hair and skirt are blended and double-sided; eyelashes are blended. Self in mouselook hides head, eyelashes, hair and eyes. Avatars not fully loaded and invisible ones draw nothing. Jellydoll uses `getMutedAVColor()`, untextured.
- `asrworld.cpp`: every wanted avatar gets a skeleton; `AvatarSkeleton::body` keeps it alive; avatar death releases the body. `asrworldshared.h` exposes the scene, shared materials, texture visits and `rowsOf()`. `SharedMaterial` became the global `ASRSharedMaterial`. Log line: `system bodies: ...`.
- LL edits, tagged: `friend class ASRAvatarBody;` in `LLPolyMeshSharedData`, `LLPolyMorphTarget`, `LLAvatarJoint`, `LLAvatarJointMesh`, `LLAvatarAppearance` (`mMeshLOD` is protected there, which the plan missed); one hook and include in `llvoavatar.cpp`.
- Open: local bake composites (self while editing appearance) may not have a renderer slot; no 0.2 discard for blended parts in Mac OIT; attachments of jellydoll or invisible avatars are not hidden; cloth wind not drawn; P4 shadows need the mouselook head in the shadow view.

## Next session: skinning cost (user, 10 November)
Busy place, 79 avatars, after the LOD-cap fix and the system body build: **20 fps**. Log: GPU 48-54 ms = skin clusters 31-34, skin vertices 8-9, skin palette 3-5, body morphs 0.03, everything else ~5. Only 26-28 system body parts drawn: not the cause. Yesterday's 150 fps came from the LOD-cap bug (coarsest LODs everywhere), so it was never a real number.
Cause: every rigged binding of every avatar is re-skinned every frame (vertices, cluster bounds, groups), visible or not, moving or not.
Plan, in order, Vulkan and Metal together, tested in asrenderer_test first:
1. **Frustum skip.** In `bind()` (`asrSceneSkinC.glsl`), test a conservative sphere per binding (skeleton centre plus its reach: the joint spheres, morph bound and pose stretch the store already computes) against the view frustum. Outside: skip the vertex, cluster and group work and give the target geometry 0 clusters this frame, so stale bounds are never drawn. No pop: a sphere that enters the view is skinned that same frame.
2. **Unchanged-pose skip.** Skip a binding when its skeleton is settled (no new pose and no prediction lead), its chosen LOD is unchanged, its morph is not dirty and it was skinned before. Needs per-binding 'last skinned' state on the GPU.
3. **Occlusion skip** (later): a previous-frame Hi-Z test hides avatars behind walls, but one that comes out must still show the same frame. Design that before coding.
Also pending: the system body rez gate (`bodyReady()`) and the Mac OIT dark-patch test (`ASRendererAvatarSkip 4`) are not built or tested yet.

## Skinning cost, done and next (10 November, Claude)
- Built and tested by the user: frustum skip (`insideView()` now in `asrSceneCommon.glsl`, shared with the cull). Busy place: 30 fps. Mac OIT ruled out for the dark patches (`ASRendererAvatarSkip 4` did not help).
- Cluster bounds from joints, not triangles: `buildGeometry()` lists each skinned cluster's joints after its indices (count, weight-sum stray in 4/65535ths, joints). `skinCluster()` is one thread per cluster: the rest sphere (plus the morph bound, now in `GpuMorph::bound`) carried through each joint, stretch bounded by Gershgorin on L Lt, widened by the stray. The facing cone is kept only for one-joint, unmorphed, rigid clusters. asrenderer_test: selftest and audit pass (0 pixels lost, 0 skin errors). Needs a viewer build for timings.
- User build: GPU 14-19 ms (skin clusters 1.1 ms), 45 fps, now main-thread bound: 'object list' 8-11 ms, avatar animation. Written, not built: `setAnimationPeriods()` in `asrworld.cpp` ranks avatars by distance; the nearest `ASRendererFullRateAvatars` (16) animate every frame, then every 8th (8 m), 16th (16 m), 32nd (32 m), 64th frame, interpolated: `poseSkeleton(..., interpolate)` makes the store set `GpuSkeleton::minLead` -1 and `maxLead` 0, so the GPU blends from the pose before to the newest, one gap late, never overshooting; the gap after a rest is capped. Seated on a moving object: every 2nd frame, extrapolated (late, it would trail its seat). The 'settled' repeat now waits a full period (it used to go out the next frame, which stopped the GPU's motion on skipped frames). Tagged hook in `LLVOAvatar::computeNeedsUpdate()` uses `ASRWorld::animationPeriod()` for the skip test only, so `mUpdatePeriod` and `isImpostor()` are unchanged. Poses are only sent on change, so the GPU lead bridges skipped frames.
- Fix: the viewer's 4 s impostor refresh (`needs_update_by_max_time`) is never reset with no impostors, so every avatar animated every frame; the hook now skips it when we set the period (`animationPeriod()` returns 0 otherwise). User build: object list 10.3 -> 4.0 ms; 40-60 fps; GPU 12 ms; main thread 49 loops/s with 'everything else' 7.9 ms (unexplained); geometry memory 3632 of 1574 MB, textures 4870 of 5634 MB (likely the sharp/blurred texture cycling).
- Then, not built: periods are times (0.12/0.25/0.5/1 s by camera distance, 1 s out of view) turned into frames at the smoothed loop rate; ranking puts in-view avatars first (`sphereInFrustum`), stable sort.
- Open: dark patches appear a while after rez (a texture arriving, likely the normal map); many avatars stuck in system body (likely their mesh's wanted LOD not downloaded: show the lowest first); the palette 10 ms reading did not recur (497k entries, ~400k live: no leak).
- Dropped: the unchanged-pose skip (animations move almost every frame in crowds).
- Next, user's choice: (1) show the lowest rigged LOD until the wanted one has arrived (fast rez); (2) beyond the 16 nearest visible avatars, lower complexity (coarser LODs), no cached pictures except maybe far ones; (3) a normals debug view for the dark patches, which stay fixed on the skin.
