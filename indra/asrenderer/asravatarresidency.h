/**
 * Off-screen avatar skinning buffers yield space before the working budget fills.
 * Author: chanayane@firestorm
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace asr
{

class AvatarResidencyPolicy
{
public:
    void update(uint64_t used, uint64_t budget, double now)
    {
        // Textures steer to 85% of the budget (TEXTURE_STEERING_PERCENT), so that is the
        // normal level: pressure starts above it, and ends once back near it. Leave time for
        // queued work and retired allocations before prewarming again.
        const double fraction = budget ? double(used) / double(budget) : (used ? 1.0 : 0.0);
        if (fraction >= 0.95)
        {
            mPressured = true;
        }
        if (!mPressured || fraction >= 0.90)
        {
            mBelowSince = -1.0;
        }
        else if (mBelowSince < 0.0)
        {
            mBelowSince = now;
        }
        else if (now - mBelowSince >= 3.0)
        {
            mPressured = false;
        }
    }

    bool underPressure() const { return mPressured; }

    bool nearView(float forwardDot, float radiusOverDistance, float halfCorner) const
    {
        constexpr float DEG_TO_RAD = 0.01745329252f;
        const float margin = (mPressured ? 12.f : 45.f) * DEG_TO_RAD;
        return std::acos(std::clamp(forwardDot, -1.f, 1.f)) - std::asin(radiusOverDistance) < halfCorner + margin;
    }

    bool keepResident(bool wanted, double now, double& lastWantedAt) const
    {
        if (wanted)
        {
            lastWantedAt = now;
            return true;
        }
        return now - lastWantedAt < (mPressured ? 0.6 : 3.0);
    }

private:
    bool mPressured = false;
    double mBelowSince = -1.0;
};

// Crowd detail ranking: the nearest avatars keep their full mesh LOD, the rest are capped
// coarser step by step with their rank. How many keep full detail follows memory: what is
// not textures (geometry, skinning, targets) may take only part of the working budget, so
// textures keep room for near surfaces. A large card keeps everyone at full detail.
class AvatarDetailPolicy
{
public:
    static constexpr int FINEST = 3;          // the viewer's LODs: 0 coarsest to 3 finest
    static constexpr int COARSEST_CAP = 1;    // creators often leave LOD 0 empty or broken
    static constexpr size_t MIN_FULL = 8;     // never fewer at full detail, beyond those always full
    // A low LOD can break an avatar, so near ones are always full (NEAR_METRES) and the steps
    // are gentle (lodScale). Past them, far avatars give way early: at 0.55 all 82 of a club
    // stayed full, skinning and drawing took 30 ms of GPU time, and the frame rate fell to 20.
    static constexpr double HIGH_SHARE = 0.45;
    static constexpr double LOW_SHARE = 0.35;
    static constexpr float NEAR_METRES = 10.f; // always full mesh detail within this distance
    static constexpr double STEP_SECONDS = 4.0; // lets freed LODs retire before the next step

    // overcommitted: the total is past the budget while textures are already at their floor
    // (only the nearest sharp). Then mesh gives way whatever its share: nothing else can, and
    // past the budget the driver spills into system memory.
    void update(uint64_t others, uint64_t used, uint64_t budget, size_t avatars, double now, bool overcommitted)
    {
        mFull = std::min(mFull, std::max(avatars, MIN_FULL));
        if (now - mChangedAt < STEP_SECONDS) return;
        const double share = budget ? double(others) / double(budget) : 1.0;
        if ((share > HIGH_SHARE || overcommitted) && mFull > MIN_FULL)
        {
            mFull = std::max(MIN_FULL, mFull - std::max<size_t>(1, mFull / 4));
            mChangedAt = now;
        }
        else if (share < LOW_SHARE && !overcommitted && used < budget / 10 * 9 && mFull < avatars)
        {
            mFull += std::max<size_t>(1, mFull / 8);
            mChangedAt = now;
        }
    }

    size_t fullDetail() const { return mFull; }

    // A rank's level as a factor on the viewer's LOD factor, not a hard cap: a hard LOD 1
    // showed creators' broken low LODs as shards at a few metres. Gentle steps below FINEST,
    // so coarse LODs still come only with distance.
    static float lodScale(int level)
    {
        return level >= FINEST ? 1.f : level == FINEST - 1 ? 0.75f : 0.5f;
    }

    // The finest LOD an avatar at this rank may use; rank counts only avatars not always full.
    int lodCap(size_t rank) const
    {
        if (rank < mFull) return FINEST;
        const size_t band = std::max<size_t>(mFull / 2, 4);
        const size_t steps = (rank - mFull) / band + 1;
        return std::max(COARSEST_CAP, FINEST - int(std::min<size_t>(steps, FINEST)));
    }

private:
    size_t mFull = SIZE_MAX;
    double mChangedAt = -1e9;
};

} // namespace asr
