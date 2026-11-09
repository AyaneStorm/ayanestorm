/**
 * asrscenegraph.h
 * The scene's passes and results, declared once for both backends. A backend only
 * gives each pass its body, and turns transitions into whatever its API needs.
 *
 * Two-phase occlusion: the first phase tests clusters against last frame's depth and
 * draws what passes, which is nearly everything that will show. A pyramid of the new
 * depth is built, and what the first phase hid is tested again against it. So the guess
 * from last frame can cost extra drawing, but can never lose anything.
 */
#pragma once

#include "asrframegraph.h"

namespace asr
{

// Added in this order by declareSceneGraph(), so these are their ids.
enum SceneResult : ResultId
{
    ResultSceneData,      // tables and geometry chunks
    ResultDrawArguments,
    ResultVisibleClusters,
    ResultRetestList,
    ResultVisibility,
    ResultDepth,
    ResultDepthPyramid,
    ResultAlbedo,
    ResultNormals,
    ResultSceneCounts,    // read back for statistics
    ResultAuditVisibility,
    ResultAuditPixels,
    ResultGroupList,      // cluster groups whose clusters are tested next
    ResultTextureFeedback, // a word per texture slot, added to by every resolve
    ResultFeedbackReadback, // read back at the end of a feedback window
    ResultCount
};

enum ScenePass : PassId
{
    PassUpload,
    PassSkinPalette, // the skin passes: SKIN_PALETTE to SKIN_GROUPS, in order
    PassSkinBind,
    PassSkinVertices,
    PassSkinClusters,
    PassSkinGroups,
    PassCullEarly,
    PassGroupsEarly,
    PassDrawEarly,
    PassPyramidEarly,
    PassCullLate,
    PassGroupsLate,
    PassDrawLate,
    PassPyramidFinal,
    PassResolve,
    PassReadCounts,
    PassAuditReset,
    PassAuditCull,
    PassAuditGroups,
    PassAuditDraw,
    PassAuditRead,
    PassReadFeedback,
    PassCount
};

constexpr uint32_t MAX_SCENE_PASSES = PassCount;

void declareSceneGraph(FrameGraph& graph);

} // namespace asr
