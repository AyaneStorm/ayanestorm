# AyaneStorm Special / Firestorm Poser Audit

## Scope

Compared the current `ayanestorm-special` tree with the local
`.phoenix-firestorm-master` reference. Four poser files differ:

- `indra/newview/fsfloaterposer.cpp`
- `indra/newview/fsposeranimator.cpp`
- `indra/newview/fsposeranimator.h`
- `indra/newview/skins/default/xui/en/floater_fs_poser.xml`

This is a source review. Per repository instructions, no build was attempted.

## Implementation status

The functional AyaneStorm-special issues identified below were addressed on 2026-09-21:

- native XML saving always writes full target rotations while preserving only
  poser-authored position and scale deltas, avoiding duplication of avatar
  shape transforms;
- full BVH export now uses base-plus-delta rotations without zeroing live bases;
- BVH export uses the original AyaneStorm-special full target rotations and
  target root position without synthetic per-joint rotations;
- BVH icons distinguish meaningful user deltas while showing all full-pose joints as locked;
- every live, non-muted avatar and animesh loaded by the viewer can be listed,
  regardless of region or ownership;
- unused full-scale support, duplicated XUI, and malformed special ownership tags were removed.

The inherited Firestorm behavior that ignores the BVH writer's return value was
intentionally left unchanged for separate upstream work. Runtime verification
is still required. The defensive fix for Firestorm's base-zeroing manipulator
bug and the existing permission-helper structure were retained to minimize
unrelated churn.

## Summary

The other-avatar/animesh changes achieve local posing for loaded characters,
but the 256 m radial filter can still omit avatars in the same region.

The direct BVH change correctly exports the target (base plus poser delta)
instead of only the poser delta. However, it bypasses Firestorm's joint-lock
safeguard, ignores the pelvis-lock preference, and mixes unrelated changes to
the native XML pose format into the BVH feature. The implementation should be
simplified before it is considered complete.

## Findings

### High: neutral joints can disappear from a supposedly full BVH pose

`writeBvhMotion()` now calls `getFullJointRotation()` directly. This bypasses
`getJointExportRotation()`, which inserts a small non-zero rotation when a joint
would otherwise be discarded by the BVH importer.

The local importer compares frame 2 with the zero-valued first frame and drops
rotation tracks below `ROTATION_MOTION_THRESHOLD`. Consequently, a joint whose
full target rotation is identity or nearly identity is not locked by the
uploaded animation. Another animation can then move it, so the result is not a
complete pose.

Recommended change: add a full-target export method which applies the same
threshold test and epsilon fallback to `getTargetRotation()`. In full-pose mode,
apply that safeguard to every intended skeletal joint, not only joints whose
base was explicitly zeroed.

### High: native pose files were changed unnecessarily and inconsistently

The stated requirement concerns BVH output, but `savePoseToXml()` is forced to
write `startFromTeePose=true`, and `tryGetJointSaveVectors()` replaces the saved
rotation delta with the full target rotation.

This changes Firestorm's native pose/diff behavior and discards its saved
motion-state mechanism. It is also not a consistently full transform:
rotation is base plus delta, while position and scale remain only their public
deltas. The resulting file is labelled and loaded as a full pose despite
having mixed semantics.

Recommended change: revert the XML serialization, load-label, save-label, and
duplicated XUI button changes. Full-pose logic should exist only in the BVH
export path unless full native pose files are a separate explicit feature. If
that feature is wanted, it needs a new file version/mode and full target
position and scale handling as well.

### Medium: the pelvis option is ignored

`writeBvhMotion()` reads `FSPoserPelvisUnlockedForBvhSave` into
`lockPelvisJoint`, but the variable is no longer used after replacing
`getJointExportRotation()` with `getFullJointRotation()`.

The checkbox therefore has no effect. The full-target export method should
retain the existing pelvis-specific lock/unlock behavior, with clearly named
boolean variables because the current setting name and old argument are
inverted.

### Medium: BVH status icons no longer describe the exported result

`jointHasUserRotationDelta()` tests the public delta for exact quaternion
inequality. Full BVH export instead writes the target rotation, which includes
the captured base. An unedited joint with a non-zero base may be exported and
locked while the UI reports it as unlocked. Conversely, floating-point noise
can mark a tiny delta as edited even when the importer drops it.

Recommended change: calculate the icon from the same full-target threshold
logic used by BVH export. Avoid exact quaternion equality.

### Medium: BVH write failure is reported as save success

`doPoseSave()` determines success only from `savePoseToXml()`. If BVH output is
enabled, it calls `savePoseToBvh()` but ignores its return value and displays
the success overlay anyway. This is inherited Firestorm behavior, but becomes
more important when BVH is the target deliverable.

Recommended change: report XML and BVH results independently, or include the
BVH result in the overall success state when BVH output is enabled.

### Medium/conditional: the 256 m test does not mean every avatar in-region

The list now includes ordinary avatars and unowned control avatars, and the
permission gates allow starting and manipulating their local posing motions.
That is sufficient for client-side posing of loaded characters.

However, `avatarIsNearbyMe()` uses a 256 m radius. Two avatars can be in the
same 256-by-256 m region but farther apart diagonally, so some same-region
avatars remain excluded. `couldAnimateAvatar()` and `isAvatarSafeToUse()`
already enforce the same-region requirement.

Recommended change: if the goal is every loaded avatar/animesh in the current
region, remove the distance test and use the existing region check. If 256 m
proximity is intentional, document that narrower goal.

### Low: redundant and dead additions

- `getFullJointScale()` is unused; BVH has no scale channels.
- The preservation changes in `updateJointRotationFromManip()` are now
  unreachable through the floater because both callers always pass
  `resetBaseRotationToZero=false`. The fix itself is defensively sound, but it
  is not required by the current full-export design.
- Both permission helpers now return true for every live avatar. Keeping two
  separate permission concepts is misleading and redundant for this fork.
- Explicit `enabled="true"` on the BVH checkbox is unnecessary unless a parent
  control actually disables it.
- `getFullJointPosition()` and `getFullJointRotation()` would be clearer as
  `getJointTargetPosition()` and `getJointTargetRotation()` because that is the
  exact state they return.

### Low: ownership tags and comments need cleanup

Several closing tags are malformed as `<//AS:Chanayane>` or repeat an opening
tag instead of using `</AS:Chanayane>`. The XUI edits use `[AS:chanayane]`
rather than the repository ownership-tag convention. Header comments for the
full position and scale getters incorrectly say "full join rotation".

The replacement XUI save button duplicates the complete upstream control just
to change its label and width. That enlarges future merge conflicts; a minimal
tagged attribute change is preferable if the native full-pose behavior is
retained.

## Changes that are justified

- Listing ordinary avatars and unowned control avatars.
- Allowing the local posing motion and manipulators to target them.
- Exporting `FSJointPose::getTargetRotation()` and
  `FSJointPose::getTargetPosition()` for a full BVH snapshot.
- Passing `false` for base-zeroing while editing when BVH output is enabled;
  export should not mutate the live posing state.
- Updating the BVH tooltip to remove the obsolete claim that the base must be
  zeroed.

## Recommended minimal design

1. Keep Firestorm's native XML pose/diff behavior unchanged.
2. Keep the expanded avatar/animesh listing and local permission behavior.
3. Replace the generic full getters with one BVH export-rotation method based
   on `getTargetRotation()`, including threshold/epsilon and pelvis handling.
4. Export the full target pelvis position for BVH; keep XML position and scale
   as poser-authored deltas.
5. Drive BVH status icons from the exact export decision.
6. Remove unused scale support and dead UI duplication.
7. Treat a requested BVH write failure as a visible failure.
8. Fix ownership tags and comments.

## Runtime verification needed

- Self, another avatar, owned animesh, and unowned animesh.
- Character farther than 256 m diagonally but still in the same region, if
  region-wide selection is intended.
- Full export while an AO/animation supplies the captured base pose.
- A deliberately identity joint that must remain locked in the uploaded pose.
- Pelvis checkbox in both states, testing rotation and position.
- Slider/spinner and in-world manipulator edits, including mirror and
  sympathetic modes.
- BVH output failure (for example an unwritable destination) and the displayed
  save result.
- Native pose and diff round trips after reverting the unrelated serialization
  changes.

## Runtime finding: full-BVH Euler channel order

Direct comparison of `felyss2.xml` and `felyss2.bvh` found matching raw Euler
values for all 155 exported joints (maximum rounding difference about
0.0000091 degrees). The apparently mismatched Z-X-Y BVH declaration is
intentional: `LLBVHLoader::makeTranslation()` assigns aliased joints a cyclic
frame matrix, and the loader conjugates every input rotation by that frame.
Declaring Z-X-Y while writing the target Euler values as X-Y-Z compensates for
that conversion and reconstructs the XML quaternion.

A trial Z-Y-X encoding omitted this loader frame conversion and visibly
inverted or permuted rotations, particularly in the arms. Mathematical replay
of the complete loader conversion over `felyss3` confirms that fixed Z-X-Y
channels with raw X-Y-Z target angles reproduce all XML joint quaternions to
within about 0.000003 degrees. BVH has no scale channels; the observed shoulder
and chest deformation was rotational, not scale data.

The original special exporter also wrote the pelvis target position, which
included the skeleton/shape base position. Native XML correctly stored only
the poser-authored position delta. BVH root translation is likewise a delta,
so export now uses the public pelvis position. In `felyss2`, this changes the
incorrect nonzero BVH root offset to the same zero delta stored in XML.

Comparison with the known-good `newtest2` pair found a second structural
difference: its XML contains all 155 poser joints, but its BVH predates the 25
uppercase `COL_VOLUMES` joints and contains only 130 joints. A broken current
export such as `ayane3` writes full target rotations for those newer joints.
That bakes their non-identity skeleton/shape base rotations into the animation
and applies the bases twice. Current BVH files retain all 155 joints, but write
the poser-authored collision-volume rotation delta; zero deltas are optimized
away by the importer, while intentional collision-volume edits remain
representable. Regular animation/deform joints continue to use full target
rotations, and native XML continues to save the complete poser state.

The same distinction is required in native XML. `FSJointPose` deliberately
refuses to zero a collision volume's base rotation. Saving its full target and
then loading that value as its public rotation therefore computes
`full target * existing base`, applying the skeleton/shape base twice. Native
full-pose XML now stores collision-volume rotation deltas while retaining full
target rotations for ordinary joints. Position and scale remain poser deltas,
so the target avatar's shape continues to supply its own base transforms.
