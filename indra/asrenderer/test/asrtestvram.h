/**
 * @file asrtestvram.h
 * Standalone GPU residency-pressure experiment. Author: chanayane@firestorm
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
#pragma once

#include "asrdevice.h"

namespace asrtest
{
enum class VramTestMode { Overcommit, ManagedTextures, AvatarSweep };
int runVramStress(const asr::DeviceOptions& options, double holdSeconds, uint32_t width, uint32_t height, VramTestMode mode, bool audit);
// Texture compression: quality of each format against its source, and encode speed.
int runEncodeTest(const asr::DeviceOptions& options);
}
