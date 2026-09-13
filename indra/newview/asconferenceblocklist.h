/**
 * @file asconferenceblocklist.h
 * @brief Persistent per-account conference block list and its editor floater.
 *
 * Copyright (c) 2026 chanayane@firestorm
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
    static void unblock(const LLUUID& session_id);
    static LLSD getEntries();

    // Hooks for the two server paths used by incoming conferences.
    static bool declineInvitationIfBlocked(const LLUUID& session_id);
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
    void refresh();
    void onUnblock();

    LLScrollListCtrl* mConferenceList;
};

#endif // AS_CONFERENCEBLOCKLIST_H
