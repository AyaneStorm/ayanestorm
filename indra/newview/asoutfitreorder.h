/**
 * @file asoutfitreorder.h
 * @author chanayane@firestorm
 * @brief Multi-step clothing layer reordering for the Edit Outfit parts list.
 */

#ifndef AS_OUTFIT_REORDER_H
#define AS_OUTFIT_REORDER_H

#include "llwearabletype.h"

#include <string>

class LLPanel;
class LLUUID;
class LLViewerInventoryItem;

namespace ASOutfitReorder
{
    // Moves a worn clothing item (COF link or linked item) to layer index
    // new_pos within its wearable type. Index 0 is closest to the body.
    // Rewrites the COF link order descriptions once per changed link and
    // updates the avatar once, instead of one round trip per single step.
    bool moveWearableToIndex(LLViewerInventoryItem* item, U32 new_pos);

    // List top is the outermost layer (furthest from the body).
    bool moveWearableToTop(const LLUUID& item_id);
    // List bottom is the innermost layer (closest to the body).
    bool moveWearableToBottom(const LLUUID& item_id);

    // Dims a row move button that would do nothing, without hiding it, so the
    // other buttons keep their horizontal place. The click stays a no-op.
    void setButtonDimmed(LLPanel* row, const std::string& button_name, bool dimmed);
}

#endif // AS_OUTFIT_REORDER_H
