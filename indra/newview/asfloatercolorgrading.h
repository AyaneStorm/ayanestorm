/**
 * @file asfloatercolorgrading.h
 * @author chanayane@ayanestorm
 * @brief Color-grading floater and reusable settings panel.
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
#ifndef AS_FLOATER_COLOR_GRADING_H
#define AS_FLOATER_COLOR_GRADING_H

#include "ascolorgrading.h"
#include "llfloater.h"
#include "llpanel.h"

#include <boost/signals2/connection.hpp>

class LLButton;
class LLComboBox;
class LLCheckBoxCtrl;
class LLColorSwatchCtrl;
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
    void commitSelectionColor();
    void resetMixer(const std::string& component);
    void resetBand();
    void refreshBandIndicators();
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
    LLColorSwatchCtrl* mSelectionColor;
    ASColorSliderCtrl* mMixerHue;
    ASColorSliderCtrl* mMixerSaturation;
    ASColorSliderCtrl* mMixerLightness;
    ASColorSliderCtrl* mMixerStrength;
    ASColorSliderCtrl* mMixerHueRange;
    ASColorSliderCtrl* mMixerChromaRange;
    ASColorSliderCtrl* mMixerLightnessRange;
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
