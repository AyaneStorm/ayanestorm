/**
 * @file asdoflive.cpp
 * @author chanayane@firestorm
 * @brief AyaneStorm Live depth of field (ASDepthOfFieldMode 3).
 *
 * Passes (all OpenGL 4.1 fragment, see doc/ayanestorm-depth-of-field-live-plan.md):
 *   1. reduce     full resolution to the half-resolution bin sums, two passes
 *                 (N2, N1, energies; F, B1, B2, visibility), then mip chains;
 *   2. complete   the bins behind N2 completed where nearer bins hide them
 *                 (push-pull), one draw per mip level, top down;
 *   3. tiles      veil kernel radius (N2, N1, B1) per 8x8 gather-pixel tile:
 *                 reduce, then dilation along x and y;
 *   4. gathers    area-tap scatter-as-gather of N2, N1, B1 (tile kernel) and
 *                 B2 (own kernel, hole fill); N1 is written over N2, B1 over
 *                 B2;
 *   5. composite  full resolution, N2 over N1 over F over B1 over B2.
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

    // Bin sums at gather resolution, mipmapped (asDoFLiveReduceF.glsl):
    // A = N2, N1, radius moments; B = F, B1, B2, visibility.
    LLRenderTarget sBinsA;
    LLRenderTarget sBinsB;
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
    const LLStaticHashedString U_REDUCE_PASS("reduce_pass");
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

    // Live DoF selected and its transparency bins wanted.
    bool binsWanted()
    {
        static LLCachedControl<S32> mode(gSavedSettings, "ASDepthOfFieldMode", 0);
        static LLCachedControl<bool> transparency(gSavedSettings, "ASDepthOfFieldLiveTransparency", true);
        return S32(mode) == ASDoFLive::LIVE_MODE && transparency && LLPipeline::RenderDepthOfField &&
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

    void releaseTargets()
    {
        if (sCompleteFBO)
        {
            glDeleteFramebuffers(1, &sCompleteFBO);
            sCompleteFBO = 0;
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
                       bool library, S32 shader_level)
    {
        program.mName = name;
        program.mShaderFiles.clear();
        program.clearPermutations();
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
    success = createProgram(sGatherProgram, "AyaneStorm Live DoF Gather Shader",
                            "deferred/asDoFLiveGatherF.glsl", true, shader_level) && success;
    success = createProgram(sCompositeProgram, "AyaneStorm Live DoF Composite Shader",
                            "deferred/asDoFLiveCompositeF.glsl", true, shader_level) && success;
    if (!success)
    {
        LL_WARNS("ASDoFLive") << "Live DoF shaders failed to load; Live DoF is unavailable." << LL_ENDL;
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

    // 1. Bins at gather resolution, then their mip chains.
    {
        LL_PROFILE_GPU_ZONE("Live DoF reduce");
        for (S32 pass = 0; pass < 2; ++pass)
        {
            LLRenderTarget& target = pass == 0 ? sBinsA : sBinsB;
            target.bindTarget();
            sReduceProgram.bind();
            sReduceProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &bin_color, false, LLTexUnit::TFO_POINT);
            sReduceProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &bin_depth, true, LLTexUnit::TFO_POINT);
            bindBins(sReduceProgram, bins);
            sReduceProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
            sReduceProgram.uniform2f(U_TARGET_RES, gather_width, gather_height);
            sReduceProgram.uniform1i(U_REDUCE_PASS, pass);
            setLensUniforms(sReduceProgram, lens);
            draw(screen_triangle);
            unbindBins(sReduceProgram, bins);
            sReduceProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH, bin_depth.getUsage());
            sReduceProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, bin_color.getUsage());
            sReduceProgram.unbind();
            target.flush();
        }
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
