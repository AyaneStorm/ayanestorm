# AyaneStorm New Renderer: HUDs and Selection Outlines (P3 step h)

Brought forward from P5 by the user, 12 November 2026: the viewer is hard to use without
HUDs and selection outlines, and neither waits for lighting.

## Decisions

- **HUDs**: a second view drawn over the finished scene, before the UI, as OpenGL does:
  HUD camera (HUD zoom, fixed orthographic projection), its own depth, the same scene
  store, textures and Mac OIT. HUD objects are today left out in `whyLeftOut()`
  (`asrworld.cpp`). Picking stays the viewer's CPU picking.
- **Selection outlines**: GPU contour, chosen by the user 12 November. Selected objects
  (prims, meshes, rigged meshes, avatars) are written to a mask; one pass draws a
  constant-width outline around their outer contour, in the viewer's selection colours.
  Inner edges are no longer outlined. Replaces the CPU silhouette edges
  (`LLSelectNode` silhouettes), which miss rigged mesh and cost CPU on big linksets.

## To research before code

- OpenGL HUD pass: projection, zoom, lighting and alpha rules for HUD objects.
- OpenGL selection drawing: colours (own, other, linked, child), how hidden parts show,
  face highlights for the face tool, hover highlights.
- Where both fit in the frame graph next to the scene colour and the UI list.
