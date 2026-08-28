# My Lights teleport region recovery

## Symptom

After teleporting to another region, viewer-local lights created by the **My
Lights** panel stop following the avatar.

## Cause

Each light is implemented as a viewer-local `LLVOVolume` created in the
agent's current `LLViewerRegion`. The object retains that region association.
During a teleport the old object may be killed with its region; if it survives,
`setPositionAgent()` still converts coordinates through the old region. Keeping
the same object therefore leaves the light dead or positioned using the wrong
region transform.

## Resolution

`ASLightRig::updateTransform()` now waits until the agent and self-avatar agree
on the destination region, then recreates a missing, dead, or old-region light
object in that region. `ASLightRig::create()` also releases a stale dead-object
reference before creating its replacement.

## Runtime verification

Build and start the viewer, enable a visible My Lights light, and teleport to a
different region. Once the destination avatar has loaded, verify that the light
and optional light beacon follow the avatar. Repeat with the My Lights floater
closed, because its idle callback intentionally remains active while hidden.
