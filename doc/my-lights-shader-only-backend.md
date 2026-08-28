# My Lights shader-only backend option

## Goal

Replace the viewer-local `LLVOVolume` used by each My Lights light with a
synthetic light submitted directly to the existing deferred local-light
renderer. Environmental illumination is intentional, so no avatar-only mask
is required.

## User-visible compatibility

The current panel and saved format can remain unchanged. Each light still has:

- a unique ID and editable name;
- an avatar anchor joint, distance, height, and azimuth;
- color, intensity, radius, and falloff;
- an enabled state;
- named-preset and autosave serialization.

Adding, removing, editing, enabling, and saving lights would continue to work
as they do now. The beacon can use the same calculated light position without
depending on a viewer object.

## Proposed rendering path

Keep `ASLightRig` as the CPU-side configuration and joint-tracking class, but
replace its `LLVOVolume` member with a lightweight synthetic-light description.
Once per frame, calculate enabled light positions from the current avatar joint
transforms and submit position, color, intensity, radius, and falloff to the
deferred local-light renderer. Render bounded light volumes and batch compatible
lights where practical; avoid a separate fullscreen pass for every light.

## Efficiency

The affected-pixel GPU lighting cost should be similar to an ordinary local
light because both approaches ultimately execute a local-light shader. Direct
submission removes the extra CPU and scene-management costs of creating and
maintaining `LLVOVolume`, drawable, spatial-group, culling, sorting, geometry,
and region-lifecycle state. It also removes teleport recreation and ghost-object
concerns.

For one or two lights the measured FPS difference may be small, especially with
large radii where fragment shading dominates. Direct submission should scale
better as the number of My Lights lights grows.

## Constraints

- Joint-derived positions must still be updated each frame.
- Transparent surfaces retain the viewer's existing local-light limitations.
- Shadow-casting lights would require a separate, substantially more expensive
  design.
- This is a larger pipeline change than the current viewer-object approach and
  should be implemented as an AS-owned module with narrowly tagged call-outs in
  shared pipeline files.

## Recommendation

A shader-submitted synthetic local-light backend is the cleaner long-term
architecture. Preserve the current `ASLightRig` data model and UI so the change
is limited to position output and renderer submission rather than user-facing
behavior or preset compatibility.
