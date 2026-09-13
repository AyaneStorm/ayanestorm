/**
 * @file asfloatercolorgrading.cpp
 * @author chanayane@firestorm
 * @brief Color-grading floater and reusable settings panel.
 */
#include "llviewerprecompiledheaders.h"

#include "asfloatercolorgrading.h"
#include "ascolorslider.h"
#include "ascolorlut.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "llnotificationsutil.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

static LLPanelInjector<ASPanelColorGrading> sColorGradingPanel("as_color_grading_panel");

namespace
{
    const char* const BAND_LABELS[ASColorGrading::BAND_COUNT] =
        { "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta",
          "Gray 1", "Gray 2", "Gray 3", "Gray 4", "Gray 5", "Gray 6", "Gray 7", "Gray 8",
          "Red skin 2", "Red skin 4", "Red skin 6", "Red skin 8",
          "Skin 2", "Skin 4", "Skin 6", "Skin 8" };
    const char* const BAND_BUTTONS[ASColorGrading::BAND_COUNT] =
        { "band_red", "band_orange", "band_yellow", "band_green", "band_aqua", "band_blue", "band_purple", "band_magenta",
          "band_gray_1", "band_gray_2", "band_gray_3", "band_gray_4", "band_gray_5", "band_gray_6", "band_gray_7", "band_gray_8",
          "band_red_skin_2", "band_red_skin_4", "band_red_skin_6", "band_red_skin_8",
          "band_skin_2", "band_skin_4", "band_skin_6", "band_skin_8" };
    const char* const BAND_DOTS[ASColorGrading::BAND_COUNT] =
        { "band_red_modified", "band_orange_modified", "band_yellow_modified", "band_green_modified",
          "band_aqua_modified", "band_blue_modified", "band_purple_modified", "band_magenta_modified",
          "band_gray_1_modified", "band_gray_2_modified", "band_gray_3_modified", "band_gray_4_modified",
          "band_gray_5_modified", "band_gray_6_modified", "band_gray_7_modified", "band_gray_8_modified",
          "band_red_skin_2_modified", "band_red_skin_4_modified", "band_red_skin_6_modified", "band_red_skin_8_modified",
          "band_skin_2_modified", "band_skin_4_modified", "band_skin_6_modified", "band_skin_8_modified" };
    const LLColor4 BAND_COLORS[ASColorGrading::BAND_COUNT] =
        { LLColor4(.92f,.16f,.18f,1.f), LLColor4(.95f,.48f,.12f,1.f),
          LLColor4(.92f,.76f,.08f,1.f), LLColor4(.20f,.72f,.28f,1.f),
          LLColor4(.08f,.72f,.76f,1.f), LLColor4(.10f,.42f,.92f,1.f),
          LLColor4(.55f,.24f,.86f,1.f), LLColor4(.88f,.12f,.68f,1.f),
          LLColor4(.949f,.949f,.949f,1.f), LLColor4(.812f,.812f,.812f,1.f),
          LLColor4(.678f,.678f,.678f,1.f), LLColor4(.549f,.549f,.549f,1.f),
          LLColor4(.424f,.424f,.424f,1.f), LLColor4(.306f,.306f,.306f,1.f),
          LLColor4(.196f,.196f,.196f,1.f), LLColor4(.094f,.094f,.094f,1.f),
          LLColor4(.949f,.557f,.533f,1.f), LLColor4(.969f,.659f,.604f,1.f),
          LLColor4(.984f,.757f,.663f,1.f), LLColor4(1.f,.855f,.725f,1.f),
          LLColor4(.941f,.808f,.671f,1.f), LLColor4(.773f,.576f,.408f,1.f),
          LLColor4(.553f,.376f,.212f,1.f), LLColor4(.345f,.192f,.004f,1.f) };

}

ASPanelColorGrading::ASPanelColorGrading()
    : mBand(ASColorGrading::RED), mRefreshing(false), mPreset(nullptr), mBefore(nullptr), mBandName(nullptr),
      mColorize(nullptr), mColorizeSwatch(nullptr), mSelectionColor(nullptr),
      mMixerHue(nullptr), mMixerSaturation(nullptr), mMixerLightness(nullptr),
      mMixerStrength(nullptr), mMixerHueRange(nullptr), mMixerChromaRange(nullptr),
      mMixerLightnessRange(nullptr), mMixerSoftness(nullptr),
      mSplitHighlightsSaturation(nullptr), mSplitShadowsSaturation(nullptr),
      mSplitHighlightsSwatch(nullptr), mSplitShadowsSwatch(nullptr)
{
}

ASPanelColorGrading::~ASPanelColorGrading()
{
    setBefore(false);
    for (boost::signals2::connection& connection : mSettingConnections) connection.disconnect();
}

bool ASPanelColorGrading::postBuild()
{
    mPreset = getChild<LLComboBox>("preset");
    mBandName = getChild<LLTextBox>("selected_band");
    mColorize = getChild<LLCheckBoxCtrl>("colorize");
    mColorizeSwatch = getChild<LLButton>("colorize_swatch");
    mSelectionColor = getChild<LLColorSwatchCtrl>("mixer_selection_color");
    mMixerHue = getChild<ASColorSliderCtrl>("mixer_hue");
    mMixerSaturation = getChild<ASColorSliderCtrl>("mixer_saturation");
    mMixerLightness = getChild<ASColorSliderCtrl>("mixer_lightness");
    mMixerStrength = getChild<ASColorSliderCtrl>("mixer_strength");
    mMixerHueRange = getChild<ASColorSliderCtrl>("mixer_hue_range");
    mMixerChromaRange = getChild<ASColorSliderCtrl>("mixer_chroma_range");
    mMixerLightnessRange = getChild<ASColorSliderCtrl>("mixer_lightness_range");
    mMixerSoftness = getChild<ASColorSliderCtrl>("mixer_softness");
    mSplitHighlightsSaturation = getChild<ASColorSliderCtrl>("ASColorGradeSplitHighlightsSaturation");
    mSplitShadowsSaturation = getChild<ASColorSliderCtrl>("ASColorGradeSplitShadowsSaturation");
    mSplitHighlightsSwatch = getChild<LLButton>("split_highlights_swatch");
    mSplitShadowsSwatch = getChild<LLButton>("split_shadows_swatch");

    mPreset->setCommitCallback(boost::bind(&ASPanelColorGrading::loadPreset, this));
    getChild<LLButton>("save_preset")->setCommitCallback(boost::bind(&ASPanelColorGrading::savePreset, this));
    getChild<LLButton>("delete_preset")->setCommitCallback(boost::bind(&ASPanelColorGrading::deletePreset, this));
    getChild<LLButton>("reset_all")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetAll, this));
    getChild<LLButton>("refresh_grain")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        ASColorGrading::refreshStaticGrain();
    });

    mBefore = getChild<LLButton>("before");
    mBefore->setMouseDownCallback(boost::bind(&ASPanelColorGrading::setBefore, this, true));
    mBefore->setMouseUpCallback(boost::bind(&ASPanelColorGrading::setBefore, this, false));

    for (S32 i = 0; i < ASColorGrading::BAND_COUNT; ++i)
    {
        LLButton* button = getChild<LLButton>(BAND_BUTTONS[i]);
        // Apply per-band colors in code so one named UI color cannot tint
        // every selector.
        button->setImageColor(LLUIColor(BAND_COLORS[i]));
        button->setCommitCallback(boost::bind(
            &ASPanelColorGrading::selectBand, this, (ASColorGrading::Band)i));
        if (LLControlVariable* control = gSavedSettings.getControl(
            ASColorGrading::selectionColorSettingName((ASColorGrading::Band)i)))
            mSettingConnections.push_back(control->getSignal()->connect(
                boost::bind(&ASPanelColorGrading::markCustom, this)));
    }
    mSelectionColor->setCommitCallback(boost::bind(&ASPanelColorGrading::commitSelectionColor, this));
    mSelectionColor->setOnSelectCallback(boost::bind(&ASPanelColorGrading::commitSelectionColor, this));
    mSelectionColor->setOnCancelCallback(boost::bind(&ASPanelColorGrading::commitSelectionColor, this));
    getChild<LLButton>("reset_mixer_band")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetBand, this));
    mMixerHue->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "Hue", mMixerHue));
    mMixerSaturation->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "Saturation", mMixerSaturation));
    mMixerLightness->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "Lightness", mMixerLightness));
    mMixerStrength->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "Strength", mMixerStrength));
    mMixerHueRange->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "HueRange", mMixerHueRange));
    mMixerChromaRange->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "ChromaRange", mMixerChromaRange));
    mMixerLightnessRange->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "LightnessRange", mMixerLightnessRange));
    mMixerSoftness->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "Softness", mMixerSoftness));
    getChild<LLButton>("reset_mixer_hue")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "Hue"));
    getChild<LLButton>("reset_mixer_saturation")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "Saturation"));
    getChild<LLButton>("reset_mixer_lightness")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "Lightness"));
    getChild<LLButton>("reset_mixer_strength")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "Strength"));
    getChild<LLButton>("reset_mixer_hue_range")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "HueRange"));
    getChild<LLButton>("reset_mixer_chroma_range")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "ChromaRange"));
    getChild<LLButton>("reset_mixer_lightness_range")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "LightnessRange"));
    getChild<LLButton>("reset_mixer_softness")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "Softness"));
    mColorize->setCommitCallback(boost::bind(&ASPanelColorGrading::toggleColorize, this));

    for (const std::string& setting : ASColorGrading::settingNames())
    {
        if (LLControlVariable* control = gSavedSettings.getControl(setting))
            mSettingConnections.push_back(control->getSignal()->connect(boost::bind(&ASPanelColorGrading::markCustom, this)));
        const std::string child = "reset_" + setting;
        if (hasChild(child, true))
        {
            getChild<LLButton>(child)->setCommitCallback([setting](LLUICtrl*, const LLSD&)
            {
                if (LLControlVariable* control = gSavedSettings.getControl(setting)) control->resetToDefault(true);
            });
        }
    }
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeColorizeEnabled"))
        mSettingConnections.push_back(control->getSignal()->connect(boost::bind(&ASPanelColorGrading::markCustom, this)));
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeSplitToningEnabled"))
        mSettingConnections.push_back(control->getSignal()->connect(boost::bind(&ASPanelColorGrading::markCustom, this)));
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeNegativeEnabled"))
        mSettingConnections.push_back(control->getSignal()->connect(boost::bind(&ASPanelColorGrading::markCustom, this)));
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeGrainStatic"))
        mSettingConnections.push_back(control->getSignal()->connect(boost::bind(&ASPanelColorGrading::markCustom, this)));
    for (const char* setting : {"ASColorGradeLUTEnabled", "ASColorGradeLUTFile"})
        if (LLControlVariable* control = gSavedSettings.getControl(setting))
            mSettingConnections.push_back(control->getSignal()->connect(boost::bind(&ASPanelColorGrading::markCustom, this)));
    ASColorLUT::initPanel(*this);
    refreshPresets();
    selectBand(ASColorGrading::RED);
    return true;
}

void ASPanelColorGrading::draw()
{
    ASColorLUT::updatePanel(*this);
    refreshSplitToning();
    const bool grain_enabled = gSavedSettings.getF32("ASColorGradeGrainAmount") > 0.f;
    getChild<LLUICtrl>("ASColorGradeGrainSize")->setEnabled(grain_enabled);
    getChild<LLUICtrl>("ASColorGradeGrainRoughness")->setEnabled(grain_enabled);
    getChild<LLUICtrl>("ASColorGradeGrainColor")->setEnabled(grain_enabled);
    getChild<LLUICtrl>("grain_static")->setEnabled(grain_enabled);
    getChild<LLButton>("refresh_grain")->setEnabled(
        grain_enabled && gSavedSettings.getBOOL("ASColorGradeGrainStatic"));
    getChild<LLButton>("reset_ASColorGradeGrainSize")->setEnabled(grain_enabled);
    getChild<LLButton>("reset_ASColorGradeGrainRoughness")->setEnabled(grain_enabled);
    getChild<LLButton>("reset_ASColorGradeGrainColor")->setEnabled(grain_enabled);
    getChild<LLButton>("delete_preset")->setEnabled(
        !ASColorGrading::isReadOnlyPreset(presetName()) && presetName() != "Custom" && !presetName().empty());
    LLPanel::draw();
}

void ASPanelColorGrading::refreshSplitToning()
{
    LLColor3 highlights, shadows;
    highlights.setHSL(gSavedSettings.getF32("ASColorGradeSplitHighlightsHue") / 360.f, .85f, .5f);
    shadows.setHSL(gSavedSettings.getF32("ASColorGradeSplitShadowsHue") / 360.f, .85f, .5f);
    const LLColor4 highlight_color(highlights, 1.f);
    const LLColor4 shadow_color(shadows, 1.f);
    mSplitHighlightsSwatch->setImageColor(LLUIColor(highlight_color));
    mSplitShadowsSwatch->setImageColor(LLUIColor(shadow_color));
    mSplitHighlightsSaturation->setTrackColors({ LLColor4(.5f,.5f,.5f,1.f), highlight_color });
    mSplitShadowsSaturation->setTrackColors({ LLColor4(.5f,.5f,.5f,1.f), shadow_color });
}

void ASPanelColorGrading::onOpen(const LLSD&)
{
    refreshPresets(mPreset ? presetName() : "Custom");
    ASColorLUT::refreshPanel(*this);
}

void ASPanelColorGrading::onVisibilityChange(bool visible)
{
    LLPanel::onVisibilityChange(visible);
    if (!visible) setBefore(false);
}

void ASPanelColorGrading::selectBand(ASColorGrading::Band band)
{
    if (mSelectionColor && band != mBand) mSelectionColor->closeFloaterColorPicker();
    mBand = band;
    for (S32 i = 0; i < ASColorGrading::BAND_COUNT; ++i)
        getChild<LLButton>(BAND_BUTTONS[i])->setToggleState(i == band);
    mBandName->setText(LLStringExplicit(BAND_LABELS[band]));
    refreshMixer();
}

void ASPanelColorGrading::refreshMixer()
{
    mRefreshing = true;
    const bool colorize = gSavedSettings.getBOOL("ASColorGradeColorizeEnabled");
    mColorize->setValue(colorize);
    for (S32 i = 0; i < ASColorGrading::BAND_COUNT; ++i) getChild<LLButton>(BAND_BUTTONS[i])->setVisible(!colorize);
    mColorizeSwatch->setVisible(colorize);
    mSelectionColor->setVisible(!colorize);
    getChild<LLUICtrl>("mixer_selection_color_label")->setVisible(!colorize);
    getChild<LLButton>("reset_mixer_band")->setVisible(!colorize);
    mBandName->setText(LLStringExplicit(colorize ? "Colorize" : BAND_LABELS[mBand]));
    mMixerHue->setMinValue(colorize ? 0.f : -180.f); mMixerHue->setMaxValue(colorize ? 360.f : 180.f);
    mMixerHue->setToolTip(LLStringExplicit(colorize ?
        "Selects the Colorize hue from 0 to 360 degrees." :
        "Shifts the selected color through the full hue circle, up to 180 degrees in either direction."));
    mMixerSaturation->setMinValue(colorize ? 0.f : -100.f); mMixerSaturation->setMaxValue(100.f);
    const std::string prefix = colorize ? "ASColorGradeColorize" : "";
    mMixerHue->setValue(gSavedSettings.getF32(colorize ? prefix + "Hue" : ASColorGrading::bandSettingName(mBand, "Hue")));
    mMixerSaturation->setValue(gSavedSettings.getF32(colorize ? prefix + "Saturation" : ASColorGrading::bandSettingName(mBand, "Saturation")));
    mMixerLightness->setValue(gSavedSettings.getF32(colorize ? prefix + "Lightness" : ASColorGrading::bandSettingName(mBand, "Lightness")));
    mMixerStrength->setVisible(!colorize); getChild<LLButton>("reset_mixer_strength")->setVisible(!colorize);
    mMixerHueRange->setVisible(!colorize); getChild<LLButton>("reset_mixer_hue_range")->setVisible(!colorize);
    mMixerChromaRange->setVisible(!colorize); getChild<LLButton>("reset_mixer_chroma_range")->setVisible(!colorize);
    mMixerLightnessRange->setVisible(!colorize); getChild<LLButton>("reset_mixer_lightness_range")->setVisible(!colorize);
    mMixerSoftness->setVisible(!colorize); getChild<LLButton>("reset_mixer_softness")->setVisible(!colorize);
    if (!colorize)
    {
        mMixerStrength->setValue(gSavedSettings.getF32(ASColorGrading::bandSettingName(mBand, "Strength")));
        mMixerHueRange->setValue(gSavedSettings.getF32(ASColorGrading::bandSettingName(mBand, "HueRange")));
        mMixerChromaRange->setValue(gSavedSettings.getF32(ASColorGrading::bandSettingName(mBand, "ChromaRange")));
        mMixerLightnessRange->setValue(gSavedSettings.getF32(ASColorGrading::bandSettingName(mBand, "LightnessRange")));
        mMixerSoftness->setValue(gSavedSettings.getF32(ASColorGrading::bandSettingName(mBand, "Softness")));
    }
    const LLColor4 base_band = gSavedSettings.getColor4(ASColorGrading::selectionColorSettingName(mBand));
    mSelectionColor->set(base_band, true);
    LLColor3 base(base_band);
    F32 center_hue, center_saturation, center_lightness;
    base.calcHSL(&center_hue, &center_saturation, &center_lightness);
    const F32 hue_shift = colorize ? 0.f :
        gSavedSettings.getF32(ASColorGrading::bandSettingName(mBand, "Hue")) / 360.f;
    LLColor3 hue_adjusted;
    hue_adjusted.setHSL(fmodf(center_hue + hue_shift + 1.f, 1.f),
                       center_saturation, center_lightness);
    const LLColor4 hue_adjusted_band(hue_adjusted, 1.f);
    if (colorize)
    {
        std::vector<LLColor4> wheel;
        for (S32 i = 0; i <= 12; ++i) { LLColor3 c; c.setHSL((F32)i / 12.f, .85f, .5f); wheel.emplace_back(c, 1.f); }
        const F32 selected_hue = gSavedSettings.getF32("ASColorGradeColorizeHue");
        LLColor3 selected; selected.setHSL(selected_hue / 360.f, .85f, .5f);
        const LLColor4 selected4(selected, 1.f);
        mMixerHue->setTrackColors(wheel);
        mMixerSaturation->setTrackColors({ LLColor4(.5f,.5f,.5f,1.f), selected4 });
        mMixerLightness->setTrackColors({ LLColor4::black, selected4, LLColor4::white });
        mColorizeSwatch->setImageColor(LLUIColor(selected4));
    }
    else
    {
        std::vector<LLColor4> wheel;
        for (S32 i = 0; i <= 12; ++i)
        {
            LLColor3 color;
            color.setHSL(fmodf(center_hue + (F32)i / 12.f + .5f, 1.f),
                         center_saturation, center_lightness);
            wheel.emplace_back(color, 1.f);
        }
        mMixerHue->setTrackColors(wheel);
        mMixerSaturation->setTrackColors({ LLColor4(.42f,.42f,.42f,1.f), hue_adjusted_band,
                                           LLColor4(hue_adjusted * 1.25f, 1.f) });
        mMixerLightness->setTrackColors({ LLColor4::black, hue_adjusted_band, LLColor4::white });
        mMixerStrength->setTrackColors({ LLColor4(.3f,.3f,.3f,1.f), hue_adjusted_band });
        mMixerHueRange->setTrackColors({ hue_adjusted_band, LLColor4(.7f,.7f,.7f,1.f) });
        mMixerChromaRange->setTrackColors({ hue_adjusted_band, LLColor4(.7f,.7f,.7f,1.f) });
        mMixerLightnessRange->setTrackColors({ hue_adjusted_band, LLColor4::white });
        mMixerSoftness->setTrackColors({ hue_adjusted_band, LLColor4(.7f,.7f,.7f,1.f) });
    }
    refreshBandIndicators();
    mRefreshing = false;
}

void ASPanelColorGrading::commitMixer(const std::string& component, LLSliderCtrl* control)
{
    if (!mRefreshing)
    {
        const std::string setting = gSavedSettings.getBOOL("ASColorGradeColorizeEnabled") ?
            "ASColorGradeColorize" + component : ASColorGrading::bandSettingName(mBand, component);
        gSavedSettings.setF32(setting, control->getValueF32());
        mPreset->setValue("Custom");
        refreshMixer();
    }
}

void ASPanelColorGrading::commitSelectionColor()
{
    if (mRefreshing) return;
    LLColor4 color = mSelectionColor->get();
    color.mV[VALPHA] = 1.f;
    gSavedSettings.setColor4(ASColorGrading::selectionColorSettingName(mBand), color);
    mPreset->setValue("Custom");
    refreshMixer();
}

void ASPanelColorGrading::resetMixer(const std::string& component)
{
    const std::string setting = gSavedSettings.getBOOL("ASColorGradeColorizeEnabled") ?
        "ASColorGradeColorize" + component : ASColorGrading::bandSettingName(mBand, component);
    if (LLControlVariable* control = gSavedSettings.getControl(setting))
        control->resetToDefault(true);
    refreshMixer();
}

void ASPanelColorGrading::resetBand()
{
    ASColorGrading::resetBand(mBand);
    mPreset->setValue("Custom");
    refreshMixer();
}

void ASPanelColorGrading::refreshBandIndicators()
{
    for (S32 i = 0; i < ASColorGrading::BAND_COUNT; ++i)
        getChild<LLUICtrl>(BAND_DOTS[i])->setVisible(
            !gSavedSettings.getBOOL("ASColorGradeColorizeEnabled") &&
            ASColorGrading::bandModified((ASColorGrading::Band)i));
}

void ASPanelColorGrading::toggleColorize()
{
    gSavedSettings.setBOOL("ASColorGradeColorizeEnabled", mColorize->getValue().asBoolean());
    refreshMixer();
}

void ASPanelColorGrading::refreshPresets(const std::string& select)
{
    if (!mPreset) return;
    mPreset->clearRows();
    for (const std::string& name : ASColorGrading::listPresets()) mPreset->add(name);
    mPreset->add("Custom");
    mPreset->setValue(select);
}

void ASPanelColorGrading::loadPreset()
{
    const std::string name = presetName();
    if (!name.empty() && name != "Custom")
    {
        if (!ASColorGrading::loadPreset(name)) LLNotificationsUtil::add("ASColorGradePresetLoadFailed");
        else mPreset->setValue(name);
    }
    refreshMixer();
}

void ASPanelColorGrading::savePreset()
{
    std::string name = presetName();
    LLStringUtil::trim(name);
    LLSD args; args["NAME"] = name;
    LLSD payload; payload["name"] = name;
    LLNotificationsUtil::add("ASColorGradeConfirmSave", args, payload,
        boost::bind(&ASPanelColorGrading::confirmSave, this, _1, _2));
}

bool ASPanelColorGrading::confirmSave(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
    {
        const std::string name = notification["payload"]["name"].asString();
        if (ASColorGrading::savePreset(name)) refreshPresets(name);
        else LLNotificationsUtil::add("ASColorGradePresetSaveFailed");
    }
    return false;
}

void ASPanelColorGrading::deletePreset()
{
    const std::string name = presetName();
    LLSD args; args["NAME"] = name;
    LLSD payload; payload["name"] = name;
    LLNotificationsUtil::add("ASColorGradeConfirmDelete", args, payload,
        boost::bind(&ASPanelColorGrading::confirmDelete, this, _1, _2));
}

bool ASPanelColorGrading::confirmDelete(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
    {
        if (!ASColorGrading::deletePreset(notification["payload"]["name"].asString()))
            LLNotificationsUtil::add("ASColorGradePresetDeleteFailed");
        refreshPresets();
    }
    return false;
}

void ASPanelColorGrading::resetAll()
{
    LLNotificationsUtil::add("ASColorGradeConfirmReset", LLSD(), LLSD(),
        boost::bind(&ASPanelColorGrading::confirmReset, this, _1, _2));
}

bool ASPanelColorGrading::confirmReset(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
    {
        ASColorGrading::resetAll();
        refreshMixer();
        refreshPresets("[AS] Neutral");
    }
    return false;
}

void ASPanelColorGrading::setBefore(bool pressed) { ASColorGrading::setPreviewBypass(pressed); }
void ASPanelColorGrading::markCustom()
{
    if (mPreset && !mRefreshing) mPreset->setValue("Custom");
}

std::string ASPanelColorGrading::presetName() const
{
    return mPreset ? mPreset->getValue().asString() : std::string();
}

ASFloaterColorGrading::ASFloaterColorGrading(const LLSD& key) : LLFloater(key) {}
ASFloaterColorGrading::~ASFloaterColorGrading() { ASColorGrading::setPreviewBypass(false); }
void ASFloaterColorGrading::onClose(bool app_quitting)
{
    ASColorGrading::setPreviewBypass(false);
    LLFloater::onClose(app_quitting);
}
