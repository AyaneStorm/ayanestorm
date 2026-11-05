/**
 * asrframegraph.cpp
 * See asrframegraph.h.
 */
#include "asrframegraph.h"

#include <algorithm>
#include <utility>

namespace asr
{

namespace
{

bool writes(uint32_t access)
{
    return (access & GraphAccess::Writes) != 0;
}

bool reads(uint32_t access)
{
    return (access & GraphAccess::Stages & ~GraphAccess::Writes) != 0 || (access & GraphAccess::Keep) != 0;
}

// Where a result stands as passes go by.
struct ResultState
{
    uint32_t lastWrite = 0;
    uint32_t readsSinceWrite = 0;
    uint32_t lastUse = 0;
    bool used = false;
};

} // namespace

ResultId FrameGraph::addResult(const char* name, ResultScope scope)
{
    mResults.push_back({ name, scope, true, 0 });
    mDirty = true;
    return ResultId(mResults.size() - 1);
}

PassId FrameGraph::addPass(const char* name, std::vector<GraphUse> uses)
{
    Pass pass;
    pass.name = name;
    pass.uses = std::move(uses);
    mPasses.push_back(std::move(pass));
    mDeclared.push_back(PassId(mPasses.size() - 1));
    mDirty = true;
    return mDeclared.back();
}

void FrameGraph::setBody(PassId pass, Body body)
{
    mPasses[pass].body = std::move(body);
}

PassId FrameGraph::replace(PassId pass, const char* name, std::vector<GraphUse> uses)
{
    Pass added;
    added.name = name;
    added.uses = std::move(uses);
    mPasses.push_back(std::move(added));
    const PassId id = PassId(mPasses.size() - 1);
    mPasses[pass].replacedBy = id;
    mDirty = true;
    return id;
}

PassId FrameGraph::insertAfter(PassId pass, const char* name, std::vector<GraphUse> uses)
{
    Pass added;
    added.name = name;
    added.uses = std::move(uses);
    mPasses.push_back(std::move(added));
    const PassId id = PassId(mPasses.size() - 1);
    mPasses[pass].following.push_back(id);
    mDirty = true;
    return id;
}

void FrameGraph::setSink(ResultId result, uint32_t access, bool enabled)
{
    const uint32_t wanted = enabled ? access : 0;
    if (mResults[result].sinkAccess != wanted)
    {
        mResults[result].sinkAccess = wanted;
        mDirty = true;
    }
}

void FrameGraph::invalidate(ResultId result)
{
    mResults[result].stale = true;
}

void FrameGraph::appendInOrder(PassId pass, std::vector<PassId>& order) const
{
    const Pass& declared = mPasses[pass];
    if (declared.replacedBy != NO_PASS)
    {
        appendInOrder(declared.replacedBy, order);
    }
    else
    {
        order.push_back(pass);
    }
    for (PassId following : declared.following)
    {
        appendInOrder(following, order);
    }
}

// Walks backwards from the sinks. A pass that reads the last frame's result makes that
// result needed at the end of this frame too, so the walk repeats until nothing changes.
void FrameGraph::findLivePasses(const std::vector<PassId>& order, std::vector<PassId>& live) const
{
    std::vector<uint8_t> neededAtEnd(mResults.size(), 0);
    for (size_t i = 0; i < mResults.size(); ++i)
    {
        neededAtEnd[i] = mResults[i].sinkAccess != 0;
    }

    while (true)
    {
        std::vector<uint8_t> needed = neededAtEnd;
        std::vector<uint8_t> nextEnd = neededAtEnd;
        live.clear();
        for (auto pass = order.rbegin(); pass != order.rend(); ++pass)
        {
            const std::vector<GraphUse>& uses = mPasses[*pass].uses;
            const bool alive = std::any_of(uses.begin(), uses.end(),
                                           [&needed](const GraphUse& use) { return writes(use.access) && needed[use.result]; });
            if (!alive)
            {
                continue;
            }
            live.push_back(*pass);
            for (const GraphUse& use : uses)
            {
                if (writes(use.access) && !(use.access & GraphAccess::Keep))
                {
                    needed[use.result] = 0;
                }
            }
            for (const GraphUse& use : uses)
            {
                if (use.access & GraphAccess::PreviousFrame)
                {
                    nextEnd[use.result] = 1;
                }
                else if (reads(use.access))
                {
                    needed[use.result] = 1;
                }
            }
        }
        if (nextEnd == neededAtEnd)
        {
            break;
        }
        neededAtEnd = std::move(nextEnd);
    }
    std::reverse(live.begin(), live.end());
}

bool FrameGraph::checkProducers(const std::vector<PassId>& live, std::string& error) const
{
    std::vector<uint8_t> produced(mResults.size(), 0);
    for (PassId id : live)
    {
        const Pass& pass = mPasses[id];
        for (const GraphUse& use : pass.uses)
        {
            const Result& result = mResults[use.result];
            const bool needsEarlier = reads(use.access) && !(use.access & GraphAccess::PreviousFrame) && result.scope == ResultScope::Frame;
            if (needsEarlier && !produced[use.result])
            {
                error = std::string("nothing makes ") + result.name + " before " + pass.name;
                return false;
            }
        }
        for (const GraphUse& use : pass.uses)
        {
            if (!writes(use.access))
            {
                continue;
            }
            if (!(use.access & GraphAccess::Keep) && produced[use.result])
            {
                error = std::string("two passes make ") + mResults[use.result].name + ", the second is " + pass.name;
                return false;
            }
            produced[use.result] = 1;
        }
    }
    return true;
}

// Reads after a write each need that write to reach their own stage, so a new kind of
// read gets its barrier even when another read has already waited. Going around twice
// makes the first pass of a frame wait for what the last frame left behind.
void FrameGraph::buildTransitions(const std::vector<PassId>& live)
{
    std::vector<ResultState> states(mResults.size());
    std::vector<uint8_t> keptInFrame(mResults.size(), 0);

    const auto step = [&](ResultId result, uint32_t access, bool record)
    {
        ResultState& state = states[result];
        const uint32_t stages = access & GraphAccess::Stages;
        GraphTransition transition{};
        transition.result = result;
        transition.previous = state.lastUse;
        transition.discard = !keptInFrame[result] && mResults[result].scope == ResultScope::Frame;
        bool needed = false;
        if (writes(access))
        {
            transition.source = state.lastWrite | state.readsSinceWrite;
            transition.destination = stages;
            state.lastWrite = stages;
            state.readsSinceWrite = 0;
            needed = true;
        }
        else
        {
            const uint32_t added = stages & ~state.readsSinceWrite;
            transition.source = state.lastWrite;
            transition.destination = added;
            state.readsSinceWrite |= added;
            needed = added != 0;
        }
        state.lastUse = stages;
        keptInFrame[result] = 1;
        if (record && needed)
        {
            mTransitions.push_back(transition);
        }
    };

    mCompiled.clear();
    mTransitions.clear();
    for (int round = 0; round < 2; ++round)
    {
        const bool record = round == 1;
        std::fill(keptInFrame.begin(), keptInFrame.end(), uint8_t(0));
        for (PassId id : live)
        {
            const uint32_t first = uint32_t(mTransitions.size());
            for (const GraphUse& use : mPasses[id].uses)
            {
                step(use.result, use.access, record);
            }
            if (record)
            {
                mCompiled.push_back({ id, first, uint32_t(mTransitions.size()) - first });
            }
        }
        if (record)
        {
            mFinalFirst = uint32_t(mTransitions.size());
        }
        for (size_t result = 0; result < mResults.size(); ++result)
        {
            if (mResults[result].sinkAccess != 0)
            {
                step(ResultId(result), mResults[result].sinkAccess, record);
            }
        }
    }
}

bool FrameGraph::compile(std::string& error)
{
    if (!mDirty)
    {
        return true;
    }
    std::vector<PassId> order;
    for (PassId pass : mDeclared)
    {
        appendInOrder(pass, order);
    }
    std::vector<PassId> live;
    findLivePasses(order, live);
    if (!checkProducers(live, error))
    {
        return false;
    }
    buildTransitions(live);
    mDirty = false;
    return true;
}

const GraphTransition* FrameGraph::transitions(uint32_t index, uint32_t& count) const
{
    count = mCompiled[index].transitionCount;
    return mTransitions.data() + mCompiled[index].firstTransition;
}

const GraphTransition* FrameGraph::finalTransitions(uint32_t& count) const
{
    count = uint32_t(mTransitions.size()) - mFinalFirst;
    return mTransitions.data() + mFinalFirst;
}

bool FrameGraph::run(uint32_t index)
{
    Pass& pass = mPasses[mCompiled[index].pass];

    bool onlyKept = false;
    bool stale = false;
    for (const GraphUse& use : pass.uses)
    {
        if (writes(use.access))
        {
            const Result& result = mResults[use.result];
            if (result.scope != ResultScope::UntilChanged)
            {
                stale = true;
                break;
            }
            onlyKept = true;
            stale = stale || result.stale;
        }
    }
    if (onlyKept && !stale)
    {
        return true;
    }

    if (pass.body && !pass.body())
    {
        return false;
    }
    for (const GraphUse& use : pass.uses)
    {
        if (writes(use.access))
        {
            mResults[use.result].stale = false;
        }
    }
    return true;
}

bool FrameGraph::isRunning(PassId pass) const
{
    return std::any_of(mCompiled.begin(), mCompiled.end(), [pass](const Compiled& compiled) { return compiled.pass == pass; });
}

} // namespace asr
