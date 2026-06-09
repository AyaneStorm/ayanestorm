/**
 * @file lldrawpoolalpha.h
 * @brief LLDrawPoolAlpha class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLDRAWPOOLALPHA_H
#define LL_LLDRAWPOOLALPHA_H

#include "lldrawpool.h"
#include "llrender.h"
#include "llframetimer.h"

class LLFace;
class LLColor4;
class LLGLSLShader;

class LLDrawPoolAlpha final: public LLRenderPass
{
public:

    // set by llsettingsvo so lldrawpoolalpha has quick access to the water plane in eye space
    static LLVector4 sWaterPlane;

    enum
    {
        VERTEX_DATA_MASK =  LLVertexBuffer::MAP_VERTEX |
                            LLVertexBuffer::MAP_NORMAL |
                            LLVertexBuffer::MAP_COLOR |
                            LLVertexBuffer::MAP_TEXCOORD0
    };
    virtual U32 getVertexDataMask() { return VERTEX_DATA_MASK; }

    LLDrawPoolAlpha(U32 type);
    /*virtual*/ ~LLDrawPoolAlpha();

    /*virtual*/ S32 getNumPostDeferredPasses();
    /*virtual*/ void renderPostDeferred(S32 pass);
    /*virtual*/ S32  getNumPasses() { return 1; }

    // <AS:Chanayane> inspired from the work of Mayatonton in AYAstorm
    // 3-pass dispatch: discriminates SIM-rezzed vs attachment non-rigged batches
    // so that attachment N-BL prims are drawn after rigged hair (pass 3), not before (pass 1).
    enum AttachmentFilter
    {
        ATTACHMENT_ALL,
        ATTACHMENT_NONE,
        ATTACHMENT_ONLY,
        ATTACHMENT_POST_WBOIT_LEGACY,
        ATTACHMENT_POST_WBOIT_OTHER_RIGGED,
        ATTACHMENT_POST_WBOIT_SELF_RIGGED
    };
    // </AS:Chanayane>

    void forwardRender(bool rigged = false, AttachmentFilter filter = ATTACHMENT_ALL);
    /*virtual*/ void prerender();

    void renderDebugAlpha();

    void renderGroupAlpha(LLSpatialGroup* group, U32 type, U32 mask, bool texture = true);

    // <AS:Chanayane> inspired from the work of Mayatonton in AYAstorm
    //void renderAlpha(U32 mask, bool depth_only = false, bool rigged = false);
    void renderAlpha(U32 mask, bool depth_only = false, bool rigged = false, AttachmentFilter filter = ATTACHMENT_ALL);
    // </AS:Chanayane>
    void renderAlphaHighlight();

    static bool sShowDebugAlpha;
    static bool sShowDebugAlphaRigged;
    // <AS:Chanayane> WBOIT — set true when WBOIT accumulation ran this frame; composite checks this
    static bool sWBOITRendered;
    // Clear the shared WBOIT MRT once per frame before the first alpha pool contributes.
    static bool sWBOITClearNeeded;
    // Draw skipped WBOIT-sensitive alpha batches with the legacy path after composite.
    static bool sPostWBOITLegacyPass;

private:
    LLGLSLShader* target_shader;

    // setup by beginFooPass, [0] is static variant, [1] is rigged variant
    LLGLSLShader* simple_shader = nullptr;
    LLGLSLShader* fullbright_shader = nullptr;
    LLGLSLShader* emissive_shader = nullptr;
    LLGLSLShader* pbr_emissive_shader = nullptr;
    LLGLSLShader* pbr_shader = nullptr;

    void drawEmissive(LLDrawInfo* draw);
    void renderEmissives(std::vector<LLDrawInfo*>& emissives);
    void renderRiggedEmissives(std::vector<LLDrawInfo*>& emissives);
    void renderPbrEmissives(std::vector<LLDrawInfo*>& emissives);
    void renderRiggedPbrEmissives(std::vector<LLDrawInfo*>& emissives);
    bool TexSetup(LLDrawInfo* draw, bool use_material);
    void RestoreTexSetup(bool tex_setup);

    // our 'normal' alpha blend function for this pass
    LLRender::eBlendFactor mColorSFactor;
    LLRender::eBlendFactor mColorDFactor;
    LLRender::eBlendFactor mAlphaSFactor;
    LLRender::eBlendFactor mAlphaDFactor;

    // if true, we're executing a rigged render pass
    bool mRigged = false;

    // <AS:Chanayane> WBOIT — true while accumulation passes are active
    bool mForwardToWBOIT = false;
    // </AS:Chanayane>
};

#endif // LL_LLDRAWPOOLALPHA_H
