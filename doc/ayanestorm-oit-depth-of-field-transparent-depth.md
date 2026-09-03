# OIT and depth of field: transparent surfaces blurred at the background's depth

Author: chanayane@firestorm. Date: 2026-09-04. Status: recorded, not to
fix now (user decision).

Searchable terms: DoF hair blur, depth of field alpha, transparent depth
write, OIT scene depth, avboitFrontKey0 depth, isolate depth pass.

## Symptom

With depth of field on and the camera focused on the face, hair that is
not in front of the face (beside the head, over the shoulder) is blurred
as if it sat at the background's distance. Same in Exact OIT and AVBOIT.
Vanilla also looks wrong in its own way; only the OIT paths matter here.

## Cause

DoF reads the scene depth buffer to pick the circle of confusion per
pixel. Neither OIT path writes the front transparent surface's depth into
that buffer:

- AVBOIT resolves in a compute pass (`avboitVolumeC.glsl` pass 7) that
  stores colour with `imageStore`; a depth-format texture cannot be
  written that way. The existing isolate-depth pass in
  `FSAVBOIT::finishDirectFrame()` (`avboitIsolateDepthF.glsl`) already
  exists for a related reason and writes a near-plane constant, not a real
  depth, and only when isolate mode is active.
- Exact OIT composites from its per-pixel fragment lists and likewise
  leaves scene depth at the opaque value behind the transparent pixels.

So where hair covers a distant wall, the depth buffer says "wall" and DoF
blurs the hair like the wall. In front of the face the depth is the
face's, close to the hair's, so it looks right there.

## Possible fix (when wanted)

AVBOIT already has the exact nearest transparent depth per pixel:
`avboitFrontKey0` (24-bit window depth, written by raster pass 3; see
`ayanestorm-oit-avboit-glass-darkening.md`). Make the isolate-depth pass
unconditional and have it write `gl_FragDepth = key0 depth` where
coverage is non-zero (weight or glow), with `GL_LEQUAL` so it never moves
depth farther. Cost: one full-screen triangle. Exact OIT would need the
same from the nearest node of its per-pixel list in its composite step.

Caveats to check before doing it: anything after the alpha pass that
depth-tests against scene depth (later isolate passes, glow, UI, water
reflections) will now see hair as a solid surface; verify each. Also
decide whether sheer surfaces (alpha well below 1, e.g. a window pane)
should write depth at all; a threshold on the key's alpha byte is the
natural knob.
