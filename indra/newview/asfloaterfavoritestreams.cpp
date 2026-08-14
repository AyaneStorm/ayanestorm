/**
 * @file asfloaterfavoritestreams.cpp
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

#include "llviewerprecompiledheaders.h"

#include "asfloaterfavoritestreams.h"

#include "asstreamkeeper.h"
#include "llbutton.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llviewercontrol.h"

ASFloaterFavoriteStreams::ASFloaterFavoriteStreams(const LLSD& key)
    : LLFloater(key),
      mStreamList(NULL),
      mPlayBtn(NULL),
      mPlayParcelBtn(NULL),
      mAddBtn(NULL),
      mAddManualBtn(NULL),
      mEditBtn(NULL),
      mRemoveBtn(NULL),
      mSetDefaultBtn(NULL),
      mClearDefaultBtn(NULL),
      mEditPanel(NULL),
      mNameEditor(NULL),
      mUrlEditor(NULL),
      mEditIndex(-1),
      mInitialized(false)
{
}

ASFloaterFavoriteStreams::~ASFloaterFavoriteStreams()
{
    if (mFavoritesChangedConnection.connected())
    {
        mFavoritesChangedConnection.disconnect();
    }
    if (mAutoPlayChangedConnection.connected())
    {
        mAutoPlayChangedConnection.disconnect();
    }
}

//virtual
bool ASFloaterFavoriteStreams::postBuild()
{
    mStreamList = getChild<LLScrollListCtrl>("stream_list");
    mStreamList->setDoubleClickCallback(boost::bind(&ASFloaterFavoriteStreams::onDoubleClick, this));

    mPlayBtn = getChild<LLButton>("play_btn");
    mPlayBtn->setCommitCallback(boost::bind(&ASFloaterFavoriteStreams::onPlay, this));

    mPlayParcelBtn = getChild<LLButton>("play_parcel_btn");
    mPlayParcelBtn->setCommitCallback(boost::bind(&ASFloaterFavoriteStreams::onPlayParcel, this));

    mAddBtn = getChild<LLButton>("add_current_btn");
    mAddBtn->setCommitCallback(boost::bind(&ASFloaterFavoriteStreams::onAddCurrent, this));

    mAddManualBtn = getChild<LLButton>("add_manual_btn");
    mAddManualBtn->setCommitCallback(boost::bind(&ASFloaterFavoriteStreams::onAddManual, this));

    mEditBtn = getChild<LLButton>("edit_btn");
    mEditBtn->setCommitCallback(boost::bind(&ASFloaterFavoriteStreams::onEdit, this));

    mRemoveBtn = getChild<LLButton>("remove_btn");
    mRemoveBtn->setCommitCallback(boost::bind(&ASFloaterFavoriteStreams::onRemove, this));

    mSetDefaultBtn = getChild<LLButton>("set_default_btn");
    mSetDefaultBtn->setCommitCallback(boost::bind(&ASFloaterFavoriteStreams::onSetDefault, this));

    mClearDefaultBtn = getChild<LLButton>("clear_default_btn");
    mClearDefaultBtn->setCommitCallback(boost::bind(&ASFloaterFavoriteStreams::onClearDefault, this));

    mListPanel = getChild<LLPanel>("list_panel");
    mButtonsPanel = getChild<LLPanel>("buttons_panel");
    mEditPanel = getChild<LLPanel>("edit_panel");
    mNameEditor = getChild<LLLineEditor>("name_editor");
    mUrlEditor = getChild<LLLineEditor>("url_editor");

    getChild<LLButton>("save_btn")->setCommitCallback(boost::bind(&ASFloaterFavoriteStreams::onSaveEdit, this));
    getChild<LLButton>("cancel_btn")->setCommitCallback(boost::bind(&ASFloaterFavoriteStreams::onCancelEdit, this));

    showEditPanel(false, -1);

    return true;
}

//virtual
void ASFloaterFavoriteStreams::onOpen(const LLSD& /*info*/)
{
    if (!mInitialized)
    {
        mFavoritesChangedConnection = ASStreamKeeper::getInstance()->setFavoritesChangedCallback(
            boost::bind(&ASFloaterFavoriteStreams::updateList, this));

        // The default row's label reflects whether auto-play is on, so it has to
        // be rebuilt when that preference is toggled elsewhere.
        mAutoPlayChangedConnection = gSavedSettings.getControl("ASAutoPlayDefaultFavorite")
            ->getSignal()->connect(boost::bind(&ASFloaterFavoriteStreams::updateList, this));

        mInitialized = true;
    }

    updateList();
}

//virtual
void ASFloaterFavoriteStreams::draw()
{
    LLFloater::draw();

    const bool has_selection = (mStreamList->getNumSelected() > 0);
    mPlayBtn->setEnabled(has_selection);
    mEditBtn->setEnabled(has_selection);
    mRemoveBtn->setEnabled(has_selection);
    mSetDefaultBtn->setEnabled(has_selection);

    mClearDefaultBtn->setEnabled(ASStreamKeeper::getInstance()->getDefaultFavoriteIndex() >= 0);

    // Only offer the parcel stream when this parcel actually has one.
    mPlayParcelBtn->setEnabled(!ASStreamKeeper::getAgentParcelMusicURL().empty());
}

S32 ASFloaterFavoriteStreams::getSelectedIndex() const
{
    LLScrollListItem* item = mStreamList->getFirstSelected();
    if (!item)
    {
        return -1;
    }
    return item->getValue().asInteger();
}

void ASFloaterFavoriteStreams::updateList()
{
    // Remember the selection so it survives the rebuild.
    S32 selected_index = getSelectedIndex();

    bool needs_sort = mStreamList->isSorted();
    mStreamList->setNeedsSort(false);
    mStreamList->clearRows();

    ASStreamKeeper* keeper = ASStreamKeeper::getInstance();
    const ASStreamKeeper::favorites_vec_t& favorites = keeper->getFavorites();
    const S32 default_index = keeper->getDefaultFavoriteIndex();

    // The default only actually plays when auto-play is switched on, so say so
    // in the label rather than leaving a bold row that may do nothing.
    const bool autoplay_on = gSavedSettings.getBOOL("ASAutoPlayDefaultFavorite");

    for (size_t i = 0; i < favorites.size(); ++i)
    {
        std::string display_name = favorites[i].mName;
        const bool is_default = ((S32)i == default_index);

        if (is_default)
        {
            LLStringUtil::format_map_t args;
            args["NAME"] = favorites[i].mName;
            display_name = getString(autoplay_on ? "DefaultSuffix" : "DefaultSuffixDisabled", args);
            LLStringUtil::trim(display_name);
        }

        LLSD row_data;
        row_data["value"] = (S32)i;
        row_data["columns"][0]["column"] = "name";
        row_data["columns"][0]["value"] = display_name;
        // The URL is not a column of its own: hover a row to see it, or open Edit.
        row_data["columns"][0]["tool_tip"] = favorites[i].mURL;

        LLScrollListItem* row = mStreamList->addElement(row_data);

        // Make the default favorite stand out.
        if (row && is_default)
        {
            LLScrollListText* name_column = (LLScrollListText*)row->getColumn(0);
            if (name_column)
            {
                name_column->setFontStyle(LLFontGL::BOLD);
            }
        }
    }

    mStreamList->setNeedsSort(needs_sort);
    mStreamList->updateSort();

    if (selected_index >= 0)
    {
        mStreamList->selectByValue(LLSD(selected_index));
    }
}

void ASFloaterFavoriteStreams::onDoubleClick()
{
    onPlay();
}

void ASFloaterFavoriteStreams::onPlay()
{
    S32 index = getSelectedIndex();
    if (index >= 0)
    {
        ASStreamKeeper::getInstance()->playFavorite((size_t)index);
    }
}

void ASFloaterFavoriteStreams::onPlayParcel()
{
    ASStreamKeeper::getInstance()->playParcelStream();
}

void ASFloaterFavoriteStreams::onAddCurrent()
{
    ASStreamKeeper* keeper = ASStreamKeeper::getInstance();

    const std::string url = keeper->getCurrentPlayingURL();
    if (url.empty() || keeper->hasFavoriteURL(url))
    {
        LLNotificationsUtil::add("ASFavoriteStreamNotAdded");
        return;
    }

    beginAddStream(keeper->getSuggestedNameForCurrentStream(), url);
}

void ASFloaterFavoriteStreams::onAddManual()
{
    // -1 means "adding", so the edit panel opens empty.
    showEditPanel(true, -1);
}

void ASFloaterFavoriteStreams::beginAddStream(const std::string& name, const std::string& url)
{
    showEditPanel(true, -1);

    mNameEditor->setValue(name);
    mUrlEditor->setValue(url);

    // Put the cursor in the name field: the URL is already correct, the name is
    // what the user is here to supply.
    mNameEditor->selectAll();
    mNameEditor->setFocus(true);
}

void ASFloaterFavoriteStreams::onEdit()
{
    S32 index = getSelectedIndex();
    if (index >= 0)
    {
        showEditPanel(true, index);
    }
}

void ASFloaterFavoriteStreams::onRemove()
{
    S32 index = getSelectedIndex();
    if (index < 0)
    {
        return;
    }

    ASStreamKeeper* keeper = ASStreamKeeper::getInstance();
    const ASStreamKeeper::favorites_vec_t& favorites = keeper->getFavorites();
    if ((size_t)index >= favorites.size())
    {
        return;
    }

    LLSD args;
    args["NAME"] = favorites[index].mName;
    // Bind the URL rather than the index: the list may change before the user
    // answers, and a stale index would remove the wrong entry.
    LLNotificationsUtil::add("ASConfirmRemoveFavoriteStream", args, LLSD(),
        boost::bind(&ASFloaterFavoriteStreams::onConfirmRemove, this, _1, _2, favorites[index].mURL));
}

bool ASFloaterFavoriteStreams::onConfirmRemove(const LLSD& notification, const LLSD& response, std::string url)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
    {
        ASStreamKeeper::getInstance()->removeFavoriteByURL(url);
        showEditPanel(false, -1);
    }
    return false;
}

void ASFloaterFavoriteStreams::onSetDefault()
{
    S32 index = getSelectedIndex();
    if (index < 0)
    {
        return;
    }

    ASStreamKeeper* keeper = ASStreamKeeper::getInstance();
    keeper->setDefaultFavorite((size_t)index);

    // Setting a default does nothing unless auto-play is switched on, so offer
    // to enable it rather than letting the choice silently have no effect.
    if (!gSavedSettings.getBOOL("ASAutoPlayDefaultFavorite"))
    {
        const ASStreamKeeper::favorites_vec_t& favorites = keeper->getFavorites();
        if ((size_t)index < favorites.size())
        {
            LLSD args;
            args["NAME"] = favorites[index].mName;
            LLNotificationsUtil::add("ASEnableAutoPlayDefaultFavorite", args, LLSD(),
                boost::bind(&ASFloaterFavoriteStreams::onConfirmEnableAutoPlay, this, _1, _2));
        }
    }
}

bool ASFloaterFavoriteStreams::onConfirmEnableAutoPlay(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
    {
        gSavedSettings.setBOOL("ASAutoPlayDefaultFavorite", true);
    }
    return false;
}

void ASFloaterFavoriteStreams::onClearDefault()
{
    ASStreamKeeper::getInstance()->clearDefaultFavorite();
}

void ASFloaterFavoriteStreams::showEditPanel(bool show, S32 edit_index)
{
    mEditIndex = edit_index;

    if (show)
    {
        ASStreamKeeper* keeper = ASStreamKeeper::getInstance();
        const ASStreamKeeper::favorites_vec_t& favorites = keeper->getFavorites();

        if (edit_index >= 0 && (size_t)edit_index < favorites.size())
        {
            mNameEditor->setValue(favorites[edit_index].mName);
            mUrlEditor->setValue(favorites[edit_index].mURL);
        }
        else
        {
            mNameEditor->setValue(LLStringUtil::null);
            mUrlEditor->setValue(LLStringUtil::null);
        }
    }

    // The two bottom panels occupy the same area, so they are mutually
    // exclusive: showing both at once overlaps the edit fields onto the
    // buttons. The list shrinks to clear the taller edit panel.
    mButtonsPanel->setVisible(!show);
    mEditPanel->setVisible(show);

    updateListBounds();
}

// Both bottom panels are anchored at the same place, so the list's lower edge
// only has to clear whichever one is currently visible.
void ASFloaterFavoriteStreams::updateListBounds()
{
    if (!mListPanel || !mButtonsPanel || !mEditPanel)
    {
        return;
    }

    const LLRect& bottom_rect = (mEditPanel->getVisible() ? mEditPanel->getRect()
                                                          : mButtonsPanel->getRect());

    LLRect list_rect = mListPanel->getRect();
    list_rect.mBottom = bottom_rect.mTop + 2;
    mListPanel->setShape(list_rect);
}

//virtual
void ASFloaterFavoriteStreams::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLFloater::reshape(width, height, called_from_parent);

    // The base class re-applies the XUI follows rules, which restore the list's
    // original bottom edge; reassert ours on top of that.
    updateListBounds();
}

void ASFloaterFavoriteStreams::onSaveEdit()
{
    const std::string name = mNameEditor->getValue().asString();
    const std::string url = mUrlEditor->getValue().asString();

    bool ok = false;
    if (mEditIndex < 0)
    {
        ok = ASStreamKeeper::getInstance()->addFavorite(name, url);
    }
    else
    {
        ok = ASStreamKeeper::getInstance()->editFavorite((size_t)mEditIndex, name, url);
    }

    if (!ok)
    {
        LLNotificationsUtil::add("ASFavoriteStreamNotAdded");
        return;
    }

    showEditPanel(false, -1);
}

void ASFloaterFavoriteStreams::onCancelEdit()
{
    showEditPanel(false, -1);
}
