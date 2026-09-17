/**
 * @file asnearbychatrangecounter.h
 * @author chanayane@firestorm
 * @brief Optional nearby-chat range population counter.
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
