/**
 * @file asfloatercolorgrading.h
 * @author chanayane@firestorm
 * @brief Color-grading floater and reusable settings panel.
 */
#ifndef AS_FLOATER_COLOR_GRADING_H
#define AS_FLOATER_COLOR_GRADING_H

#include "ascolorgrading.h"
#include "llfloater.h"
#include "llpanel.h"

#include <boost/signals2/connection.hpp>

class LLButton;
class LLComboBox;
class LLCheckBoxCtrl;
class LLSliderCtrl;
class LLTextBox;
class ASColorSliderCtrl;

class ASPanelColorGrading : public LLPanel
{
public:
    ASPanelColorGrading();
    ~ASPanelColorGrading() override;
    bool postBuild() override;
    void draw() override;
    void onOpen(const LLSD& key) override;
    void onVisibilityChange(bool visible) override;

private:
    void selectBand(ASColorGrading::Band band);
    void refreshMixer();
    void commitMixer(const std::string& component, LLSliderCtrl* control);
    void resetMixer(const std::string& component);
    void toggleColorize();
    void refreshSplitToning();
    void refreshPresets(const std::string& select = "Custom");
    void loadPreset();
    void savePreset();
    void deletePreset();
    void resetAll();
    bool confirmSave(const LLSD& notification, const LLSD& response);
    bool confirmDelete(const LLSD& notification, const LLSD& response);
    bool confirmReset(const LLSD& notification, const LLSD& response);
    void setBefore(bool pressed);
    void markCustom();
    std::string presetName() const;

    ASColorGrading::Band mBand;
    bool mRefreshing;
    LLComboBox* mPreset;
    LLButton* mBefore;
    LLTextBox* mBandName;
    LLCheckBoxCtrl* mColorize;
    LLButton* mColorizeSwatch;
    ASColorSliderCtrl* mMixerTargetLightness;
    ASColorSliderCtrl* mMixerHue;
    ASColorSliderCtrl* mMixerSaturation;
    ASColorSliderCtrl* mMixerLuminance;
    ASColorSliderCtrl* mMixerStrength;
    ASColorSliderCtrl* mMixerTolerance;
    ASColorSliderCtrl* mMixerSoftness;
    ASColorSliderCtrl* mSplitHighlightsSaturation;
    ASColorSliderCtrl* mSplitShadowsSaturation;
    LLButton* mSplitHighlightsSwatch;
    LLButton* mSplitShadowsSwatch;
    std::vector<boost::signals2::connection> mSettingConnections;
};

class ASFloaterColorGrading : public LLFloater
{
public:
    ASFloaterColorGrading(const LLSD& key);
    ~ASFloaterColorGrading() override;
    void onClose(bool app_quitting) override;
};

#endif
