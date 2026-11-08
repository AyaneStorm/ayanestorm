/**
 * @file asrcameraturn.h
 * @brief Lets the new renderer's thread turn the camera by the mouse, between main-thread frames.
 *
 * The camera tool's orbit, its zoom tool's orbit, and mouselook turn the camera by the mouse's own motion,
 * which arrives off the main thread and both threads read. The main thread turns its
 * camera by the motion up to one total, and the render thread turns that camera by the
 * motion since, the same way, so the view follows the hand at the render rate and stops
 * when it stops. See 'Camera input on the render thread' in
 * doc/ayanestorm-new-renderer-plan.md.
 *
 * With OpenGL, or with no raw motion from the mouse, or with MouseSmooth on, which the
 * render thread could not follow, the tools turn by the cursor's delta, as before.
 *
 * The wheel's zoom eases on the render thread the same way: the main thread publishes
 * how its camera eases, and on Windows the render thread also zooms by clicks the main
 * thread has not handled yet.
 */
#ifndef ASR_CAMERA_TURN_H
#define ASR_CAMERA_TURN_H

#include "v3math.h"

class LLWindow;

namespace asr
{
    class MouseMotion;
    struct CameraTurn;
}

namespace ASRCameraTurn
{
    // With the new renderer, starts and stops reading the mouse's own motion.
    void start(LLWindow* window);
    void stop();

    // What the render thread reads. Valid for the whole run.
    const asr::MouseMotion* motion();

    // LLToolCamera, on every hover, with the cursor's delta: takes the motion since the last
    // hover. The first after a frame without one takes nothing, so a drag never applies
    // motion from before it began.
    void takeOrbitMotion(bool captured, S32 cursor_dx, S32 cursor_dy);

    // LLToolCamera's orbit: true when it orbited by the motion taken, for the render thread
    // to follow, and false when the tool orbits by the cursor, as before.
    bool orbit();

    // LLToolCamera's zoom tool, the default one, which a plain Alt drag uses: true when it
    // orbited around and zoomed by the motion taken, as it does by the cursor, and false when
    // the tool goes by the cursor, as before. The drag up and down zooms once outside the slop,
    // or orbits over with mouse steering. The render thread follows the orbits, not the drag's
    // zoom, which shows at the main thread's rate.
    bool zoomToolOrbit(bool mouse_steering, bool outside_slop_y);

    // LLToolCamera, on mouse up: the motion that came since the last hover, which the render
    // thread has shown, is orbited by too, so the view does not step back.
    void endOrbit();

    // LLToolGun in mouselook: the motion to turn by, in its units and signs, which are the
    // cursor's delta negated. The render thread follows it when it is the mouse's own motion.
    void takeLookMotion(F32 mouse_sensitivity, F32& dx, F32& dy);

    // LLViewerWindow, for every wheel event from the window, wherever it goes: counts the
    // clicks handled, so the render thread knows those still on their way.
    void wheelHandled(S32 clicks);

    // LLViewerWindow, when the wheel reached the camera's zoom: for a while, the render thread
    // zooms by clicks the main thread has not handled yet, as they most likely zoom too.
    void wheelZoomed();

    // True when a tool turned the camera this frame by the mouse's own motion. The camera then
    // follows it with no smoothing, as the render thread cannot smooth what it adds.
    bool turning();

    // Fills in the turn and zoom to publish with this frame's camera, whose space is agent space
    // moved by `origin`, and ends the frame. Once a frame, with the camera, after updateCamera().
    void publish(asr::CameraTurn& turn, const LLVector3& origin);

    // Temporary: logs at which step turns stopped since the last call, as the Mac never
    // turns a frame ahead of the main thread.
    void logCounts();
}

#endif // ASR_CAMERA_TURN_H
