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

    LLSD asLLSD() const;
    static ASInventoryRule fromLLSD(const LLSD& sd);

    bool isPurge() const { return mAction == ACTION_DELETE && mDeleteMode == DELETE_PURGE; }
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

namespace ASInventoryRules
{
    typedef std::vector<ASInventoryRuleset> ruleset_list_t;

    ruleset_list_t load();
    void save(const ruleset_list_t& rulesets);

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
    std::string getSkipReason(const ASInventoryRule& rule, const LLUUID& id, const LLUUID& dest_id);
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
    // dest_id (may be null) is skipped with its subtree.
    bool scan(const ASInventoryRule& rule, const LLUUID& dest_id, uuid_vec_t& matches, std::string& error);
}

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
    enum EState { STATE_IDLE, STATE_FETCHING, STATE_RESOLVING, STATE_APPLYING };

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
