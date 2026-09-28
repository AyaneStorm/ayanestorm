/**
 * @file asdofautofocus.h
 * @author chanayane@firestorm
 * @brief Depth-of-field autofocus (ASDepthOfFieldFocusMode 1 and 2).
 *
 * Replaces Firestorm's single focus point for every DoF renderer.
 * Area mode: a small pass samples 2048 scene depths in a Gaussian pattern
 * around the autofocus area (ASDepthOfFieldAutofocusX/Y/Area); the CPU reads
 * them back asynchronously (PBO + fence, a frame or two later), weights each
 * by 1/distance (near surfaces win, the sky barely counts) and takes the
 * weighted quantile (median by default, nearer with
 * ASDepthOfFieldAutofocusNearPriority). Eye mode: the visible avatar whose
 * eyes are inside the area, nearest the camera, is focused on the
 * camera-facing surface of its nearer eye (eyeball radius measured on mesh
 * eyes, or ASDepthOfFieldAutofocusEyeRadius); eyes hidden behind geometry
 * (checked against the depth buffer) or no avatar fall back to area mode.
 * Focus moves in 1/distance with a frame-rate independent exponential
 * (ASDepthOfFieldAutofocusTime reaches 99%), and holds still until the
 * target moves by more than a small dead band, so the aperture-sampled
 * accumulation is not restarted by animation noise.
 * Firestorm focus lock (Alt+Shift+X, FSFocusPointLocked) in autofocus:
 * ASDepthOfFieldAutofocusLockMode 0 freezes the distance (classic); 1
 * locks the subject (the tracked eyes, else the surface under the area
 * centre, fixed to its avatar joint or object) and keeps focusing on it,
 * also outside the area with ASDepthOfFieldAutofocusTrackOutside.
 * Snapshots and sliced captures hold the current autofocus distance: a
 * snapshot focuses exactly as the live view did.
 */
#ifndef AS_DOF_AUTOFOCUS_H
#define AS_DOF_AUTOFOCUS_H

#include <vector>

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderTarget;
class LLVertexBuffer;

namespace ASDoFAutofocus
{
    // Menu toggle "ASDepthOfField.ToggleAutofocus" (Alt+Shift+Z) between
    // point focus and the last autofocus mode, and its check
    // "ASDepthOfField.IsAutofocus".
    void registerUICallbacks();

    // DoF on and an autofocus mode selected.
    bool isActive();

    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool createShaders(S32 shader_level);
    void unloadShaders();
    void releaseResources();

    // Called by LLPipeline::renderDoF once per frame with the scene depth.
    // distance: in, the current Firestorm focus distance (used when
    // autofocus starts); out, the autofocus distance (metres along the view
    // axis). Returns false when autofocus is off or has no result yet: the
    // caller keeps its own focus point.
    bool update(LLRenderTarget& depth, LLVertexBuffer& triangle, F32& distance);

    // Autofocus area, focus distance and tracked eyes or locked subject,
    // with ASDepthOfFieldAutofocusShowArea (menu "Show DoF Autofocus Area",
    // Alt+Shift+V) or Firestorm's "Draw DoF Focus crosshair"
    // (FSFocusPointRender, whose 3D crosshair is skipped in autofocus);
    // never in snapshots.
    // Called from render_ui() right after the world image, before HUD
    // elements, HUD attachments and the 2D UI, so all of them draw over it.
    // Sets up its own 2D state and restores the 3D matrices and viewport.
    void drawOverlay();
}

#endif
