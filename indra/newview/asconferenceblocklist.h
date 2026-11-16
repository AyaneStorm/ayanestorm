/**
 * @file asconferenceblocklist.h
 * @brief Persistent per-account conference block list and its editor floater.
 *
 * Copyright (c) 2026 chanayane@firestorm
 * @author chanayane@ayanestorm
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

#ifndef AS_CONFERENCEBLOCKLIST_H
#define AS_CONFERENCEBLOCKLIST_H

#include "llfloater.h"
#include "lluuid.h"

#include <string>

class LLScrollListCtrl;

// Keeps conference-specific state out of the shared IM implementation.
class ASConferenceBlockList
{
public:
    static bool isBlocked(const LLUUID& session_id);
    static void block(const LLUUID& session_id, const std::string& name);
    static void confirmBlockAndLeave(const LLUUID& session_id, const std::string& name);
    static void unblock(const LLUUID& session_id);
    static LLSD getEntries();

    // Hooks for the two server paths used by incoming conferences.
    static bool declineInvitationIfBlocked(const LLUUID& session_id, const LLUUID& inviter_id);
    static bool leaveSessionIfBlocked(const LLUUID& session_id, const LLUUID& other_participant_id);
};

class ASFloaterConferenceBlockList : public LLFloater
{
public:
    ASFloaterConferenceBlockList(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void refresh() override;
    void onUnblock();

    LLScrollListCtrl* mConferenceList;
};

#endif // AS_CONFERENCEBLOCKLIST_H
