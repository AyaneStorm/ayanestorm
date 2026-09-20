# AyaneStorm / Firestorm Poser Comparison

## Scope

AyaneStorm was compared with the local Firestorm reference tree at
`./.phoenix-firestorm-master`. The comparison covered:

- every file under `indra` whose name contains `pose` or `poser`;
- the core poser, pose-state, posing-motion, joint-manipulation, and English
  poser UI files explicitly;
- files in either tree that textually reference the poser classes, floater,
  menus, or related settings;
- AyaneStorm-owned files that consume Firestorm poser functionality.

This is a source-tree comparison, not a Git-history attribution analysis.

## Result

AyaneStorm does **not currently diverge from the reference Firestorm poser**.
Both trees have the same 51 `pose`/`poser`-named files under `indra`, and all
51 files are byte-identical.

The identical core implementation includes:

- `fsfloaterposer.{cpp,h}`;
- `fsposeranimator.{cpp,h}`;
- `fsjointpose.{cpp,h}`;
- `fsposestate.{cpp,h}`;
- `fspose.{cpp,h}`;
- `fsposingmotion.{cpp,h}`;
- `fsjointrotatetool.{cpp,h}`;
- `fsmaniprotatejoint.{cpp,h}` and `fsmaniptranslatejoint.{cpp,h}`;
- `floater_fs_poser.xml`, `menu_fs_poser_poses_btn.xml`, localized poser UI,
  pose-stand files, and bundled pose presets.

## Chanayane-tagged code already in Firestorm

`indra/newview/fsfloaterposer.cpp` lines 2909-2911 contain a
`<AS:chanayane> BVH fixes` ownership block. It writes the root BVH channels as:

```text
CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
```

The block is byte-identical in `./.phoenix-firestorm-master`. It is therefore
Chanayane-tagged poser work, but it is not an AyaneStorm-only change relative
to this Firestorm reference snapshot.

## AyaneStorm-only integrations

AyaneStorm adds functionality around the poser without modifying its core:

### Advanced Shape Deformer

`asfloaterbonedeformer.{cpp,h}` creates an `FSPoserAnimator` and reuses its
`PoserJoints` metadata to populate and categorize the deformer's joint list.
It supplements four skeleton targets absent from that table:
`mFaceEyeAltLeft`, `mFaceEyeAltRight`, `mFaceJawShaper`, and `LOWER_BACK`.

This is metadata reuse by a separate AyaneStorm module. It does not alter pose
state, rotations, translations, serialization, animation, or the poser UI.

### My Lights

`asfloatermylights.cpp` exposes an **Open Poser** button. Its callback only
calls `LLFloaterReg::toggleInstance("fs_poser")`. The matching control is in
`skins/default/xui/en/floater_as_my_lights.xml`.

This is a shortcut to the stock Firestorm poser, not a poser implementation
change.

### AyaneStorm menu

`skins/default/xui/en/menu_viewer.xml` adds a **Poser** entry under the new
**AyaneStorm** menu. Like the My Lights button, it checks and toggles the
existing `fs_poser` floater. Firestorm's original poser menu entry remains in
place, so this is an additional access point rather than changed poser
behavior.

## Shared files with unrelated differences

Several shared files that mention poser-related terms differ between the two
trees, including `CMakeLists.txt`, floater registration, settings, commands,
menus, strings, textures, and the viewer manifest. Inspection of their diffs
found AyaneStorm feature registration and packaging changes, including the
integrations above, but no replacement or behavioral modification of the
Firestorm poser.

## Conclusion

Relative to `./.phoenix-firestorm-master`, the answer is:

- **No AyaneStorm-only change to the Firestorm poser itself.**
- **Yes, one Chanayane-tagged BVH export fix exists in the poser, but the
  reference Firestorm tree already contains it.**
- **Yes, AyaneStorm-only features reuse or open the poser from separate
  modules.**

## In-world manipulator first-click rotation reset

### Symptom

After selecting a joint through its in-world selection sphere, the first click
or drag on a rotation axis can reset the selected joint's rotation. A second
drag behaves normally.

### Cause

Runtime testing disproved the original focus-transfer hypothesis. The reset
occurs when `FSPoserSaveExternalFileAlso` is enabled; the tested viewer's
persisted setting was enabled. The manipulator passes that setting to
`FSPoserAnimator::updateJointRotationFromManip()` as a request to zero the
captured base rotation for BVH export.

The old code then applied the manipulator delta to only the joint's public
rotation. For a newly captured joint, that public rotation is identity while
the visible pose is held in the base rotation. Zeroing the base therefore
discarded the visible pose, leaving only the small manipulator delta and making
the joint appear to reset to its default position.

### Rejected focus hypothesis

Read-only Git line history identifies Firestorm commit
`eecf28896e1495cdaa24ce88a44b669714795ad5`, authored on 2025-03-13 with the
subject **Add undo/redo keyboard accelerator support to poser**. That commit:

- made `FSFloaterPoser` an edit-menu handler for undo/redo;
- added poser `onFocusReceived()` and `onFocusLost()` handling;
- added `poser->setFocus(true)` to the in-world rotation-axis mouse-down path.

The focus call remains relevant to keyboard undo/redo and is not the cause of
this reset. The faulty base-zeroing path remains present in both AyaneStorm and
the Firestorm reference tree, so this is an upstream Firestorm defect.

### Fix

`FSPoserAnimator::updateJointRotationFromManip()` now applies the manipulator
delta to `FSJointPose::getTargetRotation()` when zeroing the base rotation.
That target contains both the public and captured base rotations, so converting
the joint to BVH-export form preserves its visible pose. The same preservation
is applied to mirrored and sympathetic manipulation.

The experimental focus change was reverted. Runtime testing still needs to
verify the first axis drag with **Save as BVH** enabled and disabled, plus
mirrored or sympathetic manipulation.
