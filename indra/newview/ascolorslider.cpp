/**
 * @file ascolorslider.cpp
 * @author chanayane@firestorm
 * @brief Slider control with a configurable multi-stop color track.
 */
#include "llviewerprecompiledheaders.h"

#include "ascolorslider.h"

#include "llgl.h"
#include "llrender.h"
#include "lluictrlfactory.h"

#include <algorithm>

static LLDefaultChildRegistry::Register<ASColorSliderCtrl> sASColorSlider("as_color_slider");

ASColorSliderCtrl::Params::Params()
    : color_1("color_1"), color_2("color_2"), color_3("color_3"), color_4("color_4"),
      color_5("color_5"), color_6("color_6"), color_7("color_7"), color_8("color_8"),
      color_9("color_9"), color_10("color_10"), color_11("color_11"), color_12("color_12"),
      color_13("color_13"), color_14("color_14"), color_15("color_15"), color_16("color_16"),
      track_thickness("track_thickness", 6)
{
}

ASColorSliderCtrl::ASColorSliderCtrl(const Params& params)
    : LLSliderCtrl(params),
      mTrackLeft(params.label_width()),
      mTrackRightInset(params.show_text() ? params.text_width() : 0),
      mTrackThickness(llclamp(params.track_thickness(), 2, 16))
{
    const auto add_stop = [this](const auto& stop)
    {
        if (stop.isProvided()) mColors.push_back(stop());
    };
    add_stop(params.color_1);  add_stop(params.color_2);
    add_stop(params.color_3);  add_stop(params.color_4);
    add_stop(params.color_5);  add_stop(params.color_6);
    add_stop(params.color_7);  add_stop(params.color_8);
    add_stop(params.color_9);  add_stop(params.color_10);
    add_stop(params.color_11); add_stop(params.color_12);
    add_stop(params.color_13); add_stop(params.color_14);
    add_stop(params.color_15); add_stop(params.color_16);
    if (mColors.size() < 2)
    {
        mColors = { LLColor4::black, LLColor4::white };
    }
}

void ASColorSliderCtrl::setTrackColors(const std::vector<LLColor4>& colors)
{
    if (colors.size() >= 2)
    {
        mColors.assign(colors.begin(), colors.begin() + std::min(colors.size(), (size_t)16));
    }
}

void ASColorSliderCtrl::draw()
{
    const S32 left = mTrackLeft + 8;
    const S32 right = getRect().getWidth() - mTrackRightInset - 8;
    const S32 bottom = (getRect().getHeight() - mTrackThickness) / 2;
    const S32 top = bottom + mTrackThickness;
    const F32 alpha = isInEnabledChain() ? getCurrentTransparency() : getCurrentTransparency() * .45f;

    if (right > left)
    {
        LLGLSUIDefault gls_ui;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        const S32 segments = (S32)mColors.size() - 1;
        for (S32 i = 0; i < segments; ++i)
        {
            const S32 x0 = left + (right - left) * i / segments;
            const S32 x1 = left + (right - left) * (i + 1) / segments;
            gGL.color4fv((mColors[i] % alpha).mV);     gGL.vertex2i(x0, bottom);
            gGL.color4fv((mColors[i] % alpha).mV);     gGL.vertex2i(x0, top);
            gGL.color4fv((mColors[i + 1] % alpha).mV); gGL.vertex2i(x1, top);
            gGL.color4fv((mColors[i] % alpha).mV);     gGL.vertex2i(x0, bottom);
            gGL.color4fv((mColors[i + 1] % alpha).mV); gGL.vertex2i(x1, top);
            gGL.color4fv((mColors[i + 1] % alpha).mV); gGL.vertex2i(x1, bottom);
        }
        gGL.end();
    }
    LLSliderCtrl::draw();
}
