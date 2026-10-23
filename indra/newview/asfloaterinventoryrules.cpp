/**
 * @file asfloaterinventoryrules.cpp
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

#include "llviewerprecompiledheaders.h"

#include "asfloaterinventoryrules.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llflatlistview.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventorypanel.h"
#include "lllineeditor.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llprogressbar.h"
#include "llradiogroup.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "llviewerinventory.h"

namespace
{
constexpr F32 PREVIEW_DELAY = 0.4f;     // debounce for the live preview
constexpr size_t PREVIEW_LIMIT = 500;   // rows shown in the preview list

// Starts a run, asking first when a rule deletes permanently.
void executeRules(const ASInventoryRules::rule_list_t& rules)
{
    if (ASInventoryRuleRunner::instance().isRunning())
    {
        return;
    }

    std::string purge_names;
    for (const ASInventoryRule& rule : rules)
    {
        if (rule.isPurge())
        {
            purge_names += (purge_names.empty() ? "'" : ", '") + rule.mName + "'";
        }
    }
    if (purge_names.empty())
    {
        ASInventoryRuleRunner::instance().start(rules);
        return;
    }

    LLSD args;
    args["RULES"] = purge_names;
    LLNotificationsUtil::add("ASInventoryRulePurgeConfirm", args, LLSD(),
        [rules](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                ASInventoryRuleRunner::instance().start(rules);
            }
        });
}

void requestFullInventory()
{
    LLInventoryModelBackgroundFetch& fetch = LLInventoryModelBackgroundFetch::instance();
    if (!fetch.isEverythingFetched())
    {
        fetch.start();
    }
}

// Rebuilds the open list windows after the stored rulesets changed.
void refreshRuleWindows()
{
    if (ASFloaterInventoryRules* rules = LLFloaterReg::findTypedInstance<ASFloaterInventoryRules>("as_inventory_rules"))
    {
        rules->requestRefresh();
    }
    if (ASFloaterInventoryRuleset* ruleset = LLFloaterReg::findTypedInstance<ASFloaterInventoryRuleset>("as_inventory_ruleset"))
    {
        ruleset->requestRefresh();
    }
}

// One row of a rulesets or rules list.
class ASInventoryRuleListItem : public LLPanel
{
public:
    typedef std::function<void()> action_t;
    typedef std::function<void(S32)> move_t;

    ASInventoryRuleListItem(const std::string& name, const std::string& summary, bool can_execute,
                            bool is_first, bool is_last, move_t on_move,
                            action_t on_execute, action_t on_edit, action_t on_delete)
        : LLPanel(),
          mName(name),
          mSummary(summary),
          mCanExecute(can_execute),
          mIsFirst(is_first),
          mIsLast(is_last),
          mOnMove(on_move),
          mOnExecute(on_execute),
          mOnEdit(on_edit),
          mOnDelete(on_delete)
    {
        buildFromFile("panel_as_inventory_rule_item.xml");
    }

    bool postBuild() override
    {
        getChild<LLTextBox>("row_name")->setText(mName);
        getChild<LLTextBox>("row_summary")->setText(mSummary);

        LLButton* up_btn = getChild<LLButton>("up_btn");
        up_btn->setClickedCallback([this](LLUICtrl*, const LLSD&) { mOnMove(-1); });
        up_btn->setEnabled(!mIsFirst);
        LLButton* down_btn = getChild<LLButton>("down_btn");
        down_btn->setClickedCallback([this](LLUICtrl*, const LLSD&) { mOnMove(1); });
        down_btn->setEnabled(!mIsLast);

        LLButton* execute_btn = getChild<LLButton>("execute_btn");
        execute_btn->setClickedCallback([this](LLUICtrl*, const LLSD&) { mOnExecute(); });
        execute_btn->setEnabled(mCanExecute);
        getChild<LLButton>("edit_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { mOnEdit(); });
        getChild<LLButton>("delete_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { mOnDelete(); });
        return true;
    }

private:
    std::string mName;
    std::string mSummary;
    bool mCanExecute;
    bool mIsFirst;
    bool mIsLast;
    move_t mOnMove;
    action_t mOnExecute;
    action_t mOnEdit;
    action_t mOnDelete;
};
}

//-----------------------------------------------------------------------------
// ASInventoryRuleRunControls
//-----------------------------------------------------------------------------

void ASInventoryRuleRunControls::init(LLFloater* floater)
{
    mProgress = floater->getChild<LLProgressBar>("progress_bar");
    mStatus = floater->getChild<LLTextBox>("status_text");
    mStopBtn = floater->getChild<LLButton>("stop_btn");
    mStopBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { ASInventoryRuleRunner::instance().stop(); });
}

void ASInventoryRuleRunControls::update()
{
    const ASInventoryRuleRunner& runner = ASInventoryRuleRunner::instance();
    const F32 progress = runner.getProgress();
    mProgress->setValue(progress < 0.f ? 0.f : progress * 100.f);
    mStopBtn->setEnabled(runner.isRunning() && !runner.isStopRequested());
    if (runner.getStatus() != mLastStatus)
    {
        mLastStatus = runner.getStatus();
        mStatus->setText(mLastStatus);
    }
}

//-----------------------------------------------------------------------------
// ASFloaterInventoryRuleList
//-----------------------------------------------------------------------------

ASFloaterInventoryRuleList::ASFloaterInventoryRuleList(const LLSD& key)
    : LLFloater(key),
      mList(nullptr),
      mExecuteAllBtn(nullptr),
      mRefreshPending(false)
{
}

bool ASFloaterInventoryRuleList::postBuild()
{
    mList = getChild<LLFlatListView>("list");
    mList->setNoItemsCommentText(getString("no_items"));

    getChild<LLButton>("new_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onNew(); });
    mExecuteAllBtn = getChild<LLButton>("execute_all_btn");
    mExecuteAllBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onExecuteAll(); });

    mRunControls.init(this);
    return true;
}

void ASFloaterInventoryRuleList::draw()
{
    if (mRefreshPending)
    {
        refreshList();
    }
    mRunControls.update();
    mExecuteAllBtn->setEnabled(!ASInventoryRuleRunner::instance().isRunning());
    LLFloater::draw();
}

void ASFloaterInventoryRuleList::refreshList()
{
    mRefreshPending = false;
    mList->clear();
    const std::vector<Row> rows = getRows();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        ASInventoryRuleListItem* item = new ASInventoryRuleListItem(
            rows[i].mName, rows[i].mSummary, rows[i].mCanExecute, i == 0, i + 1 == rows.size(),
            [this, i](S32 delta) { onMoveRow(i, delta); },
            [this, i]() { onExecuteRow(i); },
            [this, i]() { onEditRow(i); },
            [this, i]() { onDeleteRow(i); });
        mList->addItem(item, LLSD((S32)i));
    }
}

//-----------------------------------------------------------------------------
// ASFloaterInventoryRules (rulesets)
//-----------------------------------------------------------------------------

ASFloaterInventoryRules::ASFloaterInventoryRules(const LLSD& key)
    : ASFloaterInventoryRuleList(key)
{
}

void ASFloaterInventoryRules::onOpen(const LLSD& key)
{
    refreshList();
    LLFloater::onOpen(key);
}

std::vector<ASFloaterInventoryRuleList::Row> ASFloaterInventoryRules::getRows() const
{
    std::vector<Row> rows;
    for (const ASInventoryRuleset& ruleset : ASInventoryRules::load())
    {
        rows.push_back({ ruleset.mName, ruleset.getSummary(), ruleset.hasAction() });
    }
    return rows;
}

void ASFloaterInventoryRules::onNew()
{
    ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    ASInventoryRuleset ruleset;
    ruleset.mID.generate();
    ruleset.mName = getString("new_ruleset");
    rulesets.push_back(ruleset);
    ASInventoryRules::save(rulesets);

    requestRefresh();
    LLFloaterReg::showInstance("as_inventory_ruleset", LLSD(ruleset.mID));
}

void ASFloaterInventoryRules::onExecuteAll()
{
    ASInventoryRules::rule_list_t rules;
    for (const ASInventoryRuleset& ruleset : ASInventoryRules::load())
    {
        rules.insert(rules.end(), ruleset.mRules.begin(), ruleset.mRules.end());
    }
    executeRules(rules);
}

void ASFloaterInventoryRules::onMoveRow(size_t index, S32 delta)
{
    ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 other = (S32)index + delta;
    if (index >= rulesets.size() || other < 0 || other >= (S32)rulesets.size())
    {
        return;
    }
    std::swap(rulesets[index], rulesets[other]);
    ASInventoryRules::save(rulesets);
    requestRefresh();
}

void ASFloaterInventoryRules::onExecuteRow(size_t index)
{
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    if (index < rulesets.size())
    {
        executeRules(rulesets[index].mRules);
    }
}

void ASFloaterInventoryRules::onEditRow(size_t index)
{
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    if (index < rulesets.size())
    {
        LLFloaterReg::showInstance("as_inventory_ruleset", LLSD(rulesets[index].mID));
    }
}

void ASFloaterInventoryRules::onDeleteRow(size_t index)
{
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    if (index >= rulesets.size())
    {
        return;
    }

    const LLUUID ruleset_id = rulesets[index].mID;
    LLSD args;
    args["NAME"] = rulesets[index].mName;
    LLNotificationsUtil::add("ASInventoryRulesetDeleteConfirm", args, LLSD(),
        [ruleset_id](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
            {
                return;
            }
            ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
            const S32 found = ASInventoryRules::findRuleset(rulesets, ruleset_id);
            if (found < 0)
            {
                return;
            }
            rulesets.erase(rulesets.begin() + found);
            ASInventoryRules::save(rulesets);

            // Windows showing the deleted ruleset have nothing left to edit.
            ASFloaterInventoryRuleset* ruleset = LLFloaterReg::findTypedInstance<ASFloaterInventoryRuleset>("as_inventory_ruleset");
            if (ruleset && ruleset->getRulesetID() == ruleset_id)
            {
                ruleset->closeFloater();
            }
            ASFloaterInventoryRuleEdit* editor = LLFloaterReg::findTypedInstance<ASFloaterInventoryRuleEdit>("as_inventory_rule_edit");
            if (editor && editor->getRulesetID() == ruleset_id)
            {
                editor->closeFloater();
            }
            refreshRuleWindows();
        });
}

//-----------------------------------------------------------------------------
// ASFloaterInventoryRuleset (rules of one ruleset)
//-----------------------------------------------------------------------------

ASFloaterInventoryRuleset::ASFloaterInventoryRuleset(const LLSD& key)
    : ASFloaterInventoryRuleList(key),
      mNameEditor(nullptr)
{
}

bool ASFloaterInventoryRuleset::postBuild()
{
    mNameEditor = getChild<LLLineEditor>("ruleset_name");
    mNameEditor->setCommitOnFocusLost(true);
    mNameEditor->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRename(); });
    return ASFloaterInventoryRuleList::postBuild();
}

void ASFloaterInventoryRuleset::onOpen(const LLSD& key)
{
    mRulesetID = key.asUUID();
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 index = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    mNameEditor->setText(index >= 0 ? rulesets[index].mName : std::string());

    // A previous run's result does not belong to the ruleset being opened.
    ASInventoryRuleRunner::instance().clearStatus();
    refreshList();
    LLFloater::onOpen(key);
}

void ASFloaterInventoryRuleset::onRename()
{
    std::string name = mNameEditor->getText();
    LLStringUtil::trim(name);
    ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 index = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    if (index < 0 || name.empty() || name == rulesets[index].mName)
    {
        return;
    }
    rulesets[index].mName = name;
    ASInventoryRules::save(rulesets);
    refreshRuleWindows();
}

std::vector<ASFloaterInventoryRuleList::Row> ASFloaterInventoryRuleset::getRows() const
{
    std::vector<Row> rows;
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 index = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    if (index >= 0)
    {
        for (const ASInventoryRule& rule : rulesets[index].mRules)
        {
            rows.push_back({ rule.mName, rule.getSummary(), rule.mAction != ASInventoryRule::ACTION_NONE });
        }
    }
    return rows;
}

void ASFloaterInventoryRuleset::onNew()
{
    LLSD key;
    key["ruleset"] = mRulesetID;
    key["rule"] = LLUUID::null;
    LLFloaterReg::showInstance("as_inventory_rule_edit", key);
}

void ASFloaterInventoryRuleset::onExecuteAll()
{
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 index = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    if (index >= 0)
    {
        executeRules(rulesets[index].mRules);
    }
}

void ASFloaterInventoryRuleset::onMoveRow(size_t index, S32 delta)
{
    ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 ruleset = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    if (ruleset < 0)
    {
        return;
    }
    ASInventoryRules::rule_list_t& rules = rulesets[ruleset].mRules;
    const S32 other = (S32)index + delta;
    if (index >= rules.size() || other < 0 || other >= (S32)rules.size())
    {
        return;
    }
    std::swap(rules[index], rules[other]);
    ASInventoryRules::save(rulesets);
    refreshRuleWindows();
}

void ASFloaterInventoryRuleset::onExecuteRow(size_t index)
{
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 ruleset = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    if (ruleset >= 0 && index < rulesets[ruleset].mRules.size())
    {
        executeRules(ASInventoryRules::rule_list_t(1, rulesets[ruleset].mRules[index]));
    }
}

void ASFloaterInventoryRuleset::onEditRow(size_t index)
{
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 ruleset = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    if (ruleset >= 0 && index < rulesets[ruleset].mRules.size())
    {
        LLSD key;
        key["ruleset"] = mRulesetID;
        key["rule"] = rulesets[ruleset].mRules[index].mID;
        LLFloaterReg::showInstance("as_inventory_rule_edit", key);
    }
}

void ASFloaterInventoryRuleset::onDeleteRow(size_t index)
{
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 ruleset = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    if (ruleset < 0 || index >= rulesets[ruleset].mRules.size())
    {
        return;
    }

    const LLUUID ruleset_id = mRulesetID;
    const LLUUID rule_id = rulesets[ruleset].mRules[index].mID;
    LLSD args;
    args["NAME"] = rulesets[ruleset].mRules[index].mName;
    LLNotificationsUtil::add("ASInventoryRuleDeleteConfirm", args, LLSD(),
        [ruleset_id, rule_id](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
            {
                return;
            }
            ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
            const S32 ruleset = ASInventoryRules::findRuleset(rulesets, ruleset_id);
            if (ruleset < 0)
            {
                return;
            }
            ASInventoryRules::rule_list_t& rules = rulesets[ruleset].mRules;
            const S32 rule = ASInventoryRules::findRule(rules, rule_id);
            if (rule < 0)
            {
                return;
            }
            // An open editor on this rule keeps its fields; saving re-adds the rule.
            rules.erase(rules.begin() + rule);
            ASInventoryRules::save(rulesets);
            refreshRuleWindows();
        });
}

//-----------------------------------------------------------------------------
// ASFloaterInventoryRuleEdit
//-----------------------------------------------------------------------------

ASFloaterInventoryRuleEdit::ASFloaterInventoryRuleEdit(const LLSD& key)
    : LLFloater(key),
      mPreviewDirty(false),
      mInventoryWasFetched(false),
      mWasRunning(false),
      mNameEditor(nullptr),
      mSearchEditor(nullptr),
      mSearchTypeCombo(nullptr),
      mMatchCaseCheck(nullptr),
      mExceptEditor(nullptr),
      mExceptTypeCombo(nullptr),
      mExceptMatchCaseCheck(nullptr),
      mMatchTypeRadio(nullptr),
      mActionCombo(nullptr),
      mDeleteModeRadio(nullptr),
      mDestinationLabel(nullptr),
      mDestinationEditor(nullptr),
      mCreateDestinationCheck(nullptr),
      mExecuteBtn(nullptr),
      mPreviewCount(nullptr),
      mPreviewList(nullptr)
{
}

ASFloaterInventoryRuleEdit::~ASFloaterInventoryRuleEdit()
{
    if (LLContextMenu* menu = mContextMenuHandle.get())
    {
        menu->die();
    }
}

bool ASFloaterInventoryRuleEdit::postBuild()
{
    mNameEditor = getChild<LLLineEditor>("rule_name");
    mSearchEditor = getChild<LLLineEditor>("search_text");
    mSearchTypeCombo = getChild<LLComboBox>("search_type");
    mMatchCaseCheck = getChild<LLCheckBoxCtrl>("match_case");
    mExceptEditor = getChild<LLLineEditor>("except_text");
    mExceptTypeCombo = getChild<LLComboBox>("except_type");
    mExceptMatchCaseCheck = getChild<LLCheckBoxCtrl>("except_match_case");
    mMatchTypeRadio = getChild<LLRadioGroup>("match_type");
    mActionCombo = getChild<LLComboBox>("action");
    mDeleteModeRadio = getChild<LLRadioGroup>("delete_mode");
    mDestinationLabel = getChild<LLTextBox>("destination_label");
    mDestinationEditor = getChild<LLLineEditor>("destination");
    mCreateDestinationCheck = getChild<LLCheckBoxCtrl>("create_destination");
    mPreviewCount = getChild<LLTextBox>("preview_count");
    mPreviewList = getChild<LLScrollListCtrl>("preview_list");

    // Text fields refresh the preview while typing, other controls on commit.
    auto on_keystroke = [this](LLLineEditor*, void*) { onFieldChanged(); };
    mSearchEditor->setKeystrokeCallback(on_keystroke, nullptr);
    mExceptEditor->setKeystrokeCallback(on_keystroke, nullptr);
    mDestinationEditor->setKeystrokeCallback(on_keystroke, nullptr);

    auto on_commit = [this](LLUICtrl*, const LLSD&) { onFieldChanged(); };
    mSearchTypeCombo->setCommitCallback(on_commit);
    mExceptTypeCombo->setCommitCallback(on_commit);
    mExceptMatchCaseCheck->setCommitCallback(on_commit);
    mMatchCaseCheck->setCommitCallback(on_commit);
    mMatchTypeRadio->setCommitCallback(on_commit);
    mActionCombo->setCommitCallback(on_commit);
    mDeleteModeRadio->setCommitCallback(on_commit);
    mCreateDestinationCheck->setCommitCallback(on_commit);

    // Preview rows: right-click opens a menu, double-click shows the object in My Inventory.
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    registrar.add("ASInventoryRules.ShowInInventory", [this](LLUICtrl*, const LLSD&) { showInInventory(mContextMenuID); });
    LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
        "menu_as_inventory_rule_preview.xml", LLMenuGL::sMenuContainer, LLMenuHolderGL::child_registry_t::instance());
    if (menu)
    {
        mContextMenuHandle = menu->getHandle();
    }
    mPreviewList->setRightMouseDownCallback([this](LLUICtrl*, S32 x, S32 y, MASK)
        {
            LLScrollListItem* row = mPreviewList->hitItem(x, y);
            LLContextMenu* menu = mContextMenuHandle.get();
            if (row && menu)
            {
                mPreviewList->deselectAllItems(true);
                row->setSelected(true);
                mContextMenuID = row->getValue().asUUID();
                // show() makes the context menu visible; showPopup() places it (as LLListContextMenu::show).
                menu->show(x, y);
                LLMenuGL::showPopup(mPreviewList, menu, x, y);
            }
        });
    mPreviewList->setDoubleClickCallback([this]()
        {
            if (LLScrollListItem* row = mPreviewList->getFirstSelected())
            {
                showInInventory(row->getValue().asUUID());
            }
        });

    getChild<LLButton>("save_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
    mExecuteBtn = getChild<LLButton>("execute_btn");
    mExecuteBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onExecute(); });

    mRunControls.init(this);
    return true;
}

void ASFloaterInventoryRuleEdit::onOpen(const LLSD& key)
{
    mRulesetID = key["ruleset"].asUUID();
    mRuleID = key["rule"].asUUID();

    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 ruleset = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    const S32 rule = (ruleset >= 0) ? ASInventoryRules::findRule(rulesets[ruleset].mRules, mRuleID) : -1;
    if (rule >= 0)
    {
        setFields(rulesets[ruleset].mRules[rule]);
    }
    else
    {
        mRuleID.setNull();
        setFields(ASInventoryRule());
    }

    // A previous run's result does not belong to the rule being opened.
    ASInventoryRuleRunner::instance().clearStatus();

    // The preview is only complete once the whole inventory is loaded.
    requestFullInventory();
    mInventoryWasFetched = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
    updatePreview();
    LLFloater::onOpen(key);
}

void ASFloaterInventoryRuleEdit::draw()
{
    const ASInventoryRuleRunner& runner = ASInventoryRuleRunner::instance();

    // Refresh the preview when loading completes and after a run changed the inventory.
    if (!mInventoryWasFetched && LLInventoryModelBackgroundFetch::instance().isEverythingFetched())
    {
        mInventoryWasFetched = true;
        mPreviewDirty = true;
    }
    if (mWasRunning && !runner.isRunning())
    {
        mPreviewDirty = true;
    }
    mWasRunning = runner.isRunning();

    if (mPreviewDirty && mPreviewTimer.getElapsedTimeF32() > PREVIEW_DELAY)
    {
        updatePreview();
    }

    mRunControls.update();
    mExecuteBtn->setEnabled(!runner.isRunning()
                            && mActionCombo->getValue().asString() != "none");
    LLFloater::draw();
}

ASInventoryRule ASFloaterInventoryRuleEdit::getRuleFromFields() const
{
    // The controls hold the same value strings as the stored LLSD.
    LLSD sd;
    sd["name"] = mNameEditor->getText();
    sd["search"] = mSearchEditor->getText();
    sd["search_type"] = mSearchTypeCombo->getValue();
    sd["match_case"] = mMatchCaseCheck->getValue().asBoolean();
    sd["except"] = mExceptEditor->getText();
    sd["except_type"] = mExceptTypeCombo->getValue();
    sd["except_match_case"] = mExceptMatchCaseCheck->getValue().asBoolean();
    sd["match"] = mMatchTypeRadio->getValue();
    sd["action"] = mActionCombo->getValue();
    sd["delete_mode"] = mDeleteModeRadio->getValue();
    sd["destination"] = mDestinationEditor->getText();
    sd["create_destination"] = mCreateDestinationCheck->getValue().asBoolean();

    ASInventoryRule rule = ASInventoryRule::fromLLSD(sd);
    LLStringUtil::trim(rule.mName);
    return rule;
}

void ASFloaterInventoryRuleEdit::setFields(const ASInventoryRule& rule)
{
    const LLSD sd = rule.asLLSD();
    mNameEditor->setText(rule.mName);
    mSearchEditor->setText(rule.mSearch);
    mSearchTypeCombo->setValue(sd["search_type"]);
    mMatchCaseCheck->setValue(rule.mMatchCase);
    mExceptEditor->setText(rule.mExcept);
    mExceptTypeCombo->setValue(sd["except_type"]);
    mExceptMatchCaseCheck->setValue(rule.mExceptMatchCase);
    mMatchTypeRadio->setValue(sd["match"]);
    mActionCombo->setValue(sd["action"]);
    mDeleteModeRadio->setValue(sd["delete_mode"]);
    mDestinationEditor->setText(rule.mDestination);
    mCreateDestinationCheck->setValue(rule.mCreateDestination);
    onFieldChanged();
}

void ASFloaterInventoryRuleEdit::onFieldChanged()
{
    const std::string action = mActionCombo->getValue().asString();
    const bool is_move = action == "move";
    mDeleteModeRadio->setVisible(action == "delete");
    mDestinationLabel->setVisible(is_move);
    mDestinationEditor->setVisible(is_move);
    mCreateDestinationCheck->setVisible(is_move);

    mPreviewDirty = true;
    mPreviewTimer.reset();
}

void ASFloaterInventoryRuleEdit::updatePreview()
{
    mPreviewDirty = false;
    mPreviewList->deleteAllItems();

    const ASInventoryRule rule = getRuleFromFields();
    if (rule.mSearch.empty())
    {
        mPreviewCount->setText(getString("enter_search"));
        return;
    }

    LLUUID dest_id;
    if (rule.mAction == ASInventoryRule::ACTION_MOVE)
    {
        dest_id = ASInventoryRules::findFolderByPath(rule.mDestination);
    }

    uuid_vec_t matches;
    std::string error;
    if (!ASInventoryRuleScanner::scan(rule, dest_id, matches, error))
    {
        mPreviewCount->setText(error);
        return;
    }

    const size_t shown = llmin(matches.size(), PREVIEW_LIMIT);
    for (size_t i = 0; i < shown; ++i)
    {
        const LLInventoryObject* obj = gInventory.getObject(matches[i]);
        if (!obj)
        {
            continue;
        }
        const std::string reason = ASInventoryRules::getSkipReason(rule, matches[i], dest_id);

        LLSD row;
        row["value"] = matches[i];
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = obj->getName();
        row["columns"][1]["column"] = "path";
        row["columns"][1]["value"] = ASInventoryRules::getPath(obj->getParentUUID());
        row["columns"][2]["column"] = "status";
        row["columns"][2]["value"] = reason.empty() ? getString("status_ok") : reason;
        mPreviewList->addElement(row, ADD_BOTTOM);
    }

    LLStringUtil::format_map_t args;
    args["[COUNT]"] = llformat("%u", (U32)matches.size());
    args["[LIMIT]"] = llformat("%u", (U32)PREVIEW_LIMIT);
    std::string text = getString(matches.size() > PREVIEW_LIMIT ? "match_count_limited" : "match_count", args);
    if (!LLInventoryModelBackgroundFetch::instance().isEverythingFetched())
    {
        text += " " + getString("inventory_loading");
    }
    if (rule.mAction == ASInventoryRule::ACTION_MOVE && dest_id.isNull())
    {
        text += " " + getString(rule.mCreateDestination ? "destination_created" : "destination_missing");
    }
    mPreviewCount->setText(text);
}

void ASFloaterInventoryRuleEdit::showInInventory(const LLUUID& id)
{
    if (gInventory.getObject(id))
    {
        LLInventoryPanel::openInventoryPanelAndSetSelection(true, id, true);
    }
}

void ASFloaterInventoryRuleEdit::onSave()
{
    ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 ruleset = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    if (ruleset < 0)
    {
        mPreviewCount->setText(getString("ruleset_missing"));
        return;
    }
    ASInventoryRules::rule_list_t& rules = rulesets[ruleset].mRules;

    ASInventoryRule rule = getRuleFromFields();
    if (rule.mName.empty())
    {
        rule.mName = rule.mSearch.empty() ? llformat("Rule %u", (U32)rules.size() + 1) : rule.mSearch;
        mNameEditor->setText(rule.mName);
    }
    if (mRuleID.isNull())
    {
        mRuleID.generate();
    }
    rule.mID = mRuleID;

    // Replace the rule, or append it when new (or deleted from the list meanwhile).
    const S32 index = ASInventoryRules::findRule(rules, mRuleID);
    if (index >= 0)
    {
        rules[index] = rule;
    }
    else
    {
        rules.push_back(rule);
    }
    ASInventoryRules::save(rulesets);
    refreshRuleWindows();
}

void ASFloaterInventoryRuleEdit::onExecute()
{
    // Runs the current fields, saved or not.
    ASInventoryRule rule = getRuleFromFields();
    if (rule.mName.empty())
    {
        rule.mName = getString("unsaved_rule");
    }
    executeRules(ASInventoryRules::rule_list_t(1, rule));
}
