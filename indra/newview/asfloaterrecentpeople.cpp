/**
 * @file asfloaterrecentpeople.cpp
 * @brief Class for the AyaneStorm recent people floater
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

#include "asfloaterrecentpeople.h"

#include "llavataractions.h"
#include "llavatarlist.h"
#include "llavatarlistitem.h"
#include "llpanelpeoplemenus.h"
#include "llrecentpeople.h"

ASFloaterRecentPeople::ASFloaterRecentPeople(const LLSD& key)
    : LLFloater(key),
      mRecentList(nullptr)
{
}

bool ASFloaterRecentPeople::postBuild()
{
    mRecentList = findChild<LLAvatarList>("avatar_list");
    if (!mRecentList)
    {
        return false;
    }

    mRecentList->setNoItemsCommentText(getString("no_recent_people"));
    mRecentList->setNoItemsMsg(getString("no_recent_people"));
    mRecentList->setContextMenu(&LLPanelPeopleMenus::gPeopleContextMenu);
    mRecentList->setItemDoubleClickCallback(boost::bind(&ASFloaterRecentPeople::onAvatarListDoubleClicked, this, _1));

    return true;
}

void ASFloaterRecentPeople::onOpen(const LLSD& key)
{
    updateList();
    LLFloater::onOpen(key);
}

void ASFloaterRecentPeople::updateList()
{
    if (!mRecentList)
    {
        return;
    }

    LLRecentPeople::instance().get(mRecentList->getIDs());
    mRecentList->setDirty();
}

void ASFloaterRecentPeople::onAvatarListDoubleClicked(LLUICtrl* ctrl)
{
    LLAvatarListItem* item = dynamic_cast<LLAvatarListItem*>(ctrl);
    if (!item)
    {
        return;
    }

    LLAvatarActions::startIM(item->getAvatarId());
}
