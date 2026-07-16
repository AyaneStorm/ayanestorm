<!-- <AS:Chanayane> Plain-English Exact OIT overview -->
# How Exact OIT Works

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
<!-- </AS:Chanayane> -->
