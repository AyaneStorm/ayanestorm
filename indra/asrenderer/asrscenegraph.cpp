/**
 * asrscenegraph.cpp
 * See asrscenegraph.h.
 */
#include "asrscenegraph.h"

namespace asr
{

void declareSceneGraph(FrameGraph& graph)
{
    using namespace GraphAccess;

    graph.addResult("scene data", ResultScope::Persistent);
    graph.addResult("draw arguments", ResultScope::Frame);
    graph.addResult("visible clusters", ResultScope::Frame);
    graph.addResult("retest list", ResultScope::Frame);
    graph.addResult("visibility", ResultScope::Frame);
    graph.addResult("depth", ResultScope::Frame);
    graph.addResult("depth pyramid", ResultScope::Persistent);
    graph.addResult("albedo", ResultScope::Frame);
    graph.addResult("normals", ResultScope::Frame);
    graph.addResult("scene counts", ResultScope::Frame);
    graph.addResult("audit visibility", ResultScope::Frame);
    graph.addResult("audit pixels", ResultScope::Frame);
    graph.addResult("group list", ResultScope::Frame);
    graph.addResult("texture feedback", ResultScope::Persistent);
    graph.addResult("feedback readback", ResultScope::Frame);
    graph.addResult("lit", ResultScope::Frame);
    graph.addResult("scene color", ResultScope::Frame);
    graph.addResult("macoit state", ResultScope::Persistent);
    graph.addResult("macoit moments", ResultScope::Frame);
    graph.addResult("macoit tail depth", ResultScope::Frame);
    graph.addResult("macoit color", ResultScope::Frame);
    graph.addResult("macoit weights", ResultScope::Frame);
    graph.addResult("selection list", ResultScope::Frame);
    graph.addResult("selection mask", ResultScope::Frame);
    graph.addResult("ui depth", ResultScope::Frame);

    // The group passes read their dispatch size from the counts, so they take the draw
    // arguments as Indirect too.
    graph.addPass("upload", { { ResultSceneData, CopyWrite | Keep }, { ResultDrawArguments, CopyWrite } });

    // Joint-derived conservative bounds need no deformed positions. Culling can therefore
    // decide demand before spending vertex bandwidth; late depth rejection has recovery.
    for (const char* name : { "skin pose", "skin palette", "skin bind", "skin morph", "skin clusters", "skin groups" })
    {
        graph.addPass(name, { { ResultSceneData, ComputeRead | ComputeWrite | Keep } });
    }
    graph.addPass("cull early", { { ResultSceneData, ComputeRead | ComputeWrite | Keep },
                                  { ResultDepthPyramid, ComputeRead | PreviousFrame },
                                  { ResultDrawArguments, ComputeWrite | Keep },
                                  { ResultVisibleClusters, ComputeWrite },
                                  { ResultRetestList, ComputeWrite },
                                  { ResultGroupList, ComputeWrite },
                                  { ResultSelectionList, ComputeWrite } });
    graph.addPass("groups early", { { ResultSceneData, ComputeRead | ComputeWrite | Keep },
                                    { ResultDepthPyramid, ComputeRead | PreviousFrame },
                                    { ResultGroupList, ComputeRead },
                                    { ResultDrawArguments, Indirect | ComputeWrite | Keep },
                                    { ResultVisibleClusters, ComputeWrite | Keep },
                                    { ResultRetestList, ComputeWrite | Keep } });
    graph.addPass("skin vertices", { { ResultSceneData, ComputeRead | ComputeWrite | Keep } });
    // Its body is a no-op outside audit mode. Audit positions must follow deformation,
    // while the read-counts pass must still see their results in the same frame.
    graph.addPass("skin audit", { { ResultSceneData, ComputeRead },
                                  { ResultDrawArguments, ComputeWrite | Keep } });
    graph.addPass("draw early", { { ResultSceneData, VertexRead },
                                  { ResultDrawArguments, Indirect },
                                  { ResultVisibleClusters, VertexRead },
                                  { ResultVisibility, ColorTarget },
                                  { ResultDepth, DepthTarget } });
    graph.addPass("pyramid early", { { ResultDepth, ComputeRead }, { ResultDepthPyramid, ComputeWrite } });
    graph.addPass("cull late", { { ResultSceneData, ComputeRead | ComputeWrite | Keep },
                                 { ResultDepthPyramid, ComputeRead },
                                 { ResultRetestList, ComputeRead },
                                 { ResultDrawArguments, Indirect | ComputeWrite | Keep },
                                 { ResultVisibleClusters, ComputeWrite | Keep },
                                 { ResultGroupList, ComputeWrite | Keep } });
    graph.addPass("groups late", { { ResultSceneData, ComputeRead | ComputeWrite | Keep },
                                   { ResultDepthPyramid, ComputeRead },
                                   { ResultGroupList, ComputeRead },
                                   { ResultDrawArguments, Indirect | ComputeWrite | Keep },
                                   { ResultVisibleClusters, ComputeWrite | Keep } });
    graph.addPass("skin vertices late", { { ResultSceneData, ComputeRead | ComputeWrite | Keep } });
    graph.addPass("draw late", { { ResultSceneData, VertexRead },
                                 { ResultDrawArguments, Indirect },
                                 { ResultVisibleClusters, VertexRead },
                                 { ResultVisibility, ColorTarget | Keep },
                                 { ResultDepth, DepthTarget | Keep } });
    graph.addPass("pyramid final", { { ResultDepth, ComputeRead }, { ResultDepthPyramid, ComputeWrite | Keep } });
    graph.addPass("resolve", { { ResultSceneData, ComputeRead },
                               { ResultVisibleClusters, ComputeRead },
                               { ResultVisibility, ComputeRead },
                               { ResultAlbedo, ComputeWrite },
                               { ResultNormals, ComputeWrite },
                               { ResultLit, ComputeWrite },
                               { ResultTextureFeedback, ComputeWrite | Keep } });
    declareMacOIT(graph);
    // The scene's selections against the final opaque depth, read as a texture, and the
    // HUD's selected entries in the HUD view. The composite pass draws outlines from it.
    graph.addPass("selection mask", { { ResultSceneData, VertexRead | FragmentRead },
                                      { ResultDrawArguments, Indirect | Keep },
                                      { ResultSelectionList, VertexRead },
                                      { ResultDepth, FragmentRead | Keep },
                                      { ResultSelectionMask, ColorTarget } });
    // The final opaque depth into the window-sized UI depth, in the world view, which the
    // UI's world part then tests against, as OpenGL's render_hud_elements() does.
    graph.addPass("ui depth", { { ResultDepth, FragmentRead | Keep }, { ResultUIDepth, DepthTarget } });
    graph.addPass("read counts", { { ResultDrawArguments, CopyRead }, { ResultSceneCounts, CopyWrite } });

    // The audit runs last, and reuses the lists: nothing after it reads them. All of it
    // goes when its sink is off, so a normal frame pays nothing for it.
    graph.addPass("audit reset", { { ResultDrawArguments, CopyWrite | Keep } });
    graph.addPass("audit cull", { { ResultSceneData, ComputeRead },
                                  { ResultDrawArguments, ComputeWrite | Keep },
                                  { ResultVisibleClusters, ComputeWrite | Keep },
                                  { ResultGroupList, ComputeWrite | Keep } });
    graph.addPass("audit groups", { { ResultSceneData, ComputeRead },
                                    { ResultGroupList, ComputeRead },
                                    { ResultDrawArguments, Indirect | ComputeWrite | Keep },
                                    { ResultVisibleClusters, ComputeWrite | Keep } });
    graph.addPass("audit draw", { { ResultSceneData, VertexRead },
                                  { ResultDrawArguments, Indirect },
                                  { ResultVisibleClusters, VertexRead },
                                  { ResultDepth, DepthTarget | Keep },
                                  { ResultAuditVisibility, ColorTarget } });
    graph.addPass("audit read", { { ResultAuditVisibility, CopyRead }, { ResultAuditPixels, CopyWrite } });

    // Only on the last frame of a feedback window, when its sink is on.
    graph.addPass("read feedback", { { ResultTextureFeedback, CopyRead }, { ResultFeedbackReadback, CopyWrite } });

    graph.setSink(ResultSceneColor, FragmentRead, true);
    graph.setSink(ResultSceneCounts, CopyRead, true);
}

void declareMacOIT(FrameGraph& graph)
{
    using namespace GraphAccess;

    // Keys: the blended lists, early and late, against the opaque depth, which none of the
    // three passes writes. Fragments also flag a tail in the counts.
    graph.addPass("macoit keys", { { ResultSceneData, VertexRead | FragmentRead },
                                   { ResultDrawArguments, Indirect | FragmentWrite | Keep },
                                   { ResultVisibleClusters, VertexRead },
                                   { ResultDepth, DepthTarget | Keep },
                                   { ResultMacOITState, FragmentRead | FragmentWrite | Keep } });
    // Empties the tail pass's draws when no pixel has a tail.
    graph.addPass("macoit gate", { { ResultDrawArguments, ComputeRead | ComputeWrite | Keep } });
    graph.addPass("macoit tail", { { ResultSceneData, VertexRead | FragmentRead },
                                   { ResultDrawArguments, Indirect },
                                   { ResultVisibleClusters, VertexRead },
                                   { ResultDepth, DepthTarget | Keep },
                                   { ResultMacOITState, FragmentRead },
                                   { ResultMacOITMoments, ColorTarget },
                                   { ResultMacOITTailDepth, ColorTarget } });
    graph.addPass("macoit color", { { ResultSceneData, VertexRead | FragmentRead },
                                    { ResultDrawArguments, Indirect },
                                    { ResultVisibleClusters, VertexRead },
                                    { ResultDepth, DepthTarget | Keep },
                                    { ResultMacOITState, FragmentRead },
                                    { ResultMacOITMoments, FragmentRead },
                                    { ResultMacOITTailDepth, FragmentRead },
                                    { ResultMacOITColor, ColorTarget },
                                    { ResultMacOITWeights, ColorTarget },
                                    { ResultTextureFeedback, FragmentWrite | Keep } });
    graph.addPass("macoit resolve", { { ResultLit, ComputeRead },
                                      { ResultMacOITColor, ComputeRead },
                                      { ResultMacOITWeights, ComputeRead },
                                      { ResultMacOITState, ComputeRead | ComputeWrite | Keep },
                                      { ResultSceneColor, ComputeWrite } });
}

} // namespace asr
