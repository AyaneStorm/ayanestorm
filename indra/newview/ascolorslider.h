/**
 * @file ascolorslider.h
 * @author chanayane@firestorm
 * @brief Slider control with a configurable multi-stop color track.
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
