/**
 * @file asnearbychatrangecounter.cpp
 * @author chanayane@firestorm
 * @brief Optional nearby-chat range population counter.
 */

#include "llviewerprecompiledheaders.h"

#include "asnearbychatrangecounter.h"

#include "fscommon.h"
#include "fsradar.h"
#include "lfsimfeaturehandler.h"
#include "llagent.h"
#include "llcallingcard.h"
#include "llcombobox.h"
#include "llgl.h"
#include "lllayoutstack.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "rlvactions.h"

#include <algorithm>

namespace
{
constexpr F32 UPDATE_INTERVAL = 1.f;
constexpr F32 FLASH_DURATION = 0.6f;
}

static LLDefaultChildRegistry::Register<ASNearbyChatRangeCounter>
    sASNearbyChatRangeCounter("as_nearby_chat_range_counter");

ASNearbyChatRangeCounter::Params::Params() = default;

ASNearbyChatRangeCounter::ASNearbyChatRangeCounter(const Params& params)
    : LLTextBox(params),
      mChatTypeCombo(nullptr),
      mRangeColor(LLColor4::yellow),
      mAvatarCount(-1),
      mFriendCount(-1),
      mFlashActive(false)
{
    mUpdateTimer.setTimerExpirySec(0.f);
}

void ASNearbyChatRangeCounter::updateCounter()
{
    if (!mChatTypeCombo)
    {
        LLView* floater = this;
        while (floater && floater->getName() != "nearby_chat")
        {
            floater = floater->getParent();
        }
        if (floater)
        {
            mChatTypeCombo = floater->findChild<LLComboBox>("chat_type");
        }
    }

    if (!mChatTypeCombo || !RlvActions::canShowNearbyAgents())
    {
        setText(LLStringExplicit("--"));
        mAvatarCount = -1;
        return;
    }

    const std::string chat_type = mChatTypeCombo->getSelectedValue().asString();
    F32 range;
    std::string color_name;
    LLColor4 fallback_color;
    if (chat_type == "whisper")
    {
        range = (F32)LFSimFeatureHandler::getInstance()->whisperRange();
        color_name = "LtBlue";
        fallback_color = LLColor4(0.f, 0.5f, 1.f, 1.f);
    }
    else if (chat_type == "shout")
    {
        range = (F32)LFSimFeatureHandler::getInstance()->shoutRange();
        color_name = "MapShoutRingColor";
        fallback_color = LLColor4::red;
    }
    else
    {
        range = (F32)LFSimFeatureHandler::getInstance()->sayRange();
        color_name = "MapChatRingColor";
        fallback_color = LLColor4::yellow;
    }
    mRangeColor = LLUIColorTable::instance().getColor(color_name, fallback_color).get();
    // Minimap rings are translucent; preserve their exact RGB but make text legible.
    mRangeColor.mV[VALPHA] = 1.f;
    setColor(LLUIColor(mRangeColor));

    std::vector<LLSD> radar_entries;
    LLSD radar_stats;
    FSRadar::getInstance()->getCurrentData(radar_entries, radar_stats);
    S32 avatar_count = 0;
    S32 friend_count = 0;
    for (const LLSD& row : radar_entries)
    {
        const LLSD& entry = row["entry"];
        const LLUUID avatar_id = entry["id"].asUUID();
        const F32 avatar_range = (F32)entry["as_range_value"].asReal();
        if (avatar_id != gAgentID && avatar_range > AVATAR_UNKNOWN_RANGE && avatar_range <= range)
        {
            ++avatar_count;
            if (LLAvatarTracker::instance().isBuddy(avatar_id))
            {
                ++friend_count;
            }
        }
    }

    const bool show_friends = gSavedSettings.getBOOL("ASShowNearbyChatRangeFriendCount");
    if (mAvatarCount >= 0 &&
        (avatar_count != mAvatarCount || (show_friends && friend_count != mFriendCount)))
    {
        mFlashTimer.reset();
        mFlashActive = true;
    }
    mAvatarCount = avatar_count;
    mFriendCount = friend_count;

    const std::string counter_text = show_friends
        ? llformat("%d (%d)", mAvatarCount, mFriendCount)
        : llformat("%d", mAvatarCount);
    setText(LLStringExplicit(counter_text));
    setToolTip(LLStringExplicit(show_friends
        ? llformat("%d other avatars in %s range; %d are friends. Your avatar is excluded.",
                   mAvatarCount, chat_type.c_str(), mFriendCount)
        : llformat("%d other avatars in %s range. Your avatar is excluded.",
                   mAvatarCount, chat_type.c_str())));

    if (LLLayoutPanel* panel = dynamic_cast<LLLayoutPanel*>(getParent()))
    {
        panel->setTargetDim(llclamp(getTextPixelWidth() + 12, 38, 96));
    }
}

void ASNearbyChatRangeCounter::draw()
{
    // Keep the disabled feature entirely idle, even if external code makes the control visible.
    if (!gSavedSettings.getBOOL("ASShowNearbyChatRangeCounter"))
    {
        return;
    }

    if (mUpdateTimer.hasExpired())
    {
        updateCounter();
        mUpdateTimer.setTimerExpirySec(UPDATE_INTERVAL);
    }

    const F32 flash_time = mFlashTimer.getElapsedTimeF32();
    if (mFlashActive && flash_time < FLASH_DURATION)
    {
        const F32 strength = 1.f - flash_time / FLASH_DURATION;
        LLColor4 bloom_color = mRangeColor;
        bloom_color.mV[VALPHA] = 0.35f * strength;
        LLGLSUIDefault gls_ui;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4fv(bloom_color.mV);
        gl_circle_2d((F32)getLocalRect().getCenterX(), (F32)getLocalRect().getCenterY(),
                     7.f + 4.f * (1.f - strength), 24, true);
        setColor(LLUIColor(lerp(mRangeColor, LLColor4::white, 0.65f * strength)));
    }
    else if (mFlashActive)
    {
        mFlashActive = false;
        setColor(LLUIColor(mRangeColor));
    }

    LLTextBox::draw();
}
