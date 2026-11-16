/**
 * @file asgraphicslevelapply.h
 * @brief AyaneStorm: apply a graphics quality change once the slider settles.
 * @author chanayane@ayanestorm
 *
 * A quality slider commits on every step while dragged, and each commit runs
 * LLFeatureManager::setGraphicsLevel(), a full shader reload. Dragging across
 * five levels reloaded five times, each a fresh shader cache miss, long enough
 * to lose the region. The quality handlers route through request(), which
 * runs only the latest change once the mouse is released and the value has
 * been stable briefly.
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

#ifndef AS_GRAPHICS_LEVEL_APPLY_H
#define AS_GRAPHICS_LEVEL_APPLY_H

#include <functional>

namespace ASGraphicsLevelApply
{
    // Replaces any pending change with apply.
    void request(std::function<void()> apply);
    // True while a deferred change runs, so the handler it calls applies
    // instead of deferring again.
    bool applying();
}

#endif
