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

#include "lltabcontainer.h"

static LLPanelInjector<ASPanelPrefsAyaneStorm> t_pref_ayanestorm("panel_preference_ayanestorm");

ASPanelPrefsAyaneStorm::ASPanelPrefsAyaneStorm() : LLPanelPreference()
{
}

bool ASPanelPrefsAyaneStorm::postBuild()
{
#if LL_DARWIN
    // The rendering sub-tab exposes GL-specific WBOIT/OIT settings that
    // aren't applicable on macOS - hide it there.
    LLTabContainer* tab_container = getChild<LLTabContainer>("ayanestorm_tabs");
    if (tab_container)
    {
        LLPanel* rendering_panel = tab_container->getPanelByName("tab-as-rendering");
        if (rendering_panel)
            tab_container->removeTabPanel(rendering_panel);
    }
#endif // LL_DARWIN

    return LLPanelPreference::postBuild();
}
