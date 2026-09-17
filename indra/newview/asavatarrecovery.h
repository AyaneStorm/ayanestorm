/**
 * @file asavatarrecovery.h
 * @author chanayane@firestorm
 * @brief Defensive self-avatar animation and skeleton recovery.
 */

#ifndef AS_AVATAR_RECOVERY_H
#define AS_AVATAR_RECOVERY_H

namespace ASAvatarRecovery
{
    // Shows the destructive-action confirmation before starting recovery.
    void requestStrongReset();

    // Used by menu and toolbar enable callbacks.
    bool isStrongResetAvailable();

    // Recreates only the avatar physics motion controller. This is intentionally
    // separate because restarting physics can produce a visible body impulse.
    void restartAvatarPhysics();
}

#endif // AS_AVATAR_RECOVERY_H
