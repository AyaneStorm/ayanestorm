/**
 * @file aspanelprefsayanestorm.cpp
 * @brief Class for the AyaneStorm preferences panel
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (c) 2026 Chanayane @ Second Life
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "aspanelprefsayanestorm.h"

#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lltabcontainer.h"
#include "lltextbox.h"

static LLPanelInjector<ASPanelPrefsAyaneStorm> t_pref_ayanestorm("panel_preference_ayanestorm");

ASPanelPrefsAyaneStorm::ASPanelPrefsAyaneStorm() : LLPanelPreference()
{
}

bool ASPanelPrefsAyaneStorm::postBuild()
{
#if LL_DARWIN
    // Only RenderOITMode (Exact OIT / AVBOIT) requires GL 4.3, which macOS's
    // capped OpenGL 4.1 does not provide - hide just that control. The
    // volumetric lighting checkbox in the same tab works down to GL 4.0, so
    // the tab itself stays visible on Mac (see ASVolumetricLighting::isSupported()).
    LLComboBox* oit_mode_combo = findChild<LLComboBox>("render_oit_mode");
    if (oit_mode_combo)
        oit_mode_combo->setVisible(false);

    LLTextBox* oit_mode_label = findChild<LLTextBox>("render_oit_mode_label");
    if (oit_mode_label)
        oit_mode_label->setVisible(false);
#endif // LL_DARWIN

    return LLPanelPreference::postBuild();
}
