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
- **Background isolate mode**: `gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_SKY /
  RENDER_TYPE_WATER / RENDER_TYPE_CLOUDS / RENDER_TYPE_TERRAIN)`
  ([pipeline.h:433](indra/newview/pipeline.h#L433), example call site
  [llmaniptranslate.cpp:1723-1751](indra/newview/llmaniptranslate.cpp#L1723-L1751)).
  The unconditional main-frame clear-color hook is
  [llviewerdisplay.cpp:936](indra/newview/llviewerdisplay.cpp#L936)
  (`glClearColor(0.f, 0.f, 0.f, 0.f)`, runs every normal frame, not snapshot-only).
  Disabling `RENDER_TYPE_SKY` plus overriding this clear color to solid black/white
  produces a correctly depth-tested isolate background with the avatar on top — no
  custom fullscreen quad needed. No `RENDER_TYPE_GROUND`; `RENDER_TYPE_TERRAIN`
  covers land.
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
- `indra/newview/CMakeLists.txt`: add the new AS-owned source files to the source list.
- `indra/newview/llviewerdisplay.cpp:936`: minimal call-out before the existing
  `glClearColor` into `ASFloaterMyLight::getBackgroundClearOverride(color)`.
- `indra/newview/llviewerdisplay.cpp` (beacon call site, ~1886-1894): minimal
  call-out into `ASFloaterMyLight::renderAllLightBeacons()`.
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
  not per-light:
  - `None`: ordinary unmodified scene — re-enable
    `RENDER_TYPE_SKY/WATER/CLOUDS/TERRAIN` and clear the override state.
  - `All Black` / `All White`: `toggleRenderType` off for `RENDER_TYPE_SKY`,
    `RENDER_TYPE_WATER`/`RENDER_TYPE_VOIDWATER`, `RENDER_TYPE_CLOUDS`,
    `RENDER_TYPE_TERRAIN`; set override color state.
  - State lives in `ASFloaterMyLight` via a static accessor
    `getBackgroundClearOverride(LLColor4& out)`. Only touch to upstream code is the
    tag-wrapped call-out at
    [llviewerdisplay.cpp:936](indra/newview/llviewerdisplay.cpp#L936):
    ```cpp
    // <AS:Chanayane> allow the self-light floater to override the frame clear color for isolate-background photography
    // glClearColor(0.f, 0.f, 0.f, 0.f);
    LLColor4 as_clear_override;
    if (ASFloaterMyLight::getBackgroundClearOverride(as_clear_override))
    {
        glClearColor(as_clear_override.mV[0], as_clear_override.mV[1], as_clear_override.mV[2], as_clear_override.mV[3]);
    }
    else
    {
        glClearColor(0.f, 0.f, 0.f, 0.f);
    }
    // </AS:Chanayane>
    ```
  - Other avatars/objects remain visible; only sky/water/clouds/terrain suppressed.
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
