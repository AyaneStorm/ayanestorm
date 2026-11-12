# AyaneStorm New Renderer: HUDs and Selection Outlines (P3 step h)

Brought forward from P5 by the user, 12 November 2026: the viewer is hard to use without
HUDs and selection outlines, and neither waits for lighting.

## Decisions

- **HUDs**: a second view drawn over the finished scene, before the UI, as OpenGL does:
  HUD camera (HUD zoom, fixed orthographic projection), its own depth, the same scene
  store and textures. HUD objects are today left out in `whyLeftOut()`
  (`asrworld.cpp`). Picking stays the viewer's CPU picking.
- **HUD look, as OpenGL** (`render_hud_attachments()`, `get_hud_matrices()` in
  `llviewerdisplay.cpp`): unlit (`gHUDFullbrightProgram` and the HUD PBR programs:
  texture times colour, emissive), drawn after tone mapping in display colours, depth
  cleared, no occlusion culling.
- **Hiding**: drawn only when OpenGL would (`LLPipeline::sShowHUDAttachments`, toggled by
  Alt+Shift+H; not when disconnected), so the shortcut works as today (user, 12 November).
- **HUD alpha: sorted blending, not Mac OIT** (user, 12 November). OpenGL draws HUD alpha
  back to front; HUD makers layer buttons millimetres apart and rely on that order. Mac
  OIT would cost full-screen passes and could change which layer shows.
- **Selection outlines**: GPU contour, chosen by the user 12 November. Selected objects
  (prims, meshes, rigged meshes, avatars) are written to a mask; one pass draws a
  constant-width outline around their outer contour, in the viewer's selection colours.
  Inner edges are no longer outlined. Replaces the CPU silhouette edges
  (`LLSelectNode` silhouettes), which miss rigged mesh and cost CPU on big linksets.

- **HUD editing**: OpenGL eases `gAgentCamera.mHUDCurZoom` to its target inside
  `render_hud_attachments()`, which the native path (`display_native()`) never calls; the
  viewer hook does the same easing each frame, then takes `get_hud_matrices()`, so the
  edit zoom-out animates as today. Picking and dragging stay the viewer's.
- **3D UI** (`render_ui_3d()`: build handles for move, rotate and scale, and the rest),
  moved from P5 into step h by the user, 12 November: the native path draws only the 2D
  UI today, so handles are missing for world objects and HUDs alike. Drawn over the scene
  with the scene's depth.

- **Toggles honoured** (user, 12 November): outlines only when OpenGL's
  `LLSelectMgr::renderSilhouettes()` would draw them: `(mFSShowHideHighlight ==
  FS_SHOW_HIDE_HIGHLIGHT_NORMAL && mRenderSilhouettes && mRenderHighlightSelections)
  || mFSShowHideHighlight == FS_SHOW_HIDE_HIGHLIGHT_SHOW` (build floater 'Show
  Highlight', `RenderHighlightSelections`); hidden parts per `sRenderHiddenSelections`;
  face tint per `RENDER_DEBUG_FEATURE_SELECTED`, as `renderHighlights()`. Face picking
  stays the viewer's CPU picking; each face is its own instance, so the face tool's
  selection is `INSTANCE_FACE_SELECTED` on that instance.

- **HUD LOD** (12 November): HUDs always at LOD 3, linked children too, as OpenGL's
  `calcLOD()`. `followHudLods()` reads the self avatar's HUD points each frame: a prim
  below LOD 3 gets `updateLOD()` and is applied again; one applied before it counted as a
  HUD is applied again as one. Cause found by log: worn at login, meshes stayed at LOD 0
  (a panel's lowest LOD is one triangle).
- **World view** (12 November): the scene is made at the world view's size and placed at
  its offset in the window (`FrameInput::worldView`, composite viewport, `push.list`
  offset), as OpenGL's viewport; HUDs and outlines draw in the same viewport. Picking,
  world and HUD, then agrees with what shows. Metal: not yet, the scene fills its window.
- **Floating text** (`llSetText`, world and HUD): to be drawn by the viewer's own
  `render_hud_elements()` through the UI capture, after the HUD objects and their
  outline, before the 2D UI, with no HUD depth; with the 3D UI (`render_ui_3d()`).
- **Texture animation** (`llSetTextureAnim`): not drawn natively yet, world or HUD. Own
  step: each face's animation and start time in its material, UVs computed in the vertex
  shader from the frame's time; picking keeps the viewer's CPU path.
- **Web pages on faces**: dynamic textures updated by sub-images, which the texture
  bridge already carries; checked only if a web face stays white.

## OpenGL selection drawing (`llselectmgr.cpp`, `pipeline.cpp`)

- `LLSelectNode::renderOneSilhouette()`: visible edges as a strip fading outward with a
  scrolling silhouette texture; hidden parts, when `RenderHiddenSelections` is on, as
  additive lines at 0.4 alpha behind the scene depth. Colours: `SilhouetteParentColor`,
  `SilhouetteChildColor` (root and linked children).
- Face tool: `LLPipeline::renderSelectedFaces()` tints selected faces with the face
  select texture. Beacon highlights (`mHighlightFaces`) stay in P5.

## Design

The scene graph (`asrscenegraph.cpp`) is GPU driven for one view (`GpuSceneView`): GPU
cull, visibility buffer, resolve, Mac OIT. HUDs fit none of it: unlit, back-to-front
sorted, after tone mapping, a few hundred faces. So:

- **HUD pass**: its own small forward path after tone mapping, before the UI list. The
  CPU lists HUD faces sorted as OpenGL sorts them (opaque first, then alpha back to
  front along the HUD camera's X), drawn with an unlit shader from the scene store's
  vertex buffers and textures, into a cleared HUD depth. HUD instances stay out of the
  main cull.
- **Selection outline**: the CPU sends the selected instances (few) with their colour.
  A mask pass draws only those into an R8 target, comparing with the scene depth to
  mark each pixel visible or hidden; one edge pass on the mask draws the outline,
  fading outward, into the display colour after tone mapping, before the HUD pass;
  hidden parts fainter when `RenderHiddenSelections` is on. HUD selections draw the
  same way inside the HUD pass with the HUD camera.
- **Face tool**: selected faces drawn again in the mask pass's view with the face select
  texture tint, as `renderSelectedFaces()`.

## As built (12 November, in progress)

- Instance flags (`asrscene.h`, `asrSceneCommon.glsl`): `INSTANCE_HUD` (64),
  `INSTANCE_SELECTED` (128, root colour), `INSTANCE_SELECTED_CHILD` (1 << 18),
  `INSTANCE_FACE_SELECTED` (1 << 19); all in `INSTANCE_DESC_FLAGS`, so the viewer sets
  them through ordinary instance updates. `chosenByView()` in the cull skips HUD instances.
- Selection list: `cullInstance()`, early phase, after the frustum test and before
  occlusion, calls `addSelected()`: every cluster of the chosen LOD goes to
  `GpuSceneView::selection` (capacity `selectionEnd` = `SceneStore::selectionCapacity()`,
  the selected instances' finest cluster counts), counted in `GpuSceneCounts::selection`
  (one indirect draw, 64-triangle size), and skinning is requested.
- HUD list: `SceneStore::hudList()`, opaque in id order then blended by
  `InstanceUse::hudDepth` (rows[0][3]) far to near, finest LOD's clusters.
- `GpuSceneView` gained `selectionEnd`, `selection`, `hiddenSelections`, `outlineWidth`,
  `selectionColors[3]` (`SELECTION_COLOR_ROOT/CHILD/FACE`); 752 bytes.
- Shaders: `asrOverlayV` (list entry per instance from `push.visible` + `push.list`),
  `asrOverlayMaskF` (RGBA8 MAX blend: R visible 1 / hidden 0.5, G colour root 1 / child
  0.5, B face tint, A HUD phase colour; scene depth at set 1 binding 3),
  `asrOverlayHudF` (unlit, cutout, feedback), `asrOverlayOutlineF` (fullscreen, mask at
  set 1 binding 10, nearest selected pixel within `outlineWidth`, fades outward, hidden
  at 0.4). All compile with glslang.
- Frame inputs: `OverlayInput` (`asrdevice.h`) in `FrameInput` and `FrameState`: HUD
  shown and its view-projection, outlines, face highlights, hidden selections, width,
  colours.
- Frame graph: results `selection list`, `selection mask`; pass `selection mask` after
  Mac OIT, its sink on only while something is selected and shown.
- Vulkan (`asrdevicevulkan.cpp`, `Overlay`, `prepareOverlay()`, `passSelectionMask()`,
  `recordOverlays()`): the window image's single rendering splits around an overlay
  rendering (colour loaded, HUD depth cleared): scene outline, HUD opaque, HUD blended,
  HUD outline; then the UI. HUD texture feedback lands after the graph, behind a
  barrier.
- `asrenderer_test --scene --overlay`: two moving objects selected (root, child), the
  swapping knot selected and face-tinted, two HUD knots (opaque selected, blended).
  Windows: builds, selftest passes, validated audit passes, no validation errors;
  screenshot shows world and HUD outlines, the HUD knots unlit over the scene.
- Viewer (unbuilt): `asrworld.cpp`: HUD volumes are drawn (`whyLeftOut()` keeps out
  rigged HUDs only), never bound to the avatar (`boundAvatar()`), in HUD space with no
  render origin (`fillTransform()`), at LOD distance 0 (`updateLods()`), flagged
  `INSTANCE_HUD`; `followSelection()` each frame re-applies prims whose selection mark
  (root or child, face bits in face mode) changed, and `selectionFlags()` adds the flags
  per face; `ASRWorld::fillOverlay()`, called from `ASRViewer::publishFrame()`, eases the
  HUD zoom, converts `get_hud_matrices()` to Y down and reversed depth, and fills the
  toggles and colours (face tint white at 0.3). `llselectmgr.h`: one tagged friend line
  (`ASRSelectionAccess`) for the private outline toggles.
- Not yet: hover highlights (`mHighlightedObjects`), focus, inspect and transient
  (context) colours; mesh selections use the same colours.
- To do: Metal device, UI list depth state for 3D UI, viewer build test, then remaining
  viewer hooks
  (`asrworld.cpp` HUD instances and selection flags, HUD zoom easing and matrices,
  `render_ui_3d()` in `display_native()`), `asrenderer_test` checks.
