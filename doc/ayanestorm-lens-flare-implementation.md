# AyaneStorm Lens Flare Implementation

## Scope

The first implementation renders viewer-local lens flares for the EEP sun and
moon. It is controlled by `ASLensFlareEnabled` and defaults to off.

The controls live in the standalone **Camera Effects** floater, opened from
AyaneStorm Preferences > Rendering. Its generic panel is intended to host
additional viewer-local optical effects later. The `as_camera_effects` command
also exposes the floater in the toolbar toybox, using an original 18-pixel
source-and-aperture-ghost icon maintained as SVG and rasterized to RGBA PNG.

`ASLensFlareStrength` scales the complete ghost pass from invisible at zero to
the authored intensity at one. `ASLensFlareSaturation` interpolates between a
neutral ghost mask tinted solely by the current celestial light color at zero
and the authored chromatic aperture colors at one. Both controls apply live.
The effect defaults to disabled, with strength `0.35` and saturation `0.60`.

## Rendering design

The Aurora work established the project pattern used here: an independent
AyaneStorm module owns its shader object and lifecycle, while small ownership-
tagged hooks register, load, unload, and draw it from the upstream renderer.

Unlike Aurora, a lens flare is a camera artifact rather than sky geometry. The
pass therefore runs after tone mapping and antialiasing, before UI overlays. It
additively draws a full-screen triangle and samples the deferred depth buffer at
the projected celestial source. A small cross-shaped depth test makes the flare
fade when geometry occludes the source.

The GLSL keeps the main visual vocabulary and constants of `lensflares.shader`:
procedural radial noise, a chromatic burst, circular/hexagonal aperture ghosts,
and ghost placements along the source-to-screen-center axis. The remaining
Shadertoy inputs and synthetic moving light were replaced with viewer uniforms.
The provided `iChannel0.png` drove the procedural source burst in the reference;
it is no longer sampled after removal of that source-local component.

Because the viewer composites the effect over an already tone-mapped scene,
reference-strength glare would clip into a large white source and interact
poorly with partial geometry occlusion. The source burst and source-centered
halo are therefore omitted. The effect retains only the displaced aperture
ghost chain along the source-to-screen-center axis, leaving the sun/moon disc
and its immediate surroundings to the sky renderer.

## Deliberate limitation: local lights

Dynamic local lights are not included in this first pass. Supporting "any
strong source" requires a bounded selection policy (brightness, projected size,
distance, and maximum source count) and access to the pipeline's per-frame light
set. Adding that casually would make the full-screen pass scale with scene light
count and could produce unstable popping. It should be a follow-up with an
explicit source budget and performance testing.

## Runtime verification

Build and test on Windows with the checkbox enabled. Verify sun and moon flares
near the center and edges of the screen, complete disappearance behind opaque
geometry, no flare in cube/reflection captures, and correct behavior with HDR,
FXAA/SMAA, DOF, and each OIT mode.
