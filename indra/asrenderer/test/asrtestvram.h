/**
 * Standalone GPU residency-pressure experiment. Author: chanayane@firestorm
 */
#pragma once

#include "asrdevice.h"

namespace asrtest
{
enum class VramTestMode { Overcommit, ManagedTextures, AvatarSweep };
int runVramStress(const asr::DeviceOptions& options, double holdSeconds, uint32_t width, uint32_t height, VramTestMode mode);
}
