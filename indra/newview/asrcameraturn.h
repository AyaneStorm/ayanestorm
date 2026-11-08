/**
 * @file asrcameraturn.h
 * @brief Lets the new renderer's thread turn the camera by the mouse, between main-thread frames.
 *
 * The camera tool's orbit and mouselook turn the camera by the mouse's own motion,
 * which arrives off the main thread and both threads read. The main thread turns its
 * camera by the motion up to one total, and the render thread turns that camera by the
 * motion since, the same way, so the view follows the hand at the render rate and stops
 * when it stops. See 'Camera input on the render thread' in
 * doc/ayanestorm-new-renderer-plan.md.
 *
 * With OpenGL, or with no raw motion from the mouse, or with MouseSmooth on, which the
 * render thread could not follow, the tools turn by the cursor's delta, as before.
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

    // LLToolCamera, on mouse up: the motion that came since the last hover, which the render
    // thread has shown, is orbited by too, so the view does not step back.
    void endOrbit();

    // LLToolGun in mouselook: the motion to turn by, in its units and signs, which are the
    // cursor's delta negated. The render thread follows it when it is the mouse's own motion.
    void takeLookMotion(F32 mouse_sensitivity, F32& dx, F32& dy);

    // True when a tool turned the camera this frame by the mouse's own motion. The camera then
    // follows it with no smoothing, as the render thread cannot smooth what it adds.
    bool turning();

    // Fills in the turn to publish with this frame's camera, whose space is agent space moved
    // by `origin`, and ends the frame. Once a frame, with the camera, after updateCamera().
    void publish(asr::CameraTurn& turn, const LLVector3& origin);
}

#endif // ASR_CAMERA_TURN_H
