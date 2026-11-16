/**
 * @file asrlatestvalue.h
 * asrlatestvalue.h
 * Hands the newest value from one writer thread to one reader thread, with no
 * lock and no waiting on either side. A stalled writer leaves the reader on the
 * last published value, and a slow reader simply skips values. This is how state
 * snapshots reach the render thread (section 0 of the renderer plan).
 *
 * There are three slots: one the writer owns, one the reader owns, and one held
 * between them. Publishing and reading each swap their slot with the middle one.
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

#include <atomic>
#include <cstdint>

namespace asr
{

template <class T>
class LatestValue
{
public:
    // Writer only. The slot holds an older value, so fill in all of it.
    T& writeSlot() { return mSlots[mWriteSlot].value; }

    // Writer only. Makes the write slot the newest value.
    void publish() { mWriteSlot = mMiddle.exchange(mWriteSlot | IS_NEW, std::memory_order_acq_rel) & SLOT_MASK; }

    // Reader only. Returns the newest value, valid until the next read(). Before
    // the first publish() it is a default-constructed T.
    const T& read()
    {
        if (mMiddle.load(std::memory_order_acquire) & IS_NEW)
        {
            mReadSlot = mMiddle.exchange(mReadSlot, std::memory_order_acq_rel) & SLOT_MASK;
        }
        return mSlots[mReadSlot].value;
    }

private:
    static constexpr uint32_t SLOT_MASK = 3;
    static constexpr uint32_t IS_NEW = 4;

    // On separate cache lines, so the two threads never slow each other down.
    struct alignas(64) Slot
    {
        T value{};
    };

    Slot mSlots[3];
    alignas(64) std::atomic<uint32_t> mMiddle{ 2 };
    alignas(64) uint32_t mWriteSlot = 0;
    alignas(64) uint32_t mReadSlot = 1;
};

} // namespace asr
