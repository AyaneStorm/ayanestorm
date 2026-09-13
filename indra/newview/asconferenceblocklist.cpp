/**
 * @file asconferenceblocklist.cpp
 * @brief Persistent per-account conference block list and its editor floater.
 *
 * Copyright (c) 2026 chanayane@firestorm
 */

#include "llviewerprecompiledheaders.h"

#include "asconferenceblocklist.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcorehttputil.h"
#include "llfloaterreg.h"
#include "llimview.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"

namespace
{
constexpr const char* SETTING_BLOCKED_CONFERENCES = "ASBlockedConferences";

bool onConfirmBlockConference(const LLSD& notification, const LLSD& response,
                              const LLUUID& session_id, const std::string& name)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
    {
        ASConferenceBlockList::block(session_id, name);
        LLFloaterReg::showInstance("as_conference_block_list");
        if (gIMMgr)
        {
            gIMMgr->leaveSession(session_id);
        }
    }
    return false;
}
}

LLSD ASConferenceBlockList::getEntries()
{
    LLSD entries = gSavedPerAccountSettings.getLLSD(SETTING_BLOCKED_CONFERENCES);
    return entries.isArray() ? entries : LLSD::emptyArray();
}

bool ASConferenceBlockList::isBlocked(const LLUUID& session_id)
{
    if (session_id.isNull())
    {
        return false;
    }

    const LLSD entries = getEntries();
    for (LLSD::array_const_iterator it = entries.beginArray(); it != entries.endArray(); ++it)
    {
        if ((*it)["id"].asUUID() == session_id)
        {
            return true;
        }
    }
    return false;
}

void ASConferenceBlockList::block(const LLUUID& session_id, const std::string& name)
{
    if (session_id.isNull() || isBlocked(session_id))
    {
        return;
    }

    LLSD entries = getEntries();
    LLSD entry;
    entry["id"] = session_id;
    entry["name"] = name;
    entries.append(entry);
    gSavedPerAccountSettings.setLLSD(SETTING_BLOCKED_CONFERENCES, entries);
}

void ASConferenceBlockList::confirmBlockAndLeave(const LLUUID& session_id, const std::string& name)
{
    LLSD args;
    args["CONFERENCE"] = name;
    LLNotificationsUtil::add(
        "ASConfirmBlockConference", args, LLSD(),
        boost::bind(&onConfirmBlockConference, _1, _2, session_id, name));
}

void ASConferenceBlockList::unblock(const LLUUID& session_id)
{
    LLSD filtered = LLSD::emptyArray();
    const LLSD entries = getEntries();
    for (LLSD::array_const_iterator it = entries.beginArray(); it != entries.endArray(); ++it)
    {
        if ((*it)["id"].asUUID() != session_id)
        {
            filtered.append(*it);
        }
    }
    gSavedPerAccountSettings.setLLSD(SETTING_BLOCKED_CONFERENCES, filtered);
}

bool ASConferenceBlockList::declineInvitationIfBlocked(const LLUUID& session_id)
{
    if (!isBlocked(session_id))
    {
        return false;
    }

    LL_INFOS("ConferenceBlockList") << "Declining blocked conference " << session_id << LL_ENDL;
    LLViewerRegion* region = gAgent.getRegion();
    if (region)
    {
        LLSD data;
        data["method"] = "decline invitation";
        data["session-id"] = session_id;
        LLCoreHttpUtil::HttpCoroutineAdapter::messageHttpPost(
            region->getCapability("ChatSessionRequest"), data,
            "Blocked conference declined", "Blocked conference decline failed");
    }

    if (gIMMgr)
    {
        gIMMgr->clearPendingAgentListUpdates(session_id);
        gIMMgr->clearPendingInvitation(session_id);
    }
    return true;
}

bool ASConferenceBlockList::leaveSessionIfBlocked(const LLUUID& session_id, const LLUUID& other_participant_id)
{
    if (!isBlocked(session_id))
    {
        return false;
    }

    LL_INFOS("ConferenceBlockList") << "Leaving blocked conference " << session_id << LL_ENDL;
    if (gIMMgr)
    {
        gIMMgr->clearPendingInvitation(session_id);
        gIMMgr->clearPendingAgentListUpdates(session_id);
    }
    LLIMModel::getInstance()->sendLeaveSession(session_id, other_participant_id);
    return true;
}

ASFloaterConferenceBlockList::ASFloaterConferenceBlockList(const LLSD& key)
    : LLFloater(key),
      mConferenceList(nullptr)
{
}

bool ASFloaterConferenceBlockList::postBuild()
{
    mConferenceList = getChild<LLScrollListCtrl>("conference_list");
    getChild<LLButton>("unblock_btn")->setCommitCallback(
        boost::bind(&ASFloaterConferenceBlockList::onUnblock, this));
    return true;
}

void ASFloaterConferenceBlockList::onOpen(const LLSD& /*key*/)
{
    refresh();
}

void ASFloaterConferenceBlockList::draw()
{
    getChild<LLButton>("unblock_btn")->setEnabled(mConferenceList->getNumSelected() > 0);
    LLFloater::draw();
}

void ASFloaterConferenceBlockList::refresh()
{
    mConferenceList->clearRows();
    const LLSD entries = ASConferenceBlockList::getEntries();
    for (LLSD::array_const_iterator it = entries.beginArray(); it != entries.endArray(); ++it)
    {
        LLSD row;
        row["value"] = (*it)["id"];
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = (*it)["name"].asString();
        row["columns"][1]["column"] = "session_id";
        row["columns"][1]["value"] = (*it)["id"].asString();
        mConferenceList->addElement(row, ADD_BOTTOM);
    }
}

void ASFloaterConferenceBlockList::onUnblock()
{
    LLScrollListItem* item = mConferenceList->getFirstSelected();
    if (item)
    {
        ASConferenceBlockList::unblock(item->getValue().asUUID());
        refresh();
    }
}
