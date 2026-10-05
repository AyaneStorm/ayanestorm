/**
 * @file asdoflive.cpp
 * @author chanayane@firestorm
 * @brief AyaneStorm Live depth of field (ASDepthOfFieldMode 3).
 *
 * Passes (all OpenGL 4.1 fragment, see doc/ayanestorm-depth-of-field-live-plan.md):
 *   1. reduce     full resolution to the half-resolution bin sums, one pass
 *                 (N2, N1, energies; F, B1, B2, visibility), then mip chains;
 *   2. complete   the bins behind N2 completed where nearer bins hide them
 *                 (push-pull), one draw per mip level, top down;
 *   3. tiles      veil kernel radius (N2, N1, B1) per 8x8 gather-pixel tile:
 *                 reduce, then dilation along x and y;
 *   4. gathers    area-tap scatter-as-gather of N2, N1, B1 (tile kernel) and
 *                 B2 (own kernel, hole fill); N1 is written over N2, B1 over
 *                 B2;
 *   5. composite  full resolution, N2 over N1 over F over B1 over B2.
 * Highlight sprites (ASDepthOfFieldHighlightSprites): before the reduce,
 * isolated defocused lights leave the bin colour (mode 1's extraction,
 * asDepthOfFieldHighlightF.glsl under LIVE_SPRITES); after the gathers they
 * are drawn as aperture sprites into the background and the veil
 * (asDepthOfFieldSpriteV/F.glsl). A light smaller than the tap spacing kept
 * a ring pattern in the gather.
 * Bins come from two sources:
 *   - transparency bins: Mac OIT's COLOR pass adds every transparent
 *     fragment, with its exact weight, to the bins of its own depth
 *     (asMacOITCaptureF.glsl, MACOIT_DOF). The opaque colour and depth are
 *     kept before the alpha pool (prepareCapture()) and binned by T, the
 *     transmittance in front of them;
 *   - fallback: the composited image binned by the depth buffer, one surface
 *     per pixel (first frame, or no capture).
 */
#include "llviewerprecompiledheaders.h"

#include "asdoflive.h"

#include <cmath>

#include "asbackgroundisolate.h"
#include "asdepthoffield.h"
#include "asdofaperture.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
    LLGLSLShader sReduceProgram;
    LLGLSLShader sCompleteProgram;
    LLGLSLShader sTileProgram;
    LLGLSLShader sGatherProgram;
    LLGLSLShader sCompositeProgram;
    // Highlight sprites (optional: Live runs without them).
    LLGLSLShader sHighlightProgram;
    LLGLSLShader sSpriteProgram;

    // Bin sums at gather resolution, mipmapped (asDoFLiveReduceF.glsl):
    // A = N2, N1, radius moments; B = F, B1, B2, visibility. Written in one
    // pass through sReduceFBO (seven draw buffers).
    LLRenderTarget sBinsA;
    LLRenderTarget sBinsB;
    GLuint sReduceFBO = 0;
    const U32 BIN_TEXTURES = 7;
    // Completed bins, mipmapped (asDoFLiveCompleteF.glsl): A = N1, F, B1,
    // B2; B = energies (E_N1, E_B1, M_B2). Written level by level through
    // sCompleteFBO.
    LLRenderTarget sDoneA;
    LLRenderTarget sDoneB;
    GLuint sCompleteFBO = 0;
    const U32 DONE_TEXTURES = 5;
    // Tile kernel radii: reduce, dilated along x, dilated along y.
    LLRenderTarget sTileReduce;
    LLRenderTarget sTileX;
    LLRenderTarget sTileY;
    // Gathered layers at gather resolution: N2, then N2 over N1 (the
    // foreground veil); B2, then B1 over B2 (the background).
    LLRenderTarget sNear2;
    LLRenderTarget sNear1;
    LLRenderTarget sFar;
    LLRenderTarget sBack;
    // Opaque scene before the post-water alpha pool, for the bins path.
    LLRenderTarget sOpaqueColor;
    LLRenderTarget sOpaqueDepth;
    // Highlight cells (asDepthOfFieldHighlightF.glsl): energy + occupancy,
    // centroid + signed CoC, brightness levels 0-3 and 4-7; mipmapped for
    // the sprite budget. The bin colour with the kept lights removed.
    LLRenderTarget sCellTarget;
    LLRenderTarget sHighlightInput;
    bool sOpaqueReady = false;

    // Lens of the last Live frame per image height (resolution independent),
    // for the next frame's transparency capture (transparencyLens()).
    struct CaptureLens
    {
        bool mValid = false;
        F32 mFocalDistance = 0.f;
        F32 mBlurConstant = 0.f;
        F32 mMagnification = 0.f;
        F32 mTanPixelAngleHeight = 0.f;  // tan_pixel_angle * height
        F32 mMaxCoCPerHeight = 0.f;      // max_coc / height
        F32 mNearScale = 0.f;            // near radius / max_coc
        F32 mFarScale = 0.f;             // far radius / max_coc
    };
    CaptureLens sCaptureLens;

    U32 sWidth = 0;
    U32 sHeight = 0;

    // Gather pixels per tile side (TILE in asDoFLiveTileF.glsl and
    // asDoFLiveGatherF.glsl).
    const U32 TILE = 8;
    // Ring counts of the quality presets: 37, 91, 169 taps.
    const S32 QUALITY_RINGS[] = { 3, 5, 7 };
    // Tap table of the gather (LIVE_TAP_TABLE, live_taps[] in
    // asDoFLiveGatherF.glsl): the ring taps of the 7-ring pattern, 6 k on
    // ring k, the centre excluded. Every ring count reads its prefix.
    const S32 TABLE_RINGS = 7;
    const S32 TABLE_TAPS = 3 * TABLE_RINGS * (TABLE_RINGS + 1);
    // Gather linked with the tap table; false: the per-tap fallback.
    bool sGatherTable = false;
    constexpr F64 PI_D = 3.14159265358979323846;
    // Full-resolution pixels per highlight cell side (CELL_SIZE in
    // asDepthOfFieldHighlightF.glsl).
    const U32 HIGHLIGHT_CELL_SIZE = 8;
    // Dilation loop bound of asDoFLiveTileF.glsl.
    const S32 MAX_TILE_REACH = 64;

    const char* const COMMON_LIBRARY = "deferred/asDoFLiveCommonF.glsl";

    const LLStaticHashedString U_TARGET_RES("target_res");
    const LLStaticHashedString U_FOCAL_DISTANCE("focal_distance");
    const LLStaticHashedString U_BLUR_CONSTANT("blur_constant");
    const LLStaticHashedString U_TAN_PIXEL_ANGLE("tan_pixel_angle");
    const LLStaticHashedString U_MAGNIFICATION("magnification");
    const LLStaticHashedString U_MAX_COC("max_coc");
    const LLStaticHashedString U_NEAR_RADIUS("near_radius");
    const LLStaticHashedString U_FAR_RADIUS("far_radius");
    const LLStaticHashedString U_SPLIT_RADIUS("split_radius");
    const LLStaticHashedString U_FAR_SPLIT_RADIUS("far_split_radius");
    const LLStaticHashedString U_FIELD_SCALE("field_scale");
    const LLStaticHashedString U_FIELD_CURVATURE("field_curvature");
    const LLStaticHashedString U_SA_STRENGTH("sa_strength");
    const LLStaticHashedString U_CAT_EYE("cat_eye");
    const LLStaticHashedString U_VIGNETTE_SHIFT("vignette_shift");
    const LLStaticHashedString U_CA_SHIFT("ca_shift");
    const LLStaticHashedString U_CA_REACH("ca_reach");
    const LLStaticHashedString U_TILE_PASS("tile_pass");
    const LLStaticHashedString U_TILE_REACH("tile_reach");
    const LLStaticHashedString U_LAYER("layer");
    const LLStaticHashedString U_MAX_RINGS("max_rings");
    const LLStaticHashedString U_LEVEL("level");
    const LLStaticHashedString U_TOP_LEVEL("top_level");
    const LLStaticHashedString U_APERTURE_BLADES("aperture_blades");
    const LLStaticHashedString U_APERTURE_ROUNDNESS("aperture_roundness");
    const LLStaticHashedString U_APERTURE_ROTATION("aperture_rotation");
    const LLStaticHashedString U_ANAMORPHIC_RATIO("anamorphic_ratio");
    const LLStaticHashedString U_UNIT_AREA("unit_area");
    const LLStaticHashedString U_DEBUG_MODE("debug_mode");
    const LLStaticHashedString U_BINS_SOURCE("bins_source");
    const LLStaticHashedString U_GATHER_SCALE("gather_scale");
    const LLStaticHashedString U_LIVE_TAPS("live_taps");
    const LLStaticHashedString U_MAX_RADIUS("max_radius");
    const LLStaticHashedString U_NEAR_MAX_RADIUS("near_max_radius");
    const LLStaticHashedString U_ISOLATION("isolation");
    const LLStaticHashedString U_HIGHLIGHT_PASS("highlight_pass");
    const LLStaticHashedString U_CELL_GRID("cell_grid");
    const LLStaticHashedString U_CELL_TOP_LEVEL("cell_top_level");
    const LLStaticHashedString U_SPRITE_BUDGET("sprite_budget");
    const LLStaticHashedString U_PLANE("plane");
    const LLStaticHashedString U_LIVE_PART("live_part");
    const LLStaticHashedString U_HL_STRENGTH("hl_strength");
    const LLStaticHashedString U_HL_THRESHOLD("hl_threshold");

    // This frame's lens values, uploaded to every program that links the
    // common library.
    struct Lens
    {
        F32 mFocalDistance = 0.f;
        F32 mBlurConstant = 0.f;
        F32 mTanPixelAngle = 0.f;
        F32 mMagnification = 0.f;
        F32 mMaxCoC = 0.f;
        F32 mNearRadius = 0.f;
        F32 mFarRadius = 0.f;
        F32 mSplitRadius = 0.f;
        F32 mFarSplitRadius = 0.f;
        // Lens effects shared with the Advanced renderer.
        ASDepthOfField::LensField mField;
        ASDoFAperture::Shape mShape;
        F32 mUnitArea = F_PI;
        F32 mGatherScale = 0.5f;  // full-resolution to gather pixels
    };

    // Each of the two bins on a side of the focus spans the same radius
    // ratio from 2 px up to that side's largest radius.
    F32 splitRadius(F32 side_radius)
    {
        return llmax(sqrtf(2.f * side_radius), 2.5f);
    }

    // Live DoF selected: its transparency bins are always wanted (one layer
    // blurred transparent surfaces at the depth behind them; it was a
    // setting until 2026-10-06).
    bool binsWanted()
    {
        static LLCachedControl<S32> mode(gSavedSettings, "ASDepthOfFieldMode", 0);
        return S32(mode) == ASDoFLive::LIVE_MODE && LLPipeline::RenderDepthOfField &&
            sCaptureLens.mValid && !gCubeSnapshot && !ASBackgroundIsolate::isActive();
    }

    // Binds a raw GL texture to the unit a reserved sampler got at link.
    void bindRaw(LLGLSLShader& shader, S32 uniform, GLuint texture)
    {
        const S32 unit = shader.getTextureChannel(uniform);
        if (unit >= 0)
        {
            gGL.getTexUnit(unit)->bindManual(LLTexUnit::TT_TEXTURE, texture);
        }
    }

    void unbindRaw(LLGLSLShader& shader, S32 uniform)
    {
        const S32 unit = shader.getTextureChannel(uniform);
        if (unit >= 0)
        {
            gGL.getTexUnit(unit)->unbind(LLTexUnit::TT_TEXTURE);
        }
    }

    // Mac OIT bins (shadowMap0..5) and its weight / optical depth sums
    // (positionMap), as declared in asDoFLiveCommonF.glsl.
    const S32 BIN_SAMPLERS[] = {
        LLShaderMgr::DEFERRED_SHADOW0, LLShaderMgr::DEFERRED_SHADOW1,
        LLShaderMgr::DEFERRED_SHADOW2, LLShaderMgr::DEFERRED_SHADOW3,
        LLShaderMgr::DEFERRED_SHADOW4, LLShaderMgr::DEFERRED_SHADOW5 };
    static_assert(sizeof(BIN_SAMPLERS) / sizeof(BIN_SAMPLERS[0]) == ASMacOIT::DOF_BIN_TEXTURES,
                  "one sampler per Mac OIT bin texture");

    void bindBins(LLGLSLShader& shader, bool bins)
    {
        shader.uniform1i(U_BINS_SOURCE, bins ? 1 : 0);
        if (!bins)
        {
            return;
        }
        for (U32 i = 0; i < ASMacOIT::DOF_BIN_TEXTURES; ++i)
        {
            bindRaw(shader, BIN_SAMPLERS[i], ASMacOIT::dofBinTexture(i));
        }
        bindRaw(shader, LLShaderMgr::DEFERRED_POSITION, ASMacOIT::dofWeightTexture());
    }

    void unbindBins(LLGLSLShader& shader, bool bins)
    {
        if (!bins)
        {
            return;
        }
        unbindRaw(shader, LLShaderMgr::DEFERRED_POSITION);
        for (S32 uniform : BIN_SAMPLERS)
        {
            unbindRaw(shader, uniform);
        }
    }

    void setLensUniforms(LLGLSLShader& shader, const Lens& lens)
    {
        shader.uniform1f(U_FOCAL_DISTANCE, lens.mFocalDistance);
        shader.uniform1f(U_BLUR_CONSTANT, lens.mBlurConstant);
        shader.uniform1f(U_TAN_PIXEL_ANGLE, lens.mTanPixelAngle);
        shader.uniform1f(U_MAGNIFICATION, lens.mMagnification);
        shader.uniform1f(U_MAX_COC, lens.mMaxCoC);
        shader.uniform1f(U_NEAR_RADIUS, lens.mNearRadius);
        shader.uniform1f(U_FAR_RADIUS, lens.mFarRadius);
        shader.uniform1f(U_SPLIT_RADIUS, lens.mSplitRadius);
        shader.uniform1f(U_FAR_SPLIT_RADIUS, lens.mFarSplitRadius);
        shader.uniform2f(U_FIELD_SCALE, lens.mField.mFieldScale[0], lens.mField.mFieldScale[1]);
        shader.uniform1f(U_FIELD_CURVATURE, lens.mField.mCurvature);
        shader.uniform1f(U_SA_STRENGTH, lens.mField.mSpherical);
        shader.uniform1f(U_CAT_EYE, lens.mField.mCatEye);
        shader.uniform1f(U_VIGNETTE_SHIFT, lens.mField.mVignette);
        shader.uniform1f(U_CA_SHIFT, lens.mField.mAxialCA);
        shader.uniform1i(U_APERTURE_BLADES, lens.mShape.mBlades);
        shader.uniform1f(U_APERTURE_ROUNDNESS, lens.mShape.mRoundness);
        shader.uniform1f(U_APERTURE_ROTATION, lens.mShape.mRotation);
        shader.uniform1f(U_ANAMORPHIC_RATIO, lens.mShape.mAnamorphic);
        shader.uniform1f(U_UNIT_AREA, lens.mUnitArea);
        shader.uniform1f(U_GATHER_SCALE, lens.mGatherScale);
    }

    // Ring tap geometry in the gather's order, the same for every pixel and
    // ring count: (unit offset x, y, boundary, sector area span), rotation,
    // polygon and squeeze included. The gather computed it per tap (cos,
    // sin, and tan, log and cos twice for a polygon); here once per frame,
    // in double. Mirrored by dof_live_reference.py (live_tap_table()).
    void uploadTapTable(LLGLSLShader& shader, const ASDoFAperture::Shape& shape)
    {
        F32 table[TABLE_TAPS * 4];
        S32 i = 0;
        for (S32 k = 1; k <= TABLE_RINGS; ++k)
        {
            const S32 count = 6 * k;
            const F64 offset = (k & 1) ? 0.5 : 0.0;
            const F64 half_angle = PI_D / count;
            for (S32 j = 0; j < count; ++j, i += 4)
            {
                const F64 angle = 2.0 * PI_D * (j + offset) / count;
                const F64 boundary = ASDoFAperture::boundaryAt(shape, angle);
                const F64 rotated = angle + shape.mRotation;
                table[i] = (F32)(shape.mAnamorphic * std::cos(rotated) * boundary);
                table[i + 1] = (F32)(std::sin(rotated) * boundary);
                table[i + 2] = (F32)boundary;
                table[i + 3] = (F32)(ASDoFAperture::areaTo(shape, angle + half_angle) -
                                     ASDoFAperture::areaTo(shape, angle - half_angle));
            }
        }
        shader.uniform4fv(U_LIVE_TAPS, TABLE_TAPS, table);
    }

    void releaseTargets()
    {
        if (sCompleteFBO)
        {
            glDeleteFramebuffers(1, &sCompleteFBO);
            sCompleteFBO = 0;
        }
        if (sReduceFBO)
        {
            glDeleteFramebuffers(1, &sReduceFBO);
            sReduceFBO = 0;
        }
        sBinsA.release();
        sBinsB.release();
        sDoneA.release();
        sDoneB.release();
        sTileReduce.release();
        sTileX.release();
        sTileY.release();
        sNear2.release();
        sNear1.release();
        sFar.release();
        sBack.release();
        sWidth = 0;
        sHeight = 0;
    }

    void releaseSpriteTargets()
    {
        sCellTarget.release();
        sHighlightInput.release();
    }

    bool allocateMipmapped(LLRenderTarget& target, U32 width, U32 height, U32 attachments)
    {
        if (!target.allocate(width, height, GL_RGBA16F, false,
                             LLTexUnit::TT_TEXTURE, LLTexUnit::TMG_MANUAL))
        {
            return false;
        }
        for (U32 i = 1; i < attachments; ++i)
        {
            if (!target.addColorAttachment(GL_RGBA16F))
            {
                return false;
            }
        }
        return true;
    }

    // Top mip level index of a target (its 1x1 level).
    S32 topMipLevel(const LLRenderTarget& target)
    {
        return (S32)floorf(log2f((F32)llmax(target.getWidth(), target.getHeight())));
    }

    void generateMips(LLRenderTarget& target, U32 attachment)
    {
        // Explicit unit: glGenerateMipmap acts on the active unit's texture.
        LLTexUnit* unit = gGL.getTexUnit(0);
        unit->bindManual(LLTexUnit::TT_TEXTURE, target.getTexture(attachment), true);
        unit->activate();
        glGenerateMipmap(GL_TEXTURE_2D);
        unit->unbind(LLTexUnit::TT_TEXTURE);
    }

    bool ensureSpriteTargets(U32 width, U32 height)
    {
        const U32 cells_x = (width + HIGHLIGHT_CELL_SIZE - 1) / HIGHLIGHT_CELL_SIZE;
        const U32 cells_y = (height + HIGHLIGHT_CELL_SIZE - 1) / HIGHLIGHT_CELL_SIZE;
        if (sCellTarget.isComplete() && sCellTarget.getWidth() == cells_x &&
            sCellTarget.getHeight() == cells_y && sHighlightInput.isComplete() &&
            sHighlightInput.getWidth() == width && sHighlightInput.getHeight() == height)
        {
            return true;
        }
        releaseSpriteTargets();
        if (!allocateMipmapped(sCellTarget, cells_x, cells_y, 4) ||
            !sHighlightInput.allocate(width, height, GL_RGBA16F))
        {
            releaseSpriteTargets();
            return false;
        }
        return true;
    }

    // Completed textures in output order (asDoFLiveCompleteF.glsl).
    GLuint doneTexture(U32 index)
    {
        return index < 4 ? sDoneA.getTexture(index) : sDoneB.getTexture(0);
    }

    // Binds sCompleteFBO to one mip level of the completed textures.
    void attachDoneLevel(S32 level)
    {
        for (U32 i = 0; i < DONE_TEXTURES; ++i)
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D,
                                   doneTexture(i), level);
        }
    }

    // The completed targets and the FBO writing their levels. Their mip
    // storage comes from one glGenerateMipmap; every level is rewritten each
    // frame.
    bool allocateComplete(U32 width, U32 height)
    {
        if (!allocateMipmapped(sDoneA, width, height, 4) ||
            !allocateMipmapped(sDoneB, width, height, 1))
        {
            return false;
        }
        for (U32 attachment = 0; attachment < 4; ++attachment)
        {
            generateMips(sDoneA, attachment);
        }
        generateMips(sDoneB, 0);

        const U32 saved_fbo = LLRenderTarget::sCurFBO;
        glGenFramebuffers(1, &sCompleteFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, sCompleteFBO);
        GLenum buffers[DONE_TEXTURES];
        for (U32 i = 0; i < DONE_TEXTURES; ++i)
        {
            buffers[i] = GL_COLOR_ATTACHMENT0 + i;
        }
        glDrawBuffers(DONE_TEXTURES, buffers);
        bool success = true;
        for (S32 level = topMipLevel(sDoneA); level >= 0 && success; --level)
        {
            attachDoneLevel(level);
            success = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
        return success;
    }

    // The FBO writing level 0 of both bin targets in one pass.
    bool allocateReduce()
    {
        const U32 saved_fbo = LLRenderTarget::sCurFBO;
        glGenFramebuffers(1, &sReduceFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, sReduceFBO);
        GLenum buffers[BIN_TEXTURES];
        for (U32 i = 0; i < BIN_TEXTURES; ++i)
        {
            const GLuint texture = i < 3 ? sBinsA.getTexture(i) : sBinsB.getTexture(i - 3);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D,
                                   texture, 0);
            buffers[i] = GL_COLOR_ATTACHMENT0 + i;
        }
        glDrawBuffers(BIN_TEXTURES, buffers);
        const bool success = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
        return success;
    }

    bool ensureTargets(U32 width, U32 height)
    {
        if (sWidth == width && sHeight == height && sBinsA.isComplete())
        {
            return true;
        }
        releaseTargets();
        const U32 gather_width = (width + 1) / 2;
        const U32 gather_height = (height + 1) / 2;
        const U32 tiles_x = (gather_width + TILE - 1) / TILE;
        const U32 tiles_y = (gather_height + TILE - 1) / TILE;
        if (!allocateMipmapped(sBinsA, gather_width, gather_height, 3) ||
            !allocateMipmapped(sBinsB, gather_width, gather_height, 4) ||
            !allocateReduce() ||
            !allocateComplete(gather_width, gather_height) ||
            !sTileReduce.allocate(tiles_x, tiles_y, GL_RGBA16F) ||
            !sTileX.allocate(tiles_x, tiles_y, GL_RGBA16F) ||
            !sTileY.allocate(tiles_x, tiles_y, GL_RGBA16F) ||
            // Second attachments: per-channel coverage (axial CA).
            !sNear2.allocate(gather_width, gather_height, GL_RGBA16F) ||
            !sNear2.addColorAttachment(GL_RGBA16F) ||
            !sNear1.allocate(gather_width, gather_height, GL_RGBA16F) ||
            !sNear1.addColorAttachment(GL_RGBA16F) ||
            !sFar.allocate(gather_width, gather_height, GL_RGBA16F) ||
            !sBack.allocate(gather_width, gather_height, GL_RGBA16F))
        {
            releaseTargets();
            return false;
        }
        sWidth = width;
        sHeight = height;
        return true;
    }

    bool shadersComplete()
    {
        return sReduceProgram.isComplete() && sCompleteProgram.isComplete() &&
               sTileProgram.isComplete() &&
               sGatherProgram.isComplete() && sCompositeProgram.isComplete();
    }

    void draw(LLVertexBuffer& triangle)
    {
        triangle.setBuffer();
        triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    bool createProgram(LLGLSLShader& program, const char* name, const char* fragment,
                       bool library, S32 shader_level, const char* define = nullptr)
    {
        program.mName = name;
        program.mShaderFiles.clear();
        program.clearPermutations();
        if (define)
        {
            program.addPermutation(define, "1");
        }
        program.mFeatures.isDeferred = true;
        program.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
        program.mShaderFiles.emplace_back(fragment, GL_FRAGMENT_SHADER);
        if (library)
        {
            program.mShaderFiles.emplace_back(COMMON_LIBRARY, GL_FRAGMENT_SHADER);
        }
        program.mShaderLevel = shader_level;
        return program.createShader();
    }
}

void ASDoFLive::registerShaders(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sReduceProgram);
    shaders.push_back(&sCompleteProgram);
    shaders.push_back(&sTileProgram);
    shaders.push_back(&sGatherProgram);
    shaders.push_back(&sCompositeProgram);
    shaders.push_back(&sHighlightProgram);
    shaders.push_back(&sSpriteProgram);
}

bool ASDoFLive::createShaders(S32 shader_level)
{
    bool success = createProgram(sReduceProgram, "AyaneStorm Live DoF Reduce Shader",
                                 "deferred/asDoFLiveReduceF.glsl", true, shader_level);
    // No library: its samplers would count against macOS's 16 units.
    success = createProgram(sCompleteProgram, "AyaneStorm Live DoF Complete Shader",
                            "deferred/asDoFLiveCompleteF.glsl", false, shader_level) && success;
    success = createProgram(sTileProgram, "AyaneStorm Live DoF Tile Shader",
                            "deferred/asDoFLiveTileF.glsl", true, shader_level) && success;
    // The tap table adds 672 uniform components (OpenGL 4.1 guarantees 1024
    // per fragment shader). A driver that refuses it gets the per-tap
    // geometry instead: the same images, only slower.
    sGatherTable = createProgram(sGatherProgram, "AyaneStorm Live DoF Gather Shader",
                                 "deferred/asDoFLiveGatherF.glsl", true, shader_level,
                                 "LIVE_TAP_TABLE");
    if (!sGatherTable)
    {
        LL_WARNS("ASDoFLive") << "Live DoF gather tap table refused; per-tap geometry." << LL_ENDL;
        sGatherProgram.unload();
        success = createProgram(sGatherProgram, "AyaneStorm Live DoF Gather Shader",
                                "deferred/asDoFLiveGatherF.glsl", true, shader_level) && success;
    }
    success = createProgram(sCompositeProgram, "AyaneStorm Live DoF Composite Shader",
                            "deferred/asDoFLiveCompositeF.glsl", true, shader_level) && success;
    if (!success)
    {
        LL_WARNS("ASDoFLive") << "Live DoF shaders failed to load; Live DoF is unavailable." << LL_ENDL;
    }
    // Highlight sprites, mode 1's shaders with LIVE_SPRITES. Optional: Live
    // keeps every light in the gather without them.
    bool sprites = createProgram(sHighlightProgram, "AyaneStorm Live DoF Highlight Shader",
                                 "deferred/asDepthOfFieldHighlightF.glsl", true, shader_level,
                                 "LIVE_SPRITES");
    // Attribute-free instanced aperture sprites (asDepthOfFieldSpriteV.glsl).
    sSpriteProgram.mName = "AyaneStorm Live DoF Highlight Sprite Shader";
    sSpriteProgram.mShaderFiles.clear();
    sSpriteProgram.clearPermutations();
    sSpriteProgram.addPermutation("LIVE_SPRITES", "1");
    sSpriteProgram.mFeatures.attachNothing = true;
    sSpriteProgram.mShaderFiles.emplace_back("deferred/asDepthOfFieldSpriteV.glsl", GL_VERTEX_SHADER);
    sSpriteProgram.mShaderFiles.emplace_back("deferred/asDepthOfFieldSpriteF.glsl", GL_FRAGMENT_SHADER);
    sSpriteProgram.mShaderLevel = shader_level;
    sprites = sSpriteProgram.createShader() && sprites;
    if (!sprites)
    {
        LL_WARNS("ASDoFLive") << "Live DoF highlight sprite shaders failed to load; "
                                 "lights stay in the gather." << LL_ENDL;
    }
    return success;
}

void ASDoFLive::unloadShaders()
{
    sReduceProgram.unload();
    sCompleteProgram.unload();
    sTileProgram.unload();
    sGatherProgram.unload();
    sCompositeProgram.unload();
    sHighlightProgram.unload();
    sSpriteProgram.unload();
    releaseResources();
}

void ASDoFLive::releaseResources()
{
    // Called every frame while another renderer is selected.
    sCaptureLens.mValid = false;
    sOpaqueReady = false;
    if (sWidth || sBinsA.isComplete() || sOpaqueColor.isComplete())
    {
        releaseTargets();
        sOpaqueColor.release();
        sOpaqueDepth.release();
    }
    if (sCellTarget.isComplete() || sHighlightInput.isComplete())
    {
        releaseSpriteTargets();
    }
}

bool ASDoFLive::transparencyLens(U32 width, U32 height, ASMacOIT::DoFLens& lens)
{
    if (!binsWanted() || width == 0 || height == 0)
    {
        return false;
    }
    const F32 h = (F32)height;
    lens.mFocalDistance = sCaptureLens.mFocalDistance;
    lens.mBlurConstant = sCaptureLens.mBlurConstant;
    lens.mMagnification = sCaptureLens.mMagnification;
    lens.mTanPixelAngle = sCaptureLens.mTanPixelAngleHeight / h;
    lens.mMaxCoC = sCaptureLens.mMaxCoCPerHeight * h;
    lens.mNearRadius = lens.mMaxCoC * sCaptureLens.mNearScale;
    lens.mFarRadius = lens.mMaxCoC * sCaptureLens.mFarScale;
    lens.mSplitRadius = splitRadius(lens.mNearRadius);
    lens.mFarSplitRadius = splitRadius(lens.mFarRadius);
    lens.mGatherScale = (F32)((height + 1) / 2) / h;
    // The same lens field the Live passes use, at the capture's size.
    const ASDepthOfField::LensField field = ASDepthOfField::lensField(
        width, height, lens.mFocalDistance, lens.mBlurConstant, lens.mTanPixelAngle,
        lens.mMagnification, lens.mMaxCoC);
    lens.mFieldScale[0] = field.mFieldScale[0];
    lens.mFieldScale[1] = field.mFieldScale[1];
    lens.mCurvature = field.mCurvature;
    return true;
}

void ASDoFLive::prepareCapture(U32 width, U32 height)
{
    sOpaqueReady = false;
    if (!binsWanted() || width == 0 || height == 0)
    {
        return;
    }
    if (!sOpaqueColor.isComplete() || sOpaqueColor.getWidth() != width ||
        sOpaqueColor.getHeight() != height)
    {
        sOpaqueColor.release();
        sOpaqueDepth.release();
        if (!sOpaqueColor.allocate(width, height, GL_RGBA16F) ||
            !sOpaqueDepth.allocate(width, height, 0, true))
        {
            sOpaqueColor.release();
            sOpaqueDepth.release();
            LL_WARNS_ONCE("ASDoFLive") << "Live DoF opaque copy allocation failed; "
                                          "transparency stays one layer." << LL_ENDL;
            return;
        }
    }

    // The scene target holds the opaque colour and depth here; framebuffer
    // blits are part of the OpenGL 4.1 baseline.
    const U32 scene_fbo = LLRenderTarget::sCurFBO;
    LLGLDisable scissor(GL_SCISSOR_TEST);
    gGL.setColorMask(true, true);
    sOpaqueColor.bindTarget();
    const U32 color_fbo = LLRenderTarget::sCurFBO;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, color_fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, color_fbo);
    sOpaqueColor.flush();

    sOpaqueDepth.bindTarget();
    const U32 depth_fbo = LLRenderTarget::sCurFBO;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, depth_fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, depth_fbo);
    sOpaqueDepth.flush();
    gGL.setColorMask(true, false);
    sOpaqueReady = true;
}

bool ASDoFLive::render(LLRenderTarget& source, LLRenderTarget& destination,
                       LLRenderTarget& depth, LLVertexBuffer& screen_triangle,
                       F32 focal_distance, F32 blur_constant, F32 tan_pixel_angle,
                       F32 magnification, F32 max_coc)
{
    if (!shadersComplete() || gCubeSnapshot || ASBackgroundIsolate::isActive() ||
        &source == &destination || source.getWidth() == 0 || source.getHeight() == 0 ||
        source.getWidth() != destination.getWidth() ||
        source.getHeight() != destination.getHeight())
    {
        if (!shadersComplete())
        {
            LL_WARNS_ONCE("ASDoFLive") << "Live DoF shaders are incomplete; no DoF applied." << LL_ENDL;
        }
        return false;
    }

    const U32 width = source.getWidth();
    const U32 height = source.getHeight();
    if (!ensureTargets(width, height))
    {
        LL_WARNS_ONCE("ASDoFLive") << "Live DoF target allocation failed at "
                                   << width << "x" << height << LL_ENDL;
        return false;
    }
    static U32 logged_width = 0;
    static U32 logged_height = 0;
    if (logged_width != width || logged_height != height)
    {
        logged_width = width;
        logged_height = height;
        LL_INFOS("ASDoFLive") << "Live DoF active at " << width << "x" << height << LL_ENDL;
    }

    // Blur size: the same frontend as the Advanced renderer (physical blur
    // in percent of the image height by default, so any resolution and
    // snapshots frame alike).
    Lens lens;
    lens.mFocalDistance = focal_distance;
    lens.mBlurConstant = blur_constant;
    lens.mTanPixelAngle = tan_pixel_angle;
    lens.mMagnification = magnification;
    lens.mMaxCoC = gSavedSettings.getBOOL("ASDepthOfFieldPhysicalBlur") ?
        0.01f * llclamp(gSavedSettings.getF32("ASDepthOfFieldMaxBlur"), 1.f, 10.f) * (F32)height :
        llclamp(fabsf(max_coc), 0.f, 150.f);
    lens.mNearRadius = lens.mMaxCoC * llclamp(gSavedSettings.getF32("ASDepthOfFieldNearRadius"), 0.f, 4.f);
    lens.mFarRadius = lens.mMaxCoC * llclamp(gSavedSettings.getF32("ASDepthOfFieldFarRadius"), 0.f, 4.f);
    lens.mSplitRadius = splitRadius(lens.mNearRadius);
    lens.mFarSplitRadius = splitRadius(lens.mFarRadius);
    lens.mField = ASDepthOfField::lensField(width, height, focal_distance, blur_constant,
                                            tan_pixel_angle, magnification, lens.mMaxCoC);
    lens.mShape.mBlades = llclamp(gSavedSettings.getS32("ASDepthOfFieldApertureBlades"), 0, 12);
    lens.mShape.mRoundness = llclamp(gSavedSettings.getF32("ASDepthOfFieldApertureRoundness"), 0.f, 1.f);
    lens.mShape.mRotation = gSavedSettings.getF32("ASDepthOfFieldApertureRotation") * DEG_TO_RAD;
    lens.mShape.mAnamorphic = llclamp(gSavedSettings.getF32("ASDepthOfFieldAnamorphicRatio"), 0.1f, 2.f);
    lens.mUnitArea = ASDoFAperture::unitArea(lens.mShape);
    const S32 rings = QUALITY_RINGS[llclamp(gSavedSettings.getS32("ASDepthOfFieldQuality"), 0, 2)];
    const S32 debug_mode = llclamp(gSavedSettings.getS32("ASDepthOfFieldLiveDebug"), 0, 10);

    const F32 gather_width = (F32)sBinsA.getWidth();
    const F32 gather_height = (F32)sBinsA.getHeight();
    const S32 max_level = topMipLevel(sBinsA);
    lens.mGatherScale = gather_height / (F32)height;

    // Transparency bins of this frame, or the one-layer fallback: the
    // composited image by the depth buffer.
    const bool bins = sOpaqueReady && ASMacOIT::dofBinsReady(width, height) &&
        sOpaqueColor.getWidth() == width && sOpaqueColor.getHeight() == height;
    sOpaqueReady = false;
    LLRenderTarget& bin_color = bins ? sOpaqueColor : source;
    LLRenderTarget& bin_depth = bins ? sOpaqueDepth : depth;
    static S32 logged_bins = -1;
    if (logged_bins != (bins ? 1 : 0))
    {
        logged_bins = bins ? 1 : 0;
        LL_INFOS("ASDoFLive") << "Live DoF transparency bins "
                              << (bins ? "from Mac OIT" : "unavailable; one layer") << LL_ENDL;
    }

    // Highlight sprites, with mode 1's settings. Without their programs or
    // targets every light stays in the gather, as before.
    bool sprites = gSavedSettings.getBOOL("ASDepthOfFieldHighlightSprites") &&
        sHighlightProgram.isComplete() && sSpriteProgram.isComplete() &&
        (lens.mNearRadius > 0.f || lens.mFarRadius > 0.f);
    if (sprites && !ensureSpriteTargets(width, height))
    {
        LL_WARNS_ONCE("ASDoFLive") << "Live DoF highlight sprite allocation failed; "
                                      "lights stay in the gather." << LL_ENDL;
        sprites = false;
    }
    if (!sprites && sCellTarget.isComplete())
    {
        releaseSpriteTargets();
    }
    const F32 isolation = llclamp(gSavedSettings.getF32("ASDepthOfFieldHighlightIsolation"), 1.2f, 8.f);
    const F32 sprite_budget = (F32)llclamp(gSavedSettings.getS32("ASDepthOfFieldHighlightMaxSprites"), 256, 32768);
    // Bright bokeh highlights (artistic), the Aperture-sampled renderer's
    // settings and ranges, on the sprites' light.
    const F32 hl_strength = gSavedSettings.getBOOL("ASDepthOfFieldApertureHighlights") ?
        llclamp(gSavedSettings.getF32("ASDepthOfFieldApertureHighlightStrength"), 0.f, 1.f) : 0.f;
    const F32 hl_threshold =
        llclamp(gSavedSettings.getF32("ASDepthOfFieldApertureHighlightThreshold"), 0.05f, 8.f);
    const S32 cells_x = sprites ? (S32)sCellTarget.getWidth() : 0;
    const S32 cells_y = sprites ? (S32)sCellTarget.getHeight() : 0;
    const S32 cell_top_level = sprites ? topMipLevel(sCellTarget) : 0;
    // The reduce bins the bin colour without the lights the sprites redraw;
    // the composite keeps the original for its exact in-focus share.
    LLRenderTarget& gather_input = sprites ? sHighlightInput : bin_color;

    // The next capture's lens, per image height.
    sCaptureLens.mValid = lens.mMaxCoC > 0.f;
    sCaptureLens.mFocalDistance = lens.mFocalDistance;
    sCaptureLens.mBlurConstant = lens.mBlurConstant;
    sCaptureLens.mMagnification = lens.mMagnification;
    sCaptureLens.mTanPixelAngleHeight = lens.mTanPixelAngle * (F32)height;
    sCaptureLens.mMaxCoCPerHeight = lens.mMaxCoC / (F32)height;
    sCaptureLens.mNearScale = lens.mMaxCoC > 0.f ? lens.mNearRadius / lens.mMaxCoC : 0.f;
    sCaptureLens.mFarScale = lens.mMaxCoC > 0.f ? lens.mFarRadius / lens.mMaxCoC : 0.f;

    LL_PROFILE_GPU_ZONE("AyaneStorm Live DoF");
    LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);

    // 0. Highlight extraction (asDepthOfFieldHighlightF.glsl, LIVE_SPRITES):
    // cells of isolated defocused light, then the gather input without it.
    if (sprites)
    {
        LL_PROFILE_GPU_ZONE("Live DoF highlights");
        auto highlight_pass = [&](LLRenderTarget& target, S32 pass)
        {
            target.bindTarget();
            sHighlightProgram.bind();
            sHighlightProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &bin_color, false,
                                          LLTexUnit::TFO_POINT);
            sHighlightProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &bin_depth, true,
                                          LLTexUnit::TFO_POINT);
            bindBins(sHighlightProgram, bins);
            if (pass == 1)
            {
                sHighlightProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR, &sCellTarget, false,
                                              LLTexUnit::TFO_TRILINEAR, 0);
                sHighlightProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &sCellTarget, false,
                                              LLTexUnit::TFO_TRILINEAR, 2);
                sHighlightProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sCellTarget, false,
                                              LLTexUnit::TFO_TRILINEAR, 3);
            }
            sHighlightProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
            sHighlightProgram.uniform1f(U_MAX_RADIUS, lens.mFarRadius);
            sHighlightProgram.uniform1f(U_NEAR_MAX_RADIUS, lens.mNearRadius);
            sHighlightProgram.uniform1f(U_ISOLATION, isolation);
            sHighlightProgram.uniform1i(U_HIGHLIGHT_PASS, pass);
            sHighlightProgram.uniform2i(U_CELL_GRID, cells_x, cells_y);
            sHighlightProgram.uniform1i(U_CELL_TOP_LEVEL, cell_top_level);
            sHighlightProgram.uniform1f(U_SPRITE_BUDGET, sprite_budget);
            sHighlightProgram.uniform1f(U_HL_STRENGTH, hl_strength);
            sHighlightProgram.uniform1f(U_HL_THRESHOLD, hl_threshold);
            setLensUniforms(sHighlightProgram, lens);
            draw(screen_triangle);
            if (pass == 1)
            {
                sHighlightProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM, sCellTarget.getUsage());
                sHighlightProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT, sCellTarget.getUsage());
                sHighlightProgram.unbindTexture(LLShaderMgr::DEFERRED_SPECULAR, sCellTarget.getUsage());
            }
            unbindBins(sHighlightProgram, bins);
            sHighlightProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH, bin_depth.getUsage());
            sHighlightProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, bin_color.getUsage());
            sHighlightProgram.unbind();
            target.flush();
        };
        highlight_pass(sCellTarget, 0);
        // The top levels' averages give the sprite count and the counts per
        // brightness level for the budget rule.
        generateMips(sCellTarget, 0);
        generateMips(sCellTarget, 2);
        generateMips(sCellTarget, 3);
        highlight_pass(sHighlightInput, 1);
    }

    // 1. Bins at gather resolution, then their mip chains.
    {
        LL_PROFILE_GPU_ZONE("Live DoF reduce");
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        const U32 saved_fbo = LLRenderTarget::sCurFBO;
        glBindFramebuffer(GL_FRAMEBUFFER, sReduceFBO);
        glViewport(0, 0, sBinsA.getWidth(), sBinsA.getHeight());
        sReduceProgram.bind();
        sReduceProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &gather_input, false, LLTexUnit::TFO_POINT);
        sReduceProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &bin_depth, true, LLTexUnit::TFO_POINT);
        bindBins(sReduceProgram, bins);
        sReduceProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
        setLensUniforms(sReduceProgram, lens);
        draw(screen_triangle);
        unbindBins(sReduceProgram, bins);
        sReduceProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH, bin_depth.getUsage());
        sReduceProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, gather_input.getUsage());
        sReduceProgram.unbind();
        glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        for (U32 attachment = 0; attachment < sBinsA.getNumTextures(); ++attachment)
        {
            generateMips(sBinsA, attachment);
        }
        for (U32 attachment = 0; attachment < sBinsB.getNumTextures(); ++attachment)
        {
            generateMips(sBinsB, attachment);
        }
    }

    // 2. Completed bins, top level down (asDoFLiveCompleteF.glsl). Level l
    // reads the completed level l + 1: the completed textures' base level is
    // l + 1 meanwhile, so no level is read while written.
    {
        LL_PROFILE_GPU_ZONE("Live DoF complete");
        const S32 coarser[DONE_TEXTURES] = {
            LLShaderMgr::DEFERRED_DIFFUSE, LLShaderMgr::DEFERRED_SPECULAR,
            LLShaderMgr::DEFERRED_EMISSIVE, LLShaderMgr::DEFERRED_BLOOM,
            LLShaderMgr::DEFERRED_LIGHT };
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        const U32 saved_fbo = LLRenderTarget::sCurFBO;
        glBindFramebuffer(GL_FRAMEBUFFER, sCompleteFBO);
        sCompleteProgram.bind();
        sCompleteProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW0, &sBinsA, false, LLTexUnit::TFO_TRILINEAR, 1);
        sCompleteProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW1, &sBinsB, false, LLTexUnit::TFO_TRILINEAR, 0);
        sCompleteProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW2, &sBinsB, false, LLTexUnit::TFO_TRILINEAR, 1);
        sCompleteProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW3, &sBinsB, false, LLTexUnit::TFO_TRILINEAR, 2);
        sCompleteProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW4, &sBinsA, false, LLTexUnit::TFO_TRILINEAR, 2);
        sCompleteProgram.bindTexture(LLShaderMgr::DEFERRED_SHADOW5, &sBinsB, false, LLTexUnit::TFO_TRILINEAR, 3);
        sCompleteProgram.uniform1i(U_TOP_LEVEL, max_level);
        for (S32 level = max_level; level >= 0; --level)
        {
            attachDoneLevel(level);
            glViewport(0, 0, llmax((S32)sDoneA.getWidth() >> level, 1),
                       llmax((S32)sDoneA.getHeight() >> level, 1));
            const bool top = level == max_level;
            if (!top)
            {
                for (U32 i = 0; i < DONE_TEXTURES; ++i)
                {
                    bindRaw(sCompleteProgram, coarser[i], doneTexture(i));
                    const S32 unit = sCompleteProgram.getTextureChannel(coarser[i]);
                    if (unit >= 0)
                    {
                        gGL.getTexUnit(unit)->activate();
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, level + 1);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    }
                }
            }
            sCompleteProgram.uniform1i(U_LEVEL, level);
            draw(screen_triangle);
            if (!top)
            {
                for (U32 i = 0; i < DONE_TEXTURES; ++i)
                {
                    const S32 unit = sCompleteProgram.getTextureChannel(coarser[i]);
                    if (unit >= 0)
                    {
                        gGL.getTexUnit(unit)->activate();
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                    }
                    unbindRaw(sCompleteProgram, coarser[i]);
                }
            }
        }
        sCompleteProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW5);
        sCompleteProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW4);
        sCompleteProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW3);
        sCompleteProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW2);
        sCompleteProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW1);
        sCompleteProgram.unbindTexture(LLShaderMgr::DEFERRED_SHADOW0);
        sCompleteProgram.unbind();
        glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }

    // 3. Veil kernel radius per tile: N2, N1, B1.
    {
        LL_PROFILE_GPU_ZONE("Live DoF tiles");
        // B1 ends where the B1/B2 ramp does. Axial CA widens every source by
        // its widest stratum (0.75 delta, asDoFLiveGatherF.glsl).
        const F32 ca_near = 0.75f * lens.mField.mAxialCA * lens.mNearRadius;
        const F32 ca_far = 0.75f * lens.mField.mAxialCA * lens.mFarRadius;
        const F32 veil_radius = llmax(lens.mNearRadius + ca_near,
                                      llmin(1.25f * lens.mFarSplitRadius, lens.mFarRadius) + ca_far);
        const F32 veil_gather = veil_radius * gather_height / (F32)height;
        const S32 reach = llclamp((S32)ceilf(veil_gather / (F32)TILE), 0, MAX_TILE_REACH);
        LLRenderTarget* previous = nullptr;
        LLRenderTarget* const outputs[] = { &sTileReduce, &sTileX, &sTileY };
        for (S32 pass = 0; pass < 3; ++pass)
        {
            LLRenderTarget& target = *outputs[pass];
            target.bindTarget();
            sTileProgram.bind();
            if (pass == 0)
            {
                // The bins as the gathers read them: N2 raw, N1 and B1
                // completed.
                sTileProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sBinsA, false, LLTexUnit::TFO_TRILINEAR, 0);
                sTileProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR, &sDoneA, false, LLTexUnit::TFO_TRILINEAR, 0);
                sTileProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE, &sBinsA, false, LLTexUnit::TFO_TRILINEAR, 2);
                sTileProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sDoneA, false, LLTexUnit::TFO_TRILINEAR, 2);
                sTileProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &sDoneB, false, LLTexUnit::TFO_TRILINEAR, 0);
            }
            else
            {
                sTileProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, previous, false, LLTexUnit::TFO_POINT);
            }
            sTileProgram.uniform1i(U_TILE_PASS, pass);
            sTileProgram.uniform1i(U_TILE_REACH, reach);
            sTileProgram.uniform2f(U_CA_REACH, ca_near * lens.mGatherScale, ca_far * lens.mGatherScale);
            draw(screen_triangle);
            if (pass == 0)
            {
                sTileProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT, sDoneB.getUsage());
                sTileProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM, sDoneA.getUsage());
                sTileProgram.unbindTexture(LLShaderMgr::DEFERRED_EMISSIVE, sBinsA.getUsage());
                sTileProgram.unbindTexture(LLShaderMgr::DEFERRED_SPECULAR, sDoneA.getUsage());
                sTileProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, sBinsA.getUsage());
            }
            else
            {
                sTileProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, previous->getUsage());
            }
            sTileProgram.unbind();
            target.flush();
            previous = &target;
        }
    }

    // Highlight sprites (asDepthOfFieldSpriteV/F.glsl, LIVE_SPRITES), each
    // part of a cell's light in its own layer: the far back part (B2) into
    // B2 before B1 is laid over it, so hair just behind the focus covers it
    // as it covers gathered light; the far front part (B1) over the
    // background; near, the front part (N2) over the veil and the back part
    // (N1) under N2's coverage. The diagnostic views 3, 9 and 10 keep the
    // background targets for themselves.
    const bool draw_live_sprites = sprites && debug_mode != 3 && debug_mode != 9 &&
                                   debug_mode != 10;
    auto draw_sprites = [&](LLRenderTarget& target, S32 plane, S32 part)
    {
        LL_PROFILE_GPU_ZONE("Live DoF sprites");
        LLGLEnable sprite_blend(GL_BLEND);
        target.bindTarget();
        if (plane > 0)
        {
            // The background is divided by its alpha (B2 by the B1 pass, the
            // background by the composite): the light weighted by it comes
            // out at exactly its radiance, and nothing where F empties it.
            gGL.blendFunc(LLRender::BF_DEST_ALPHA, LLRender::BF_ONE,
                          LLRender::BF_ZERO, LLRender::BF_ONE);
        }
        else
        {
            // Light over what lies behind the veil; its coverage stays.
            gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE,
                          LLRender::BF_ZERO, LLRender::BF_ONE);
        }
        sSpriteProgram.bind();
        sSpriteProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR, &sCellTarget, false,
                                   LLTexUnit::TFO_TRILINEAR, 0);
        sSpriteProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE, &sCellTarget, false,
                                   LLTexUnit::TFO_POINT, 1);
        sSpriteProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &sCellTarget, false,
                                   LLTexUnit::TFO_TRILINEAR, 2);
        sSpriteProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sCellTarget, false,
                                   LLTexUnit::TFO_TRILINEAR, 3);
        if (part == 0 && plane > 0)
        {
            // B2's own kernel per pixel: completed (S, W) and M.
            sSpriteProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sDoneA, false,
                                       LLTexUnit::TFO_POINT, 3);
            sSpriteProgram.bindTexture(LLShaderMgr::NORMAL_MAP, &sDoneB, false,
                                       LLTexUnit::TFO_POINT, 0);
        }
        else if (part == 0)
        {
            // N2, whose coverage lies over N1.
            sSpriteProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &sNear2, false,
                                       LLTexUnit::TFO_POINT, 0);
        }
        sSpriteProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
        sSpriteProgram.uniform2f(U_TARGET_RES, gather_width, gather_height);
        sSpriteProgram.uniform1f(U_MAX_RADIUS, lens.mFarRadius);
        sSpriteProgram.uniform1f(U_NEAR_MAX_RADIUS, lens.mNearRadius);
        sSpriteProgram.uniform1i(U_PLANE, plane);
        sSpriteProgram.uniform1i(U_LIVE_PART, part);
        sSpriteProgram.uniform2i(U_CELL_GRID, cells_x, cells_y);
        sSpriteProgram.uniform1i(U_CELL_TOP_LEVEL, cell_top_level);
        sSpriteProgram.uniform1f(U_SPRITE_BUDGET, sprite_budget);
        // Aperture, squeeze, unit area, field, cat's eye, axial CA and
        // spherical aberration; astigmatism stays 0 (Live has none).
        setLensUniforms(sSpriteProgram, lens);
        // Attribute-free: any bound vertex buffer satisfies the core-profile
        // VAO; positions come from gl_VertexID and gl_InstanceID.
        screen_triangle.setBuffer();
        glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2 * cells_x * cells_y);
        if (part == 0 && plane > 0)
        {
            sSpriteProgram.unbindTexture(LLShaderMgr::NORMAL_MAP, sDoneB.getUsage());
            sSpriteProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, sDoneA.getUsage());
        }
        else if (part == 0)
        {
            sSpriteProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sNear2.getUsage());
        }
        sSpriteProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM, sCellTarget.getUsage());
        sSpriteProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT, sCellTarget.getUsage());
        sSpriteProgram.unbindTexture(LLShaderMgr::DEFERRED_EMISSIVE, sCellTarget.getUsage());
        sSpriteProgram.unbindTexture(LLShaderMgr::DEFERRED_SPECULAR, sCellTarget.getUsage());
        sSpriteProgram.unbind();
        target.flush();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    };

    // 4. Gathers, in layer order (asDoFLiveGatherF.glsl): N2; N1, written
    // over N2; B2; B1, written over B2.
    {
        LL_PROFILE_GPU_ZONE("Live DoF gathers");
        struct Gather
        {
            LLRenderTarget* mOutput;
            LLRenderTarget* mBins;    // the layer alone: N2 raw, others completed
            U32 mAttachment;
            LLRenderTarget* mEnergy;  // its energies
            U32 mEnergyAttachment;
            LLRenderTarget* mBehind;  // the layer this one is written over
        };
        const Gather gathers[] = {
            { &sNear2, &sBinsA, 0, &sBinsA, 2, nullptr },
            { &sNear1, &sDoneA, 0, &sDoneB, 0, &sNear2 },
            { &sFar, &sDoneA, 3, &sDoneB, 0, nullptr },
            { &sBack, &sDoneA, 2, &sDoneB, 0, &sFar },
        };
        for (S32 layer = 0; layer < 4; ++layer)
        {
            const Gather& gather = gathers[layer];
            gather.mOutput->bindTarget();
            sGatherProgram.bind();
            sGatherProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, gather.mBins, false,
                                       LLTexUnit::TFO_TRILINEAR, gather.mAttachment);
            sGatherProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR, gather.mEnergy, false,
                                       LLTexUnit::TFO_TRILINEAR, gather.mEnergyAttachment);
            if (gather.mBehind)
            {
                sGatherProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT, gather.mBehind, false,
                                           LLTexUnit::TFO_POINT);
            }
            if (gather.mBehind == &sNear2)
            {
                // N2's per-channel coverage (axial CA).
                sGatherProgram.bindTexture(LLShaderMgr::NORMAL_MAP, &sNear2, false,
                                           LLTexUnit::TFO_POINT, 1);
            }
            sGatherProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sBinsB, false,
                                       LLTexUnit::TFO_TRILINEAR, 0);
            sGatherProgram.bindTexture(LLShaderMgr::DEFERRED_NOISE, &sTileY, false,
                                       LLTexUnit::TFO_POINT);
            sGatherProgram.uniform2f(U_TARGET_RES, gather_width, gather_height);
            sGatherProgram.uniform1i(U_LAYER, layer);
            sGatherProgram.uniform1i(U_MAX_RINGS, rings);
            sGatherProgram.uniform1i(U_DEBUG_MODE, debug_mode);
            setLensUniforms(sGatherProgram, lens);
            if (sGatherTable && layer == 0)
            {
                // Program state: once per frame serves all four layers.
                uploadTapTable(sGatherProgram, lens.mShape);
            }
            draw(screen_triangle);
            if (gather.mBehind == &sNear2)
            {
                sGatherProgram.unbindTexture(LLShaderMgr::NORMAL_MAP, sNear2.getUsage());
            }
            if (gather.mBehind)
            {
                sGatherProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT, gather.mBehind->getUsage());
            }
            sGatherProgram.unbindTexture(LLShaderMgr::DEFERRED_NOISE, sTileY.getUsage());
            sGatherProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM, sBinsB.getUsage());
            sGatherProgram.unbindTexture(LLShaderMgr::DEFERRED_SPECULAR, gather.mEnergy->getUsage());
            sGatherProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, gather.mBins->getUsage());
            sGatherProgram.unbind();
            gather.mOutput->flush();
            if (layer == 2 && draw_live_sprites && lens.mFarRadius > 0.f)
            {
                // The far back part into B2, before B1 is laid over it.
                draw_sprites(sFar, 1, 0);
            }
        }
    }

    if (draw_live_sprites)
    {
        if (lens.mFarRadius > 0.f)
        {
            draw_sprites(sBack, 1, 1);
        }
        if (lens.mNearRadius > 0.f)
        {
            draw_sprites(sNear1, -1, 1);
            draw_sprites(sNear1, -1, 0);
        }
    }

    // 5. Composite at full resolution.
    {
        LL_PROFILE_GPU_ZONE("Live DoF composite");
        destination.bindTarget();
        sCompositeProgram.bind();
        sCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_PROJECTION, &source, false, LLTexUnit::TFO_POINT);
        sCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &bin_color, false, LLTexUnit::TFO_POINT);
        sCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &bin_depth, true, LLTexUnit::TFO_POINT);
        bindBins(sCompositeProgram, bins);
        sCompositeProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
        // F completed, read at level 0.
        sCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR, &sDoneA, false,
                                      LLTexUnit::TFO_BILINEAR, 1);
        sCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &sBack, false, LLTexUnit::TFO_BILINEAR);
        sCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sNear1, false, LLTexUnit::TFO_BILINEAR);
        // The veil's per-channel coverage (axial CA). Debug view 3 is drawn
        // by the B1 gather, so the composite stays within 16 texture units.
        sCompositeProgram.bindTexture(LLShaderMgr::NORMAL_MAP, &sNear1, false, LLTexUnit::TFO_BILINEAR, 1);
        sCompositeProgram.uniform1i(U_DEBUG_MODE, debug_mode);
        setLensUniforms(sCompositeProgram, lens);
        draw(screen_triangle);
        sCompositeProgram.unbindTexture(LLShaderMgr::NORMAL_MAP, sNear1.getUsage());
        sCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_BLOOM, sNear1.getUsage());
        sCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_LIGHT, sBack.getUsage());
        sCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_SPECULAR, sDoneA.getUsage());
        unbindBins(sCompositeProgram, bins);
        sCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH, bin_depth.getUsage());
        sCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, bin_color.getUsage());
        sCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_PROJECTION, source.getUsage());
        sCompositeProgram.unbind();
        destination.flush();
    }
    return true;
}
