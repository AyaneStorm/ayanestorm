/**
 * @file asinventoryrules.h
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

#ifndef AS_INVENTORYRULES_H
#define AS_INVENTORYRULES_H

#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "llviewerinventory.h"

#include <boost/regex.hpp>

#include <map>
#include <string>
#include <vector>

class LLEventTimer;

// One user rule, stored inside a ruleset.
struct ASInventoryRule
{
    enum ESearchType { SEARCH_CONTAINS, SEARCH_STARTS_WITH, SEARCH_ENDS_WITH, SEARCH_REGEX, SEARCH_EXACT };
    // MATCH_CONTAINING_FOLDER: the search runs on items, and each matching item's parent folder is selected.
    enum EMatchType { MATCH_ITEM, MATCH_FOLDER, MATCH_CONTAINING_FOLDER };
    enum EAction { ACTION_NONE, ACTION_MOVE, ACTION_DELETE };
    enum EDeleteMode { DELETE_TRASH, DELETE_PURGE };
    // Item filters, used when the search runs on items (MATCH_ITEM, MATCH_CONTAINING_FOLDER).
    enum EItemType { ITEM_ANY, ITEM_ANIMATION, ITEM_BODYPART, ITEM_CALLINGCARD, ITEM_CLOTHING, ITEM_GESTURE,
                     ITEM_LANDMARK, ITEM_MATERIAL, ITEM_NOTECARD, ITEM_OBJECT, ITEM_SCRIPT, ITEM_SETTINGS,
                     ITEM_SNAPSHOT, ITEM_SOUND, ITEM_TEXTURE };
    enum EAge { AGE_ANY, AGE_NEWER, AGE_OLDER };
    enum ECreator { CREATOR_ANYONE, CREATOR_ME, CREATOR_OTHERS, CREATOR_SPECIFIC };
    enum EPermission { PERMISSION_ANY, PERMISSION_YES, PERMISSION_NO };

    LLUUID mID;                     // stable identity, so editors survive reordering and deletions
    std::string mName;
    std::string mSearch;
    ESearchType mSearchType = SEARCH_CONTAINS;
    bool mMatchCase = false;
    std::string mExcept;            // objects (and folder subtrees) to ignore; ';'-separated unless regex
    ESearchType mExceptType = SEARCH_CONTAINS;
    bool mExceptMatchCase = false;
    EMatchType mMatchType = MATCH_ITEM;
    EAction mAction = ACTION_NONE;
    EDeleteMode mDeleteMode = DELETE_TRASH;
    std::string mDestination;       // folder path, used by ACTION_MOVE
    bool mCreateDestination = false;
    EItemType mItemType = ITEM_ANY;
    EAge mAge = AGE_ANY;            // compared with the item creation date
    U32 mAgeDays = 0;
    U32 mAgeHours = 0;
    ECreator mCreator = CREATOR_ANYONE;
    std::string mCreatorName;       // CREATOR_SPECIFIC: user name or UUID
    EPermission mPermCopy = PERMISSION_ANY;     // owner permissions
    EPermission mPermModify = PERMISSION_ANY;
    EPermission mPermTransfer = PERMISSION_ANY;

    LLSD asLLSD() const;
    static ASInventoryRule fromLLSD(const LLSD& sd);

    bool isPurge() const { return mAction == ACTION_DELETE && mDeleteMode == DELETE_PURGE; }
    bool usesItemFilters() const { return mMatchType != MATCH_FOLDER; }
    // Short human-readable description for the rule list.
    std::string getSummary() const;
};

namespace ASInventoryRules
{
    typedef std::vector<ASInventoryRule> rule_list_t;
}

// Named, ordered list of rules. Persisted as LLSD in the per-account ASInventoryRulesets setting.
struct ASInventoryRuleset
{
    LLUUID mID;
    std::string mName;
    ASInventoryRules::rule_list_t mRules;

    LLSD asLLSD() const;
    static ASInventoryRuleset fromLLSD(const LLSD& sd);

    // Short human-readable description for the rulesets list.
    std::string getSummary() const;
    bool hasAction() const;
};

// View of My Inventory for the scanner. Empty, it is the real inventory. A dry run records in it the
// moves, deletions and folder creations of each rule, so the next rules see their result.
class ASInventoryRuleSimulation
{
public:
    typedef std::vector<LLViewerInventoryCategory*> cat_list_t;
    typedef std::vector<LLViewerInventoryItem*> item_list_t;

    // Shared empty instance.
    static const ASInventoryRuleSimulation& real();

    void getChildren(const LLUUID& folder_id, cat_list_t& cats, item_list_t& items) const;
    LLViewerInventoryCategory* getCategory(const LLUUID& id) const;
    LLUUID getParent(const LLUUID& id) const;
    std::string getName(const LLUUID& id) const;
    // Created by the dry run, not in the inventory.
    bool isVirtual(const LLUUID& id) const { return mVirtual.count(id) != 0; }
    bool exists(const LLUUID& id) const;
    bool isDescendentOf(const LLUUID& id, const LLUUID& ancestor_id) const;
    LLUUID findChildFolder(const LLUUID& parent_id, const std::string& name) const;
    LLUUID findFolderByPath(const std::string& path) const;
    std::string getPath(const LLUUID& id) const;

    // A null parent means deleted.
    void move(const LLUUID& id, const LLUUID& new_parent_id);
    LLUUID createFolder(const LLUUID& parent_id, const std::string& name);

private:
    std::map<LLUUID, LLUUID> mParents;          // new parent of each moved object
    std::map<LLUUID, uuid_vec_t> mAdded;        // objects moved into each folder
    std::map<LLUUID, LLPointer<LLViewerInventoryCategory> > mVirtual;
};

namespace ASInventoryRules
{
    typedef std::vector<ASInventoryRuleset> ruleset_list_t;

    // Version of the stored setting and of JSON exports. Bump it whenever a rule field is added or
    // changes meaning, and add a step to migrate(): an older viewer ignoring a new filter would match more.
    constexpr S32 FORMAT_VERSION = 1;

    ruleset_list_t load();
    // Refused (returns false) when the stored rulesets come from a newer viewer.
    bool save(const ruleset_list_t& rulesets);
    // Not empty when the stored rulesets come from a newer viewer and cannot be read.
    const std::string& getStorageError();

    // JSON import/export. An export is an envelope {format, version, type, data}; data is a rule,
    // a ruleset (name and rules) or the list of rulesets. Ids are not exported.
    enum EExportType { EXPORT_RULE, EXPORT_RULESET, EXPORT_RULESETS };
    std::string exportJson(const ASInventoryRule& rule);
    std::string exportJson(const ASInventoryRuleset& ruleset);
    std::string exportJson(const ruleset_list_t& rulesets);
    // Accepts this version and older ones (migrated), refuses newer ones. Every field is validated;
    // error names the ruleset, rule and field. rulesets receives new ids; a single rule comes back
    // as one unnamed ruleset holding it.
    bool importJson(const std::string& text, EExportType& type, ruleset_list_t& rulesets, std::string& error);

    // Index of the ruleset / rule with this id, or -1.
    S32 findRuleset(const ruleset_list_t& rulesets, const LLUUID& id);
    S32 findRule(const rule_list_t& rules, const LLUUID& id);

    // Path helpers. Paths are '/'-separated folder names from the My Inventory root;
    // segments compare case-insensitively.
    std::vector<std::string> splitPath(const std::string& path);
    LLUUID findFolderByPath(const std::string& path);
    std::string getPath(const LLUUID& id);

    // Returns an empty string when the rule's action may be applied to id, else why it is skipped.
    // dest_id may be null when the destination does not exist yet (preview).
    std::string getSkipReason(const ASInventoryRule& rule, const LLUUID& id, const LLUUID& dest_id,
                              const ASInventoryRuleSimulation& sim = ASInventoryRuleSimulation::real());
}

// Compiled matcher for one search term.
class ASInventoryRuleMatcher
{
public:
    // Returns false and fills error when the term cannot be matched (empty, bad regex).
    bool init(const std::string& search, ASInventoryRule::ESearchType type, bool match_case, std::string& error);
    bool matches(const std::string& text) const;
    // A search containing '/' is compared with the full path ("/Objects/Box/demo") instead of the name.
    bool usesPath() const { return mUsesPath; }

private:
    ASInventoryRule::ESearchType mType = ASInventoryRule::SEARCH_CONTAINS;
    bool mMatchCase = false;
    bool mUsesPath = false;
    std::string mSearch;            // lower-cased when !mMatchCase
    boost::regex mRegex;
};

// Walks My Inventory and collects the objects a rule matches.
namespace ASInventoryRuleScanner
{
    // dest_id (may be null) is skipped with its subtree. pending_names (optional) receives the number
    // of creator names still loading; their items do not match yet.
    bool scan(const ASInventoryRule& rule, const LLUUID& dest_id, uuid_vec_t& matches, std::string& error,
              const ASInventoryRuleSimulation& sim = ASInventoryRuleSimulation::real(), U32* pending_names = nullptr);
}

// What executing rules would do, computed without changing anything.
struct ASInventoryRuleDryRun
{
    struct Entry
    {
        LLUUID mID;
        std::string mName;
        std::string mFolder;        // parent folder path before the action
        std::string mSkipReason;    // empty: the action would be applied
    };
    struct Rule
    {
        std::string mName;
        std::string mAction;        // "Move to /A", "Move to Trash", "Delete permanently"
        std::string mNote;          // error, or destination creation
        std::vector<Entry> mEntries;
        U32 mApplied = 0;
        U32 mSkipped = 0;
    };

    // Rules without an action are left out, as when executing.
    void run(const ASInventoryRules::rule_list_t& rules);

    std::vector<Rule> mRules;
    U32 mPendingNames = 0;
};

// Runs rules one after another. Operations are sent in batches of BATCH_SIZE; the next
// batch is only sent when the server confirmed every operation of the current one.
class ASInventoryRuleRunner : public LLSingleton<ASInventoryRuleRunner>
{
    LLSINGLETON(ASInventoryRuleRunner);
    ~ASInventoryRuleRunner();

public:
    // Returns false when a run is already active.
    bool start(const ASInventoryRules::rule_list_t& rules);
    void stop();

    bool isRunning() const { return mState != STATE_IDLE; }
    bool isStopRequested() const { return mStopRequested; }
    // 0..1, or a negative value while the amount of work is unknown (inventory loading).
    F32 getProgress() const;
    const std::string& getStatus() const { return mStatus; }
    // Forgets the last run's result (no effect while running).
    void clearStatus();

    // Called by the timer, returns true when the run ended (timer deletes itself).
    bool onTick();

    // Completion entry points for message acks and AIS callbacks.
    void onOperationDone(U32 key, bool success);
    void onFolderCreated(U32 key, const LLUUID& new_id);

private:
    // STATE_NAMES: the scan waits for creator names to load.
    enum EState { STATE_IDLE, STATE_FETCHING, STATE_RESOLVING, STATE_NAMES, STATE_APPLYING };

    struct PendingOp
    {
        LLUUID mID;
        LLUUID mOldParent;
        bool mIsPurge = false;
    };

    void beginRule();
    void continueResolve();
    void beginApply();
    void sendBatch();
    bool sendOperation(const LLUUID& id);
    bool sendMove(const LLUUID& id, const LLUUID& new_parent, bool restamp);
    bool sendPurge(const LLUUID& id);
    void finishRule(const std::string& error = std::string());
    void finishRun(const std::string& status);
    void foldRuleCounters();
    void updateStatus();

    EState mState;
    bool mStopRequested;
    LLEventTimer* mTimer;

    ASInventoryRules::rule_list_t mRules;
    size_t mRuleIndex;

    // Destination resolution / creation.
    std::vector<std::string> mDestSegments;
    size_t mDestSegment;
    LLUUID mDestID;
    U32 mCreateKey;
    F64 mCreateDeadline;

    // Creator names.
    U32 mPendingNames;
    F64 mNamesDeadline;             // 0 until the rule first waits for names
    F64 mNextNamesCheck;

    // Apply phase.
    uuid_vec_t mMatches;
    size_t mNext;
    std::map<U32, PendingOp> mPending;
    U32 mNextKey;
    F64 mBatchDeadline;             // the in-flight batch must be confirmed before this time
    F64 mNextBatchTime;             // pause between two batches
    bool mTrashUsed;

    // Counters for the current rule, and totals for the run.
    U32 mDone, mSkipped, mFailed;
    U32 mTotalDone, mTotalSkipped, mTotalFailed;
    std::string mStatus;
    std::string mRuleErrors;
};

#endif // AS_INVENTORYRULES_H
