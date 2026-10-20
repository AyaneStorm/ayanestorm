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

## No eye highlights (eye-specular mask, Shader backend)

### Behavior

The persistent **No eye highlights** checkbox (`ASLightRigNoEyeSpecular`)
removes My Lights specular highlights (catchlights) from the self avatar's
eyes. Diffuse illumination of the eyes is kept. The checkbox is disabled while
the backend is **LLVOVolume**, because those lights go through the stock light
loop mixed with world lights.

### Implementation

- `ASLightRigRenderer::render()` collects the world positions of `mEyeLeft`,
  `mEyeRight`, `mFaceEyeAltLeft`, and `mFaceEyeAltRight`, transforms them to
  view space, and uploads them as `as_eye_mask[4]` (`.w` = radius) plus
  `as_eye_mask_count` to `gDeferredLightProgram` and each
  `gDeferredMultiLightProgram` it uses.
- `class3/deferred/pointLightF.glsl` and `multiPointLightF.glsl` compute
  `asEyeSpecularScale(pos)`: 0 inside each sphere, smooth edge from 75% to
  100% of the radius. PBR `specPunc` and legacy specular are multiplied by it.
- The count is reset to 0 before unbinding, so stock lights sharing these
  programs are unchanged.
- Sphere radius: debug setting `ASLightRigEyeMaskRadius` (default 0.02 m).

No G-buffer tagging is needed; the mask is purely geometric. Mesh eyes work
when rigged to either eye joint pair.

### Limitations

- Deferred path only. Alpha-blended cornea shells rendered in the forward
  alpha pass still receive My Lights specular through the hardware-light
  slots (`appendForwardLights()`).
- Self avatar only.
- The unused eye-joint pair (legacy vs. Bento) still has a sphere; skin
  specular inside it is also removed. Reduce `ASLightRigEyeMaskRadius` if
  visible on eyelids.

### Runtime verification

Shader backend, a light in front of the face, close-up camera. Toggle the
checkbox: the eye highlight disappears, skin and eye diffuse remain. Check a
snapshot. Test with system eyes, and with legacy and Bento mesh eyes.

## Spot lights (LLVOVolume backend)

### Behavior

Each light has a **Spotlight** checkbox. A spotlight aims automatically at a
chosen avatar joint (**Aim at**, default `mNeck`) and follows it every frame.
Optional **Aim yaw** / **Aim pitch** offsets (degrees) turn the aim away from
the joint: positive yaw turns left as seen from the light, positive pitch tilts
up. **Field of view** (degrees in the UI, radians in `LLLightImageParams`),
**Focus**, **Ambiance**, and an optional **Projector image** match the build
floater's projector controls. With no image the cone is plain white
(`IMG_WHITE`).

The Shader backend stays point-only. With it selected the spot section is
hidden and the floater shrinks to its pre-spot height
(`updateSpotSectionVisibility()`, which computes the height from the section
header and keeps the top edge fixed). Spot settings are kept but ignored.

### Implementation

- In SL a light becomes a spot (projector) when it has a light image:
  `LLLightImageParams::isLightSpotlight()` is `mLightTexture.notNull()`.
  `ASLightRig::applyParams()` calls `setLightTextureID()` with the chosen image
  or `IMG_WHITE`, then `setSpotLightParams(fov, focus, ambiance)`. Turning the
  spot off clears the image, which restores a point light.
- `LLPipeline::setupSpotLight()` projects along the object's -Z axis using
  `getRenderRotation()`. Every frame, `ASLightRig::updateTransform()` computes
  that rotation in `computeSpotRotation()` and applies it with `setRotation()`:
  - -Z points from the light to the aim joint. If the joint is missing or sits
    on the light, it falls back to the anchor joint.
  - Image up follows world up, or world X when the aim is near vertical.
  - The pitch offset is applied about the light's X axis, then the yaw offset
    about its Y axis.
- The projector frustum also uses the object's scale. The light object is
  0.01 m, so the cone apex is effectively at the light position, like a tiny
  in-world projector prim.
- Spots are ordinary entries in `mNearbyLights`. They share the
  `RenderLocalLightCount` budget, go through the deferred projector pass, and
  with `RenderShadowDetail` 2 can be chosen as one of the two shadow-casting
  projectors.
- The light beacon draws an extra arm along the aim direction for spots. Its
  length is `0.4 + radius * intensity / 20` m, so it hints at the light's
  strength.
- Presets and autosave gain `spot`, `aim_joint`, `aim_yaw`, `aim_pitch`,
  `spot_fov`, `spot_focus`, `spot_ambiance`, and `spot_texture`. Older files
  without them load as point lights aimed at `mNeck`.
- Like the existing light setters, the image setters call
  `parameterChanged(..., local_origin=true)`, which sends an
  `ObjectExtraParams` for local id 0. The region ignores it.

### Runtime verification

LLVOVolume backend, beacons on. Add a light in front of the chest and enable
**Spotlight**: the beacon's aim arm should point at the neck, and a cone should
light the neck and face. Walk and animate; the cone should follow. Try other
**Aim at** joints, the yaw/pitch offsets, FOV, Focus, and Ambiance, and a
projector image. Then try None. Save and reload a preset, and relog to check
autosave. Load an older preset and confirm its lights are points. Switch to the
Shader backend: the spot section disappears, the floater shrinks with its top
edge fixed, and the lights become points. Switch back: the section and height
return. Close and reopen the floater in each backend to check the saved size.

## Fixed (placed) lights, per-light On, autosave on change

### Behavior

- **Placement** per light: **Follow avatar** keeps the original behavior (head,
  chest and back presets plus distance, height and azimuth). **Fixed** keeps the
  light at a world position, and the editor then shows:
  - **Place at camera**: the camera position.
  - **Place at camera focus**: `gAgentCamera.getFocusGlobal()`. After an
    Alt-click on an object this is the `calcFocusOffset()` point near the
    object's middle, not the surface hit. Without an Alt-click it is at the
    avatar. (The exact ray hit is on the `feat-local-light-subject-sort`
    branch.)
  - Placing does nothing when no loaded region contains the point
    (`LLWorld::getRegionFromPosGlobal()` matches region bounds only).
  - **Offset X/Y/Z**: meters from the placed (base) location along world
    east/north/up, -10 to +10, centered at 0. Placing again sets a new base
    and resets the offsets to 0. The home region is always the base's region,
    even if an offset reaches over a border. Saved as `fixed_offset`.
  - The home region name, with "(away)" while that region is not loaded.

  Switching a light from Follow to Fixed freezes it where it currently is. A
  light that was never positioned goes to the camera instead. There is no
  "place at avatar" for fixed lights.
- A fixed light belongs to its home region: grid id
  (`LLGridManager::getGridId()`), region name and global position.
  - It is **dormant** while that region is not loaded (teleported away,
    beyond draw distance): no object, no light, no beacon, nothing submitted
    to the Shader backend.
  - It is recreated in its region when the region loads again.
  - It only wakes in a region with the expected name on the expected grid,
    so global coordinates reused by another grid or a moved region never
    light the wrong place.
  - Crossing into a neighbour does not make it dormant. Its agent position
    is recomputed from the global position every frame.
- This gives one set of fixed lights per region with no extra mechanism. Each
  region's set wakes when you arrive. Following lights come with you everywhere.
- **On** (per light, saved as `on`) switches a single light without changing
  its settings. **Enable My Lights** stays a global override: a light emits
  only when both are on (`ASLightRig::isEnabled()`), and the master no longer
  overwrites the per-light state.
- **Lights list filter**: **Active here** lists worn (following) lights and
  fixed lights placed in the agent's region. Fixed lights in loaded neighbours
  still shine but are listed only under **All lights**, which shows every light,
  with dormant ones tagged "(away: Region)". "(off)" marks lights switched
  off. The list refreshes when the agent's region changes, including walking
  across a border, and when any light's dormancy changes.
- **After a teleport** (`LLAgent::getTeleportState()` returning to
  `TELEPORT_NONE`), the filter switches to **Active here**, the list is
  rebuilt for the arrival region, and its first light (if any) is selected.
- **Clear region** deletes the fixed lights placed in the agent's region.
  **Clear away** deletes all dormant lights. Both ask for confirmation first
  (`GenericAlertYesCancel`).
- **This region only** preset: saves `{region, grid, lights}` holding the
  current region's fixed lights. Loading it replaces only that region's fixed
  lights. A normal preset (an array) still replaces every light. A region preset
  loaded elsewhere adds dormant lights that wake in their region.
- **Autosave on change**: every edit marks the rig dirty, and `updateLights()`
  writes the per-account autosave after 2 s without further changes. Quit
  still saves too. A crash no longer loses edits made during the session.

### Implementation notes

- `ASLightRig::getTargetRegion()`: the agent's region when following, the home
  region when fixed. `create()` and the recreate check in `updateTransform()`
  both use it, so a fixed light's object lives in its own region even while
  the agent stands in a neighbour.
- Dormancy is evaluated before the enabled check, so switched-off fixed
  lights still show the right away state.
- New LLSD keys: `on`, `placement` (`follow`/`fixed`), `pos_global`, `region`,
  `grid`. Older files load as following lights that are on.
- Spot lights work in both placements. A fixed spot still aims at a joint of
  your avatar, which makes it a stand light that tracks you.

### Runtime verification

1. Add a light, set Fixed, and use **Place at camera** and **Place at camera
   focus**. Then nudge it with Offset X/Y/Z (0 = placed spot, +/-10 m), and
   place it again to check the offsets reset to 0.
2. Set the filter to All lights and teleport away: the light disappears, the
   filter switches to Active here, and the list shows only worn lights and
   lights placed in the arrival region, with the first one selected. Under
   All lights the placed light shows "(away: Region)". Teleport back: it
   returns.
3. Place a second set in another region and teleport between them: each set
   should wake only in its own region.
4. Toggle **On** for one light, then toggle **Enable My Lights**: the light
   that was off stays off.
5. Try **Clear region** and **Clear away**, both cancelling and confirming.
6. Save a **This region only** preset, delete the region's lights, and load it.
   Lights in other regions should be untouched.
7. Edit a light, wait a few seconds, and kill the viewer: after relogging, the
   edit is kept.
8. Repeat steps 1–2 with the Shader backend.

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

## Isolate background: crowded avatars and floating text

### Symptoms

With **Show other avatars** disabled in a crowded club, isolate mode became
substantially slower. Object hover text and attachment-owned text also remained
visible after their source geometry was hidden.

### Causes

Hidden avatar bodies and attachments were restored during shadow passes and
hidden again for the main pass. This repeatedly dirtied and rebuilt large
attachment geometry sets. Floating `LLHUDText` follows a separate post-scene
render path and never reaches drawable state sorting.

### Resolution

Non-self resident and animesh content now remains hidden during shadow passes
when excluded by the isolate allowlist. Ordinary scenery still participates in
shadows so rooms and roofs can shade the self avatar.

`ASBackgroundIsolate::shouldHideHUDText()` applies the matching source-object
policy at the narrow `LLHUDText::render()` entry point: object-owned and
source-less world text is hidden; self text is allowed; other resident text
follows **Show other avatars**; control-avatar text remains hidden. HUD
attachment text is unaffected.

### Runtime verification

Compare FPS with **Show other avatars** on and off in a crowded location. Test
object `llSetText`, attachment text, and resident avatar text in Normal Scene
and solid backgrounds. Confirm world text follows the allowlist while HUD UI
remains unchanged.

## Isolate background: legacy tree leak and render-type coverage

### Symptom

With **My Lights > Background** set to a solid color such as black, legacy
Second Life trees and bushes could remain visible while the rest of the world
was isolated.

### Cause

Legacy vegetation represented by `LLVOTree` renders through
`LLPipeline::RENDER_TYPE_TREE`. This dedicated geometry path does not reliably
honor `LLDrawable::FORCE_INVISIBLE`. The background isolate render-type list
already disabled other world-only paths, including grass, but omitted trees.

### Resolution

Include `RENDER_TYPE_TREE` in `ASBackgroundIsolate`'s isolate render-type list.
Entering isolate mode now disables legacy trees, and leaving it restores their
previous render-type state through the existing symmetric toggle logic.

### Render-type coverage audit

The isolate list now contains every dedicated world-only render type that must
be suppressed directly:

- `RENDER_TYPE_TERRAIN`
- `RENDER_TYPE_WATER`
- `RENDER_TYPE_VOIDWATER`
- `RENDER_TYPE_WATEREXCLUSION`
- `RENDER_TYPE_CLOUDS`
- `RENDER_TYPE_TREE`
- `RENDER_TYPE_GRASS`
- `RENDER_TYPE_PARTICLES`

Other render types are intentionally not added:

- Volume material pools and passes (`SIMPLE`, `ALPHA`, `ALPHA_MASK`,
  `FULLBRIGHT`, `BUMP`, `MATERIALS`, `GLTF_PBR`, `GLOW`, and their pass and
  rigged variants) are shared by scenery and the user's attachments.
  `ASBackgroundIsolate::updateDrawableHiddenState()` filters their underlying
  volume drawables, and `LLVolumeGeometryManager::rebuildGeom()` excludes
  drawables marked `FORCE_INVISIBLE` from all those batches.
- `RENDER_TYPE_AVATAR` and `RENDER_TYPE_CONTROL_AV` are shared with the self
  avatar and attachment/animesh paths. They are filtered per drawable so the
  self avatar remains visible.
- `RENDER_TYPE_SKY` and `RENDER_TYPE_WL_SKY` remain enabled deliberately.
  Disabling them interferes with atmosphere and reflection-probe updates; the
  final isolate shader covers their pixels instead.
- `RENDER_TYPE_HUD` and `RENDER_TYPE_HUD_PARTICLES` belong to the separate HUD
  render path and are not part of the photographed world background.

Thus no additional current `LLPipeline::LLRenderTypeMask` value should be
added to the global isolate list. Debug overlays and viewer UI are controlled
by debug-feature masks and UI rendering, not by scene render types.

### Runtime verification

Set **My Lights > Background** to **Black** in a region containing legacy trees
or bushes. Confirm that vegetation disappears immediately and returns when the
background mode is set to **None**.
