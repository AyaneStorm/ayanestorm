/**
 * @file asdofrenderer.h
 * @author chanayane@firestorm
 * @brief Aperture-sampled depth-of-field frame coordinator (ASDepthOfFieldMode 2).
 *
 * Progressive (converged-capture) policy: while the camera, lens, focus and
 * viewport stay unchanged, each frame renders ASDepthOfFieldApertureSamples
 * new lens samples (culling, G-buffer, lighting and the selected
 * transparency compositor per sample) and adds their resolved linear HDR
 * (glow in alpha) to a running average shown before renderFinalize(), up to
 * ASDepthOfFieldApertureMaxSamples; then it shows the finished average from
 * an ordinary-cost central pass. Any change restarts with a central
 * (pinhole) frame. Scene animation is not detected: moving content
 * smears into the average like a long exposure. Sample 0 is the ordinary
 * display() pass; samples 1..N-1 run from renderRemainingSamples().
 * Snapshots (gSnapshot) average separately from the live view, up to
 * ASDepthOfFieldApertureSnapshotSamples. The snapshot floater's captures
 * are time-sliced (requestCaptureSlice()): each rawSnapshot() call renders
 * about CAPTURE_SLICE_SECONDS of samples and returns to the main loop, so
 * networking and UI keep running; other callers and tiled snapshots block,
 * within a 30 s budget.
 * ASDepthOfFieldApertureDebugSample >= 0 renders only that sample, unaveraged. Upstream hooks: llviewerdisplay.cpp (beginSample,
 * renderRemainingSamples) and pipeline.cpp (focus publication, legacy blur
 * skip, endSample, once-per-frame matrix capture guard). Shaders register
 * through ASDepthOfField's existing hooks.
 */
#ifndef AS_DOF_RENDERER_H
#define AS_DOF_RENDERER_H

#include <vector>

#include "stdtypes.h"

class LLCullResult;
class LLGLSLShader;

namespace ASDoFRenderer
{
    // Connects ASDepthOfFieldFreezeAnimations to the global animation freeze.
    void registerUICallbacks();
    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool createShaders(S32 shader_level);
    void unloadShaders();
    void releaseResources();

    // Mode 2 selected and DoF allowed now (renderDoF's own gate).
    bool isEnabled();

    // LLPipeline::renderDoF publishes its smoothed focus distance (metres,
    // along the view axis); the next frame's lens uses it. Once per frame.
    void setFocusDistance(F32 distance);

    // Called right before rawSnapshot() by a caller that retries while
    // isCapturePending(): that snapshot renders one time slice of samples.
    void requestCaptureSlice();

    // After a sliced rawSnapshot(): true when the image is a partial
    // average and the caller should call again on a later main-loop pass.
    // A view change, Esc or the time limit ends the capture with the
    // samples rendered so far.
    bool isCapturePending();

    // True while a sliced capture is pending or the DoF floater's "Freeze
    // all animations" is on: time-driven scene changes (sky and stars, cloud
    // scroll, snow) pause so averaged renders see one instant.
    bool isSceneFrozen();

    // Star dome clocks (lldrawpoolwlsky.cpp) while lens samples accumulate:
    // rotation uses the time the running average started, so stars do not
    // drift between samples; twinkle gets a per-sample value so it averages
    // to the mean brightness. Pass-through otherwise.
    F32 starRotationTime(F32 frame_time);
    F32 starTwinkleTime(F32 time);

    // After a partial slice: true when the snapshot floater should refresh
    // its preview from the partial image (first after 8 samples, then every
    // 32). Records the refresh.
    bool isCapturePreviewDue();

    // Remembers that the snapshot preview hook switched the preview to
    // subsampled thumbnails for a capture's progress (so it restores only
    // what it changed). Returns the previous state.
    bool setProgressThumbnail(bool active);

    // After a partial slice: lets the next live frame be presented
    // (rawSnapshot() disables that swap).
    void resumeLiveView();

    // Called right before a thumbnail rawSnapshot(): that snapshot continues
    // the live average with at most a few samples (no progress screen) and
    // leaves any pending capture untouched.
    void requestPreviewCapture();

    // Called after display_update_camera(), before culling. Saves the
    // central camera, chooses this frame's samples and installs sample 0.
    // No-op when disabled, for snapshots and cube captures.
    void beginSample(bool for_snapshot);

    // Called at the end of the 3D scene, before the last-frame matrix
    // capture: accumulates this sample and restores the central camera.
    void endSample();

    // True while samples 1..N-1 render; once-per-frame work (matrix
    // history) must skip these.
    bool isRepeatSample();

    // Called by display() right after sample 0's renderDeferredLighting():
    // renders samples 1..N-1 and writes the normalized average into the
    // screen target renderFinalize() reads. Ends the frame's sampling.
    void renderRemainingSamples(LLCullResult& result);

    // Optional top-right sample counter and progress bar
    // (ASDepthOfFieldApertureShowProgress); never drawn into snapshots.
    // Called from render_ui() in 2D UI state.
    void drawProgress();
}

#endif
