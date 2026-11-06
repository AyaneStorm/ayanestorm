/**
 * @file asfloatermylights.h
 * @author chanayane@firestorm
 * @brief Self-only photography lighting floater. Manages a list of
 *        viewer-local light sources that follow the avatar, an isolate
 *        background mode, an animation freeze toggle, and a light-position
 *        beacon -- plus quick access to the Firestorm Poser.
 */

#ifndef AS_FLOATERMYLIGHTS_H
#define AS_FLOATERMYLIGHTS_H

#include "llfloater.h"
#include "llframetimer.h"
#include "aslightrig.h"
#include "v4color.h"

#include <functional>
#include <memory>
#include <set>
#include <vector>

class LLUICtrl;
class LLColorSwatchCtrl;
class LLScrollListCtrl;
class LLLineEditor;
class LLComboBox;
class LLCheckBoxCtrl;
class LLTextureCtrl;

class ASFloaterMyLights : public LLFloater
{
public:
    ASFloaterMyLights(const LLSD& key);
    virtual ~ASFloaterMyLights();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
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
    // Rebuilds the list from mLights through the list filter, keeping the
    // selection when it is still listed.
    void rebuildList();
    // List filter: "Active here" lists worn lights and fixed lights placed
    // in the agent's region; "All" lists every light.
    bool isListed(const ASLightRig* rig) const;
    // Selects the first listed light (or none) and rebuilds the list.
    void selectFirstListedLight();
    // Name plus "(off)" / "(away: Region)" tags.
    static std::string getRowLabel(const ASLightRig* rig);
    void onListFilterChanged();
    // Removes every light matching pred, after a confirmation.
    void confirmDeleteLights(const std::string& message, std::function<bool(const ASLightRig&)> pred);
    void onClearThisRegion();
    void onClearAwayLights();

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
    void onLightOnChanged();

    // Placement: Follow avatar (joint presets + distance/height/azimuth) or
    // Fixed (place at camera / camera focus + X/Y/Z offsets from there).
    void onPlacementChanged();
    void onPlaceAtCamera();
    void onPlaceAtCameraFocus();
    void placeSelectedAt(const LLVector3d& pos_global);
    void onFixedOffsetChanged();
    // Shows the controls of the selected light's placement mode.
    void updatePlacementControls();

    // Spot light controls (LLVOVolume backend only)
    void onSpotChanged();
    void onSpotTextureChanged();
    void updateSpotControlsEnabled();
    // Shows the spot section only with the LLVOVolume backend and resizes
    // the floater so the hidden section leaves no gap.
    void updateSpotSectionVisibility();

    // Master switch, background, freeze animations, beacon, poser
    void onMasterEnabledChanged();
    void onRenderBackendChanged();
    void onBackgroundModeChanged();
    void onBackgroundColorChanged();
    void onShowOtherAvatarsChanged();
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
    // A preset saved "This region only" holds just the current region's fixed
    // lights; loading it replaces only that region's fixed lights.
    void saveLightsToFile(const std::string& filename, bool region_only = false) const;
    void loadLightsFromFile(const std::string& filename);
    std::string getAutosaveFilename() const;
    // Autosave shortly after any change, not only on quit, so a crash does
    // not undo recent edits. Writes are coalesced in updateLights().
    void markDirty();

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
    LLCheckBoxCtrl* mLightOnCheck;
    LLComboBox* mPlacementCombo;
    LLUICtrl* mPosXSlider;
    LLUICtrl* mPosYSlider;
    LLUICtrl* mPosZSlider;
    LLComboBox* mListFilterCombo;
    LLCheckBoxCtrl* mPresetRegionOnlyCheck;
    LLCheckBoxCtrl* mSpotCheck;
    LLComboBox* mAimJointCombo;
    LLUICtrl* mAimYawSlider;
    LLUICtrl* mAimPitchSlider;
    LLUICtrl* mSpotFOVSlider;
    LLUICtrl* mSpotFocusSlider;
    LLUICtrl* mSpotAmbianceSlider;
    LLTextureCtrl* mSpotTexturePicker;
    LLCheckBoxCtrl* mMasterEnabledCheck;
    LLComboBox* mRenderBackendCombo;
    LLComboBox* mBackgroundCombo;
    LLColorSwatchCtrl* mBackgroundColorSwatch;
    LLCheckBoxCtrl* mShowOtherAvatarsCheck;
    LLCheckBoxCtrl* mFreezeAnimationsCheck;
    LLCheckBoxCtrl* mBeaconCheck;
    LLLineEditor* mPresetNameEditor;
    LLComboBox* mPresetCombo;

    bool mUpdatingControls;
    S32 mFullHeight;        // XUI height, spot section included
    S32 mCollapsedHeight;   // height without the spot section

    bool mAutosaveDirty;
    LLFrameTimer mAutosaveTimer;        // time since the last change
    std::vector<bool> mLastDormant;     // per-light dormancy, to refresh the list on change
    std::string mLastRegionName;        // agent region, to refresh "Active here" on change
    bool mWasTeleporting;               // to detect the end of a teleport
};

#endif // AS_FLOATERMYLIGHTS_H
