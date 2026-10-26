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
#include "llclipboard.h"
#include "llcombobox.h"
#include "lldir.h"
#include "llfile.h"
#include "llfilepicker.h"
#include "llflatlistview.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventorypanel.h"
#include "lllineeditor.h"
#include "llmenugl.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llprogressbar.h"
#include "llradiogroup.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "llviewerinventory.h"
#include "llviewermenufile.h"

#include <iterator>

namespace
{
constexpr F32 PREVIEW_DELAY = 0.4f;     // debounce for the live preview
constexpr size_t PREVIEW_LIMIT = 500;   // rows shown in the preview list
constexpr size_t DRY_RUN_LIMIT = 5000;  // object rows shown in the dry run list
// Missing creator names: refresh every NAMES_RETRY seconds, for NAMES_TIMEOUT seconds.
constexpr F32 NAMES_RETRY = 1.f;
constexpr F32 NAMES_TIMEOUT = 30.f;
constexpr const char* EXECUTE_WARNING = "ASInventoryRuleExecuteConfirm";
constexpr std::streamsize IMPORT_FILE_MAX_SIZE = 4 * 1024 * 1024;

typedef std::function<void(const std::string&)> text_callback_t;

void showTransferError(const std::string& message)
{
    LLSD args;
    args["MESSAGE"] = message;
    LLNotificationsUtil::add("ASInventoryRuleTransferError", args);
}

// Export and Import flyout buttons: the button itself uses the clipboard, the arrow also offers a file.
bool usesFile(const LLUICtrl* ctrl)
{
    return ctrl->getValue().asString() == "file";
}

// report receives the success message.
void exportText(const std::string& json, const std::string& file_name, bool to_file, text_callback_t report)
{
    if (!to_file)
    {
        const LLWString text = utf8str_to_wstring(json);
        LLClipboard::instance().copyToClipboard(text, 0, (S32)text.size());
        report("Exported to the clipboard.");
        return;
    }

    LLFilePickerReplyThread::startPicker(
        [json, report](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (filenames.empty())
            {
                return;
            }
            llofstream out(filenames[0].c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (out.is_open())
            {
                out << json;
                out.close();
            }
            if (out.fail())
            {
                showTransferError("Cannot write " + filenames[0] + ".");
                return;
            }
            report("Exported to " + filenames[0] + ".");
        },
        LLFilePicker::FFSAVE_ALL, LLDir::getScrubbedFileName(file_name + ".json"));
}

// on_text receives the clipboard text or the file content.
void readImportText(bool from_file, text_callback_t on_text)
{
    if (!from_file)
    {
        LLWString text;
        LLClipboard::instance().pasteFromClipboard(text);
        on_text(wstring_to_utf8str(text));
        return;
    }

    LLFilePickerReplyThread::startPicker(
        [on_text](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (filenames.empty())
            {
                return;
            }
            llifstream in(filenames[0].c_str(), std::ios::in | std::ios::binary);
            if (!in.is_open())
            {
                showTransferError("Cannot read " + filenames[0] + ".");
                return;
            }
            if (llifstream_size(in) > IMPORT_FILE_MAX_SIZE)
            {
                showTransferError(filenames[0] + " is too large to be inventory rules.");
                return;
            }
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            on_text(text);
        },
        LLFilePicker::FFLOAD_ALL, false);
}

void showDryRun(const ASInventoryRules::rule_list_t& rules)
{
    LLSD key;
    key["rules"] = LLSD::emptyArray();
    for (const ASInventoryRule& rule : rules)
    {
        key["rules"].append(rule.asLLSD());
    }
    LLFloaterReg::showInstance("as_inventory_rule_dry_run", key);
}

// Every Execute button: the warning (Cancel / Dry Run / Execute Now) unless the user turned it off,
// then the permanent delete confirmation when a rule purges. warn is false after a dry run.
void executeRules(const ASInventoryRules::rule_list_t& rules, bool warn = true)
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

    // The warning includes the permanent delete names, so it replaces the purge confirmation.
    if (warn && !LLNotifications::instance().getIgnored(EXECUTE_WARNING))
    {
        LLSD args;
        args["PURGE"] = purge_names.empty() ? std::string()
                        : "\n\nThese rules delete permanently, without going through the Trash: " + purge_names;
        LLNotificationsUtil::add(EXECUTE_WARNING, args, LLSD(),
            [rules](const LLSD& notification, const LLSD& response)
            {
                switch (LLNotificationsUtil::getSelectedOption(notification, response))
                {
                case 1:
                    showDryRun(rules);
                    break;
                case 2:
                    ASInventoryRuleRunner::instance().start(rules);
                    break;
                default:
                    break;
                }
            });
        return;
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

void ASInventoryRuleRunControls::showMessage(const std::string& text)
{
    mStatus->setText(text);
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
    mExecuteAllBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { executeRules(getAllRules()); });
    getChild<LLButton>("dry_run_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { showDryRun(getAllRules()); });

    // The file pickers answer later, the window may be closed by then.
    LLHandle<ASFloaterInventoryRuleList> handle = getDerivedHandle<ASFloaterInventoryRuleList>();
    getChild<LLUICtrl>("export_btn")->setCommitCallback([this, handle](LLUICtrl* ctrl, const LLSD&)
        {
            std::string file_name;
            const std::string json = getExportJson(file_name);
            // Unreadable rules would export as an empty list.
            if (!ASInventoryRules::getStorageError().empty())
            {
                showTransferError(ASInventoryRules::getStorageError());
                return;
            }
            exportText(json, file_name, usesFile(ctrl), [handle](const std::string& message)
                {
                    if (ASFloaterInventoryRuleList* floater = handle.get())
                    {
                        floater->mRunControls.showMessage(message);
                    }
                });
        });
    getChild<LLUICtrl>("import_btn")->setCommitCallback([handle](LLUICtrl* ctrl, const LLSD&)
        {
            if (!ASInventoryRules::getStorageError().empty())
            {
                showTransferError(ASInventoryRules::getStorageError());
                return;
            }
            readImportText(usesFile(ctrl), [handle](const std::string& text)
                {
                    if (ASFloaterInventoryRuleList* floater = handle.get())
                    {
                        floater->importText(text);
                    }
                });
        });

    mRunControls.init(this);
    return true;
}

void ASFloaterInventoryRuleList::importText(const std::string& text)
{
    ASInventoryRules::EExportType type = ASInventoryRules::EXPORT_RULE;
    ASInventoryRules::ruleset_list_t imported;
    std::string message;
    if (!ASInventoryRules::importJson(text, type, imported, message) || !onImport(type, imported, message))
    {
        showTransferError(message);
        return;
    }
    mRunControls.showMessage(message);
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

bool ASFloaterInventoryRules::postBuild()
{
    if (!ASFloaterInventoryRuleList::postBuild())
    {
        return false;
    }
    // Asks the run ID; ASInventoryRuleRevert reports errors and the result.
    getChild<LLButton>("revert_btn")->setCommitCallback([](LLUICtrl*, const LLSD&)
        {
            LLNotificationsUtil::add("ASInventoryRuleRevertPrompt", LLSD(), LLSD(),
                [](const LLSD& notification, const LLSD& response)
                {
                    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                    {
                        ASInventoryRuleRevert::start(response["message"].asString());
                    }
                });
        });
    return true;
}

void ASFloaterInventoryRules::onOpen(const LLSD& key)
{
    refreshList();
    // Rules saved by a newer viewer: say why the list is empty.
    const std::string& storage_error = ASInventoryRules::getStorageError();
    getChild<LLFlatListView>("list")->setNoItemsCommentText(storage_error.empty() ? getString("no_items") : storage_error);
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

ASInventoryRules::rule_list_t ASFloaterInventoryRules::getAllRules() const
{
    ASInventoryRules::rule_list_t rules;
    for (const ASInventoryRuleset& ruleset : ASInventoryRules::load())
    {
        rules.insert(rules.end(), ruleset.mRules.begin(), ruleset.mRules.end());
    }
    return rules;
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

std::string ASFloaterInventoryRules::getExportJson(std::string& file_name) const
{
    file_name = "Inventory rules";
    return ASInventoryRules::exportJson(ASInventoryRules::load());
}

bool ASFloaterInventoryRules::onImport(ASInventoryRules::EExportType type, const ASInventoryRules::ruleset_list_t& imported,
                                       std::string& message)
{
    if (type == ASInventoryRules::EXPORT_RULE)
    {
        message = "This is a single rule. Open a ruleset and import it there.";
        return false;
    }

    ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    U32 rule_count = 0;
    for (const ASInventoryRuleset& ruleset : imported)
    {
        rulesets.push_back(ruleset);
        rule_count += (U32)ruleset.mRules.size();
    }
    if (!ASInventoryRules::save(rulesets))
    {
        message = ASInventoryRules::getStorageError();
        return false;
    }
    requestRefresh();
    message = llformat("Imported %u ruleset(s) with %u rule(s), added at the end of the list.", (U32)imported.size(), rule_count);
    return true;
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

ASInventoryRules::rule_list_t ASFloaterInventoryRuleset::getAllRules() const
{
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 index = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    return index >= 0 ? rulesets[index].mRules : ASInventoryRules::rule_list_t();
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

std::string ASFloaterInventoryRuleset::getExportJson(std::string& file_name) const
{
    const ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 index = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    const ASInventoryRuleset ruleset = index >= 0 ? rulesets[index] : ASInventoryRuleset();
    file_name = ruleset.mName.empty() ? "Inventory ruleset" : ruleset.mName;
    return ASInventoryRules::exportJson(ruleset);
}

bool ASFloaterInventoryRuleset::onImport(ASInventoryRules::EExportType type, const ASInventoryRules::ruleset_list_t& imported,
                                         std::string& message)
{
    if (type == ASInventoryRules::EXPORT_RULESETS || imported.size() != 1)
    {
        message = "This is a list of rulesets. Import it in the Inventory Rules window.";
        return false;
    }

    ASInventoryRules::ruleset_list_t rulesets = ASInventoryRules::load();
    const S32 index = ASInventoryRules::findRuleset(rulesets, mRulesetID);
    if (index < 0)
    {
        message = "This ruleset no longer exists.";
        return false;
    }
    // A ruleset's rules join this one; its name is not used.
    const ASInventoryRules::rule_list_t& rules = imported[0].mRules;
    rulesets[index].mRules.insert(rulesets[index].mRules.end(), rules.begin(), rules.end());
    if (!ASInventoryRules::save(rulesets))
    {
        message = ASInventoryRules::getStorageError();
        return false;
    }
    refreshRuleWindows();
    message = llformat("Imported %u rule(s), added at the end of this ruleset.", (U32)rules.size());
    return true;
}

//-----------------------------------------------------------------------------
// ASFloaterInventoryRuleEdit
//-----------------------------------------------------------------------------

ASFloaterInventoryRuleEdit::ASFloaterInventoryRuleEdit(const LLSD& key)
    : LLFloater(key),
      mPreviewDirty(false),
      mInventoryWasFetched(false),
      mWasRunning(false),
      mPendingNames(0),
      mNameEditor(nullptr),
      mSearchEditor(nullptr),
      mSearchTypeCombo(nullptr),
      mMatchCaseCheck(nullptr),
      mExceptEditor(nullptr),
      mExceptTypeCombo(nullptr),
      mExceptMatchCaseCheck(nullptr),
      mMatchTypeRadio(nullptr),
      mActionCombo(nullptr),
      mDeleteWarning(nullptr),
      mDestinationLabel(nullptr),
      mDestinationEditor(nullptr),
      mCreateDestinationCheck(nullptr),
      mItemTypeCombo(nullptr),
      mAgeCombo(nullptr),
      mAgeDaysSpin(nullptr),
      mAgeHoursSpin(nullptr),
      mCreatorCombo(nullptr),
      mCreatorEditor(nullptr),
      mPermCopyCombo(nullptr),
      mPermModifyCombo(nullptr),
      mPermTransferCombo(nullptr),
      mExecuteBtn(nullptr),
      mDryRunBtn(nullptr),
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
    mDeleteWarning = getChild<LLTextBox>("delete_warning");
    mDestinationLabel = getChild<LLTextBox>("destination_label");
    mDestinationEditor = getChild<LLLineEditor>("destination");
    mCreateDestinationCheck = getChild<LLCheckBoxCtrl>("create_destination");
    mItemTypeCombo = getChild<LLComboBox>("item_type");
    mAgeCombo = getChild<LLComboBox>("age");
    mAgeDaysSpin = getChild<LLSpinCtrl>("age_days");
    mAgeHoursSpin = getChild<LLSpinCtrl>("age_hours");
    mCreatorCombo = getChild<LLComboBox>("creator");
    mCreatorEditor = getChild<LLLineEditor>("creator_name");
    mPermCopyCombo = getChild<LLComboBox>("perm_copy");
    mPermModifyCombo = getChild<LLComboBox>("perm_modify");
    mPermTransferCombo = getChild<LLComboBox>("perm_transfer");
    mPreviewCount = getChild<LLTextBox>("preview_count");
    mPreviewList = getChild<LLScrollListCtrl>("preview_list");

    // Text fields refresh the preview while typing, other controls on commit.
    auto on_keystroke = [this](LLLineEditor*, void*) { onFieldChanged(); };
    mSearchEditor->setKeystrokeCallback(on_keystroke, nullptr);
    mExceptEditor->setKeystrokeCallback(on_keystroke, nullptr);
    mDestinationEditor->setKeystrokeCallback(on_keystroke, nullptr);
    mCreatorEditor->setKeystrokeCallback(on_keystroke, nullptr);

    auto on_commit = [this](LLUICtrl*, const LLSD&) { onFieldChanged(); };
    mSearchTypeCombo->setCommitCallback(on_commit);
    mExceptTypeCombo->setCommitCallback(on_commit);
    mExceptMatchCaseCheck->setCommitCallback(on_commit);
    mMatchCaseCheck->setCommitCallback(on_commit);
    mMatchTypeRadio->setCommitCallback(on_commit);
    // Choosing a permanent delete warns once more than the red text.
    mActionCombo->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            if (mActionCombo->getValue().asString() == "delete")
            {
                LLNotificationsUtil::add("ASInventoryRuleDeleteWarning");
            }
            onFieldChanged();
        });
    mCreateDestinationCheck->setCommitCallback(on_commit);
    mItemTypeCombo->setCommitCallback(on_commit);
    mAgeCombo->setCommitCallback(on_commit);
    mAgeDaysSpin->setCommitCallback(on_commit);
    mAgeHoursSpin->setCommitCallback(on_commit);
    mCreatorCombo->setCommitCallback(on_commit);
    mPermCopyCombo->setCommitCallback(on_commit);
    mPermModifyCombo->setCommitCallback(on_commit);
    mPermTransferCombo->setCommitCallback(on_commit);

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
    // Both run the current fields, saved or not.
    mExecuteBtn = getChild<LLButton>("execute_btn");
    mExecuteBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            executeRules(ASInventoryRules::rule_list_t(1, getRuleToRun()));
        });
    mDryRunBtn = getChild<LLButton>("dry_run_btn");
    mDryRunBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            showDryRun(ASInventoryRules::rule_list_t(1, getRuleToRun()));
        });
    LLHandle<ASFloaterInventoryRuleEdit> handle = getDerivedHandle<ASFloaterInventoryRuleEdit>();
    getChild<LLUICtrl>("export_btn")->setCommitCallback([this, handle](LLUICtrl* ctrl, const LLSD&)
        {
            const ASInventoryRule rule = getRuleToRun();
            exportText(ASInventoryRules::exportJson(rule), rule.mName, usesFile(ctrl), [handle](const std::string& message)
                {
                    if (ASFloaterInventoryRuleEdit* floater = handle.get())
                    {
                        floater->mRunControls.showMessage(message);
                    }
                });
        });

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
    // Creator names arrive asynchronously.
    if (mPendingNames && mRetryTimer.getElapsedTimeF32() > NAMES_RETRY && mPreviewTimer.getElapsedTimeF32() < NAMES_TIMEOUT)
    {
        mPreviewDirty = true;
    }

    if (mPreviewDirty && mPreviewTimer.getElapsedTimeF32() > PREVIEW_DELAY)
    {
        updatePreview();
    }

    mRunControls.update();
    const bool has_action = mActionCombo->getValue().asString() != "none";
    mExecuteBtn->setEnabled(!runner.isRunning() && has_action);
    mDryRunBtn->setEnabled(has_action);
    LLFloater::draw();
}

ASInventoryRule ASFloaterInventoryRuleEdit::getRuleFromFields() const
{
    // The controls hold the same value strings as the stored LLSD, except the action combo:
    // "trash" and "delete" are the delete action in its two modes.
    const std::string action = mActionCombo->getValue().asString();
    const bool is_delete = action == "trash" || action == "delete";
    LLSD sd;
    sd["name"] = mNameEditor->getText();
    sd["search"] = mSearchEditor->getText();
    sd["search_type"] = mSearchTypeCombo->getValue();
    sd["match_case"] = mMatchCaseCheck->getValue().asBoolean();
    sd["except"] = mExceptEditor->getText();
    sd["except_type"] = mExceptTypeCombo->getValue();
    sd["except_match_case"] = mExceptMatchCaseCheck->getValue().asBoolean();
    sd["match"] = mMatchTypeRadio->getValue();
    sd["action"] = is_delete ? std::string("delete") : action;
    sd["delete_mode"] = action == "delete" ? "purge" : "trash";
    sd["destination"] = mDestinationEditor->getText();
    sd["create_destination"] = mCreateDestinationCheck->getValue().asBoolean();
    sd["item_type"] = mItemTypeCombo->getValue();
    sd["age"] = mAgeCombo->getValue();
    sd["age_days"] = mAgeDaysSpin->getValue().asInteger();
    sd["age_hours"] = mAgeHoursSpin->getValue().asInteger();
    sd["creator"] = mCreatorCombo->getValue();
    sd["creator_name"] = mCreatorEditor->getText();
    sd["perm_copy"] = mPermCopyCombo->getValue();
    sd["perm_modify"] = mPermModifyCombo->getValue();
    sd["perm_transfer"] = mPermTransferCombo->getValue();

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
    mActionCombo->setValue(rule.mAction != ASInventoryRule::ACTION_DELETE ? sd["action"]
                           : LLSD(rule.isPurge() ? "delete" : "trash"));
    mDestinationEditor->setText(rule.mDestination);
    mCreateDestinationCheck->setValue(rule.mCreateDestination);
    mItemTypeCombo->setValue(sd["item_type"]);
    mAgeCombo->setValue(sd["age"]);
    mAgeDaysSpin->setValue((S32)rule.mAgeDays);
    mAgeHoursSpin->setValue((S32)rule.mAgeHours);
    mCreatorCombo->setValue(sd["creator"]);
    mCreatorEditor->setText(rule.mCreatorName);
    mPermCopyCombo->setValue(sd["perm_copy"]);
    mPermModifyCombo->setValue(sd["perm_modify"]);
    mPermTransferCombo->setValue(sd["perm_transfer"]);
    onFieldChanged();
}

void ASFloaterInventoryRuleEdit::onFieldChanged()
{
    const std::string action = mActionCombo->getValue().asString();
    const bool is_move = action == "move";
    mDeleteWarning->setVisible(action == "delete");
    mDestinationLabel->setVisible(is_move);
    mDestinationEditor->setVisible(is_move);
    mCreateDestinationCheck->setVisible(is_move);

    // Item filters do not apply to folder matches.
    const bool item_filters = mMatchTypeRadio->getValue().asString() != "folder";
    const bool has_age = mAgeCombo->getValue().asString() != "any";
    mItemTypeCombo->setEnabled(item_filters);
    mAgeCombo->setEnabled(item_filters);
    mAgeDaysSpin->setEnabled(item_filters && has_age);
    mAgeHoursSpin->setEnabled(item_filters && has_age);
    mCreatorCombo->setEnabled(item_filters);
    mCreatorEditor->setEnabled(item_filters && mCreatorCombo->getValue().asString() == "specific");
    mPermCopyCombo->setEnabled(item_filters);
    mPermModifyCombo->setEnabled(item_filters);
    mPermTransferCombo->setEnabled(item_filters);

    mPreviewDirty = true;
    mPreviewTimer.reset();
}

void ASFloaterInventoryRuleEdit::updatePreview()
{
    mPreviewDirty = false;
    mPendingNames = 0;
    mRetryTimer.reset();
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
    if (!ASInventoryRuleScanner::scan(rule, dest_id, matches, error, ASInventoryRuleSimulation::real(), &mPendingNames))
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
    if (mPendingNames)
    {
        args["[NAMES]"] = llformat("%u", mPendingNames);
        text += " " + getString("names_loading", args);
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

ASInventoryRule ASFloaterInventoryRuleEdit::getRuleToRun() const
{
    ASInventoryRule rule = getRuleFromFields();
    if (rule.mName.empty())
    {
        rule.mName = getString("unsaved_rule");
    }
    return rule;
}

//-----------------------------------------------------------------------------
// ASFloaterInventoryRuleDryRun
//-----------------------------------------------------------------------------

ASFloaterInventoryRuleDryRun::ASFloaterInventoryRuleDryRun(const LLSD& key)
    : LLFloater(key),
      mInventoryWasFetched(false),
      mWasRunning(false),
      mPendingNames(0),
      mSummary(nullptr),
      mList(nullptr),
      mExecuteBtn(nullptr)
{
}

bool ASFloaterInventoryRuleDryRun::postBuild()
{
    mSummary = getChild<LLTextBox>("summary");
    mList = getChild<LLScrollListCtrl>("result_list");
    mList->setDoubleClickCallback([this]()
        {
            LLScrollListItem* row = mList->getFirstSelected();
            if (row && gInventory.getObject(row->getValue().asUUID()))
            {
                LLInventoryPanel::openInventoryPanelAndSetSelection(true, row->getValue().asUUID(), true);
            }
        });

    // The user has just seen what would happen: no warning, but still the permanent delete confirmation.
    mExecuteBtn = getChild<LLButton>("execute_btn");
    mExecuteBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { executeRules(mRules, false); });

    mRunControls.init(this);
    return true;
}

void ASFloaterInventoryRuleDryRun::onOpen(const LLSD& key)
{
    mRules.clear();
    const LLSD& rules = key["rules"];
    for (LLSD::array_const_iterator it = rules.beginArray(); it != rules.endArray(); ++it)
    {
        mRules.push_back(ASInventoryRule::fromLLSD(*it));
    }

    ASInventoryRuleRunner::instance().clearStatus();
    requestFullInventory();
    mInventoryWasFetched = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
    mOpenTimer.reset();
    update();
    LLFloater::onOpen(key);
}

void ASFloaterInventoryRuleDryRun::draw()
{
    const ASInventoryRuleRunner& runner = ASInventoryRuleRunner::instance();

    // Recompute when loading completes, after a run, and while creator names arrive.
    bool dirty = false;
    if (!mInventoryWasFetched && LLInventoryModelBackgroundFetch::instance().isEverythingFetched())
    {
        mInventoryWasFetched = true;
        dirty = true;
    }
    if (mWasRunning && !runner.isRunning())
    {
        dirty = true;
    }
    mWasRunning = runner.isRunning();
    if (mPendingNames && mRetryTimer.getElapsedTimeF32() > NAMES_RETRY && mOpenTimer.getElapsedTimeF32() < NAMES_TIMEOUT)
    {
        dirty = true;
    }
    if (dirty && !runner.isRunning())
    {
        update();
    }

    mRunControls.update();
    mExecuteBtn->setEnabled(!runner.isRunning() && !mRules.empty());
    LLFloater::draw();
}

void ASFloaterInventoryRuleDryRun::update()
{
    mRetryTimer.reset();
    ASInventoryRuleDryRun dry_run;
    dry_run.run(mRules);
    mPendingNames = dry_run.mPendingNames;

    // Keep the scroll position across refreshes.
    const S32 scroll = mList->getScrollPos();
    mList->deleteAllItems();

    U32 applied = 0;
    U32 skipped = 0;
    size_t shown = 0;
    for (const ASInventoryRuleDryRun::Rule& rule : dry_run.mRules)
    {
        applied += rule.mApplied;
        skipped += rule.mSkipped;

        // Rule header row.
        LLStringUtil::format_map_t args;
        args["[NAME]"] = rule.mName;
        args["[ACTION]"] = rule.mAction;
        args["[APPLIED]"] = llformat("%u", rule.mApplied);
        args["[SKIPPED]"] = llformat("%u", rule.mSkipped);
        LLSD row;
        row["value"] = LLUUID::null;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = getString("rule_header", args);
        row["columns"][0]["font"]["name"] = "SANSSERIF_SMALL";
        row["columns"][0]["font"]["style"] = "BOLD";
        row["columns"][1]["column"] = "path";
        row["columns"][1]["value"] = getString("rule_counts", args);
        row["columns"][2]["column"] = "result";
        row["columns"][2]["value"] = rule.mNote;
        mList->addElement(row, ADD_BOTTOM);

        for (const ASInventoryRuleDryRun::Entry& entry : rule.mEntries)
        {
            if (shown++ >= DRY_RUN_LIMIT)
            {
                break;
            }
            LLSD item_row;
            item_row["value"] = entry.mID;
            item_row["columns"][0]["column"] = "name";
            item_row["columns"][0]["value"] = "    " + entry.mName;
            item_row["columns"][1]["column"] = "path";
            item_row["columns"][1]["value"] = entry.mFolder;
            item_row["columns"][2]["column"] = "result";
            item_row["columns"][2]["value"] = entry.mSkipReason.empty() ? rule.mAction : "Skip: " + entry.mSkipReason;
            mList->addElement(item_row, ADD_BOTTOM);
        }
    }
    mList->setScrollPos(scroll);

    LLStringUtil::format_map_t args;
    args["[RULES]"] = llformat("%u", (U32)dry_run.mRules.size());
    args["[APPLIED]"] = llformat("%u", applied);
    args["[SKIPPED]"] = llformat("%u", skipped);
    args["[LIMIT]"] = llformat("%u", (U32)DRY_RUN_LIMIT);
    args["[NAMES]"] = llformat("%u", mPendingNames);
    std::string text = dry_run.mRules.empty() ? getString("no_rules") : getString("summary_text", args);
    if (shown > DRY_RUN_LIMIT)
    {
        text += " " + getString("limited", args);
    }
    if (!LLInventoryModelBackgroundFetch::instance().isEverythingFetched())
    {
        text += " " + getString("inventory_loading");
    }
    if (mPendingNames)
    {
        text += " " + getString("names_loading", args);
    }
    mSummary->setText(text);
}
