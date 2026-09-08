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

#include "llcombobox.h"
#include "llview.h"

static LLPanelInjector<ASPanelPrefsAyaneStorm> t_pref_ayanestorm("panel_preference_ayanestorm");

ASPanelPrefsAyaneStorm::ASPanelPrefsAyaneStorm() : LLPanelPreference()
{
}

bool ASPanelPrefsAyaneStorm::postBuild()
{
#if LL_DARWIN
    // macOS can use Standard and AYAstorm ordering, but not the GL 4.3 OIT
    // renderers. Keep the selector visible and disable only unsupported modes.
    if (LLComboBox* mode = findChild<LLComboBox>("render_oit_mode"))
    {
        mode->setEnabledByValue(LLSD(1), false);
        mode->setEnabledByValue(LLSD(2), false);
    }

    const char* oit_controls[] =
    {
        "render_exact_oit_debug",
        "render_exact_oit_debug_label",
        "render_avboit_debug",
        "render_avboit_debug_label"
    };

    for (const char* control_name : oit_controls)
    {
        if (LLView* control = findChild<LLView>(control_name))
        {
            control->setVisible(false);
        }
    }
#endif // LL_DARWIN

    return LLPanelPreference::postBuild();
}
