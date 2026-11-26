/**
 * @file asrscenegraph.h
 * asrscenegraph.h
 * The scene's passes and results, declared once for both backends. A backend only
 * gives each pass its body, and turns transitions into whatever its API needs.
 *
 * Two-phase occlusion: the first phase tests clusters against last frame's depth and
 * draws what passes, which is nearly everything that will show. A pyramid of the new
 * depth is built, and what the first phase hid is tested again against it. So the guess
 * from last frame can cost extra drawing, but can never lose anything.
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
    ResultSelectionList,  // the early cull's clusters of selected instances
    ResultSelectionMask,  // where selections are, for the outline: see asrOverlayMaskF.glsl
    ResultUIDepth,        // window-sized: the scene depth in the world view, for the UI's world part
    ResultTransmittanceLut, // the sky's tables, made every frame: see asratmosphere.h
    ResultMultiscatterLut,
    ResultSkyViewLut,
    ResultAerialLut,      // the aerial perspective volume, along the main view's rays
    ResultExposure,      // GpuExposure, from the sky: read by shading and the composite
    ResultBloom,         // bloom's levels in one atlas, read by the composite
    ResultResolveTiles,  // GpuResolveTiles: the resolve's tiles and dispatches by material kind
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
    PassTransmittanceLut,
    PassMultiscatterLut,
    PassSkyViewLut,
    PassAerialLut,
    PassExposure,
    PassClassify,      // the resolve's tiles by material kind
    PassResolve,       // one indirect dispatch per kind
    PassStars,         // only while the sky has stars to show
    PassMacOITKeys,   // Mac OIT, declared by declareMacOIT()
    PassMacOITGate,
    PassMacOITTail,
    PassMacOITColor,
    PassMacOITResolve,
    PassBloom,
    PassSelectionMask, // only while something is selected: the mask's sink is off otherwise
    PassUIDepth,       // only while the UI's world part tests depth: see asruilist.h
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
