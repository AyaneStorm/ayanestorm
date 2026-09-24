/**
 * @file asdofrenderer.cpp
 * @author chanayane@firestorm
 * @brief Aperture-sampled depth-of-field frame coordinator (ASDepthOfFieldMode 2).
 */
#include "llviewerprecompiledheaders.h"
#include "asdofrenderer.h"

#include <cmath>

#include "glm/gtc/type_ptr.hpp"

#include "asdofaperture.h"
#include "asdofcamera.h"
#include "llappviewer.h"
#include "llfloater.h"
#include "llfontgl.h"
#include "llgl.h"
#include "llrender2dutils.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrootview.h"
#include "llspatialpartition.h"
#include "lltoolmgr.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llwindow.h"
#include "pipeline.h"

#if LL_WINDOWS
#include "llwin32headers.h" // capture cancel: GetAsyncKeyState
#endif

extern bool gCubeSnapshot;
extern bool gSnapshot; // display(): set for every snapshot capture
extern bool gDisplaySwapBuffers;

namespace
{
    // Aperture-sampled renderer; 0/1 are the Firestorm and screen-space ones.
    constexpr S32 APERTURE_MODE = 2;
    // Upper bound for sample counts and the development sample index.
    constexpr S32 MAX_SAMPLES = 4096;
    // Focus smoothing drifts slightly every frame; smaller relative changes
    // keep the frozen accumulation focus instead of restarting.
    constexpr F32 FOCUS_RESTART_TOLERANCE = 0.005f;
    // Seconds between progress screens during a blocking snapshot capture.
    constexpr F32 CAPTURE_PROGRESS_INTERVAL = 0.5f;
    // Blocking captures (tiled or synchronous callers) suspend networking:
    // an ~80 s one was logged out by the region, so they stop well before.
    constexpr F32 MAX_BLOCKING_CAPTURE_SECONDS = 30.f;
    // Time-sliced captures return to the main loop between slices.
    constexpr F32 MAX_SLICED_CAPTURE_SECONDS = 600.f;
    // Sample rendering per slice; the main loop (networking, UI, one live
    // frame) runs between slices.
    constexpr F32 CAPTURE_SLICE_SECONDS = 0.3f;
    // A pending capture whose caller stopped calling (floater closed) is
    // abandoned after this long.
    constexpr F32 CAPTURE_ABANDON_SECONDS = 2.f;
    // New samples a thumbnail snapshot may add to the live average.
    constexpr S32 PREVIEW_SAMPLES = 16;
    // Snapshot floater preview refresh during a sliced capture: first after
    // this many samples, then every CAPTURE_PREVIEW_INTERVAL.
    constexpr S32 CAPTURE_PREVIEW_FIRST = 8;
    constexpr S32 CAPTURE_PREVIEW_INTERVAL = 32;

    // Cap on the per-sample residual softening: it closes gaps between
    // sample images (about 1 px at 4096 samples for a 40 px bokeh radius)
    // and, at higher strengths, deliberately softens the dots further. The
    // lens samples still make the blur and the aperture shape.
    constexpr F32 RESIDUAL_MAX_PIXELS = 24.f;

    const LLStaticHashedString U_SAMPLE_WEIGHT("sample_weight");
    const LLStaticHashedString U_RESIDUAL_SCALE("residual_scale");
    const LLStaticHashedString U_RESIDUAL_MAX("residual_max");
    const LLStaticHashedString U_INV_FOCUS("inv_focus");
    const LLStaticHashedString U_PROJ_Z("proj_z");
    const LLStaticHashedString U_TEXEL_SIZE("texel_size");
    const LLStaticHashedString U_TAP_ROTATION("tap_rotation");

    // What this presented frame renders and shows.
    enum class FramePlan
    {
        PINHOLE,    // camera/lens changed or invalid lens: central view only
        DEBUG,      // one unaveraged lens sample (registration check)
        ACCUMULATE, // still: add new lens samples to the running average
        CONVERGED   // cap reached or capture stopped: show the finished average
    };

    // Everything that invalidates the running average when it changes.
    struct AccumulationKey
    {
        glm::mat4 mProjection;
        glm::mat4 mModelview;
        F32 mView = 0.f;
        F32 mFNumber = 0.f;
        F32 mFocalLength = 0.f;
        F32 mDefaultFov = 0.f;
        ASDoFAperture::Shape mShape;
        F32 mResidualBlur = 0.f;
        F32 mAxialCA = 0.f;     // focal shift / focal length; 0 = off
        U32 mWidth = 0;
        U32 mHeight = 0;
    };

    // A running average: full-precision linear HDR sum, unit weights.
    struct Accumulator
    {
        LLRenderTarget mTarget;
        AccumulationKey mKey;
        bool mHaveKey = false;
        F32 mKeyFocus = 0.f;    // focus frozen for this average
        S32 mAccumulated = 0;   // samples in the sum
        F32 mSkyTime = 0.f;     // frame time frozen for star rotation
    };

    LLGLSLShader sAccumulateProgram;
    // Live view and snapshot captures average separately: live frames run
    // at window size between the slices of a larger capture.
    Accumulator sLive;
    Accumulator sCapture;
    Accumulator* sSlot = &sLive;

    FramePlan sPlan = FramePlan::PINHOLE;
    bool sActive = false;
    // Set when accumulation could not be allocated: the frame keeps its own
    // rendered sample instead of an incomplete average (logged once).
    bool sAccumulationFailed = false;
    S32 sSampleCount = 1;       // samples rendered this frame
    S32 sSampleIndex = 0;       // index within this frame
    S32 sFirstSequenceIndex = 0;
    S32 sMaxSamples = 1;        // target of this frame's average
    S32 sLiveMaxSamples = 1;    // live target (progress overlay)
    S32 sSavedOcclusion = 0;
    bool sOcclusionOverridden = false;
    F32 sFocusDistance = 16.f;  // renderDoF's initial focus distance
    ASDoFCamera::Lens sLens;
    std::vector<glm::vec2> sLensSamples;
    std::vector<glm::vec2> sPixelJitter;
    ASDoFAperture::Shape sSamplesShape; // shape sLensSamples were generated for
    glm::mat4 sCentralProjection;
    glm::mat4 sCentralModelview;
    // Axial CA of this frame's samples (key value) and the installed
    // sample's colour weight and inverse focus.
    F32 sAxialCA = 0.f;
    glm::vec4 sSampleWeight(1.f);
    F32 sSampleInvFocus = 0.f;

    // Time-sliced capture state (see requestCaptureSlice()).
    bool sSliceRequested = false;   // set by the caller before rawSnapshot()
    bool sSliced = false;           // this snapshot display() is a slice
    bool sPreviewRequested = false; // thumbnail: set before rawSnapshot()
    bool sPreview = false;          // this snapshot display() is a thumbnail
    bool sCapturePending = false;   // the capture needs more slices
    bool sCaptureStopped = false;   // Esc, time limit or view change
    F32 sCaptureSeconds = 0.f;      // rendering time of the current capture
    S32 sCaptureMaxSamples = 1;
    S32 sCapturePreviewSamples = 0; // samples in the last preview refresh
    LLTimer sSliceTimer;            // started at each slice

    bool sameMatrix(const glm::mat4& a, const glm::mat4& b)
    {
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                const F32 x = a[column][row];
                if (fabsf(x - b[column][row]) > 1e-6f * llmax(1.f, fabsf(x)))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool sameShape(const ASDoFAperture::Shape& a, const ASDoFAperture::Shape& b)
    {
        return a.mBlades == b.mBlades && a.mRoundness == b.mRoundness &&
            a.mRotation == b.mRotation && a.mAnamorphic == b.mAnamorphic;
    }

    bool sameKey(const AccumulationKey& a, const AccumulationKey& b)
    {
        return sameMatrix(a.mProjection, b.mProjection) && sameMatrix(a.mModelview, b.mModelview) &&
            a.mView == b.mView && a.mFNumber == b.mFNumber && a.mFocalLength == b.mFocalLength &&
            a.mDefaultFov == b.mDefaultFov && sameShape(a.mShape, b.mShape) &&
            a.mResidualBlur == b.mResidualBlur && a.mAxialCA == b.mAxialCA &&
            a.mWidth == b.mWidth && a.mHeight == b.mHeight;
    }

    bool nearMatrix(const glm::mat4& a, const glm::mat4& b, F32 tolerance)
    {
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                if (fabsf(a[column][row] - b[column][row]) > tolerance)
                {
                    return false;
                }
            }
        }
        return true;
    }

    // Same view up to camera drift a continuing capture ignores: about
    // 1 mm / 0.06 degrees (idle avatar, snapshot aspect round trips). Lens,
    // shape and size must match exactly.
    // Same image settings: size, lens, shape and smoothing (camera excluded).
    bool sameSettings(const AccumulationKey& a, const AccumulationKey& b)
    {
        return a.mView == b.mView && a.mFNumber == b.mFNumber && a.mFocalLength == b.mFocalLength &&
            a.mDefaultFov == b.mDefaultFov && sameShape(a.mShape, b.mShape) &&
            a.mResidualBlur == b.mResidualBlur && a.mAxialCA == b.mAxialCA &&
            a.mWidth == b.mWidth && a.mHeight == b.mHeight;
    }

    bool nearlySameView(const AccumulationKey& a, const AccumulationKey& b)
    {
        return nearMatrix(a.mProjection, b.mProjection, 1e-4f) && nearMatrix(a.mModelview, b.mModelview, 1e-3f) &&
            sameSettings(a, b);
    }

    bool matches(const Accumulator& slot, const AccumulationKey& key, F32 focus)
    {
        return slot.mHaveKey && sameKey(key, slot.mKey) &&
            fabsf(focus - slot.mKeyFocus) <= FOCUS_RESTART_TOLERANCE * fabsf(slot.mKeyFocus);
    }

    void restart(Accumulator& slot, const AccumulationKey& key, F32 focus)
    {
        slot.mKey = key;
        slot.mKeyFocus = focus;
        slot.mHaveKey = true;
        slot.mAccumulated = 0;
        slot.mSkyTime = gFrameTimeSeconds;
    }

    void release(Accumulator& slot)
    {
        slot.mTarget.release();
        slot.mAccumulated = 0;
        slot.mHaveKey = false;
    }

    void setFlag(const char* name, bool value)
    {
        if (gSavedSettings.getBOOL(name) != value)
        {
            gSavedSettings.setBOOL(name, value);
        }
    }

    // Floater controls enable via enabled_control; these non-persisted
    // flags mirror ASDepthOfFieldMode so controls without effect in the
    // selected renderer are greyed out. Written only on change.
    void syncModeFlags()
    {
        const S32 mode = gSavedSettings.getS32("ASDepthOfFieldMode");
        setFlag("ASDepthOfFieldUIAdvanced", mode == 1);
        setFlag("ASDepthOfFieldUIAperture", mode == APERTURE_MODE);
        setFlag("ASDepthOfFieldUIShape", mode == 1 || mode == APERTURE_MODE);
        setFlag("ASDepthOfFieldUIAxialCA",
                mode == APERTURE_MODE && gSavedSettings.getBOOL("ASDepthOfFieldApertureAxialCA"));
    }

    // Same state setPerspective() establishes: GL stack, cached globals
    // and the camera frustum planes that culling reads.
    void install(const glm::mat4& projection, const glm::mat4& modelview)
    {
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.loadMatrix(glm::value_ptr(projection));
        set_current_projection(projection);
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.loadMatrix(glm::value_ptr(modelview));
        set_current_modelview(modelview);
        LLViewerCamera::updateFrustumPlanes(*LLViewerCamera::getInstance());
    }

    // Installs lens sample sSampleIndex (central view when the lens is invalid).
    void installSample()
    {
        glm::mat4 projection = sCentralProjection;
        glm::mat4 modelview = sCentralModelview;
        sSampleWeight = glm::vec4(1.f);
        if (sLens.mValid)
        {
            const S32 sequence_index = sFirstSequenceIndex + sSampleIndex;
            // Axial CA: this sample's wavelength focuses at a shifted
            // distance, 1/S' = 1/S - s * alpha / (2 f) with alpha the
            // red-to-blue focal shift over the focal length (thin lens,
            // sensor fixed; red focuses farther, blue nearer), and is
            // weighted by colour.
            sSampleInvFocus = 1.f / sLens.mFocusDistance;
            if (sAxialCA > 0.f)
            {
                const F32 s = ASDoFAperture::spectralCoordinate((U32)sequence_index);
                sSampleInvFocus -= s * 0.5f * sAxialCA / sLens.mFocalLength;
                sSampleWeight = glm::vec4(ASDoFAperture::spectralWeights(s), 1.f);
            }
            // Beyond-infinity focus (negative inverse) is valid for the shear.
            const F32 inv_focus = fabsf(sSampleInvFocus) < 1e-6f ? 1e-6f : sSampleInvFocus;
            const glm::vec2 offset = ASDoFCamera::lensOffset(sLens, sLensSamples[sequence_index]);
            projection = ASDoFCamera::lensProjection(projection, offset, 1.f / inv_focus);
            projection = ASDoFCamera::jitterProjection(projection, sPixelJitter[sequence_index],
                                                       (F32)gGLViewport[2], (F32)gGLViewport[3]);
            modelview = ASDoFCamera::lensModelview(modelview, offset);
        }
        install(projection, modelview);
    }

    // Weighted fullscreen copy of source into the bound target. A positive
    // residual_scale softens it by the per-pixel residual disk (see
    // asDoFAccumulateF.glsl), reading this sample's depth.
    void drawWeighted(LLRenderTarget& source, const glm::vec4& weight, F32 residual_scale = 0.f)
    {
        sAccumulateProgram.bind();
        sAccumulateProgram.uniform4f(U_SAMPLE_WEIGHT, weight.x, weight.y, weight.z, weight.w);
        sAccumulateProgram.uniform1f(U_RESIDUAL_SCALE, residual_scale);
        sAccumulateProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, &source);
        if (residual_scale > 0.f)
        {
            sAccumulateProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen,
                                           true, LLTexUnit::TFO_POINT);
            sAccumulateProgram.uniform1f(U_RESIDUAL_MAX, RESIDUAL_MAX_PIXELS);
            sAccumulateProgram.uniform1f(U_INV_FOCUS, sSampleInvFocus);
            sAccumulateProgram.uniform2f(U_PROJ_Z, sCentralProjection[2][2], sCentralProjection[3][2]);
            sAccumulateProgram.uniform2f(U_TEXEL_SIZE, 1.f / source.getWidth(), 1.f / source.getHeight());
            // Golden-angle rotation per sequence index decorrelates the tap pattern.
            sAccumulateProgram.uniform1f(U_TAP_ROTATION,
                                         2.39996323f * (F32)(sFirstSequenceIndex + sSampleIndex));
        }
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        sAccumulateProgram.unbindTexture(LLShaderMgr::DIFFUSE_MAP);
        if (residual_scale > 0.f)
        {
            sAccumulateProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
        }
        sAccumulateProgram.unbind();
    }

    // Residual disk scale in output pixels per unit |1/focus - 1/distance|:
    // strength * R * pixels-per-unit-image-plane * sqrt(pi / N). Uses the
    // installed projection's [1][1], so snapshot tiles (zoomed) scale too.
    F32 residualScale()
    {
        static LLCachedControl<F32> strength(gSavedSettings, "ASDepthOfFieldApertureResidualBlur", 2.f);
        if (strength <= 0.f || !sLens.mValid)
        {
            return 0.f;
        }
        const F32 pixels_per_unit = sCentralProjection[1][1] * 0.5f * (F32)gPipeline.mRT->screen.getHeight();
        return strength * sLens.mApertureRadius * pixels_per_unit *
            sqrtf(F_PI / (F32)llmax(sMaxSamples, 1));
    }

    // (Re)allocates slot's sum at the screen size; false when that failed.
    bool ensureTarget(Accumulator& slot)
    {
        LLRenderTarget& screen = gPipeline.mRT->screen;
        if (slot.mTarget.getWidth() == screen.getWidth() && slot.mTarget.getHeight() == screen.getHeight())
        {
            return true;
        }
        slot.mTarget.release();
        slot.mAccumulated = 0;
        if (!slot.mTarget.allocate(screen.getWidth(), screen.getHeight(), GL_RGBA32F))
        {
            LL_WARNS_ONCE("ASDoF") << "Aperture DoF accumulation allocation failed; "
                                   << "showing unaveraged samples" << LL_ENDL;
            return false;
        }
        return true;
    }

    // Adds the current resolved screen (unit weight) to the running sum.
    void accumulateScreen()
    {
        Accumulator& slot = *sSlot;
        if (!ensureTarget(slot))
        {
            sAccumulationFailed = true;
            return;
        }

        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        const F32 residual = residualScale();
        slot.mTarget.bindTarget();
        if (slot.mAccumulated == 0)
        { // First sample of a new average overwrites.
            LLGLDisable blend(GL_BLEND);
            drawWeighted(gPipeline.mRT->screen, sSampleWeight, residual);
        }
        else
        {
            LLGLEnable blend(GL_BLEND);
            gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
            drawWeighted(gPipeline.mRT->screen, sSampleWeight, residual);
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
        }
        slot.mTarget.flush();
        ++slot.mAccumulated;
    }

    // A new capture of the view the live average already developed starts
    // from that average (the sample sequence continues where it stopped).
    void seedCaptureFromLive()
    {
        if (sLive.mAccumulated <= 0 || !ensureTarget(sCapture) ||
            sLive.mTarget.getWidth() != sCapture.mTarget.getWidth() ||
            sLive.mTarget.getHeight() != sCapture.mTarget.getHeight())
        {
            return;
        }
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        sCapture.mTarget.bindTarget();
        drawWeighted(sLive.mTarget, glm::vec4(1.f));
        sCapture.mTarget.flush();
        sCapture.mAccumulated = sLive.mAccumulated;
        sCapture.mSkyTime = sLive.mSkyTime;
    }

    // Seconds one blocking snapshot display() call may spend on samples.
    // The whole snapshot (all high-res tiles, ceil(zoom)^2 of them) stays
    // within ASDepthOfFieldApertureSnapshotMaxSeconds, capped well below
    // the simulator connection timeout.
    F32 blockingBudgetSeconds()
    {
        const F32 total = llclamp(gSavedSettings.getF32("ASDepthOfFieldApertureSnapshotMaxSeconds"),
                                  1.f, MAX_BLOCKING_CAPTURE_SECONDS);
        const F32 tiles_per_side = (F32)llceil(llmax(LLViewerCamera::getInstance()->getZoomFactor(), 1.f));
        return total / (tiles_per_side * tiles_per_side);
    }

    bool captureCancelRequested()
    {
#if LL_WINDOWS
        // A blocking capture suspends the main loop (and its input), and a
        // sliced one may miss a short press between slices: read the key
        // state directly (down now, or pressed since the last query), only
        // while the viewer window is in front.
        HWND window = (HWND)gViewerWindow->getPlatformWindow();
        return window && GetForegroundWindow() == window && (GetAsyncKeyState(VK_ESCAPE) & 0x8001);
#else
        return false;
#endif
    }

    // Long blocking captures render offscreen for many seconds: show a
    // progress screen in the window meanwhile. Restores the bound
    // framebuffer and viewport; the next sample reinstalls the matrices.
    void presentCaptureProgress()
    {
        GLint previous_fbo = 0;
        GLint previous_viewport[4] = {};
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
        glGetIntegerv(GL_VIEWPORT, previous_viewport);

        const LLRect window = gViewerWindow->getWindowRectRaw();
        const S32 width = window.getWidth();
        const S32 height = window.getHeight();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        gGL.setColorMask(true, true);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        {
            LLGLDepthTest depth(GL_FALSE, GL_FALSE);
            LLGLEnable blend(GL_BLEND);
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            gl_state_for_2d(width, height);
            // Raw window pixels: drop the UI offset/scale a snapshot display()
            // leaves behind, and undo the font's UI scaling of positions.
            gGL.pushUIMatrix();
            gGL.loadUIIdentity();
            const LLCoordGL saved_font_origin = LLFontGL::sCurOrigin;
            LLFontGL::sCurOrigin.set(0, 0);
            gUIProgram.bind();

            const S32 accumulated = sSlot->mAccumulated;
            const F32 fraction = llclamp((F32)accumulated / (F32)llmax(sMaxSamples, 1), 0.f, 1.f);
#if LL_WINDOWS
            const char* hint = "  (Esc to stop)";
#else
            const char* hint = "";
#endif
            const std::string text = llformat("Capturing depth of field... %d / %d%s",
                                              accumulated, sMaxSamples, hint);
            const LLColor4 yellow(1.f, 0.85f, 0.1f, 1.f);
            constexpr S32 BAR_WIDTH = 400;
            constexpr S32 BAR_HEIGHT = 8;
            const S32 centre_x = width / 2;
            const S32 centre_y = height / 2;
            LLFontGL::getFontSansSerif()->renderUTF8(text, 0,
                (F32)centre_x / llmax(LLFontGL::sScaleX, 0.01f), (F32)(centre_y + 12) / llmax(LLFontGL::sScaleY, 0.01f),
                yellow, LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            const S32 left = centre_x - BAR_WIDTH / 2;
            gl_rect_2d(left, centre_y, left + BAR_WIDTH, centre_y - BAR_HEIGHT,
                       LLColor4(0.3f, 0.3f, 0.3f, 1.f), true);
            gl_rect_2d(left, centre_y, left + (S32)(BAR_WIDTH * fraction), centre_y - BAR_HEIGHT, yellow, true);
            gGL.flush();
            gUIProgram.unbind();
            LLFontGL::sCurOrigin = saved_font_origin;
            gGL.popUIMatrix();
        }

        gViewerWindow->getWindow()->swapBuffers();
        glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);
        glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
    }

    // Lowest bottom edge (UI screen coordinates) of the visible top bars:
    // navigation, favorites and the chiclet (notification) bar. Found by
    // name once they exist; the snap rect does not cover the favorites bar.
    S32 topBarsBottom(S32 fallback)
    {
        static const char* const NAMES[] = { "navigation_bar", "favorite", "chiclet_bar" };
        static LLHandle<LLView> handles[LL_ARRAY_SIZE(NAMES)];
        static U32 lookup_frame = 0;
        LLView* root = gViewerWindow->getRootView();
        const U32 frame = LLFrameTimer::getFrameCount();
        const bool lookup = root && frame >= lookup_frame;
        if (lookup)
        { // Recursive search: retried at most every 60 frames until found.
            lookup_frame = frame + 60;
        }

        S32 bottom = fallback;
        for (size_t i = 0; i < LL_ARRAY_SIZE(NAMES); ++i)
        {
            if (!handles[i].get() && lookup)
            {
                if (LLView* view = root->findChildView(NAMES[i], true))
                {
                    handles[i] = view->getHandle();
                }
            }
            LLView* view = handles[i].get();
            if (view && view->isInVisibleChain() && view->getRect().getHeight() > 0)
            {
                bottom = llmin(bottom, view->calcScreenRect().mBottom);
            }
        }
        return bottom;
    }

    void finishFrame()
    {
        if (sOcclusionOverridden)
        {
            LLPipeline::sUseOcclusion = sSavedOcclusion;
            sOcclusionOverridden = false;
        }
        sActive = false;
        sSampleIndex = 0;
    }

    // Plan for a snapshot display(): picks the capture average, continues
    // or restarts it and decides whether this slice renders samples.
    void planSnapshot(const AccumulationKey& key, bool sliced)
    {
        const bool continuing = sliced && sCapturePending;
        sCapturePending = false;
        if (sliced && !continuing)
        { // A new sliced capture: fresh time limit and stop state.
            sCaptureSeconds = 0.f;
            sCaptureStopped = false;
            captureCancelRequested(); // discards an Esc press from before this capture
        }
        sSlot = &sCapture;
        sMaxSamples = llclamp(gSavedSettings.getS32("ASDepthOfFieldApertureSnapshotSamples"), 1, MAX_SAMPLES);
        sCaptureMaxSamples = sMaxSamples;

        if (!matches(sCapture, key, sFocusDistance))
        {
            // A new size or lens setting restarts (the caller wants a new
            // image); only camera movement ends a capture early.
            if (continuing && sCapture.mAccumulated > 0 && sameSettings(key, sCapture.mKey))
            {
                if (nearlySameView(key, sCapture.mKey))
                { // Drift or focus smoothing between slices: keep rendering
                  // the requested view with its frozen focus.
                    sCentralProjection = sCapture.mKey.mProjection;
                    sCentralModelview = sCapture.mKey.mModelview;
                }
                else
                { // The camera moved (or Esc reset the view): finish with
                  // the average of the requested view.
                    sCaptureStopped = true;
                    LL_INFOS("ASDoF") << "Aperture DoF capture stopped by a view change at "
                                      << sCapture.mAccumulated << " / " << sMaxSamples << " samples" << LL_ENDL;
                }
            }
            else
            { // New snapshot (size, tile or view): restart, seeded by an
              // identical live average (window-size snapshots are instant).
                restart(sCapture, key, sFocusDistance);
                sCaptureSeconds = 0.f; // also when a pending capture restarts
                sCapturePreviewSamples = 0;
                if (matches(sLive, key, sFocusDistance))
                {
                    seedCaptureFromLive();
                }
                LL_INFOS("ASDoF") << "Aperture DoF capture started: " << key.mWidth << "x" << key.mHeight
                                  << ", " << sMaxSamples << " samples, " << sCapture.mAccumulated
                                  << " from the live view, " << (sliced ? "sliced" : "blocking") << LL_ENDL;
            }
        }
        if (sliced && !sCaptureStopped)
        {
            sCaptureStopped = captureCancelRequested() ||
                sCaptureSeconds >= llclamp(gSavedSettings.getF32("ASDepthOfFieldApertureSnapshotMaxSeconds"),
                                           1.f, MAX_SLICED_CAPTURE_SECONDS);
            if (sCaptureStopped)
            {
                LL_INFOS("ASDoF") << "Aperture DoF capture stopped at " << sCapture.mAccumulated
                                  << " / " << sMaxSamples << " samples" << LL_ENDL;
            }
        }
    }
}

namespace ASDoFRenderer
{
    void registerUICallbacks()
    {
        syncModeFlags();
        for (const char* name : { "ASDepthOfFieldMode", "ASDepthOfFieldApertureAxialCA" })
        {
            if (LLControlVariable* control = gSavedSettings.getControl(name))
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
                    {
                        syncModeFlags();
                    });
            }
        }

        // "Freeze animations" in the DoF floater: the same global time-factor
        // freeze as Advanced > Animation > Freeze Animations and My Lights.
        if (LLControlVariable* control = gSavedSettings.getControl("ASDepthOfFieldFreezeAnimations"))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
                {
                    set_all_animation_time_factors(value.asBoolean() ? 0.f : 1.f);
                });
        }
    }

    void registerShaders(std::vector<LLGLSLShader*>& shaders)
    {
        shaders.push_back(&sAccumulateProgram);
    }

    bool createShaders(S32 shader_level)
    {
        sAccumulateProgram.mName = "AyaneStorm Aperture DoF Accumulate Shader";
        sAccumulateProgram.mShaderFiles.clear();
        sAccumulateProgram.clearPermutations();
        sAccumulateProgram.mShaderFiles.emplace_back("interface/copyV.glsl", GL_VERTEX_SHADER);
        sAccumulateProgram.mShaderFiles.emplace_back("deferred/asDoFAccumulateF.glsl", GL_FRAGMENT_SHADER);
        sAccumulateProgram.mShaderLevel = shader_level;
        return sAccumulateProgram.createShader();
    }

    void unloadShaders()
    {
        sAccumulateProgram.unload();
        releaseResources();
    }

    void releaseResources()
    {
        release(sLive);
        release(sCapture);
        sSlot = &sLive;
        sCapturePending = false;
    }

    bool isEnabled()
    {
        return LLPipeline::RenderDepthOfField && !gCubeSnapshot &&
            (LLPipeline::RenderDepthOfFieldInEditMode || !LLToolMgr::getInstance()->inBuildMode()) &&
            gSavedSettings.getS32("ASDepthOfFieldMode") == APERTURE_MODE;
    }

    void setFocusDistance(F32 distance)
    {
        sFocusDistance = distance;
    }

    void requestCaptureSlice()
    {
        sSliceRequested = true;
    }

    void requestPreviewCapture()
    {
        sPreviewRequested = true;
    }

    bool isCapturePending()
    {
        return sCapturePending;
    }

    F32 starRotationTime(F32 frame_time)
    {
        return sActive && sLens.mValid && sPlan == FramePlan::ACCUMULATE ? sSlot->mSkyTime : frame_time;
    }

    F32 starTwinkleTime(F32 time)
    {
        if (!(sActive && sLens.mValid && sPlan == FramePlan::ACCUMULATE))
        {
            return time;
        }
        // Golden-ratio steps over the shader's 1.25 s twinkle period: every
        // lens sample gets its own twinkle state, so the average is the mean
        // brightness whatever the samples-per-frame grouping.
        const F32 index = (F32)(sFirstSequenceIndex + sSampleIndex);
        return fmodf(index * 0.618034f, 1.f) * 1.25f;
    }

    bool isSceneFrozen()
    {
        static LLCachedControl<bool> freeze(gSavedSettings, "ASDepthOfFieldFreezeAnimations", false);
        return sCapturePending || freeze;
    }

    bool setProgressThumbnail(bool active)
    {
        static bool progress_thumbnail = false;
        const bool was_active = progress_thumbnail;
        progress_thumbnail = active;
        return was_active;
    }

    bool isCapturePreviewDue()
    {
        const S32 done = sCapture.mAccumulated;
        const S32 next = sCapturePreviewSamples == 0 ? CAPTURE_PREVIEW_FIRST :
            sCapturePreviewSamples + CAPTURE_PREVIEW_INTERVAL;
        if (!sCapturePending || done < next)
        {
            return false;
        }
        sCapturePreviewSamples = done;
        return true;
    }

    void resumeLiveView()
    {
        // rawSnapshot() leaves swapping off so the next frame is not shown;
        // with a slice every main-loop pass no live frame would ever be.
        gDisplaySwapBuffers = true;
    }

    void beginSample(bool for_snapshot)
    {
        if (sOcclusionOverridden)
        { // Previous frame never reached finishFrame(): restore before saving again.
            LLPipeline::sUseOcclusion = sSavedOcclusion;
            sOcclusionOverridden = false;
        }
        sActive = false;
        sSampleIndex = 0;
        sSampleCount = 1;
        sAccumulationFailed = false;
        sAxialCA = 0.f;
        // One-shot: only the snapshot display() right after the request is a slice.
        const bool slice_requested = sSliceRequested;
        const bool preview_requested = sPreviewRequested;
        sSliceRequested = false;
        sPreviewRequested = false;
        sSliced = false;
        sPreview = false;
        // Covers settings loaded after registration; cheap when unchanged.
        syncModeFlags();
        // Deferred display() clears for_snapshot before this call (sky hack),
        // so it is only set on the non-deferred path; snapshots are detected
        // with gSnapshot below.
        if (for_snapshot || !isEnabled())
        {
            // DoF off / other modes keep no aperture resources.
            releaseResources();
            return;
        }

        sCentralProjection = get_current_projection();
        sCentralModelview = get_current_modelview();

        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const F32 default_fov = LLPipeline::CameraFieldOfView * DEG_TO_RAD;
        const ASDoFAperture::Shape shape = ASDoFAperture::shapeFromSettings();

        const S32 debug_sample = llmin(gSavedSettings.getS32("ASDepthOfFieldApertureDebugSample"),
                                       MAX_SAMPLES - 1);
        if (debug_sample >= 0)
        { // One unaveraged lens sample at the live focus.
            releaseResources();
            sPlan = FramePlan::DEBUG;
            sLens = ASDoFCamera::makeLens(camera->getView(), default_fov,
                LLPipeline::CameraFocalLength, LLPipeline::CameraFNumber, sFocusDistance);
            sFirstSequenceIndex = debug_sample;
            if (sLens.mValid)
            {
                ASDoFAperture::generate(shape, debug_sample + 1, sLensSamples, sPixelJitter);
                sSamplesShape = shape;
            }
            installSample();
            sActive = true;
            return;
        }

        AccumulationKey key;
        key.mProjection = sCentralProjection;
        key.mModelview = sCentralModelview;
        key.mView = camera->getView();
        key.mFNumber = LLPipeline::CameraFNumber;
        key.mFocalLength = LLPipeline::CameraFocalLength;
        key.mDefaultFov = default_fov;
        key.mShape = shape;
        key.mResidualBlur = gSavedSettings.getF32("ASDepthOfFieldApertureResidualBlur");
        static LLCachedControl<bool> axial_ca(gSavedSettings, "ASDepthOfFieldApertureAxialCA", false);
        static LLCachedControl<F32> axial_ca_percent(gSavedSettings, "ASDepthOfFieldApertureAxialCAStrength", 0.2f);
        key.mAxialCA = axial_ca ? llclamp((F32)axial_ca_percent, 0.f, 2.f) * 0.01f : 0.f;
        sAxialCA = key.mAxialCA;
        key.mWidth = gPipeline.mRT->screen.getWidth();
        key.mHeight = gPipeline.mRT->screen.getHeight();

        bool accumulate = true;
        if (gSnapshot && preview_requested)
        { // Thumbnail: the live view's average plus a few samples; any
          // pending capture stays untouched.
            sPreview = true;
            sSliceTimer.reset();
            sSlot = &sLive;
            sMaxSamples = llclamp(gSavedSettings.getS32("ASDepthOfFieldApertureMaxSamples"), 1, MAX_SAMPLES);
            if (!matches(sLive, key, sFocusDistance))
            {
                restart(sLive, key, sFocusDistance);
            }
        }
        else if (gSnapshot)
        {
            // Tiled (zoomed) snapshots restart per tile and stay blocking.
            sSliced = slice_requested && camera->getZoomFactor() <= 1.f;
            planSnapshot(key, sSliced);
            sSliceTimer.reset();
            if (sSliced && sCaptureStopped)
            {
                accumulate = false;
            }
        }
        else
        {
            if (sCapturePending && sSliceTimer.getElapsedTimeF32() > CAPTURE_ABANDON_SECONDS)
            { // The capture's caller went away (e.g. snapshot floater closed).
                sCapturePending = false;
            }
            if (!sCapturePending && sCapture.mHaveKey)
            { // Finished captures keep no (possibly huge) snapshot-size sum.
                release(sCapture);
            }
            sSlot = &sLive;
            sMaxSamples = llclamp(gSavedSettings.getS32("ASDepthOfFieldApertureMaxSamples"), 1, MAX_SAMPLES);
            sLiveMaxSamples = sMaxSamples;
            if (!matches(sLive, key, sFocusDistance))
            { // Moving: fast central view; averaging restarts once still.
                restart(sLive, key, sFocusDistance);
                sPlan = FramePlan::PINHOLE;
                sLens.mValid = false;
                installSample();
                sActive = true;
                return;
            }
            // Live frames between capture slices stay cheap: the capture
            // gets the rendering time.
            accumulate = !sCapturePending;
        }

        Accumulator& slot = *sSlot;
        sLens = ASDoFCamera::makeLens(key.mView, default_fov, key.mFocalLength, key.mFNumber, slot.mKeyFocus);
        if (!sLens.mValid)
        { // Invalid lens (e.g. focus inside the focal length): nothing clamped.
            slot.mAccumulated = 0;
            sPlan = FramePlan::PINHOLE;
        }
        else if (slot.mAccumulated >= sMaxSamples || (!accumulate && slot.mAccumulated > 0))
        {
            sPlan = FramePlan::CONVERGED;
            sLens.mValid = false;
        }
        else if (!accumulate)
        {
            sPlan = FramePlan::PINHOLE;
            sLens.mValid = false;
        }
        else
        {
            sPlan = FramePlan::ACCUMULATE;
            // Blocking snapshots finish the whole average in one capture;
            // slices stop on time, live frames add a few samples each.
            const S32 per_frame = sPreview ? PREVIEW_SAMPLES : gSnapshot ? MAX_SAMPLES :
                llclamp(gSavedSettings.getS32("ASDepthOfFieldApertureSamples"), 1, MAX_SAMPLES);
            sSampleCount = llmin(per_frame, sMaxSamples - slot.mAccumulated);
            sFirstSequenceIndex = slot.mAccumulated;
            const S32 needed = slot.mAccumulated + sSampleCount;
            if ((S32)sLensSamples.size() < needed || !sameShape(sSamplesShape, shape))
            { // Nested sequence: generate the whole prefix once per target.
                ASDoFAperture::generate(shape, llmax(needed, sMaxSamples), sLensSamples, sPixelJitter);
                sSamplesShape = shape;
            }
            // Central-view occlusion must not reject surfaces another lens position sees.
            sSavedOcclusion = LLPipeline::sUseOcclusion;
            LLPipeline::sUseOcclusion = 0;
            sOcclusionOverridden = true;
        }

        installSample();
        sActive = true;
    }

    void endSample()
    {
        if (!sActive)
        {
            return;
        }
        if (sPlan == FramePlan::ACCUMULATE && !sAccumulationFailed)
        {
            accumulateScreen();
        }
        install(sCentralProjection, sCentralModelview);
    }

    bool isRepeatSample()
    {
        return sActive && sSampleIndex > 0;
    }

    void renderRemainingSamples(LLCullResult& result)
    {
        if (!sActive)
        {
            return;
        }

        LLViewerCamera& camera = *LLViewerCamera::getInstance();
        LLRenderTarget& deferred = gPipeline.mRT->deferredScreen;
        Accumulator& slot = *sSlot;
        LLTimer progress_timer;
        const bool blocking = gSnapshot && !sSliced && !sPreview;
        const F32 blocking_budget = blocking ? blockingBudgetSeconds() : 0.f;
        if (blocking)
        {
            captureCancelRequested(); // discards an Esc press from before this capture
        }
        for (sSampleIndex = 1; sSampleIndex < sSampleCount; ++sSampleIndex)
        {
            LL_PROFILE_GPU_ZONE("aperture dof sample");
            LLAppViewer::instance()->pingMainloopTimeout("Display:ApertureDoF");
            if (sSliced && sSliceTimer.getElapsedTimeF32() > CAPTURE_SLICE_SECONDS)
            { // Back to the main loop; the next slice continues.
                break;
            }
            if (blocking)
            {
                // The main loop, including simulator networking, is suspended
                // during a blocking capture: stop before the region drops the connection.
                if (sSliceTimer.getElapsedTimeF32() > blocking_budget)
                {
                    LL_INFOS("ASDoF") << "Aperture DoF capture reached its " << blocking_budget
                                      << " s limit at " << slot.mAccumulated << " / " << sMaxSamples
                                      << " samples" << LL_ENDL;
                    break;
                }
                if (progress_timer.getElapsedTimeF32() > CAPTURE_PROGRESS_INTERVAL)
                {
                    presentCaptureProgress();
                    progress_timer.reset();
                }
                if (captureCancelRequested())
                { // Keep what is done.
                    LL_INFOS("ASDoF") << "Aperture DoF capture stopped at " << slot.mAccumulated
                                      << " / " << sMaxSamples << " samples" << LL_ENDL;
                    break;
                }
            }
            installSample();

            // Mirrors display()'s per-view sequence: cull, sort, G-buffer,
            // lighting and transparency. LOD and alpha-sort distances use
            // the unchanged central camera origin, so they stay frozen.
            LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
            gPipeline.updateCull(camera, result);
            gPipeline.stateSort(camera, result);

            gGL.setColorMask(true, true);
            deferred.bindTarget();
            glClearColor(1, 0, 1, 1);
            deferred.clear();
            gGL.setColorMask(true, true);
            gPipeline.renderGeomDeferred(camera, false);
            deferred.flush();

            gPipeline.renderDeferredLighting(); // endSample() accumulates
        }

        // The capture progress screen leaves 2D matrices on the GL stack.
        install(sCentralProjection, sCentralModelview);

        // Show the running average (converged frames reuse it unchanged).
        if ((sPlan == FramePlan::ACCUMULATE || sPlan == FramePlan::CONVERGED) &&
            !sAccumulationFailed && slot.mAccumulated > 0)
        {
            LLGLDepthTest depth(GL_FALSE, GL_FALSE);
            LLGLDisable blend(GL_BLEND);
            LLRenderTarget& screen = gPipeline.mRT->screen;
            screen.bindTarget();
            drawWeighted(slot.mTarget, glm::vec4(1.f / slot.mAccumulated));
            screen.flush();
        }
        glClearColor(0.f, 0.f, 0.f, 0.f);

        if (sSliced)
        {
            sCaptureSeconds += sSliceTimer.getElapsedTimeF32();
            sCapturePending = sPlan == FramePlan::ACCUMULATE && !sAccumulationFailed &&
                !sCaptureStopped && slot.mAccumulated < sMaxSamples;
            sSliceTimer.reset(); // abandon timeout counts from the slice end
        }
        finishFrame();
    }

    void drawProgress()
    {
        static LLCachedControl<bool> show_progress(gSavedSettings, "ASDepthOfFieldApertureShowProgress", true);
        // Shown only while samples are being added: a live frame that
        // accumulated, or a pending snapshot capture (always, even when
        // the optional live counter is off). Hidden once converged,
        // while moving and in the debug view.
        const bool live_accumulating = show_progress && sPlan == FramePlan::ACCUMULATE;
        if (gSnapshot || !isEnabled() || (!live_accumulating && !sCapturePending))
        {
            return;
        }

        S32 accumulated = sLive.mAccumulated;
        S32 target = sLiveMaxSamples;
        std::string text;
        if (sCapturePending)
        {
            accumulated = sCapture.mAccumulated;
            target = sCaptureMaxSamples;
#if LL_WINDOWS
            text = llformat("Snapshot DoF %d / %d  (Esc to stop)", accumulated, target);
#else
            text = llformat("Snapshot DoF %d / %d", accumulated, target);
#endif
        }
        else
        {
            text = llformat("DoF %d / %d", accumulated, target);
        }
        const F32 fraction = llclamp((F32)accumulated / (F32)llmax(target, 1), 0.f, 1.f);
        const LLColor4 yellow(1.f, 0.85f, 0.1f, 1.f);

        // Same space as the viewer's debug text: scaled UI coordinates.
        gUIProgram.bind();
        gGL.pushMatrix();
        gGL.pushUIMatrix();
        {
            const LLVector2& scale = gViewerWindow->getDisplayScale();
            gGL.scaleUI(scale.mV[VX], scale.mV[VY], 1.f);

            // Top right of the floater snap area (clear of side toolbars),
            // below the navigation, favorites and notification bars.
            LLRect area = gViewerWindow->getWorldViewRectScaled();
            if (gFloaterView)
            {
                gFloaterView->localRectToScreen(gFloaterView->getSnapRect(), &area);
            }
            constexpr S32 MARGIN = 10;
            constexpr S32 BAR_WIDTH = 160;
            constexpr S32 BAR_HEIGHT = 6;
            const S32 right = area.mRight - MARGIN;
            const S32 left = right - BAR_WIDTH;
            const S32 top = topBarsBottom(area.mTop) - MARGIN;

            LLFontGL* font = LLFontGL::getFontSansSerif();
            font->renderUTF8(text, 0, (F32)right, (F32)top, yellow,
                             LLFontGL::RIGHT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);

            const S32 bar_top = top - (S32)font->getLineHeight() - 4;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gl_rect_2d(left, bar_top, right, bar_top - BAR_HEIGHT, LLColor4(0.f, 0.f, 0.f, 0.5f), true);
            gl_rect_2d(left, bar_top, left + (S32)(BAR_WIDTH * fraction), bar_top - BAR_HEIGHT, yellow, true);
        }
        gGL.popUIMatrix();
        gGL.popMatrix();
        gGL.flush();
        gUIProgram.unbind();
    }
}
