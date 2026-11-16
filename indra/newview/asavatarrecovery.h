/**
 * @file asavatarrecovery.h
 * @author chanayane@ayanestorm
 * @brief Defensive self-avatar animation and skeleton recovery.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (C) 2026, Ayane Lyla
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * $/LicenseInfo$
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
