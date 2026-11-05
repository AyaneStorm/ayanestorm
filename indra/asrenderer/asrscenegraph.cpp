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

    graph.addPass("upload", { { ResultSceneData, CopyWrite | Keep }, { ResultDrawArguments, CopyWrite } });
    graph.addPass("cull early", { { ResultSceneData, ComputeRead },
                                  { ResultDepthPyramid, ComputeRead | PreviousFrame },
                                  { ResultDrawArguments, ComputeWrite | Keep },
                                  { ResultVisibleClusters, ComputeWrite },
                                  { ResultRetestList, ComputeWrite } });
    graph.addPass("draw early", { { ResultSceneData, VertexRead },
                                  { ResultDrawArguments, Indirect },
                                  { ResultVisibleClusters, VertexRead },
                                  { ResultVisibility, ColorTarget },
                                  { ResultDepth, DepthTarget } });
    graph.addPass("pyramid early", { { ResultDepth, ComputeRead }, { ResultDepthPyramid, ComputeWrite } });
    graph.addPass("cull late", { { ResultSceneData, ComputeRead },
                                 { ResultDepthPyramid, ComputeRead },
                                 { ResultRetestList, ComputeRead },
                                 { ResultDrawArguments, Indirect | ComputeWrite | Keep },
                                 { ResultVisibleClusters, ComputeWrite | Keep } });
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
                               { ResultNormals, ComputeWrite } });
    graph.addPass("read counts", { { ResultDrawArguments, CopyRead }, { ResultSceneCounts, CopyWrite } });

    // The audit runs last, and reuses the lists: nothing after it reads them. All of it
    // goes when its sink is off, so a normal frame pays nothing for it.
    graph.addPass("audit reset", { { ResultDrawArguments, CopyWrite | Keep } });
    graph.addPass("audit cull", { { ResultSceneData, ComputeRead },
                                  { ResultDrawArguments, ComputeWrite | Keep },
                                  { ResultVisibleClusters, ComputeWrite | Keep } });
    graph.addPass("audit draw", { { ResultSceneData, VertexRead },
                                  { ResultDrawArguments, Indirect },
                                  { ResultVisibleClusters, VertexRead },
                                  { ResultDepth, DepthTarget | Keep },
                                  { ResultAuditVisibility, ColorTarget } });
    graph.addPass("audit read", { { ResultAuditVisibility, CopyRead }, { ResultAuditPixels, CopyWrite } });

    graph.setSink(ResultAlbedo, FragmentRead, true);
    graph.setSink(ResultNormals, FragmentRead, true);
    graph.setSink(ResultSceneCounts, CopyRead, true);
}

} // namespace asr
