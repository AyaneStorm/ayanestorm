/**
 * @file asnearbychatrangecounter.h
 * @author chanayane@ayanestorm
 * @brief Optional nearby-chat range population counter.
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

#ifndef AS_NEARBY_CHAT_RANGE_COUNTER_H
#define AS_NEARBY_CHAT_RANGE_COUNTER_H

#include "llframetimer.h"
#include "lltextbox.h"

class LLComboBox;

// Displays the number of other avatars inside the selected local-chat range.
class ASNearbyChatRangeCounter : public LLTextBox
{
public:
    struct Params : public LLInitParam::Block<Params, LLTextBox::Params>
    {
        Params();
    };

    void draw() override;

protected:
    ASNearbyChatRangeCounter(const Params& params);
    friend class LLUICtrlFactory;

private:
    void updateCounter();

    LLComboBox*  mChatTypeCombo;
    LLFrameTimer mUpdateTimer;
    LLFrameTimer mFlashTimer;
    LLColor4     mRangeColor;
    S32          mAvatarCount;
    S32          mFriendCount;
    bool         mFlashActive;
};

#endif // AS_NEARBY_CHAT_RANGE_COUNTER_H
