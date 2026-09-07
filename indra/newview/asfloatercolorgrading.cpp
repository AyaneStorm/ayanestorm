/**
 * @file asfloatercolorgrading.cpp
 * @author chanayane@firestorm
 * @brief Color-grading floater and reusable settings panel.
 */
#include "llviewerprecompiledheaders.h"

#include "asfloatercolorgrading.h"
#include "ascolorslider.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llcheckboxctrl.h"
#include "llnotificationsutil.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

static LLPanelInjector<ASPanelColorGrading> sColorGradingPanel("as_color_grading_panel");

namespace
{
    const char* const BAND_LABELS[ASColorGrading::BAND_COUNT] =
        { "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta" };
    const char* const BAND_BUTTONS[ASColorGrading::BAND_COUNT] =
        { "band_red", "band_orange", "band_yellow", "band_green", "band_aqua", "band_blue", "band_purple", "band_magenta" };
    const LLColor4 BAND_COLORS[ASColorGrading::BAND_COUNT] =
        { LLColor4(.92f,.16f,.18f,1.f), LLColor4(.95f,.48f,.12f,1.f),
          LLColor4(.92f,.76f,.08f,1.f), LLColor4(.20f,.72f,.28f,1.f),
          LLColor4(.08f,.72f,.76f,1.f), LLColor4(.10f,.42f,.92f,1.f),
          LLColor4(.55f,.24f,.86f,1.f), LLColor4(.88f,.12f,.68f,1.f) };
}

ASPanelColorGrading::ASPanelColorGrading()
    : mBand(ASColorGrading::RED), mRefreshing(false), mPreset(nullptr), mBefore(nullptr), mBandName(nullptr),
      mColorize(nullptr), mColorizeSwatch(nullptr),
      mMixerHue(nullptr), mMixerSaturation(nullptr), mMixerLuminance(nullptr),
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
    mMixerHue = getChild<ASColorSliderCtrl>("mixer_hue");
    mMixerSaturation = getChild<ASColorSliderCtrl>("mixer_saturation");
    mMixerLuminance = getChild<ASColorSliderCtrl>("mixer_luminance");
    mSplitHighlightsSaturation = getChild<ASColorSliderCtrl>("ASColorGradeSplitHighlightsSaturation");
    mSplitShadowsSaturation = getChild<ASColorSliderCtrl>("ASColorGradeSplitShadowsSaturation");
    mSplitHighlightsSwatch = getChild<LLButton>("split_highlights_swatch");
    mSplitShadowsSwatch = getChild<LLButton>("split_shadows_swatch");

    mPreset->setCommitCallback(boost::bind(&ASPanelColorGrading::loadPreset, this));
    getChild<LLButton>("save_preset")->setCommitCallback(boost::bind(&ASPanelColorGrading::savePreset, this));
    getChild<LLButton>("delete_preset")->setCommitCallback(boost::bind(&ASPanelColorGrading::deletePreset, this));
    getChild<LLButton>("reset_all")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetAll, this));

    mBefore = getChild<LLButton>("before");
    mBefore->setMouseDownCallback(boost::bind(&ASPanelColorGrading::setBefore, this, true));
    mBefore->setMouseUpCallback(boost::bind(&ASPanelColorGrading::setBefore, this, false));

    for (S32 i = 0; i < ASColorGrading::BAND_COUNT; ++i)
    {
        LLButton* button = getChild<LLButton>(BAND_BUTTONS[i]);
        // Apply the band colors in code so skin colors cannot tint every
        // selector through the same named UI color.
        button->setImageColor(LLUIColor(BAND_COLORS[i]));
        button->setCommitCallback(boost::bind(
            &ASPanelColorGrading::selectBand, this, (ASColorGrading::Band)i));
    }
    mMixerHue->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "Hue", mMixerHue));
    mMixerSaturation->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "Saturation", mMixerSaturation));
    mMixerLuminance->setCommitCallback(boost::bind(&ASPanelColorGrading::commitMixer, this, "Luminance", mMixerLuminance));
    getChild<LLButton>("reset_mixer_hue")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "Hue"));
    getChild<LLButton>("reset_mixer_saturation")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "Saturation"));
    getChild<LLButton>("reset_mixer_luminance")->setCommitCallback(boost::bind(&ASPanelColorGrading::resetMixer, this, "Luminance"));
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
    refreshPresets();
    selectBand(ASColorGrading::RED);
    return true;
}

void ASPanelColorGrading::draw()
{
    refreshSplitToning();
    const bool grain_enabled = gSavedSettings.getF32("ASColorGradeGrainAmount") > 0.f;
    getChild<LLUICtrl>("ASColorGradeGrainSize")->setEnabled(grain_enabled);
    getChild<LLUICtrl>("ASColorGradeGrainRoughness")->setEnabled(grain_enabled);
    getChild<LLUICtrl>("ASColorGradeGrainColor")->setEnabled(grain_enabled);
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

void ASPanelColorGrading::onOpen(const LLSD&) { refreshPresets(mPreset ? presetName() : "Custom"); }

void ASPanelColorGrading::onVisibilityChange(bool visible)
{
    LLPanel::onVisibilityChange(visible);
    if (!visible) setBefore(false);
}

void ASPanelColorGrading::selectBand(ASColorGrading::Band band)
{
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
    mBandName->setText(LLStringExplicit(colorize ? "Colorize" : BAND_LABELS[mBand]));
    mMixerHue->setMinValue(colorize ? 0.f : -100.f); mMixerHue->setMaxValue(colorize ? 360.f : 100.f);
    mMixerSaturation->setMinValue(colorize ? 0.f : -100.f); mMixerSaturation->setMaxValue(100.f);
    const std::string prefix = colorize ? "ASColorGradeColorize" : "";
    mMixerHue->setValue(gSavedSettings.getF32(colorize ? prefix + "Hue" : ASColorGrading::bandSettingName(mBand, "Hue")));
    mMixerSaturation->setValue(gSavedSettings.getF32(colorize ? prefix + "Saturation" : ASColorGrading::bandSettingName(mBand, "Saturation")));
    mMixerLuminance->setValue(gSavedSettings.getF32(colorize ? prefix + "Luminance" : ASColorGrading::bandSettingName(mBand, "Luminance")));
    const F32 centers[ASColorGrading::BAND_COUNT] = { 0.f, 30.f, 60.f, 120.f, 180.f, 240.f, 280.f, 320.f };
    LLColor3 hue_left, hue_center, hue_right;
    hue_left.setHSL(fmodf((centers[mBand] - 30.f + 360.f) / 360.f, 1.f), .85f, .5f);
    hue_center.setHSL(centers[mBand] / 360.f, .85f, .5f);
    hue_right.setHSL(fmodf((centers[mBand] + 30.f) / 360.f, 1.f), .85f, .5f);
    const LLColor4 band(hue_center, 1.f);
    if (colorize)
    {
        std::vector<LLColor4> wheel;
        for (S32 i = 0; i <= 12; ++i) { LLColor3 c; c.setHSL((F32)i / 12.f, .85f, .5f); wheel.emplace_back(c, 1.f); }
        const F32 selected_hue = gSavedSettings.getF32("ASColorGradeColorizeHue");
        LLColor3 selected; selected.setHSL(selected_hue / 360.f, .85f, .5f);
        const LLColor4 selected4(selected, 1.f);
        mMixerHue->setTrackColors(wheel);
        mMixerSaturation->setTrackColors({ LLColor4(.5f,.5f,.5f,1.f), selected4 });
        mMixerLuminance->setTrackColors({ LLColor4::black, selected4, LLColor4::white });
        mColorizeSwatch->setImageColor(LLUIColor(selected4));
    }
    else
    {
        mMixerHue->setTrackColors({ LLColor4(hue_left, 1.f), band, LLColor4(hue_right, 1.f) });
        mMixerSaturation->setTrackColors({ LLColor4(.42f,.42f,.42f,1.f), band, LLColor4(hue_center * 1.25f, 1.f) });
        mMixerLuminance->setTrackColors({ LLColor4::black, band, LLColor4::white });
    }
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
        if (gSavedSettings.getBOOL("ASColorGradeColorizeEnabled")) refreshMixer();
    }
}

void ASPanelColorGrading::resetMixer(const std::string& component)
{
    const std::string setting = gSavedSettings.getBOOL("ASColorGradeColorizeEnabled") ?
        "ASColorGradeColorize" + component : ASColorGrading::bandSettingName(mBand, component);
    if (LLControlVariable* control = gSavedSettings.getControl(setting))
        control->resetToDefault(true);
    refreshMixer();
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
