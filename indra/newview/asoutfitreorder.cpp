/**
 * @file asoutfitreorder.cpp
 * @author chanayane@firestorm
 * @brief Multi-step clothing layer reordering for the Edit Outfit parts list.
 */

#include "llviewerprecompiledheaders.h"

#include "asoutfitreorder.h"

#include "llagentwearables.h"
#include "llbutton.h"
#include "llpanel.h"
#include "llappearancemgr.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"
#include "llvoavatarself.h"

// Defined in llappearancemgr.cpp (external linkage, no header declaration).
// Builds the COF link description that encodes a clothing layer order.
std::string build_order_string(LLWearableType::EType type, U32 i);

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
}
