/**
 * asrrenderthread.h
 * The free-running render thread of section 0 of the renderer plan. It renders
 * from the newest published state and never waits for the main thread, so a
 * main-thread stall cannot freeze the view. After start(), only this thread
 * touches the device, until stop().
 */
#pragma once

#include "asrdevice.h"
#include "asrlatestvalue.h"
#include "asrmousemotion.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace asr
{

// How the published camera turns with the mouse, while the user turns it. The main
// thread turned it by the motion up to `included`, and the render thread turns it by
// the motion since, the same way, so a frame shows the hand's position of that moment
// and the next main-thread camera agrees with it. The eye and the point looked at each
// turn around a pivot of their own, as the third-person camera does, then the view
// looks from one to the other. All in the camera's space, as SceneCamera3D.
struct CameraTurn
{
    bool active = false;
    MouseMotion::Total included = 0;
    float yawPerUnit = 0.f;   // radians, of the motion's x and y
    float pitchPerUnit = 0.f;
    float yawAxis[3] = {};    // unit axes, turned about by the right-hand rule
    float pitchAxis[3] = {};
    float largestPitch = 0.f; // the pitch the main thread allows, as it limits it: a
    float smallestPitch = 0.f; // pitch above 0 stops at the first, one below at the second
    float eyePivot[3] = {};
    float eyeArm[3] = {};     // from the pivot to the eye
    float focusPivot[3] = {};
    float focusArm[3] = {};
    float up[3] = {};         // the up the view is built with
    float roll = 0.f;         // then turned about its view axis by this, in radians
};

// The published camera, turned by the motion given, with both pivots moved by `shift`.
// Exactly the published camera when there is neither.
SceneCamera3D turnCamera(const SceneCamera3D& published, const CameraTurn& turn, float dx, float dy, const float shift[3]);

// What the main thread hands over. Later phases add motion.
struct FrameState
{
    std::chrono::steady_clock::time_point published{};
    uint32_t width = 0; // drawable size in pixels
    uint32_t height = 0;
    bool testTriangle = false;
    bool hasCamera = false; // the scene is drawn from this camera when set
    SceneCamera3D camera{};
    CameraTurn turn{};
    bool predictCamera = false; // see CameraPredictor; while turning, only the pivots
    bool audit = false;     // see FrameInput
    float maxFramesPerSecond = 0.f; // the user's frame limit, which paces this thread; 0 is none
};

// Makes the camera move on every frame, not only on those that follow a main-thread step.
// The main thread can run far slower than the render thread, and every frame between two of
// its steps would repeat the same camera. This carries on the motion of the last two steps
// for the time since the newest one, and when a new step disagrees with what was shown, the
// difference fades over a fixed real time, so a slow main thread does not make it slower.
class CameraPredictor
{
public:
    using Clock = std::chrono::steady_clock;

    // Returns the camera to draw at the time given. The newest camera has to be the one
    // published at that time stamp.
    SceneCamera3D predict(const SceneCamera3D& newest, Clock::time_point published, Clock::time_point now);

    // The same for a point alone, such as the pivot of a camera the mouse turns: the
    // turn is known exactly, and only how the pivot moves needs predicting.
    void predictPoint(const float newest[3], Clock::time_point published, Clock::time_point now, float shown[3]);

    // Forgets all motion: the next camera lands as it is, as after a cut. For a switch
    // between predicting cameras and predicting points.
    void reset() { mStarted = false; }

private:
    struct Vec
    {
        float x = 0.f, y = 0.f, z = 0.f;
    };
    struct Quat
    {
        float w = 1.f, x = 0.f, y = 0.f, z = 0.f;
    };

    // Moves the prediction on to `now` and leaves what to show in mShownEye and
    // mShownRotation. True when nothing moves, and the newest step is shown as it is.
    bool step(const Vec& eye, const Quat& rotation, Clock::time_point published, Clock::time_point now);

    static Quat rotationOf(const SceneCamera3D& camera);
    static Quat multiply(const Quat& a, const Quat& b);
    static Quat fromVector(const Vec& rotation);
    static Vec toVector(const Quat& rotation); // axis times angle

    Clock::time_point mPublished{};
    Clock::time_point mLastFrame{};
    Vec mEye;
    Quat mRotation;
    float mStepSeconds = 0.f;  // between the last two steps
    Vec mVelocity;             // metres a second
    Vec mSpin;                 // axis times radians a second
    float mSpeed = 0.f;        // of the last step, before any scaling
    float mSpinRate = 0.f;
    Vec mEyeError;            // what was shown minus what the prediction says, fading out
    Vec mRotationError;
    Vec mShownEye;
    Quat mShownRotation;
    bool mStarted = false;
};

struct FrameStats
{
    uint64_t frames = 0;
    double worstFrameSeconds = 0.0;
    double worstStateAgeSeconds = 0.0; // how old the newest state was, at its worst
    double averageStateAgeSeconds = 0.0;
    uint64_t newStates = 0;            // frames that had a state no earlier frame used: only
                                       // these show the world moving, so judge this with frames
    FrameParts worstFrameParts;        // where the worst frame's time went
    double worstRefreshSeconds = 0.0;  // the memory budget refresh, between two frames
    uint64_t turnedFrames = 0;         // frames the mouse turned, from motion the main
                                       // thread had not seen yet
};

class RenderThread
{
public:
    // The mouse motion that CameraTurn counts in. Without it the camera never turns here.
    RenderThread(Device& device, const MouseMotion* motion) : mDevice(device), mMotion(motion) {}
    ~RenderThread() { stop(); }

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    void start();
    void stop();

    // Main thread: fill in state(), then publish it. Neither call ever waits.
    FrameState& state() { return mState.writeSlot(); }
    void publishState();

    // Main thread: clear ui(), fill it, then publish it. The UI goes on drawing
    // from the last published list for as long as no new one arrives.
    UIList& ui() { return mUI.writeSlot(); }
    void publishUI();

    // Main thread: the generation the list being filled will get. Passed to
    // TextureQueue::destroy(), because that list may already use the texture.
    uint64_t currentUIGeneration() const { return mUIGeneration + 1; }

    // Main thread: stats since the previous call.
    FrameStats takeStats();

    // Any thread: every frame presented so far.
    uint64_t presentedFrames() const { return mPresented.load(std::memory_order_relaxed); }

    // True once rendering has stopped on an unrecoverable error.
    bool failed() const { return mFailed.load(std::memory_order_acquire); }

    // Valid once failed() is true.
    const std::string& error() const { return mError; }

private:
    void run();

    Device& mDevice;
    const MouseMotion* mMotion;
    LatestValue<FrameState> mState;
    LatestValue<UIList> mUI;
    uint64_t mUIGeneration = 0;
    std::thread mThread;
    std::atomic<bool> mStop{ false };
    std::atomic<bool> mFailed{ false };
    std::string mError;

    std::atomic<uint64_t> mFrames{ 0 };
    std::atomic<uint64_t> mPresented{ 0 };
    std::atomic<uint64_t> mWorstFrameMicros{ 0 };
    std::atomic<uint64_t> mWorstAgeMicros{ 0 };
    std::atomic<uint64_t> mAgeSumMicros{ 0 };
    std::atomic<uint64_t> mNewStates{ 0 };
    std::atomic<uint64_t> mWorstRefreshMicros{ 0 };
    std::atomic<uint64_t> mTurnedFrames{ 0 };

    // Taken only when a frame is the worst so far, and by takeStats().
    std::mutex mWorstPartsMutex;
    FrameParts mWorstParts;
};

} // namespace asr
