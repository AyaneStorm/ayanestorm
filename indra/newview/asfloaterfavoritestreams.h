/**
 * @file asfloaterfavoritestreams.h
 * @brief Class for the AyaneStorm favorite audio streams floater
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (c) 2026 Chanayane @ Second Life
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
 * $/LicenseInfo$
 */

#ifndef AS_FLOATERFAVORITESTREAMS_H
#define AS_FLOATERFAVORITESTREAMS_H

#include "llfloater.h"

class LLButton;
class LLLineEditor;
class LLPanel;
class LLScrollListCtrl;

// View only: all state and logic lives in ASStreamKeeper.
class ASFloaterFavoriteStreams : public LLFloater
{
public:
    ASFloaterFavoriteStreams(const LLSD& key);
    virtual ~ASFloaterFavoriteStreams();

    bool postBuild() override;
    void onOpen(const LLSD& info) override;
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

    // Opens the edit panel prefilled for a new entry, so the user can name a
    // stream before it is saved.
    void beginAddStream(const std::string& name, const std::string& url);

private:
    void updateList();
    S32  getSelectedIndex() const;

    void onDoubleClick();
    void onPlay();
    void onPlayParcel();
    void onAddCurrent();
    void onAddManual();
    void onEdit();
    void onRemove();
    void onSetDefault();
    void onClearDefault();

    bool onConfirmRemove(const LLSD& notification, const LLSD& response, std::string url);
    bool onConfirmEnableAutoPlay(const LLSD& notification, const LLSD& response);
    void onSaveEdit();
    void onCancelEdit();

    void showEditPanel(bool show, S32 edit_index);
    // Keeps the list clear of whichever bottom panel is currently showing.
    void updateListBounds();

    boost::signals2::connection mFavoritesChangedConnection;
    boost::signals2::connection mAutoPlayChangedConnection;

    LLScrollListCtrl*   mStreamList;
    LLPanel*            mListPanel;
    LLButton*           mPlayBtn;
    LLButton*           mPlayParcelBtn;
    LLButton*           mAddBtn;
    LLButton*           mAddManualBtn;
    LLButton*           mEditBtn;
    LLButton*           mRemoveBtn;
    LLButton*           mSetDefaultBtn;
    LLButton*           mClearDefaultBtn;
    LLPanel*            mButtonsPanel;
    LLPanel*            mEditPanel;
    LLLineEditor*       mNameEditor;
    LLLineEditor*       mUrlEditor;

    S32                 mEditIndex;     // -1 while adding a new entry
    bool                mInitialized;
};

#endif // AS_FLOATERFAVORITESTREAMS_H
