/**
 * @file asinventoryrules.cpp
 * @brief User-defined inventory sorting rules: model, storage, scanner and batched runner.
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

#include "asinventoryrules.h"

#include "llagent.h"
#include "llapp.h"
#include "lleventtimer.h"
#include "llframetimer.h"
#include "llgesturemgr.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llregex.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "message.h"

#include <set>

namespace
{
constexpr const char* SETTING_RULESETS = "ASInventoryRulesets";

// Server pacing. Operations go out BATCH_SIZE at a time; the next batch waits for every
// confirmation of the current one, then BATCH_PAUSE more seconds.
constexpr size_t BATCH_SIZE = 10;
constexpr F32 TICK_PERIOD = 0.1f;
constexpr F64 BATCH_PAUSE = 0.5;
constexpr F64 BATCH_TIMEOUT = 60.0;
constexpr F64 CREATE_TIMEOUT = 60.0;

const char* const SEARCH_TYPE_NAMES[] = { "contains", "starts_with", "ends_with", "regex", "exact" };
const char* const MATCH_TYPE_NAMES[] = { "item", "folder", "containing_folder" };
const char* const ACTION_NAMES[] = { "none", "move", "delete" };
const char* const DELETE_MODE_NAMES[] = { "trash", "purge" };

F64 nowSeconds()
{
    return LLFrameTimer::getElapsedSeconds();
}

template <size_t N>
S32 lookupName(const std::string& value, const char* const (&names)[N], S32 fallback)
{
    for (size_t i = 0; i < N; ++i)
    {
        if (value == names[i])
        {
            return (S32)i;
        }
    }
    return fallback;
}

// Unicode-aware lower-casing for case-insensitive name and path comparisons.
std::string lowered(const std::string& text)
{
    LLWString wtext = utf8str_to_wstring(text);
    LLWStringUtil::toLower(wtext);
    return wstring_to_utf8str(wtext);
}

LLUUID findChildFolder(const LLUUID& parent_id, const std::string& name)
{
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(parent_id, cats, items);
    if (!cats)
    {
        return LLUUID::null;
    }

    const std::string wanted = lowered(name);
    for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
    {
        if (cat && lowered(cat->getName()) == wanted)
        {
            return cat->getUUID();
        }
    }
    return LLUUID::null;
}

// Deleted gestures must not stay active (same as LLInventoryModel::removeCategory).
void deactivateGestures(const LLUUID& id)
{
    if (const LLViewerInventoryItem* item = gInventory.getItem(id))
    {
        if (item->getType() == LLAssetType::AT_GESTURE && LLGestureMgr::instance().isGestureActive(id))
        {
            LLGestureMgr::instance().deactivateGesture(id);
        }
        return;
    }

    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    gInventory.collectDescendents(id, cats, items, LLInventoryModel::EXCLUDE_TRASH);
    for (const LLPointer<LLViewerInventoryItem>& item : items)
    {
        if (item && item->getType() == LLAssetType::AT_GESTURE && !item->getIsLinkType()
            && LLGestureMgr::instance().isGestureActive(item->getUUID()))
        {
            LLGestureMgr::instance().deactivateGesture(item->getUUID());
        }
    }
}

// Circuit ack of a MoveInventoryItem/MoveInventoryFolder message.
void onReliableAck(void** data, S32 result)
{
    if (ASInventoryRuleRunner::instanceExists())
    {
        ASInventoryRuleRunner::instance().onOperationDone((U32)(uintptr_t)data, result == LL_ERR_NOERR);
    }
}

// AIS calls this once per permanent delete, whether it succeeded or not.
class ASInventoryRulePurgeCallback : public LLInventoryCallback
{
public:
    explicit ASInventoryRulePurgeCallback(U32 key) : mKey(key) {}

    void fire(const LLUUID&) override
    {
        if (ASInventoryRuleRunner::instanceExists())
        {
            ASInventoryRuleRunner::instance().onOperationDone(mKey, true);
        }
    }

private:
    U32 mKey;
};

class ASInventoryRuleTimer : public LLEventTimer
{
public:
    ASInventoryRuleTimer() : LLEventTimer(TICK_PERIOD) {}

    bool tick() override
    {
        return !ASInventoryRuleRunner::instanceExists() || ASInventoryRuleRunner::instance().onTick();
    }
};
}

//-----------------------------------------------------------------------------
// ASInventoryRule
//-----------------------------------------------------------------------------

LLSD ASInventoryRule::asLLSD() const
{
    LLSD sd;
    sd["id"] = mID;
    sd["name"] = mName;
    sd["search"] = mSearch;
    sd["search_type"] = SEARCH_TYPE_NAMES[mSearchType];
    sd["match_case"] = mMatchCase;
    sd["except"] = mExcept;
    sd["except_type"] = SEARCH_TYPE_NAMES[mExceptType];
    sd["except_match_case"] = mExceptMatchCase;
    sd["match"] = MATCH_TYPE_NAMES[mMatchType];
    sd["action"] = ACTION_NAMES[mAction];
    sd["delete_mode"] = DELETE_MODE_NAMES[mDeleteMode];
    sd["destination"] = mDestination;
    sd["create_destination"] = mCreateDestination;
    return sd;
}

ASInventoryRule ASInventoryRule::fromLLSD(const LLSD& sd)
{
    ASInventoryRule rule;
    rule.mID = sd["id"].asUUID();
    rule.mName = sd["name"].asString();
    rule.mSearch = sd["search"].asString();
    rule.mSearchType = (ESearchType)lookupName(sd["search_type"].asString(), SEARCH_TYPE_NAMES, SEARCH_CONTAINS);
    rule.mMatchCase = sd["match_case"].asBoolean();
    rule.mExcept = sd["except"].asString();
    rule.mExceptType = (ESearchType)lookupName(sd["except_type"].asString(), SEARCH_TYPE_NAMES, SEARCH_CONTAINS);
    rule.mExceptMatchCase = sd["except_match_case"].asBoolean();
    rule.mMatchType = (EMatchType)lookupName(sd["match"].asString(), MATCH_TYPE_NAMES, MATCH_ITEM);
    rule.mAction = (EAction)lookupName(sd["action"].asString(), ACTION_NAMES, ACTION_NONE);
    rule.mDeleteMode = (EDeleteMode)lookupName(sd["delete_mode"].asString(), DELETE_MODE_NAMES, DELETE_TRASH);
    rule.mDestination = sd["destination"].asString();
    rule.mCreateDestination = sd["create_destination"].asBoolean();
    return rule;
}

std::string ASInventoryRule::getSummary() const
{
    static const char* const search_labels[] = { "contains", "starts with", "ends with", "matches regex", "is exactly" };

    static const char* const match_labels[] = { "Items", "Folders", "Folders containing items" };

    std::string summary = match_labels[mMatchType];
    summary += ": " + std::string(search_labels[mSearchType]) + " '" + mSearch + "'";
    if (mMatchCase)
    {
        summary += " (match case)";
    }
    if (!mExcept.empty())
    {
        summary += ", except " + std::string(search_labels[mExceptType]) + " '" + mExcept + "'";
        if (mExceptMatchCase)
        {
            summary += " (match case)";
        }
    }

    switch (mAction)
    {
    case ACTION_MOVE:
        summary += ": move to " + mDestination;
        break;
    case ACTION_DELETE:
        summary += (mDeleteMode == DELETE_PURGE) ? ": delete permanently" : ": move to Trash";
        break;
    default:
        summary += ": no action";
        break;
    }
    return summary;
}

//-----------------------------------------------------------------------------
// ASInventoryRuleset
//-----------------------------------------------------------------------------

LLSD ASInventoryRuleset::asLLSD() const
{
    LLSD sd;
    sd["id"] = mID;
    sd["name"] = mName;
    sd["rules"] = LLSD::emptyArray();
    for (const ASInventoryRule& rule : mRules)
    {
        sd["rules"].append(rule.asLLSD());
    }
    return sd;
}

ASInventoryRuleset ASInventoryRuleset::fromLLSD(const LLSD& sd)
{
    ASInventoryRuleset ruleset;
    ruleset.mID = sd["id"].asUUID();
    if (ruleset.mID.isNull())
    {
        ruleset.mID.generate();
    }
    ruleset.mName = sd["name"].asString();
    const LLSD& rules = sd["rules"];
    for (LLSD::array_const_iterator it = rules.beginArray(); it != rules.endArray(); ++it)
    {
        ruleset.mRules.push_back(ASInventoryRule::fromLLSD(*it));
    }
    return ruleset;
}

std::string ASInventoryRuleset::getSummary() const
{
    if (mRules.empty())
    {
        return "No rules";
    }

    std::string summary = llformat("%u rule%s: ", (U32)mRules.size(), mRules.size() > 1 ? "s" : "");
    for (size_t i = 0; i < mRules.size(); ++i)
    {
        summary += (i ? ", " : "") + mRules[i].mName;
    }
    return summary;
}

bool ASInventoryRuleset::hasAction() const
{
    for (const ASInventoryRule& rule : mRules)
    {
        if (rule.mAction != ASInventoryRule::ACTION_NONE)
        {
            return true;
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
// Storage and path helpers
//-----------------------------------------------------------------------------

ASInventoryRules::ruleset_list_t ASInventoryRules::load()
{
    ruleset_list_t rulesets;
    const LLSD entries = gSavedPerAccountSettings.getLLSD(SETTING_RULESETS);
    if (entries.isArray())
    {
        for (LLSD::array_const_iterator it = entries.beginArray(); it != entries.endArray(); ++it)
        {
            rulesets.push_back(ASInventoryRuleset::fromLLSD(*it));
        }
    }
    return rulesets;
}

void ASInventoryRules::save(const ruleset_list_t& rulesets)
{
    LLSD entries = LLSD::emptyArray();
    for (const ASInventoryRuleset& ruleset : rulesets)
    {
        entries.append(ruleset.asLLSD());
    }
    gSavedPerAccountSettings.setLLSD(SETTING_RULESETS, entries);
}

S32 ASInventoryRules::findRuleset(const ruleset_list_t& rulesets, const LLUUID& id)
{
    for (size_t i = 0; i < rulesets.size(); ++i)
    {
        if (rulesets[i].mID == id)
        {
            return (S32)i;
        }
    }
    return -1;
}

S32 ASInventoryRules::findRule(const rule_list_t& rules, const LLUUID& id)
{
    for (size_t i = 0; i < rules.size(); ++i)
    {
        if (rules[i].mID == id)
        {
            return (S32)i;
        }
    }
    return -1;
}

std::vector<std::string> ASInventoryRules::splitPath(const std::string& path)
{
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= path.size())
    {
        size_t end = path.find('/', start);
        if (end == std::string::npos)
        {
            end = path.size();
        }
        std::string segment = path.substr(start, end - start);
        LLStringUtil::trim(segment);
        if (!segment.empty())
        {
            segments.push_back(segment);
        }
        start = end + 1;
    }
    return segments;
}

LLUUID ASInventoryRules::findFolderByPath(const std::string& path)
{
    const std::vector<std::string> segments = splitPath(path);
    if (segments.empty())
    {
        return LLUUID::null;
    }

    LLUUID folder_id = gInventory.getRootFolderID();
    for (const std::string& segment : segments)
    {
        folder_id = findChildFolder(folder_id, segment);
        if (folder_id.isNull())
        {
            break;
        }
    }
    return folder_id;
}

std::string ASInventoryRules::getPath(const LLUUID& id)
{
    const LLUUID& root_id = gInventory.getRootFolderID();
    std::vector<std::string> names;
    LLUUID current = id;
    while (current.notNull() && current != root_id)
    {
        const LLInventoryObject* obj = gInventory.getObject(current);
        if (!obj)
        {
            break;
        }
        names.push_back(obj->getName());
        current = obj->getParentUUID();
    }

    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
    {
        path += "/" + *it;
    }
    return path.empty() ? std::string("/") : path;
}

std::string ASInventoryRules::getSkipReason(const ASInventoryRule& rule, const LLUUID& id, const LLUUID& dest_id)
{
    const LLInventoryObject* obj = gInventory.getObject(id);
    if (!obj)
    {
        return "No longer exists";
    }
    const bool is_folder = gInventory.getCategory(id) != nullptr;

    if (rule.mAction == ASInventoryRule::ACTION_MOVE)
    {
        if (dest_id.notNull() && obj->getParentUUID() == dest_id)
        {
            return "Already in destination";
        }
        if (is_folder)
        {
            if (dest_id.notNull() && (dest_id == id || gInventory.isObjectDescendentOf(dest_id, id)))
            {
                return "Contains the destination";
            }
            if (!get_is_category_removable(&gInventory, id))
            {
                return "Protected or locked";
            }
        }
        else if (!get_is_item_removable(&gInventory, id, false))
        {
            return "Protected or locked";
        }
    }
    else if (rule.mAction == ASInventoryRule::ACTION_DELETE)
    {
        // Worn items may be moved but never deleted.
        if (is_folder)
        {
            if (!get_is_category_and_children_removable(&gInventory, id, true))
            {
                return "Protected, locked or contains worn items";
            }
        }
        else
        {
            if (get_is_item_worn(id))
            {
                return "Worn";
            }
            if (!get_is_item_removable(&gInventory, id, true))
            {
                return "Protected or locked";
            }
        }
    }
    return std::string();
}

//-----------------------------------------------------------------------------
// ASInventoryRuleMatcher
//-----------------------------------------------------------------------------

bool ASInventoryRuleMatcher::init(const std::string& search, ASInventoryRule::ESearchType type, bool match_case, std::string& error)
{
    mType = type;
    mMatchCase = match_case;
    mSearch = search;
    mUsesPath = mSearch.find('/') != std::string::npos;
    if (mSearch.empty())
    {
        error = "Search text is empty.";
        return false;
    }

    if (mType == ASInventoryRule::SEARCH_REGEX)
    {
        try
        {
            mRegex.assign(mSearch, mMatchCase ? boost::regex::perl : (boost::regex::perl | boost::regex::icase));
        }
        catch (const boost::regex_error& e)
        {
            error = std::string("Invalid regex: ") + e.what();
            return false;
        }
    }
    else if (!mMatchCase)
    {
        mSearch = lowered(mSearch);
    }
    return true;
}

bool ASInventoryRuleMatcher::matches(const std::string& subject) const
{
    if (mType == ASInventoryRule::SEARCH_REGEX)
    {
        return ll_regex_search(subject, mRegex);
    }

    const std::string text = mMatchCase ? subject : lowered(subject);
    switch (mType)
    {
    case ASInventoryRule::SEARCH_STARTS_WITH:
        return text.compare(0, mSearch.size(), mSearch) == 0;
    case ASInventoryRule::SEARCH_ENDS_WITH:
        return text.size() >= mSearch.size()
            && text.compare(text.size() - mSearch.size(), mSearch.size(), mSearch) == 0;
    case ASInventoryRule::SEARCH_EXACT:
        return text == mSearch;
    default:
        return text.find(mSearch) != std::string::npos;
    }
}

//-----------------------------------------------------------------------------
// ASInventoryRuleScanner
//-----------------------------------------------------------------------------

namespace
{
typedef std::vector<ASInventoryRuleMatcher> matcher_list_t;

// A regex is one pattern; the other search types take ';'-separated terms. Empty terms are dropped.
bool buildMatchers(const std::string& text, ASInventoryRule::ESearchType type, bool match_case,
                   matcher_list_t& matchers, std::string& error)
{
    std::vector<std::string> terms;
    if (type == ASInventoryRule::SEARCH_REGEX)
    {
        terms.push_back(text);
    }
    else
    {
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find(';', start);
            if (end == std::string::npos)
            {
                end = text.size();
            }
            terms.push_back(text.substr(start, end - start));
            start = end + 1;
        }
    }

    for (std::string& term : terms)
    {
        LLStringUtil::trim(term);
        if (term.empty())
        {
            continue;
        }
        ASInventoryRuleMatcher matcher;
        if (!matcher.init(term, type, match_case, error))
        {
            return false;
        }
        matchers.push_back(matcher);
    }
    return true;
}

// Each term compares with the path or the name, depending on whether it contains '/'.
bool anyMatches(const matcher_list_t& matchers, const std::string& name, const std::string& path)
{
    for (const ASInventoryRuleMatcher& matcher : matchers)
    {
        if (matcher.matches(matcher.usesPath() ? path : name))
        {
            return true;
        }
    }
    return false;
}
}

bool ASInventoryRuleScanner::scan(const ASInventoryRule& rule, const LLUUID& dest_id, uuid_vec_t& matches, std::string& error)
{
    matches.clear();

    matcher_list_t search_matchers;
    if (!buildMatchers(rule.mSearch, rule.mSearchType, rule.mMatchCase, search_matchers, error))
    {
        return false;
    }
    if (search_matchers.empty())
    {
        error = "Search text is empty.";
        return false;
    }

    matcher_list_t except_matchers;
    if (!buildMatchers(rule.mExcept, rule.mExceptType, rule.mExceptMatchCase, except_matchers, error))
    {
        error = "Except: " + error;
        return false;
    }

    const LLUUID& root_id = gInventory.getRootFolderID();
    if (root_id.isNull())
    {
        error = "Inventory is not available.";
        return false;
    }

    // Subtrees that are never entered, including folders protected with FIRE-29342 "Protected folders".
    std::set<LLUUID> skipped = gInventory.getProtectedCategories();
    skipped.insert(gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH));
    skipped.insert(gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT));
    skipped.insert(gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS));
    skipped.insert(gInventory.findCategoryUUIDForType(LLFolderType::FT_MARKETPLACE_LISTINGS));
    skipped.insert(dest_id);
    skipped.erase(LLUUID::null);

    // Depth-first walk carrying each folder's path ("" for the root, "/Objects/Box" below it);
    // children are pushed in reverse so results come out top-down.
    typedef std::pair<LLUUID, std::string> folder_entry_t;
    std::vector<folder_entry_t> stack(1, folder_entry_t(root_id, std::string()));
    while (!stack.empty())
    {
        const folder_entry_t folder = stack.back();
        stack.pop_back();

        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder.first, cats, items);

        if (items && rule.mMatchType == ASInventoryRule::MATCH_CONTAINING_FOLDER && folder.first != root_id)
        {
            const LLViewerInventoryCategory* folder_cat = gInventory.getCategory(folder.first);
            if (folder_cat && !LLFolderType::lookupIsProtectedType(folder_cat->getPreferredType()))
            {
                bool has_match = false;
                for (const LLPointer<LLViewerInventoryItem>& item : *items)
                {
                    if (!item || item->getIsLinkType())
                    {
                        continue;
                    }
                    const std::string path = folder.second + "/" + item->getName();
                    if (anyMatches(search_matchers, item->getName(), path)
                        && !anyMatches(except_matchers, item->getName(), path))
                    {
                        has_match = true;
                        break;
                    }
                }
                if (has_match)
                {
                    // The folder is taken whole; its subfolders are not scanned.
                    matches.push_back(folder.first);
                    continue;
                }
            }
        }

        if (items && rule.mMatchType == ASInventoryRule::MATCH_ITEM)
        {
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                if (!item || item->getIsLinkType())
                {
                    continue;
                }
                const std::string path = folder.second + "/" + item->getName();
                if (anyMatches(search_matchers, item->getName(), path)
                    && !anyMatches(except_matchers, item->getName(), path))
                {
                    matches.push_back(item->getUUID());
                }
            }
        }

        if (cats)
        {
            std::vector<folder_entry_t> children;
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                if (!cat || skipped.count(cat->getUUID()))
                {
                    continue;
                }
                const std::string path = folder.second + "/" + cat->getName();
                // An excepted folder is ignored with everything inside it.
                if (anyMatches(except_matchers, cat->getName(), path))
                {
                    continue;
                }
                // A matching folder is taken whole; its contents are not scanned.
                if (rule.mMatchType == ASInventoryRule::MATCH_FOLDER
                    && !LLFolderType::lookupIsProtectedType(cat->getPreferredType())
                    && anyMatches(search_matchers, cat->getName(), path))
                {
                    matches.push_back(cat->getUUID());
                    continue;
                }
                children.push_back(folder_entry_t(cat->getUUID(), path));
            }
            stack.insert(stack.end(), children.rbegin(), children.rend());
        }
    }
    return true;
}

//-----------------------------------------------------------------------------
// ASInventoryRuleRunner
//-----------------------------------------------------------------------------

ASInventoryRuleRunner::ASInventoryRuleRunner()
    : mState(STATE_IDLE),
      mStopRequested(false),
      mTimer(nullptr),
      mRuleIndex(0),
      mDestSegment(0),
      mCreateKey(0),
      mCreateDeadline(0.0),
      mNext(0),
      mNextKey(0),
      mBatchDeadline(0.0),
      mNextBatchTime(0.0),
      mTrashUsed(false),
      mDone(0), mSkipped(0), mFailed(0),
      mTotalDone(0), mTotalSkipped(0), mTotalFailed(0)
{
}

ASInventoryRuleRunner::~ASInventoryRuleRunner()
{
    delete mTimer;
    mTimer = nullptr;
}

bool ASInventoryRuleRunner::start(const ASInventoryRules::rule_list_t& rules)
{
    if (isRunning())
    {
        return false;
    }

    mRules.clear();
    for (const ASInventoryRule& rule : rules)
    {
        if (rule.mAction != ASInventoryRule::ACTION_NONE)
        {
            mRules.push_back(rule);
        }
    }
    if (mRules.empty())
    {
        mStatus = "No rule with an action to execute.";
        return false;
    }

    mRuleIndex = 0;
    mStopRequested = false;
    mPending.clear();
    mMatches.clear();
    mNext = 0;
    mDone = mSkipped = mFailed = 0;
    mTotalDone = mTotalSkipped = mTotalFailed = 0;
    mRuleErrors.clear();

    // Rules must see the whole inventory, not only the folders opened so far.
    mState = STATE_FETCHING;
    LLInventoryModelBackgroundFetch& fetch = LLInventoryModelBackgroundFetch::instance();
    if (!fetch.isEverythingFetched())
    {
        fetch.start();
    }
    mStatus = "Loading inventory...";

    if (!mTimer)
    {
        mTimer = new ASInventoryRuleTimer();
    }
    LL_INFOS("InventoryRules") << "Starting " << mRules.size() << " rule(s)" << LL_ENDL;
    return true;
}

void ASInventoryRuleRunner::clearStatus()
{
    if (!isRunning())
    {
        mStatus.clear();
    }
}

void ASInventoryRuleRunner::stop()
{
    if (isRunning() && !mStopRequested)
    {
        mStopRequested = true;
        updateStatus();
    }
}

F32 ASInventoryRuleRunner::getProgress() const
{
    if (mState == STATE_IDLE)
    {
        return 0.f;
    }
    if (mState == STATE_FETCHING || mRules.empty())
    {
        return -1.f;
    }

    F32 rule_fraction = 0.f;
    if (mState == STATE_APPLYING && !mMatches.empty())
    {
        rule_fraction = (F32)(mDone + mSkipped + mFailed) / (F32)mMatches.size();
    }
    return llclamp(((F32)mRuleIndex + rule_fraction) / (F32)mRules.size(), 0.f, 1.f);
}

bool ASInventoryRuleRunner::onTick()
{
    const F64 now = nowSeconds();

    switch (mState)
    {
    case STATE_FETCHING:
        if (mStopRequested)
        {
            finishRun("Stopped.");
        }
        else if (LLInventoryModelBackgroundFetch::instance().isEverythingFetched())
        {
            beginRule();
        }
        break;

    case STATE_RESOLVING:
        if (mStopRequested)
        {
            mCreateKey = 0;
            finishRun("Stopped.");
        }
        else if (mCreateKey && now > mCreateDeadline)
        {
            mCreateKey = 0;
            finishRule("Destination folder creation timed out.");
        }
        break;

    case STATE_APPLYING:
        if (!mPending.empty())
        {
            if (now > mBatchDeadline)
            {
                // Something is wrong with the connection or the inventory service: do not keep sending.
                mFailed += (U32)mPending.size();
                mPending.clear();
                finishRun("Aborted: the server did not confirm a batch within 60 seconds.");
            }
            break;
        }
        if (mStopRequested)
        {
            finishRun("Stopped.");
        }
        else if (now >= mNextBatchTime)
        {
            if (mNext >= mMatches.size())
            {
                finishRule();
            }
            else
            {
                sendBatch();
            }
        }
        break;

    default:
        break;
    }

    if (mState == STATE_IDLE)
    {
        // Returning true deletes the timer.
        mTimer = nullptr;
        return true;
    }
    return false;
}

void ASInventoryRuleRunner::beginRule()
{
    if (mRuleIndex >= mRules.size())
    {
        finishRun("Finished.");
        return;
    }

    const ASInventoryRule& rule = mRules[mRuleIndex];
    mDone = mSkipped = mFailed = 0;
    mMatches.clear();
    mNext = 0;
    mDestID.setNull();
    mTrashUsed = false;
    LL_INFOS("InventoryRules") << "Rule '" << rule.mName << "': " << rule.getSummary() << LL_ENDL;

    if (rule.mAction == ASInventoryRule::ACTION_MOVE)
    {
        mDestSegments = ASInventoryRules::splitPath(rule.mDestination);
        if (mDestSegments.empty())
        {
            finishRule("No destination folder.");
            return;
        }
        mDestSegment = 0;
        mDestID = gInventory.getRootFolderID();
        mState = STATE_RESOLVING;
        updateStatus();
        continueResolve();
    }
    else
    {
        beginApply();
    }
}

void ASInventoryRuleRunner::continueResolve()
{
    const ASInventoryRule& rule = mRules[mRuleIndex];
    while (mDestSegment < mDestSegments.size())
    {
        const LLUUID child_id = findChildFolder(mDestID, mDestSegments[mDestSegment]);
        if (child_id.notNull())
        {
            mDestID = child_id;
            ++mDestSegment;
            continue;
        }

        if (!rule.mCreateDestination)
        {
            finishRule("Destination folder not found: " + rule.mDestination);
            return;
        }

        // Create one missing folder and resume from its callback.
        mCreateKey = ++mNextKey;
        mCreateDeadline = nowSeconds() + CREATE_TIMEOUT;
        const U32 key = mCreateKey;
        LL_INFOS("InventoryRules") << "Creating destination folder '" << mDestSegments[mDestSegment] << "'" << LL_ENDL;
        gInventory.createNewCategory(mDestID, LLFolderType::FT_NONE, mDestSegments[mDestSegment],
            [key](const LLUUID& new_id)
            {
                if (ASInventoryRuleRunner::instanceExists())
                {
                    ASInventoryRuleRunner::instance().onFolderCreated(key, new_id);
                }
            });
        return;
    }
    beginApply();
}

void ASInventoryRuleRunner::onFolderCreated(U32 key, const LLUUID& new_id)
{
    if (mState != STATE_RESOLVING || key != mCreateKey)
    {
        return;
    }
    mCreateKey = 0;
    if (new_id.isNull())
    {
        finishRule("Could not create destination folder: " + mRules[mRuleIndex].mDestination);
        return;
    }
    mDestID = new_id;
    ++mDestSegment;
    continueResolve();
}

void ASInventoryRuleRunner::beginApply()
{
    const ASInventoryRule& rule = mRules[mRuleIndex];
    std::string error;
    if (!ASInventoryRuleScanner::scan(rule, mDestID, mMatches, error))
    {
        finishRule(error);
        return;
    }
    LL_INFOS("InventoryRules") << mMatches.size() << " match(es)" << LL_ENDL;

    mNext = 0;
    mNextBatchTime = nowSeconds();
    mState = STATE_APPLYING;
    updateStatus();
}

void ASInventoryRuleRunner::sendBatch()
{
    const ASInventoryRule& rule = mRules[mRuleIndex];
    size_t sent = 0;
    while (sent < BATCH_SIZE && mNext < mMatches.size())
    {
        const LLUUID id = mMatches[mNext++];
        // Re-check right before acting: the inventory may have changed since the scan.
        const std::string reason = ASInventoryRules::getSkipReason(rule, id, mDestID);
        if (!reason.empty())
        {
            ++mSkipped;
            LL_INFOS("InventoryRules") << "Skipped " << ASInventoryRules::getPath(id) << ": " << reason << LL_ENDL;
            continue;
        }
        if (sendOperation(id))
        {
            ++sent;
        }
        else
        {
            ++mFailed;
            LL_WARNS("InventoryRules") << "Could not send operation for " << ASInventoryRules::getPath(id) << LL_ENDL;
        }
    }

    const F64 now = nowSeconds();
    mBatchDeadline = now + BATCH_TIMEOUT;
    if (sent == 0)
    {
        mNextBatchTime = now;
    }
    updateStatus();
}

bool ASInventoryRuleRunner::sendOperation(const LLUUID& id)
{
    const ASInventoryRule& rule = mRules[mRuleIndex];
    if (rule.mAction == ASInventoryRule::ACTION_MOVE)
    {
        return sendMove(id, mDestID, false);
    }

    deactivateGestures(id);
    if (rule.mDeleteMode == ASInventoryRule::DELETE_PURGE)
    {
        return sendPurge(id);
    }

    const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    if (trash_id.isNull())
    {
        return false;
    }
    mTrashUsed = true;
    // Restamp like LLInventoryModel::removeItem/removeCategory.
    return sendMove(id, trash_id, true);
}

bool ASInventoryRuleRunner::sendMove(const LLUUID& id, const LLUUID& new_parent, bool restamp)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (gDisconnected || !region || new_parent.isNull())
    {
        return false;
    }

    // Same work as LLInventoryModel::changeItemParent/changeCategoryParent, but the message is
    // sent with an ack callback so the batch can wait for the server.
    LLMessageSystem* msg = gMessageSystem;
    const U32 key = ++mNextKey;
    PendingOp op;
    op.mID = id;
    LLPointer<LLViewerInventoryItem> new_item;
    LLPointer<LLViewerInventoryCategory> new_cat;

    if (LLViewerInventoryItem* item = gInventory.getItem(id))
    {
        op.mOldParent = item->getParentUUID();
        new_item = new LLViewerInventoryItem(item);
        new_item->setParent(new_parent);

        msg->newMessageFast(_PREHASH_MoveInventoryItem);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addBOOLFast(_PREHASH_Stamp, restamp);
        msg->nextBlockFast(_PREHASH_InventoryData);
        msg->addUUIDFast(_PREHASH_ItemID, id);
        msg->addUUIDFast(_PREHASH_FolderID, new_parent);
        msg->addString("NewName", NULL);
    }
    else if (LLViewerInventoryCategory* cat = gInventory.getCategory(id))
    {
        op.mOldParent = cat->getParentUUID();
        new_cat = new LLViewerInventoryCategory(cat);
        new_cat->setParent(new_parent);

        msg->newMessageFast(_PREHASH_MoveInventoryFolder);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addBOOL("Stamp", restamp);
        msg->nextBlockFast(_PREHASH_InventoryData);
        msg->addUUIDFast(_PREHASH_FolderID, id);
        msg->addUUIDFast(_PREHASH_ParentID, new_parent);
    }
    else
    {
        return false;
    }

    mPending[key] = op;
    msg->sendReliable(region->getHost(), LL_DEFAULT_RELIABLE_RETRIES, true, LL_PING_BASED_TIMEOUT_DUMMY,
                      &onReliableAck, (void**)(uintptr_t)key);

    // Local model update, after the message is sent so observers cannot interleave messages.
    gInventory.accountForUpdate(LLInventoryModel::LLCategoryUpdate(op.mOldParent, -1));
    gInventory.accountForUpdate(LLInventoryModel::LLCategoryUpdate(new_parent, 1, false));
    if (new_item)
    {
        gInventory.updateItem(new_item);
    }
    else
    {
        gInventory.updateCategory(new_cat);
    }
    gInventory.notifyObservers();
    return true;
}

bool ASInventoryRuleRunner::sendPurge(const LLUUID& id)
{
    const LLInventoryObject* obj = gInventory.getObject(id);
    if (!obj)
    {
        return false;
    }

    const U32 key = ++mNextKey;
    PendingOp op;
    op.mID = id;
    op.mOldParent = obj->getParentUUID();
    op.mIsPurge = true;
    mPending[key] = op;
    remove_inventory_object(id, new ASInventoryRulePurgeCallback(key));
    return true;
}

void ASInventoryRuleRunner::onOperationDone(U32 key, bool success)
{
    auto it = mPending.find(key);
    if (it == mPending.end())
    {
        // Stale confirmation from an aborted batch, or a duplicate callback.
        return;
    }
    const PendingOp op = it->second;
    mPending.erase(it);

    if (op.mIsPurge)
    {
        // AIS removes the object from the local model on success.
        success = gInventory.getObject(op.mID) == nullptr;
    }

    if (success)
    {
        ++mDone;
    }
    else
    {
        ++mFailed;
        LL_WARNS("InventoryRules") << "Server did not confirm the operation on " << op.mID << LL_ENDL;
        if (!op.mIsPurge)
        {
            // The local model was moved optimistically: resync both folders.
            gInventory.fetchDescendentsOf(op.mOldParent);
            const LLInventoryObject* obj = gInventory.getObject(op.mID);
            if (obj)
            {
                gInventory.fetchDescendentsOf(obj->getParentUUID());
            }
        }
    }

    if (mPending.empty())
    {
        mNextBatchTime = nowSeconds() + BATCH_PAUSE;
    }
    updateStatus();
}

void ASInventoryRuleRunner::foldRuleCounters()
{
    mTotalDone += mDone;
    mTotalSkipped += mSkipped;
    mTotalFailed += mFailed;
    mDone = mSkipped = mFailed = 0;
}

void ASInventoryRuleRunner::finishRule(const std::string& error)
{
    const ASInventoryRule& rule = mRules[mRuleIndex];
    if (!error.empty())
    {
        LL_WARNS("InventoryRules") << "Rule '" << rule.mName << "': " << error << LL_ENDL;
        mRuleErrors += " Rule '" + rule.mName + "': " + error;
    }
    if (mTrashUsed)
    {
        gInventory.checkTrashOverflow();
        mTrashUsed = false;
    }
    foldRuleCounters();

    ++mRuleIndex;
    beginRule();
}

void ASInventoryRuleRunner::finishRun(const std::string& status)
{
    if (mTrashUsed)
    {
        gInventory.checkTrashOverflow();
        mTrashUsed = false;
    }
    foldRuleCounters();
    mPending.clear();
    mMatches.clear();
    mCreateKey = 0;
    mState = STATE_IDLE;
    mStopRequested = false;

    mStatus = llformat("%s Done %u, skipped %u, failed %u.", status.c_str(), mTotalDone, mTotalSkipped, mTotalFailed);
    mStatus += mRuleErrors;
    LL_INFOS("InventoryRules") << mStatus << LL_ENDL;
}

void ASInventoryRuleRunner::updateStatus()
{
    if (mState == STATE_IDLE || mRuleIndex >= mRules.size())
    {
        return;
    }

    if (mState == STATE_FETCHING)
    {
        mStatus = mStopRequested ? "Loading inventory... Stopping..." : "Loading inventory...";
        return;
    }

    const ASInventoryRule& rule = mRules[mRuleIndex];
    std::string status = llformat("Rule %u/%u '%s': ", (U32)mRuleIndex + 1, (U32)mRules.size(), rule.mName.c_str());
    if (mState == STATE_RESOLVING)
    {
        status += "preparing destination folder...";
    }
    else if (mState == STATE_APPLYING)
    {
        status += llformat("%u/%u (skipped %u, failed %u)", mDone + mSkipped + mFailed, (U32)mMatches.size(), mSkipped, mFailed);
    }
    if (mStopRequested)
    {
        status += " Stopping...";
    }
    mStatus = status;
}
