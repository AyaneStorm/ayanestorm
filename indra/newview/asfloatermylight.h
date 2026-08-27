/**
 * @file asfloatermylight.h
 * @author chanayane@firestorm
 * @brief Self-only photography lighting floater. Manages a list of
 *        viewer-local light sources that follow the avatar, an isolate
 *        background mode, an animation freeze toggle, and a light-position
 *        beacon -- plus quick access to the Firestorm Poser.
 */

#ifndef AS_FLOATERMYLIGHT_H
#define AS_FLOATERMYLIGHT_H

#include "llfloater.h"
#include "aslightrig.h"
#include "v4color.h"

#include <memory>
#include <set>
#include <vector>

class LLUICtrl;
class LLColorSwatchCtrl;
class LLScrollListCtrl;
class LLLineEditor;
class LLComboBox;
class LLCheckBoxCtrl;

class ASFloaterMyLight : public LLFloater
{
public:
    ASFloaterMyLight(const LLSD& key);
    virtual ~ASFloaterMyLight();

    bool postBuild() override;
    void onClose(bool app_quitting) override;

    // Called from a tag-wrapped call-out near render_hud_elements()'s stock
    // beacon call in llviewerdisplay.cpp. Draws a cross beacon at every
    // active light's position when the ASRenderLightBeacon setting is on.
    // Rides on the same RENDER_DEBUG_FEATURE_UI gate as stock beacons, so it
    // is automatically excluded from snapshots.
    static void renderAllLightBeacons();

private:
    static void onIdle(void* userdata);
    void updateLights();
    std::set<LLUUID> getLightRigObjectIds() const;

    // List panel
    void onAddLight();
    void onDeleteLight();
    void onLightSelected();
    void onNameChanged();
    void selectLight(const LLUUID& id);
    ASLightRig* getSelectedLight();
    void refreshListRow(const ASLightRig* rig);
    void rebuildList();

    // Edit controls, bound to the currently-selected light
    void onDistanceChanged();
    void onHeightChanged();
    void onAzimuthChanged();
    void onIntensityChanged();
    void onRadiusChanged();
    void onFalloffChanged();
    void onColorChanged();
    void onPresetHead();
    void onPresetChest();
    void onPresetBack();
    void refreshControlsFromSelectedLight();
    void setControlsEnabled(bool enabled);

    // Master switch, background, freeze animations, beacon, poser
    void onMasterEnabledChanged();
    void onBackgroundModeChanged();
    void onBackgroundColorChanged();
    void onFreezeAnimationsChanged();
    void onBeaconToggled();
    void onOpenPoser();

    // Presets
    void onSavePreset();
    void onLoadPreset();
    void refreshPresetList();
    std::string getPresetDir() const;

    // Auto-save: mirrors preset save/load but always targets a fixed
    // per-account file, so the rig quietly survives a logout/login without
    // the user having to manage a named preset for it.
    void saveLightsToFile(const std::string& filename) const;
    void loadLightsFromFile(const std::string& filename);
    std::string getAutosaveFilename() const;

    std::vector<std::unique_ptr<ASLightRig>> mLights;
    LLUUID mSelectedLightId;
    bool mMasterEnabled;

    LLScrollListCtrl* mLightList;
    LLLineEditor* mNameEditor;
    LLUICtrl* mDistanceSlider;
    LLUICtrl* mHeightSlider;
    LLUICtrl* mAzimuthSlider;
    LLUICtrl* mIntensitySlider;
    LLUICtrl* mRadiusSlider;
    LLUICtrl* mFalloffSlider;
    LLColorSwatchCtrl* mColorSwatch;
    LLCheckBoxCtrl* mMasterEnabledCheck;
    LLComboBox* mBackgroundCombo;
    LLColorSwatchCtrl* mBackgroundColorSwatch;
    LLCheckBoxCtrl* mFreezeAnimationsCheck;
    LLCheckBoxCtrl* mBeaconCheck;
    LLLineEditor* mPresetNameEditor;
    LLComboBox* mPresetCombo;

    bool mUpdatingControls;
};

#endif // AS_FLOATERMYLIGHT_H
