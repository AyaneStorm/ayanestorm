# Exact OIT active-pixel sorting plan

## Result

Rejected and removed. The shader compiled after an initial reserved-keyword
fix, but the NVIDIA driver crashed while loading the world. Changing the point
draw to a bounded one-vertex instanced draw still crashed immediately when the
optimization was enabled. The shader, coordinate buffer, control-layout
extension, setting, and draw path were removed. Shader-cache revision v12
restores the proven fullscreen architecture.

## Goal

Avoid launching every natural-sort pass across the complete viewport when only
a subset of pixels captured transparent fragments. Preserve every node, the
existing total order, blend behavior, overflow handling, and same-frame vanilla
fallback.

## Removed experiment

The experiment recorded a pixel coordinate only when `imageAtomicExchange` reported that
the new node replaced an empty head pointer. Exactly one fragment can observe
the null head for a pixel, so the active list contains each captured pixel once.
Coordinates are packed as two 16-bit components in one 32-bit word.

The active count shared the existing control SSBO and therefore arrived in the
existing validation readback. This added no synchronization point. A separate
SSBO reserved one 32-bit coordinate per viewport pixel.

Natural-sort draws used a dedicated vertex shader. One point vertex was
instanced for every active pixel, and `gl_InstanceID` read the corresponding
coordinate and emitted the point at that pixel center. The existing Exact OIT
composite fragment shader performed the unchanged natural merge pass. The final
blend remained fullscreen.

The removed `RenderExactOITActivePixelSort` setting gated collection and
sorting. The active-sort shader and coordinate buffer were optional so their
initial compile or allocation failure retained baseline Exact OIT.

The first active-sort vertex shader used `packed`, which is a reserved GLSL
keyword, as a local variable name. NVIDIA rejected the shader and the optional
fullscreen fallback operated correctly. The variable was renamed and the
shader-cache revision advanced to v10.

The instanced point-draw correction advances the shader-cache revision to v11.

## Correctness constraints

- Record a pixel only after a node allocation succeeds.
- Never append more than one coordinate for a pixel.
- Allocate the coordinate buffer for every viewport pixel.
- Keep capture counts, maximum-list length, overflow, and node capacity based
  on the complete capture.
- Preserve all linked lists and natural-sort ordering.
- Retain all memory barriers between capture and sort passes.
- Do not add CPU readback or a partial fallback.
- Keep the fullscreen path available for live A/B validation.

## Validation

Compare active-pixel and fullscreen sorting for:

- empty and nearly empty transparency scenes;
- fullscreen transparent surfaces;
- dense overlapping sprites while moving and stationary;
- equal-depth fragments;
- glow-only pixels;
- opaque-cutoff pruning enabled and disabled;
- diagnostic modes 0 through 7;
- overflow and complete same-frame fallback;
- viewport resizing and first-/third-person transitions.

Images must match exactly. Profile capture, every natural-sort pass, final
blend, validation wait, and whole-frame GPU time. Active collection adds one
control atomic and one coordinate write per populated pixel, while sorting
changes from one fragment invocation per viewport pixel per pass to one
invocation per populated pixel per pass.

## Later work

The initial active list is not compacted between passes. Pixels whose lists
have reached one natural run still launch and return early. A later GPU-only
ping-pong queue may compact unfinished pixels and drive indirect draws, but it
should be attempted only if this simpler active-pixel list demonstrates a
measurable benefit and remains stable.
