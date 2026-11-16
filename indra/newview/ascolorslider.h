/**
 * @file ascolorslider.h
 * @author chanayane@ayanestorm
 * @brief Slider control with a configurable multi-stop color track.
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
#ifndef AS_COLOR_SLIDER_H
#define AS_COLOR_SLIDER_H

#include "llsliderctrl.h"

class ASColorSliderCtrl : public LLSliderCtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLSliderCtrl::Params>
    {
        Optional<LLColor4> color_1, color_2, color_3, color_4,
                           color_5, color_6, color_7, color_8,
                           color_9, color_10, color_11, color_12,
                           color_13, color_14, color_15, color_16;
        Optional<S32> track_thickness;
        Params();
    };

    void draw() override;
    // Two to sixteen evenly-spaced colors are accepted.
    void setTrackColors(const std::vector<LLColor4>& colors);

protected:
    ASColorSliderCtrl(const Params& params);
    friend class LLUICtrlFactory;

private:
    std::vector<LLColor4> mColors;
    S32 mTrackLeft;
    S32 mTrackRightInset;
    S32 mTrackThickness;
};

#endif
