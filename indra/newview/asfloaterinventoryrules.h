/**
 * @file asfloaterinventoryrules.h
 * @brief Inventory rules list floater and rule editor floater.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (c) 2026 chanayane@firestorm
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

#ifndef AS_FLOATERINVENTORYRULES_H
#define AS_FLOATERINVENTORYRULES_H

#include "asinventoryrules.h"
#include "llfloater.h"
#include "llframetimer.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLContextMenu;
class LLFlatListView;
class LLLineEditor;
class LLProgressBar;
class LLRadioGroup;
class LLScrollListCtrl;
class LLTextBox;

// Progress bar, status text and Stop button mirroring ASInventoryRuleRunner.
struct ASInventoryRuleRunControls
{
    void init(LLFloater* floater);
    void update();

    LLProgressBar* mProgress = nullptr;
    LLTextBox* mStatus = nullptr;
    LLButton* mStopBtn = nullptr;
    std::string mLastStatus;
};

// Shared list window: rows with Up / Down / Execute / Edit / Delete, New and Execute All
// buttons, and the run controls. Subclasses provide the rows and the actions.
class ASFloaterInventoryRuleList : public LLFloater
{
public:
    ASFloaterInventoryRuleList(const LLSD& key);
    virtual ~ASFloaterInventoryRuleList() = default;

    bool postBuild() override;
    void draw() override;

    // Rebuilds the list on the next draw, so it is safe from a row's own button.
    void requestRefresh() { mRefreshPending = true; }

protected:
    struct Row
    {
        std::string mName;
        std::string mSummary;
        bool mCanExecute;
    };

    virtual std::vector<Row> getRows() const = 0;
    virtual void onNew() = 0;
    virtual void onExecuteAll() = 0;
    virtual void onMoveRow(size_t index, S32 delta) = 0;
    virtual void onExecuteRow(size_t index) = 0;
    virtual void onEditRow(size_t index) = 0;
    virtual void onDeleteRow(size_t index) = 0;

    void refreshList();

private:
    LLFlatListView* mList;
    LLButton* mExecuteAllBtn;
    bool mRefreshPending;
    ASInventoryRuleRunControls mRunControls;
};

// Top window: the rulesets.
class ASFloaterInventoryRules : public ASFloaterInventoryRuleList
{
public:
    ASFloaterInventoryRules(const LLSD& key);

    void onOpen(const LLSD& key) override;

protected:
    std::vector<Row> getRows() const override;
    void onNew() override;
    void onExecuteAll() override;
    void onMoveRow(size_t index, S32 delta) override;
    void onExecuteRow(size_t index) override;
    void onEditRow(size_t index) override;
    void onDeleteRow(size_t index) override;
};

// The rules of one ruleset (key: ruleset id), and the ruleset's name.
class ASFloaterInventoryRuleset : public ASFloaterInventoryRuleList
{
public:
    ASFloaterInventoryRuleset(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    const LLUUID& getRulesetID() const { return mRulesetID; }

protected:
    std::vector<Row> getRows() const override;
    void onNew() override;
    void onExecuteAll() override;
    void onMoveRow(size_t index, S32 delta) override;
    void onExecuteRow(size_t index) override;
    void onEditRow(size_t index) override;
    void onDeleteRow(size_t index) override;

private:
    void onRename();

    LLUUID mRulesetID;
    LLLineEditor* mNameEditor;
};

// Edits one rule (key: map with "ruleset" id and "rule" id, null for a new rule)
// with a live preview of its matches.
class ASFloaterInventoryRuleEdit : public LLFloater
{
public:
    ASFloaterInventoryRuleEdit(const LLSD& key);
    virtual ~ASFloaterInventoryRuleEdit();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    const LLUUID& getRulesetID() const { return mRulesetID; }

private:
    ASInventoryRule getRuleFromFields() const;
    void setFields(const ASInventoryRule& rule);
    void onFieldChanged();
    void updatePreview();
    void showInInventory(const LLUUID& id);
    void onSave();
    void onExecute();

    LLUUID mRulesetID;
    LLUUID mRuleID;                 // null until a new rule is saved
    bool mPreviewDirty;
    bool mInventoryWasFetched;
    bool mWasRunning;
    LLFrameTimer mPreviewTimer;
    LLHandle<LLContextMenu> mContextMenuHandle;
    LLUUID mContextMenuID;          // preview row the context menu was opened on

    LLLineEditor* mNameEditor;
    LLLineEditor* mSearchEditor;
    LLComboBox* mSearchTypeCombo;
    LLCheckBoxCtrl* mMatchCaseCheck;
    LLLineEditor* mExceptEditor;
    LLComboBox* mExceptTypeCombo;
    LLCheckBoxCtrl* mExceptMatchCaseCheck;
    LLRadioGroup* mMatchTypeRadio;
    LLComboBox* mActionCombo;
    LLRadioGroup* mDeleteModeRadio;
    LLTextBox* mDestinationLabel;
    LLLineEditor* mDestinationEditor;
    LLCheckBoxCtrl* mCreateDestinationCheck;
    LLButton* mExecuteBtn;
    LLTextBox* mPreviewCount;
    LLScrollListCtrl* mPreviewList;
    ASInventoryRuleRunControls mRunControls;
};

#endif // AS_FLOATERINVENTORYRULES_H
