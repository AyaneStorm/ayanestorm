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

    // Transparency's interface: it reads the opaque image, the opaque depth and the blended
    // lists, and makes the scene colour. Whatever makes it can be replaced as a whole: see
    // declareMacOIT(). Its own results below are private to it.
    ResultLit,            // the opaque image, kept for readers such as depth of field
    ResultSceneColor,     // opaque and transparent together
    ResultMacOITState,    // Mac OIT's per-pixel keys, farthest depth and count, kept clean between frames
    ResultMacOITMoments,  // tail moments
    ResultMacOITTailDepth, // tail optical depth
    ResultMacOITColor,    // sum of colour * weight, and glow
    ResultMacOITWeights,  // sum of weight, and of optical depth
    ResultCount
};

enum ScenePass : PassId
{
    PassUpload,
    PassSkinPose, // cheap pose/bounds work before culling; deformation follows demand
    PassSkinPalette,
    PassSkinBind,
    PassSkinMorph,
    PassSkinClusters,
    PassSkinGroups,
    PassCullEarly,
    PassGroupsEarly,
    PassSkinVertices,
    PassSkinAudit,
    PassDrawEarly,
    PassPyramidEarly,
    PassCullLate,
    PassGroupsLate,
    PassSkinVerticesLate,
    PassDrawLate,
    PassPyramidFinal,
    PassResolve,
    PassMacOITKeys,    // Mac OIT, declared by declareMacOIT()
    PassMacOITGate,
    PassMacOITTail,
    PassMacOITColor,
    PassMacOITResolve,
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

// Transparency by Mac OIT: makes ResultSceneColor from ResultLit, the opaque depth and the
// blended lists. Another transparency replaces these passes with its own (FrameGraph's
// replacement), making the same result, and no reader of it changes.
void declareMacOIT(FrameGraph& graph);

} // namespace asr
