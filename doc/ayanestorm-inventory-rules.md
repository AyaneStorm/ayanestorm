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
- Apply in batches of 10 operations. Next batch is sent only when every operation of the current batch is confirmed (move: circuit ack; delete: AIS callback), then a 0.5 s pause.
- A batch not confirmed within 60 s aborts the run. A failed move triggers a refetch of the source and destination folders so the local view resyncs.
- Stop: no new batch is sent; the in-flight batch is still accounted, then the run ends.
- Permanent delete asks for confirmation.

### UI
Three windows; the two list windows share `ASFloaterInventoryRuleList` (rows with Up / Down / Execute / Edit / Delete, New, Execute All, Stop, progress bar, status; list order is Execute All order; lists rebuild on the next draw so a row's own button is never destroyed under it).
- `as_inventory_rules` (AyaneStorm menu > Inventory Rules): rulesets. Execute All runs every ruleset in order. New Ruleset creates one and opens it. Deleting a ruleset closes the ruleset window and rule editor showing it.
- `as_inventory_ruleset` (key: ruleset id): ruleset name (saved on Enter / focus lost) and its rules. Execute All runs this ruleset.
- `as_inventory_rule_edit` (key: map `ruleset` id, `rule` id or null for new): rule editor on the left, live preview on the right (debounced 0.4 s, first 500 matches shown with path and skip reason). Preview row: double-click shows it in My Inventory (`LLInventoryPanel::openInventoryPanelAndSetSelection`); right-click opens `menu_as_inventory_rule_preview.xml` (Show in Inventory) (real inventory menus only exist inside an inventory folder view). Save, Execute now, Stop, progress bar, status. Execute now runs the editor's current values, saved or not. Opening the editor clears the last run's status (unless a run is active).
- Rules whose action is none are skipped by every Execute.

### Storage
- Per-account LLSD setting `ASInventoryRulesets`: array of `{id, name, rules: [rule maps]}`. Rulesets and rules carry stable UUIDs (`id`) so open editors keep pointing at the right object across reorders and deletions. A rule deleted while its editor is open is re-added on Save.

## Files
- `asinventoryrules.{h,cpp}`: rule and ruleset model, storage, matcher, scanner, runner.
- `asfloaterinventoryrules.{h,cpp}`: list window base, rulesets window, ruleset window, row panel, rule editor.
- `floater_as_inventory_rules.xml`, `floater_as_inventory_ruleset.xml`, `panel_as_inventory_rule_item.xml`, `floater_as_inventory_rule_edit.xml`, `menu_as_inventory_rule_preview.xml`.
- Tagged hooks: `CMakeLists.txt`, `llviewerfloaterreg.cpp`, `menu_viewer.xml`, `notifications.xml`, `settings_per_account.xml`.
