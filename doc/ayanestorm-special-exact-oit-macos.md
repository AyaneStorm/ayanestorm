# Exact OIT on macOS

## Current limitation

The current Exact OIT implementation cannot run through Apple's native
OpenGL implementation. This is an API limitation, not a limitation of the GPU
hardware in Apple silicon or in otherwise Metal-capable Macs.

Apple's OpenGL implementation exposes at most OpenGL 4.1. The current Exact
OIT renderer is a per-pixel linked-list renderer (PPLL, also called an
A-buffer) whose capture and composite shaders require OpenGL 4.3 and GLSL
4.30. Updating macOS, including to macOS Tahoe, does not raise the OpenGL
version exposed by Apple.

`FSExactOIT::isSupported()` consequently requires both OpenGL 4.3 and GLSL
4.30. Unsupported systems retain the complete standard transparency renderer;
they do not allocate Exact OIT resources or compile its shader family.

## Why this implementation requires OpenGL 4.3

Every captured transparent fragment becomes a node in a global,
variable-length shader-storage buffer object (SSBO). The capture shader
atomically reserves an index, writes the shaded fragment into the node array,
and inserts that index at the head of the current pixel's linked list. The
composite shader later reads and sorts those nodes.

The decisive OpenGL 4.3 facilities are:

- shader-storage buffer objects and the `GL_SHADER_STORAGE_BUFFER` target;
- GLSL `buffer` interface blocks with `std430` layout;
- atomic operations on the counters stored in those buffers; and
- `glBindBufferBase` bindings that expose the node and control SSBOs to the
  shaders.

Shader image load/store is also fundamental to this PPLL design. Two `R32UI`
images hold each pixel's list head and exact fragment count.
`imageAtomicExchange` inserts a node without losing the previous head, while
`imageAtomicAdd` increments the pixel count. These operations,
`glBindImageTexture`, and the relevant image-access memory barriers entered
core OpenGL in version 4.2. They are therefore also unavailable in Apple's
OpenGL 4.1 implementation.

The renderer currently calls `glCopyImageSubData`, which entered core OpenGL
in version 4.3, to preserve the opaque scene before compositing. That call is
convenient rather than fundamental: an FBO blit or fullscreen copy could
replace it. Replacing it would not remove the SSBO and image-atomic blockers.

Reducing the implementation to an OpenGL 4.2 design might be possible by
replacing the SSBO node array with different image-backed storage. It would
still not run through Apple OpenGL 4.1 because image load/store and image
atomics would remain unavailable.

## Universal-build linker failure

A macOS universal build currently fails to link its x86_64 slice when
`fsexactoit.o` directly references:

- `glBindImageTexture`;
- `glMemoryBarrier`; and
- `glCopyImageSubData`.

The missing linker symbols are an early symptom of the platform limitation,
not its complete extent. Declaring the functions locally or resolving them
dynamically would at most change how the calls are found. It would not add the
underlying OpenGL 4.2/4.3 shader features to Apple's OpenGL 4.1 driver, and the
Exact OIT GLSL would still be unsupported.

A future build fix must therefore compile the current OpenGL backend only on
platforms that can provide it and leave standard transparency active on native
Apple OpenGL. A separate macOS backend can then be selected when and if one is
implemented.

## Translation-layer assessment

No OpenGL-to-Metal translation layer is currently integrated into the viewer.
The repository's `vulkan_gltf` dependency is not a Vulkan rendering backend
and does not provide MoltenVK.

### MoltenGL

[MoltenGL](https://moltengl.com/products/) implements OpenGL ES 2.0 over Metal.
It does not provide the desktop OpenGL 4.3 API or the SSBO and image facilities
used by this implementation.

### ANGLE

[ANGLE](https://github.com/google/angle) translates OpenGL ES to several
native backends, including Metal. It is not a drop-in desktop OpenGL 4.3
implementation. Adapting the viewer to ANGLE would require a broad conversion
to its supported OpenGL ES surface and would not directly preserve this PPLL
implementation.

### MoltenVK

[MoltenVK](https://github.com/KhronosGroup/MoltenVK) implements a Vulkan
portability subset over Metal. It does not translate OpenGL calls. Using it
would first require a Vulkan viewer renderer or a self-contained Vulkan
transparency renderer with the same resource-sharing problems as a Metal
renderer.

### MGL

[MGL](https://github.com/openglonmetal/MGL) is the closest translation-layer
experiment because it aims to implement desktop OpenGL 4.6 and OpenGL ES 3.x
over Metal. Its own documentation says that not all OpenGL functionality is
available and identifies partially implemented areas. It is not present in
the viewer and must not be assumed to be a conformant or production-ready
replacement for the existing macOS context.

A bounded MGL experiment would need to validate the ordinary viewer before
Exact OIT: context creation, GLSL translation, framebuffer behavior, texture
formats, buffer mapping, synchronization, every relevant draw path, and both
arm64 and x86_64 builds. Exact OIT would then specifically require verified
support for GLSL 4.30, SSBO atomics, image atomics, memory barriers, and the
required copy operation or its replacement.

## Implementation options

### Hybrid OpenGL and Metal

The leading native feasibility direction is a small Metal transparency
renderer alongside the existing OpenGL renderer. Apple documents
[mixing Metal and OpenGL rendering in one view](https://developer.apple.com/documentation/metal/mixing-metal-and-opengl-rendering-in-a-view)
through Core Video/IOSurface-backed interoperable textures.

Apple's sample creates a `CVPixelBuffer` with both
`kCVPixelBufferOpenGLCompatibilityKey` and
`kCVPixelBufferMetalCompatibilityKey` enabled. It then creates two API views
of that same backing:

- `CVOpenGLTextureCacheCreateTextureFromImage` produces a
  `CVOpenGLTexture`, whose OpenGL name is obtained with
  `CVOpenGLTextureGetName`.
- `CVMetalTextureCacheCreateTextureFromImage` produces a `CVMetalTexture`,
  whose Metal texture is obtained with `CVMetalTextureGetTexture`.

This is directly useful for exchanging a compatible color image without CPU
readback. The Core Video pixel format, Metal pixel format, and OpenGL internal
format must be selected as a compatible combination. The sample requires
macOS 10.13 or later.

The sample does not establish that an existing viewer OpenGL texture can be
retrofitted with shared backing. The interoperable `CVPixelBuffer` must own the
backing from creation, so viewer integration would allocate the exchanged
color target through Core Video and render or copy into its OpenGL view. It
also does not demonstrate sharing a depth attachment, vertex or index
buffers, material textures, or Metal PPLL node and counter buffers. Those
remain separate prototype questions. In particular, color-texture
interoperability alone does not eliminate the depth conversion/copy described
below.

Apple presents the mechanism as a migration bridge: either Metal renders the
shared texture for OpenGL to sample, or OpenGL renders it for Metal to sample.
That validates alternating API ownership of one image, but it is not a way to
issue Metal operations from inside OpenGL rendering or to share general
OpenGL object names.

A hybrid frame would have this boundary:

1. OpenGL renders the opaque scene normally.
2. Scene color and depth are made available to Metal through compatible
   interoperable textures or explicit copies.
3. Metal rerenders the supported transparent draw list, builds the per-pixel
   nodes, sorts them, and composites them over the opaque scene.
4. The completed color is returned through an interoperable texture.
5. OpenGL resumes the remaining post-processing, UI, and presentation work.

Both transparent capture and composite must execute in Metal. A Metal
composite pass alone cannot solve the problem because OpenGL 4.1 fragment
shaders cannot construct the Metal node lists. Likewise, Metal API calls cannot
be substituted inside an executing OpenGL shader.

This approach preserves most of the viewer renderer but still constitutes a
small second renderer. Metal needs the transparent geometry, vertex formats,
indices, transforms, textures, material parameters, blend rules, depth state,
rigging data, and draw order currently prepared for OpenGL. OpenGL object names
are not general-purpose Metal resource handles, so resources need compatible
shared backing, Metal mirrors, or explicit uploads.

API ownership transfers also require explicit synchronization. Poorly placed
transfers can serialize the two command streams and introduce GPU or CPU
stalls. The design should minimize the boundary to one OpenGL-to-Metal handoff
and one Metal-to-OpenGL handoff per eligible frame.

### Full Metal renderer

A complete Metal viewer backend would eliminate ongoing OpenGL interoperability
and deprecation problems. It would also require porting the entire renderer,
shader system, resource lifecycle, post-processing, UI composition, and
platform integration. This is the cleanest long-term architecture and the
largest project.

### OpenGL 4.1 depth peeling

Depth peeling can produce correctly ordered transparency without SSBOs or
image atomics by repeatedly rendering transparent geometry to discover
successive depth layers. It could remain within native OpenGL 4.1, but its
geometry cost grows with the number of layers. Scenes containing hair,
particles, vegetation, or intersecting glass may require many passes, making
the technique substantially more expensive than PPLL and requiring a bounded
quality or performance policy.

### Standard transparency fallback

The current safe behavior is to leave Exact OIT unsupported on macOS and use
the complete standard transparency renderer. This requires no new renderer and
remains the fallback for unsupported hardware, disabled Exact OIT, backend
initialization failure, or runtime resource failure under every future option.

## Hybrid feasibility prototype

The first prototype should test the interoperability boundary rather than
attempting all viewer materials immediately:

1. Create a Metal device and command queue associated with the same physical
   GPU as the active OpenGL context.
2. Create one color texture with compatible OpenGL and Metal views and prove
   alternating writes and reads in both directions.
3. Establish a compatible depth-transfer representation. If the OpenGL depth
   attachment cannot be shared directly, copy or linearize it into a shareable
   texture that Metal can depth-test or sample.
4. Render one transparent triangle in Metal over an OpenGL opaque scene and
   return the result for OpenGL presentation.
5. Implement a minimal Metal PPLL capture and composite for that triangle,
   including buffer atomics, overflow bounds, memory ordering, sorting, and
   blending.
6. Measure the two API handoffs and copies independently on Apple silicon and
   an Intel Mac supported by the viewer.
7. Prove that the implementation builds and links in both arm64 and x86_64
   slices of the universal application.

Only after those steps succeed should viewer integration expand to the actual
alpha draw traversal, shader/material variants, rigging, glow, particles,
fallback rerendering, diagnostics, resizing, and resource-budget behavior.

The prototype is successful when it demonstrates all of the following:

- no CPU readback of scene color, depth, or fragment nodes;
- correct opaque-depth rejection and exact ordering of overlapping fragments;
- bounded node allocation with a complete standard-renderer fallback;
- a working OpenGL-to-Metal-to-OpenGL frame on supported arm64 and x86_64 Macs;
- correct behavior after resize, display change, context recreation, enable,
  disable, and re-enable;
- measured synchronization and copy costs low enough to justify full viewer
  integration; and
- no visual or performance effect when Exact OIT is disabled or the Metal
  backend is unavailable.

This document records feasibility and architectural constraints. It does not
select MGL, commit the viewer to a Metal backend, or claim that macOS Exact OIT
has already been implemented.
