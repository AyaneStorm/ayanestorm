/**
 * @file asrscenegraph.cpp
 * asrscenegraph.cpp
 * See asrscenegraph.h.
 * @author chanayane@ayanestorm
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (C) 2026, Ayane Lyla
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
 *
 * $/LicenseInfo$
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
    graph.addResult("transmittance lut", ResultScope::Frame);
    graph.addResult("multiscatter lut", ResultScope::Frame);
    graph.addResult("sky view lut", ResultScope::Frame);
    graph.addResult("aerial lut", ResultScope::Frame);
    graph.addResult("exposure", ResultScope::Frame);
    graph.addResult("bloom", ResultScope::Frame);
    graph.addResult("resolve tiles", ResultScope::Frame);
    graph.addResult("shadow views", ResultScope::Persistent); // last frame's windows place this frame's cache
    graph.addResult("shadow lists", ResultScope::Frame);
    graph.addResult("sun shadow", ResultScope::Persistent); // read as is between updates (ASRendererShadowInterval)
    graph.addResult("shadow cache", ResultScope::Persistent);
    graph.addResult("sun opacity", ResultScope::Persistent);
    graph.addResult("opacity cache", ResultScope::Persistent);
    graph.addResult("instance lods", ResultScope::Persistent);
    graph.addResult("contact shadows", ResultScope::Frame);
    graph.addResult("sun shadow mask", ResultScope::Frame);

    // The group passes read their dispatch size from the counts, so they take the draw
    // arguments as Indirect too.
    graph.addPass("upload", { { ResultSceneData, CopyWrite | Keep }, { ResultDrawArguments, CopyWrite } });

    // Joint-derived conservative bounds need no deformed positions. Culling can therefore
    // decide demand before spending vertex bandwidth; late depth rejection has recovery.
    for (const char* name : { "skin pose", "skin palette", "skin bind", "skin morph", "skin clusters", "skin groups" })
    {
        graph.addPass(name, { { ResultSceneData, ComputeRead | ComputeWrite | Keep } });
    }
    // The cascades, before the cull that reads them.
    graph.addPass("shadow fit", { { ResultShadowViews, ComputeRead | ComputeWrite | Keep } });
    // What the cached maps show of each texture, into the texture feedback (asrShadowDemandC).
    graph.addPass("shadow demand", { { ResultShadowViews, ComputeRead | ComputeWrite | Keep },
                                     { ResultTextureFeedback, ComputeWrite | Keep } });
    // Each instance's LOD, read by every cull below; a changed one marks its cached shadow dirty.
    graph.addPass("lod select", { { ResultSceneData, ComputeRead },
                                  { ResultShadowViews, ComputeRead | ComputeWrite | Keep },
                                  { ResultInstanceLods, ComputeRead | ComputeWrite | Keep } });
    // Shadow views are culled in the same threads as the main view: see asrscene.h.
    graph.addPass("cull early", { { ResultSceneData, ComputeRead | ComputeWrite | Keep },
                                  { ResultInstanceLods, ComputeRead },
                                  { ResultDepthPyramid, ComputeRead | PreviousFrame },
                                  { ResultShadowViews, ComputeRead },
                                  { ResultDrawArguments, ComputeWrite | Keep },
                                  { ResultVisibleClusters, ComputeWrite },
                                  { ResultRetestList, ComputeWrite },
                                  { ResultGroupList, ComputeWrite },
                                  { ResultSelectionList, ComputeWrite },
                                  { ResultShadowLists, ComputeWrite } });
    graph.addPass("groups early", { { ResultSceneData, ComputeRead | ComputeWrite | Keep },
                                    { ResultDepthPyramid, ComputeRead | PreviousFrame },
                                    { ResultGroupList, ComputeRead },
                                    { ResultDrawArguments, Indirect | ComputeWrite | Keep },
                                    { ResultVisibleClusters, ComputeWrite | Keep },
                                    { ResultRetestList, ComputeWrite | Keep } });
    graph.addPass("shadow groups", { { ResultSceneData, ComputeRead | ComputeWrite | Keep },
                                     { ResultShadowViews, ComputeRead },
                                     { ResultDrawArguments, Indirect | ComputeWrite | Keep },
                                     { ResultShadowLists, ComputeRead | ComputeWrite | Keep } });
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
                                 { ResultInstanceLods, ComputeRead },
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
    // Moving casters after the final depth (asrShadowReceiversC.glsl): drawn into a cascade
    // only where a visible pixel reads it, then skinned if nothing else asked.
    graph.addPass("shadow receivers", { { ResultDepth, ComputeRead | Keep },
                                        { ResultShadowViews, ComputeRead | ComputeWrite | Keep } });
    graph.addPass("shadow moving cull", { { ResultSceneData, ComputeRead | ComputeWrite | Keep },
                                          { ResultInstanceLods, ComputeRead },
                                          { ResultShadowViews, ComputeRead },
                                          { ResultDrawArguments, ComputeWrite | Keep },
                                          { ResultShadowLists, ComputeWrite | Keep } });
    graph.addPass("shadow moving groups", { { ResultSceneData, ComputeRead | ComputeWrite | Keep },
                                            { ResultShadowViews, ComputeRead },
                                            { ResultDrawArguments, Indirect | ComputeWrite | Keep },
                                            { ResultShadowLists, ComputeRead | ComputeWrite | Keep } });
    graph.addPass("skin vertices shadow", { { ResultSceneData, ComputeRead | ComputeWrite | Keep } });
    // The static cache (see asrShadowShiftF.glsl): shifted into the working map, static
    // casters drawn into its dirty tiles, copied back, then moving casters on top.
    graph.addPass("shadow shift", { { ResultShadowViews, FragmentRead },
                                    { ResultShadowCache, FragmentRead | PreviousFrame },
                                    { ResultOpacityCache, FragmentRead | PreviousFrame },
                                    { ResultSunShadow, DepthTarget | Keep },
                                    { ResultSunOpacity, ColorTarget | Keep } });
    // Masked casters add the texture levels they read to GpuShadowViews::demand.
    graph.addPass("shadow draw", { { ResultSceneData, VertexRead },
                                   { ResultDrawArguments, Indirect },
                                   { ResultShadowViews, VertexRead | FragmentRead | FragmentWrite | Keep },
                                   { ResultShadowLists, VertexRead },
                                   { ResultSunShadow, DepthTarget | Keep },
                                   { ResultSunOpacity, ColorTarget | Keep } });
    // Also clears the dirty tiles it has drawn: marks gathered until the next update add up.
    graph.addPass("shadow store", { { ResultSunShadow, CopyRead | Keep },
                                    { ResultShadowCache, CopyWrite },
                                    { ResultSunOpacity, CopyRead | Keep },
                                    { ResultOpacityCache, CopyWrite },
                                    { ResultShadowViews, CopyWrite | Keep } });
    graph.addPass("shadow moving", { { ResultSceneData, VertexRead },
                                     { ResultDrawArguments, Indirect },
                                     { ResultShadowViews, VertexRead | FragmentRead | FragmentWrite | Keep },
                                     { ResultShadowLists, VertexRead },
                                     { ResultSunShadow, DepthTarget | Keep },
                                     { ResultSunOpacity, ColorTarget | Keep } });
    // The sky's tables, rebuilt every frame since EEP skies blend: each reads the ones before.
    graph.addPass("transmittance lut", { { ResultTransmittanceLut, ComputeWrite } });
    graph.addPass("multiscatter lut", { { ResultTransmittanceLut, ComputeRead }, { ResultMultiscatterLut, ComputeWrite } });
    graph.addPass("sky view lut", { { ResultTransmittanceLut, ComputeRead },
                                    { ResultMultiscatterLut, ComputeRead },
                                    { ResultSkyViewLut, ComputeWrite } });
    graph.addPass("aerial lut", { { ResultTransmittanceLut, ComputeRead },
                                  { ResultMultiscatterLut, ComputeRead },
                                  { ResultAerialLut, ComputeWrite } });
    graph.addPass("exposure", { { ResultTransmittanceLut, ComputeRead }, { ResultSkyViewLut, ComputeRead }, { ResultExposure, ComputeWrite } });
    // Two dispatches: the work list (light's point, bands, rays), then the rays over it.
    graph.addPass("contact shadows", { { ResultDepth, ComputeRead | Keep },
                                       { ResultExposure, ComputeRead },
                                       { ResultShadowViews, ComputeRead },
                                       { ResultContactShadows, ComputeWrite },
                                       { ResultSunShadowMask, ComputeWrite } });
    // The tiles' counts are cleared inside this pass, before its dispatch.
    graph.addPass("classify", { { ResultSceneData, ComputeRead },
                                { ResultVisibleClusters, ComputeRead },
                                { ResultVisibility, ComputeRead },
                                { ResultResolveTiles, ComputeWrite } });
    graph.addPass("resolve", { { ResultSceneData, ComputeRead },
                               { ResultSunShadowMask, ComputeRead },
                               { ResultDepth, ComputeRead | Keep }, // where the mask was written
                               { ResultVisibleClusters, ComputeRead },
                               { ResultVisibility, ComputeRead },
                               { ResultResolveTiles, Indirect | ComputeRead },
                               { ResultTransmittanceLut, ComputeRead },
                               { ResultSkyViewLut, ComputeRead },
                               { ResultAerialLut, ComputeRead },
                               { ResultExposure, ComputeRead },
                               { ResultShadowViews, ComputeRead },
                               { ResultSunShadow, ComputeRead },
                               { ResultSunOpacity, ComputeRead },
                               { ResultNormals, ComputeWrite },
                               { ResultLit, ComputeWrite },
                               { ResultTextureFeedback, ComputeWrite | Keep } });
    // The stars, added to the lit image where the depth is still the far plane, before the
    // blended layers go over it.
    graph.addPass("stars", { { ResultTransmittanceLut, FragmentRead },
                             { ResultSkyViewLut, FragmentRead }, // the sky behind each star fades it
                             { ResultExposure, VertexRead | FragmentRead },
                             { ResultDepth, DepthTarget | Keep },
                             { ResultLit, ColorTarget | Keep },
                             { ResultTextureFeedback, FragmentWrite | Keep } });
    declareMacOIT(graph);
    // Bloom and SL glow from the scene colour: all its levels, down then up, in one pass.
    graph.addPass("bloom", { { ResultSceneColor, ComputeRead },
                             { ResultExposure, ComputeRead },
                             { ResultBloom, ComputeWrite } });
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
                                  { ResultInstanceLods, ComputeRead },
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
    graph.setSink(ResultExposure, FragmentRead, true); // the composite exposes scene colour by it
    graph.setSink(ResultBloom, FragmentRead, true);
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
                                   { ResultExposure, FragmentRead }, // the shared fragment code shades with it
                                   { ResultMacOITState, FragmentRead | FragmentWrite | Keep } });
    // Empties the tail pass's draws when no pixel has a tail.
    graph.addPass("macoit gate", { { ResultDrawArguments, ComputeRead | ComputeWrite | Keep } });
    graph.addPass("macoit tail", { { ResultSceneData, VertexRead | FragmentRead },
                                   { ResultDrawArguments, Indirect },
                                   { ResultVisibleClusters, VertexRead },
                                   { ResultDepth, DepthTarget | Keep },
                                   { ResultMacOITState, FragmentRead },
                                   { ResultExposure, FragmentRead },
                                   { ResultMacOITMoments, ColorTarget },
                                   { ResultMacOITTailDepth, ColorTarget } });
    graph.addPass("macoit color", { { ResultSceneData, VertexRead | FragmentRead },
                                    { ResultDrawArguments, Indirect },
                                    { ResultVisibleClusters, VertexRead },
                                    { ResultDepth, DepthTarget | Keep },
                                    { ResultMacOITState, FragmentRead },
                                    { ResultMacOITMoments, FragmentRead },
                                    { ResultMacOITTailDepth, FragmentRead },
                                    { ResultExposure, FragmentRead },
                                    { ResultAerialLut, FragmentRead },
                                    { ResultShadowViews, FragmentRead },
                                    { ResultSunShadow, FragmentRead },
                                    { ResultSunOpacity, FragmentRead },
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
