# Avatar Undeform Investigation

## Summary

`Avatar > Avatar Health > Undeform Avatar` repairs an avatar skeleton left in an
incorrect pose by translation-based animations (historically called deformer
animations). It is not a texture rebake and does not alter the saved shape.

In this viewer, the command does three things:

1. Calls `LLVOAvatar::resetSkeleton(true)`, which rebuilds the local skeleton,
   restores appearance and attachment overrides, and stops the avatar's current
   animations.
2. Starts the animation in `FSUndeformUUID` (`44e98907-3764-119f-1c13-cba9945d2ff4`).
   This is the legacy undeformer animation. It is started without becoming the
   pose stand's tracked current pose.
3. Calls `updateVisualParams()` once more, although `resetSkeleton()` already
   does this internally.

Implementation locations:

- `indra/newview/llviewermenu.cpp`: `FSToolsUndeform`
- `indra/newview/llvoavatar.cpp`: `LLVOAvatar::resetSkeleton`
- `indra/newview/fspose.cpp`: `FSPose::setPose`
- `indra/newview/app_settings/settings.xml`: `FSUndeformUUID`

## Why It Exists

Animations can contain bone-position translations. A badly authored animation,
an avatar intended to reshape the skeleton, or a griefer object can leave bones
looking displaced after the content is removed. Changing shape or rebaking
textures does not necessarily cure this because the problem is animation and
skeleton state, not the wearable shape or baked textures.

The special undeformer animation is an older workaround: it supplies neutral
position keys intended to overwrite displaced joint positions. The later
viewer-side skeleton-reset mechanism rebuilds the skeleton directly and can
also stop animations.

## Can It Be Avoided?

Usually, yes. The adjacent `Reset skeleton and animations` command calls the
same `resetSkeleton(true)` operation and should be the normal repair. Relogging
also reconstructs avatar and animation state, but is more disruptive.

`Undeform Avatar` differs only by additionally starting the legacy undeformer
animation and issuing a redundant final visual-parameter update. Consequently,
the menu action, listener, setting, and special animation dependency could be
removed if AyaneStorm does not require compatibility with cases where that
extra animation succeeds after a plain reset.

Before removal, runtime-test malformed legacy deformers, Bento facial/body
translation animations, and avatars deliberately reshaped by animation. If the
source object or attachment continually restarts the deformer, neither reset is
permanent; detach/leave it or revoke its animation permission first.

## Reset Skeleton Versus Reset Skeleton And Animations

Both commands rebuild the skeleton, reapply the last received appearance,
restore attachment joint overrides, update visual parameters, and reconstruct
attachment points. The difference is the `reset_animations` argument.

`Reset skeleton` passes `false`. It preserves current animations and is the
least disruptive choice for a broken skeleton or attachment override. It may
not fix deformation caused by an animation because that animation remains
active and can immediately reapply its bone translations.

`Reset skeleton and animations` passes `true`. For the self avatar it also:

- stops current motions locally and sends stop requests to the region;
- clears simulator animation-state overrides;
- conditionally revokes region-wide trigger/override-animation permissions;
- restores the default stand and optional default Bento idle;
- reasserts Firestorm AO `Always` animations; and
- recreates the avatar physics motion controller.

Permission revocation is conditional on `RevokePermsOnStopAnimation`, which is
enabled by default. If revocation happens while seated, the command stands the
avatar. Thus this command is stronger for animation-caused failures, but not
universally better: it disrupts legitimate poses, furniture, scripted content,
and some AO state.

## Proposed Strong Avatar Reset

A separate, clearly labelled `Strong avatar reset` could make recovery more
deterministic without making the ordinary reset unnecessarily destructive:

1. Temporarily suppress the viewer AO so it cannot reassert animations during
   recovery.
2. Stop all local and simulator animations and clear animation-state overrides.
3. Preserve trigger-animation and override-animation permissions so healthy
   scripted attachments can resume after recovery. Permission revocation is a
   separate last-resort operation, not part of skeleton repair.
4. Rebuild the skeleton *after* animations have stopped rather than using the
   current rebuild-before-stop order. Do not call `resetAnimations()` here:
   its motion-cache flush intentionally restarts active motion instances,
   including motions still easing out after the stop request.
5. Preserve the existing physics controller and the baseline stand/Bento
   motions established by the stop path, then restore the AO's prior enabled
   state.
6. Optionally offer attachment refresh as a separate checkbox/action. Do not
   rebake textures by default because baking is unrelated to bone state.

This cannot guarantee a lasting repair while deforming content remains worn or
keeps reacquiring permission. An even stronger detach-and-rewear operation is
possible, but it should not be silently included: it is slow, can conflict with
RLV locks or pending COF operations, and changes outfit state.

Implementation should live in a new AyaneStorm-owned module. Only small,
ownership-tagged hooks should be added to shared `ll*` or `fs*` files.

## AyaneStorm Strong Reset Implementation

The test implementation is in `asavatarrecovery.cpp` and
`asavatarrecovery.h`. It is exposed as `Strong avatar reset...` under
`Avatar > Avatar Health` and as the `Strong Avatar Reset` Toybox command.

The action requires confirmation, checks that the avatar and current region
are fully loaded, prevents synchronous re-entry, and holds a strong avatar
reference for the operation. It pauses and later restores the AO's actual live
state (resetting its remembered motion to standing first), stops animations
locally and through the simulator while preserving script animation permissions,
stands if necessary, and rebuilds the skeleton. It does not rebake, detach,
refresh attachments, revoke permissions, or restart avatar physics.

### Physics restart chest impulse

Runtime testing found that both the existing reset-with-animations action and
the strong reset could make the chest sway left when recreating
`LLPhysicsMotionController`. Each newly constructed `LLPhysicsMotion` began at
normalized parameter position zero instead of the avatar's current physics
driver value. Its previous joint world position and several integrator values
were also not initialized before normal integration began. For breast
left/right physics this creates an artificial impulse and can temporarily leave
the chest visibly displaced.

Initializing the new controller from current driver and joint values reduced
but did not eliminate the visible impulse in runtime testing. Even a correctly
initialized new physical integrator can react to real movement between its
first samples. The deterministic solution is therefore not to recreate avatar
physics as part of strong skeleton recovery.

The skeleton rebuild retains the existing joint objects, so the active physics
controller's joint and visual-parameter references remain valid. The Firestorm
physics restart was an independent workaround for frozen physics, not a
requirement for resetting translated bones. The strong reset now preserves the
running physics controller, eliminating the recovery-induced breast impulse.

That independent workaround is exposed as `Restart avatar physics` under
`Avatar > Avatar Health`. It stops and removes only
`ANIM_AGENT_PHYSICS_MOTION`, waits for the pose to settle, and creates a fresh
controller. The normal activation branch initializes its integration history
from the current controller parameter and joint positions before calculating
physics. It does not stop animations, revoke permissions, or rebuild bones.

Runtime testing also showed that the brief rebuild/T-pose during strong reset
could be sampled by the still-running physics controller as violent avatar
acceleration. Strong reset now stops only the avatar physics motion for a
one-second stabilization window spanning the rebuild. The controller remains
allocated while normal animations settle, then its joint and integration
history is rebased and the same instance resumes from the settled pose. Other
animations continue normally.

The strong-reset confirmation now uses the standard persistent notification
ignore checkbox. Selecting `Do not show this confirmation again` and confirming
the reset causes subsequent menu and toolbar invocations to execute directly;
the notification can be restored through the viewer's usual ignored-dialog
settings.

### Stale animation state and incomplete joint resets

Runtime testing also found that translated hip, belly, or waist joints could
survive the strong reset until the user switched shapes. `stopCurrentAnimations()`
stops the motion instances and sends reliable simulator requests, but it leaves
`mSignaledAnimations` and `mPlayingAnimations` populated until a later animation
state message. The immediate `resetSkeleton()` rebuild calls
`processAnimationStateChanges()`, sees those stale signals, and restarts the
translation animation that was just stopped. When a later shape change rebuilds
the skeleton, the simulator state has caught up and the affected joints finally
return to their intended positions.

The strong reset now clears the self avatar's stale signaled state and
immediately reconciles the playing-motion map after sending stop and state-reset
messages. Only then does it rebuild the skeleton. It deliberately preserves
script animation permissions so healthy mesh-head and other attachment
animations can resume.
This is deliberately scoped to the explicit strong-recovery action instead of
changing global upstream animation-message behavior.

### Complete Bento skeleton coverage audit

`character/avatar_skeleton.xml` declares 133 bones: 26 base bones and 107
extended Bento bones. It also declares 26 collision volumes. The count is below
the motion system's 216-animated-joint capacity.

`LLAvatarAppearance::buildSkeleton()` does not use a legacy bone allowlist. It
walks every top-level skeleton node and `setupBone()` recursively visits every
child. For every one of the 133 base and Bento bones it assigns the XML
position, default position, rotation, scale, default scale, support type, end
point, skin pivot, and joint number. The same traversal reconstructs every
collision volume. This includes the extended spine, face, fingers, wings, tail,
groin, and hind-limb hierarchies.

The 55 attachment-point joints from `avatar_lad.xml` are handled separately by
`initAttachmentPoints()`, which the reset also calls. Attachment joints declare
themselves non-animatable, so they are not omitted animation targets.

Therefore the reset does not omit any standard Bento bone. A final joint
transform is not necessarily equal to its XML default: after reconstruction,
the viewer intentionally reapplies the current shape's skeletal distortions,
worn-mesh alternate-bind/joint overrides, baseline or newly restarted
animations, and avatar physics. Those are post-reset writers, not missing bones.

## References

- Second Life Wiki, Project Bento Resources and Information:
  <https://wiki.secondlife.com/wiki/Project_Bento_Resources_and_Information>
- Archived Firestorm menu documentation:
  <https://firestorm300.rssing.com/chan-63782702/article1372.html>
