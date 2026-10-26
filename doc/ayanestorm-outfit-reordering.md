# AyaneStorm Edit Outfit Clothing Reordering

Edit Outfit panel ("edit outfit parts", `panel_outfit_edit.xml`), COF (Current Outfit Folder) clothing list.

## Code map

- Rows: `LLPanelClothingListItem` (`llwearableitemslist.cpp/.h`), params from `widgets/clothing_list_item.xml` (default + `starlightcui` skins). `panel_clothing_list_item.xml` files are unused.
- List: `LLCOFWearables::buildClothingListItem()` / `populateClothingList()` (`llcofwearables.cpp`). Rows shown outermost first (`first` = top = highest layer index).
- Up/down: `LLPanelOutfitEdit::moveWearable()` -> `LLAppearanceMgr::moveWearable()` (one adjacent swap, two link `updateServer` calls) -> `LLAgentWearables::moveWearable()` (local swap by asset id).
- Order storage: COF link description = `build_order_string(type, index)` (`llappearancemgr.cpp`, external linkage, no header). `gAgentWearables` is the source of truth.
- Refresh: `gInventory.notifyObservers()` -> `LLOutfitObserver` COF changed -> `LLPanelOutfitEdit::onCOFChanged()` -> `LLCOFWearables::refresh()` rebuilds all rows. `LLFlatListView::clear()` uses `die()`, so a row may trigger its own rebuild from a click.

## Move to top / bottom (implemented)

- `asoutfitreorder.cpp/.h`: `ASOutfitReorder::moveWearableToIndex(item, new_pos)` does local adjacent swaps, rewrites only the changed links' descriptions, one `wearableUpdated`, one `notifyObservers`. `moveWearableToTop/Bottom(link_id)`.
- Buttons `btn_move_top` / `btn_move_bottom` (params `top_btn` / `bottom_btn`); right-side order top, bottom, up, down (up / down keep their original place next to lock / edit).
- Fixed button positions: `reshapeRightWidgets()` packs only visible widgets, and list items use the enabled state as "may be shown", so edge buttons are not hidden or disabled. All four show when the type has 2+ layers; edge ones are dimmed via overlay alpha (`ASOutfitReorder::setButtonDimmed()`) and their click is a no-op. Icons `AS_MoveTop_Off` / `AS_MoveBottom_Off` (`textures/as/move_top_off.png`, `move_bottom_off.png`, 18x18 LA, derived from `UpArrow_Off`).
- Callbacks bound in `LLCOFWearables::buildClothingListItem()` with the row's COF link id (not the selection).

## Drag and drop feasibility

Feasible, and `moveWearableToIndex()` already provides the drop action.

### Why the global drag tool path is the hard path

- `LLToolDragAndDrop` (`beginDrag`, `DAD_CLOTHING`) routes drops through `handleDragAndDrop`; `LLPanelOutfitEdit::handleDragAndDrop()` accepts `DAD_CLOTHING`/`DAD_LINK` and wears the item (it does not reorder).
- `LLFlatListView` rows have no `handleDragAndDrop`; hit testing goes through accordion + scroll container.
- Cargo is an inventory item, so the target layer index must be recomputed from row geometry anyway.

### Recommended path: in-row mouse capture

- `LLPanelClothingListItem` overrides (tagged) `handleMouseDown` / `handleHover` / `handleMouseUp` / `onMouseCaptureLost` / `draw`, forwarding to an AS helper.
- Mouse down not handled by a child button: call base (keeps selection), record start point, `gFocusMgr.setMouseCapture(this)`.
- Hover past a few pixels: dragging; find the row under the cursor in the parent `LLFlatListView`; only same `getWearableType()` rows are valid targets; draw an indicator on the target row.
- Mouse up: release capture; `new_pos = gAgentWearables.getWearableIdxFromItem(target)`; `moveWearableToIndex()`.
- Capture lost (list rebuilt by an external COF change): cancel.
- Optional: autoscroll near the scroll container edges.
