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
class LLSpinCtrl;
class LLTextBox;

// Progress bar, status text and Stop button mirroring ASInventoryRuleRunner.
struct ASInventoryRuleRunControls
{
    void init(LLFloater* floater);
    void update();
    // Shown in the status line until the runner's status changes.
    void showMessage(const std::string& text);

    LLProgressBar* mProgress = nullptr;
    LLTextBox* mStatus = nullptr;
    LLButton* mStopBtn = nullptr;
    std::string mLastStatus;
};

// Shared list window: rows with Up / Down / Execute / Edit / Delete, New, Execute All, Dry Run,
// Export and Import buttons, and the run controls. Subclasses provide the rows and the actions.
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
    // Rules run by Execute All and Dry Run, in order.
    virtual ASInventoryRules::rule_list_t getAllRules() const = 0;
    virtual void onNew() = 0;
    virtual void onMoveRow(size_t index, S32 delta) = 0;
    virtual void onExecuteRow(size_t index) = 0;
    virtual void onEditRow(size_t index) = 0;
    virtual void onDeleteRow(size_t index) = 0;
    // JSON of what Export saves, and its proposed file name (without extension).
    virtual std::string getExportJson(std::string& file_name) const = 0;
    // Adds imported rulesets (a single rule comes as one ruleset holding it). Returns false with
    // message as the error when this window does not take this type.
    virtual bool onImport(ASInventoryRules::EExportType type, const ASInventoryRules::ruleset_list_t& imported,
                          std::string& message) = 0;

    void refreshList();

private:
    void importText(const std::string& text);

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
    ASInventoryRules::rule_list_t getAllRules() const override;
    void onNew() override;
    void onMoveRow(size_t index, S32 delta) override;
    void onExecuteRow(size_t index) override;
    void onEditRow(size_t index) override;
    void onDeleteRow(size_t index) override;
    std::string getExportJson(std::string& file_name) const override;
    bool onImport(ASInventoryRules::EExportType type, const ASInventoryRules::ruleset_list_t& imported,
                  std::string& message) override;
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
    ASInventoryRules::rule_list_t getAllRules() const override;
    void onNew() override;
    void onMoveRow(size_t index, S32 delta) override;
    void onExecuteRow(size_t index) override;
    void onEditRow(size_t index) override;
    void onDeleteRow(size_t index) override;
    std::string getExportJson(std::string& file_name) const override;
    bool onImport(ASInventoryRules::EExportType type, const ASInventoryRules::ruleset_list_t& imported,
                  std::string& message) override;

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
    ASInventoryRule getRuleToRun() const;

    LLUUID mRulesetID;
    LLUUID mRuleID;                 // null until a new rule is saved
    bool mPreviewDirty;
    bool mInventoryWasFetched;
    bool mWasRunning;
    U32 mPendingNames;              // creator names the last preview was missing
    LLFrameTimer mPreviewTimer;     // since the last field change
    LLFrameTimer mRetryTimer;       // since the last preview
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
    LLComboBox* mItemTypeCombo;
    LLComboBox* mAgeCombo;
    LLSpinCtrl* mAgeDaysSpin;
    LLSpinCtrl* mAgeHoursSpin;
    LLComboBox* mCreatorCombo;
    LLLineEditor* mCreatorEditor;
    LLComboBox* mPermCopyCombo;
    LLComboBox* mPermModifyCombo;
    LLComboBox* mPermTransferCombo;
    LLButton* mExecuteBtn;
    LLButton* mDryRunBtn;
    LLTextBox* mPreviewCount;
    LLScrollListCtrl* mPreviewList;
    ASInventoryRuleRunControls mRunControls;
};

// What executing rules would do (key: map with "rules", an array of rule LLSD), and Execute Now.
class ASFloaterInventoryRuleDryRun : public LLFloater
{
public:
    ASFloaterInventoryRuleDryRun(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void update();

    ASInventoryRules::rule_list_t mRules;
    bool mInventoryWasFetched;
    bool mWasRunning;
    U32 mPendingNames;
    LLFrameTimer mOpenTimer;
    LLFrameTimer mRetryTimer;
    LLTextBox* mSummary;
    LLScrollListCtrl* mList;
    LLButton* mExecuteBtn;
    ASInventoryRuleRunControls mRunControls;
};

#endif // AS_FLOATERINVENTORYRULES_H
