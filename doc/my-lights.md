# My Lights

Research notes and implementation history for the **My Lights** floater
(`ASFloaterMyLights`, `as_my_lights`), the self-only photography lighting
system that manages viewer-local lights following the avatar, an isolate
background mode, an animation freeze toggle, and a light-position beacon.

## Backend options: LLVOVolume vs. shader-submitted lights

### Goal

Replace the viewer-local `LLVOVolume` used by each My Lights light with a
synthetic light submitted directly to the existing deferred local-light
renderer. Environmental illumination is intentional, so no avatar-only mask
is required.

### User-visible compatibility

The current panel and saved format can remain unchanged. Each light still has:

- a unique ID and editable name;
- an avatar anchor joint, distance, height, and azimuth;
- color, intensity, radius, and falloff;
- an enabled state;
- named-preset and autosave serialization.

Adding, removing, editing, enabling, and saving lights would continue to work
as they do now. The beacon can use the same calculated light position without
depending on a viewer object.

### Proposed rendering path

Keep `ASLightRig` as the CPU-side configuration and joint-tracking class, but
replace its `LLVOVolume` member with a lightweight synthetic-light description.
Once per frame, calculate enabled light positions from the current avatar joint
transforms and submit position, color, intensity, radius, and falloff to the
deferred local-light renderer. Render bounded light volumes and batch compatible
lights where practical; avoid a separate fullscreen pass for every light.

### Efficiency

The affected-pixel GPU lighting cost should be similar to an ordinary local
light because both approaches ultimately execute a local-light shader. Direct
submission removes the extra CPU and scene-management costs of creating and
maintaining `LLVOVolume`, drawable, spatial-group, culling, sorting, geometry,
and region-lifecycle state. It also removes teleport recreation and ghost-object
concerns.

For one or two lights the measured FPS difference may be small, especially with
large radii where fragment shading dominates. Direct submission should scale
better as the number of My Lights lights grows.

### Constraints

- Joint-derived positions must still be updated each frame.
- Transparent surfaces retain the viewer's existing local-light limitations.
- Shadow-casting lights would require a separate, substantially more expensive
  design.
- This is a larger pipeline change than the current viewer-object approach and
  should be implemented as an AS-owned module with narrowly tagged call-outs in
  shared pipeline files.

### Recommendation

A shader-submitted synthetic local-light backend is the cleaner long-term
architecture. Preserve the current `ASLightRig` data model and UI so the change
is limited to position output and renderer submission rather than user-facing
behavior or preset compatibility.

### A/B implementation

The My Lights panel now exposes a persistent **Backend** combo box:

- **LLVOVolume** retains the original viewer-local object implementation and
  remains the default.
- **Shader** destroys the rig's viewer objects and submits cached light data
  directly through the stock deferred point-light shaders.

Changing the combo applies immediately without altering the rig list or saved
presets. Compare opaque-surface appearance, FPS, teleport behavior, and light
editing between both modes.

The experimental shader backend targets the deferred opaque lighting pass and
also populates the same hardware-light uniforms used by forward-rendered
transparency. Synthetic lights reserve the available local slots before normal
nearby lights, ensuring My Lights remains available to alpha hair and clothing
without viewer objects. It is still absent from the optional volumetric
local-light collector, which currently discovers lights through pipeline
drawables. That difference should be checked before choosing a permanent
backend.

## Direct-backend transparent lighting fix

### Symptom

The direct shader backend initially produced darker alpha hair and other
forward-rendered transparent objects than the `LLVOVolume` backend. Opaque skin
and scene surfaces were substantially equivalent.

### Cause

Direct lights were submitted only to the deferred point-light pass.
`LLVOVolume` lights also enter `LLPipeline::mNearbyLights`, from which
`LLPipeline::setupHWLights()` fills local hardware-light slots 2 through 7.
Forward transparency shaders consume those uniforms, so they could not see the
viewer-object-free lights.

### Resolution

`ASLightRigRenderer::appendForwardLights()` now configures the same hardware
light state for direct lights, matching the stock point-light calculations for
linear color and intensity, position, adjusted radius, falloff, linear and
quadratic attenuation, size, and omnidirectional flags. Direct My Lights reserve
the available local slots before ordinary nearby lights so alpha content cannot
lose them merely because no drawable participates in nearby-light sorting.

### Runtime verification

Compare both backends without moving the camera or changing the rig. Pay
particular attention to alpha hair strands, lashes, sheer clothing, jewelry,
and opaque skin adjacent to them. Their local-light response should now closely
match. Ordinary nearby lights can differ when more than six local lights compete
for forward slots because direct My Lights are deliberately reserved first.

## Teleport / region-change recovery (LLVOVolume backend)

### Symptom

After teleporting to another region, viewer-local lights created by the **My
Lights** panel stop following the avatar.

### Cause

Each light is implemented as a viewer-local `LLVOVolume` created in the
agent's current `LLViewerRegion`. The object retains that region association.
During a teleport the old object may be killed with its region; if it survives,
`setPositionAgent()` still converts coordinates through the old region. Keeping
the same object therefore leaves the light dead or positioned using the wrong
region transform.

### Resolution

`ASLightRig::updateTransform()` now waits until the agent and self-avatar agree
on the destination region, then recreates a missing, dead, or old-region light
object in that region. `ASLightRig::create()` also releases a stale dead-object
reference before creating its replacement.

### Runtime verification

Build and start the viewer, enable a visible My Lights light, and teleport to a
different region. Once the destination avatar has loaded, verify that the light
and optional light beacon follow the avatar. Repeat with the My Lights floater
closed, because its idle callback intentionally remains active while hidden.

## Login restoration (autosave)

### Symptom

Enabled autosaved lights do not appear after login until the user opens the
My Lights floater.

### Cause

`LLFloaterReg` constructs registered floaters lazily. Autosave loading and the
My Lights idle callback are initialized by `ASFloaterMyLights::postBuild()`, so
neither runs before the floater's first construction.

### Resolution

The completed-login cleanup state now calls
`LLFloaterReg::getInstance("as_my_lights")`. This constructs the instance without
showing it, loads the per-account autosave, and registers the idle callback after
the account path and self-avatar are available. If avatar creation is unusually
late, the idle callback creates enabled lights once the avatar becomes valid.

### Runtime verification

Enable at least one light, quit normally so the rig autosaves, then log in
without opening My Lights. Verify that the light affects the scene after the
avatar appears. Open the floater afterward and verify that its list and controls
match the restored lights.

## Isolate background: FPS cost

### Symptom

FPS can be lower when **My Lights** uses a solid background instead of
**Normal scene**, even though most scene geometry is hidden.

### Primary cause

Isolate mode adds expensive CPU and geometry-management work in
`LLPipeline::stateSort()`:

- Occluded spatial groups are treated as visible so their drawable hidden
  states cannot become stale.
- Every drawable in each visible spatial group is state-sorted every frame,
  bypassing the normal `changeLOD()` gate.
- `ASBackgroundIsolate::updateDrawableHiddenState()` changes
  `LLDrawable::FORCE_INVISIBLE` and dirties/requeues the owning spatial group
  whenever the desired hidden state changes.
- Shadow passes deliberately reveal scene geometry so it can still cast
  shadows onto the avatar. The main-camera pass hides it again. Consequently,
  drawable flags can toggle and spatial groups can be rebuilt repeatedly
  between shadow and main-camera passes.

This work scales with the surrounding scene's drawable and spatial-group
count. Hiding the final pixels therefore does not necessarily make the frame
cheaper.

### Secondary GPU costs

Active isolate mode also adds two full-screen background shader passes: an HDR
base layer before transparency and a final exact-color pass after post
processing. ExactOIT or AVBOIT additionally performs an isolate-only depth pass
for transparent-pixel coverage. These costs scale mainly with display
resolution and are likely secondary to repeated scene traversal and geometry
rebuilds in content-heavy regions.

### Profiling direction

Compare CPU time in state sorting and spatial-group geometry rebuilding between
Normal scene and isolate mode. Also compare with shadows disabled and with a
low-content region. A disproportionately large improvement from either test
would confirm the traversal/rebuild path as the main bottleneck.

## Isolate background: newcomer avatars stay visible

### Symptom

Avatars arriving after a solid My Lights background is enabled can remain
visible. Toggling My Lights or switching through Normal Scene does not reliably
remove them.

### Cause

The live isolate filter correctly classified the avatar and set
`LLDrawable::FORCE_INVISIBLE`. That flag is sufficient for volume geometry only
after its spatial-group batch is rebuilt. Avatar drawables are non-volume:
`LLPipeline::stateSort(LLDrawable*, LLCamera&)` skipped their `setVisible()` call
when the flag was present, but then continued and enqueued their faces later in
the same function. A newly arrived avatar could therefore remain renderable
despite having the correct hidden flag.

### Resolution

`ASBackgroundIsolate::updateDrawableHiddenState()` now requests an immediate
state-sort return only when the current drawable is both hidden and an avatar.
Volume and other drawable types retain their original state-sort bookkeeping
after receiving the same flag and group rebuild. Avatar faces can no longer
proceed to face enqueueing. Shadow passes continue to receive `false` because
the isolate module deliberately restores hidden geometry while rendering
shadows.

### Runtime verification

Enable a black, white, or custom background in a populated region and wait for
previously unseen avatars to arrive. Confirm that neither their bodies nor their
attachments appear. Repeat after switching through Normal Scene, and confirm
that all avatars return in Normal Scene while newcomers remain hidden after
isolate mode is re-enabled.

## Isolate background: "Show other avatars" option

### Behavior

The persistent **Show other avatars** checkbox controls the isolate-background
allowlist:

- Off (default): only the self avatar, self attachments, and My Lights light
  objects are allowed.
- On: other resident avatar bodies and their attachments are also allowed;
  unrelated scenery and animated-object control avatars remain hidden.

The setting has no visual effect in Normal Scene because the isolate filter is
inactive there.

### Live updates

Changing the option re-evaluates drawables already tracked as hidden. This
allows other avatars immediately without restoring the hidden environment and
without requiring a general refresh button. Turning the option off relies on
the existing per-frame live filter, which hides visible avatars on their next
state-sort traversal and hides newcomers before their faces are enqueued.

### Runtime verification

Enable a solid background with several avatars nearby. Toggle **Show other
avatars** on and confirm their bodies and attachments appear while the scene
stays isolated. Toggle it off and confirm they disappear. Repeat while another
avatar arrives or changes attachments.
