# Exact OIT on macOS

## Status

The Zink-over-MoltenVK and Zink-over-KosmicKrisp approaches were rejected at
their runtime proof gates on August 27, 2026. No viewer renderer code was
changed.

Apple's OpenGL 4.1 cannot implement the existing per-pixel linked-list capture.
Exact OIT requires fragment shader-storage writes, image atomics, and buffer
atomics introduced after OpenGL 4.1. A CPU-side SSBO wrapper or buffer texture
cannot supply those missing shader operations.

Depth peeling is also excluded because an earlier project implementation proved
too complex and unreliable.

## Zink and MoltenVK proof

The test used an Apple M4 Mac running macOS 26.6.1 and Xcode 26.6. All sources,
tools, builds, caches, and runtime files were isolated beneath
`/Users/ayane/ayanestorm/codex`.

Pinned sources:

- MoltenVK `v1.4.2` (`db66022459ffb663aa2b50f6b018bc2e124f5edf`)
- Mesa `mesa-26.2.1` (`da14d65e4499e66468094be52bff9ea0915a695e`)
- Compatibility comparison: Mesa `mesa-25.0.0`
- Python `3.12.11`, Meson `1.11.2`, Ninja `1.13.0`, and Bison `3.8.2`

MoltenVK and its pinned SPIRV-Cross and SPIRV-Tools dependencies built
successfully. Mesa 26.2.1 also built successfully with only the Zink Gallium
driver and its macOS and surfaceless EGL platforms. The resulting `libEGL`
loaded Zink and MoltenVK, and MoltenVK detected the Apple M4 and exposed Vulkan
1.4.357.

Surfaceless EGL initialization then failed before an OpenGL context could be
created:

```text
MESA: error: Zink requires the nullDescriptor feature of KHR/EXT robustness2.
FAIL: eglInitialize error=0x3001
```

This is a real feature mismatch rather than a command-line or window-system
problem:

- Mesa 26.2.1 hard-fails in `zink_screen.c` when
  `VkPhysicalDeviceRobustness2FeaturesKHR::nullDescriptor` is false.
- MoltenVK 1.4.2 explicitly reports `nullDescriptor = false` in
  `MVKDevice.mm`.
- MoltenVK's source notes that it does not support null descriptors.
- No supported environment option or alternate Zink descriptor mode bypasses
  the requirement in Mesa 26.2.1.

`GALLIUM_DRIVER=zink` is required for the surfaceless proof. Without it, Mesa's
surfaceless frontend selects a non-Zink path and reports only a generic DRI2
screen-creation failure. Requesting `ZINK_DEBUG=validation` without bundled
Vulkan validation layers also crashes after reporting the missing layer; the
decisive result above was reproduced without that optional setting.

## Older Mesa comparison

Mesa 25.0.0 did not unconditionally require `nullDescriptor`; its automatic
descriptor selection can fall back from descriptor buffers. Its original build
rejects EGL on the macOS platform:

```text
ERROR: Feature egl cannot be enabled: EGL requires DRI, Haiku, Windows or Android
```

A one-line build-system backport enabling DRI for Zink on Darwin, combined with
`-Degl-native-platform=surfaceless`, was sufficient to build Mesa 25.0.0. It
initialized Zink and MoltenVK past the `nullDescriptor` check, but could not
create the requested OpenGL 4.3 context. Zink reported that MoltenVK also lacks
base requirements including `logicOp` and `VK_EXT_custom_border_color`.

Downgrading Mesa therefore moves the failure rather than providing the required
OpenGL semantics.

## KosmicKrisp candidate

[KosmicKrisp](https://docs.mesa3d.org/drivers/kosmickrisp.html) is Mesa's Vulkan
implementation over Metal 4 for Apple Silicon and requires macOS 26 or newer.
Unlike MoltenVK 1.4.2, Mesa 26.2.1's KosmicKrisp source advertises the Zink
requirements that blocked both MoltenVK probes:

- `logicOp = true`
- `fragmentStoresAndAtomics = true`
- `vertexPipelineStoresAndAtomics = true`
- storage-image dynamic indexing, extended formats, and formatless reads/writes
- `VK_KHR_robustness2` and `VK_EXT_robustness2`, including
  `nullDescriptor = true`

KosmicKrisp's custom-border-color support is currently experimental and must be
enabled with `MESA_KK_EXPERIMENTAL=custom_border`. This is not an Exact-OIT
primitive, but it is a Zink base requirement and must pass ordinary-viewer
correctness testing before this backend is acceptable.

The next proof must build Zink and KosmicKrisp from the same pinned Mesa source,
create an OpenGL 4.3 or newer surfaceless context, and execute the Exact-OIT
SSBO, image-atomic, barrier, readback, and indirect-compute operations. Window
presentation remains a later gate.

### KosmicKrisp proof result

The combined stack was built successfully from two Mesa snapshots:

- Mesa `26.2.1` (`da14d65e4499e66468094be52bff9ea0915a695e`)
- Mesa `26.3.0-devel` snapshot
  `1a3bc6e571fa5a165171d958c5a0c0890a4184e6`, matching the latest
  KosmicKrisp documentation on August 27, 2026

The latter includes the documented custom-border lowering and enables it with
`MESA_KK_EXPERIMENTAL=custom_border`. The build used Zink and KosmicKrisp from
the same Mesa source plus the standard Vulkan loader 1.4.357.0. KosmicKrisp
loaded the Apple M4 successfully, and Zink no longer reported missing base
requirements. In particular, `nullDescriptor`, `logicOp`, and custom border
color passed Zink's startup validation.

An unversioned EGL context nevertheless reported only:

```text
GL_RENDERER=zink Vulkan 1.4(Apple M4 (MESA_KOSMICKRISP))
GL_VERSION=2.1 Mesa 26.3.0-devel
GLSL_VERSION=1.20
```

Requests for core OpenGL 3.0 through 4.3 all failed with `EGL_BAD_MATCH`.
KosmicKrisp does not expose `VK_EXT_transform_feedback`, so Zink advertises no
`GL_EXT_transform_feedback`; this is the first missing requirement in Mesa's
desktop OpenGL version chain. A diagnostic-only extension advertisement raised
the natural compatibility context to OpenGL 3.3. The remaining OpenGL 4.0
requirements then missing were transform-feedback 2/3 and
`ARB_texture_buffer_object_rgb32`.

`MESA_GL_VERSION_OVERRIDE=4.3` produced a diagnostic context with GLSL 4.60 and
the following useful limits:

```text
GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS=72
GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS=16
GL_MAX_IMAGE_UNITS=256
GL_MAX_FRAGMENT_IMAGE_UNIFORMS=32
GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS=1024
GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS=16
```

That override is not an acceptable implementation: it advertises a desktop GL
version whose complete semantics the backend does not provide. Implementing or
maintaining transform-feedback-to-storage-buffer emulation and RGB32 texel
buffer emulation would turn the spike into a custom Mesa backend fork and could
affect ordinary viewer correctness. It is outside the approved translated
renderer plan.

## Consequences

Do not add the proposed Zink/MoltenVK build option, packages, or macOS viewer
context backend in the current state. They cannot reach OpenGL context creation
with unmodified supported upstream components.

The remaining technically credible directions are:

1. Wait for MoltenVK to implement `nullDescriptor`, then rerun the existing
   surfaceless proof before attempting window presentation.
2. Re-evaluate a native Metal transparency backend as a larger renderer project.
3. Re-evaluate Zink over KosmicKrisp only after upstream exposes a complete GL
   4.3 feature profile, including transform feedback and RGB32 texel buffers.

Any future translation-layer retry must first create an OpenGL 4.3 surfaceless
context and execute the Exact OIT fragment SSBO/image-atomic probe. Cocoa window
presentation and viewer integration come only after that gate succeeds.

## Hybrid Apple OpenGL and Metal transparency backend

A native Metal backend limited to transparent rendering is the remaining
credible immediate implementation direction. Metal cannot merely supply an
SSBO to an Apple OpenGL shader: Apple OpenGL 4.1 has no API for binding an
arbitrary Metal buffer as shader storage, and its fragment shaders still could
not execute the required buffer and image atomics. Metal must therefore own the
complete transparent-fragment capture, sorting, and composite sequence.

The intended frame structure would be:

```text
Apple OpenGL
  render the ordinary opaque world
  expose or copy opaque color and depth information

Metal
  render transparent geometry
  capture PPLL nodes into Metal buffers with atomics
  sort each pixel's list
  composite over the opaque result

Present the combined image
```

This is smaller than a complete Vulkan or Metal viewer renderer, but it is an
Apple-only transparent-renderer backend rather than a storage-buffer shim. It
would require:

- Metal equivalents of every shader path that can contribute transparent
  fragments, including legacy, PBR, GLTF, fullbright, emissive/glow, particles,
  foliage, hair, glass, rigged meshes, and alpha-blended attachments.
- Metal vertex/index resources and transparent draw submission, or a reliable
  mirroring layer for the viewer's OpenGL geometry resources.
- Mirroring or sharing textures, samplers, transforms, material parameters,
  animation state, and per-draw uniforms.
- A representation of the opaque depth buffer that Metal can sample with the
  same depth convention and precision.
- Sharing or copying the opaque color result for the final composite.
- Explicit OpenGL-to-Metal ordering and lifetime management.
- Metal implementations of the unchanged PPLL node semantics, capture atomics,
  deterministic sorting, blend behavior, overflow detection, node-pool growth,
  opaque-cutoff pruning, and same-frame vanilla fallback.

IOSurface-backed color textures may provide a sharing mechanism where the
required formats are supported. OpenGL/Metal depth sharing is less certain and
must not be assumed. A fallback that copies or separately encodes opaque depth
may add frame cost or introduce precision differences. Likewise, ordinary
OpenGL buffer objects cannot generally be imported directly as Metal buffers,
so geometry and material resources may need duplication.

Using `glFinish()` before Metal submission is a possible correctness baseline,
but it serializes the APIs and may be too expensive. Production adoption
requires a less disruptive synchronization strategy or proof that the baseline
cost is acceptable. Vulkan offers no advantage for this hybrid on macOS: it
would still require Metal-backed interoperability while adding a loader and
another abstraction layer.

### Hybrid proof gates

Before implementing viewer material coverage, build an isolated macOS probe
that performs these steps in one application window:

1. Render opaque color and depth with native Apple OpenGL.
2. Make the opaque color available to Metal without CPU readback.
3. Make equivalent opaque depth available to Metal and validate depth equality
   at edges and equal-depth intersections.
4. Render transparent geometry in Metal and capture a PPLL using the production
   node layout and atomic allocation semantics.
5. Sort and composite in Metal, then present the combined image.
6. Repeat across resize, fullscreen-window changes, multiple frames, and
   teardown while checking validation output and resource lifetime.
7. Measure GPU/CPU synchronization, copies, VRAM duplication, and total frame
   time against native Apple OpenGL rendering.

Reject the hybrid approach if opaque color/depth cannot be transferred with
matching precision, if correct synchronization requires unacceptable pipeline
serialization, or if resource duplication makes ordinary viewer operation
unreliable. Do not weaken depth tests, fragment retention, deterministic ties,
blend semantics, or overflow fallback to make the interop probe pass.

If the interop proof passes, integrate one simple transparent material before
expanding to the full material matrix. Native Apple OpenGL remains the default
and fallback throughout the experiment. Windows and Linux rendering code,
shaders, dependencies, output, and performance remain unchanged.

## Asahi and HoneyKrisp

Mesa's Asahi Gallium OpenGL driver and HoneyKrisp Vulkan driver are not macOS
userspace drivers. They target Asahi Linux and require the Linux DRM subsystem,
`libdrm`, the Asahi kernel-driver UAPI, and a DRM render-node file descriptor.
Mesa's Asahi winsys is implemented in `gallium/winsys/asahi/drm`, and both the
Gallium and Vulkan builds depend on `dep_libdrm`.

Asahi can provide modern OpenGL when the machine is booted into a supported
Asahi Linux installation, but it cannot be bundled into a macOS viewer and
cannot submit work to Apple's macOS GPU kernel driver. Supporting it would
change the operating-system target rather than provide a macOS renderer.

KosmicKrisp is the relevant macOS effort: it reuses Mesa's Apple-GPU compiler
and common infrastructure while submitting through Metal 4. Its proof failure
therefore cannot be bypassed by substituting the Linux Asahi driver underneath
the macOS application.

## MGL OpenGL-on-Metal comparison

[MGL](https://github.com/openglonmetal/MGL) is an Apache-2.0 OpenGL-on-Metal
implementation that advertises OpenGL 4.6 entry points. Revision
`de0ded04ec7dc99182e27e555a17775523b26911` from January 12, 2026 was inspected.
Its README explicitly states that it is not a conformance implementation, that
uniform support is partial, that tests cover functional paths rather than all
permutations, and that many declared functions remain unimplemented.

The Exact-OIT-critical declarations exist, but their implementations do not
meet the proof gate:

- `mglDispatchComputeIndirect()` calls `assert(0)`.
- `mglShaderStorageBlockBinding()` is a TODO/no-op.
- `mglMemoryBarrier()` and `mglMemoryBarrierByRegion()` validate their bitmasks
  but issue no Metal synchronization.
- SSBO and image binding contain partial state tracking, but this does not prove
  fragment SSBO writes, image atomics, visibility barriers, or the unchanged
  GLSL node layout.

MGL therefore cannot run the existing Exact-OIT capture/sort/composite pipeline
unchanged. Filling those functions would still leave the much larger ordinary
viewer compatibility gate against a deliberately non-conformant OpenGL
implementation. It is currently a weaker integration base than Mesa
Zink/KosmicKrisp.

## Khronos_AppleICDs comparison

[Khronos_AppleICDs](https://github.com/Anonymous137-sudo/Khronos_AppleICDs) is
a third-party, unofficial repackaging of Mesa KosmicKrisp and MoltenVK into a
Vulkan ICD plus an OpenGL path for Apple Silicon macOS, published by a single
GitHub account with no Khronos affiliation despite the project name. It
explicitly disclaims Vulkan conformance certification and OpenGL 4.6
completeness, and its Vulkan CTS run reports 257,266 of 881,906 cases passing,
a pass ratio low enough to treat the qualification claim with skepticism
rather than as evidence of a solid driver.

Its standard OpenGL path caps out at OpenGL 4.1 core, the same ceiling this
document's Zink and KosmicKrisp proofs already found insufficient: fragment
shader-storage writes, image atomics, and buffer atomics used by the existing
PPLL capture do not exist before OpenGL 4.3. Because it is built from the same
Mesa/KosmicKrisp components already tested above rather than an independent
implementation, it does not clear the OpenGL 4.3 surfaceless-context gate this
document requires and does not change the Consequences below.
