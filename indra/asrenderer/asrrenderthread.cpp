/**
 * asrrenderthread.cpp
 * See asrrenderthread.h.
 */
#include "asrrenderthread.h"

#include <algorithm>
#include <cmath>

namespace asr
{

namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::chrono::seconds MEMORY_REFRESH_PERIOD(1);
constexpr std::chrono::milliseconds MINIMIZED_SLEEP(5);

uint64_t micros(Clock::duration duration)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

// Only the render thread raises the value, and takeStats() only zeroes it, so a
// lost update costs one reading at worst.
void raiseTo(std::atomic<uint64_t>& worst, uint64_t value)
{
    if (value > worst.load(std::memory_order_relaxed))
    {
        worst.store(value, std::memory_order_relaxed);
    }
}

constexpr float LONGEST_PREDICTION = 0.25f;    // seconds: past this the camera holds still
constexpr float PREDICT_STEPS = 1.1f;          // as a share of the time between two steps
constexpr float SLOWEST_STEP = 0.5f;           // seconds: with steps this far apart there is no motion to carry on
constexpr float ERROR_HALF_LIFE = 0.06f;       // seconds
constexpr float LARGEST_MOVE = 32.f;           // metres in one step: more is a cut, not motion
constexpr float LARGEST_TURN = 1.6f;           // radians in one step, as above
constexpr float MIN_ZOOM_STEP = 0.001f;        // seconds: a zoom is stepped no finer than this
constexpr std::chrono::seconds LONGEST_ZOOM(2); // past this every zoom has settled

} // namespace

// Rows of the view matrix are the camera's side, up and back axes in world space, which
// makes them the columns of its rotation.
CameraPredictor::Quat CameraPredictor::rotationOf(const SceneCamera3D& camera)
{
    const float (*m)[4] = camera.viewRows;
    const float xx = m[0][0], xy = m[1][0], xz = m[2][0];
    const float yx = m[0][1], yy = m[1][1], yz = m[2][1];
    const float zx = m[0][2], zy = m[1][2], zz = m[2][2];

    Quat q;
    const float trace = xx + yy + zz;
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        q = { s * 0.25f, (zy - yz) / s, (xz - zx) / s, (yx - xy) / s };
    }
    else if (xx > yy && xx > zz)
    {
        const float s = std::sqrt(1.f + xx - yy - zz) * 2.f;
        q = { (zy - yz) / s, s * 0.25f, (xy + yx) / s, (xz + zx) / s };
    }
    else if (yy > zz)
    {
        const float s = std::sqrt(1.f + yy - xx - zz) * 2.f;
        q = { (xz - zx) / s, (xy + yx) / s, s * 0.25f, (yz + zy) / s };
    }
    else
    {
        const float s = std::sqrt(1.f + zz - xx - yy) * 2.f;
        q = { (yx - xy) / s, (xz + zx) / s, (yz + zy) / s, s * 0.25f };
    }
    return q;
}

CameraPredictor::Quat CameraPredictor::multiply(const Quat& a, const Quat& b)
{
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
             a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}

CameraPredictor::Quat CameraPredictor::fromVector(const Vec& rotation)
{
    const float angle = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z);
    if (angle < 1e-7f)
    {
        return { 1.f, rotation.x * 0.5f, rotation.y * 0.5f, rotation.z * 0.5f };
    }
    const float scale = std::sin(angle * 0.5f) / angle;
    return { std::cos(angle * 0.5f), rotation.x * scale, rotation.y * scale, rotation.z * scale };
}

CameraPredictor::Vec CameraPredictor::toVector(const Quat& rotation)
{
    const float sign = rotation.w < 0.f ? -1.f : 1.f; // the short way round
    const float sine = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z);
    if (sine < 1e-7f)
    {
        return { sign * rotation.x * 2.f, sign * rotation.y * 2.f, sign * rotation.z * 2.f };
    }
    const float scale = sign * 2.f * std::atan2(sine, sign * rotation.w) / sine;
    return { rotation.x * scale, rotation.y * scale, rotation.z * scale };
}

bool CameraPredictor::step(const Vec& eye, const Quat& rotation, Clock::time_point published, Clock::time_point now)
{
    const auto seconds = [](Clock::duration duration) { return std::chrono::duration<float>(duration).count(); };

    const bool newStep = !mStarted || published != mPublished;
    if (newStep)
    {
        const Quat conjugate{ mRotation.w, -mRotation.x, -mRotation.y, -mRotation.z };
        const Vec turn = toVector(multiply(rotation, conjugate));
        const Vec move{ eye.x - mEye.x, eye.y - mEye.y, eye.z - mEye.z };
        const float step = seconds(published - mPublished);
        const float moved = std::sqrt(move.x * move.x + move.y * move.y + move.z * move.z);
        const float turned = std::sqrt(turn.x * turn.x + turn.y * turn.y + turn.z * turn.z);

        if (!mStarted || moved > LARGEST_MOVE || turned > LARGEST_TURN)
        {
            // A cut: it lands at once, and the view has no motion to carry on.
            mVelocity = {};
            mSpin = {};
            mStepSeconds = 0.f;
            mSpeed = 0.f;
            mSpinRate = 0.f;
            mEyeError = {};
            mRotationError = {};
            mShownEye = eye;
            mShownRotation = rotation;
        }
        else if (step <= 0.f || step > SLOWEST_STEP)
        {
            // After a stall nothing is known about the motion, but what is on screen still
            // has to fade to the new step and not jump there.
            mVelocity = {};
            mSpin = {};
            mStepSeconds = 0.f;
            mSpeed = 0.f;
            mSpinRate = 0.f;
        }
        else
        {
            // Only steady motion is worth carrying on. A camera easing in or out, as a zoom
            // does, changes speed at every step, and carrying on the last speed would pass
            // where it is going and then pull back, which shows as a bump. So the prediction
            // is scaled by how alike the last two speeds are: 1 when steady, 0 on a first step.
            const auto steadiness = [](float now, float before)
            {
                const float faster = std::max(now, before);
                return faster > 0.f ? std::min(now, before) / faster : 1.f;
            };
            const float speed = moved / step;
            const float spinRate = turned / step;
            const float moveTrust = steadiness(speed, mSpeed);
            const float turnTrust = steadiness(spinRate, mSpinRate);
            mSpeed = speed;
            mSpinRate = spinRate;
            mVelocity = { moveTrust * move.x / step, moveTrust * move.y / step, moveTrust * move.z / step };
            mSpin = { turnTrust * turn.x / step, turnTrust * turn.y / step, turnTrust * turn.z / step };
            mStepSeconds = step;
        }
        mEye = eye;
        mRotation = rotation;
        mPublished = published;
    }

    const float frameSeconds = mStarted ? std::max(seconds(now - mLastFrame), 0.f) : 0.f;
    const float ahead = std::clamp(seconds(now - published), 0.f, std::min(mStepSeconds * PREDICT_STEPS, LONGEST_PREDICTION));
    const Vec predictedEye{ mEye.x + mVelocity.x * ahead, mEye.y + mVelocity.y * ahead, mEye.z + mVelocity.z * ahead };
    const Quat predictedRotation = multiply(fromVector({ mSpin.x * ahead, mSpin.y * ahead, mSpin.z * ahead }), mRotation);
    mStarted = true;
    mLastFrame = now;

    if (newStep)
    {
        // Whatever this new step corrects has to fade, not jump.
        mEyeError = { mShownEye.x - predictedEye.x, mShownEye.y - predictedEye.y, mShownEye.z - predictedEye.z };
        const Quat conjugate{ predictedRotation.w, -predictedRotation.x, -predictedRotation.y, -predictedRotation.z };
        mRotationError = toVector(multiply(mShownRotation, conjugate));
    }
    const float fade = std::pow(0.5f, frameSeconds / ERROR_HALF_LIFE);
    mEyeError = { mEyeError.x * fade, mEyeError.y * fade, mEyeError.z * fade };
    mRotationError = { mRotationError.x * fade, mRotationError.y * fade, mRotationError.z * fade };

    mShownEye = { predictedEye.x + mEyeError.x, predictedEye.y + mEyeError.y, predictedEye.z + mEyeError.z };
    mShownRotation = multiply(fromVector(mRotationError), predictedRotation);

    // Something still comes through exactly as published, with nothing rebuilt.
    const auto zero = [](const Vec& v) { return v.x == 0.f && v.y == 0.f && v.z == 0.f; };
    if (zero(mVelocity) && zero(mSpin) && std::fabs(mEyeError.x) + std::fabs(mEyeError.y) + std::fabs(mEyeError.z) < 1e-5f
        && std::fabs(mRotationError.x) + std::fabs(mRotationError.y) + std::fabs(mRotationError.z) < 1e-6f)
    {
        mEyeError = {};
        mRotationError = {};
        return true;
    }
    return false;
}

SceneCamera3D CameraPredictor::predict(const SceneCamera3D& newest, Clock::time_point published, Clock::time_point now)
{
    if (step({ newest.position[0], newest.position[1], newest.position[2] }, rotationOf(newest), published, now))
    {
        return newest;
    }

    // The rotation's matrix columns are the side, up and back axes.
    const Quat& q = mShownRotation;
    const float length = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    const float w = q.w / length, x = q.x / length, y = q.y / length, z = q.z / length;
    const float up[3] = { 2.f * (x * y - w * z), 1.f - 2.f * (x * x + z * z), 2.f * (y * z + w * x) };
    const float forward[3] = { -2.f * (x * z + w * y), -2.f * (y * z - w * x), -(1.f - 2.f * (x * x + y * y)) };
    const float shownEye[3] = { mShownEye.x, mShownEye.y, mShownEye.z };

    const float verticalFov = 2.f * std::atan(1.f / newest.focal[1]);
    SceneCamera3D camera = makeCameraFromAxes(shownEye, forward, up, verticalFov, newest.focal[1] / newest.focal[0], newest.nearClip, newest.farClip);
    camera.lodScale = newest.lodScale;
    camera.debugView = newest.debugView;
    return camera;
}

void CameraPredictor::predictPoint(const float newest[3], Clock::time_point published, Clock::time_point now, float shown[3])
{
    if (step({ newest[0], newest[1], newest[2] }, Quat{}, published, now))
    {
        std::copy(newest, newest + 3, shown);
        return;
    }
    shown[0] = mShownEye.x;
    shown[1] = mShownEye.y;
    shown[2] = mShownEye.z;
}

namespace
{

void cross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// False, leaving it, when it is too short to have a direction.
bool normalize(float v[3])
{
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1e-12f))
    {
        return false;
    }
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

// About a unit axis, by the right-hand rule.
void rotate(const float v[3], const float axis[3], float angle, float out[3])
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float along = (v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2]) * (1.f - c);
    float side[3];
    cross(axis, v, side);
    for (int i = 0; i < 3; ++i)
    {
        out[i] = v[i] * c + side[i] * s + axis[i] * along;
    }
}

} // namespace

SceneCamera3D turnCamera(const SceneCamera3D& published, const CameraTurn& turn, float dx, float dy, const float shift[3],
                         bool armChanged)
{
    const float yaw = dx * turn.yawPerUnit;
    float pitch = dy * turn.pitchPerUnit;
    pitch = pitch > 0.f ? std::min(pitch, turn.largestPitch) : std::max(pitch, turn.smallestPitch);
    const bool shifted = shift[0] != 0.f || shift[1] != 0.f || shift[2] != 0.f;
    if (yaw == 0.f && pitch == 0.f && !shifted && !armChanged)
    {
        return published;
    }

    // Pitch about the published axis, then yaw: the main thread's order, up to terms
    // too small to see between two of its steps.
    float eye[3];
    float focus[3];
    float pitched[3];
    rotate(turn.eyeArm, turn.pitchAxis, pitch, pitched);
    rotate(pitched, turn.yawAxis, yaw, eye);
    rotate(turn.focusArm, turn.pitchAxis, pitch, pitched);
    rotate(pitched, turn.yawAxis, yaw, focus);
    for (int i = 0; i < 3; ++i)
    {
        eye[i] += turn.eyePivot[i] + shift[i];
        focus[i] += turn.focusPivot[i] + shift[i];
    }

    // As the viewer builds its view: left is up across forward, then up is made square.
    float forward[3] = { focus[0] - eye[0], focus[1] - eye[1], focus[2] - eye[2] };
    float left[3];
    cross(turn.up, forward, left);
    if (!normalize(forward) || !normalize(left))
    {
        return published;
    }
    float up[3];
    cross(forward, left, up);
    const float c = std::cos(turn.roll);
    const float s = std::sin(turn.roll);
    const float rolledUp[3] = { up[0] * c - left[0] * s, up[1] * c - left[1] * s, up[2] * c - left[2] * s };

    const float verticalFov = 2.f * std::atan(1.f / published.focal[1]);
    SceneCamera3D camera = makeCameraFromAxes(eye, forward, rolledUp, verticalFov, published.focal[1] / published.focal[0],
                                              published.nearClip, published.farClip);
    camera.lodScale = published.lodScale;
    camera.debugView = published.debugView;
    return camera;
}

void stepZoom(float eye[3], float shown[3], const float target[3], float seconds, float zoomHalfLife, float smoothingHalfLife)
{
    // LLSmoothInterpolation::getInterpolant(): a lerp by 1 - 2^(-step / half-life).
    const float eyeKeeps = zoomHalfLife > 0.f ? std::pow(0.5f, seconds / zoomHalfLife) : 0.f;
    const float shownKeeps = smoothingHalfLife > 0.f ? std::pow(0.5f, seconds / smoothingHalfLife) : 0.f;
    for (int i = 0; i < 3; ++i)
    {
        eye[i] = target[i] + (eye[i] - target[i]) * eyeKeeps;
        shown[i] = eye[i] + (shown[i] - eye[i]) * shownKeeps;
    }
}

void ZoomFollower::step(const CameraZoom& zoom)
{
    std::copy(mTo, mTo + 3, mFrom);
    stepZoom(mEye, mTo, mTarget, mStepSeconds, zoom.zoomHalfLife, zoom.smoothingHalfLife);
}

void ZoomFollower::retarget(const CameraTurn& turn, int32_t clicks)
{
    const CameraZoom& zoom = turn.zoom;
    float scale = 1.f;
    const int32_t unseen = zoom.followClicks ? MouseMotion::clicksBetween(zoom.included, clicks) : 0;
    if (unseen != 0 && zoom.targetDistance > 0.f)
    {
        const float distance = std::clamp(zoom.targetDistance * std::pow(zoom.perClick, float(unseen)), zoom.nearest, zoom.farthest);
        scale = distance / zoom.targetDistance;
    }
    for (int i = 0; i < 3; ++i)
    {
        const float origin = zoom.origin[i] - turn.eyePivot[i];
        mTarget[i] = origin + (zoom.eyeTarget[i] - turn.eyePivot[i] - origin) * scale;
    }
    mClicks = clicks;
}

void ZoomFollower::follow(const CameraTurn& turn, const float publishedEye[3], int32_t clicks, Clock::time_point published,
                          Clock::time_point now, float arm[3])
{
    const auto seconds = [](Clock::duration duration) { return std::max(std::chrono::duration<float>(duration).count(), 0.f); };
    const CameraZoom& zoom = turn.zoom;
    const bool newState = !mStarted || published != mPublished;
    if (newState)
    {
        // Clicks it had not handled when it published, it handles in its next step.
        for (int i = 0; i < 3; ++i)
        {
            mEye[i] = zoom.eyeNow[i] - turn.eyePivot[i];
            mTo[i] = publishedEye[i] - turn.eyePivot[i];
        }
        retarget(turn, clicks);
        mStepSeconds = std::max(zoom.stepSeconds, MIN_ZOOM_STEP);
        mFromTime = published;
        mPublished = published;
        step(zoom);
    }
    else if (clicks != mClicks)
    {
        // A click that came after the main thread took its input: it handles it in the step
        // after the next, which is the one this target starts.
        retarget(turn, clicks);
    }

    // Take the steps that have passed, then show the point between the last and the next.
    const auto stepLength = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(mStepSeconds));
    if (now - mFromTime > LONGEST_ZOOM)
    {
        // Settled long since: nothing is left to step through.
        std::copy(mTarget, mTarget + 3, mEye);
        std::copy(mTarget, mTarget + 3, mTo);
        std::copy(mTarget, mTarget + 3, mFrom);
        mFromTime = now;
    }
    while (now >= mFromTime + stepLength)
    {
        mFromTime += stepLength;
        step(zoom);
    }
    const float along = std::clamp(seconds(now - mFromTime) / mStepSeconds, 0.f, 1.f);
    float shown[3];
    for (int i = 0; i < 3; ++i)
    {
        shown[i] = mFrom[i] + (mTo[i] - mFrom[i]) * along;
    }

    const float frameSeconds = mStarted ? seconds(now - mLastFrame) : 0.f;
    if (newState)
    {
        for (int i = 0; i < 3; ++i)
        {
            mError[i] = mStarted ? mArm[i] - shown[i] : 0.f;
        }
    }
    const float fade = std::pow(0.5f, frameSeconds / ERROR_HALF_LIFE);
    for (int i = 0; i < 3; ++i)
    {
        mError[i] *= fade;
        arm[i] = shown[i] + mError[i];
        mArm[i] = arm[i];
    }
    mLastFrame = now;
    mStarted = true;
}

void RenderThread::start()
{
    mStop.store(false, std::memory_order_relaxed);
    mThread = std::thread(&RenderThread::run, this);
}

void RenderThread::stop()
{
    if (mThread.joinable())
    {
        mStop.store(true, std::memory_order_relaxed);
        mThread.join();
    }
}

void RenderThread::publishState()
{
    mState.writeSlot().published = Clock::now();
    mState.publish();
}

void RenderThread::publishUI()
{
    mUI.writeSlot().generation = ++mUIGeneration;
    mUI.publish();
}

FrameStats RenderThread::takeStats()
{
    FrameStats stats;
    stats.frames = mFrames.exchange(0, std::memory_order_relaxed);
    stats.worstFrameSeconds = mWorstFrameMicros.exchange(0, std::memory_order_relaxed) * 1e-6;
    stats.worstStateAgeSeconds = mWorstAgeMicros.exchange(0, std::memory_order_relaxed) * 1e-6;
    const uint64_t ageSum = mAgeSumMicros.exchange(0, std::memory_order_relaxed);
    stats.averageStateAgeSeconds = stats.frames != 0 ? ageSum * 1e-6 / double(stats.frames) : 0.0;
    stats.newStates = mNewStates.exchange(0, std::memory_order_relaxed);
    stats.worstRefreshSeconds = mWorstRefreshMicros.exchange(0, std::memory_order_relaxed) * 1e-6;
    stats.turnedFrames = mTurnedFrames.exchange(0, std::memory_order_relaxed);
    stats.activeTurnFrames = mActiveTurnFrames.exchange(0, std::memory_order_relaxed);
    stats.zoomedFrames = mZoomedFrames.exchange(0, std::memory_order_relaxed);
    stats.clickFrames = mClickFrames.exchange(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mWorstPartsMutex);
    stats.worstFrameParts = mWorstParts;
    return stats;
}

void RenderThread::run()
{
    const Clock::time_point start = Clock::now();
    Clock::time_point previousFrame = start;
    Clock::time_point lastMemoryRefresh = start;
    Clock::time_point lastState{};
    Clock::time_point frameDue = start;
    CameraPredictor predictor;
    ZoomFollower zoomFollower;
    bool wasTurning = false;
    bool wasZooming = false;

    while (!mStop.load(std::memory_order_relaxed))
    {
        const FrameState& state = mState.read();
        if (state.width == 0 || state.height == 0)
        {
            // Nothing to present into: no state yet, or the window is minimized.
            std::this_thread::sleep_for(MINIMIZED_SLEEP);
            previousFrame = Clock::now();
            continue;
        }

        // A frame limit spaces frames from when each was due, so the rate holds without
        // bursts. After a late frame the next one is due a full period on, not at once.
        if (state.maxFramesPerSecond > 0.f)
        {
            if (frameDue > Clock::now())
            {
                std::this_thread::sleep_until(frameDue);
            }
            const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / state.maxFramesPerSecond));
            frameDue = std::max(frameDue + period, Clock::now());
        }

        const Clock::time_point frameStart = Clock::now();
        FrameInput input;
        input.width = state.width;
        input.height = state.height;
        input.seconds = std::chrono::duration<float>(frameStart - start).count();
        input.testTriangle = state.testTriangle;
        input.ui = &mUI.read();
        SceneCamera3D predicted;
        const bool zooming = state.hasCamera && state.turn.zoom.active && mMotion;
        const bool turning = state.hasCamera && (state.turn.active || zooming) && mMotion;
        if (turning != wasTurning)
        {
            // What is predicted changes, from the camera to its pivot or back.
            predictor.reset();
            wasTurning = turning;
        }
        if (zooming != wasZooming)
        {
            zoomFollower.reset();
            wasZooming = zooming;
        }
        if (turning)
        {
            // The motion is read as late as it can be, just before the frame uses it.
            float dx = 0.f;
            float dy = 0.f;
            if (state.turn.active)
            {
                MouseMotion::between(state.turn.included, mMotion->total(), dx, dy);
                mActiveTurnFrames.fetch_add(1, std::memory_order_relaxed);
            }
            CameraTurn zoomed;
            const CameraTurn* turn = &state.turn;
            if (zooming)
            {
                const int32_t clicks = mMotion->clicks();
                zoomed = state.turn;
                zoomFollower.follow(state.turn, state.camera.position, clicks, state.published, frameStart, zoomed.eyeArm);
                turn = &zoomed;
                mZoomedFrames.fetch_add(1, std::memory_order_relaxed);
                if (state.turn.zoom.followClicks && clicks != state.turn.zoom.included)
                {
                    mClickFrames.fetch_add(1, std::memory_order_relaxed);
                }
            }
            float shift[3] = {};
            if (state.predictCamera)
            {
                float pivot[3];
                predictor.predictPoint(state.turn.eyePivot, state.published, frameStart, pivot);
                for (int i = 0; i < 3; ++i)
                {
                    shift[i] = pivot[i] - state.turn.eyePivot[i];
                }
            }
            predicted = turnCamera(state.camera, *turn, dx, dy, shift, zooming);
            input.camera = &predicted;
            if (dx != 0.f || dy != 0.f)
            {
                mTurnedFrames.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else if (state.hasCamera && state.predictCamera)
        {
            predicted = predictor.predict(state.camera, state.published, frameStart);
            input.camera = &predicted;
        }
        else
        {
            input.camera = state.hasCamera ? &state.camera : nullptr;
        }
        input.audit = state.audit;
        if (!mDevice.renderFrame(input))
        {
            mError = mDevice.lastError();
            mFailed.store(true, std::memory_order_release);
            return;
        }

        const Clock::time_point frameEnd = Clock::now();
        const uint64_t frameMicros = micros(frameEnd - previousFrame);
        if (frameMicros > mWorstFrameMicros.load(std::memory_order_relaxed))
        {
            mWorstFrameMicros.store(frameMicros, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mWorstPartsMutex);
            mWorstParts = mDevice.frameParts();
        }
        const uint64_t ageMicros = micros(frameStart - state.published);
        raiseTo(mWorstAgeMicros, ageMicros);
        mAgeSumMicros.fetch_add(ageMicros, std::memory_order_relaxed);
        if (state.published != lastState)
        {
            lastState = state.published;
            mNewStates.fetch_add(1, std::memory_order_relaxed);
        }
        previousFrame = frameEnd;
        mFrames.fetch_add(1, std::memory_order_relaxed);
        mPresented.fetch_add(1, std::memory_order_relaxed);

        if (frameEnd - lastMemoryRefresh >= MEMORY_REFRESH_PERIOD)
        {
            lastMemoryRefresh = frameEnd;
            mDevice.refreshMemoryBudget();
            raiseTo(mWorstRefreshMicros, micros(Clock::now() - frameEnd));
        }
    }
}

} // namespace asr
