# AyaneStorm Inventory Rules

User-defined rules that find inventory items or folders by name and move them, move them to Trash, or delete them permanently. Native viewer feature (no LEAP).

## Research notes

### LEAP (secondlife/leap)
- Enabled in this tree: `--leap <cmd>` (`cmd_line.xml` → `LeapCommand`), launched in `llappviewer.cpp` via `LLLeap::create`.
- `LLInventory` LEAP API (`llinventorylistener.cpp`) is read-only (getItemsInfo, getDirectDescendants, collectDescendantsIf...). No move/link/delete. Lua (`llluamanager`) is not in this tree.
- Not used for this feature.

### Inventory write paths and completion signals
- `LLInventoryModel::changeItemParent` / `changeCategoryParent`: local model update + UDP `MoveInventoryItem` / `MoveInventoryFolder` via `gAgent.sendReliableMessage()`. No completion callback.
- `LLMessageSystem::sendReliable(host, retries, ping_based, timeout, callback, data)`: callback fires on circuit ack with `LL_ERR_NOERR`, or `LL_ERR_TCP_TIMEOUT` / `LL_ERR_CIRCUIT_GONE` (`llcircuit.cpp`). Used to confirm each move.
- `LLInventoryModel::removeItem` / `removeCategory`: move to Trash (restamp=true), category version deactivates active gestures.
- `remove_inventory_object(id, LLInventoryCallback)`: permanent delete through AIS (`AISAPI::RemoveItem` / `RemoveCategory`). AIS calls the callback once, success or failure (`llaisapi.cpp`); success = object gone from `gInventory` afterwards.
- `gInventory.createNewCategory(parent, FT_NONE, name, callback)`: callback receives the new folder id (null on failure).
- `LLInventoryModelBackgroundFetch::instance().start()` + `isEverythingFetched()`: full inventory fetch before scanning.

### Permission checks reused
- `get_is_item_removable(model, id, check_worn)`: library, FS locked folders (AO, bridge, wearable favorites), protected COF items, RLVa locks.
- `get_is_category_removable`, `get_is_category_and_children_removable(model, id, check_worn)`: protected folder types, FIRE-29342 protected folders, RLVa, FS locks, worn base outfit, worn descendants.
- `get_is_item_worn(id)`.

## Behavior

### Rule fields
- Name.
- Search text, search type: contains / starts with / ends with / regex / exact. Match case checkbox (default off).
- Non-regex search: `;`-separated terms, any term can match (regex: one pattern, use `|`). Each term chooses name or path mode on its own.
- Search subject: the object's name. Path mode: when the search text contains `/`, it is compared with the object's full path from My Inventory (`/Objects/Box/demo item`, no root name), e.g. regex `/Objects/.*demo.*`, starts with `/Objects/`. Paths are built during the scan walk (no per-object parent walk).
- Except text + Except type (same five types as Search, same path mode when a term contains `/`, its own Match case checkbox). Non-regex: `;`-separated terms; regex: one pattern (use `|`). A matching item is ignored; a matching folder is ignored with its whole subtree. Example (exact): `/Clothing/Demos;/Objects/Keep`.
- Destination paths: segments match folder names case-insensitively from My Inventory root.
- Match: item, folder, or containing folder (search runs on items; the direct parent folder of a matching, non-excepted item is selected, once; root and protected system folders are never selected; a selected folder is taken whole and its subfolders are not scanned).
- Item filters (Match: item or containing folder; disabled for folder matches). All must pass, after the name search:
  - Type: any / animation / body part / calling card / clothing / gesture / landmark / material / notecard / object / script / settings / snapshot / sound / texture. Asset type; snapshot = texture with inventory type `IT_SNAPSHOT`, texture excludes snapshots.
  - Age: any / newer than / older than N days + M hours, against `getCreationDate()` and `time_corrected()` (as the inventory filter). Unknown creation date (0) never matches an age condition.
  - Created by: anyone / me / others / specific person (user name or UUID). Names compare with `LLAvatarNameCache` legacy and account names, normalized (lower case, `.` = space, trailing ` resident` dropped), so `john.doe`, `John Doe`, `jane`, `Jane Resident` work; display names are not used. Uncached names are requested (`LLAvatarNameCache::get` queues them) and their items do not match yet. One lookup per creator per scan.
  - Permissions: copy / mod / transfer, each any / yes / no, on the owner mask.
- Action: none / move / delete.
  - Move: destination path, "Create destination folder if not exist".
  - Delete: Move to Trash (default) or Delete permanently.

### Scan
- Depth-first from My Inventory root. Links ignored.
- Never entered: Trash, Current Outfit, My Outfits (outfit folders are FT_OUTFIT, not protected), Marketplace Listings, FIRE-29342 protected folders (`gInventory.getProtectedCategories()`), the destination subtree, folders matching Except.
- Folder rules never match protected system folders. A matching folder is not descended into (its contents travel with it).

### Skip rules at apply time
- Worn items can be moved but never deleted (trash or purge). Folders containing worn items are not deleted.
- Locked/protected objects (checks above) are skipped.
- Moving a folder into itself or a descendant is skipped. Objects already in the destination are skipped.

### Execution (reliability)
- One run at a time (`ASInventoryRuleRunner` singleton, 0.1 s `LLEventTimer` tick, never blocks the main loop).
- Phases: load inventory (full fetch) → per rule: resolve/create destination (one folder at a time, waits for each creation callback) → scan → apply.
- Creator names: when the scan reports names still loading, the runner re-scans every 1 s (state `STATE_NAMES`) for up to 30 s, then applies without them (their items are not matched, logged).
- Apply in batches of 10 operations. Next batch is sent only when every operation of the current batch is confirmed (move: circuit ack; delete: AIS callback), then a 0.5 s pause.
- A batch not confirmed within 60 s aborts the run. A failed move triggers a refetch of the source and destination folders so the local view resyncs.
- Stop: no new batch is sent; the in-flight batch is still accounted, then the run ends.
- Permanent delete asks for confirmation.

### Execute warning
- Every Execute button (row Execute, Execute All, editor Execute Now) shows notification `ASInventoryRuleExecuteConfirm`: Cancel (default) / Dry Run / Execute Now, with the standard "don't show again" checkbox. One notification name, so dismissing it once turns it off for every Execute button. Re-enabled from Preferences popup list ("Executing inventory rules").
- The code checks `LLNotifications::getIgnored` itself and skips the dialog when ignored (the ignore default response, Cancel, is never used).
- When shown, it lists the permanent-delete rules ([PURGE]) and replaces `ASInventoryRulePurgeConfirm`. When ignored, the purge confirmation still appears.
- The checkbox applies whatever button is clicked (standard behavior).

### Dry run
- `ASInventoryRuleDryRun::run`: rules in order, rules without action left out. Destination resolved like the runner; missing folders are created virtually when "Create destination" is on. Scan, then per match the skip reason, as the runner re-checks each object right before acting.
- `ASInventoryRuleSimulation`: overlay on `gInventory` used by the scanner (`getChildren`, parents, paths, folder lookup). Records moves (`mParents`, `mAdded`), deletions (null parent; Trash moves land in the never-scanned Trash) and virtual folders (`LLViewerInventoryCategory` objects not in the model). Later rules of a dry run see the earlier rules' result. Empty (`ASInventoryRuleSimulation::real()`), it is the real inventory: preview and runner use it.
- Limits: protection, lock and worn checks use the real inventory (a folder whose content changed in the simulation is checked on its real content; virtual folders are not checked). The real run re-checks everything.
- Window `as_inventory_rule_dry_run` (key: `rules`, array of rule LLSD): summary, list with a bold header row per rule (action, counts, note) then its objects (name, folder before the action, action or `Skip: reason`), first 5000 objects. Double-click shows the object in My Inventory. Recomputed when the inventory finishes loading, after a run, and every 1 s for 30 s while creator names load. Execute Now runs the same rules without the warning (the dry run was just seen), still with the purge confirmation. Stop, progress bar, status.

### Import / export (JSON)
- Envelope: `{"format": "ayanestorm-inventory-rules", "version": 1, "type": "rule" | "ruleset" | "rulesets", "data": ...}`. data: a rule (same keys as storage, without `id`), a ruleset `{name, rules: [rules]}`, or an array of rulesets. Written indented, keys in editor order (`writeJson`, `keyRank`).
- Versions: `ASInventoryRules::FORMAT_VERSION`, shared with storage. Imports accept this version and older ones (`migrate()` upgrades one version at a time), refuse newer ones. Bump the version whenever a rule field is added or changes meaning, even a filter: an older viewer ignoring an unknown filter would match more objects.
- Import validation (`importJson`): valid JSON, format, version, type, then every field: no unknown key (`id` tolerated, ignored), text / true-false / whole number in range (`age_days` 0-10000, `age_hours` 0-23), enum values from the editor's list, regex compiles, a move has a destination. Missing fields take their default. The first error stops the import and names the place: `Rule 2 ('Hats') of ruleset 3 ('Demos'), field 'search_type': must be one of contains, starts_with, ...`. UTF-8 BOM accepted, 4 MB maximum.
- Imported rulesets and rules get new ids, keep their actions, are appended (nothing replaced) and nothing executes.
- Buttons: flyout `Export ▾` / `Import ▾`. The button uses the clipboard; the arrow offers clipboard or file (`.json` proposed, any file accepted). Success goes to the status line, errors to notification `ASInventoryRuleTransferError`.
  - Rules window: Export = every ruleset; Import = one ruleset or a full list, appended to the list. A single rule is refused (import it in a ruleset).
  - Ruleset window: Export = this ruleset; Import = one rule or a ruleset's rules, appended to this ruleset (the imported ruleset's name is not used). A full list is refused.
  - Rule editor: Export = the current fields, saved or not.

### UI
Four windows; the two list windows share `ASFloaterInventoryRuleList` (rows with Up / Down / Execute / Edit / Delete, New, Execute All, Dry Run, Stop, progress bar, status; list order is Execute All order; lists rebuild on the next draw so a row's own button is never destroyed under it).
- `as_inventory_rules` (AyaneStorm menu > Inventory Rules): rulesets. Execute All runs every ruleset in order. New Ruleset creates one and opens it. Deleting a ruleset closes the ruleset window and rule editor showing it.
- `as_inventory_ruleset` (key: ruleset id): ruleset name (saved on Enter / focus lost) and its rules. Execute All runs this ruleset.
- `as_inventory_rule_edit` (key: map `ruleset` id, `rule` id or null for new): rule editor on the left, live preview on the right (debounced 0.4 s, first 500 matches shown with path and skip reason). Preview row: double-click shows it in My Inventory (`LLInventoryPanel::openInventoryPanelAndSetSelection`); right-click opens `menu_as_inventory_rule_preview.xml` (Show in Inventory) (real inventory menus only exist inside an inventory folder view). Filters rows: Type; Age (combo + days + hours spinners, enabled when not "any"); Created by (combo + name/UUID field, enabled for "specific person"); Permissions (Copy / Mod / Transfer combos). Preview reruns every 1 s for 30 s after the last change while creator names load. Save, Execute now, Dry Run, Stop, progress bar, status. Execute now and Dry Run use the editor's current values, saved or not. Opening the editor clears the last run's status (unless a run is active).
- Rules whose action is none are skipped by every Execute.

### Storage
- Per-account LLSD setting `ASInventoryRulesets`: `{version, rulesets: [{id, name, rules: [rule maps]}]}` (version 1 also stored the bare array, still read). Older versions are migrated on load. Saved by a newer viewer: nothing is shown, every save is refused so the data stays intact, and the rules window says why (`getStorageError`). Filter keys: `item_type`, `age`, `age_days`, `age_hours`, `creator`, `creator_name`, `perm_copy`, `perm_modify`, `perm_transfer` (missing keys = no filter, so older rules load unchanged). Rulesets and rules carry stable UUIDs (`id`) so open editors keep pointing at the right object across reorders and deletions. A rule deleted while its editor is open is re-added on Save.

## Files
- `asinventoryrules.{h,cpp}`: rule and ruleset model, storage, simulation, matcher, item filter, scanner, dry run, runner.
- `asfloaterinventoryrules.{h,cpp}`: list window base, rulesets window, ruleset window, row panel, rule editor, dry run window.
- `floater_as_inventory_rules.xml`, `floater_as_inventory_ruleset.xml`, `panel_as_inventory_rule_item.xml`, `floater_as_inventory_rule_edit.xml`, `floater_as_inventory_rule_dry_run.xml`, `menu_as_inventory_rule_preview.xml`.
- Tagged hooks: `CMakeLists.txt`, `llviewerfloaterreg.cpp`, `menu_viewer.xml`, `notifications.xml`, `settings_per_account.xml`.
