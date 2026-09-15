/**
 * @file asavatarrecovery.cpp
 * @author chanayane@firestorm
 * @brief Defensive self-avatar animation and skeleton recovery.
 */

#include "llviewerprecompiledheaders.h"

#include "asavatarrecovery.h"

#include "aoengine.h"
#include "llagent.h"
#include "llanimationstates.h"
#include "lleventtimer.h"
#include "llframetimer.h"
#include "llnotificationsutil.h"
#include "llvoavatarself.h"

namespace
{
    constexpr F32 PHYSICS_STABILIZATION_SECONDS = 1.f;
    bool sResetInProgress = false;
    U32 sPhysicsStabilizationGeneration = 0;

    // AyaneStorm owns the complete stabilization lifetime.
    class AvatarPhysicsStabilizer final : public LLEventTimer
    {
    public:
        AvatarPhysicsStabilizer(LLVOAvatarSelf* avatar, U32 generation,
                                bool recreate, bool notify_completion)
        : LLEventTimer(0.05f),
          mAvatar(avatar),
          mGeneration(generation),
          mRecreate(recreate),
          mNotifyCompletion(notify_completion)
        {
        }

        bool tick() override
        {
            // A newer request owns the current controller and its resume time.
            if (mGeneration != sPhysicsStabilizationGeneration)
            {
                return true;
            }

            if (mAvatar.isNull() || mAvatar->isDead())
            {
                return true;
            }

            if (mElapsed.getElapsedTimeF32() < PHYSICS_STABILIZATION_SECONDS)
            {
                return false;
            }

            if (!mRecreate && !mAvatar->findMotion(ANIM_AGENT_PHYSICS_MOTION))
            {
                if (mNotifyCompletion)
                {
                    LLNotificationsUtil::add("ASAvatarPhysicsRestartFailed");
                }
                return true;
            }

            const bool restarted = mAvatar->startMotion(ANIM_AGENT_PHYSICS_MOTION);
            if (mNotifyCompletion)
            {
                LLNotificationsUtil::add(restarted
                    ? "ASAvatarPhysicsRestartComplete"
                    : "ASAvatarPhysicsRestartFailed");
            }
            return true;
        }

    private:
        LLPointer<LLVOAvatarSelf> mAvatar;
        U32 mGeneration;
        bool mRecreate;
        bool mNotifyCompletion;
        LLFrameTimer mElapsed;
    };

    // Prevent physics from interpreting transient reset poses as acceleration.
    bool stabilizeAvatarPhysics(LLVOAvatarSelf* avatar, bool recreate,
                                bool notify_completion)
    {
        if (!avatar || avatar->isDead())
        {
            return false;
        }

        LLMotion* physics_motion = avatar->findMotion(ANIM_AGENT_PHYSICS_MOTION);
        if (!physics_motion && !recreate)
        {
            return false;
        }

        if (physics_motion)
        {
            avatar->stopMotion(ANIM_AGENT_PHYSICS_MOTION, true);
            if (recreate)
            {
                avatar->removeMotion(ANIM_AGENT_PHYSICS_MOTION);
            }
        }

        new AvatarPhysicsStabilizer(avatar, ++sPhysicsStabilizationGeneration,
                                    recreate, notify_completion);
        return true;
    }

    // AOEngine::enable() changes only live engine state. Preserving that state
    // directly avoids unexpectedly enabling an AO that is configured but not
    // currently active.
    class ScopedAOPause
    {
    public:
        ScopedAOPause()
        : mRestore(AOEngine::instanceExists() && AOEngine::instance().isEnabled())
        {
            if (mRestore)
            {
                AOEngine::instance().enable(false);
            }
        }

        ~ScopedAOPause()
        {
            if (mRestore && AOEngine::instanceExists() && isAgentAvatarValid())
            {
                // While disabled this updates the AO's remembered state
                // without starting anything, preventing a stale sit/fly
                // override from being resurrected when the AO is restored.
                AOEngine::instance().override(ANIM_AGENT_STAND, true);
                AOEngine::instance().enable(true);
            }
        }

    private:
        bool mRestore;
    };

    class ScopedResetFlag
    {
    public:
        ScopedResetFlag() { sResetInProgress = true; }
        ~ScopedResetFlag() { sResetInProgress = false; }
    };

    void performStrongReset()
    {
        if (sResetInProgress || !ASAvatarRecovery::isStrongResetAvailable())
        {
            LLNotificationsUtil::add("ASStrongAvatarResetUnavailable");
            return;
        }

        ScopedResetFlag reset_guard;
        ScopedAOPause ao_pause;

        // Keep a strong reference throughout this synchronous operation and
        // never continue through the global pointer after viewer state changes.
        LLPointer<LLVOAvatarSelf> avatar = gAgentAvatarp;
        if (avatar.isNull() || avatar->isDead())
        {
            LLNotificationsUtil::add("ASStrongAvatarResetUnavailable");
            return;
        }

        stabilizeAvatarPhysics(avatar, false, false);

        // Stop locally and on the simulator before rebuilding, while retaining
        // script permissions so healthy attachment animations can resume.
        gAgent.stopCurrentAnimations(true);

        // Use the viewer's normal stand path to preserve its safety checks and
        // remove any active sit animation from the recovery baseline.
        if (avatar->isSitting())
        {
            gAgent.standUp();
        }

        if (avatar->isDead())
        {
            LLNotificationsUtil::add("ASStrongAvatarResetUnavailable");
            return;
        }

        // stopCurrentAnimations() stops motions but intentionally leaves the
        // simulator-owned animation maps intact until a later network update.
        // resetSkeleton() rebuilds motions from those maps, which can therefore
        // resurrect a just-stopped translation animation during this same
        // command. Forget the stale local signal state and reconcile it now;
        // the reliable stop/reset requests above remain authoritative and a
        // subsequent simulator update will supply the clean baseline state.
        avatar->mSignaledAnimations.clear();
        avatar->processAnimationStateChanges();

        if (avatar->isDead())
        {
            LLNotificationsUtil::add("ASStrongAvatarResetUnavailable");
            return;
        }

        // Rebuild only after animation sources have been stopped. Do not call
        // resetAnimations(): flushAllMotions() deliberately restarts active
        // motion instances and can resurrect motions that are easing out.
        avatar->resetSkeleton(false);

        LLNotificationsUtil::add("ASStrongAvatarResetComplete");
    }

    bool confirmStrongReset(const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
        {
            performStrongReset();
        }
        return false;
    }
}

bool ASAvatarRecovery::isStrongResetAvailable()
{
    return !sResetInProgress && isAgentAvatarValid() && !gAgentAvatarp->isDead() &&
           gAgentAvatarp->isFullyLoaded() && gAgent.getRegion();
}

void ASAvatarRecovery::requestStrongReset()
{
    if (!isStrongResetAvailable())
    {
        LLNotificationsUtil::add("ASStrongAvatarResetUnavailable");
        return;
    }

    LLNotificationsUtil::add("ASStrongAvatarResetConfirm", LLSD(), LLSD(),
                             &confirmStrongReset);
}

void ASAvatarRecovery::restartAvatarPhysics()
{
    if (!isStrongResetAvailable())
    {
        LLNotificationsUtil::add("ASAvatarPhysicsRestartUnavailable");
        return;
    }

    LLPointer<LLVOAvatarSelf> avatar = gAgentAvatarp;
    if (avatar.isNull() || avatar->isDead())
    {
        LLNotificationsUtil::add("ASAvatarPhysicsRestartUnavailable");
        return;
    }

    // Recreate the controller after a settling interval. Its activation path
    // initializes from the settled pose before its first physics calculation.
    if (!stabilizeAvatarPhysics(avatar, true, true))
    {
        LLNotificationsUtil::add("ASAvatarPhysicsRestartFailed");
        return;
    }
}
