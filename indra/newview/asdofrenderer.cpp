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
#include "asdoflive.h"
#include "llappviewer.h"
#include "llcharacter.h"
#include "llfloater.h"
#include "llfontgl.h"
#include "llgl.h"
#include "llnotificationsutil.h"
#include "llprogressview.h"
#include "llrender2dutils.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llglslshader.h"
#include "llmotioncontroller.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrootview.h"
#include "llspatialpartition.h"
#include "lltoolmgr.h"
#include "lluictrl.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llwindow.h"
#include "pipeline.h"

#include "llfocusmgr.h"

// Capture cancel: direct Esc reads (see captureCancelRequested()).
#if LL_WINDOWS
#include "llwin32headers.h"
#elif LL_SDL2
#include "SDL2/SDL.h"
#elif LL_DARWIN
#include <CoreGraphics/CGEventSource.h>
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
    // frame) runs between slices. Live frames are UI-only during a capture
    // (isLiveViewFrozen()), so short slices keep typing responsive at little
    // cost to sampling.
    constexpr F32 CAPTURE_SLICE_SECONDS = 0.15f;
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
    // Cap on the final smoothing radius (ASDepthOfFieldApertureSmoothing)
    // and the sample count below which it stops growing (a few samples
    // would otherwise ask for more than the blur itself).
    constexpr F32 SMOOTHING_MAX_PIXELS = 32.f;
    constexpr S32 SMOOTHING_MIN_SAMPLES = 4;

    const LLStaticHashedString U_SAMPLE_WEIGHT("sample_weight");
    const LLStaticHashedString U_RESIDUAL_SCALE("residual_scale");
    const LLStaticHashedString U_RESIDUAL_MAX("residual_max");
    const LLStaticHashedString U_INV_FOCUS("inv_focus");
    const LLStaticHashedString U_PROJ_Z("proj_z");
    const LLStaticHashedString U_TEXEL_SIZE("texel_size");
    const LLStaticHashedString U_TAP_ROTATION("tap_rotation");
    const LLStaticHashedString U_LENS_MODE("lens_mode");
    const LLStaticHashedString U_LENS_POS("lens_pos");
    const LLStaticHashedString U_CAT_EYE("cat_eye");
    const LLStaticHashedString U_FIELD_SCALE("field_scale");
    const LLStaticHashedString U_SA_STRENGTH("sa_strength");
    const LLStaticHashedString U_SA_COC_SCALE("sa_coc_scale");
    const LLStaticHashedString U_PUPIL_R2("pupil_r2");
    const LLStaticHashedString U_HL_STRENGTH("hl_strength");
    const LLStaticHashedString U_HL_THRESHOLD("hl_threshold");
    const LLStaticHashedString U_SHOW_SMOOTHING("show_smoothing");
    const LLStaticHashedString U_HAS_STAR_MASK("has_star_mask");

    // asDoFAccumulateF.glsl lens_mode.
    enum class Draw
    {
        COPY = 0,    // plain weighted copy (seeding)
        SAMPLE = 1,  // one lens sample: residual, cat's eye, spherical weights
        AVERAGE = 2, // normalized average: cat's eye brightness compensation
        MASKED = 3,  // AVERAGE premultiplied by the defocus mask (smoothing input)
        SMOOTHED = 4, // AVERAGE with the final smoothing
        SOURCES = 5  // one lens sample's small-source indicator (bokeh map)
    };

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
        F32 mCatEye = 0.f;      // barrel shift at the frame corner; 0 = off
        bool mCatEyeDarken = false; // keep the physical corner light loss
        F32 mSpherical = 0.f;   // spherical aberration; 0 = off
        F32 mHighlight = 0.f;   // artistic highlight boost; 0 = off
        F32 mHighlightThreshold = 0.f;
        U32 mWidth = 0;
        U32 mHeight = 0;
    };

    // A running average: full-precision linear HDR sum, unit weights.
    struct Accumulator
    {
        LLRenderTarget mTarget;
        LLRenderTarget mSmooth; // mipmapped masked average (final smoothing)
        // Per-pixel sum of the small-source indicator over the samples: the
        // dots every small light left, i.e. exactly where its bokeh is.
        LLRenderTarget mSources;
        bool mSourcesFailed = false; // mSources misses samples: no smoothing
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
    // Live frame rejected by makeLens (focus at or inside the zoom-adjusted
    // focal length, or behind the camera): pinhole, reported by the counter.
    bool sInvalidFocus = false;
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
    ASDoFCamera::Lens sAverageLens; // lens of the shown average (valid while sLens is not)
    // This lens sample's stars, drawn a second time by the sky pool
    // (beginStarMask()); sStarMaskDrawn: drawn since the last accumulation.
    LLRenderTarget sStarMask;
    bool sStarMaskDrawn = false;
    std::vector<glm::vec2> sLensSamples;
    std::vector<glm::vec2> sPixelJitter;
    ASDoFAperture::Shape sSamplesShape; // shape sLensSamples were generated for
    glm::mat4 sCentralProjection;
    glm::mat4 sCentralModelview;
    // Axial CA of this frame's samples (key value) and the installed
    // sample's colour weight and inverse focus.
    F32 sAxialCA = 0.f;
    // Lens character of this frame (key values).
    F32 sCatEye = 0.f;
    bool sCatEyeDarken = false;
    F32 sSpherical = 0.f;
    F32 sHighlight = 0.f;
    F32 sHighlightThreshold = 1.f;
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
    bool sEscapePressed = false;    // Esc key-down during a pending capture

    // World freeze while a sliced capture is pending (setWorldFrozen()).
    bool sWorldFrozen = false;
    std::vector<LLAnimPauseRequest> sPauseHandles;
    // The last slice's finished partial capture (post-processed, before any
    // UI), reduced to at most twice the window fit and mipmapped; the
    // UI-only live frames show it instead of the world.
    LLRenderTarget sFrozenView;
    bool sFrozenKept = false;
    bool sFrozenFailed = false;   // allocation failed: no retry this capture
    bool sLiveViewFrozen = false; // this live frame shows the kept image

    void releaseFrozenView()
    {
        sFrozenView.release();
        sFrozenKept = false;
    }

    // Freezes or thaws the world with the same means as the snapshot
    // floater's freeze frame (drawables and non-avatar idle updates stop,
    // avatars pause); positions received meanwhile apply on thaw.
    void setWorldFrozen(bool frozen)
    {
        if (frozen == sWorldFrozen)
        {
            return;
        }
        sWorldFrozen = frozen;
        if (frozen)
        {
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                sPauseHandles.push_back(character->requestPause());
            }
        }
        else
        {
            sPauseHandles.clear();
            releaseFrozenView();
            sFrozenFailed = false;
            // The world moves on from here: the live view averages afresh
            // instead of continuing a sum from before the capture.
            sLive.mHaveKey = false;
            sLive.mAccumulated = 0;
        }
        LLPipeline::FreezeTime = gSavedSettings.getBOOL("FreezeTime") || frozen;
        LL_INFOS("ASDoF") << "Aperture DoF capture world freeze " << (frozen ? "on" : "off") << LL_ENDL;
    }

    // Ends a pending capture whose caller stopped calling (e.g. snapshot
    // floater closed) and thaws the world once no capture is pending (also
    // after a blocking snapshot replaced it). Checked by every live frame.
    void checkCaptureAbandoned()
    {
        if (sCapturePending && sSliceTimer.getElapsedTimeF32() > CAPTURE_ABANDON_SECONDS)
        {
            sCapturePending = false;
        }
        if (!sCapturePending)
        {
            setWorldFrozen(false);
        }
    }

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
            a.mCatEye == b.mCatEye && a.mCatEyeDarken == b.mCatEyeDarken && a.mSpherical == b.mSpherical &&
            a.mHighlight == b.mHighlight && a.mHighlightThreshold == b.mHighlightThreshold &&
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
            a.mCatEye == b.mCatEye && a.mCatEyeDarken == b.mCatEyeDarken && a.mSpherical == b.mSpherical &&
            a.mHighlight == b.mHighlight && a.mHighlightThreshold == b.mHighlightThreshold &&
            a.mWidth == b.mWidth && a.mHeight == b.mHeight;
    }

    bool nearlySameView(const AccumulationKey& a, const AccumulationKey& b)
    {
        return nearMatrix(a.mProjection, b.mProjection, 1e-4f) && nearMatrix(a.mModelview, b.mModelview, 1e-3f) &&
            sameSettings(a, b);
    }

    F32 maxMatrixDelta(const glm::mat4& a, const glm::mat4& b)
    {
        F32 delta = 0.f;
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                delta = llmax(delta, fabsf(a[column][row] - b[column][row]));
            }
        }
        return delta;
    }

    // Diagnostic: while the live average keeps restarting (never gets to
    // accumulate), log which part of the key changes, at most every 2 s.
    void logLiveRestart(const Accumulator& slot, const AccumulationKey& key, F32 focus)
    {
        static S32 streak = 0;
        static U32 last_frame = 0;
        static LLTimer log_timer;
        streak = (last_frame + 1 == LLFrameTimer::getFrameCount()) ? streak + 1 : 1;
        last_frame = LLFrameTimer::getFrameCount();
        if (!slot.mHaveKey || streak < 10 || log_timer.getElapsedTimeF32() < 2.f)
        {
            return;
        }
        log_timer.reset();
        LL_INFOS("ASDoF") << "Aperture DoF live average restarting for " << streak << " frames:"
                          << " focus " << slot.mKeyFocus << " -> " << focus
                          << (fabsf(focus - slot.mKeyFocus) > FOCUS_RESTART_TOLERANCE * fabsf(slot.mKeyFocus) ? " (changed)" : "")
                          << ", projection delta " << maxMatrixDelta(key.mProjection, slot.mKey.mProjection)
                          << (sameMatrix(key.mProjection, slot.mKey.mProjection) ? "" : " (changed)")
                          << ", modelview delta " << maxMatrixDelta(key.mModelview, slot.mKey.mModelview)
                          << (sameMatrix(key.mModelview, slot.mKey.mModelview) ? "" : " (changed)")
                          << (sameSettings(key, slot.mKey) ? "" : ", settings/size changed") << LL_ENDL;
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
        slot.mSourcesFailed = false;
        slot.mSkyTime = gFrameTimeSeconds;
    }

    void release(Accumulator& slot)
    {
        slot.mTarget.release();
        slot.mSmooth.release();
        slot.mSources.release();
        slot.mSourcesFailed = false;
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
        // Live (mode 3) shares the Advanced renderer's blur size, quality,
        // aperture shape, field curvature and spherical aberration controls;
        // its other lens effects follow
        // (doc/ayanestorm-depth-of-field-live-plan.md, phase 4).
        const bool screen_space = mode == 1 || mode == ASDoFLive::LIVE_MODE;
        setFlag("ASDepthOfFieldUIAdvanced", mode == 1);
        setFlag("ASDepthOfFieldUILive", mode == ASDoFLive::LIVE_MODE);
        setFlag("ASDepthOfFieldUIScreenSpace", screen_space);
        setFlag("ASDepthOfFieldUIAperture", mode == APERTURE_MODE);
        setFlag("ASDepthOfFieldUIShape", screen_space || mode == APERTURE_MODE);
        // Axial CA, cat's eye (with its corner darkening) and spherical
        // aberration apply to both
        // the Advanced and the Aperture-sampled renderers; spherical
        // aberration to Live too, whose lens checkboxes are enabled ahead
        // of their effects (phase 4).
        const bool lens_modes = mode == 1 || mode == APERTURE_MODE;
        const bool live = mode == ASDoFLive::LIVE_MODE;
        setFlag("ASDepthOfFieldUILens", lens_modes || live);
        setFlag("ASDepthOfFieldUIAxialCA",
                lens_modes && gSavedSettings.getBOOL("ASDepthOfFieldApertureAxialCA"));
        setFlag("ASDepthOfFieldUICatEye",
                lens_modes && gSavedSettings.getBOOL("ASDepthOfFieldApertureCatEye"));
        // Advanced and Live renderers.
        setFlag("ASDepthOfFieldUIFieldCurvature",
                screen_space && gSavedSettings.getBOOL("ASDepthOfFieldFieldCurvature"));
        // Advanced renderer only.
        setFlag("ASDepthOfFieldUIAstigmatism",
                mode == 1 && gSavedSettings.getBOOL("ASDepthOfFieldAstigmatism"));
        setFlag("ASDepthOfFieldUIMaxBlur",
                screen_space && gSavedSettings.getBOOL("ASDepthOfFieldPhysicalBlur"));
        setFlag("ASDepthOfFieldUISpherical",
                (lens_modes || live) && gSavedSettings.getBOOL("ASDepthOfFieldApertureSpherical"));
        setFlag("ASDepthOfFieldUIHighlights",
                mode == APERTURE_MODE && gSavedSettings.getBOOL("ASDepthOfFieldApertureHighlights"));
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

    // Pixels per unit image-plane offset at unit distance (installed
    // projection's [1][1], so zoomed snapshot tiles scale too).
    F32 pixelsPerUnit()
    {
        return sCentralProjection[1][1] * 0.5f * (F32)gPipeline.mRT->screen.getHeight();
    }

    // Weighted fullscreen copy of source into the bound target (see
    // asDoFAccumulateF.glsl). SAMPLE draws one lens sample: a positive
    // residual_scale softens it by the per-pixel residual disk, and the
    // cat's-eye and spherical weights apply; both read this sample's depth.
    // AVERAGE compensates the cat's-eye light loss unless darkening is kept.
    // MASKED and SMOOTHED are AVERAGE plus the final smoothing (radius
    // scale residual_scale, see smoothingScale()); SMOOTHED reads the
    // mipmapped MASKED image from smooth.
    // SOURCES marks this sample's small sources (residual_scale: ring radius
    // scale); SMOOTHED also reads their summed map from sources.
    // filter: source sampling (TFO_TRILINEAR for a mipmapped reduction).
    void drawWeighted(LLRenderTarget& source, const glm::vec4& weight, Draw mode, F32 residual_scale = 0.f,
                      LLRenderTarget* smooth = nullptr, LLRenderTarget* sources = nullptr,
                      LLTexUnit::eTextureFilterOptions filter = LLTexUnit::TFO_BILINEAR)
    {
        if (mode == Draw::SAMPLE && !sLens.mValid)
        {
            mode = Draw::COPY; // no lens sample installed
        }
        const bool sample = mode == Draw::SAMPLE;
        const bool smoothing = mode == Draw::MASKED || mode == Draw::SMOOTHED;
        const bool source_map = mode == Draw::SOURCES;
        const F32 spherical = sample ? sSpherical : 0.f;
        const F32 highlight = sample ? sHighlight : 0.f;
        const bool need_depth = residual_scale > 0.f || spherical != 0.f || highlight > 0.f || source_map;
        const F32 cat_eye = (sample || ((mode == Draw::AVERAGE || smoothing) && !sCatEyeDarken)) ? sCatEye : 0.f;

        sAccumulateProgram.bind();
        sAccumulateProgram.uniform4f(U_SAMPLE_WEIGHT, weight.x, weight.y, weight.z, weight.w);
        sAccumulateProgram.uniform1f(U_RESIDUAL_SCALE, residual_scale);
        sAccumulateProgram.uniform1i(U_LENS_MODE, (S32)mode);
        sAccumulateProgram.uniform1f(U_CAT_EYE, cat_eye);
        sAccumulateProgram.uniform1f(U_SA_STRENGTH, spherical);
        sAccumulateProgram.uniform1f(U_HL_STRENGTH, highlight);
        sAccumulateProgram.uniform1f(U_HL_THRESHOLD, sHighlightThreshold);
        // Field position: 1 at the frame corner, aspect-correct.
        const F32 aspect = (F32)source.getWidth() / (F32)llmax(source.getHeight(), 1U);
        const F32 diagonal = sqrtf(aspect * aspect + 1.f);
        sAccumulateProgram.uniform2f(U_FIELD_SCALE, 2.f * aspect / diagonal, 2.f / diagonal);
        sAccumulateProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, &source, false, filter);
        if (smooth)
        {
            // SOURCES: the star mask has no mips.
            sAccumulateProgram.bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, smooth, false,
                                           source_map ? LLTexUnit::TFO_POINT : LLTexUnit::TFO_TRILINEAR);
        }
        if (sources)
        {
            sAccumulateProgram.bindTexture(LLShaderMgr::SPECULAR_MAP, sources, false, LLTexUnit::TFO_TRILINEAR);
        }
        if (source_map)
        {
            sAccumulateProgram.uniform1f(U_HAS_STAR_MASK, smooth ? 1.f : 0.f);
        }
        if (mode == Draw::SMOOTHED)
        {
            static LLCachedControl<bool> show(gSavedSettings, "ASDepthOfFieldApertureShowSmoothing", false);
            sAccumulateProgram.uniform1f(U_SHOW_SMOOTHING, show ? 1.f : 0.f);
        }
        if (smoothing || source_map)
        { // Full-aperture CoC per unit defocus.
            const ASDoFCamera::Lens& lens = smoothing ? sAverageLens : sLens;
            sAccumulateProgram.uniform1f(U_SA_COC_SCALE, lens.mApertureRadius * pixelsPerUnit());
        }
        if (sample)
        {
            const S32 sequence_index = sFirstSequenceIndex + sSampleIndex;
            const glm::vec2& lens = sLensSamples[sequence_index];
            sAccumulateProgram.uniform2f(U_LENS_POS, lens.x, lens.y);
            sAccumulateProgram.uniform1f(U_PUPIL_R2, ASDoFAperture::pupilRadius2((U32)sequence_index));
            sAccumulateProgram.uniform1f(U_SA_COC_SCALE, sLens.mApertureRadius * pixelsPerUnit());
        }
        if (need_depth)
        {
            sAccumulateProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen,
                                           true, LLTexUnit::TFO_POINT);
            sAccumulateProgram.uniform1f(U_RESIDUAL_MAX, smoothing ? SMOOTHING_MAX_PIXELS : RESIDUAL_MAX_PIXELS);
            sAccumulateProgram.uniform1f(U_INV_FOCUS,
                                         smoothing ? 1.f / sAverageLens.mFocusDistance : sSampleInvFocus);
            sAccumulateProgram.uniform2f(U_PROJ_Z, sCentralProjection[2][2], sCentralProjection[3][2]);
            sAccumulateProgram.uniform2f(U_TEXEL_SIZE, 1.f / source.getWidth(), 1.f / source.getHeight());
            // Golden-angle rotation per sequence index decorrelates the tap pattern.
            sAccumulateProgram.uniform1f(U_TAP_ROTATION,
                                         2.39996323f * (F32)(sFirstSequenceIndex + sSampleIndex));
        }
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        sAccumulateProgram.unbindTexture(LLShaderMgr::DIFFUSE_MAP);
        if (smooth)
        {
            sAccumulateProgram.unbindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP);
        }
        if (sources)
        {
            sAccumulateProgram.unbindTexture(LLShaderMgr::SPECULAR_MAP);
        }
        if (need_depth)
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
        return strength * sLens.mApertureRadius * pixelsPerUnit() *
            sqrtf(F_PI / (F32)llmax(sMaxSamples, 1));
    }

    // Final smoothing radius scale for an average of `samples`: the dot
    // spacing coc * sqrt(pi / N) that remains, times the strength. It
    // follows the samples actually averaged, so it fades as the average
    // develops. 0 when off.
    F32 smoothingScale(S32 samples)
    {
        static LLCachedControl<F32> strength(gSavedSettings, "ASDepthOfFieldApertureSmoothing", 2.f);
        if (strength <= 0.f || !sAverageLens.mValid || samples <= 0)
        {
            return 0.f;
        }
        return strength * sAverageLens.mApertureRadius * pixelsPerUnit() *
            sqrtf(F_PI / (F32)llmax(samples, SMOOTHING_MIN_SAMPLES));
    }

    // (Re)allocates a mipmapped (manual) target at the sum's size.
    bool ensureMipTarget(LLRenderTarget& target, const Accumulator& slot, U32 format)
    {
        const U32 width = slot.mTarget.getWidth();
        const U32 height = slot.mTarget.getHeight();
        if (target.getWidth() == width && target.getHeight() == height)
        {
            return true;
        }
        target.release();
        if (!target.allocate(width, height, format, false, LLTexUnit::TT_TEXTURE, LLTexUnit::TMG_MANUAL))
        {
            LL_WARNS_ONCE("ASDoF") << "Aperture DoF smoothing allocation failed; showing the unsmoothed average"
                                   << LL_ENDL;
            target.release();
            return false;
        }
        return true;
    }

    void generateMips(LLRenderTarget& target)
    {
        // Explicit unit: glGenerateMipmap acts on the active unit's texture.
        LLTexUnit* unit = gGL.getTexUnit(0);
        unit->bindManual(LLTexUnit::TT_TEXTURE, target.getTexture(), true);
        unit->activate();
        glGenerateMipmap(GL_TEXTURE_2D);
        unit->unbind(LLTexUnit::TT_TEXTURE);
    }

    // Adds this lens sample's light sources (glowing pixels and visible
    // star pixels, see asDoFAccumulateF.glsl mode 5) to slot's source map
    // (overwrites on the first sample). Blend state: as the caller's.
    void accumulateSources(Accumulator& slot, bool first)
    {
        const bool drawn = sStarMaskDrawn && sStarMask.getWidth() == slot.mTarget.getWidth() &&
            sStarMask.getHeight() == slot.mTarget.getHeight();
        sStarMaskDrawn = false;
        if (!sLens.mValid || slot.mSourcesFailed)
        {
            return;
        }
        if (!ensureMipTarget(slot.mSources, slot, GL_R32F))
        {
            slot.mSourcesFailed = true;
            return;
        }
        if (!first && slot.mSources.getWidth() != slot.mTarget.getWidth())
        {
            slot.mSourcesFailed = true;
            return;
        }
        // Sources: this sample's glowing pixels (screen alpha) and, when
        // the sky pool drew them, its visible stars.
        slot.mSources.bindTarget();
        drawWeighted(gPipeline.mRT->screen, glm::vec4(1.f), Draw::SOURCES, 0.f, drawn ? &sStarMask : nullptr);
        slot.mSources.flush();
    }

    // Writes slot's normalized average into screen, with the final
    // smoothing when it is on and the average is finished (a developing
    // average has few samples: smoothing it would blur heavily, then fade).
    // Smoothing applies only where the source map shows a bokeh.
    void presentAverage(Accumulator& slot, LLRenderTarget& screen, bool finished)
    {
        const glm::vec4 weight(1.f / slot.mAccumulated);
        const F32 smoothing = finished ? smoothingScale(slot.mAccumulated) : 0.f;
        const bool have_sources = !slot.mSourcesFailed && slot.mSources.getWidth() == slot.mTarget.getWidth() &&
            slot.mSources.getHeight() == slot.mTarget.getHeight();
        if (smoothing > 0.f && have_sources && ensureMipTarget(slot.mSmooth, slot, GL_RGBA16F))
        {
            slot.mSmooth.bindTarget();
            drawWeighted(slot.mTarget, weight, Draw::MASKED, smoothing);
            slot.mSmooth.flush();
            generateMips(slot.mSmooth);
            generateMips(slot.mSources);
            screen.bindTarget();
            drawWeighted(slot.mTarget, weight, Draw::SMOOTHED, smoothing, &slot.mSmooth, &slot.mSources);
        }
        else
        {
            screen.bindTarget();
            drawWeighted(slot.mTarget, weight, Draw::AVERAGE);
        }
        screen.flush();
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
        const bool first = slot.mAccumulated == 0;
        if (first)
        {
            slot.mSourcesFailed = false; // a new average starts a new map
        }
        slot.mTarget.bindTarget();
        if (first)
        { // First sample of a new average overwrites.
            LLGLDisable blend(GL_BLEND);
            drawWeighted(gPipeline.mRT->screen, sSampleWeight, Draw::SAMPLE, residual);
            slot.mTarget.flush();
            accumulateSources(slot, true);
        }
        else
        {
            LLGLEnable blend(GL_BLEND);
            gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
            drawWeighted(gPipeline.mRT->screen, sSampleWeight, Draw::SAMPLE, residual);
            slot.mTarget.flush();
            accumulateSources(slot, false);
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
        }
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
        drawWeighted(sLive.mTarget, glm::vec4(1.f), Draw::COPY);
        sCapture.mTarget.flush();
        sCapture.mAccumulated = sLive.mAccumulated;
        sCapture.mSkyTime = sLive.mSkyTime;
        // The source map continues with the sum.
        sCapture.mSourcesFailed = sLive.mSourcesFailed || sLive.mSources.getWidth() != sLive.mTarget.getWidth() ||
            !ensureMipTarget(sCapture.mSources, sCapture, GL_R32F);
        if (!sCapture.mSourcesFailed)
        {
            sCapture.mSources.bindTarget();
            drawWeighted(sLive.mSources, glm::vec4(1.f), Draw::COPY);
            sCapture.mSources.flush();
        }
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

    // Esc reported by the viewer's key handler (noteEscapeKey()) and not yet
    // consumed; also discards a stale press when a capture starts. Blocking
    // captures suspend the main loop and its input events, so each platform
    // also reads Esc directly, only while the viewer has the keyboard.
    bool captureCancelRequested()
    {
        const bool noted = sEscapePressed;
        sEscapePressed = false;
#if LL_WINDOWS
        // Down now, or pressed since the last query.
        HWND window = (HWND)gViewerWindow->getPlatformWindow();
        return noted || (window && GetForegroundWindow() == window && (GetAsyncKeyState(VK_ESCAPE) & 0x8001));
#elif LL_SDL2
        // Key-down events still queued: pumped and peeked, never removed (the
        // viewer handles them afterwards). A press counts once: only events
        // newer than the last one seen. SDL sends keyboard events only to
        // the focused window.
        static Uint32 last_seen = 0;
        bool pressed = false;
        SDL_PumpEvents();
        SDL_Event events[64];
        const int count = SDL_PeepEvents(events, (int)LL_ARRAY_SIZE(events), SDL_PEEKEVENT, SDL_KEYDOWN, SDL_KEYDOWN);
        for (int i = 0; i < count; ++i)
        {
            const SDL_KeyboardEvent& key = events[i].key;
            if (key.keysym.sym == SDLK_ESCAPE && (Sint32)(key.timestamp - last_seen) > 0)
            {
                last_seen = key.timestamp;
                pressed = true;
            }
        }
        return noted || pressed;
#elif LL_DARWIN
        // Down now (no press history outside the event loop), while the
        // viewer is the active application.
        constexpr CGKeyCode ESCAPE_KEY = 0x35; // kVK_Escape
        return noted || (gFocusMgr.getAppHasFocus() &&
                         CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, ESCAPE_KEY));
#else
        return noted;
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
            const std::string text = llformat("Capturing depth of field... %d / %d  (Esc to stop)",
                                              accumulated, sMaxSamples);
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
                // Keep rendering the requested view with its frozen focus:
                // drift or focus smoothing between slices, and the last
                // slice after a camera move, whose central depth feeds the
                // final smoothing of the finished average.
                sCentralProjection = sCapture.mKey.mProjection;
                sCentralModelview = sCapture.mKey.mModelview;
                if (!nearlySameView(key, sCapture.mKey))
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
        for (const char* name : { "ASDepthOfFieldMode", "ASDepthOfFieldApertureAxialCA",
                                  "ASDepthOfFieldApertureCatEye", "ASDepthOfFieldApertureSpherical",
                                  "ASDepthOfFieldApertureHighlights", "ASDepthOfFieldFieldCurvature",
                                  "ASDepthOfFieldAstigmatism", "ASDepthOfFieldPhysicalBlur" })
        {
            if (LLControlVariable* control = gSavedSettings.getControl(name))
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
                    {
                        syncModeFlags();
                    });
            }
        }

        // Selecting the Aperture-sampled renderer (from any combo) explains
        // that it is for still pictures; the notification has "Do not show
        // this again".
        if (LLControlVariable* control = gSavedSettings.getControl("ASDepthOfFieldMode"))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD& previous)
                {
                    if (value.asInteger() == APERTURE_MODE && previous.asInteger() != APERTURE_MODE)
                    {
                        LLNotificationsUtil::add("ASApertureDoFInfo");
                    }
                });
        }

        // DoF floater toolbar. The animation freeze can also change
        // elsewhere (Ctrl+Alt+N, Ctrl+J), so the button inverts the current
        // state instead of showing one.
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
            "ASDepthOfField.Toolbar",
            [](LLUICtrl*, const LLSD& data)
            {
                const std::string action = data.asString();
                if (action == "animations")
                { // Same global time-factor freeze as Advanced > Animation >
                  // Freeze Animations; the setting also holds sky and snow.
                    const bool freeze = LLMotionController::getCurrentTimeFactor() != 0.f;
                    set_all_animation_time_factors(freeze ? 0.f : 1.f);
                    gSavedSettings.setBOOL("ASDepthOfFieldFreezeAnimations", freeze);
                }
                else if (action == "refresh")
                { // The next live frame no longer matches: restarts averaging.
                    sLive.mHaveKey = false;
                }
            });
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
        sStarMask.release();
        sStarMaskDrawn = false;
        sSlot = &sLive;
        sCapturePending = false;
        setWorldFrozen(false);
        releaseFrozenView();
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

    void noteEscapeKey()
    {
        if (sCapturePending)
        {
            sEscapePressed = true;
        }
    }

    bool isCapturePending()
    {
        return sCapturePending;
    }

    F32 starRotationTime(F32 frame_time)
    {
        return sActive && sLens.mValid && sPlan == FramePlan::ACCUMULATE ? sSlot->mSkyTime : frame_time;
    }

    F32 starTwinkleMean()
    {
        // Twinkle is fract() of a per-pixel, per-time hash: uniform in
        // [0, 1), mean 0.5. Varying it per sample left each star bokeh a sum
        // of randomly bright, speckled dots (14x the interior variation of
        // a constant brightness in simulation); a still exposure averages
        // twinkle anyway.
        return sActive && sLens.mValid && sPlan == FramePlan::ACCUMULATE ? 0.5f : 0.f;
    }

    bool beginStarMask()
    {
        static LLCachedControl<F32> smoothing(gSavedSettings, "ASDepthOfFieldApertureSmoothing", 2.f);
        if (!sActive || !sLens.mValid || sPlan != FramePlan::ACCUMULATE || sAccumulationFailed ||
            smoothing <= 0.f || LLPipeline::sReflectionRender || LLPipeline::sShadowRender)
        {
            return false;
        }
        // The scene depth is attached (no writes): the second draw is
        // occluded exactly like the first, so only visible star pixels mark.
        static U32 shared_depth = 0;
        LLRenderTarget& screen = gPipeline.mRT->deferredScreen;
        if (!screen.getDepth())
        {
            return false;
        }
        if (sStarMask.getWidth() != screen.getWidth() || sStarMask.getHeight() != screen.getHeight() ||
            shared_depth != screen.getDepth())
        {
            sStarMask.release();
            shared_depth = 0;
            if (!sStarMask.allocate(screen.getWidth(), screen.getHeight(), GL_R16F))
            {
                sStarMask.release();
                return false;
            }
            screen.shareDepthBuffer(sStarMask);
            shared_depth = screen.getDepth();
        }
        sStarMask.bindTarget();
        GLfloat previous_clear[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, previous_clear);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        sStarMask.clear(GL_COLOR_BUFFER_BIT); // colour only: the depth is the scene's
        glClearColor(previous_clear[0], previous_clear[1], previous_clear[2], previous_clear[3]);
        return true;
    }

    void endStarMask()
    {
        sStarMask.flush(); // back to the G-buffer
        sStarMaskDrawn = true;
    }

    bool isSceneFrozen()
    {
        // Unfreezing animations elsewhere (Ctrl+J) also releases the sky.
        static LLCachedControl<bool> freeze(gSavedSettings, "ASDepthOfFieldFreezeAnimations", false);
        return sCapturePending || (freeze && LLMotionController::getCurrentTimeFactor() == 0.f);
    }

    bool isWorldFrozen()
    {
        return sWorldFrozen;
    }

    bool isLiveViewFrozen()
    {
        sLiveViewFrozen = false;
        if (!isEnabled())
        { // Mode switched mid-capture: the next rendered frame releases it.
            sCapturePending = false;
        }
        checkCaptureAbandoned();
        if (!sWorldFrozen || !sFrozenKept)
        {
            return false;
        }
        sLiveViewFrozen = true;
        return true;
    }

    bool presentFrozenView()
    {
        if (!sLiveViewFrozen || gSnapshot || gDisconnected || !sFrozenKept)
        {
            return false;
        }
        // Fitted into the world view, centred, never above 1:1 so the
        // capture's own pixels (and its noise) stay visible.
        const LLRect window = gViewerWindow->getWindowRectRaw();
        const LLRect rect = gViewerWindow->getWorldViewRectRaw();
        const F32 kept_width = (F32)sFrozenView.getWidth();
        const F32 kept_height = (F32)sFrozenView.getHeight();
        const F32 scale = llmin(1.f, llmin((F32)rect.getWidth() / kept_width, (F32)rect.getHeight() / kept_height));
        const S32 width = llmax(1, ll_round(kept_width * scale));
        const S32 height = llmax(1, ll_round(kept_height * scale));

        LLGLDisable scissor(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, window.getWidth(), window.getHeight());
        {
            LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS); // depth writes on for the clear
            gGL.setColorMask(true, true);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glClearColor(0.f, 0.f, 0.f, 0.f); // renderFinalize()'s clear colour
        }
        glViewport(rect.mLeft + (rect.getWidth() - width) / 2, rect.mBottom + (rect.getHeight() - height) / 2,
                   width, height);
        {
            LLGLDepthTest depth(GL_FALSE, GL_FALSE);
            LLGLDisable blend(GL_BLEND);
            drawWeighted(sFrozenView, glm::vec4(1.f), Draw::COPY, 0.f, nullptr, nullptr, LLTexUnit::TFO_TRILINEAR);
        }

        // The viewport renderFinalize() leaves for the UI.
        gGLViewport[0] = rect.mLeft;
        gGLViewport[1] = rect.mBottom;
        gGLViewport[2] = rect.getWidth();
        gGLViewport[3] = rect.getHeight();
        glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        return true;
    }

    void keepFrozenView()
    {
        // Only a pending sliced capture's own display() (not thumbnails or
        // blocking snapshots); the first slice froze the world just before.
        if (!gSnapshot || !sSliced || !sWorldFrozen || sFrozenFailed)
        {
            return;
        }
        // renderFinalize() drew the world view rect of the bound framebuffer:
        // the back buffer, or rawSnapshot()'s full-size scratch target.
        const LLRect source = gViewerWindow->getWorldViewRectRaw();
        const LLRect window = gViewerWindow->getWindowRectRaw();
        const S32 source_width = source.getWidth();
        const S32 source_height = source.getHeight();
        if (source_width <= 0 || source_height <= 0 || window.getWidth() <= 0 || window.getHeight() <= 0)
        {
            return;
        }
        // Reduced to at most twice the window fit (a bilinear step of at
        // most 2:1 per level beyond that is left to the mips).
        const F32 fit = llmin(1.f, 2.f * llmin((F32)window.getWidth() / (F32)source_width,
                                               (F32)window.getHeight() / (F32)source_height));
        const U32 width = (U32)llmax(1, ll_round(source_width * fit));
        const U32 height = (U32)llmax(1, ll_round(source_height * fit));
        if (sFrozenView.getWidth() != width || sFrozenView.getHeight() != height)
        {
            releaseFrozenView();
            if (!sFrozenView.allocate(width, height, GL_RGBA8, false, LLTexUnit::TT_TEXTURE, LLTexUnit::TMG_MANUAL))
            { // Live frames keep rendering the (frozen) scene.
                LL_WARNS("ASDoF") << "Aperture DoF capture view allocation failed: " << width << "x" << height
                                  << LL_ENDL;
                releaseFrozenView();
                sFrozenFailed = true;
                return;
            }
        }

        GLint source_fbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &source_fbo);
        LLGLDisable scissor(GL_SCISSOR_TEST); // blits obey the scissor test
        sFrozenView.bindTarget();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source_fbo);
        glBlitFramebuffer(source.mLeft, source.mBottom, source.mLeft + source_width, source.mBottom + source_height,
                          0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        sFrozenView.flush(); // rebinds the source framebuffer
        generateMips(sFrozenView);
        sFrozenKept = true;
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
        sAverageLens.mValid = false;
        sStarMaskDrawn = false;
        sAxialCA = 0.f;
        sCatEye = 0.f;
        sCatEyeDarken = false;
        sSpherical = 0.f;
        sHighlight = 0.f;
        // One-shot: only the snapshot display() right after the request is a slice.
        const bool slice_requested = sSliceRequested;
        const bool preview_requested = sPreviewRequested;
        sSliceRequested = false;
        sPreviewRequested = false;
        sSliced = false;
        sPreview = false;
        sInvalidFocus = false;
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
        static LLCachedControl<bool> cat_eye(gSavedSettings, "ASDepthOfFieldApertureCatEye", false);
        static LLCachedControl<F32> cat_eye_strength(gSavedSettings, "ASDepthOfFieldApertureCatEyeStrength", 0.6f);
        static LLCachedControl<bool> cat_eye_darken(gSavedSettings, "ASDepthOfFieldApertureCatEyeDarken", false);
        static LLCachedControl<bool> spherical(gSavedSettings, "ASDepthOfFieldApertureSpherical", false);
        static LLCachedControl<F32> spherical_strength(gSavedSettings, "ASDepthOfFieldApertureSphericalStrength", 0.5f);
        key.mCatEye = cat_eye ? llclamp((F32)cat_eye_strength, 0.f, 2.f) : 0.f;
        key.mCatEyeDarken = key.mCatEye > 0.f && cat_eye_darken;
        key.mSpherical = spherical ? llclamp((F32)spherical_strength, -1.f, 1.f) : 0.f;
        static LLCachedControl<bool> highlights(gSavedSettings, "ASDepthOfFieldApertureHighlights", false);
        static LLCachedControl<F32> highlight_strength(gSavedSettings, "ASDepthOfFieldApertureHighlightStrength", 0.3f);
        static LLCachedControl<F32> highlight_threshold(gSavedSettings, "ASDepthOfFieldApertureHighlightThreshold", 1.f);
        key.mHighlight = highlights ? llclamp((F32)highlight_strength, 0.f, 1.f) : 0.f;
        key.mHighlightThreshold = key.mHighlight > 0.f ? llclamp((F32)highlight_threshold, 0.05f, 8.f) : 0.f;
        sCatEye = key.mCatEye;
        sCatEyeDarken = key.mCatEyeDarken;
        sSpherical = key.mSpherical;
        sHighlight = key.mHighlight;
        sHighlightThreshold = llmax(key.mHighlightThreshold, 0.05f);
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
            checkCaptureAbandoned();
            if (!sCapturePending && sCapture.mHaveKey)
            { // Finished captures keep no (possibly huge) snapshot-size sum.
                release(sCapture);
            }
            sSlot = &sLive;
            sMaxSamples = llclamp(gSavedSettings.getS32("ASDepthOfFieldApertureMaxSamples"), 1, MAX_SAMPLES);
            sLiveMaxSamples = sMaxSamples;
            if (!matches(sLive, key, sFocusDistance))
            { // Moving: fast central view; averaging restarts once still.
                logLiveRestart(sLive, key, sFocusDistance);
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
        sAverageLens = sLens;
        if (!sLens.mValid)
        { // Invalid lens (e.g. focus inside the focal length): nothing clamped.
            slot.mAccumulated = 0;
            sPlan = FramePlan::PINHOLE;
            sInvalidFocus = !gSnapshot;
            static LLTimer log_timer;
            if (log_timer.getElapsedTimeF32() > 5.f)
            {
                log_timer.reset();
                LL_INFOS("ASDoF") << "Aperture DoF lens invalid, rendering pinhole: focus " << slot.mKeyFocus
                                  << " m, view angle " << key.mView << " rad, focal length " << key.mFocalLength
                                  << " mm (at the default FOV), f/" << key.mFNumber << LL_ENDL;
            }
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

        if (sSliced)
        {
            sCaptureSeconds += sSliceTimer.getElapsedTimeF32();
            sCapturePending = sPlan == FramePlan::ACCUMULATE && !sAccumulationFailed &&
                !sCaptureStopped && slot.mAccumulated < sMaxSamples;
            sSliceTimer.reset(); // abandon timeout counts from the slice end
            setWorldFrozen(sCapturePending);
        }

        // Show the running average (converged frames reuse it unchanged).
        // The final smoothing only applies to a finished image: a blocking
        // snapshot, the last slice of a capture, or a converged average.
        if ((sPlan == FramePlan::ACCUMULATE || sPlan == FramePlan::CONVERGED) &&
            !sAccumulationFailed && slot.mAccumulated > 0)
        {
            const bool finished = blocking || (sSliced && !sCapturePending) ||
                sPlan == FramePlan::CONVERGED || slot.mAccumulated >= sMaxSamples;
            LLGLDepthTest depth(GL_FALSE, GL_FALSE);
            LLGLDisable blend(GL_BLEND);
            presentAverage(slot, gPipeline.mRT->screen, finished);
        }
        glClearColor(0.f, 0.f, 0.f, 0.f);
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
        const bool invalid_focus = show_progress && sInvalidFocus;
        // Hidden under the teleport/login progress screen (the sliced
        // capture's progress image is not that view).
        LLProgressView* progress_view = gViewerWindow->getProgressView();
        if (gSnapshot || !isEnabled() || (!live_accumulating && !sCapturePending && !invalid_focus) ||
            (progress_view && progress_view->getVisible()))
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
            text = llformat("Snapshot DoF %d / %d  (Esc to stop)", accumulated, target);
        }
        else if (invalid_focus)
        { // No samples: the lens cannot focus there (see makeLens).
            accumulated = 0;
            text = llformat("DoF: focus %.2f m is behind the camera or too close", sLive.mKeyFocus);
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
