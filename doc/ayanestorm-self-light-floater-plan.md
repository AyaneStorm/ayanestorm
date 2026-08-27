# Self-Lighting Floater (multi-light photography rig + isolate background)

## Context

The user wants an in-viewer-only "face light" rig for self-photography: one or more
local light sources with no inventory footprint, visible only to themselves, managed
from a dedicated floater. Each light has its own placement (joint anchor + offset),
intensity, radius, falloff/softness, and color, editable via sliders-with-numeric-entry
and a draggable 2D placement pad, with quick position presets (Head/Chest/Back). A
collapsible list panel on the right lets the user add/select/delete lights — selecting
a light in the list loads its parameters into the edit controls — and the whole rig
(the full light list) can be saved to / loaded from a simple XML preset file. The
floater also offers a solid-background mode (none/black/white) to isolate the avatar
for photos, a toggle to freeze all avatar animations (matching the existing AyaneStorm
menu behavior), a toggleable in-world beacon marking each light's position (never
visible in snapshots), and a button to open the existing Firestorm Poser floater.

This is a new C++/XUI feature in the Firestorm/AyaneStorm SL viewer
(`e:\dev\AyaneStorm\ayanestorm-normal`, `indra/newview`).

## Repository conventions (AGENTS.md — must follow)

- New source files we author are AS-owned and use an `as` prefix (not `ll`/`fs`,
  which are reserved for Second-Life-owned and Firestorm-owned files respectively).
  AS-owned files need no ownership-tag comments, but do need normal explanatory
  comments.
- Any edit to an existing `ll*`/`fs*` file (registration line in
  `llviewerfloaterreg.cpp`, the menu entry in `menu_viewer.xml`, the clear-color hook
  in `llviewerdisplay.cpp`, the beacon call-out in `llviewerdisplay.cpp`, anything
  touched in `llviewermenu.cpp`, `CMakeLists.txt`) must be wrapped in ownership-tag
  comments, preserving the original code as a comment inside the tags:
  ```cpp
  // <AS:Chanayane> explanatory comment
  // original code commented here;
  new code here;
  // </AS:Chanayane>
  ```
- Prefer keeping substantial logic inside the new AS-owned module rather than growing
  an existing upstream file — mirrors how `fsexactoit` offloads logic to avoid
  enlarging `llpoolalpha`/`llpipeline`. All light-rig data, per-frame idle logic,
  joint math, beacon drawing, background-override state, and preset serialization
  live in the new AS-owned files; edits to `llviewerdisplay.cpp` are minimal
  call-outs into AS-owned static methods, never inlined logic.
- Commit authorship: `chanayane@firestorm`.
- Record research/design notes in a new `/doc` markdown file (this repo already has
  `doc/ayanestorm-special-exact-oit-macos.md` as precedent), not only in this plan.
- Do not spawn multiple parallel agents for this work; proceed with single-threaded,
  sequential research/edits.
- Do not attempt to build — the user builds and reports back "bok"/"bokt".

## Research summary (confirmed via code reading)

- **Local-only light object**: No existing precedent creates `LL_PCODE_VOLUME` via
  `LLViewerObjectList::createObjectViewer()` — new territory, but proven by the
  `LLSurfacePatch` pattern
  ([llsurfacepatch.cpp:117-129](indra/newview/llsurfacepatch.cpp#L117-L129)):
  `createObjectViewer(pcode, region)` → `gPipeline.createObject(vobjp)` (creates
  `mDrawable`) → position it. Light setters
  (`setIsLight`, `setLightLinearColor`, `setLightIntensity`, `setLightRadius`,
  `setLightFalloff`, `setLightCutoff`) declared
  [llvovolume.h:262-273](indra/newview/llvovolume.h#L262-L273), implemented
  [llvovolume.cpp:3343-3441](indra/newview/llvovolume.cpp#L3343-L3441).
  `setIsLight(true)` calls `gPipeline.setLight(mDrawable, true)`
  ([llvovolume.cpp:3360](indra/newview/llvovolume.cpp#L3360)) — `mDrawable` must
  exist first, so `gPipeline.createObject()` runs before any light setter. Color
  setters take `LLColor3`; intensity is a separate `F32`. `LLVOVolume` needs
  `setVolume(LLVolumeParams, 0)` to become renderable (pattern in
  [lltoolplacer.cpp:337-406](indra/newview/lltoolplacer.cpp#L337-L406)). No
  "invisiprim" pattern exists; use a tiny-scale (~0.01m) prim with a fullbright,
  alpha-0 face to keep it a light without a visible mesh. **This same creation
  sequence is reused per light**, so with multiple lights each gets its own
  `LLVOVolume`/`mDrawable` instance.
- **Nearby-light pipeline**: `LLPipeline::calcNearbyLights`/`setupHWLights`
  ([pipeline.cpp:6211](indra/newview/pipeline.cpp#L6211),
  [pipeline.cpp:6399](indra/newview/pipeline.cpp#L6399)) gather any
  `LLDrawable::isLight()` object within range into `mNearbyLights` every frame
  automatically — no special-casing needed per light. Deferred rendering has no
  `MAX_LOCAL_LIGHTS` cap; non-deferred does
  ([pipeline.cpp:6352](indra/newview/pipeline.cpp#L6352)) — worth surfacing to the
  user if they add many lights on non-deferred rendering.
- **Floater pattern**: `LLFloaterJoystick` — private ctor + `friend class LLFloaterReg`
  ([llfloaterjoystick.h:40-67](indra/newview/llfloaterjoystick.h#L40-L67)),
  `postBuild()` binds named XUI controls, registered in
  [llviewerfloaterreg.cpp:573](indra/newview/llviewerfloaterreg.cpp#L573) via
  `LLFloaterReg::add("name", "floater_xxx.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<T>)`.
- **Per-frame idle callback**: `gIdleCallbacks.addFunction(fn, this)` /
  `deleteFunction(fn, this)`
  ([llcallbacklist.h:35-78](indra/llcommon/llcallbacklist.h#L35-L78)). Precedent
  `FSFloaterImport`
  ([fsfloaterimport.cpp:107](indra/newview/fsfloaterimport.cpp#L107) add in ctor,
  [:114](indra/newview/fsfloaterimport.cpp#L114) remove in dtor). A single shared
  idle callback iterates all active lights each frame (not one callback per light).
- **Joint tracking**: `LLJoint::getWorldPosition()`/`getWorldRotation()`
  ([lljoint.h:245,254](indra/llcharacter/lljoint.h#L245)) via
  `avatar->getJoint("mPelvis")` (pattern in
  [fsposeranimator.cpp:761](indra/newview/fsposeranimator.cpp#L761)). No existing
  local-object-follows-joint precedent — each light's world position is recomputed
  every idle tick from its anchor joint's current world transform plus its stored
  offset, which is what makes it move continuously with the avatar (walking,
  running, flying, sitting, any animation).
- **Background isolate mode — SUPERSEDED, see "Status update" below for the actual
  shipped design.** (Original research, kept for history: `gPipeline.toggleRenderType`
  and a `glClearColor` override were the first idea; both were replaced after several
  rounds of real-world testing surfaced correctness problems described below.)
- **Freeze animations**: `Advanced.AnimFreeze`
  ([llviewermenu.cpp:2234-2243](indra/newview/llviewermenu.cpp#L2234-L2243)) is
  fire-and-forget: `set_all_animation_time_factors(0.0f)` across all
  `LLCharacter::sInstances` (viewer-wide — user confirmed keep this scope). No
  tracked on/off state exists today; the floater's checkbox becomes the first real
  toggle (off → `set_all_animation_time_factors(1.0f)`); initial state read from
  `LLMotionController::getCurrentTimeFactor() == 0.0f`.
- **Poser floater**: registered as `"fs_poser"`
  ([menu_viewer.xml:6448](indra/newview/skins/default/xui/en/menu_viewer.xml#L6448)),
  opened via `LLFloaterReg::toggleInstance("fs_poser")`.
- **Beacon rendering + automatic snapshot exclusion**: stock beacons are drawn via
  immediate-mode `LLRender` in `LLViewerObjectList::renderObjectBeacons()`
  ([llglsandbox.cpp](indra/newview/llglsandbox.cpp)), invoked each frame from
  `render_hud_elements()`
  ([llviewerdisplay.cpp:1886-1894](indra/newview/llviewerdisplay.cpp#L1886-L1894))
  guarded by `gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI)`.
  That same bit is force-disabled during `LLViewerWindow::rawSnapshot()` capture when
  `show_ui=false` (the normal case)
  ([llviewerwindow.cpp:6206-6211](indra/newview/llviewerwindow.cpp#L6206-L6211)) — so
  anything drawn under that guard is automatically excluded from snapshots, matching
  the "never in snapshots" requirement for free. Sun/moon beacons use simple
  standalone `gSavedSettings` bools
  ([llviewermenu.cpp:11970-11976](indra/newview/llviewermenu.cpp#L11970-L11976)),
  the closer precedent for a single-purpose toggle here (one shared toggle draws
  beacons for all active lights, not `sRenderBeacons`).
- **List UI (add/select/delete) with a backing data store**: `ALFloaterRegionTracker`
  ([alfloaterregiontracker.h/.cpp](indra/newview/alfloaterregiontracker.cpp)) is the
  closest transferable template — `LLScrollListCtrl*` bound in `postBuild()`
  (`getChild<LLScrollListCtrl>("region_list")`,
  [alfloaterregiontracker.cpp:77-80](indra/newview/alfloaterregiontracker.cpp#L77-L80)),
  rows added via `addElement(LLSD, ADD_BOTTOM, userdata)` (simpler form used in
  [llfloatersettingsdebug.cpp:141-149](indra/newview/llfloatersettingsdebug.cpp#L141-L149)),
  selection read via `getFirstSelected()->getValue()`
  ([alfloaterregiontracker.cpp:291](indra/newview/alfloaterregiontracker.cpp#L291)),
  delete button erases the backing entry then calls `deleteSelectedItems()`
  ([alfloaterregiontracker.cpp:216-225](indra/newview/alfloaterregiontracker.cpp#L216-L225)),
  full rebuild via `deleteAllItems()`
  ([alfloaterregiontracker.cpp:117](indra/newview/alfloaterregiontracker.cpp#L117)).
  `LLScrollListCtrl` is declared in
  [llscrolllistctrl.h](indra/llui/llscrolllistctrl.h) with `addElement`,
  `getFirstSelected`, `getAllSelected`, `getNumSelected`, `deleteSelectedItems`,
  `clearRows`/`deleteAllItems`.
- **XML preset save/load**: simplest precedent is
  [fsavatarrenderpersistence.cpp](indra/newview/fsavatarrenderpersistence.cpp) —
  build an `LLSD` (array-of-maps for a list of structured entries, one map per light
  with `name`/`joint`/`offset`/`color`/`intensity`/`radius`/`falloff` keys), write via
  `LLSDSerialize::toPrettyXML(data, ofstream)`
  ([fsavatarrenderpersistence.cpp:104-125](indra/newview/fsavatarrenderpersistence.cpp#L104-L125)),
  read back via `LLSDSerialize::fromXMLDocument(data, ifstream)`
  ([fsavatarrenderpersistence.cpp:77-101](indra/newview/fsavatarrenderpersistence.cpp#L77-L101)).
  File path via `gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "...")` for
  per-account storage, or a `LL_PATH_USER_SETTINGS`-rooted subdirectory (mirroring
  `llpresetsmanager.cpp`'s one-file-per-preset-name convention under a `PRESETS_DIR`
  subfolder) if per-named-preset files are preferred over one combined list file —
  recommend the latter (one file per saved rig preset) so "Save preset..." can prompt
  for a name and "Load preset..." can offer a picker of existing files.
- **XUI slider/color-swatch attributes** (from
  [panel_as_sun_settings.xml:7,10](indra/newview/skins/default/xui/en/panel_as_sun_settings.xml#L7)):
  `<slider can_edit_text="true" show_text="true" text_width="50" decimal_digits="1"
  increment="..." min_val="..." max_val="..." label="..." name="..." />` for
  slider+numeric-box combos; `<color_swatch color="r,g,b,a" can_apply_immediately="true"
  label="" name="..." width="44" height="24" />` for the color picker.

## Implementation plan

### 1. New files (AS-owned, no ownership tags needed, normal comments only)
- `indra/newview/asfloatermylight.h` / `.cpp` — the floater class `ASFloaterMyLight`;
  owns the list of lights, the currently-selected-light editing state, background
  override state, freeze-animation toggle, and preset save/load.
- `indra/newview/aslightrig.h` / `.cpp` — `ASLightRig`, a small struct/class holding
  one light's data (name/id, anchor joint name, distance/height/azimuth offset,
  intensity, radius, falloff, color) plus the live `LLVOVolume` pointer for that
  light, and methods to create/update/destroy its underlying viewer object. This is
  the per-light unit the list panel manages — pulled out of the floater class itself
  so the floater stays focused on UI wiring and the rig logic is independently
  testable/reusable.
- `indra/newview/aspanellightpad.h` / `.cpp` — the placement-pad widget
  `ASPanelLightPad`, editing whichever `ASLightRig` is currently selected.
- `indra/newview/skins/default/xui/en/floater_as_my_light.xml` — layout.
- `doc/ayanestorm-self-light-floater.md` — research/design notes, following the
  existing `/doc` precedent.

### 1b. Edits to existing LL/FS files (must be wrapped in `<AS:Chanayane>` tags,
     original code preserved as comments inside the tags)
- Register in `indra/newview/llviewerfloaterreg.cpp`:
  `LLFloaterReg::add("as_my_light", "floater_as_my_light.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<ASFloaterMyLight>);`
- Add a menu entry under the existing `AyaneStorm` menu
  ([menu_viewer.xml:6430](indra/newview/skins/default/xui/en/menu_viewer.xml#L6430))
  using the same `Floater.Toggle` / `Floater.Visible` pattern as the Poser entry.
- `indra/newview/CMakeLists.txt`: add the new AS-owned source files to the source list
  (as shipped: `asfloatermylight.{h,cpp}`, `aslightrig.{h,cpp}`,
  `asbackgroundisolate.{h,cpp}` — see "Status update" below; `aspanellightpad.*` was
  never implemented, sliders proved sufficient).
- `indra/newview/llviewerdisplay.cpp` (beacon call site, ~1886-1894): minimal
  call-out into `ASFloaterMyLight::renderAllLightBeacons()`.
- `indra/newview/pipeline.cpp` (`LLPipeline::stateSort(LLDrawable*, LLCamera&)`):
  as-shipped isolate-background mode needed one more call-out here — see "Status
  update" below. Not part of the original plan.
- `indra/newview/llviewershadermgr.cpp`: shader lifecycle registration for the
  as-shipped `ASBackgroundIsolate` shader (mirrors the existing `ASLensFlare`/
  `ASVignette` registration pattern already in this file) — also not part of the
  original plan.
- `indra/newview/llviewermenu.cpp`: only if `set_all_animation_time_factors` needs to
  become non-file-local; check first.

### 2. `ASLightRig` (per-light data + object lifecycle)
- Fields: `LLUUID mId` (local identifier for list rows/selection), `std::string mName`
  (user-editable label, e.g. "Key Light", "Fill"), `std::string mAnchorJoint`
  (`"mChest"`/`"mHead"`/`"mPelvis"`), `F32 mDistance/mHeight/mAzimuth`,
  `F32 mIntensity/mRadius/mFalloff`, `LLColor3 mColor`, `LLPointer<LLVOVolume> mObject`.
- `create()`: `gObjectList.createObjectViewer(LL_PCODE_VOLUME, gAgent.getRegion())` →
  `setVolume()` (tiny sphere/box, fullbright alpha-0 face) → `gPipeline.createObject(vobjp)`
  → `setIsLight(true)` → apply all current params.
- `destroy()`: `mObject->markDead()`, clear pointer.
- `updateTransform()`: called every idle tick for every active rig — re-read the
  anchor joint's current world position/rotation, recompute the light's world
  position from `mDistance/mHeight/mAzimuth`, and set it on `mObject`. Called for
  every rig in the list regardless of which one is currently selected/being edited,
  so all lights track the avatar simultaneously, not just the one shown in the
  sliders.
- `applyParams()`: pushes `mIntensity/mRadius/mFalloff/mColor` to the live
  `LLVOVolume` setters; called whenever the floater's controls change for the
  currently-selected rig.
- `toLLSD()`/`fromLLSD()`: (de)serialize this rig's fields for preset save/load.
- `setEnabled(bool)`: toggles the rig's contribution to the scene without destroying
  its object — calls `mObject->setIsLight(enabled)` (cheap, keeps the object/`mDrawable`
  alive and its params intact, just adds/removes it from `LLPipeline::mNearbyLights`
  via the existing `setIsLight` → `gPipeline.setLight()` path) so re-enabling doesn't
  need to recreate anything. Tracks its own `mEnabled` bool so it can be skipped by
  `updateTransform()` (no need to keep repositioning a light that isn't contributing)
  while still existing for the list/editor.

### 3. `ASFloaterMyLight` class responsibilities
- **Ctor**: private, `friend class LLFloaterReg`, matching `LLFloaterJoystick`.
  Holds `std::vector<std::unique_ptr<ASLightRig>> mLights`, an index/id of the
  currently-selected rig for editing, and a single `bool mMasterEnabled` (default
  `true`).
- **postBuild()**: bind sliders/spinners/color swatch/placement pad (edit controls
  for whichever rig is selected), the list panel's `LLScrollListCtrl`, "Add Light",
  "Delete Light", "Load preset...", "Save preset..." buttons, the background combo,
  freeze-animations checkbox, beacon-toggle checkbox, the master enable/disable
  checkbox, and "Open Poser" button.
- **Master enable/disable switch**: a single top-level checkbox/toggle (e.g. "Enable
  My Lights") that turns every light in `mLights` on/off at once via each rig's
  `setEnabled()`, without touching individual per-light state (positions, colors,
  params, and the list itself are preserved — only whether they're currently
  contributing light is affected). This is the quick kill-switch for "I want to see
  the scene without my rig for a moment" without having to delete/re-add lights or
  reopen the floater. Defaults to enabled when lights exist; newly-added lights
  respect the current master state (a light added while the master switch is off
  starts disabled, matching the visible rig state). Placed prominently near the top
  of the floater since it's the most likely single control to be toggled quickly.
- **List panel** (collapsible, extends the floater to the right when opened — a
  toggle button controls a side panel's visibility, matching how collapsible XUI
  panels elsewhere resize a floater):
  - "Add Light" creates a new `ASLightRig` (default Chest preset, default params),
    calls `create()`, adds a row to the scroll list (`addElement`, row `value` =
    the rig's `LLUUID`), and selects it.
  - Selecting a row (`LLScrollListCtrl` commit callback) looks up the matching
    `ASLightRig` by id, loads its fields into the edit sliders/pad/color swatch —
    this is the "selecting a light makes it current for editing" behavior.
  - "Delete Light" reads `getFirstSelected()->getValue()`, finds and calls
    `destroy()` on the matching rig, removes it from `mLights`, and calls
    `deleteSelectedItems()` on the list; if the deleted rig was selected, falls back
    to selecting the first remaining rig (or clears/disables edit controls if the
    list is now empty).
  - Renaming a light: either an inline-editable list column or a text field next to
    the list tied to the selected rig's `mName` — simplest is a name text field
    above/alongside the edit controls, committing to `mName` and refreshing that
    row's list label.
- **Light object lifecycle**: `mLights` is empty until "Add Light" is used, or until
  a preset is loaded (which populates it from the preset file). Floater close/destroy
  calls `destroy()` on every rig and removes the shared idle callback — the whole rig
  is a transient photography aid, not persisted implicitly (only via explicit "Save
  preset...").
- **Shared idle callback** (`gIdleCallbacks.addFunction`, added once in the ctor,
  removed once in the dtor): each frame, calls `updateTransform()` on every rig in
  `mLights` (all lights track the avatar continuously, not just the selected one),
  and handles region-crossing/teleport by recreating every rig's object if
  `gAgent.getRegion()` changed since last tick.
- **Edit controls → currently-selected rig**: distance/height/azimuth sliders (+ pad)
  and intensity/radius/falloff/color controls all read/write the selected
  `ASLightRig`'s fields and call `applyParams()`/reposition immediately on change;
  switching selection in the list swaps which rig the controls are bound to.
- **Placement pad** (`ASPanelLightPad`): direct-mapping drag pad (unlike
  `LLJoystick`'s quadrant/held-rate model) editing the selected rig's
  distance/azimuth; synced bidirectionally with the numeric sliders through one
  shared `updateSelectedLightOffset()` method.
- **Position presets**: "Head" / "Chest" / "Back" buttons set the selected rig's
  joint + canned distance/height/azimuth (Chest: `mChest`, ~0.5m in front, level
  height; Head: `mHead`, ~0.4m in front, head height; Back: `mChest`/`mPelvis`,
  ~0.5m behind, level height). New lights default to the Chest preset.
- **Background combo** (`None` / `All Black` / `All White`) — global to the floater,
  not per-light. **As originally planned this only hid sky/water/clouds/terrain.
  The shipped behavior is a true self-only isolate (avatar + attachments + our
  light-rig objects, EVERYTHING else hidden) — see "Status update" below for the
  full story and the final design.**
- **Freeze Animations checkbox**: on/off calls `set_all_animation_time_factors(0.0f)`
  / `(1.0f)` (the helper already used by `LLAdvancedAnimFreeze`,
  [llviewermenu.cpp:2234](indra/newview/llviewermenu.cpp#L2234) — currently
  file-local `static`; check first, expose via minimal tag-wrapped header move if
  needed rather than duplicating the loop). Initial state from
  `LLMotionController::getCurrentTimeFactor() == 0.0f`.
- **Open Poser button**: `LLFloaterReg::toggleInstance("fs_poser")`.
- **Light position beacons**: one shared toggle draws a beacon for every active rig
  in `mLights` (not per-light toggles).
  - New standalone bool `gSavedSettings "ASRenderLightBeacon"`, mirroring the
    sun/moon-beacon pattern
    ([llviewermenu.cpp:11970-11976](indra/newview/llviewermenu.cpp#L11970-L11976)).
  - `ASFloaterMyLight::renderAllLightBeacons()` (static method, AS-owned) iterates
    `mLights` and draws a 3-axis cross (immediate-mode `LLRender`, matching
    `renderObjectBeacons()`'s idiom in
    [llglsandbox.cpp](indra/newview/llglsandbox.cpp)) at each rig's current world
    position; a directional cone can follow later as a v2 upgrade.
  - Single tag-wrapped call-out near `render_hud_elements()`'s existing beacon call
    ([llviewerdisplay.cpp:1886-1894](indra/newview/llviewerdisplay.cpp#L1886-L1894)):
    ```cpp
    // <AS:Chanayane> draw self-light position beacons under the same UI-debug-feature gate as stock beacons, so they are excluded from snapshots automatically
    if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        ASFloaterMyLight::renderAllLightBeacons();
    }
    // </AS:Chanayane>
    ```
    Rides on the gate already force-disabled during `LLViewerWindow::rawSnapshot()`
    capture ([llviewerwindow.cpp:6206-6211](indra/newview/llviewerwindow.cpp#L6206-L6211)),
    so snapshot exclusion is automatic. The `"ASRenderLightBeacon"` check happens
    inside `renderAllLightBeacons()` itself.
- **Preset save/load** (whole rig — the full light list — as one preset):
  - "Save preset...": prompts for a preset name (simple text-entry dialog, matching
    `llpresetsmanager.cpp`'s save-preset flow), serializes `mLights` to an LLSD array
    of maps (one map per light via `ASLightRig::toLLSD()`), writes via
    `LLSDSerialize::toPrettyXML` to a file named after the preset under a dedicated
    subdirectory (e.g. `gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "as_light_presets", name + ".xml")`).
  - "Load preset...": offers a picker (simplest: a combo/list of existing files in
    that subdirectory, or a native file-open dialog if one is already used elsewhere
    in this codebase for similar pickers — check `llpresetsmanager.cpp` for the
    existing UI pattern to reuse), reads the chosen file via
    `LLSDSerialize::fromXMLDocument`, destroys any currently-active rigs, rebuilds
    `mLights` from the loaded array (`ASLightRig::fromLLSD()` + `create()` per
    entry), and repopulates the list panel.
  - Loading a preset does not touch the background/freeze-animation/beacon toggles —
    only the light list itself.

### 4. XUI layout (`floater_as_my_light.xml`)
Two-column layout: left = light editor, right = collapsible light list panel.

Top of floater, spanning both columns:
0. "Enable My Lights" master checkbox — placed above everything else since it's the
   fastest single toggle a user reaches for.

Left column (edit controls for the selected light), top to bottom:
1. Light name field.
2. Position presets row: "Head" / "Chest" / "Back" buttons.
3. Placement pad (custom widget) + Distance/Height/Azimuth sliders with numeric
   entry, synced to the pad.
4. Intensity / Radius / Softness sliders (numeric entry enabled).
5. Color swatch.

Right column (toggled via an "expand list" button, shown/hidden):
6. Scroll list of lights (name column).
7. "Add Light" / "Delete Light" buttons.
8. "Load preset..." / "Save preset..." buttons.

Floater-global controls (bottom, span both columns):
9. Background combo box (None/All Black/All White).
10. "Show light beacons" checkbox.
11. Freeze Animations checkbox.
12. "Open Poser" button.

## Verification
Per AGENTS.md, the user performs all builds — do not attempt to build. After the
edits are complete, ask the user to build and report back "bok" (build OK) or "bokt"
(build OK and tested at runtime). Once built, in-world checks to ask the user to run
(or perform together interactively):
- Open the floater from the AyaneStorm menu; click "Add Light" twice and confirm two
  independent lights appear on the avatar, each listed by name in the list panel.
- Toggle "Enable My Lights" off and confirm both lights stop contributing light
  immediately (avatar reverts to ambient lighting) while their list entries and
  configured parameters remain intact; toggle back on and confirm both lights
  reappear exactly as configured, with no need to re-add them.
- Select each light in the list and confirm the sliders/pad/color swatch update to
  show that light's own parameters (not the other light's).
- Edit a selected light's sliders/pad/color and confirm only that light changes
  in-world, while the other light stays as configured.
- Delete one light from the list and confirm its light disappears in-world while the
  other remains, and the list/edit-controls fall back sensibly.
- Confirm all remaining lights follow the avatar continuously when walking, running,
  sitting, and across a local teleport/region crossing, with no stale lights left
  behind.
- Save a preset with 2+ lights, delete all lights via the UI, then load the preset
  back and confirm the same lights (names, positions, colors, params) are recreated.
- Toggle background modes and confirm sky/water/clouds/terrain disappear/reappear
  correctly with `None` restoring the normal scene.
- Toggle Freeze Animations and confirm avatars stop/resume animating.
- Click Open Poser and confirm `fs_poser` floater opens/toggles.
- Toggle "Show light beacons" and confirm a cross gizmo appears at each light's
  position; take a snapshot (UI hidden, the normal case) while beacons are visible
  on-screen and confirm none appear in the captured image.
- Close the floater and confirm every light object and any render-type/clear
  overrides are cleanly removed (no leftover lights or black/white screen after
  closing).

## Status update (post-implementation, superseding several sections above)

Everything above this section is the original plan as approved before implementation
started. Real in-world testing surfaced correctness problems in the original
background-isolate design that required three successive redesigns. This section is
the authoritative description of what actually shipped; treat any conflict between
this section and the text above as this section winning.

**Shipped, not originally planned:**
- No placement pad / `ASPanelLightPad` — sliders with numeric entry (Distance/
  Height/Azimuth) proved sufficient; the pad was dropped, not built.
- Master "Enable My Lights" switch state persists across sessions
  (`ASLightRigMasterEnabled` in `settings.xml`), added after initial ship per a
  later explicit request.
- Lights (and isolate-background mode) now survive closing/reopening the floater —
  only destroyed/reset on an actual viewer quit, not on a plain close. The full rig
  also auto-saves to a per-account file on quit and auto-loads on next login
  (`ASFloaterMyLight::getAutosaveFilename()`), independent of the named-preset
  save/load feature.
- New toolbar command `as_my_light` (`app_settings/commands.xml`,
  `Command_ASMyLight_Icon`) with a custom hand-drawn 18×18 icon
  (`toolbar_icons/my_light.png`), matching the existing AS command style (e.g.
  `as_sun_settings`).
- The light-position beacon's color now matches each light's own configured color
  instead of a fixed yellow.

**Background isolate mode — three redesigns, final architecture:**

1. *Original plan (never shipped as designed)*: toggle
   `RENDER_TYPE_SKY/WATER/CLOUDS/TERRAIN` off and override the frame `glClearColor`.
   Abandoned before ship: this only ever hid sky/water/clouds/terrain, not other
   avatars or other people's builds — nowhere near "isolate the self avatar," and
   disabling `RENDER_TYPE_SKY` specifically broke `LLVOSky::updateSky()`'s
   atmospherics refresh and corrupted `LLReflectionMapManager`'s baked ambient/
   irradiance cubemaps (the global render-type mask also suppresses sky during the
   probe manager's own internal capture passes), producing a camera-reactive magenta
   tint on every surface.

2. *Redesign 1 — depth-tested full-screen quad + per-object `FORCE_INVISIBLE`
   hiding.* Replaced the `glClearColor` override with a solid-color quad drawn via
   `gSolidColorProgram`, depth-tested against the opaque scene so the avatar
   naturally occludes it (no render-type disabling, so sky/probes render normally
   under the hood while being painted over on screen). Other avatars were hidden via
   `LLVOAvatar::setVisualMuteSettings(AV_DO_NOT_RENDER)` (a real per-instance
   mechanism); non-avatar objects (other people's prims/mesh) were hidden via
   `LLPipeline::hideObject()` (`LLDrawable::FORCE_INVISIBLE`), the same mechanism the
   pathfinding floaters use, re-applied every idle tick because
   `LLViewerObject::processUpdateMessage()` silently clears `FORCE_INVISIBLE` on any
   routine inbound object-update packet (terse updates, interest-list refreshes) —
   a one-shot hide gets undone within a frame or two of normal sim traffic.
   Root-caused but never fully fixed: leftover fragments (terrain edges, distant
   trees, parts of nearby buildings) stayed stubbornly visible, and objects would
   not reliably reappear when switching back to "None" without the user manually
   zooming the camera out and back in to force a fresh cull/redraw. Multiple research
   passes confirmed `FORCE_INVISIBLE` itself is correctly respected by
   `LLPipeline::stateSort`'s per-frame visibility gate and that occlusion/octree
   culling re-evaluates fully every frame regardless of camera movement — the
   remaining bugs were never fully pinned down before the approach was abandoned in
   favor of a structurally different fix.
   Also discovered and fixed in this phase, independent of the above: the isolate
   quad originally drew too early in the pipeline (right after `renderGeomDeferred()`,
   into the deferred G-buffer), so later lighting/tonemap/bloom/vignette passes
   re-tinted the flat color (black wasn't fully black under volumetric lighting,
   white showed a gradient from bloom/vignette) — this part of the fix (draw the
   backdrop after the whole post-process chain) carried forward into the final
   design below.

3. *Redesign 2 — live per-frame allowlist in `LLPipeline::stateSort`, but only
   early-returning (no FORCE_INVISIBLE) — turned out to be a no-op for ordinary
   prims/mesh.* The user's own suggestion after redesign 1 kept failing: instead of
   hiding a denylist of "everything else" (fighting the sim's own update traffic
   and the pipeline's cull/visibility caching), allowlist only what should render:
   self avatar, self attachments, and our own light-rig objects.
   `ASBackgroundIsolate::shouldHideDrawable(LLDrawable*)`
   (`indra/newview/asbackgroundisolate.{h,cpp}`) was called once per drawable per
   frame from a new call-out in `LLPipeline::stateSort(LLDrawable*, LLCamera&)`
   (right after the existing `LLSelectMgr::mHideSelectedObjects` early-out) and, if
   the drawable should be hidden, simply `return`ed early from `stateSort` without
   drawing it.
   This looked promising in early testing (most of a test scene hid correctly) but
   turned out to be **fixing the wrong thing**: for ordinary prim/mesh ("volume")
   drawables, `stateSort(LLDrawable*, camera)` only calls `setVisible()` and (for
   non-volume drawables only) enqueues faces directly — for volumes,
   `drawablep->getVOVolume()` is true, so the immediate-mode face-enqueue path is
   skipped entirely (`pipeline.cpp` ~3589, `if (!drawablep->getVOVolume())`).
   Volume geometry instead draws from `LLSpatialGroup::mDrawMap`, a persistent
   batch built once per group by `LLVolumeGeometryManager::rebuildGeom()`
   (`llvovolume.cpp`), completely independent of that frame's `stateSort()`
   outcome. **Early-returning from `stateSort` therefore had zero effect on volume
   rendering** — the apparent partial success in testing was actually just the
   solid-color backdrop pass (see below) painting over background-depth pixels,
   not real object hiding. This was only discovered by direct real-world log
   evidence (see redesign 4).
   A second, compounding bug found in this phase: `LLSpatialGroup::changeLOD()`
   gates whether a `render_by_group` group (`PARTITION_VOLUME`, i.e. ordinary
   prims/mesh — includes large static builds like houses) ever recurses into the
   per-drawable `stateSort()` overload at all (`LLPipeline::stateSort(LLSpatialGroup*,
   LLCamera&)`, `pipeline.cpp` ~3462). For a static camera that hasn't crossed the
   group's ~25% distance-change slop ratio, `changeLOD()` stays false indefinitely,
   so static geometry (a house, landscaping, a wall) never got visited by our
   filter at all — explaining "always the same objects" staying visible regardless
   of isolate mode. Fixed by forcing the group's inner loop to always run while
   `ASBackgroundIsolate::isActive()`, independent of `changeLOD()`'s answer
   (the original distance-bookkeeping is preserved, only gated on the real
   `changeLOD()` result, not the forced one).
   A third, separate bug found via GPU occlusion culling: `LLPipeline::stateSort(
   LLCamera&, LLCullResult&)`'s visible-groups loop (`pipeline.cpp` ~3423) routes
   any group whose GPU occlusion query returned `OCCLUDED` into `markOccluder()`
   instead of `stateSort(group, camera)` — skipping our filter entirely for
   occluded groups. Room-scale/static geometry is occluded by other room-scale/
   static geometry very commonly (indoors especially), so most of a scene could
   get stuck in a stale, partially-hidden state. Fixed by also bypassing the
   `OCCLUDED` branch while isolate mode is active (the underlying `checkOcclusion()`
   GPU query itself is untouched, so it resumes normally the instant isolate mode
   turns off).

4. *Redesign 3 / final design (shipped, confirmed working) — same live allowlist,
   but actually setting `LLDrawable::FORCE_INVISIBLE` + forcing a real geometry
   rebuild.* Even after fixing both the `changeLOD()` and occlusion gates above, a
   subtle bug remained that took direct log-evidence debugging to find: real
   in-world testing captured specific object UUIDs that were confirmed via log
   output to have `FORCE_INVISIBLE` correctly set (`should_hide=1` logged,
   `setState`/`markRebuild` both called) yet **stayed visually rendered anyway**.
   Root cause: `LLPipeline::markRebuild(LLDrawable*, REBUILD_ALL)` only queues the
   *drawable's own* `updateGeometry()` (a no-op for a static prim) — it never
   touches the drawable's owning `LLSpatialGroup`. `LLVolumeGeometryManager::
   rebuildGeom()` (the function that actually reads `FORCE_INVISIBLE` when
   rebuilding `mDrawMap` — confirmed real, current code at `llvovolume.cpp`
   ~6017) is gated inside `LLSpatialPartition::rebuildGeom(group)` on the *group*
   carrying `LLSpatialGroup::GEOM_DIRTY`, which is a completely separate flag from
   anything `markRebuild(drawable, ...)` touches. Without it, a static group just
   keeps drawing from its previously-built `mDrawMap` forever, no matter how many
   times `FORCE_INVISIBLE` is toggled or the drawable-level rebuild is requested.
   **The fix**, in `ASBackgroundIsolate::updateDrawableHiddenState()`
   (`asbackgroundisolate.cpp`): alongside the existing
   `gPipeline.markRebuild(drawable, REBUILD_ALL)`, also call
   `drawable->getSpatialGroup()->dirtyGeom()` and `gPipeline.markRebuild(group)` —
   the same pairing `restoreAllHiddenDrawables()` uses on the way back out. This
   was confirmed fixed by a real in-world test (user-provided object UUIDs
   cross-referenced against diagnostic log output before AND after this specific
   fix) — the previous three sub-fixes (early-return, changeLOD bypass, occlusion
   bypass) were each real, necessary, and individually confirmed-insufficient
   steps toward this one.
   Final architecture, all pieces together:
   - `ASBackgroundIsolate::shouldHideDrawable(LLDrawable*)` — the live per-frame
     classification: allows the self avatar (`obj->asAvatar()->isSelf()`), any
     object whose `getAvatarAncestor()` is the self avatar (worn attachments,
     including rigged mesh clothing — critically, `RENDER_TYPE_VOLUME` is shared
     by every `LLVOVolume` drawable including self attachments, so a
     render-type-level exclusion can never distinguish "my attachment" from
     "someone else's object"; per-object identity is required), and any object id
     in a small exempt set kept current via `ASBackgroundIsolate::setLightRigIds()`
     (called from `ASFloaterMyLight::updateLights()`'s idle callback). Everything
     else returns `true` (should be hidden).
   - `ASBackgroundIsolate::updateDrawableHiddenState(LLDrawable*)` — called from a
     tag-wrapped call-out in `LLPipeline::stateSort(LLDrawable*, LLCamera&)`
     (`pipeline.cpp`, right after the existing `LLSelectMgr::mHideSelectedObjects`
     early-out). Sets/clears `FORCE_INVISIBLE` to match `shouldHideDrawable()`'s
     live answer and, on any state change, forces both a drawable-level and a
     group-level geometry rebuild (see fix above) so it takes effect the same
     frame. Recomputed live every single call, so it can never go stale on its own
     and self-heals against the sim's routine object-update traffic silently
     clearing `FORCE_INVISIBLE` (`LLViewerObject::processUpdateMessage()`).
   - `LLPipeline::stateSort(LLSpatialGroup*, LLCamera&)` (`pipeline.cpp`) — forces
     its inner per-drawable loop to always run while
     `ASBackgroundIsolate::isActive()`, bypassing the normal `changeLOD()` gate so
     static/unmoving groups still get visited every frame (original distance
     bookkeeping preserved, gated on the real `changeLOD()` result only).
   - `LLPipeline::stateSort(LLCamera&, LLCullResult&)`'s visible-groups loop
     (`pipeline.cpp`) — also bypasses the `OCCLUDED`/`markOccluder()` branch while
     isolate mode is active, so occluded static geometry (extremely common
     indoors) still reaches the filter every frame; the underlying occlusion query
     itself (`checkOcclusion()`) is untouched and resumes normally once isolate
     mode turns off.
   - `ASBackgroundIsolate::restoreAllHiddenDrawables()` — called right after
     `setActive(false, ...)` turns isolate mode off (from both
     `ASFloaterMyLight::onBackgroundModeChanged()`'s "None" branch and
     `onClose(true)` on viewer quit). Explicitly walks every object id this module
     hid (tracked in a small set) and clears `FORCE_INVISIBLE` + forces the same
     drawable+group rebuild pairing directly, rather than depending on incidental
     future traversal of that object's group — this is what makes turning isolate
     mode off immediate and reliable rather than needing a camera nudge.
   - `ASBackgroundIsolate::setActive(bool, LLColor4)` also toggles a small, fixed
     set of render types wholesale: `RENDER_TYPE_TERRAIN`, `RENDER_TYPE_WATER`,
     `RENDER_TYPE_VOIDWATER`, `RENDER_TYPE_WATEREXCLUSION`, `RENDER_TYPE_CLOUDS`,
     `RENDER_TYPE_GRASS`, `RENDER_TYPE_PARTICLES`. These render through entirely
     separate geometry managers (`LLSurfacePatch` for terrain, `LLVOWater`,
     `LLVOGrass`, the particle system) that were confirmed (by grepping for
     `FORCE_INVISIBLE` in `llsurfacepatch.cpp` — zero hits) to never check
     `FORCE_INVISIBLE` at all, so no amount of per-object flagging can hide them;
     unlike `RENDER_TYPE_VOLUME`/`RENDER_TYPE_AVATAR`, none of these can ever be
     "mine," so disabling them wholesale for the whole isolate-mode duration is
     always safe. `RENDER_TYPE_SKY`/`RENDER_TYPE_WL_SKY` are deliberately still
     excluded from this list (see redesign 1's magenta-tint root cause) — the sky
     dome keeps rendering normally and is simply painted over by the backdrop pass.
   - `ASBackgroundIsolate::isActive()` also gates `ASLensFlare`, `ASVignette`, and
     `ASVolumetricLighting::isEnabled()` so none of those draw over the isolate
     backdrop during isolate mode (but behave completely normally when isolate
     mode is "Normal Scene" — this bypass is scoped strictly to Black/White/
     Custom, never affects normal rendering). Moon halo and procedural sun
     render as part of the sky-dome geometry itself (not a separate post-pass),
     so they need no separate bypass — the backdrop already covers the sky.

5. *Redesign 5 (shipped, confirmed working) — backdrop moved from a late
   depth-sampling shader pass to an early color-buffer base-layer fill, fixing a
   real-world-confirmed hair rendering bug.* After redesign 4 shipped and was
   confirmed working for hiding, a real screenshot comparison caught a genuine
   regression: chunks of the self avatar's hair (alpha-blended rigged-mesh strand
   tips, rendered through this codebase's ExactOIT/AVBOIT order-independent-
   transparency systems) were being painted over by the solid isolate color, in
   both black and white modes, and — confirmed via a follow-up screenshot with
   standard (non-OIT) alpha rendering too — this wasn't OIT-specific at all.
   Root cause: the backdrop was a shader pass (`asBackgroundIsolateF.glsl`) drawn
   at the very end of `LLPipeline::renderFinalize()`, testing
   `gPipeline.mRT->deferredScreen`'s depth to decide "was anything opaque drawn
   here" — but alpha-blended geometry (hair, particles, glass) never writes
   depth in this pipeline (confirmed: `lldrawpoolalpha.cpp` uses
   `LLGLDepthTest(GL_TRUE, GL_FALSE)`, and neither ExactOIT's `composite()` nor
   AVBOIT's `finishDirectFrame` write depth for their resolved output either —
   standard, correct alpha-blending behavior in virtually any renderer, not a
   bug in those systems). So a translucent-only pixel (hair against open sky,
   nothing opaque behind it) read as far-plane/empty depth, indistinguishable
   from genuinely empty background, and got incorrectly painted over. A
   follow-up research pass also confirmed the alpha channel of `mRT->screen`
   (the obvious alternative "was anything drawn here" signal) is not usable
   either — it's repurposed as a glow/bloom-intensity accumulator from the
   moment it's first populated (`softenLightF.glsl` unconditionally zeroes it
   for every opaque pixel including sky), not a coverage mask, and by the time
   `ASLensFlare`/`ASVignette`/the old backdrop pass run, `sourceBuffer` has
   already been blitted to the default framebuffer — there's no render target
   left to sample color+alpha from at all at that point.
   **The fix**: invert the approach entirely. `ASBackgroundIsolate::renderBaseLayer()`
   now fills `mRT->screen` with the isolate color as a **base layer**, called
   from a new tag-wrapped call-out in `LLPipeline::renderDeferredLighting()`
   (`pipeline.cpp`) immediately after `screen_target->clear(GL_COLOR_BUFFER_BIT)`
   — i.e. right when the buffer is empty for the frame — and *before*
   atmospherics and the alpha-forward pass composite on top of it. This uses the
   original fixed-function `LLGLDepthTest(GL_TRUE, GL_FALSE, GL_LEQUAL)` quad
   technique from redesign 1 (depth-tested against the opaque depth already
   populated by `renderGeomDeferred()`, depth writes off so it doesn't perturb
   what atmospherics/alpha still need to test against), reusing the existing
   `gSolidColorProgram`. Since our fill now happens *before* hair/particles/
   glass are drawn, ordinary alpha blending naturally composites them on top of
   it exactly as it would against a real sky — no coverage test needed at all,
   and the earlier redesign-2 tonemap/bloom/vignette-gradient problem stays
   fixed for the same reason redesign 2 fixed it (our fill still isn't affected
   by post-process re-tinting in a way that matters, since it's now legitimately
   part of the lit scene those effects are *supposed* to apply to uniformly).
   The now-unused GLSL shader (`asBackgroundIsolateF.glsl`) and its
   `llviewershadermgr.cpp` register/create/unload lifecycle wiring were removed
   entirely — `renderBaseLayer()` needs no custom shader, just the stock
   `gSolidColorProgram` already used elsewhere in this codebase.

**Open follow-up requests (not yet implemented):**
- ~~Background isolate color limited to Black/White~~ — done: a "Custom" combo
  entry plus a color swatch (`as_light_background_color` in the XUI) now lets
  the user pick any color, applied via the same `ASBackgroundIsolate::setActive(
  bool, const LLColor4&)` mechanism the two presets already used.

6. *Redesign 6 (in progress) — reverted redesign 5's early base-layer fill back to
   a late depth-tested pass, fixed real bugs it exposed, found and fixed a
   star-leak regression, and hit a genuine remaining exact-color-under-hair
   limitation that is now paused pending a decision.*

   Redesign 5's early base-layer fill (writing the isolate color into
   `mRT->screen` *before* tonemap/exposure) turned out to break exact color
   reproduction: `mRT->screen` is linear HDR, and the auto-exposure system
   (`LLPipeline::generateExposure()`) computes this frame's exposure scalar
   *from this very buffer's content* after the fill has already happened, and
   the tonemap curves (`RenderTonemapType`: PBR Neutral or ACES Hill, in
   `tonemapUtilF.glsl`) are nonlinear — so there is no single linear value that
   reliably reproduces a specific requested sRGB color once exposure and
   tonemap are applied. Confirmed via screenshots: white and custom background
   colors rendered visibly dark/wrong, while black (a fixed point of both
   curves at exposure-independent zero) still looked correct by coincidence.

   **Reverted to the late-pass design** (`ASBackgroundIsolate::render()`,
   `asBackgroundIsolateF.glsl` restored, called from the end of
   `LLPipeline::renderFinalize()` again, after the full post-process chain) —
   this is exact by construction, since it paints the requested color directly
   into the already-tonemapped LDR framebuffer. Its depth test
   (`step(0.999, depth)` against `mRT->deferredScreen`) correctly distinguishes
   "background" from "opaque scene geometry" for ordinary opaque content, but
   — same root cause redesign 5 was created to fix — alpha-blended content
   (hair, particles) never writes depth in this pipeline
   (`lldrawpoolalpha.cpp` uses `LLGLDepthTest(GL_TRUE, GL_FALSE)`), so hair
   pixels read as "background" and got painted over by the solid color again.

   **Fix for hair depth specifically**: rather than restructure the whole
   pipeline's color/tonemap handling again, made the viewer's two independent
   order-independent-transparency systems — ExactOIT and AVBOIT, both
   `#if !LL_DARWIN`-gated with stub fallbacks for macOS, both AS-owned despite
   their `fs` prefix — write a near-plane depth for whatever alpha content
   they actually composited, but *only* while isolate mode is active:
   - **ExactOIT** (`fsexactoit.cpp`, `exactOITCompositeF.glsl`): added a third
     draw pass (`oitPass == 3`) in `FSExactOIT::composite()`, run strictly
     after the normal two-pass sort+blend completes, with
     `gGL.setColorMask(false, false)` so it can never affect color. Per pixel,
     it reads the same `oitHeadPointers` linked-list head already used by the
     normal passes: `discard`s where `head == OIT_NULL` (no coverage; a
     `discard`ed fragment writes neither depth nor color, so this can never
     corrupt anything), otherwise writes `gl_FragDepth = 0.0`. Two real bugs
     were caught and avoided before landing on this design: (1) an earlier
     attempt wrote `gl_FragDepth = gl_FragCoord.z` unconditionally as a
     "preserve existing depth" default — wrong, because for this full-screen
     *triangle* (not the real scene geometry) `gl_FragCoord.z` is a fixed,
     uniform NDC-space value across the whole screen, not the pre-existing
     scene depth, so this stomped depth everywhere with a single wrong flat
     value; (2) reading back `mRT->deferredScreen`'s existing depth via
     `texelFetch` to explicitly echo it on non-coverage pixels was considered
     and rejected — that attachment is simultaneously bound for writing in the
     same draw call, which is an undefined/illegal read-after-write hazard.
     Confirmed working: user screenshots showed standard rendering and
     ExactOIT both producing exact background colors with correct hair
     transparency (mostly — see the star-leak paragraph below).
   - **AVBOIT** (`fsavboit.cpp`, new `avboitIsolateDepthF.glsl`): AVBOIT's
     resolve step is a compute shader (`gAVBOITResolveProgram`, dispatched
     from `FSAVBOIT::finishDirectFrame()`) that writes color via
     `glBindImageTexture`+`imageStore` — compute shaders cannot `imageStore`
     into a depth-format texture, so the ExactOIT technique (an extra pass of
     the *same* shader) doesn't transfer directly. Instead, added a small
     ordinary fragment-shader pass (`gAVBOITIsolateDepthProgram`) run right
     after the compute resolve dispatch (still inside `finishDirectFrame()`,
     while `mRT->screen` — which shares its depth attachment with
     `mRT->deferredScreen` via `shareDepthBuffer()`, confirmed in
     `LLPipeline::allocateScreenBuffer()` — is still the bound framebuffer),
     sampling AVBOIT's own per-pixel coverage textures
     (`sResources.accumulatedWeight`, `GL_R16F`; `sResources.accumulatedColorGlow`,
     `GL_RGBA16F`) as ordinary `sampler2D`s via `texelFetch` (they're real GL
     textures, not just compute images, so this is legal outside the compute
     dispatch). Same discard-or-near-plane-depth logic as ExactOIT's pass 3.
     Confirmed the shader itself compiles successfully (log: "Loaded cached
     binary for shader: AVBOIT Isolate Depth"), and confirmed via code reading
     that it draws into the correct, correctly-shared depth attachment (an
     earlier hypothesis that it might be targeting the wrong bound framebuffer
     was checked and ruled out) — the mechanism itself is sound.

   **Star-leak regression, found and fixed.** After the above depth-write
   fixes, the user reported (with screenshots) faint white dots visible inside
   hair strands even against a white background, and separately "I can see
   stars in the transparent parts of the hair" more broadly, for *both*
   standard rendering and ExactOIT (i.e. not an OIT-specific bug). Root cause:
   `ASBackgroundIsolate::setActive()` deliberately leaves `RENDER_TYPE_SKY`/
   `RENDER_TYPE_WL_SKY` enabled during isolate mode (disabling them wholesale
   was redesign 1's original approach and caused a magenta ambient-lighting
   tint bug, because `LLVOSky::updateSky()` early-returns and stops refreshing
   its atmospherics cache when `RENDER_TYPE_SKY` is off, and this cache feeds
   the deferred lighting shaders directly) — so the sky dome, including stars
   (`LLDrawPoolWLSky::renderStarsDeferred()`), keeps rendering normally into
   `mRT->screen` every frame, same as outside isolate mode. The late backdrop
   pass only repaints pixels whose *depth* reads as fully background; it
   cannot retroactively fix color that alpha-blended hair already composited
   *against* sky/star color earlier in the frame (forward alpha rendering
   happens well before the late backdrop pass, and blending is irreversible —
   the sky color is baked into the result the moment hair blends over it).
   So any hair pixel with partial alpha coverage necessarily shows a trace of
   whatever was behind it at blend time, and that was the actual sky (with
   stars), not the isolate color.
   **Fix**: added a narrow, tag-wrapped bypass directly inside
   `LLDrawPoolWLSky::renderDeferred()` (`lldrawpoolwlsky.cpp`) — while isolate
   mode is active, this function now returns immediately after its existing
   `RENDER_TYPE_SKY`/null checks, before drawing the sky dome, heavenly
   bodies, stars, aurora, or clouds. This is a different, narrower lever than
   toggling `RENDER_TYPE_SKY` itself: that render-type flag stays on (so
   `updateSky()` keeps refreshing its cache, and `LLReflectionMapManager`'s
   own internal probe captures — which force sky on via their own
   `pushRenderTypeMask()`/`andRenderTypeMask()`/`popRenderTypeMask()`
   regardless of outside state, confirmed in `llreflectionmapmanager.cpp` —
   are unaffected either way), only this one draw call is skipped. Not yet
   confirmed by the user with a fresh build (this fix landed at the very end
   of the session), but the logic directly matches the confirmed root cause
   and the screenshot evidence (two isolated star dots, not widespread
   corruption).

   **Remaining open problem: exact color under partially-transparent hair.**
   Independent of the star leak, the user also reported (screenshot) that
   white background specifically "is not correct, transparency is weird" —
   this is a *different*, deeper issue than stars: even with sky/stars fully
   suppressed, hair's semi-transparent pixels now blend against whatever *is*
   left in `mRT->screen` at blend time, which is near-`(0,0,0,0)` (the
   frame's initial clear color) rather than the isolate color itself, since
   nothing fills that buffer with the isolate color before hair renders in
   this (reverted-to) late-pass design. This produces visible dark/black
   fringing at hair edges against light backgrounds (white, light custom
   colors) — physically correct in the sense that alpha blending against
   whatever's actually there is well-defined, but not what the user needs for
   a clean photography backdrop.
   Three fix approaches were scoped and presented to the user, who asked to
   pause here and document rather than pick one yet:
   1. **Full tonemap-inversion early fill** — reintroduce an early color fill
      (like redesign 5's, but *only* for background-depth pixels, using the
      existing late pass's `mRT->deferredScreen` depth test, applied right
      after the atmospheric soften pass and before local lights/forward alpha
      in `LLPipeline::renderDeferredLighting()`) that inverts the *exact*
      tonemap+exposure+gamma pipeline (there are 6 real shader variants —
      `NO_POST` × `GAMMA_CORRECT` × `LEGACY_GAMMA` combinations, selected at
      runtime by `gSnapshotNoPost`/build-tools-open/probe-ambiance checks in
      `LLPipeline::tonemap()`) via a numeric (e.g. bisection) per-channel
      inversion, using last frame's exposure scalar (`mExposureMap`, a 1x1
      `GL_R16F` target — still holds last frame's value at this point in the
      frame, since `generateExposure()` doesn't overwrite it until
      `renderFinalize()`, later). Most correct — hair would blend against a
      value that reproduces the exact requested color after this frame's real
      tonemap runs — but duplicates real tonemap logic in a second shader
      that must be kept in sync if the tonemap pipeline ever changes.
   2. **Simple un-inverted linear fill** — same early fill, but store
      `isolate_color / last_frame_exposure` directly with no tonemap
      inversion. Exact for pure black (a fixed point of both tonemap curves
      at any exposure); close-but-not-exact for white/custom (both curves are
      compressive near 1.0, so pure linear white doesn't tonemap back to
      pure white except coincidentally). Much simpler, no duplicated tonemap
      math, small but real color error on non-black backgrounds.
   3. **Leave as-is (current state)** — keep only the late pass; accept
      dark/black fringing on hair edges against non-black isolate colors as a
      known limitation.
   **Implemented follow-up after daylight-EEP screenshot review:** option 2's
   hybrid base-layer approach is now active. `ASBackgroundIsolate::renderBaseLayer()`
   runs immediately after the deferred atmospheric soften pass and before local
   lights/forward alpha/ExactOIT/AVBOIT. It replaces only far-depth background
   pixels in `mRT->screen`, so transparent hair composites against the requested
   isolate color instead of preserving the cyan/white daylight atmosphere. The
   existing late post-tonemap `render()` remains authoritative for fully uncovered
   pixels, keeping the visible solid background exact; only partially transparent
   pixels can expose the early HDR color's tonemap difference. The safe separate
   ExactOIT pass 3 and AVBOIT coverage-depth pass remain in place so the late pass
   preserves those already-correctly-composited hair pixels. The narrow WL-sky draw
   bypass also remains to prevent stars and sky geometry from entering the buffer.

   Follow-up runtime testing exposed two implementation bugs. First, the early
   base-layer draw initially wrote alpha as well as RGB; `mRT->screen` alpha is the
   glow accumulator, so white/custom backgrounds became full-screen bloom emitters
   and produced severe overexposure/colored halos. The base layer now masks alpha
   writes and seeds RGB only. Second, AVBOIT's isolate-depth pass ran while
   `gAVBOITOpaqueTarget` was still bound, updating its private depth copy rather than
   the screen target's shared scene depth. `finishDirectFrame()` now flushes the
   private target after compute resolve and restores the caller's screen target
   before drawing isolate coverage.

   A final comparison against the normal scene showed an extra highlight whose
   position changed with sun elevation and azimuth. This was not exposure: hiding the room via
   `FORCE_INVISIBLE` also removed its walls/roof from the sun shadow draw maps, so
   direct sunlight that the room normally occluded reached the avatar. During
   `LLPipeline::sShadowRender`, `updateDrawableHiddenState()` now temporarily
   restores isolated scene drawables so they remain shadow casters; the ordinary
   camera pass hides them again. Thus the scene stays visually absent while its
   real sun occlusion is preserved.
