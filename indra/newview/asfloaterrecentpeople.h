/**
 * @file asfloaterrecentpeople.h
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

#ifndef AS_FLOATERRECENTPEOPLE_H
#define AS_FLOATERRECENTPEOPLE_H

#include "llfloater.h"

class LLAvatarList;

// Lists people the agent has recently interacted with (LLRecentPeople), so it
// can be docked as an optional tab in the Conversations window.
class ASFloaterRecentPeople : public LLFloater
{
public:
    ASFloaterRecentPeople(const LLSD& key);
    virtual ~ASFloaterRecentPeople() = default;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void updateList();
    void onAvatarListDoubleClicked(LLUICtrl* ctrl);

    LLAvatarList* mRecentList;
};

#endif // AS_FLOATERRECENTPEOPLE_H
