/**
 * @file asrcameraturn.cpp
 * @brief See asrcameraturn.h.
 */
#include "llviewerprecompiledheaders.h"

#include "asrcameraturn.h"

#include "asrgltable.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llframetimer.h"
#include "llkeyboard.h"
#include "llselectmgr.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llwindow.h"

// As asrviewer.cpp: the Linux window is still to do.
#if defined(AS_RENDERER) && (LL_WINDOWS || LL_DARWIN)
#define ASR_TURN_SUPPORTED 1
#include "asrmousemotion.h"
#include "asrrenderthread.h"
#else
#define ASR_TURN_SUPPORTED 0
#endif

#include <algorithm>

#if ASR_TURN_SUPPORTED

namespace
{

// A mouse with no raw motion while the cursor moves, such as some trackpads, is followed
// by its cursor instead after this long.
constexpr F64 RAW_SILENCE_SECONDS = 0.5;

// Wheel clicks the main thread has not handled yet are followed for this long after the
// wheel last zoomed the world, as the next ones most likely do too.
constexpr F64 WHEEL_FOLLOW_SECONDS = 1.0;

// As llagentcamera.cpp has them, where they are local to that file. Should they differ, the
// render thread's zoom is a little off until the main thread's next camera, which fades in.
constexpr F32 CAMERA_ZOOM_HALF_LIFE = 0.07f;
constexpr F32 CAMERA_FOCUS_HALF_LIFE = 0.f;
constexpr F32 SMOOTHING_HALF_LIFE = 0.02f; // times CameraPositionSmoothing
constexpr F32 MIN_ZOOM_FRACTION = 0.25f;
constexpr F32 MAX_ZOOM_FRACTION = 8.f;
constexpr F32 MIN_CAMERA_DISTANCE = 0.1f;
constexpr F32 LAND_MIN_ZOOM = 0.15f;
constexpr F32 AVATAR_MIN_ZOOM = 0.5f;
constexpr F32 OBJECT_MIN_ZOOM = 0.02f;
constexpr F32 APPEARANCE_MIN_ZOOM = 0.39f;
constexpr F32 APPEARANCE_MAX_ZOOM = 8.f;
constexpr F32 FARTHEST_UNCONSTRAINED = 1.0e6f;

asr::MouseMotion sMotion;

#if LL_WINDOWS
void addMotion(float dx, float dy)
{
    sMotion.add(dx, dy);
}

void addClicks(S32 clicks)
{
    sMotion.addClicks(clicks);
}
#endif

LLWindow* sWindow = nullptr;
bool sRunning = false;
bool sWheelRunning = false;
S32 sHandledClicks = 0;       // of the window's clicks, those handleScrollWheel() has had
F64 sWheelZoomed = -1.0e9;    // when the wheel last zoomed the world
S32 sLastClicks = 0;          // the window's total when publish() last saw it change
F64 sClicksChanged = -1.0e9;

// A click handed over before the window's callback was set, and handled after, would leave
// the two counts apart for good. Once the window's total has not moved for a while nothing
// is on its way, so the counts must agree, and are made to.
void resyncClicks()
{
    const S32 total = sMotion.clicks();
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (total != sLastClicks)
    {
        sLastClicks = total;
        sClicksChanged = now;
    }
    else if (now - sClicksChanged > WHEEL_FOLLOW_SECONDS)
    {
        sHandledClicks = total;
    }
}

// Frames are counted by publish(), as the native display() does not count them.
U64 sFrame = 1;
U64 sTakenFrame = 0;
U64 sTurnedFrame = 0;
asr::MouseMotion::Total sTaken = 0;
F64 sLastRawMotion = -1.0e9;
bool sLoggedRaw = false;
bool sLoggedCursor = false;

enum class Tool { Orbit, Mouselook };
Tool sTool = Tool::Orbit;
F32 sYawPerUnit = 0.f;
F32 sPitchPerUnit = 0.f;

// What LLToolCamera's last hover took, and whether it orbited by it.
bool sOrbitRaw = false;
F32 sOrbitDx = 0.f;
F32 sOrbitDy = 0.f;
bool sRawOrbiting = false;

bool enabled()
{
    static LLCachedControl<bool> mouse_smooth(gSavedSettings, "MouseSmooth", false);
    return sRunning && !mouse_smooth();
}

// The motion since the last take, x to the right and y up. True when it is the mouse's own
// motion; false when it is the cursor's delta given.
bool takeMotion(S32 cursor_dx, S32 cursor_dy, F32& dx, F32& dy)
{
    dx = (F32)cursor_dx;
    dy = (F32)cursor_dy;
    if (!enabled())
    {
        return false;
    }

    const asr::MouseMotion::Total total = sMotion.total();
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (total != sTaken)
    {
        sLastRawMotion = now;
        if (!sLoggedRaw)
        {
            sLoggedRaw = true;
            LL_INFOS("ASRenderer") << "raw mouse motion arrives" << LL_ENDL;
        }
    }
    F32 raw_dx = 0.f;
    F32 raw_dy = 0.f;
    if (sTakenFrame == sFrame || sTakenFrame + 1 == sFrame)
    {
        asr::MouseMotion::between(sTaken, total, raw_dx, raw_dy);
    }
    sTaken = total;
    sTakenFrame = sFrame;

    if ((cursor_dx != 0 || cursor_dy != 0) && now - sLastRawMotion > RAW_SILENCE_SECONDS)
    {
        if (!sLoggedCursor)
        {
            sLoggedCursor = true;
            LL_INFOS("ASRenderer") << "the cursor moves with no raw mouse motion: camera turns by the cursor until it comes" << LL_ENDL;
        }
        return false;
    }
    dx = raw_dx;
    dy = raw_dy;
    return true;
}

// A tool turned the camera this frame: the yaw by dx times the first rate, the pitch by dy
// times the second, in radians, through LLAgentCamera's orbit or LLAgent's rotations.
void turned(Tool tool, F32 yaw_per_unit, F32 pitch_per_unit)
{
    sTurnedFrame = sFrame;
    sTool = tool;
    sYawPerUnit = yaw_per_unit;
    sPitchPerUnit = pitch_per_unit;
}

// At the rate the cursor orbits, scaled by ASCameraOrbitSpeed. Returns the radians per unit.
F32 orbitBy(F32 dx, F32 dy)
{
    static LLCachedControl<F32> orbit_speed(gSavedSettings, "ASCameraOrbitSpeed", 1.f);
    const F32 radians_per_unit = 360.f * DEG_TO_RAD / gViewerWindow->getWorldViewWidthScaled() * orbit_speed;
    if (dx != 0.f)
    {
        gAgentCamera.cameraOrbitAround(-dx * radians_per_unit);
    }
    if (dy != 0.f)
    {
        gAgentCamera.cameraOrbitOver(-dy * radians_per_unit);
    }
    return radians_per_unit;
}

void copy(const LLVector3& from, float to[3])
{
    std::copy(from.mV, from.mV + 3, to);
}

} // namespace

// The camera tool's orbit and mouselook turn the camera by the mouse, and the render thread
// turns it on by the motion that comes before the next frame. This says how, from what built
// the camera, in the three ways those tools turn it. The two pivots are what make the
// third-person camera exact: its eye turns about the head, and what it looks at about the
// agent. The wheel's zoom moves the eye along its arm, and is described with the same
// pivots, see describeZoom(). Anything else, such as a camera animating to a new mode, says
// nothing, and the renderer shows the camera as it is. A friend of LLAgentCamera, to read
// how it built it.
class ASRCameraTurnAccess
{
public:
    // False when no zoom can be on: describe() is then only needed while the mouse turns.
    static bool mayZoom(const LLAgentCamera& agent_camera)
    {
        const F32 target = agent_camera.mTargetCameraDistance;
        return LLFrameTimer::getTotalSeconds() - sWheelZoomed < WHEEL_FOLLOW_SECONDS
            || llabs(target - agent_camera.mCurrentCameraDistance) > 1.0e-3f * target + 1.0e-4f;
    }

    // In agent space. False when the camera does not turn this way. With `turning` false, the
    // pivots only place the zoom, if there is one, and turn.active stays false.
    static bool describe(LLAgentCamera& agent_camera, bool turning, asr::CameraTurn& turn)
    {
        if (agent_camera.mCameraAnimating)
        {
            return false;
        }
        LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
        if (selection->getObjectCount() && selection->getSelectType() == SELECT_TYPE_HUD)
        {
            return false; // the orbit does nothing
        }

        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 eye = camera->getOrigin();
        F32 yaw_per_unit = sYawPerUnit;
        LLVector3 yaw_axis;
        LLVector3 pitch_axis;
        F32 largest_pitch = 0.f;
        F32 smallest_pitch = 0.f;
        LLVector3 eye_pivot;
        LLVector3 focus_pivot;

        // LLAgent::yaw(), rotate() and pitch() turn the agent's frame, which is the parent's
        // when sitting. Its axes, in agent space, and LLAgent::pitch()'s limits: no lower than
        // 179 degrees from up, measured on the frame, and no higher than 5, on the view.
        const auto describe_agent_turn = [&](bool parent_turns_camera)
        {
            LLQuaternion to_agent;
            if (isAgentAvatarValid() && gAgentAvatarp->getParent() && parent_turns_camera)
            {
                to_agent = ((LLViewerObject*)gAgentAvatarp->getParent())->getRenderRotation();
            }
            const LLCoordFrame& frame = gAgent.getFrameAgent();
            const LLVector3 skyward = gAgent.getReferenceUpVector();
            yaw_axis = skyward * to_agent;
            yaw_axis.normalize();
            pitch_axis = frame.getLeftAxis() * to_agent;
            pitch_axis.normalize();
            const LLVector3 look_dir = gAgent.getPosAgentFromGlobal(agent_camera.calcFocusPositionTargetGlobal()) - eye;
            largest_pitch = 179.f * DEG_TO_RAD - acosf(llclamp(frame.getAtAxis() * skyward, -1.f, 1.f));
            smallest_pitch = 5.f * DEG_TO_RAD - angle_between(look_dir, skyward);
        };

        const ECameraMode mode = agent_camera.mCameraMode;
        const bool on_avatar = agent_camera.mFocusOnAvatar;
        LLVector3 focus = gAgent.getPosAgentFromGlobal(agent_camera.mFocusGlobal);
        if (turning && (sTool == Tool::Mouselook) != (mode == CAMERA_MODE_MOUSELOOK))
        {
            return false;
        }
        bool zooms_about_head = false;
        if (mode == CAMERA_MODE_MOUSELOOK)
        {
            if (!turning || !isAgentAvatarValid())
            {
                return false; // the wheel does not zoom in mouselook
            }
            // LLToolGun turns the view about the eye.
            LLViewerObject* root = (LLViewerObject*)gAgentAvatarp->getRoot();
            describe_agent_turn(!root || !root->flagCameraDecoupled());
            eye_pivot = eye;
            focus_pivot = eye;
            focus = eye + camera->getAtAxis();
        }
        else if (on_avatar && mode == CAMERA_MODE_THIRD_PERSON)
        {
            if (agent_camera.mSitCameraEnabled && isAgentAvatarValid() && gAgentAvatarp->isSitting()
                && agent_camera.mSitCameraReferenceObject.notNull())
            {
                return false; // the sit camera does not turn
            }
            // cameraOrbitAround() and cameraOrbitOver() turn the agent, which the camera follows:
            // see calcCameraPositionTargetGlobal(), and the field of view correction in updateCamera().
            describe_agent_turn(true);
            if (gAgent.rotateGrabbed())
            {
                yaw_per_unit = 0.f;
            }
            const LLVector3 agent = gAgent.getPositionAgent();
            const LLVector3 head = (isAgentAvatarValid() ? agent_camera.getAvatarRootPosition() : agent)
                + agent_camera.mThirdPersonHeadOffset - agent_camera.mCameraLag;
            eye_pivot = agent + (head - agent) * (1.f + agent_camera.mCameraFOVZoomFactor);
            focus_pivot = agent;
            zooms_about_head = true;
        }
        else if (mode != CAMERA_MODE_MOUSELOOK && !(on_avatar && mode == CAMERA_MODE_FOLLOW))
        {
            // cameraOrbitAround() and cameraOrbitOver() turn the focus offset, about up and the
            // camera's left, and keep it between 1 and 179 degrees from up.
            yaw_axis.setVec(0.f, 0.f, 1.f);
            pitch_axis = camera->getLeftAxis();
            LLVector3 offset_unit(agent_camera.mCameraFocusOffsetTarget);
            offset_unit.normalize();
            const F32 angle_from_up = acosf(llclamp(offset_unit * gAgent.getReferenceUpVector(), -1.f, 1.f));
            largest_pitch = angle_from_up - 1.f * DEG_TO_RAD;
            smallest_pitch = angle_from_up - 179.f * DEG_TO_RAD;
            eye_pivot = focus;
            focus_pivot = focus;
        }
        else
        {
            return false;
        }

        if (!turning)
        {
            yaw_per_unit = 0.f;
        }
        turn.active = turning;
        turn.yawPerUnit = yaw_per_unit;
        turn.pitchPerUnit = turning ? sPitchPerUnit : 0.f;
        copy(yaw_axis, turn.yawAxis);
        copy(pitch_axis, turn.pitchAxis);
        turn.largestPitch = largest_pitch;
        turn.smallestPitch = smallest_pitch;
        copy(eye_pivot, turn.eyePivot);
        copy(eye - eye_pivot, turn.eyeArm);
        copy(focus_pivot, turn.focusPivot);
        copy(focus - focus_pivot, turn.focusArm);
        copy(agent_camera.mCameraUpVector, turn.up);
        turn.roll = agent_camera.mRollAngle;
        if (mode != CAMERA_MODE_MOUSELOOK)
        {
            describeZoom(agent_camera, zooms_about_head, focus, turning, turn.zoom);
        }
        return true;
    }

private:
    // The zoom as updateCamera() eases it, in agent space. The eye before smoothing is the
    // camera's target pushed out by the field of view correction, and lerps towards the eye at
    // the target distance: about the head for the camera on the avatar, which lerps its
    // distance, and about the focus otherwise, whose offset jumps (CAMERA_FOCUS_HALF_LIFE).
    // Then the smoothing lerps the shown eye towards it. A click scales the target distance as
    // handleScrollWheel() does, within the limits of cameraOrbitIn().
    static void describeZoom(LLAgentCamera& agent_camera, bool about_head, const LLVector3& focus, bool turning, asr::CameraZoom& zoom)
    {
        static LLCachedControl<bool> no_wheel_zoom(gSavedSettings, "FSDisableMouseWheelCameraZoom");
        static LLCachedControl<bool> no_constraints(gSavedSettings, "DisableCameraConstraints", false);
        static LLCachedControl<F32> smoothing(gSavedSettings, "CameraPositionSmoothing");

        const F32 fov_scale = 1.f + agent_camera.mCameraFOVZoomFactor;
        const LLVector3 eye_now = focus + (agent_camera.mCameraVirtualPositionAgent - focus) * fov_scale;
        LLVector3 origin;
        LLVector3 eye_target;
        F32 target_distance = 0.f;
        bool easing = false;
        if (about_head)
        {
            const LLVector3 agent = gAgent.getPositionAgent();
            const LLVector3 head = (isAgentAvatarValid() ? agent_camera.getAvatarRootPosition() : agent)
                + agent_camera.mThirdPersonHeadOffset - agent_camera.mCameraLag;
            const F32 current = agent_camera.mCurrentCameraDistance;
            target_distance = agent_camera.mTargetCameraDistance;
            if (!(current > 0.f))
            {
                return;
            }
            origin = focus + (head - focus) * fov_scale;
            eye_target = origin + (agent_camera.mCameraVirtualPositionAgent - head) * (fov_scale * target_distance / current);
            const F32 offset_distance = agent_camera.getCameraOffsetInitial().magVec() * agent_camera.getCameraOffsetScale();
            zoom.nearest = no_constraints ? MIN_CAMERA_DISTANCE : llmax(MIN_ZOOM_FRACTION * offset_distance, MIN_CAMERA_DISTANCE);
            zoom.farthest = no_constraints ? FARTHEST_UNCONSTRAINED : llmax(MAX_ZOOM_FRACTION * offset_distance, MIN_CAMERA_DISTANCE);
            zoom.zoomHalfLife = CAMERA_ZOOM_HALF_LIFE;
            easing = llabs(target_distance - current) > 1.0e-3f * target_distance + 1.0e-4f;
        }
        else
        {
            const LLVector3 offset_target(agent_camera.mCameraFocusOffsetTarget);
            origin = focus;
            eye_target = focus + offset_target * fov_scale;
            target_distance = offset_target.magVec();
            F32 nearest = LAND_MIN_ZOOM;
            if (agent_camera.mFocusObject.notNull())
            {
                nearest = agent_camera.mFocusObject->isAvatar() ? AVATAR_MIN_ZOOM : OBJECT_MIN_ZOOM;
            }
            zoom.nearest = no_constraints ? 0.f : nearest;
            zoom.farthest = agent_camera.getCameraMaxZoomDistance(true);
            if (!no_constraints && agent_camera.mCameraMode == CAMERA_MODE_CUSTOMIZE_AVATAR)
            {
                zoom.nearest = llclamp(zoom.nearest, APPEARANCE_MIN_ZOOM, APPEARANCE_MAX_ZOOM);
                zoom.farthest = llclamp(zoom.farthest, APPEARANCE_MIN_ZOOM, APPEARANCE_MAX_ZOOM);
            }
            zoom.zoomHalfLife = CAMERA_FOCUS_HALF_LIFE;
        }

        // handleScrollWheel() changes offsets instead with these held, on the avatar.
        const MASK mask = gKeyboard ? gKeyboard->currentMask(true) : 0;
        const bool clicks_zoom = !no_wheel_zoom && sWheelRunning && !(about_head && (mask & (MASK_SHIFT | MASK_CONTROL)));
        zoom.followClicks = clicks_zoom && LLFrameTimer::getTotalSeconds() - sWheelZoomed < WHEEL_FOLLOW_SECONDS;
        // A camera on the avatar smooths relative to the agent, so its smoothing only lags what
        // the zoom and turns move. Any other smooths in the world, so a moving focus lags too,
        // which this cannot follow: it is followed only while the wheel is in use.
        zoom.active = (easing || zoom.followClicks || LLFrameTimer::getTotalSeconds() - sWheelZoomed < WHEEL_FOLLOW_SECONDS)
            && target_distance > 0.f;
        if (!zoom.active)
        {
            return;
        }
        // updateCamera() smooths only these, and not while the mouse turns the camera.
        const bool smoothed = agent_camera.cameraThirdPerson() && !LLToolMgr::getInstance()->inBuildMode() && !turning;
        zoom.smoothingHalfLife = smoothed ? llmax(smoothing() * SMOOTHING_HALF_LIFE, 0.f) : 0.f;
        zoom.stepSeconds = gFrameIntervalSeconds.value();
        zoom.included = sHandledClicks;
        zoom.targetDistance = target_distance;
        zoom.perClick = sqrtf(F_SQRT2);
        copy(eye_now, zoom.eyeNow);
        copy(eye_target, zoom.eyeTarget);
        copy(origin, zoom.origin);
    }
};

#endif // ASR_TURN_SUPPORTED

void ASRCameraTurn::start(LLWindow* window)
{
#if ASR_TURN_SUPPORTED
    if (!ASR::active() || !window)
    {
        return;
    }
    sWindow = window;
#if LL_DARWIN
    asr::listenToMice(&sMotion);
    sRunning = true;
#else
    sRunning = window->setRawMouseMotionCallback(&addMotion);
    sWheelRunning = window->setWheelClicksCallback(&addClicks);
#endif
    LL_INFOS("ASRenderer") << (sRunning ? "camera turns by raw mouse motion" : "no raw mouse motion here: camera turns by the cursor")
                           << (sWheelRunning ? ", and zooms by wheel clicks before the main thread handles them" : "") << LL_ENDL;
#endif
}

void ASRCameraTurn::stop()
{
#if ASR_TURN_SUPPORTED
    if (sRunning)
    {
#if LL_DARWIN
        asr::listenToMice(nullptr);
#else
        sWindow->setRawMouseMotionCallback(nullptr);
#endif
    }
#if !LL_DARWIN
    if (sWheelRunning)
    {
        sWindow->setWheelClicksCallback(nullptr);
    }
#endif
    sRunning = false;
    sWheelRunning = false;
    sWindow = nullptr;
#endif
}

void ASRCameraTurn::wheelHandled(S32 clicks)
{
#if ASR_TURN_SUPPORTED
    if (sWheelRunning)
    {
        sHandledClicks = S32(U32(sHandledClicks) + U32(clicks));
    }
#endif
}

void ASRCameraTurn::wheelZoomed()
{
#if ASR_TURN_SUPPORTED
    sWheelZoomed = LLFrameTimer::getTotalSeconds();
#endif
}

const asr::MouseMotion* ASRCameraTurn::motion()
{
#if ASR_TURN_SUPPORTED
    return &sMotion;
#else
    return nullptr;
#endif
}

void ASRCameraTurn::takeOrbitMotion(bool captured, S32 cursor_dx, S32 cursor_dy)
{
#if ASR_TURN_SUPPORTED
    sRawOrbiting = false;
    sOrbitRaw = captured && takeMotion(cursor_dx, cursor_dy, sOrbitDx, sOrbitDy);
#endif
}

bool ASRCameraTurn::orbit()
{
#if ASR_TURN_SUPPORTED
    if (!sOrbitRaw)
    {
        return false;
    }
    const F32 radians_per_unit = orbitBy(sOrbitDx, sOrbitDy);
    turned(Tool::Orbit, -radians_per_unit, -radians_per_unit);
    sRawOrbiting = true;
    gViewerWindow->moveCursorToCenter();
    return true;
#else
    return false;
#endif
}

void ASRCameraTurn::endOrbit()
{
#if ASR_TURN_SUPPORTED
    if (sRawOrbiting)
    {
        F32 dx = 0.f;
        F32 dy = 0.f;
        if (takeMotion(0, 0, dx, dy))
        {
            orbitBy(dx, dy);
        }
    }
    sRawOrbiting = false;
    sOrbitRaw = false;
#endif
}

void ASRCameraTurn::takeLookMotion(F32 mouse_sensitivity, F32& dx, F32& dy)
{
    S32 cursor_dx = gViewerWindow->getCurrentMouseDX();
    S32 cursor_dy = gViewerWindow->getCurrentMouseDY();
#if ASR_TURN_SUPPORTED
    F32 motion_dx = 0.f;
    F32 motion_dy = 0.f;
    if (takeMotion(cursor_dx, cursor_dy, motion_dx, motion_dy))
    {
        // LLToolGun pitches by the sensitivity times -dy, or dy when inverted, and yaws by
        // it times -dx, both of the motion as taken here.
        static LLCachedControl<bool> invert_mouse(gSavedSettings, "InvertMouse");
        turned(Tool::Mouselook, -mouse_sensitivity, invert_mouse ? mouse_sensitivity : -mouse_sensitivity);
    }
    dx = -motion_dx;
    dy = -motion_dy;
#else
    dx = (F32)-cursor_dx;
    dy = (F32)-cursor_dy;
#endif
}

bool ASRCameraTurn::turning()
{
#if ASR_TURN_SUPPORTED
    return sTurnedFrame == sFrame;
#else
    return false;
#endif
}

void ASRCameraTurn::publish(asr::CameraTurn& turn, const LLVector3& origin)
{
#if ASR_TURN_SUPPORTED
    turn = asr::CameraTurn{};
    if (sWheelRunning)
    {
        resyncClicks();
    }
    const bool turning = sTurnedFrame == sFrame;
    if ((turning || ASRCameraTurnAccess::mayZoom(gAgentCamera)) && ASRCameraTurnAccess::describe(gAgentCamera, turning, turn)
        && (turn.active || turn.zoom.active))
    {
        turn.included = sTaken;
        for (int i = 0; i < 3; ++i)
        {
            turn.eyePivot[i] += origin.mV[i];
            turn.focusPivot[i] += origin.mV[i];
            turn.zoom.eyeNow[i] += origin.mV[i];
            turn.zoom.eyeTarget[i] += origin.mV[i];
            turn.zoom.origin[i] += origin.mV[i];
        }
    }
    else
    {
        turn = asr::CameraTurn{};
    }
    ++sFrame;
#endif
}
