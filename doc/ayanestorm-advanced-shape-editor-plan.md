# Advanced Shape Tools — Implementation Plan

Target: AyaneStorm (Firestorm-derived SL viewer fork), `e:\dev\AyaneStorm\ayanestorm-normal`

## Context

The classic "Editing Shape" panel exposes ~149 sliders, one per `LLVisualParam` tagged with an `edit_group="shape_*"` in `avatar_lad.xml`. Investigation of a real commercial shape-editing service revealed two independent, previously-unexploited capabilities of the existing (free, no-upload) shape wearable format:

1. **Hidden params.** `avatar_lad.xml` defines **1,410** total params, **561** applicable to the shape wearable, but only **149** carry an `edit_group` and get a UI slider. The other ~400 are legitimate, individually-ranged, individually-clamped params with no control anywhere in the stock UI.
2. **Driver/driven decoupling.** Many visible sliders are actually "driver" params that push a computed value onto one or more hidden "driven" sub-params in lockstep, along one fixed interpolation curve. That push is one-directional and not continuously reconciled — a driven sub-param's ID can be set independently (a different value than its driver would ever produce) via the plain `id weight` shape-asset format, and it persists until its driver is explicitly moved again. This is exactly how the commercial product achieves "change one bone without influencing another" edits that no slider combination can reach — and it requires no bypass of the normal clamped import path (`LLWearable::importStream` → `revertValues()` → `LLVisualParam::setWeight()`, which still clamps every value to its own `value_min`/`value_max`).

Separately, true per-joint bone position/scale control (asymmetric offsets, magnitudes beyond any shape param's range, collision-volume manipulation) is not expressible through the shape-param system at all — it requires the deformer/rigged-mesh-attachment mechanism (`LLJoint::addAttachmentPosOverride`/`addAttachmentScaleOverride`), which is a structurally different, real-money capability (mesh upload, real inventory asset) because it's the only way such data becomes visible to unmodified viewers.

This plan builds **two independent floaters** reflecting that split:

- **Advanced Shape Editor** — free, instant, no upload. Exposes all 561 shape params (not just the 149 with sliders), including normally-hidden driven sub-params, each with real-time preview, saved via the completely standard shape-wearable mechanism.
- **Advanced Shape Deformer** — real per-joint bone editor with live local preview plus an explicit, clearly-priced "Bake & Upload" step that persists the result as a real, tiny invisible rigged-mesh attachment, visible to any unmodified viewer.

They are deliberately separate floaters/menu entries, not tabs of one window, given how different their cost/mechanism/risk profiles are.

---

## Post-implementation correction (verified against this repo's actual avatar_lad.xml)

The "561 hidden params" and "decoupling persists automatically, universally" claims above did **not** hold up once Part 1 was built and tested. What was actually verified:

- **Only 149 params carry `wearable="shape"`** anywhere in this repo's `avatar_lad.xml` (confirmed by exhaustive scan and cross-checked against a real commercial-tool export/import round-trip on a live shape). `LLWearable::createVisualParams()` uses that exact same `wearable="shape"` attribute to decide what gets cloned onto the wearable at all — a param without it (the vast majority of the other ~1,250) is never reachable through the standard shape-asset format, full stop, regardless of any UI built to expose it. Of the 149, 126 are the classic panel's own sliders; only ~23 have no `shape_*` edit_group and are otherwise invisible in the stock UI.
- Most of those 23 extra rows are either an empty `<param_morph/>` (no morph data at all, cannot do anything) or a driven param whose driver's own curve already sweeps its full value range (no reachable combination the driver alone couldn't already produce). The real, demonstrable value of independent editing is **combinations** of several driven siblings of the same driver that the driver's single-weight curve cannot trace through simultaneously — not access to any single param's value in isolation.
- **The driven-param decoupling itself does not persist by default**, on this codebase, for two independent reasons, both since patched (`<AS:Chanayane>`-tagged) but **only in this AyaneStorm build**:
  1. `LLVOAvatarSelf::idleUpdateAppearanceAnimation()` calls `writeWearablesToAvatar()` → `LLWearable::writeToAvatar()` every single frame, unconditionally, which re-invokes every driver's `setWeight()` and re-overwrites its driven params' *live avatar* state continuously — there is no "persists until the driver moves again" window in stock code at all. Patched in `writeToAvatar()`, gated to wearable names starting with `"AS-"`.
  2. `LLWearable::importStream()` (i.e. every wear-in, every viewer) ends by calling `revertValues()`, which applies saved param weights in ascending param-ID order; since driven sub-params typically have a *lower* id than their own driver (e.g. `Big_Belly_Torso`=104 < `Belly Size`=157), the driven value gets set correctly first and is then immediately overwritten back by the driver's own cascade later in the same pass — discarding the decoupling on every single load, even though it was saved correctly to the asset. Patched in `revertValues()`, same `"AS-"` gate.
  3. **Both fixes are local to this fork.** They live in `indra/llappearance/llwearable.cpp`, not in the asset format itself, and there is no way to encode a decoupled value such that unpatched stock code honors it — the stock cascade is unconditional. A decoupled `"AS-"` shape therefore renders correctly and persistently only on viewers running this same patched build; on stock Firestorm, stock SL viewer, or any other unpatched viewer — including other people's viewers looking at an AyaneStorm user's avatar — it renders exactly as if only the driver's own value had ever been set. Decoupling is real and functional, but not currently shareable/universal.

None of this affects Part 2 (bone/joint deformer), which was never built and remains a separate, structurally different mechanism as originally scoped below.

---

## Part 1 status: cancelled (2026-09-16)

Part 1 was implemented (floater, rows, save/reset/search, and four separate `<AS:Chanayane>`-gated patches to `llwearable.cpp`/`llviewerwearable.cpp`/`llagentwearables.cpp`/`llwearablelist.cpp` targeting the idle-loop, revert-ordering, save-copy, and isDirty gaps above) but a decoupled driven-param edit still did not reliably survive Save + re-wear in testing. Root-caused as far as: `LLDriverParam::setDrivenWeight()` writes to a driven param via a direct `driven->mParam->setWeight()` call that bypasses every chokepoint patched so far (`LLCharacter::setVisualParamWeight`, `LLWearable::writeToAvatar`'s driver-skip) — final instrumentation targeted the true universal chokepoint (`LLVisualParam::setWeight` itself) but was never run to a conclusion.

All Part 1 code (new AS-owned files and the four upstream-file patches) has been reverted. This document is kept as the record of what was tried and learned, in case Part 1 is revisited: the enumeration/UI approach is sound, but any future attempt must first resolve the driven-param persistence problem at its actual root (likely: make `LLDriverParam`'s direct driven-weight write itself aware of the decoupled-value case, rather than patching every indirect call site).

Part 2 (bone/joint deformer) is unaffected and remains the next planned unit of work.

---

## Part 1 — Advanced Shape Editor (free, no upload)

### Mechanism (verified against source)

- `LLCharacter::getFirstVisualParam()`/`getNextVisualParam()`/`getVisualParamCount()` (`indra/llcharacter/llcharacter.h:213-252`) already enumerate **every** visual param on the avatar, unfiltered by `edit_group` — the edit_group filter used by the classic panel (`LLPanelEditWearable::getSortedParams`, `llpaneleditwearable.cpp:1611,1626`) is caller-side, not a property of storage. Walk `gAgentAvatarp->getFirstVisualParam()/getNextVisualParam()`, cast to `LLViewerVisualParam*`, filter on `getWearableType() == LLWearableType::WT_SHAPE` (`llviewervisualparam.h:95`) to get the full 561-param set.
- Every param has a mandatory `name` attribute (`LLVisualParamInfo::parseXml`, `indra/llcharacter/llvisualparam.cpp:51-141`, parse fails without it) regardless of whether `edit_group` is present — confirmed concretely on driven sub-param id `20002` (`avatar_lad.xml:5279-5286`, no `edit_group`/`wearable=`, still has `name="Nose_Big_Out"`). So every row in a raw 561-param list can show a real name via `getDisplayName()`, never a bare numeric ID.
- `LLScrollingPanelParamBase` (`indra/newview/llscrollingpanelparambase.h/.cpp`) is directly reusable, unmodified — its constructor takes any `LLViewerVisualParam*`, and `weightToPercent`/`percentToWeight` (`llscrollingpanelparambase.cpp:105-115`) already use that param's own `getMinWeight()`/`getMaxWeight()`. The commit path (`onSliderMoved` → `mWearable->setVisualParamWeight(id, weight, false)` → `writeToAvatar`/`updateVisualParams()`) is the same no-cost, save-compatible path the classic sliders use.
- Setting a driven param's weight directly is confirmed safe and non-transient: the only resync paths are (a) the driver's own `setWeight()` being called again, and (b) `LLDriverParam::updateCrossDrivenParams()` (`lldriverparam.cpp:537-567`), which only fires for **cross-wearable** driven params (e.g. shape driving eyes/hair) on a wearable-type-change event — same-wearable (shape-internal) driven sub-morphs are untouched by it. Verified `llpaneleditwearable.cpp`'s open/close/sex-change/draw logic has no other resync path that would silently overwrite a manually-set driven param.
- `LLScrollingPanelList` (`indra/llui/llscrollingpanellist.h`) has no built-in search/filter. With 400+ rows, build a small search box (reuse `LLFilterEditor`, already used elsewhere e.g. inventory panels) in the new panel that just toggles row visibility by matching `getDisplayName()`/`getName()` — no changes needed to the shared list widget itself.

### Design

- New floater `ASFloaterShapeRawEditor` (`asfloatershaperaweditor.h/.cpp`, `floater_as_shape_raw_editor.xml`).
- No "driven vs driver" distinction needs special UI handling — every param is just a row with its own name and range; the decoupling behavior described in Context happens naturally as a consequence of setting rows independently, exactly like the commercial product's effect, with no extra plumbing.

#### UI organization (561 rows is a real design problem, not an afterthought)

A flat list of 561 sliders is unusable. Structure it in layers so a user can narrow down fast without ever seeing the full wall at once:

1. **Top-level grouping by `getEditGroup()`** — the 149 params that already carry a `shape_*` edit_group slot into the same 9 familiar categories the classic panel uses (Body/Head/Eyes/Ears/Nose/Mouth/Chin/Torso/Legs), so anything a user already knows from the classic panel lives in the same place here.
2. **A 10th category, "Advanced / Hidden"**, for the ~400 params with no `edit_group` — the normally-driven sub-params this floater exists to expose. Within that category, sub-group by **driver relationship**: for every param that appears as a `<driven id="...">` target somewhere in `avatar_lad.xml`'s `<driver_parameters>` block, label its row with its driver's name (e.g. "→ driven by: Broad Nostrils") and cluster driven siblings of the same driver together, rather than dumping them in raw ID order. This is the single biggest legibility win, since it's exactly the relationship a user needs to understand to use this floater purposefully (per Context: decoupling a driven param from its driver is the whole point). Params with no driver relationship at all (rare, orphaned) sit in a final "Other" sub-bucket.
3. **Search/filter box** (`LLFilterEditor`, already used elsewhere e.g. inventory panels) above the list, filtering on `getDisplayName()`/`getName()`/param ID across all categories at once — for a user who already knows the name of the param they want (e.g. from the commercial product's own export) and doesn't want to navigate categories at all.
4. **Each row shows, at minimum**: display name, current value as a real number in the param's own units (not just a 0-100 UI percent — see note below), a slider spanning that param's own `value_min`/`value_max`, and for driven rows, a small indicator/tooltip naming the driver and noting "this value may be overwritten if you move '<driver name>'".
5. Consider showing raw weight units (not 0-100%) directly in this advanced floater, unlike the classic panel — since this tool is explicitly for power users comparing exact values (e.g. against an externally-tweaked exported shape), hiding the real number behind a percent mapping works against the tool's purpose.
6. Accordion or collapsible-category tree (mirroring `LLPanelEditWearable`'s accordion-tab pattern) rather than a single scroll — keeps 400+ rows from being one endless scrollbar, and lets a user collapse categories they aren't touching.

#### Editing an existing (modifiable) shape, with Save / Save As

This floater must operate on the currently-worn or explicitly-selected shape wearable, and only if it's modifiable:

- On open, target the currently-worn shape wearable (`LLAgentWearables`/`LLViewerWearable` for `WT_SHAPE`), the same object the classic Editing Shape panel edits.
- Check the wearable's permissions before allowing edits — a shape wearable can be no-modify (e.g. received from someone else, or a locked/protected shape). If not modifiable, open the floater read-only (sliders visible but disabled, or the floater refuses to open with a clear message), mirroring how the classic panel already handles non-modifiable wearables (`LLPanelEditWearable` — check its existing modify-permission gating, e.g. around `isAgentAvatarValid()`/wearable permission checks, and reuse that exact logic rather than re-deriving it).
- **"Save"** — enabled only when editing an existing, already-saved, modifiable shape wearable (not a never-saved/new one). Writes the current param values back into that same wearable/inventory item in place (the same "overwrite this item" mechanism the classic panel's own "Save" already uses — reuse it directly, do not reimplement wearable-saving logic).
- **"Save As…"** — always available; prompts for a name and creates a new shape wearable/inventory item from current values (same underlying mechanism as the classic panel's "Save As", reused directly).
- Both buttons operate on the exact same no-cost `LLWearable::exportStream`/inventory-item-update path already established in Context — no new persistence code, just wiring the two existing save entry points (find and reuse whatever `LLPanelEditWearable`/`LLAppearanceMgr` already calls for its own Save and Save As buttons, e.g. `LLAgentWearables::saveWearable`/`saveWearableAs` or equivalent — confirm exact function names during implementation rather than assuming, but the mechanism itself is already proven and must not be reimplemented).

### Files

- `indra/newview/asfloatershaperaweditor.h/.cpp`
- `indra/newview/skins/default/xui/en/floater_as_shape_raw_editor.xml`
- `indra/newview/CMakeLists.txt` — add the two source files (plain entries, matching existing `as*` files at e.g. lines ~148/~1066).
- `indra/newview/llviewerfloaterreg.cpp` — `LLFloaterReg::add("as_shape_raw_editor", "floater_as_shape_raw_editor.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<ASFloaterShapeRawEditor>);`, bracketed `// <AS:Chanayane> ... // </AS:Chanayane>`, mirroring `as_my_lights` (lines 729-731).
- `indra/newview/skins/default/xui/en/menu_viewer.xml` — new `menu_item_check` under the `AyaneStorm` menu, bracketed `<!-- <AS:Chanayane> --> ... <!-- </AS:Chanayane> -->`, toggling `as_shape_raw_editor`.

### Verification

1. Open the floater; confirm 561 rows appear (or however many resolve at runtime), each with a real name, not a bare ID, correctly grouped (9 classic categories + Advanced/Hidden sub-grouped by driver).
2. Search for a known driven-only param (e.g. "Nose_Big_Out" id 20002) and confirm it's listed and editable even though it has no classic-panel slider, and that its row correctly names its driver.
3. Move a driven param independently of its driver; confirm the avatar visibly reflects it same-frame, and confirm moving the driver slider afterward properly overwrites it (expected, documented behavior — not a bug).
4. Open the floater on a no-modify shape wearable; confirm it correctly refuses edits / opens read-only rather than silently allowing changes that can't be saved.
5. On a modifiable, already-saved shape: change a value, click "Save"; confirm the same inventory item is updated in place (no new item created), and the change persists across reopen/relog.
6. Click "Save As…"; confirm a new, separate shape wearable/inventory item is created without altering the original.
7. Confirm no L$ cost or upload dialog appears anywhere in this flow.

---

## Part 2 — Advanced Shape Deformer (real bone/joint editor, paid bake)

### Mechanism (verified against source)

- `LLJoint::addAttachmentPosOverride`/`addAttachmentScaleOverride` (`indra/llcharacter/lljoint.cpp:415,622`) require only a non-null UUID key — no real mesh/attachment needed. Calling them directly with a synthesized per-session fake UUID gives instant, real, in-world local preview (your actual avatar visibly deforms, same as the classic shape panel) — mirroring a trick the engine's own COLLADA-import preview already uses (`LLDAELoader::processDomModel`, `indra/llprimitive/lldaeloader.cpp:1569-1636`).
- All 133 bones + 26 collision volumes are legal override targets, including asymmetric left/right values and magnitudes/axes no shape param can express (shape params are single-weight, applied along one fixed authored vector).
- `findActiveOverride()` (`lljoint.cpp:46-62`) resolves ties between multiple mesh_ids on one joint by largest UUID, not recency — must be tracked locally by the editor (not trusted for UI reflection) and documented in-UI for when other rigged items also override the same bone.
- `LLPolySkeletalDistortion::apply()` (shape sliders) already passes `apply_attachment_overrides=true` — classic shape sliders and this editor's overrides compose automatically, no glue code needed.
- **Persistence requires a real new mesh asset.** SL's asset store is content-addressed/immutable (confirmed: every upload, mesh or animation, always mints a new UUID via `generateNewAssetId()`, `llviewerassetupload.cpp:262` — no in-place update capability exists). LSL has no function capable of posing an arbitrary avatar bone from runtime parameters (confirmed against the full first-party LSL keyword list: `llSetKeyframedMotion` only moves the scripted prim itself; `llSetAnimationOverride` only selects among pre-existing uploaded assets). So "pay once, edit forever, still visible to everyone" is not achievable — the only two real endpoints are pay-per-bake-but-universally-visible, or free-but-client-side-only. This plan uses the former, since persistence-and-visibility to others is a hard requirement.
- Bake pipeline is buildable entirely in-memory, no file I/O: `LLMeshSkinInfo` (`indra/llprimitive/llmodel.h:46-77`) is a plain struct; `LLModel` (`llmodel.h:80+`) exposes `setVolumeFaceData()`/`addFace()` and a public `mSkinWeights` map; `LLModelInstance` is plain/heap-constructible; `LLMeshRepository::uploadModel(std::vector<LLModelInstance>&, ...)` (`indra/newview/llmeshrepository.h:927-934`) takes the in-memory instance vector directly, no filename anywhere in the chain.
- The Firestorm bridge (`fslslbridge.h/.cpp`) was investigated and rejected as a carrier: it's a legacy non-mesh prim with no skin data, purely for scripting relay. This feature uses its own dedicated new invisible attachment.
- Reusable joint table: `FSPoserAnimator::PoserJoints` (`indra/newview/fsposeranimator.h:200-388`) — reuse via a private member instance purely for its name/category table, never its posing/motion methods (those drive a separate `LLMotion`-based mechanism, not persistent overrides).

### Open Design Risks (flag before/while implementing)

1. **Scale-override encoding into `mAlternateBindMatrix` is undesigned.** The only precedent (DAE loader) only overwrites translation, never scale. Two candidates: (a) fold scale into the matrix's 3x3 linear part — needs verification the skinning code actually consumes that part at runtime; (b) don't encode scale into the mesh at all, re-apply `addAttachmentScaleOverride` at runtime with the real mesh_id once the attachment registers, piggybacking on proven machinery. **Recommend prototyping (b) first**; only pursue (a) if the relog test (verification step 12) shows scale doesn't persist, since a relog re-derives everything from the mesh asset alone with no runtime call involved. Position overrides are low-risk (mirror the DAE precedent exactly); scale is the genuinely novel half.
2. **Default bind-pose sourcing** — resolved, low risk: `LLJoint::getDefaultPosition()`/`getDefaultScale()` give per-joint bind defaults directly; build `mInvBindMatrix[i]` as the inverse of the joint's default-pose world transform composed up the parent chain (check for an existing composition helper near `LLJoint::updateWorldMatrix` before hand-rolling it).
3. **Placeholder mesh minimums** — a single small, non-degenerate (real area, not fully coincident points) triangle satisfies geometry requirements; `mSkinWeights` needs at least one `JointWeight` entry to pass the `upload_skin && !mSkinWeights.empty()` gate (`llmodel.cpp:856`). Needs an in-viewer smoke test to confirm the simulator doesn't reject near-zero-area geometry.
4. **LOD population** — unconfirmed whether missing medium/low/lowest LODs auto-derive or must be explicitly populated. Safe default: populate all four `mLOD[]` slots with the same placeholder model.
5. **`findActiveOverride`'s UUID-max tie-break** — document in-UI (tooltip noting live preview may not reflect changes if another worn rigged item also overrides the same bone); cover explicitly in testing.
6. **Real L$ cost, real inventory asset per bake** — unavoidable; UI must show an explicit confirmation with the real fee quote before committing (quote-then-confirm, matching `llfloatermodelpreview.cpp`'s existing sequencing).

### Phase A — Live Preview (local-only, no upload)

- `LLUUID mSessionFakeMeshId`, generated once per floater instance, reused for every override call this session. Verify the scope of any "clear all overrides on this joint" convenience before using it — if it clears contributions from *other* worn items too, use `removeAttachmentPosOverride(mSessionFakeMeshId, ...)` per-joint instead.
- Track active session overrides locally in `std::map<LLJoint*, ASJointOverrideState>` so the UI reflects true state independent of `findActiveOverride`'s tie-break, and so Phase B can enumerate directly from this map.
- New floater `ASFloaterBoneDeformer` (`asfloaterbonedeformer.h/.cpp`, `floater_as_bone_deformer.xml`), modeled on the accordion-of-scrolling-lists pattern already used by the classic Editing Shape panel. One tab per `E_BoneTypes` category (Body/Face/Hands/Misc/Collision Volumes via `FSPoserAnimator`'s table), each an `LLScrollingPanelList` of joint rows.
- New row class `ASScrollingPanelJointDeformer` (adapted from `LLScrollingPanelParamBase`) holds `LLJoint*` and six per-axis sliders (Pos X/Y/Z centered on 0, wide range **not** clamped to any shape-param range; Scale X/Y/Z centered on 1.0/`getDefaultScale()`).
- Each row's commit callback calls `addAttachmentPosOverride`/`addAttachmentScaleOverride` directly — dirty-flag propagation is automatic, next frame reflects the change. Per-row "Reset" calls the matching `remove*Override`. Floater-level "Reset All" iterates the tracked-override map only (not a blanket clear).

#### Phase A implementation status (2026-09-16)

- Implemented as `ASFloaterBoneDeformer` plus `ASScrollingPanelJointDeformer`, with five accordion categories, per-axis position offsets (`-0.25..+0.25 m`) and absolute per-axis scales (`0.000001..10`). Controls use `0.000001` precision instead of forcing values onto the earlier `0.005` step.
- Rows initially reflect live joint state: position is shown relative to bind position so long bone offsets do not exceed the UI range, while scale is shown as the current absolute scale. The live override state is keyed by one generated floater-session UUID and stored in `std::map<LLJoint*, ASJointOverrideState>`. Reset explicitly removes this UUID's matching override; closing the floater clears only its own overrides.
- Each axis has its own reset button, each joint has a reset button, and Reset All removes the complete editor override set. The row layout pairs Position X/Scale X, Position Y/Scale Y, and Position Z/Scale Z; rows recompute both column widths when the floater is resized horizontally.
- Slider commits update only the selected axis. Untouched axes retain their original full-precision values, and reset-button comparisons use a sub-step epsilon; this prevents display quantization from falsely marking adjacent axes as changed.
- Undo/Redo store complete override-map snapshots. Slider drags are coalesced into one action, Reset Joint/Reset All are each one undoable action, and a new edit clears the redo branch. `ASBoneDeformerUndoLevels` bounds both histories, is configurable under AyaneStorm Preferences from 1 to 99, and defaults to 99.
- The floater warns users to remove other deformers because UUID-priority conflicts can hide this session's overrides. Its edit-pose button temporarily selects `PS_Arms_downward_Legs_together` through the existing Firestorm Pose Stand and restores the prior Pose Stand selection/state when toggled off or when the deformer closes.
- Exhaustive comparison found that `FSPoserAnimator::PoserJoints` contains 155 targets while this repository's `avatar_skeleton.xml` contains 159 (133 bones + 26 collision volumes). The four omissions are `mFaceEyeAltLeft`, `mFaceEyeAltRight`, `mFaceJawShaper`, and `LOWER_BACK`; the floater adds those explicitly, so all 159 targets are shown without modifying Firestorm's poser table.
- Phase B is not present. No upload, inventory creation, L$ quote, or persistence path is wired into this Phase A floater.
- Rotation is intentionally absent: `LLJoint` has attachment override APIs only for position and scale. Firestorm Poser rotations use a motion-based mechanism and cannot be persisted through Phase B's planned rigged-mesh joint-override path.

### Phase B — Bake & Upload

Triggered by one explicit "Bake & Upload…" button, gated by a confirmation dialog stating plainly this uploads a new mesh asset (real L$ fee) to make the current adjustments permanent/shareable. New standalone class `ASBoneDeformerBaker` (`asbonedeformerbaker.h/.cpp`), kept separate from the floater UI:

1. Collect all active session overrides (position + scale) from the tracked-override map.
2. Construct a placeholder `LLModel`: one small, non-degenerate triangle, fully transparent material.
3. Populate `mSkinWeights`: one entry binding the placeholder geometry to a stable "carrier" joint (e.g. `mPelvis`).
4. Populate `mSkinInfo.mJointNames`: the carrier joint plus every joint with an active override (sparse list — expand only if testing shows registration requires the full skeleton).
5. Populate `mInvBindMatrix` from `getDefaultPosition()`/`getDefaultScale()` composed up the parent chain.
6. Populate `mAlternateBindMatrix`: for position-overridden joints, copy `mInvBindMatrix[i]` and overwrite the translation column (mirrors `lldaeloader.cpp:1622-1624`). For scale-only joints, keep translation unmodified; encode scale per whichever Risk 1 path is chosen. Stay index-aligned with `mJointNames`.
7. Build one `LLModelInstance`, populate all four `mLOD[]` slots with the placeholder.
8. Call `gMeshRepo.uploadModel(...)` with `upload_skin=true, upload_joints=true`, a dedicated destination folder, and fee/upload observer callbacks (`ASBoneDeformerUploadObserver`) sequenced quote-then-confirm-then-commit.
9. On success, auto-wear the new item via the same call path the inventory "Attach" context-menu action uses, at an unused/invisible attach point.
10. Once the real attachment's overrides are confirmed active (wait for an explicit signal, not a fixed delay, to avoid a visible "pop"), clear the session's fake-mesh-id overrides for every joint touched.

### Files

- `indra/newview/asfloaterbonedeformer.h/.cpp` — `ASFloaterBoneDeformer : public LLFloater`.
- `indra/newview/asscrollingpaneljointdeformer.h/.cpp` — `ASScrollingPanelJointDeformer : public LLScrollingPanel`.
- `indra/newview/asbonedeformerbaker.h/.cpp` — `ASBoneDeformerBaker`, all mesh-construction/upload/auto-wear/cleanup logic, independent of UI code.
- `indra/newview/skins/default/xui/en/floater_as_bone_deformer.xml` — accordion (one tab per `E_BoneTypes` category) + "Reset All"/"Bake & Upload…" buttons.
- `indra/newview/CMakeLists.txt`, `indra/newview/llviewerfloaterreg.cpp` (`"as_bone_deformer"`), `menu_viewer.xml` — same convention as Part 1.

### Verification

In-viewer, no relog:
1. Open the floater; confirm every joint/category listed, sliders default to true joint defaults.
2. Drag a position slider on an obvious joint (`mHead`/`mChest`); confirm same-frame visible deformation, no log errors.
3. Drag the matching scale slider; confirm visible scale change.
4. Adjust a classic Editing Shape slider affecting the same joint; confirm both effects compose.
5. "Reset this joint"; confirm snap-back and removal from the tracked-override map.
6. Wear an unrelated rigged item overriding the same joint; observe and document actual tie-break behavior (Risk 5).

Bake & Upload path:
7. Mix of position-only, scale-only, and both-type overrides; "Bake & Upload…"; confirm the fee-quote dialog shows a real non-zero L$ figure before commit.
8. Confirm upload succeeds, inventory item appears, item auto-attaches.
9. Confirm no visible "pop" across the fake-override-clear transition.
10. Detach the baked item; confirm avatar reverts to default.
11. Re-attach from inventory directly; confirm deformation re-applies with no floater interaction.
12. **Full relog test (critical — validates Risk 1's chosen approach)**: log out/in with the item worn. Confirm position persists. Confirm scale persists — if it reverts, path (b) has failed and path (a) must be implemented instead.

---

## Sequencing

1. **Part 1 (Advanced Shape Editor) first** — free, no upload risk, no mesh/asset complexity, delivers real value on its own, and directly generalizes the commercial-product behavior that motivated this plan.
2. **Part 2, Phase A (live bone preview)** next — independent of Phase B, delivers standalone value (a live "extreme shape editor" for bones) even if baking is deferred.
3. **Part 2, Phase B (bake/upload)** last — resolve Risks 1 and 4 empirically via a minimal single-joint throwaway bake before wiring the full multi-joint pipeline; build/test auto-wear and session cleanup only once upload itself is confirmed working end-to-end.
