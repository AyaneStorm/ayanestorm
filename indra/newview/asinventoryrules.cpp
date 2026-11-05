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
#include "llassetstorage.h"
#include "llavatarnamecache.h"
#include "lleventtimer.h"
#include "llfilesystem.h"
#include "llfloaterperms.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llgesturemgr.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llnotecard.h"
#include "llnotificationsutil.h"
#include "llregex.h"
#include "llsdjson.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "message.h"

#include <algorithm>
#include <ctime>
#include <iterator>
#include <set>
#include <sstream>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace
{
constexpr const char* SETTING_RULESETS = "ASInventoryRulesets";
constexpr const char* SETTING_LOG_KEY = "ASInventoryRulesLogKey";

// Server pacing. Operations go out BATCH_SIZE at a time; the next batch waits for every
// confirmation of the current one, then BATCH_PAUSE more seconds.
constexpr size_t BATCH_SIZE = 10;
constexpr F32 TICK_PERIOD = 0.1f;
constexpr F64 BATCH_PAUSE = 0.5;
constexpr F64 BATCH_TIMEOUT = 60.0;
constexpr F64 CREATE_TIMEOUT = 60.0;
// Creator names are re-checked every NAMES_RETRY seconds; after NAMES_TIMEOUT the rule runs without the missing ones.
constexpr F64 NAMES_RETRY = 1.0;
constexpr F64 NAMES_TIMEOUT = 30.0;

// Run logs: notecards in this root folder, split in parts of at most LOG_PART_SIZE bytes
// (below LLNotecard::MAX_SIZE).
constexpr const char* LOG_FOLDER = "logs";
constexpr size_t LOG_PART_SIZE = 60000;
constexpr const char* LOG_NAME_PREFIX = "Inventory rules ";
// Starts the undo section; its lines are "@kind seq type id from to name".
constexpr const char* UNDO_HEADER = "Undo data, read by Revert (do not edit):";
// Revert result notification: at most this many lines of what was not reverted.
constexpr size_t REVERT_REPORT_LINES = 30;
// Skip reason of an object a revert finds already reverted; not reported as a problem.
constexpr const char* REVERT_ALREADY_DONE = "already back in its original folder";

const char* const SEARCH_TYPE_NAMES[] = { "contains", "starts_with", "ends_with", "regex", "exact" };
const char* const MATCH_TYPE_NAMES[] = { "item", "folder", "containing_folder" };
const char* const ACTION_NAMES[] = { "none", "move", "delete" };
const char* const DELETE_MODE_NAMES[] = { "trash", "purge" };
const char* const ITEM_TYPE_NAMES[] = { "any", "animation", "bodypart", "callingcard", "clothing", "gesture",
                                        "landmark", "material", "notecard", "object", "script", "settings",
                                        "snapshot", "sound", "texture" };
const char* const ITEM_TYPE_LABELS[] = { "any", "animation", "body part", "calling card", "clothing", "gesture",
                                         "landmark", "material", "notecard", "object", "script", "settings",
                                         "snapshot", "sound", "texture" };
// Asset type of each EItemType; snapshots and textures are told apart by inventory type.
const LLAssetType::EType ITEM_ASSET_TYPES[] = { LLAssetType::AT_NONE, LLAssetType::AT_ANIMATION, LLAssetType::AT_BODYPART,
                                                LLAssetType::AT_CALLINGCARD, LLAssetType::AT_CLOTHING, LLAssetType::AT_GESTURE,
                                                LLAssetType::AT_LANDMARK, LLAssetType::AT_MATERIAL, LLAssetType::AT_NOTECARD,
                                                LLAssetType::AT_OBJECT, LLAssetType::AT_LSL_TEXT, LLAssetType::AT_SETTINGS,
                                                LLAssetType::AT_TEXTURE, LLAssetType::AT_SOUND, LLAssetType::AT_TEXTURE };
const char* const AGE_NAMES[] = { "any", "newer", "older" };
const char* const CREATOR_NAMES[] = { "anyone", "me", "others", "specific" };
const char* const PERMISSION_NAMES[] = { "any", "yes", "no" };

// JSON exports.
constexpr const char* EXPORT_FORMAT = "ayanestorm-inventory-rules";
const char* const EXPORT_TYPE_NAMES[] = { "rule", "ruleset", "rulesets" };
constexpr size_t IMPORT_MAX_SIZE = 4 * 1024 * 1024;

// Rule keys of the stored and exported LLSD, in export order, with what an import accepts.
enum EFieldKind { FIELD_STRING, FIELD_BOOL, FIELD_INT, FIELD_ENUM };
struct RuleField
{
    const char* mKey;
    EFieldKind mKind;
    const char* const* mNames = nullptr;    // FIELD_ENUM
    size_t mCount = 0;
    S32 mMax = 0;                           // FIELD_INT, minimum 0
};
const RuleField RULE_FIELDS[] = {
    { "name", FIELD_STRING },
    { "search", FIELD_STRING },
    { "search_type", FIELD_ENUM, SEARCH_TYPE_NAMES, std::size(SEARCH_TYPE_NAMES) },
    { "match_case", FIELD_BOOL },
    { "except", FIELD_STRING },
    { "except_type", FIELD_ENUM, SEARCH_TYPE_NAMES, std::size(SEARCH_TYPE_NAMES) },
    { "except_match_case", FIELD_BOOL },
    { "match", FIELD_ENUM, MATCH_TYPE_NAMES, std::size(MATCH_TYPE_NAMES) },
    { "item_type", FIELD_ENUM, ITEM_TYPE_NAMES, std::size(ITEM_TYPE_NAMES) },
    { "age", FIELD_ENUM, AGE_NAMES, std::size(AGE_NAMES) },
    { "age_days", FIELD_INT, nullptr, 0, 10000 },
    { "age_hours", FIELD_INT, nullptr, 0, 23 },
    { "creator", FIELD_ENUM, CREATOR_NAMES, std::size(CREATOR_NAMES) },
    { "creator_name", FIELD_STRING },
    { "perm_copy", FIELD_ENUM, PERMISSION_NAMES, std::size(PERMISSION_NAMES) },
    { "perm_modify", FIELD_ENUM, PERMISSION_NAMES, std::size(PERMISSION_NAMES) },
    { "perm_transfer", FIELD_ENUM, PERMISSION_NAMES, std::size(PERMISSION_NAMES) },
    { "action", FIELD_ENUM, ACTION_NAMES, std::size(ACTION_NAMES) },
    { "delete_mode", FIELD_ENUM, DELETE_MODE_NAMES, std::size(DELETE_MODE_NAMES) },
    { "destination", FIELD_STRING },
    { "create_destination", FIELD_BOOL },
};

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

// "John.Doe", "john doe" and "Jane Resident" compare as "john doe" and "jane".
std::string normalizedName(const std::string& name)
{
    std::string text = lowered(name);
    LLStringUtil::replaceChar(text, '.', ' ');
    LLStringUtil::trim(text);
    const std::string suffix = " resident";
    if (text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0)
    {
        text.erase(text.size() - suffix.size());
    }
    return text;
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
    sd["item_type"] = ITEM_TYPE_NAMES[mItemType];
    sd["age"] = AGE_NAMES[mAge];
    sd["age_days"] = (S32)mAgeDays;
    sd["age_hours"] = (S32)mAgeHours;
    sd["creator"] = CREATOR_NAMES[mCreator];
    sd["creator_name"] = mCreatorName;
    sd["perm_copy"] = PERMISSION_NAMES[mPermCopy];
    sd["perm_modify"] = PERMISSION_NAMES[mPermModify];
    sd["perm_transfer"] = PERMISSION_NAMES[mPermTransfer];
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
    rule.mItemType = (EItemType)lookupName(sd["item_type"].asString(), ITEM_TYPE_NAMES, ITEM_ANY);
    rule.mAge = (EAge)lookupName(sd["age"].asString(), AGE_NAMES, AGE_ANY);
    rule.mAgeDays = (U32)llmax(0, sd["age_days"].asInteger());
    rule.mAgeHours = (U32)llmax(0, sd["age_hours"].asInteger());
    rule.mCreator = (ECreator)lookupName(sd["creator"].asString(), CREATOR_NAMES, CREATOR_ANYONE);
    rule.mCreatorName = sd["creator_name"].asString();
    LLStringUtil::trim(rule.mCreatorName);
    rule.mPermCopy = (EPermission)lookupName(sd["perm_copy"].asString(), PERMISSION_NAMES, PERMISSION_ANY);
    rule.mPermModify = (EPermission)lookupName(sd["perm_modify"].asString(), PERMISSION_NAMES, PERMISSION_ANY);
    rule.mPermTransfer = (EPermission)lookupName(sd["perm_transfer"].asString(), PERMISSION_NAMES, PERMISSION_ANY);
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

    if (usesItemFilters())
    {
        if (mItemType != ITEM_ANY)
        {
            summary += ", type " + std::string(ITEM_TYPE_LABELS[mItemType]);
        }
        if (mAge != AGE_ANY)
        {
            summary += llformat(", %s than %ud %uh", mAge == AGE_NEWER ? "newer" : "older", mAgeDays, mAgeHours);
        }
        switch (mCreator)
        {
        case CREATOR_ME:
            summary += ", created by me";
            break;
        case CREATOR_OTHERS:
            summary += ", created by others";
            break;
        case CREATOR_SPECIFIC:
            summary += ", created by " + mCreatorName;
            break;
        default:
            break;
        }
        const EPermission perms[] = { mPermCopy, mPermModify, mPermTransfer };
        const char* const perm_labels[] = { "copy", "mod", "transfer" };
        for (size_t i = 0; i < 3; ++i)
        {
            if (perms[i] != PERMISSION_ANY)
            {
                summary += std::string(perms[i] == PERMISSION_NO ? ", no " : ", ") + perm_labels[i];
            }
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
// ASInventoryRuleSimulation
//-----------------------------------------------------------------------------

const ASInventoryRuleSimulation& ASInventoryRuleSimulation::real()
{
    static const ASInventoryRuleSimulation sReal;
    return sReal;
}

void ASInventoryRuleSimulation::getChildren(const LLUUID& folder_id, cat_list_t& cats, item_list_t& items) const
{
    cats.clear();
    items.clear();

    // Real children, minus those moved elsewhere.
    LLInventoryModel::cat_array_t* real_cats = nullptr;
    LLInventoryModel::item_array_t* real_items = nullptr;
    gInventory.getDirectDescendentsOf(folder_id, real_cats, real_items);
    if (real_cats)
    {
        for (const LLPointer<LLViewerInventoryCategory>& cat : *real_cats)
        {
            if (cat && !mParents.count(cat->getUUID()))
            {
                cats.push_back(cat.get());
            }
        }
    }
    if (real_items)
    {
        for (const LLPointer<LLViewerInventoryItem>& item : *real_items)
        {
            if (item && !mParents.count(item->getUUID()))
            {
                items.push_back(item.get());
            }
        }
    }

    // Objects moved in, and folders created here.
    auto added = mAdded.find(folder_id);
    if (added != mAdded.end())
    {
        for (const LLUUID& id : added->second)
        {
            if (LLViewerInventoryCategory* cat = getCategory(id))
            {
                cats.push_back(cat);
            }
            else if (LLViewerInventoryItem* item = gInventory.getItem(id))
            {
                items.push_back(item);
            }
        }
    }
}

LLViewerInventoryCategory* ASInventoryRuleSimulation::getCategory(const LLUUID& id) const
{
    auto it = mVirtual.find(id);
    return it != mVirtual.end() ? it->second.get() : gInventory.getCategory(id);
}

bool ASInventoryRuleSimulation::exists(const LLUUID& id) const
{
    return isVirtual(id) || gInventory.getObject(id) != nullptr;
}

LLUUID ASInventoryRuleSimulation::getParent(const LLUUID& id) const
{
    auto it = mParents.find(id);
    if (it != mParents.end())
    {
        return it->second;
    }
    const LLInventoryObject* obj = gInventory.getObject(id);
    return obj ? obj->getParentUUID() : LLUUID::null;
}

std::string ASInventoryRuleSimulation::getName(const LLUUID& id) const
{
    if (const LLViewerInventoryCategory* cat = getCategory(id))
    {
        return cat->getName();
    }
    const LLInventoryObject* obj = gInventory.getObject(id);
    return obj ? obj->getName() : std::string();
}

bool ASInventoryRuleSimulation::isDescendentOf(const LLUUID& id, const LLUUID& ancestor_id) const
{
    for (LLUUID current = getParent(id); current.notNull(); current = getParent(current))
    {
        if (current == ancestor_id)
        {
            return true;
        }
    }
    return false;
}

LLUUID ASInventoryRuleSimulation::findChildFolder(const LLUUID& parent_id, const std::string& name) const
{
    cat_list_t cats;
    item_list_t items;
    getChildren(parent_id, cats, items);
    const std::string wanted = lowered(name);
    for (const LLViewerInventoryCategory* cat : cats)
    {
        if (lowered(cat->getName()) == wanted)
        {
            return cat->getUUID();
        }
    }
    return LLUUID::null;
}

LLUUID ASInventoryRuleSimulation::findFolderByPath(const std::string& path) const
{
    const std::vector<std::string> segments = ASInventoryRules::splitPath(path);
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

std::string ASInventoryRuleSimulation::getPath(const LLUUID& id) const
{
    const LLUUID& root_id = gInventory.getRootFolderID();
    std::vector<std::string> names;
    for (LLUUID current = id; current.notNull() && current != root_id; current = getParent(current))
    {
        if (!exists(current))
        {
            break;
        }
        names.push_back(getName(current));
    }

    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
    {
        path += "/" + *it;
    }
    return path.empty() ? std::string("/") : path;
}

void ASInventoryRuleSimulation::move(const LLUUID& id, const LLUUID& new_parent_id)
{
    auto added = mAdded.find(getParent(id));
    if (added != mAdded.end())
    {
        added->second.erase(std::remove(added->second.begin(), added->second.end(), id), added->second.end());
    }

    // Back in its real folder: the real children list has it again.
    const LLInventoryObject* obj = gInventory.getObject(id);
    if (obj && obj->getParentUUID() == new_parent_id)
    {
        mParents.erase(id);
        return;
    }
    mParents[id] = new_parent_id;
    if (new_parent_id.notNull())
    {
        mAdded[new_parent_id].push_back(id);
    }
}

LLUUID ASInventoryRuleSimulation::createFolder(const LLUUID& parent_id, const std::string& name)
{
    LLUUID id;
    id.generate();
    mVirtual[id] = new LLViewerInventoryCategory(id, parent_id, LLFolderType::FT_NONE, name, gAgent.getID());
    mParents[id] = parent_id;
    mAdded[parent_id].push_back(id);
    return id;
}

//-----------------------------------------------------------------------------
// Storage and path helpers
//-----------------------------------------------------------------------------

namespace
{
std::string sStorageError;

// Upgrades data of an older format version in place, one version at a time. data is the list of
// rulesets, one ruleset or one rule (type). Version 1 is the first format: each version bump adds
// a step here converting from the previous version (if (version == 1) { ... }).
void migrate(LLSD& /*data*/, ASInventoryRules::EExportType /*type*/, S32 version)
{
    for (; version < ASInventoryRules::FORMAT_VERSION; ++version)
    {
    }
}
}

ASInventoryRules::ruleset_list_t ASInventoryRules::load()
{
    sStorageError.clear();
    ruleset_list_t rulesets;

    // Version 1 stored the bare array, later versions {version, rulesets}.
    const LLSD stored = gSavedPerAccountSettings.getLLSD(SETTING_RULESETS);
    S32 version = 1;
    LLSD entries = stored;
    if (stored.isMap())
    {
        version = llmax(1, stored["version"].asInteger());
        entries = stored["rulesets"];
    }
    if (version > FORMAT_VERSION)
    {
        sStorageError = llformat("Your inventory rules were saved by a newer viewer (format version %d, this viewer reads up to %d). "
                                 "They are kept unchanged, but cannot be shown or changed here.", version, FORMAT_VERSION);
        return rulesets;
    }
    migrate(entries, EXPORT_RULESETS, version);

    if (entries.isArray())
    {
        for (LLSD::array_const_iterator it = entries.beginArray(); it != entries.endArray(); ++it)
        {
            rulesets.push_back(ASInventoryRuleset::fromLLSD(*it));
        }
    }
    return rulesets;
}

bool ASInventoryRules::save(const ruleset_list_t& rulesets)
{
    // Never overwrite what a newer viewer saved.
    if (!sStorageError.empty())
    {
        LL_WARNS("InventoryRules") << "Not saving: " << sStorageError << LL_ENDL;
        return false;
    }

    LLSD entries = LLSD::emptyArray();
    for (const ASInventoryRuleset& ruleset : rulesets)
    {
        entries.append(ruleset.asLLSD());
    }
    LLSD stored;
    stored["version"] = FORMAT_VERSION;
    stored["rulesets"] = entries;
    gSavedPerAccountSettings.setLLSD(SETTING_RULESETS, stored);
    return true;
}

const std::string& ASInventoryRules::getStorageError()
{
    return sStorageError;
}

//-----------------------------------------------------------------------------
// JSON import/export
//-----------------------------------------------------------------------------

namespace
{
// Exported keys come in this order, other keys alphabetically after them.
size_t keyRank(const std::string& key)
{
    static const std::vector<std::string> order = []()
        {
            std::vector<std::string> keys = { "format", "version", "type", "data" };
            for (const RuleField& field : RULE_FIELDS)
            {
                keys.push_back(field.mKey);
            }
            keys.push_back("rules");
            return keys;
        }();
    return (size_t)(std::find(order.begin(), order.end(), key) - order.begin());
}

// Indented JSON, easier to read and edit by hand than boost::json::serialize.
void writeJson(std::string& out, const LLSD& sd, const std::string& indent)
{
    const std::string inner = indent + "  ";
    if (sd.isMap())
    {
        std::vector<std::string> keys;
        for (LLSD::map_const_iterator it = sd.beginMap(); it != sd.endMap(); ++it)
        {
            keys.push_back(it->first);
        }
        if (keys.empty())
        {
            out += "{}";
            return;
        }
        // Map keys come sorted, so equal ranks stay alphabetical.
        std::stable_sort(keys.begin(), keys.end(),
                         [](const std::string& a, const std::string& b) { return keyRank(a) < keyRank(b); });
        out += "{\n";
        for (size_t i = 0; i < keys.size(); ++i)
        {
            out += inner + boost::json::serialize(boost::json::value(boost::json::string_view(keys[i]))) + ": ";
            writeJson(out, sd[keys[i]], inner);
            out += (i + 1 < keys.size()) ? ",\n" : "\n";
        }
        out += indent + "}";
    }
    else if (sd.isArray())
    {
        if (sd.size() == 0)
        {
            out += "[]";
            return;
        }
        out += "[\n";
        for (size_t i = 0; i < sd.size(); ++i)
        {
            out += inner;
            writeJson(out, sd[i], inner);
            out += (i + 1 < sd.size()) ? ",\n" : "\n";
        }
        out += indent + "]";
    }
    else
    {
        out += boost::json::serialize(LlsdToJson(sd));
    }
}

std::string writeExport(ASInventoryRules::EExportType type, const LLSD& data)
{
    LLSD sd;
    sd["format"] = EXPORT_FORMAT;
    sd["version"] = ASInventoryRules::FORMAT_VERSION;
    sd["type"] = EXPORT_TYPE_NAMES[type];
    sd["data"] = data;
    std::string out;
    writeJson(out, sd, std::string());
    return out + "\n";
}

LLSD ruleExportLLSD(const ASInventoryRule& rule)
{
    LLSD sd = rule.asLLSD();
    sd.erase("id");
    return sd;
}

LLSD rulesetExportLLSD(const ASInventoryRuleset& ruleset)
{
    LLSD sd;
    sd["name"] = ruleset.mName;
    sd["rules"] = LLSD::emptyArray();
    for (const ASInventoryRule& rule : ruleset.mRules)
    {
        sd["rules"].append(ruleExportLLSD(rule));
    }
    return sd;
}

std::string quotedText(const std::string& text)
{
    return "'" + text + "'";
}

// Errors name the rule as label, its name, context: "Rule 2 ('Hats') of ruleset 1 ('Demos')".
// Every field is checked; missing fields take their default.
bool importRule(const LLSD& sd, const std::string& label, const std::string& context, ASInventoryRule& rule, std::string& error)
{
    std::string where = label;
    if (sd.isMap() && sd["name"].isString() && !sd["name"].asString().empty())
    {
        where += " (" + quotedText(sd["name"].asString()) + ")";
    }
    where += context;
    if (!sd.isMap())
    {
        error = where + " is not a rule (expected { ... }).";
        return false;
    }

    for (LLSD::map_const_iterator it = sd.beginMap(); it != sd.endMap(); ++it)
    {
        // Rules copied from the stored setting carry an id; a new one is generated anyway.
        if (it->first == "id")
        {
            continue;
        }
        const RuleField* field = nullptr;
        for (const RuleField& candidate : RULE_FIELDS)
        {
            if (it->first == candidate.mKey)
            {
                field = &candidate;
                break;
            }
        }
        const std::string prefix = where + ", field " + quotedText(it->first) + ": ";
        if (!field)
        {
            error = prefix + "unknown field.";
            return false;
        }

        const LLSD& value = it->second;
        switch (field->mKind)
        {
        case FIELD_STRING:
            if (!value.isString())
            {
                error = prefix + "must be text in double quotes.";
                return false;
            }
            break;
        case FIELD_BOOL:
            if (!value.isBoolean())
            {
                error = prefix + "must be true or false.";
                return false;
            }
            break;
        case FIELD_INT:
            if (!value.isInteger() || value.asInteger() < 0 || value.asInteger() > field->mMax)
            {
                error = prefix + llformat("must be a whole number from 0 to %d.", field->mMax);
                return false;
            }
            break;
        case FIELD_ENUM:
        {
            bool known = false;
            std::string names;
            for (size_t i = 0; i < field->mCount; ++i)
            {
                known = known || (value.isString() && value.asString() == field->mNames[i]);
                names += (i ? ", " : "") + std::string(field->mNames[i]);
            }
            if (!known)
            {
                error = prefix + "must be one of " + names + ".";
                return false;
            }
            break;
        }
        }
    }

    rule = ASInventoryRule::fromLLSD(sd);
    rule.mID.generate();
    LLStringUtil::trim(rule.mName);
    if (rule.mName.empty())
    {
        rule.mName = rule.mSearch.empty() ? "Imported rule" : rule.mSearch;
    }

    if (rule.mAction == ASInventoryRule::ACTION_MOVE && ASInventoryRules::splitPath(rule.mDestination).empty())
    {
        error = where + ", field 'destination': a folder path is required when action is move.";
        return false;
    }
    // Same check as a run, so a bad regex is reported now rather than when executing.
    const std::pair<const std::string*, ASInventoryRule::ESearchType> texts[] = {
        { &rule.mSearch, rule.mSearchType }, { &rule.mExcept, rule.mExceptType } };
    for (size_t i = 0; i < 2; ++i)
    {
        if (texts[i].second == ASInventoryRule::SEARCH_REGEX && !texts[i].first->empty())
        {
            ASInventoryRuleMatcher matcher;
            std::string regex_error;
            if (!matcher.init(*texts[i].first, ASInventoryRule::SEARCH_REGEX, true, regex_error))
            {
                error = where + ", field " + quotedText(i ? "except" : "search") + ": " + regex_error;
                return false;
            }
        }
    }
    return true;
}

bool importRuleset(const LLSD& sd, std::string where, ASInventoryRuleset& ruleset, std::string& error)
{
    if (!sd.isMap())
    {
        error = where + " is not a ruleset (expected { \"name\": ..., \"rules\": [ ... ] }).";
        return false;
    }
    for (LLSD::map_const_iterator it = sd.beginMap(); it != sd.endMap(); ++it)
    {
        if (it->first != "name" && it->first != "rules" && it->first != "id")
        {
            error = where + ", field " + quotedText(it->first) + ": unknown field.";
            return false;
        }
    }
    if (sd.has("name") && !sd["name"].isString())
    {
        error = where + ", field 'name': must be text in double quotes.";
        return false;
    }
    if (sd.has("rules") && !sd["rules"].isArray())
    {
        error = where + ", field 'rules': must be a list of rules ([ ... ]).";
        return false;
    }

    ruleset.mID.generate();
    ruleset.mName = sd["name"].asString();
    LLStringUtil::trim(ruleset.mName);
    if (ruleset.mName.empty())
    {
        ruleset.mName = "Imported ruleset";
    }
    else
    {
        where += " (" + quotedText(ruleset.mName) + ")";
    }

    const LLSD& rules = sd["rules"];
    for (size_t i = 0; i < rules.size(); ++i)
    {
        ASInventoryRule rule;
        if (!importRule(rules[i], llformat("Rule %u", (U32)i + 1), " of " + lowered(where.substr(0, 1)) + where.substr(1), rule, error))
        {
            return false;
        }
        ruleset.mRules.push_back(rule);
    }
    return true;
}
}

std::string ASInventoryRules::exportJson(const ASInventoryRule& rule)
{
    return writeExport(EXPORT_RULE, ruleExportLLSD(rule));
}

std::string ASInventoryRules::exportJson(const ASInventoryRuleset& ruleset)
{
    return writeExport(EXPORT_RULESET, rulesetExportLLSD(ruleset));
}

std::string ASInventoryRules::exportJson(const ruleset_list_t& rulesets)
{
    LLSD data = LLSD::emptyArray();
    for (const ASInventoryRuleset& ruleset : rulesets)
    {
        data.append(rulesetExportLLSD(ruleset));
    }
    return writeExport(EXPORT_RULESETS, data);
}

bool ASInventoryRules::importJson(const std::string& input, EExportType& type, ruleset_list_t& rulesets, std::string& error)
{
    rulesets.clear();
    if (input.size() > IMPORT_MAX_SIZE)
    {
        error = "The text is too large to be inventory rules.";
        return false;
    }
    std::string text = input;
    // Some editors start files with a byte order mark.
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
    {
        text.erase(0, 3);
    }
    LLStringUtil::trim(text);
    if (text.empty())
    {
        error = "Nothing to import: the text is empty.";
        return false;
    }

    boost::system::error_code ec;
    const boost::json::value json = boost::json::parse(text, ec);
    if (ec)
    {
        error = "This is not valid JSON: " + ec.message() + ".";
        return false;
    }
    const LLSD sd = LlsdFromJson(json);
    if (!sd.isMap() || sd["format"].asString() != EXPORT_FORMAT)
    {
        error = "This is not an AyaneStorm inventory rules export (its 'format' must be '" + std::string(EXPORT_FORMAT) + "').";
        return false;
    }
    for (LLSD::map_const_iterator it = sd.beginMap(); it != sd.endMap(); ++it)
    {
        if (it->first != "format" && it->first != "version" && it->first != "type" && it->first != "data")
        {
            error = "Unknown field " + quotedText(it->first) + " (expected format, version, type and data).";
            return false;
        }
    }

    const LLSD& version_sd = sd["version"];
    if (!version_sd.isInteger() || version_sd.asInteger() < 1)
    {
        error = "The field 'version' is missing or is not a whole number.";
        return false;
    }
    const S32 version = version_sd.asInteger();
    if (version > FORMAT_VERSION)
    {
        error = llformat("These rules come from a newer viewer (format version %d). This viewer reads up to version %d: "
                         "update it to import them.", version, FORMAT_VERSION);
        return false;
    }

    const S32 type_index = lookupName(sd["type"].asString(), EXPORT_TYPE_NAMES, -1);
    if (type_index < 0)
    {
        error = "The field 'type' must be rule, ruleset or rulesets.";
        return false;
    }
    type = (EExportType)type_index;

    LLSD data = sd["data"];
    migrate(data, type, version);

    switch (type)
    {
    case EXPORT_RULE:
    {
        ASInventoryRuleset holder;
        holder.mID.generate();
        ASInventoryRule rule;
        if (!importRule(data, "The rule", std::string(), rule, error))
        {
            return false;
        }
        holder.mRules.push_back(rule);
        rulesets.push_back(holder);
        break;
    }
    case EXPORT_RULESET:
    {
        ASInventoryRuleset ruleset;
        if (!importRuleset(data, "The ruleset", ruleset, error))
        {
            return false;
        }
        rulesets.push_back(ruleset);
        break;
    }
    case EXPORT_RULESETS:
        if (!data.isArray())
        {
            error = "The field 'data' must be a list of rulesets ([ ... ]).";
            return false;
        }
        for (size_t i = 0; i < data.size(); ++i)
        {
            ASInventoryRuleset ruleset;
            if (!importRuleset(data[i], llformat("Ruleset %u", (U32)i + 1), ruleset, error))
            {
                rulesets.clear();
                return false;
            }
            rulesets.push_back(ruleset);
        }
        break;
    }
    return true;
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
    return ASInventoryRuleSimulation::real().findFolderByPath(path);
}

std::string ASInventoryRules::getPath(const LLUUID& id)
{
    return ASInventoryRuleSimulation::real().getPath(id);
}

std::string ASInventoryRules::getSkipReason(const ASInventoryRule& rule, const LLUUID& id, const LLUUID& dest_id,
                                            const ASInventoryRuleSimulation& sim)
{
    if (!sim.exists(id))
    {
        return "No longer exists";
    }
    // A folder a dry run would create is new, so nothing protects it.
    const bool is_virtual = sim.isVirtual(id);
    const bool is_folder = sim.getCategory(id) != nullptr;

    if (rule.mAction == ASInventoryRule::ACTION_MOVE)
    {
        if (dest_id.notNull() && sim.getParent(id) == dest_id)
        {
            return "Already in destination";
        }
        if (is_folder)
        {
            if (dest_id.notNull() && (dest_id == id || sim.isDescendentOf(dest_id, id)))
            {
                return "Contains the destination";
            }
            if (!is_virtual && !get_is_category_removable(&gInventory, id))
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
            if (!is_virtual && !get_is_category_and_children_removable(&gInventory, id, true))
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

bool matchesPermission(ASInventoryRule::EPermission filter, bool allowed)
{
    return filter == ASInventoryRule::PERMISSION_ANY || allowed == (filter == ASInventoryRule::PERMISSION_YES);
}

// Type, age, permission and creator conditions on items.
class ASInventoryRuleItemFilter
{
public:
    explicit ASInventoryRuleItemFilter(const ASInventoryRule& rule) : mRule(rule) {}

    bool init(std::string& error)
    {
        const time_t age = ((time_t)mRule.mAgeDays * 24 + mRule.mAgeHours) * 3600;
        mAgeLimit = time_corrected() - age;
        if (mRule.mCreator == ASInventoryRule::CREATOR_SPECIFIC)
        {
            if (mRule.mCreatorName.empty())
            {
                error = "Enter the creator's user name or UUID.";
                return false;
            }
            if (LLUUID::validate(mRule.mCreatorName))
            {
                mCreatorID.set(mRule.mCreatorName);
            }
            else
            {
                mCreatorName = normalizedName(mRule.mCreatorName);
            }
        }
        return true;
    }

    bool matches(const LLViewerInventoryItem* item)
    {
        if (mRule.mItemType == ASInventoryRule::ITEM_SNAPSHOT || mRule.mItemType == ASInventoryRule::ITEM_TEXTURE)
        {
            const bool is_snapshot = item->getInventoryType() == LLInventoryType::IT_SNAPSHOT;
            if (item->getType() != LLAssetType::AT_TEXTURE || is_snapshot != (mRule.mItemType == ASInventoryRule::ITEM_SNAPSHOT))
            {
                return false;
            }
        }
        else if (mRule.mItemType != ASInventoryRule::ITEM_ANY && item->getType() != ITEM_ASSET_TYPES[mRule.mItemType])
        {
            return false;
        }

        if (mRule.mAge != ASInventoryRule::AGE_ANY)
        {
            // An unknown creation date never matches an age condition.
            const time_t created = item->getCreationDate();
            if (created <= 0 || (mRule.mAge == ASInventoryRule::AGE_NEWER) != (created >= mAgeLimit))
            {
                return false;
            }
        }

        const PermissionMask mask = item->getPermissions().getMaskOwner();
        if (!matchesPermission(mRule.mPermCopy, (mask & PERM_COPY) != 0)
            || !matchesPermission(mRule.mPermModify, (mask & PERM_MODIFY) != 0)
            || !matchesPermission(mRule.mPermTransfer, (mask & PERM_TRANSFER) != 0))
        {
            return false;
        }

        // Last: it may request names.
        return matchesCreator(item->getCreatorUUID());
    }

    U32 getPendingNames() const { return mPendingNames; }

private:
    bool matchesCreator(const LLUUID& creator_id)
    {
        switch (mRule.mCreator)
        {
        case ASInventoryRule::CREATOR_ME:
            return creator_id == gAgent.getID();
        case ASInventoryRule::CREATOR_OTHERS:
            return creator_id != gAgent.getID();
        case ASInventoryRule::CREATOR_SPECIFIC:
            break;
        default:
            return true;
        }
        // An item without a creator never matches a specific person.
        if (creator_id.isNull())
        {
            return false;
        }
        if (mCreatorID.notNull())
        {
            return creator_id == mCreatorID;
        }

        // One name lookup per creator. A name not cached yet is requested and does not match.
        auto it = mCreatorMatches.find(creator_id);
        if (it == mCreatorMatches.end())
        {
            LLAvatarName av_name;
            bool match = false;
            if (LLAvatarNameCache::get(creator_id, &av_name))
            {
                match = normalizedName(av_name.getLegacyName()) == mCreatorName
                        || normalizedName(av_name.getAccountName()) == mCreatorName;
            }
            else
            {
                ++mPendingNames;
            }
            it = mCreatorMatches.emplace(creator_id, match).first;
        }
        return it->second;
    }

    const ASInventoryRule& mRule;
    time_t mAgeLimit = 0;           // newer: created at or after this time
    LLUUID mCreatorID;
    std::string mCreatorName;       // normalized
    std::map<LLUUID, bool> mCreatorMatches;
    U32 mPendingNames = 0;
};
}

bool ASInventoryRuleScanner::scan(const ASInventoryRule& rule, const LLUUID& dest_id, uuid_vec_t& matches, std::string& error,
                                  const ASInventoryRuleSimulation& sim, U32* pending_names, const LLUUID& skip_id)
{
    matches.clear();
    if (pending_names)
    {
        *pending_names = 0;
    }

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

    ASInventoryRuleItemFilter filter(rule);
    if (rule.usesItemFilters() && !filter.init(error))
    {
        return false;
    }
    // Name and item filters together.
    auto item_matches = [&](const LLViewerInventoryItem* item, const std::string& path)
    {
        return anyMatches(search_matchers, item->getName(), path)
            && !anyMatches(except_matchers, item->getName(), path)
            && filter.matches(item);
    };

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
    skipped.insert(skip_id);
    skipped.erase(LLUUID::null);

    // Depth-first walk carrying each folder's path ("" for the root, "/Objects/Box" below it);
    // children are pushed in reverse so results come out top-down.
    typedef std::pair<LLUUID, std::string> folder_entry_t;
    std::vector<folder_entry_t> stack(1, folder_entry_t(root_id, std::string()));
    ASInventoryRuleSimulation::cat_list_t cats;
    ASInventoryRuleSimulation::item_list_t items;
    while (!stack.empty())
    {
        const folder_entry_t folder = stack.back();
        stack.pop_back();
        sim.getChildren(folder.first, cats, items);

        if (rule.mMatchType == ASInventoryRule::MATCH_CONTAINING_FOLDER && folder.first != root_id)
        {
            const LLViewerInventoryCategory* folder_cat = sim.getCategory(folder.first);
            if (folder_cat && !LLFolderType::lookupIsProtectedType(folder_cat->getPreferredType()))
            {
                bool has_match = false;
                for (const LLViewerInventoryItem* item : items)
                {
                    if (!item->getIsLinkType() && item_matches(item, folder.second + "/" + item->getName()))
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

        if (rule.mMatchType == ASInventoryRule::MATCH_ITEM)
        {
            for (const LLViewerInventoryItem* item : items)
            {
                if (!item->getIsLinkType() && item_matches(item, folder.second + "/" + item->getName()))
                {
                    matches.push_back(item->getUUID());
                }
            }
        }

        std::vector<folder_entry_t> children;
        for (const LLViewerInventoryCategory* cat : cats)
        {
            if (skipped.count(cat->getUUID()))
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

    if (pending_names)
    {
        *pending_names = filter.getPendingNames();
    }
    return true;
}

//-----------------------------------------------------------------------------
// ASInventoryRuleDryRun
//-----------------------------------------------------------------------------

void ASInventoryRuleDryRun::run(const ASInventoryRules::rule_list_t& rules)
{
    mRules.clear();
    mPendingNames = 0;
    ASInventoryRuleSimulation sim;
    const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);

    for (const ASInventoryRule& rule : rules)
    {
        if (rule.mAction == ASInventoryRule::ACTION_NONE)
        {
            continue;
        }
        mRules.emplace_back();
        Rule& result = mRules.back();
        result.mName = rule.mName;

        // Destination, resolved and created like ASInventoryRuleRunner::continueResolve.
        LLUUID dest_id;
        if (rule.mAction == ASInventoryRule::ACTION_MOVE)
        {
            result.mAction = "Move to " + rule.mDestination;
            const std::vector<std::string> segments = ASInventoryRules::splitPath(rule.mDestination);
            if (segments.empty())
            {
                result.mNote = "No destination folder.";
                continue;
            }
            dest_id = gInventory.getRootFolderID();
            for (const std::string& segment : segments)
            {
                LLUUID child_id = sim.findChildFolder(dest_id, segment);
                if (child_id.isNull() && rule.mCreateDestination)
                {
                    child_id = sim.createFolder(dest_id, segment);
                    result.mNote = "Destination folder would be created.";
                }
                dest_id = child_id;
                if (dest_id.isNull())
                {
                    break;
                }
            }
            if (dest_id.isNull())
            {
                result.mNote = "Destination folder not found: " + rule.mDestination;
                continue;
            }
        }
        else
        {
            result.mAction = rule.isPurge() ? "Delete permanently" : "Move to Trash";
        }

        uuid_vec_t matches;
        std::string error;
        U32 pending_names = 0;
        if (!ASInventoryRuleScanner::scan(rule, dest_id, matches, error, sim, &pending_names))
        {
            result.mNote = error;
            continue;
        }
        mPendingNames += pending_names;

        // Applied one by one, as the runner re-checks each object right before acting.
        const LLUUID new_parent = (rule.mAction == ASInventoryRule::ACTION_MOVE) ? dest_id
                                  : rule.isPurge() ? LLUUID::null : trash_id;
        for (const LLUUID& id : matches)
        {
            Entry entry;
            entry.mID = id;
            entry.mName = sim.getName(id);
            entry.mFolder = sim.getPath(sim.getParent(id));
            entry.mSkipReason = ASInventoryRules::getSkipReason(rule, id, dest_id, sim);
            if (entry.mSkipReason.empty())
            {
                ++result.mApplied;
                sim.move(id, new_parent);
            }
            else
            {
                ++result.mSkipped;
            }
            result.mEntries.push_back(entry);
        }
    }
}

//-----------------------------------------------------------------------------
// Run log notecards
//-----------------------------------------------------------------------------

namespace
{
std::string localTimeText(time_t time)
{
    char buffer[32] = "";
    if (const std::tm* local = std::localtime(&time))
    {
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", local);
    }
    return buffer;
}

// "folder", or the item type as named in the rule editor ("texture", "body part"...).
std::string objectTypeLabel(const LLUUID& id)
{
    if (gInventory.getCategory(id))
    {
        return "folder";
    }
    const LLViewerInventoryItem* item = gInventory.getItem(id);
    if (!item)
    {
        return "unknown";
    }
    if (item->getInventoryType() == LLInventoryType::IT_SNAPSHOT)
    {
        return ITEM_TYPE_LABELS[ASInventoryRule::ITEM_SNAPSHOT];
    }
    for (size_t i = ASInventoryRule::ITEM_ANY + 1; i < std::size(ITEM_ASSET_TYPES); ++i)
    {
        if (i != (size_t)ASInventoryRule::ITEM_SNAPSHOT && item->getType() == ITEM_ASSET_TYPES[i])
        {
            return ITEM_TYPE_LABELS[i];
        }
    }
    return LLAssetType::lookupHumanReadable(item->getType());
}

// Undo lines are space-separated, so the type label travels with '_' for spaces.
std::string typeToken(const std::string& label)
{
    std::string token = label;
    LLStringUtil::replaceChar(token, ' ', '_');
    return token;
}

// Log notecards of a run, by part number (1 when not split), in /logs and its subfolders.
// total is the part count their names give. Only notecards this account created are taken, so a
// notecard given by someone else cannot drive a revert, even if it is put in /logs.
typedef std::map<U32, LLViewerInventoryItem*> log_parts_t;
void findLogParts(const std::string& run_id, log_parts_t& parts, U32& total)
{
    parts.clear();
    total = 0;
    const LLUUID folder_id = ASInventoryRuleSimulation::real().findChildFolder(gInventory.getRootFolderID(), LOG_FOLDER);
    if (folder_id.isNull())
    {
        return;
    }
    static const boost::regex name_regex("^Inventory rules .* #([0-9A-F]{6})(?: \\((\\d{1,4}) of (\\d{1,4})\\))?$");
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    gInventory.collectDescendents(folder_id, cats, items, LLInventoryModel::EXCLUDE_TRASH);
    for (const LLPointer<LLViewerInventoryItem>& item : items)
    {
        boost::smatch match;
        if (!item || item->getIsLinkType() || item->getType() != LLAssetType::AT_NOTECARD
            || item->getCreatorUUID() != gAgent.getID()
            || !ll_regex_match(item->getName(), match, name_regex) || match[1] != run_id)
        {
            continue;
        }
        const U32 index = match[2].matched ? (U32)std::stoul(match[2].str()) : 1;
        total = llmax(total, match[3].matched ? (U32)std::stoul(match[3].str()) : 1);
        // Of copies of a part, the first one found is used.
        parts.emplace(index, item.get());
    }
}

std::string hexText(const U8* bytes, size_t count)
{
    std::string text;
    for (size_t i = 0; i < count; ++i)
    {
        text += llformat("%02x", bytes[i]);
    }
    return text;
}

// This account's secret on this computer, created on first use and saved at once, so a crash
// cannot lose the key of logs already written. Empty when no random bytes could be had.
std::string logKey()
{
    std::string key = gSavedPerAccountSettings.getString(SETTING_LOG_KEY);
    if (key.empty())
    {
        U8 bytes[32];
        if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        {
            LL_WARNS("InventoryRules") << "No random bytes for the log key" << LL_ENDL;
            return std::string();
        }
        key = hexText(bytes, sizeof(bytes));
        gSavedPerAccountSettings.setString(SETTING_LOG_KEY, key);
        gSavedPerAccountSettings.saveToFile(gSavedSettings.getString("PerAccountSettingsFile"), true);
    }
    return key;
}

// HMAC-SHA256 of the undo section ("@run" line and operation lines, as written) with the account's key,
// and the account id. Only this account on this computer can produce it, so a notecard made by anyone
// or anything else (a scripted product, a copy from another avatar) never passes. Empty without a key.
std::string undoSignature(const std::string& undo_text)
{
    const std::string key = logKey();
    if (key.empty())
    {
        return std::string();
    }
    const std::string data = gAgent.getID().asString() + "\n" + undo_text;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.data(), (int)key.size(), reinterpret_cast<const unsigned char*>(data.data()), data.size(),
              digest, &length))
    {
        return std::string();
    }
    return hexText(digest, length);
}

// 6 hex digits no log notecard uses yet.
std::string newRunID()
{
    while (true)
    {
        LLUUID id;
        id.generate();
        std::string run_id = id.asString().substr(0, 6);
        LLStringUtil::toUpper(run_id);
        log_parts_t parts;
        U32 total = 0;
        findLogParts(run_id, parts, total);
        if (parts.empty())
        {
            return run_id;
        }
    }
}

// Uploads text as the notecard item's content.
void uploadLogNotecard(const LLUUID& item_id, const std::string& text)
{
    const std::string url = gAgent.getRegionCapability("UpdateNotecardAgentInventory");
    if (item_id.isNull() || url.empty())
    {
        LL_WARNS("InventoryRules") << "Could not save the run log notecard" << LL_ENDL;
        return;
    }
    LLNotecard notecard;
    notecard.setText(text);
    std::ostringstream stream;
    notecard.exportStream(stream);
    LLViewerAssetUpload::EnqueueInventoryUpload(url,
        std::make_shared<LLBufferedAssetUploadInfo>(item_id, LLAssetType::AT_NOTECARD, stream.str(), nullptr, nullptr));
}

// One notecard per part, cut at line ends; parts are named "name (2 of 3)".
void createLogNotecards(const LLUUID& folder_id, const std::string& name, const std::string& desc, const std::string& text)
{
    std::vector<std::string> parts(1);
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        end = (end == std::string::npos) ? text.size() : end + 1;
        const std::string line = text.substr(start, llmin(end - start, LOG_PART_SIZE));
        if (!parts.back().empty() && parts.back().size() + line.size() > LOG_PART_SIZE)
        {
            parts.emplace_back();
        }
        parts.back() += line;
        start = end;
    }

    const U32 perms = LLFloaterPerms::getNextOwnerPerms("Notecards");
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const std::string part_name = parts.size() > 1 ? name + llformat(" (%u of %u)", (U32)i + 1, (U32)parts.size()) : name;
        const std::string part = parts[i];
        create_inventory_item(gAgent.getID(), gAgent.getSessionID(), folder_id, LLTransactionID::tnull, part_name,
                              desc, LLAssetType::AT_NOTECARD, LLInventoryType::IT_NOTECARD,
                              NO_INV_SUBTYPE, perms,
                              new LLBoostFuncInventoryCallback([part](const LLUUID& item_id)
                                  {
                                      // With ShowNewInventory, LLOpenTaskOffer opened the new, still empty notecard
                                      // just before this callback: close it before it is drawn.
                                      if (LLFloater* preview = LLFloaterReg::findInstance("preview_notecard", LLSD(item_id)))
                                      {
                                          preview->closeFloater();
                                      }
                                      uploadLogNotecard(item_id, part);
                                  }));
    }
}

// Saves text to notecards in /logs, creating the folder when missing.
void writeLogNotecards(const std::string& name, const std::string& desc, const std::string& text)
{
    const LLUUID& root_id = gInventory.getRootFolderID();
    if (root_id.isNull())
    {
        return;
    }
    const LLUUID folder_id = ASInventoryRuleSimulation::real().findChildFolder(root_id, LOG_FOLDER);
    if (folder_id.notNull())
    {
        createLogNotecards(folder_id, name, desc, text);
        return;
    }
    gInventory.createNewCategory(root_id, LLFolderType::FT_NONE, LOG_FOLDER,
        [name, desc, text](const LLUUID& new_id)
        {
            if (new_id.isNull())
            {
                LL_WARNS("InventoryRules") << "Could not create the /" << LOG_FOLDER << " folder for the run log" << LL_ENDL;
                return;
            }
            createLogNotecards(new_id, name, desc, text);
        });
}

void showRevertMessage(const std::string& message)
{
    LLSD args;
    args["MESSAGE"] = message;
    LLNotificationsUtil::add("ASInventoryRuleRevertResult", args);
}

// Reads the undo section lines of the parts, in part order. Returns false with error when a line is
// damaged, the log has no undo section, or its signature is not this account's (see undoSignature).
bool parseUndo(const std::vector<std::string>& texts, const std::string& run_id,
               std::vector<ASInventoryRuleUndoOp>& ops, std::string& error)
{
    static const std::map<std::string, ASInventoryRuleUndoOp::EKind> kinds = {
        { "@move", ASInventoryRuleUndoOp::UNDO_MOVE }, { "@trash", ASInventoryRuleUndoOp::UNDO_TRASH },
        { "@purge", ASInventoryRuleUndoOp::UNDO_PURGE }, { "@folder", ASInventoryRuleUndoOp::UNDO_FOLDER } };

    bool has_undo = false;
    std::string undo_text;          // signed lines, as written
    std::string signature;
    for (size_t part = 0; part < texts.size(); ++part)
    {
        std::istringstream lines(texts[part]);
        std::string line;
        for (U32 line_number = 1; std::getline(lines, line); ++line_number)
        {
            if (line.empty() || line[0] != '@')
            {
                continue;
            }
            std::istringstream fields(line);
            std::string kind;
            fields >> kind;
            if (kind == "@sig")
            {
                fields >> signature;
                continue;
            }
            undo_text += line + "\n";
            if (kind == "@run")
            {
                std::string id;
                fields >> id;
                has_undo = has_undo || id == run_id;
                continue;
            }

            ASInventoryRuleUndoOp op;
            std::string id, from, to;
            const auto found = kinds.find(kind);
            fields >> op.mSeq >> op.mType >> id >> from >> to;
            if (found == kinds.end() || !fields || !LLUUID::validate(id) || !LLUUID::validate(from) || !LLUUID::validate(to))
            {
                error = llformat("The log of run #%s is damaged: line %u of part %u is not valid undo data.",
                                 run_id.c_str(), line_number, (U32)part + 1);
                return false;
            }
            op.mKind = found->second;
            LLStringUtil::replaceChar(op.mType, '_', ' ');
            op.mID.set(id);
            op.mFrom.set(from);
            op.mTo.set(to);
            std::getline(fields, op.mName);
            LLStringUtil::trim(op.mName);
            ops.push_back(op);
        }
    }
    if (!has_undo)
    {
        error = "The log of run #" + run_id + " has no undo data: it was made before Revert existed, or was edited.";
        return false;
    }
    const std::string expected = undoSignature(undo_text);
    if (signature.empty() || expected.empty() || signature != expected)
    {
        ops.clear();
        error = "The log of run #" + run_id + " cannot be reverted: it was not written by this account on this computer, "
                "or it was edited. Only logs your own viewer signed are used.";
        return false;
    }
    return true;
}

// Log parts being loaded for a revert.
struct RevertLoad
{
    std::string mRunID;
    std::vector<std::string> mTexts;
    size_t mPending = 0;
    std::string mError;
};
struct RevertPartRequest
{
    std::shared_ptr<RevertLoad> mLoad;
    size_t mIndex;
};

void onRevertPartLoaded(const LLUUID& asset_id, LLAssetType::EType type, void* user_data, S32 status, LLExtStat)
{
    std::unique_ptr<RevertPartRequest> request(static_cast<RevertPartRequest*>(user_data));
    RevertLoad& load = *request->mLoad;
    if (status == 0)
    {
        LLFileSystem file(asset_id, type, LLFileSystem::READ);
        std::string raw(file.getSize(), '\0');
        if (!raw.empty())
        {
            file.read(reinterpret_cast<U8*>(&raw[0]), (S32)raw.size());
        }
        std::istringstream stream(raw);
        LLNotecard notecard;
        if (notecard.importStream(stream))
        {
            load.mTexts[request->mIndex] = notecard.getText();
        }
        else if (load.mError.empty())
        {
            load.mError = llformat("Part %u of the log of run #%s could not be read.", (U32)request->mIndex + 1, load.mRunID.c_str());
        }
    }
    else if (load.mError.empty())
    {
        load.mError = llformat("Part %u of the log of run #%s could not be loaded (asset error %d).",
                               (U32)request->mIndex + 1, load.mRunID.c_str(), status);
    }

    if (--load.mPending)
    {
        return;
    }
    std::vector<ASInventoryRuleUndoOp> ops;
    std::string error = load.mError;
    if (error.empty() && parseUndo(load.mTexts, load.mRunID, ops, error)
        && ASInventoryRuleRunner::instance().startRevert(load.mRunID, ops, error))
    {
        return;
    }
    showRevertMessage(error);
}
}

void ASInventoryRuleRevert::start(const std::string& input)
{
    std::string run_id = input;
    LLStringUtil::trim(run_id);
    if (!run_id.empty() && run_id[0] == '#')
    {
        run_id.erase(0, 1);
    }
    LLStringUtil::toUpper(run_id);
    static const boost::regex id_regex("^[0-9A-F]{6}$");
    if (!ll_regex_match(run_id, id_regex))
    {
        showRevertMessage("Enter the run ID that ends the log notecard name: 6 characters, for example #A1B2C3.");
        return;
    }
    if (ASInventoryRuleRunner::instance().isRunning())
    {
        showRevertMessage("Wait for the current run to end before reverting.");
        return;
    }
    LLInventoryModelBackgroundFetch& fetch = LLInventoryModelBackgroundFetch::instance();
    if (!fetch.isEverythingFetched() || !gAssetStorage)
    {
        fetch.start();
        showRevertMessage("Your inventory is still loading. Try again in a moment.");
        return;
    }

    log_parts_t parts;
    U32 total = 0;
    findLogParts(run_id, parts, total);
    if (parts.empty())
    {
        showRevertMessage("No log notecard of run #" + run_id + " was found in /" + std::string(LOG_FOLDER)
                          + ". Only notecards you created there are used: put the log back in /" + LOG_FOLDER + " if you moved it.");
        return;
    }
    for (U32 index = 1; index <= total; ++index)
    {
        auto part = parts.find(index);
        if (part == parts.end())
        {
            showRevertMessage(llformat("Part %u of %u of the log of run #%s is missing.", index, total, run_id.c_str()));
            return;
        }
        if (part->second->getAssetUUID().isNull())
        {
            showRevertMessage("The log notecard '" + part->second->getName() + "' is not saved yet. Try again in a moment.");
            return;
        }
    }

    auto load = std::make_shared<RevertLoad>();
    load->mRunID = run_id;
    load->mTexts.resize(total);
    load->mPending = total;
    for (U32 index = 1; index <= total; ++index)
    {
        const LLViewerInventoryItem* item = parts[index];
        gAssetStorage->getInvItemAsset(LLHost(), gAgent.getID(), gAgent.getSessionID(), item->getPermissions().getOwner(),
                                       LLUUID::null, item->getUUID(), item->getAssetUUID(), LLAssetType::AT_NOTECARD,
                                       &onRevertPartLoaded, new RevertPartRequest{ load, index - 1 }, true);
    }
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
      mPendingNames(0),
      mNamesDeadline(0.0),
      mNextNamesCheck(0.0),
      mNext(0),
      mNextKey(0),
      mBatchDeadline(0.0),
      mNextBatchTime(0.0),
      mTrashUsed(false),
      mDone(0), mSkipped(0), mFailed(0),
      mTotalDone(0), mTotalSkipped(0), mTotalFailed(0),
      mStartTime(0),
      mCreatingRunTrash(false),
      mUndoSeq(0)
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

    mPlans.clear();
    mRevertOf.clear();
    beginRun();
    LL_INFOS("InventoryRules") << "Starting " << mRules.size() << " rule(s), run #" << mRunID << LL_ENDL;
    return true;
}

bool ASInventoryRuleRunner::startRevert(const std::string& run_id, std::vector<ASInventoryRuleUndoOp> ops, std::string& error)
{
    if (isRunning())
    {
        error = "Wait for the current run to end before reverting.";
        return false;
    }

    // Newest operation first, so an object moved by several rules goes back step by step.
    std::sort(ops.begin(), ops.end(),
              [](const ASInventoryRuleUndoOp& a, const ASInventoryRuleUndoOp& b) { return a.mSeq > b.mSeq; });
    Plan objects;
    objects.mSummary = "move each object back to the folder it came from, if it is still where the run put it";
    Plan folders;
    folders.mSummary = "move the folders the run created to the Trash, if they are empty";
    for (const ASInventoryRuleUndoOp& op : ops)
    {
        PlannedOp planned;
        planned.mKind = op.mKind;
        planned.mID = op.mID;
        planned.mType = op.mType;
        planned.mName = op.mName;
        if (op.mKind == ASInventoryRuleUndoOp::UNDO_FOLDER)
        {
            folders.mOps.push_back(planned);
            continue;
        }
        // Permanent deletes stay in the list, to be reported as not reverted.
        planned.mTarget = op.mFrom;
        planned.mExpectedParent = op.mTo;
        objects.mOps.push_back(planned);
    }
    if (objects.mOps.empty() && folders.mOps.empty())
    {
        error = "Run #" + run_id + " has nothing to revert: it did not move or delete anything.";
        return false;
    }

    // Each step is a rule whose action gives the operation and the protection checks.
    mRules.clear();
    mPlans.clear();
    if (!objects.mOps.empty())
    {
        ASInventoryRule rule;
        rule.mName = "Revert #" + run_id + ": objects";
        rule.mAction = ASInventoryRule::ACTION_MOVE;
        mRules.push_back(rule);
        mPlans.push_back(objects);
    }
    if (!folders.mOps.empty())
    {
        ASInventoryRule rule;
        rule.mName = "Revert #" + run_id + ": created folders";
        // To the Trash, never permanently (sendPurge also refuses it during a revert).
        rule.mAction = ASInventoryRule::ACTION_DELETE;
        rule.mDeleteMode = ASInventoryRule::DELETE_TRASH;
        mRules.push_back(rule);
        mPlans.push_back(folders);
    }
    mRevertOf = run_id;
    mRevertProblems.clear();
    beginRun();
    LL_INFOS("InventoryRules") << "Reverting run #" << run_id << " (" << ops.size() << " operation(s)), run #" << mRunID << LL_ENDL;
    return true;
}

void ASInventoryRuleRunner::beginRun()
{
    mRuleIndex = 0;
    mStopRequested = false;
    mPending.clear();
    mMatches.clear();
    mNext = 0;
    mDone = mSkipped = mFailed = 0;
    mTotalDone = mTotalSkipped = mTotalFailed = 0;
    mRuleErrors.clear();
    mLog.clear();
    mUndo.clear();
    mUndoSeq = 0;
    mStartTime = time(nullptr);
    mRunID = newRunID();
    mRunTrashID.setNull();
    mCreatingRunTrash = false;

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
            mCreatingRunTrash = false;
            finishRule("Folder creation timed out.");
        }
        break;

    case STATE_NAMES:
        if (mStopRequested)
        {
            finishRun("Stopped.");
        }
        else if (now >= mNextNamesCheck)
        {
            beginApply();
        }
        break;

    case STATE_APPLYING:
        if (!mPending.empty())
        {
            if (now > mBatchDeadline)
            {
                // Something is wrong with the connection or the inventory service: do not keep sending.
                for (const auto& pending : mPending)
                {
                    logObject(pending.second, "failed: not confirmed by the server");
                }
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
    mPendingNames = 0;
    mNamesDeadline = 0.0;
    const std::string summary = isRevert() ? mPlans[mRuleIndex].mSummary : rule.getSummary();
    LL_INFOS("InventoryRules") << "Rule '" << rule.mName << "': " << summary << LL_ENDL;
    mLog += "\n";
    logRule(llformat("rule %u/%u: ", (U32)mRuleIndex + 1, (U32)mRules.size()) + summary);

    // A revert step has its objects already: no destination to resolve, no scan.
    if (isRevert())
    {
        for (const PlannedOp& planned : mPlans[mRuleIndex].mOps)
        {
            mMatches.push_back(planned.mID);
        }
        logRule(llformat("%u object(s)", (U32)mMatches.size()));
        mLogDestination = "Trash";
        mNextBatchTime = nowSeconds();
        mState = STATE_APPLYING;
        updateStatus();
        return;
    }

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
    else if (!rule.isPurge() && mRunTrashID.isNull())
    {
        // The run's trash folder, created on its first Move to Trash rule; resumes in onFolderCreated.
        mCreatingRunTrash = true;
        mCreateKey = ++mNextKey;
        mCreateDeadline = nowSeconds() + CREATE_TIMEOUT;
        const U32 key = mCreateKey;
        mState = STATE_RESOLVING;
        updateStatus();
        gInventory.createNewCategory(gInventory.getRootFolderID(), LLFolderType::FT_NONE, getRunTrashName(),
            [key](const LLUUID& new_id)
            {
                if (ASInventoryRuleRunner::instanceExists())
                {
                    ASInventoryRuleRunner::instance().onFolderCreated(key, new_id);
                }
            });
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
        const LLUUID child_id = ASInventoryRuleSimulation::real().findChildFolder(mDestID, mDestSegments[mDestSegment]);
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
    if (mCreatingRunTrash)
    {
        mCreatingRunTrash = false;
        if (new_id.isNull())
        {
            finishRule("Could not create the folder for the trashed objects.");
            return;
        }
        mRunTrashID = new_id;
        logRule("created folder " + ASInventoryRules::getPath(new_id) + ", moved to the Trash when the run ends");
        beginApply();
        return;
    }
    if (new_id.isNull())
    {
        finishRule("Could not create destination folder: " + mRules[mRuleIndex].mDestination);
        return;
    }
    recordUndo("folder", "folder", new_id, mDestID, LLUUID::null, mDestSegments[mDestSegment]);
    mDestID = new_id;
    ++mDestSegment;
    logRule("created folder " + ASInventoryRules::getPath(new_id));
    continueResolve();
}

void ASInventoryRuleRunner::beginApply()
{
    const ASInventoryRule& rule = mRules[mRuleIndex];
    std::string error;
    // The run's trash folder already holds objects trashed by earlier rules: never match them again.
    if (!ASInventoryRuleScanner::scan(rule, mDestID, mMatches, error, ASInventoryRuleSimulation::real(), &mPendingNames,
                                      mRunTrashID))
    {
        finishRule(error);
        return;
    }

    // Creator names still loading: scan again later, up to NAMES_TIMEOUT. Their items do not match.
    const F64 now = nowSeconds();
    if (mPendingNames)
    {
        if (mNamesDeadline == 0.0)
        {
            mNamesDeadline = now + NAMES_TIMEOUT;
        }
        if (now < mNamesDeadline)
        {
            mMatches.clear();
            mNextNamesCheck = now + NAMES_RETRY;
            mState = STATE_NAMES;
            updateStatus();
            return;
        }
        LL_WARNS("InventoryRules") << mPendingNames << " creator name(s) did not load; their items are not matched" << LL_ENDL;
        logRule(llformat("%u creator name(s) did not load; their items were not matched", mPendingNames));
    }
    LL_INFOS("InventoryRules") << mMatches.size() << " match(es)" << LL_ENDL;
    logRule(llformat("%u match(es)", (U32)mMatches.size()));
    mLogDestination = (rule.mAction == ASInventoryRule::ACTION_MOVE) ? ASInventoryRules::getPath(mDestID)
                      : rule.isPurge() ? "(deleted permanently)" : "Trash/" + getRunTrashName();

    mNext = 0;
    mNextBatchTime = now;
    mState = STATE_APPLYING;
    updateStatus();
}

void ASInventoryRuleRunner::sendBatch()
{
    const ASInventoryRule& rule = mRules[mRuleIndex];
    size_t sent = 0;
    while (sent < BATCH_SIZE && mNext < mMatches.size())
    {
        const PlannedOp* planned = isRevert() ? &mPlans[mRuleIndex].mOps[mNext] : nullptr;
        const LLUUID id = mMatches[mNext++];
        // A revert moves each object to its own folder.
        if (planned && rule.mAction == ASInventoryRule::ACTION_MOVE)
        {
            mDestID = planned->mTarget;
            mLogDestination = gInventory.getCategory(mDestID) ? ASInventoryRules::getPath(mDestID) : std::string("(missing folder)");
        }
        // For skip and failure lines; an object that no longer exists keeps its logged name and type.
        auto describe = [&](PendingOp& op)
        {
            describeObject(id, op);
            if (planned && !gInventory.getObject(id))
            {
                op.mType = planned->mType;
                op.mName = planned->mName;
                op.mFolder = "(gone)";
            }
        };
        // Re-check right before acting: the inventory may have changed since the scan.
        const std::string reason = planned ? getRevertSkipReason(*planned) : ASInventoryRules::getSkipReason(rule, id, mDestID);
        if (!reason.empty())
        {
            ++mSkipped;
            LL_INFOS("InventoryRules") << "Skipped " << ASInventoryRules::getPath(id) << ": " << reason << LL_ENDL;
            PendingOp op;
            describe(op);
            logObject(op, "skipped: " + reason);
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
            PendingOp op;
            describe(op);
            logObject(op, "failed: could not be sent");
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
    // A run moves to its trash folder, without restamping, so items keep their date.
    if (!isRevert() && mRunTrashID.notNull())
    {
        return sendMove(id, mRunTrashID, false);
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
    op.mNewParent = new_parent;
    describeObject(id, op);
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
    // Every permanent delete goes through here. A revert only puts things back: it never purges,
    // whatever its undo data says.
    if (isRevert())
    {
        LL_WARNS("InventoryRules") << "Refused a permanent delete during a revert: " << id << LL_ENDL;
        return false;
    }
    const LLInventoryObject* obj = gInventory.getObject(id);
    if (!obj)
    {
        return false;
    }

    const U32 key = ++mNextKey;
    PendingOp op;
    op.mID = id;
    op.mOldParent = obj->getParentUUID();
    describeObject(id, op);
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
    logObject(op, success ? "done" : "failed: not confirmed by the server");

    if (success)
    {
        ++mDone;
        const char* kind = op.mIsPurge ? "purge"
                           : mRules[mRuleIndex].mAction == ASInventoryRule::ACTION_MOVE ? "move" : "trash";
        recordUndo(kind, op.mType, op.mID, op.mOldParent, op.mNewParent, op.mName);
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

void ASInventoryRuleRunner::describeObject(const LLUUID& id, PendingOp& op) const
{
    const ASInventoryRuleSimulation& real = ASInventoryRuleSimulation::real();
    op.mType = objectTypeLabel(id);
    op.mName = real.getName(id);
    op.mFolder = real.getPath(real.getParent(id));
    op.mTo = mLogDestination;
}

// Why a revert leaves this object (or created folder) as it is; empty when it can be reverted.
std::string ASInventoryRuleRunner::getRevertSkipReason(const PlannedOp& planned) const
{
    if (planned.mKind == ASInventoryRuleUndoOp::UNDO_PURGE)
    {
        return "deleted permanently by the run, cannot be restored";
    }
    const LLInventoryObject* obj = gInventory.getObject(planned.mID);
    if (!obj)
    {
        return planned.mKind == ASInventoryRuleUndoOp::UNDO_TRASH ? "no longer exists (the Trash was emptied?)" : "no longer exists";
    }
    const ASInventoryRule& rule = mRules[mRuleIndex];
    const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);

    if (planned.mKind == ASInventoryRuleUndoOp::UNDO_FOLDER)
    {
        if (gInventory.isObjectDescendentOf(planned.mID, trash_id))
        {
            return "already in the Trash";
        }
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(planned.mID, cats, items);
        if ((cats && !cats->empty()) || (items && !items->empty()))
        {
            return "not empty";
        }
        return ASInventoryRules::getSkipReason(rule, planned.mID, LLUUID::null);
    }

    // Already reverted, by an earlier revert that did not reach its end for example.
    if (obj->getParentUUID() == planned.mTarget)
    {
        return REVERT_ALREADY_DONE;
    }
    if (obj->getParentUUID() != planned.mExpectedParent)
    {
        return "moved after the run";
    }
    if (!gInventory.getCategory(planned.mTarget))
    {
        return "its original folder no longer exists";
    }
    if (gInventory.isObjectDescendentOf(planned.mTarget, trash_id))
    {
        return "its original folder is in the Trash";
    }
    return ASInventoryRules::getSkipReason(rule, planned.mID, planned.mTarget);
}

// "@kind seq type id from to name", read back by parseUndo.
void ASInventoryRuleRunner::recordUndo(const char* kind, const std::string& type, const LLUUID& id, const LLUUID& from,
                                       const LLUUID& to, const std::string& name)
{
    mUndo += llformat("@%s %u ", kind, ++mUndoSeq) + typeToken(type) + " " + id.asString() + " " + from.asString()
             + " " + to.asString() + " " + name + "\n";
}

// Every line starts with the rule name, so the log can be filtered by rule.
void ASInventoryRuleRunner::logRule(const std::string& text)
{
    mLog += "'" + mRules[mRuleIndex].mName + "' | " + text + "\n";
}

// rule | type | name | from | to | status. A revert also keeps what it could not do, for its report.
void ASInventoryRuleRunner::logObject(const PendingOp& op, const std::string& status)
{
    logRule(op.mType + " | " + op.mName + " | from " + op.mFolder + " | to " + op.mTo + " | " + status);
    if (isRevert() && status != "done" && status != std::string("skipped: ") + REVERT_ALREADY_DONE)
    {
        mRevertProblems.push_back(op.mType + " '" + op.mName + "': " + status);
    }
}

void ASInventoryRuleRunner::logRuleCounters()
{
    logRule(llformat("done %u, skipped %u, failed %u", mDone, mSkipped, mFailed));
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
        logRule("error: " + error);
    }
    if (mTrashUsed)
    {
        gInventory.checkTrashOverflow();
        mTrashUsed = false;
    }
    logRuleCounters();
    foldRuleCounters();

    ++mRuleIndex;
    beginRule();
}

void ASInventoryRuleRunner::finishRun(const std::string& status)
{
    // The run's trash folder goes to the Trash with everything moved into it, however the run ended.
    if (mRunTrashID.notNull())
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(mRunTrashID);
        const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        if (cat && trash_id.notNull() && !gInventory.isObjectDescendentOf(mRunTrashID, trash_id))
        {
            gInventory.changeCategoryParent(cat, trash_id, true);
            mTrashUsed = true;
        }
        mRunTrashID.setNull();
    }
    if (mTrashUsed)
    {
        gInventory.checkTrashOverflow();
        mTrashUsed = false;
    }
    // A rule interrupted by a stop or an abort.
    if (mState != STATE_FETCHING && mRuleIndex < mRules.size())
    {
        logRuleCounters();
    }
    foldRuleCounters();
    mPending.clear();
    mMatches.clear();
    mCreateKey = 0;
    mState = STATE_IDLE;
    mStopRequested = false;

    mStatus = llformat("%s Done %u, skipped %u, failed %u.", status.c_str(), mTotalDone, mTotalSkipped, mTotalFailed);
    LL_INFOS("InventoryRules") << mStatus << mRuleErrors << LL_ENDL;

    // Nothing to log when the run stopped before its first rule.
    std::string log_name;
    if (!mLog.empty())
    {
        log_name = LOG_NAME_PREFIX + localTimeText(mStartTime) + " #" + mRunID;
        const std::string title = isRevert() ? "AyaneStorm inventory rules revert of run #" + mRevertOf
                                             : std::string("AyaneStorm inventory rules run");
        const std::string desc = isRevert() ? "Revert of inventory rules run #" + mRevertOf : std::string("Inventory rules run log");
        // Signed, so Revert only accepts logs this account wrote here.
        const std::string undo_text = "@run " + mRunID + "\n" + mUndo;
        const std::string signature = undoSignature(undo_text);
        writeLogNotecards(log_name, desc, title + " #" + mRunID + "\nStarted " + localTimeText(mStartTime)
                                          + ", ended " + localTimeText(time(nullptr)) + "\n" + mStatus + "\n"
                                          + "Object lines: rule | type | name | from | to | status\n" + mLog
                                          + "\n" + UNDO_HEADER + "\n" + undo_text
                                          + (signature.empty() ? std::string() : "@sig " + signature + "\n"));
        mStatus += " Log saved to /" + std::string(LOG_FOLDER) + "/" + log_name + ".";
        mLog.clear();
        mUndo.clear();
    }

    // What the revert could not do, the first lines in a notification, all of them in the log.
    if (isRevert())
    {
        std::string report = "Revert of run #" + mRevertOf + ": " + mStatus;
        if (status != "Finished.")
        {
            report += "\n\nThe revert did not reach its end: execute it again to continue.";
        }
        else if (mRevertProblems.empty())
        {
            report += "\n\nEverything was reverted.";
        }
        if (!mRevertProblems.empty())
        {
            report += llformat("\n\nNot reverted (%u):", (U32)mRevertProblems.size());
            for (size_t i = 0; i < mRevertProblems.size() && i < REVERT_REPORT_LINES; ++i)
            {
                report += "\n- " + mRevertProblems[i];
            }
            if (mRevertProblems.size() > REVERT_REPORT_LINES)
            {
                report += llformat("\n... and %u more.", (U32)(mRevertProblems.size() - REVERT_REPORT_LINES));
            }
            if (!log_name.empty())
            {
                report += "\n\nThe full list is in the notecard /" + std::string(LOG_FOLDER) + "/" + log_name + ".";
            }
        }
        showRevertMessage(report + mRuleErrors);
        mPlans.clear();
        mRevertProblems.clear();
    }
    mStatus += mRuleErrors;
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
    else if (mState == STATE_NAMES)
    {
        status += llformat("loading %u creator name(s)...", mPendingNames);
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
