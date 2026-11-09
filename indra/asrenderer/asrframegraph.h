/**
 * asrframegraph.h
 * The frame as a list of passes, each saying which results it uses and how. That one
 * list gives what section 0a of the renderer plan asks of the result registry:
 *
 * - a pass nothing needs does not run, and neither does what only it needed;
 * - a result has one producer, and a second one is an error when the graph compiles;
 * - replace() stops a pass and runs another in its place, and insertAfter() adds work to
 *   what is already there. Consumers change in neither case, and overrides stack in the
 *   order they were added;
 * - barriers come from the declared uses, so no pass orders itself by hand;
 * - every pass is timed by the backend, under the pass's name.
 *
 * Compiling happens only when something changes, never per frame. A frame walks a flat
 * array, with no lookups. Nothing here touches a GPU API.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace asr
{

using ResultId = uint16_t;
using PassId = uint16_t;

constexpr PassId NO_PASS = 0xFFFF;

// How a pass uses a result. Several may be combined for one result.
namespace GraphAccess
{
constexpr uint32_t CopyRead = 1u << 0;
constexpr uint32_t CopyWrite = 1u << 1;
constexpr uint32_t Indirect = 1u << 2;
constexpr uint32_t VertexRead = 1u << 3;
constexpr uint32_t FragmentRead = 1u << 4;
constexpr uint32_t ComputeRead = 1u << 5;
constexpr uint32_t ComputeWrite = 1u << 6;
constexpr uint32_t ColorTarget = 1u << 7;
constexpr uint32_t DepthTarget = 1u << 8;
constexpr uint32_t FragmentWrite = 1u << 11; // storage written by fragment shaders, as Mac OIT's keys
constexpr uint32_t Writes = CopyWrite | ComputeWrite | ColorTarget | DepthTarget | FragmentWrite;
constexpr uint32_t Stages = ((1u << 9) - 1) | FragmentWrite;

constexpr uint32_t Keep = 1u << 9;           // a write that adds to what is there
constexpr uint32_t PreviousFrame = 1u << 10; // reads what the last frame left: no order inside this frame
} // namespace GraphAccess

enum class ResultScope : uint8_t
{
    Frame,        // made every frame, and gone at the end of it
    Persistent,   // made every frame, and read by the next one
    UntilChanged  // kept across frames until invalidate()
};

struct GraphUse
{
    ResultId result;
    uint32_t access;
};

// What a barrier before a pass has to do for one result.
struct GraphTransition
{
    ResultId result;
    uint32_t source;       // writes and reads that must finish first
    uint32_t destination;  // the uses this pass adds
    uint32_t previous;     // the last use, which for an image gives the layout it is in
    bool discard;          // its contents do not matter, so an image can start undefined
};

class FrameGraph
{
public:
    using Body = std::function<bool()>;

    ResultId addResult(const char* name, ResultScope scope);
    PassId addPass(const char* name, std::vector<GraphUse> uses);
    void setBody(PassId pass, Body body);

    // Overrides. The pass given stops running, or is followed by the new one.
    PassId replace(PassId pass, const char* name, std::vector<GraphUse> uses);
    PassId insertAfter(PassId pass, const char* name, std::vector<GraphUse> uses);

    // A sink is what the frame is for. Only sinks, and what they lead to, run.
    void setSink(ResultId result, uint32_t access, bool enabled);

    // An UntilChanged result is made again on the next frame.
    void invalidate(ResultId result);

    // Does nothing unless something changed. False, with the reason, when the
    // passes cannot work as declared.
    bool compile(std::string& error);

    // The frame: for each compiled pass, its transitions, then run().
    uint32_t passCount() const { return uint32_t(mCompiled.size()); }
    const char* passName(uint32_t index) const { return mPasses[mCompiled[index].pass].name; }
    PassId passId(uint32_t index) const { return mCompiled[index].pass; }
    const GraphTransition* transitions(uint32_t index, uint32_t& count) const;
    bool run(uint32_t index);

    // Transitions after the last pass, into what the sinks need.
    const GraphTransition* finalTransitions(uint32_t& count) const;

    bool isRunning(PassId pass) const;

private:
    struct Result
    {
        const char* name;
        ResultScope scope;
        bool stale;
        uint32_t sinkAccess;
    };

    struct Pass
    {
        const char* name;
        std::vector<GraphUse> uses;
        Body body;
        PassId replacedBy = NO_PASS;
        std::vector<PassId> following;
    };

    struct Compiled
    {
        PassId pass;
        uint32_t firstTransition;
        uint32_t transitionCount;
    };

    void appendInOrder(PassId pass, std::vector<PassId>& order) const;
    void findLivePasses(const std::vector<PassId>& order, std::vector<PassId>& live) const;
    bool checkProducers(const std::vector<PassId>& live, std::string& error) const;
    void buildTransitions(const std::vector<PassId>& live);

    std::vector<Result> mResults;
    std::vector<Pass> mPasses;
    std::vector<PassId> mDeclared; // passes added with addPass, in order
    std::vector<Compiled> mCompiled;
    std::vector<GraphTransition> mTransitions;
    uint32_t mFinalFirst = 0;
    bool mDirty = true;
};

} // namespace asr
