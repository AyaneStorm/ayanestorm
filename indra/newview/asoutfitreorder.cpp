/**
 * @file asoutfitreorder.cpp
 * @author chanayane@firestorm
 * @brief Multi-step clothing layer reordering for the Edit Outfit parts list.
 */

#include "llviewerprecompiledheaders.h"

#include "asoutfitreorder.h"

#include "llagentwearables.h"
#include "llbutton.h"
#include "llflatlistview.h"
#include "llfocusmgr.h"
#include "llpanel.h"
#include "llrender2dutils.h"
#include "llui.h"
#include "llwindow.h"
#include "llappearancemgr.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"
#include "llvoavatarself.h"
#include "llwearableitemslist.h"

#include <algorithm>

// Defined in llappearancemgr.cpp (external linkage, no header declaration).
// Builds the COF link description that encodes a clothing layer order.
std::string build_order_string(LLWearableType::EType type, U32 i);

namespace
{
    // Row drag state. Views are held by handle: a COF change rebuilds the list
    // mid-drag, and a destroyed captor gets no onMouseCaptureLost().
    struct RowDrag
    {
        LLHandle<LLPanel> source;
        S32 down_x = 0;                 // Screen coordinates of the mouse down
        S32 down_y = 0;
        bool dragging = false;          // Past the drag threshold
        S32 new_pos = -1;               // Target layer index, -1 = no move
        LLHandle<LLPanel> line_row;     // Row that draws the drop line
        bool line_on_top = false;
    };
    RowDrag sDrag;

    bool isDraggable(LLPanelClothingListItem* row)
    {
        // Same condition as the move buttons being shown.
        LLViewerInventoryItem* item = row->getItem();
        if (!item || item->getType() != LLAssetType::AT_CLOTHING) return false;
        if (gAgentWearables.getWearableIdxFromItem(item) < 0) return false;
        return gAgentWearables.getWearableCount(item->getWearableType()) >= 2;
    }

    // True when the screen point is inside the list and every clipping ancestor.
    bool isInVisibleList(LLView* row, S32 screen_x, S32 screen_y)
    {
        LLFlatListView* list = row->getParentByType<LLFlatListView>();
        if (!list) return false;
        for (LLView* view = list; view; view = view->getParent())
        {
            if (!view->getVisible() || !view->calcScreenRect().pointInRect(screen_x, screen_y)) return false;
        }
        return true;
    }

    // Visible rows of the given type, top (outermost) to bottom.
    void collectTypeRows(LLView* items_panel, LLWearableType::EType type, std::vector<LLPanelClothingListItem*>& rows)
    {
        for (LLView* child : *items_panel->getChildList())
        {
            LLPanelClothingListItem* row = dynamic_cast<LLPanelClothingListItem*>(child);
            if (row && row->getVisible() && row->getWearableType() == type)
            {
                rows.push_back(row);
            }
        }
        std::sort(rows.begin(), rows.end(), [](const LLPanelClothingListItem* a, const LLPanelClothingListItem* b)
                  { return a->getRect().mTop > b->getRect().mTop; });
    }

    void updateTarget(LLPanelClothingListItem* source, S32 screen_x, S32 screen_y)
    {
        sDrag.new_pos = -1;
        sDrag.line_row = LLHandle<LLPanel>();

        LLView* items_panel = source->getParent();
        if (!items_panel || !isInVisibleList(source, screen_x, screen_y)) return;

        std::vector<LLPanelClothingListItem*> rows;
        collectTypeRows(items_panel, source->getWearableType(), rows);
        auto source_it = std::find(rows.begin(), rows.end(), source);
        if (source_it == rows.end()) return;
        const size_t src = source_it - rows.begin();
        const size_t count = rows.size();

        // Gap g in [0, count]: gap g is above rows[g], gap count is below the last row.
        // A cursor above or below the type's rows snaps to its first or last gap.
        S32 local_x, local_y;
        items_panel->screenPointToLocal(screen_x, screen_y, &local_x, &local_y);
        size_t gap = count;
        for (size_t k = 0; k < count; ++k)
        {
            if (local_y >= rows[k]->getRect().getCenterY())
            {
                gap = k;
                break;
            }
        }
        if (gap == src || gap == src + 1) return; // Back where it was

        // The dragged row takes the layer index of the row now in its new slot.
        LLPanelClothingListItem* slot_row = rows[gap < src ? gap : gap - 1];
        S32 pos = gAgentWearables.getWearableIdxFromItem(slot_row->getItem());
        if (pos < 0) return;

        sDrag.new_pos = pos;
        sDrag.line_on_top = gap < count;
        sDrag.line_row = (gap < count ? rows[gap] : rows[count - 1])->getHandle();
    }

    void resetDrag()
    {
        sDrag = RowDrag();
    }
}

namespace ASOutfitReorder
{
    bool moveWearableToIndex(LLViewerInventoryItem* item, U32 new_pos)
    {
        // Same preconditions as LLAppearanceMgr::moveWearable().
        if (!item || !item->isWearableType()) return false;
        if (item->getType() != LLAssetType::AT_CLOTHING) return false;
        LLAppearanceMgr* app_mgr = LLAppearanceMgr::getInstance();
        const LLUUID& cof_id = app_mgr->getCOF();
        if (!gInventory.isObjectDescendentOf(item->getUUID(), cof_id)) return false;

        const LLWearableType::EType type = item->getWearableType();
        S32 pos = gAgentWearables.getWearableIdxFromItem(item);
        if (pos < 0) return false; // Not worn
        U32 count = gAgentWearables.getWearableCount(type);
        if (count < 2) return false;
        new_pos = llmin(new_pos, count - 1);
        U32 old_pos = (U32)pos;
        if (old_pos == new_pos) return false;

        // Reorder gAgentWearables locally with adjacent swaps (no server traffic).
        const bool closer_to_body = new_pos < old_pos;
        U32 steps = closer_to_body ? old_pos - new_pos : new_pos - old_pos;
        for (U32 i = 0; i < steps; ++i)
        {
            if (!gAgentWearables.moveWearable(item, closer_to_body)) break;
        }

        // Rewrite the order description of every COF link in the affected range.
        // gAgentWearables is the source of truth, as in LLAppearanceMgr::moveWearable().
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t links;
        LLFindWearablesOfType filter_wearables_of_type(type);
        gInventory.collectDescendentsIf(cof_id, cats, links, true, filter_wearables_of_type);

        const U32 range_lo = llmin(old_pos, new_pos);
        const U32 range_hi = llmax(old_pos, new_pos);
        for (U32 i = range_lo; i <= range_hi; ++i)
        {
            const LLUUID item_id = gAgentWearables.getWearableItemID(type, i);
            for (LLViewerInventoryItem* link : links)
            {
                if (!link || link->getLinkedUUID() != item_id) continue;
                std::string new_desc = build_order_string(type, i);
                if (link->getActualDescription() != new_desc)
                {
                    link->setDescription(new_desc);
                    link->setComplete(true);
                    link->updateServer(false);
                    gInventory.updateItem(link);
                }
                break;
            }
        }

        if (isAgentAvatarValid())
        {
            gAgentAvatarp->wearableUpdated(type, false);
        }

        app_mgr->setOutfitDirty(true);
        gInventory.notifyObservers();
        return true;
    }

    bool moveWearableToTop(const LLUUID& item_id)
    {
        // Clamped to the outermost index.
        return moveWearableToIndex(gInventory.getItem(item_id), LLWearableData::MAX_CLOTHING_LAYERS);
    }

    bool moveWearableToBottom(const LLUUID& item_id)
    {
        return moveWearableToIndex(gInventory.getItem(item_id), 0);
    }

    void setButtonDimmed(LLPanel* row, const std::string& button_name, bool dimmed)
    {
        LLButton* button = row ? row->findChild<LLButton>(button_name) : nullptr;
        if (!button || !button->getImageOverlay()) return;
        // Enabled state cannot be used: list items treat it as "may be shown".
        // Alphas match LLButton's image_overlay_color / image_overlay_disabled_color defaults.
        button->setImageOverlay(button->getImageOverlay()->getName(), button->getImageOverlayHAlign(),
                                LLColor4::white % (dimmed ? 0.3f : 0.75f));
    }

    bool rowMouseDown(LLPanelClothingListItem* row, S32 x, S32 y, MASK mask)
    {
        // Modifier clicks stay multi-selection; buttons keep their click.
        // The handled flag of the base handler cannot tell: mouse opaque
        // children (icon, name) count as handled too.
        if (!row || mask != MASK_NONE || !isDraggable(row)) return false;
        if (dynamic_cast<LLButton*>(row->childFromPoint(x, y, true))) return false;

        resetDrag();
        sDrag.source = row->getHandle();
        row->localPointToScreen(x, y, &sDrag.down_x, &sDrag.down_y);
        gFocusMgr.setMouseCapture(row);
        return true;
    }

    bool rowHover(LLPanelClothingListItem* row, S32 x, S32 y, MASK mask)
    {
        if (!row || !row->hasMouseCapture() || sDrag.source.get() != row) return false;

        S32 screen_x, screen_y;
        row->localPointToScreen(x, y, &screen_x, &screen_y);
        if (!sDrag.dragging)
        {
            S32 dx = screen_x - sDrag.down_x;
            S32 dy = screen_y - sDrag.down_y;
            if (dx * dx + dy * dy <= DRAG_N_DROP_DISTANCE_THRESHOLD * DRAG_N_DROP_DISTANCE_THRESHOLD) return true;
            sDrag.dragging = true;
        }

        updateTarget(row, screen_x, screen_y);
        row->getWindow()->setCursor(isInVisibleList(row, screen_x, screen_y) ? UI_CURSOR_ARROWDRAG : UI_CURSOR_NO);
        return true;
    }

    bool rowMouseUp(LLPanelClothingListItem* row, S32 x, S32 y, MASK mask)
    {
        if (!row || !row->hasMouseCapture() || sDrag.source.get() != row) return false;

        S32 new_pos = -1;
        if (sDrag.dragging)
        {
            S32 screen_x, screen_y;
            row->localPointToScreen(x, y, &screen_x, &screen_y);
            updateTarget(row, screen_x, screen_y);
            new_pos = sDrag.new_pos;
        }
        resetDrag();
        gFocusMgr.setMouseCapture(nullptr);

        // Rebuilds the list; rows are deleted later (die()), so row stays valid here.
        if (new_pos >= 0)
        {
            moveWearableToIndex(row->getItem(), (U32)new_pos);
        }
        return true;
    }

    void rowCaptureLost(LLPanelClothingListItem* row)
    {
        if (sDrag.source.get() == row)
        {
            resetDrag();
        }
    }

    bool isDragGhost(const LLView* row)
    {
        return sDrag.dragging && row && sDrag.source.get() == row;
    }

    void drawDropLine(LLView* row)
    {
        if (!sDrag.dragging || sDrag.new_pos < 0 || !row || sDrag.line_row.get() != row) return;

        const LLRect rect = row->getLocalRect();
        const S32 top = sDrag.line_on_top ? rect.mTop : rect.mBottom + 2;
        gl_rect_2d(rect.mLeft + 2, top, rect.mRight - 2, top - 2, LLColor4::white);
    }
}
