/**
 * @file asoutfitreorder.h
 * @author chanayane@ayanestorm
 * @brief Multi-step clothing layer reordering for the Edit Outfit parts list.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (C) 2026, Ayane Lyla
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * $/LicenseInfo$
 */

#ifndef AS_OUTFIT_REORDER_H
#define AS_OUTFIT_REORDER_H

#include "indra_constants.h"
#include "llwearabletype.h"

#include <string>

class LLPanel;
class LLPanelClothingListItem;
class LLUUID;
class LLView;
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

    // Drag and drop reordering of clothing rows, forwarded from LLPanelClothingListItem.
    // Only rows of a type with several layers can be dragged, only onto rows of the
    // same type. The dragged row stays in place as a ghost, a line shows the drop
    // gap, and a drop outside the visible list cancels.
    // Mouse handlers return true when the event belongs to a row drag.
    bool rowMouseDown(LLPanelClothingListItem* row, S32 x, S32 y, MASK mask);
    bool rowHover(LLPanelClothingListItem* row, S32 x, S32 y, MASK mask);
    bool rowMouseUp(LLPanelClothingListItem* row, S32 x, S32 y, MASK mask);
    void rowCaptureLost(LLPanelClothingListItem* row);
    // True while row is the dragged one (drawn faded at its start position).
    bool isDragGhost(const LLView* row);
    // Draws the drop line when row borders the current drop gap.
    void drawDropLine(LLView* row);
}

#endif // AS_OUTFIT_REORDER_H
