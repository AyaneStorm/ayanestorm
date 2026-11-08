/**
 * @file asrcameraturn.cpp
 * @brief See asrcameraturn.h.
 */
#include "llviewerprecompiledheaders.h"

#include "asrcameraturn.h"

#include "asrbridge.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llframetimer.h"
#include "llselectmgr.h"
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

asr::MouseMotion sMotion;

#if LL_WINDOWS
void addMotion(float dx, float dy)
{
    sMotion.add(dx, dy);
}
#endif

LLWindow* sWindow = nullptr;
bool sRunning = false;

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
// agent. Anything else, such as a camera animating to a new mode, says nothing, and the
// renderer shows the camera as it is. A friend of LLAgentCamera, to read how it built it.
class ASRCameraTurnAccess
{
public:
    // In agent space. False when the camera does not turn this way.
    static bool describe(LLAgentCamera& agent_camera, asr::CameraTurn& turn)
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
        if (sTool == Tool::Mouselook)
        {
            if (mode != CAMERA_MODE_MOUSELOOK || !isAgentAvatarValid())
            {
                return false;
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

        turn.yawPerUnit = yaw_per_unit;
        turn.pitchPerUnit = sPitchPerUnit;
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
        return true;
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
#endif
    LL_INFOS("ASRenderer") << (sRunning ? "camera turns by raw mouse motion" : "no raw mouse motion here: camera turns by the cursor") << LL_ENDL;
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
    sRunning = false;
    sWindow = nullptr;
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
    if (sTurnedFrame == sFrame && ASRCameraTurnAccess::describe(gAgentCamera, turn))
    {
        turn.active = true;
        turn.included = sTaken;
        for (int i = 0; i < 3; ++i)
        {
            turn.eyePivot[i] += origin.mV[i];
            turn.focusPivot[i] += origin.mV[i];
        }
    }
    ++sFrame;
#endif
}
