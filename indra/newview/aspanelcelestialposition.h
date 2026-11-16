/**
 * @file aspanelcelestialposition.h
 * @brief Compact sun and moon position controls for AyaneStorm settings panels.
 * @author chanayane@ayanestorm
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

#ifndef AS_PANELCELESTIALPOSITION_H
#define AS_PANELCELESTIALPOSITION_H

#include "llenvironment.h"
#include "llpanel.h"
#include "llsettingssky.h"

class LLUICtrl;

class ASPanelCelestialPosition : public LLPanel
{
public:
    ASPanelCelestialPosition() = default;
    ~ASPanelCelestialPosition() override;

    bool postBuild() override;
    void onVisibilityChange(bool new_visibility) override;

private:
    enum class Body { SUN, MOON };

    void refreshPosition();
    void onPositionChanged();
    LLSettingsSky::ptr_t getEditableSky();

    Body mBody = Body::SUN;
    LLUICtrl* mAzimuth = nullptr;
    LLUICtrl* mElevation = nullptr;
    LLSettingsSky::ptr_t mLiveSky;
    LLEnvironment::connection_t mEnvironmentConnection;
    bool mUpdating = false;
};

#endif // AS_PANELCELESTIALPOSITION_H
