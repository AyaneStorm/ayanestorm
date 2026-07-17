# How Exact OIT Works

## Simple explanation

Normally, transparent objects must be drawn in the correct order. If a renderer
draws them in the wrong order, hair, glass, eyelashes, smoke, and similar
surfaces can hide or overwrite one another incorrectly.

Exact OIT removes that dependency on draw order:

1. While rendering transparency, every visible transparent fragment is stored
   in GPU memory instead of being drawn immediately. A fragment is one object's
   contribution to one screen pixel.
2. Each screen pixel keeps its own linked list of fragments, including their
   color, opacity, depth, glow, and blending rules.
3. The GPU sorts each pixel's list by depth, from the farthest fragment to the
   nearest.
4. The sorted fragments are blended over the opaque scene in their exact visual
   order.

Opaque geometry still uses the normal depth buffer, so transparent fragments
hidden behind walls or other solid objects are rejected before entering the
lists.

The lists have no fixed per-pixel layer limit. They share a large GPU buffer
whose size is bounded by a safe VRAM budget. If that buffer ever fills, the
renderer does not show an incomplete result: it discards the captured data and
rerenders all transparency with the complete vanilla renderer for that frame.

In short, Exact OIT first records what every pixel should contain, then sorts
and combines it correctly. This makes the result independent of the order in
which objects happened to be submitted by the viewer.

## Technical explanation

### Frame resources

The implementation is a per-pixel linked-list OIT renderer, also called PPLL or
an A-buffer. At full render resolution it uses:

- An `R32UI` head-pointer image. Each texel contains the index of the first node
  for that pixel, or `0xFFFFFFFF` when the list is empty.
- An `R32UI` list-count image containing the exact number of successfully
  captured fragments for each pixel.
- A shader-storage buffer containing 48-byte fragment nodes.
- A small control shader-storage buffer containing the total node count, node
  capacity, overflow flag, and maximum per-pixel list length.
- An RGBA16F copy of the opaque scene used as the background for final
  compositing. Capture itself leaves the main scene target untouched, allowing
  vanilla fallback to start from the original opaque result.

Each node stores shaded color, glow, window-space depth, the next-node index,
the packed original blend factors, and a capture sequence number. The sequence
number gives equal-depth fragments a deterministic order.

The node buffer initially targets an average of four fragments per screen
pixel. Its allocation is limited to the smaller of 25 percent of reported
dedicated VRAM and 2 GiB. Resolution-dependent images are recreated when the
viewport changes, but an existing sufficiently large node buffer is retained
to avoid reallocating hundreds of MiB during camera-mode transitions.

### Fragment capture

The standard post-water transparency shaders have Exact OIT variants for
regular and rigged alpha, PBR alpha, fullbright alpha, legacy materials, GLTF,
emissive surfaces, and PBR glow.

Instead of writing color into the framebuffer, a capture shader:

1. Atomically reserves a node from the global node buffer.
2. Writes the fragment data into that node.
3. Uses `imageAtomicExchange` to make the new node the pixel's list head while
   preserving the previous head as the node's `next` index.
4. Atomically increments the pixel's exact list count.
5. Updates the maximum observed list length.

All capture fragment shaders declare `layout(early_fragment_tests) in`. The
opaque depth buffer therefore rejects transparent fragments hidden by solid
geometry before they allocate nodes. Depth testing remains enabled, while
transparent capture does not write depth.

Allocation is bounds checked. A fragment that cannot reserve a valid node sets
the overflow flag and does not write outside the buffer.

### Synchronization and validation

After all capture draws, the renderer issues shader-storage and image-access
memory barriers. It then performs one synchronous read of the control buffer.
This provides the exact allocation count, overflow state, and maximum list
length needed to choose between exact compositing and complete fallback.

The readback is currently mandatory because the CPU must not submit an Exact
OIT composite for an incomplete capture. The same read supplies diagnostics,
so diagnostics do not add another synchronization point.

### Exact per-pixel sorting

Each pixel list must be ordered farthest-to-nearest before blending. The
renderer uses exact natural linked-list merge sort:

1. A fullscreen draw discovers naturally ordered runs in every active pixel.
2. Runs already ordered farthest-to-nearest are retained. Reverse-ordered runs
   are reversed while being detached.
3. Adjacent run pairs are merged into larger ordered runs.
4. The number of remaining runs is stored for the pixel.
5. Later fullscreen draws repeat this process only for pixels with more than
   one run remaining.
6. A memory barrier separates every draw because one pass rewrites links that
   the next pass reads.

Splitting the sort across draws bounds the work done by one shader invocation
and avoids the GPU watchdog risk of sorting a very deep list in one invocation.
The renderer submits at most
`ceil(log2(maximum per-pixel list length))` sort draws, which is sufficient even
for a list whose every fragment begins as a separate run.

Particle and sprite batches commonly arrive mostly depth sorted. When the
camera is stationary, a pixel may contain one monotonic run and finish after
one traversal. Camera movement can introduce local depth-order changes without
making the whole list random. Natural merge sort preserves the ordered portions
and bases the remaining work on the number of runs rather than treating every
fragment as an isolated run.

A completely monotonic list therefore costs `O(n)`. A genuinely unordered list
retains the exact `O(n log n)` worst case. Partially ordered lists fall between
those cases, while producing the same final order and retaining every fragment.
Later fullscreen sort draws may still be required by other pixels, but pixels
that reach one run return immediately without traversing their lists again.

### Blending and glow

The final fullscreen pass starts with the saved opaque pixel and traverses the
sorted list from farthest to nearest. For every color node it reconstructs the
captured source and destination color and alpha blend factors, then evaluates
the corresponding blend operation in shader code. Glow nodes contribute to the
pixel's glow value without pretending to be ordinary color-alpha surfaces.

The resulting color is written back to the main scene target. Because blending
happens only after exact sorting, the result does not depend on the order in
which separate objects and draw batches reached the capture shaders.

### Overflow and failure behavior

If capture reports overflow, the linked-list result is discarded completely.
The renderer reruns the entire standard transparency path over the untouched
opaque scene during the same frame. It may then grow the node buffer within the
VRAM limit for later frames.

Missing shaders, unsupported OpenGL capabilities, allocation failures, or
other session-level Exact OIT failures also select the complete standard
renderer. The implementation never displays a partially captured list and does
not fall back to weighted blended OIT.

The feature is controlled by `RenderExactOIT`. Disabling it leaves the standard
transparency path active and avoids Exact OIT allocation and capture work.
