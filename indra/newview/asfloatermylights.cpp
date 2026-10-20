/**
 * @file asfloatermylights.cpp
 * @author chanayane@firestorm
 * @brief See asfloatermylights.h
 */

#include "llviewerprecompiledheaders.h"

#include "asfloatermylights.h"

#include <algorithm>

#include "asbackgroundisolate.h"
#include "aslightrigrenderer.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "llcombobox.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llglstates.h"
#include "lllineeditor.h"
#include "llrender2dutils.h"
#include "llmotioncontroller.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsdserialize.h"
#include "lltexturectrl.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llfloaterreg.h"
#include "llviewermenu.h"

#include <set>

namespace
{
    const std::string PRESET_SUBDIR = "as_light_presets";
    // Seconds of quiet after the last change before the autosave is written.
    const F32 AUTOSAVE_DELAY = 2.f;
}

ASFloaterMyLights::ASFloaterMyLights(const LLSD& key)
:   LLFloater(key),
    mMasterEnabled(true),
    mLightList(nullptr),
    mNameEditor(nullptr),
    mDistanceSlider(nullptr),
    mHeightSlider(nullptr),
    mAzimuthSlider(nullptr),
    mIntensitySlider(nullptr),
    mRadiusSlider(nullptr),
    mFalloffSlider(nullptr),
    mColorSwatch(nullptr),
    mLightOnCheck(nullptr),
    mPlacementCombo(nullptr),
    mPosXSlider(nullptr),
    mPosYSlider(nullptr),
    mPosZSlider(nullptr),
    mListFilterCombo(nullptr),
    mPresetRegionOnlyCheck(nullptr),
    mSpotCheck(nullptr),
    mAimJointCombo(nullptr),
    mAimYawSlider(nullptr),
    mAimPitchSlider(nullptr),
    mSpotFOVSlider(nullptr),
    mSpotFocusSlider(nullptr),
    mSpotAmbianceSlider(nullptr),
    mSpotTexturePicker(nullptr),
    mMasterEnabledCheck(nullptr),
    mRenderBackendCombo(nullptr),
    mBackgroundCombo(nullptr),
    mBackgroundColorSwatch(nullptr),
    mShowOtherAvatarsCheck(nullptr),
    mFreezeAnimationsCheck(nullptr),
    mBeaconCheck(nullptr),
    mPresetNameEditor(nullptr),
    mPresetCombo(nullptr),
    mUpdatingControls(false),
    mFullHeight(0),
    mCollapsedHeight(0),
    mAutosaveDirty(false),
    mWasTeleporting(false)
{
}

ASFloaterMyLights::~ASFloaterMyLights()
{
    gIdleCallbacks.deleteFunction(onIdle, this);
}

bool ASFloaterMyLights::postBuild()
{
    mLightList = getChild<LLScrollListCtrl>("as_light_list");
    mNameEditor = getChild<LLLineEditor>("as_light_name");
    mDistanceSlider = getChild<LLUICtrl>("as_light_distance");
    mHeightSlider = getChild<LLUICtrl>("as_light_height");
    mAzimuthSlider = getChild<LLUICtrl>("as_light_azimuth");
    mIntensitySlider = getChild<LLUICtrl>("as_light_intensity");
    mRadiusSlider = getChild<LLUICtrl>("as_light_radius");
    mFalloffSlider = getChild<LLUICtrl>("as_light_falloff");
    mColorSwatch = getChild<LLColorSwatchCtrl>("as_light_color");
    mLightOnCheck = getChild<LLCheckBoxCtrl>("as_light_on");
    mPlacementCombo = getChild<LLComboBox>("as_light_placement");
    mPosXSlider = getChild<LLUICtrl>("as_light_pos_x");
    mPosYSlider = getChild<LLUICtrl>("as_light_pos_y");
    mPosZSlider = getChild<LLUICtrl>("as_light_pos_z");
    mListFilterCombo = getChild<LLComboBox>("as_light_list_filter");
    mPresetRegionOnlyCheck = getChild<LLCheckBoxCtrl>("as_light_preset_region_only");
    mSpotCheck =getChild<LLCheckBoxCtrl>("as_light_spot");
    mAimJointCombo = getChild<LLComboBox>("as_light_aim_joint");
    mAimYawSlider = getChild<LLUICtrl>("as_light_aim_yaw");
    mAimPitchSlider = getChild<LLUICtrl>("as_light_aim_pitch");
    mSpotFOVSlider = getChild<LLUICtrl>("as_light_spot_fov");
    mSpotFocusSlider = getChild<LLUICtrl>("as_light_spot_focus");
    mSpotAmbianceSlider = getChild<LLUICtrl>("as_light_spot_ambiance");
    mSpotTexturePicker = getChild<LLTextureCtrl>("as_light_spot_texture");
    mMasterEnabledCheck = getChild<LLCheckBoxCtrl>("as_light_master_enabled");
    mRenderBackendCombo = getChild<LLComboBox>("as_light_render_backend");
    mBackgroundCombo = getChild<LLComboBox>("as_light_background");
    mBackgroundColorSwatch = getChild<LLColorSwatchCtrl>("as_light_background_color");
    mShowOtherAvatarsCheck = getChild<LLCheckBoxCtrl>("as_light_show_other_avatars");
    mFreezeAnimationsCheck = getChild<LLCheckBoxCtrl>("as_light_freeze_anim");
    mBeaconCheck = getChild<LLCheckBoxCtrl>("as_light_show_beacon");
    mPresetNameEditor = getChild<LLLineEditor>("as_light_preset_name");
    mPresetCombo = getChild<LLComboBox>("as_light_preset_combo");

    mLightList->setCommitOnSelectionChange(true);
    mLightList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLightSelected(); });
    getChild<LLUICtrl>("as_light_add")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddLight(); });
    getChild<LLUICtrl>("as_light_delete")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDeleteLight(); });
    mNameEditor->setCommitCallback([this](LLUICtrl*, const LLSD&) { onNameChanged(); });
    mLightOnCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLightOnChanged(); });
    mListFilterCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onListFilterChanged(); });
    getChild<LLUICtrl>("as_light_clear_region")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClearThisRegion(); });
    getChild<LLUICtrl>("as_light_clear_away")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClearAwayLights(); });

    mPlacementCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPlacementChanged(); });
    getChild<LLUICtrl>("as_light_place_camera")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPlaceAtCamera(); });
    getChild<LLUICtrl>("as_light_place_focus")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPlaceAtCameraFocus(); });
    for (LLUICtrl* ctrl : { mPosXSlider, mPosYSlider, mPosZSlider })
    {
        ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFixedOffsetChanged(); });
    }

    mDistanceSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDistanceChanged(); });
    mHeightSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onHeightChanged(); });
    mAzimuthSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAzimuthChanged(); });
    mIntensitySlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onIntensityChanged(); });
    mRadiusSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRadiusChanged(); });
    mFalloffSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFalloffChanged(); });
    mColorSwatch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onColorChanged(); });

    for (LLUICtrl* ctrl : { (LLUICtrl*)mSpotCheck, (LLUICtrl*)mAimJointCombo, mAimYawSlider, mAimPitchSlider,
                            mSpotFOVSlider, mSpotFocusSlider, mSpotAmbianceSlider })
    {
        ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSpotChanged(); });
    }
    // The picker applies immediately; "None"/cancel arrive through the cancel callback.
    mSpotTexturePicker->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSpotTextureChanged(); });
    mSpotTexturePicker->setOnCancelCallback([this](LLUICtrl*, const LLSD&) { onSpotTextureChanged(); });
    mSpotTexturePicker->setOnSelectCallback([this](LLUICtrl*, const LLSD&) { onSpotTextureChanged(); });

    getChild<LLUICtrl>("as_light_preset_head")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetHead(); });
    getChild<LLUICtrl>("as_light_preset_chest")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetChest(); });
    getChild<LLUICtrl>("as_light_preset_back")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetBack(); });

    mMasterEnabledCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMasterEnabledChanged(); });
    mRenderBackendCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRenderBackendChanged(); });
    mBackgroundCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBackgroundModeChanged(); });
    mBackgroundColorSwatch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBackgroundColorChanged(); });
    mShowOtherAvatarsCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShowOtherAvatarsChanged(); });
    mFreezeAnimationsCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFreezeAnimationsChanged(); });
    mBeaconCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBeaconToggled(); });
    getChild<LLUICtrl>("as_light_open_poser")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onOpenPoser(); });

    getChild<LLUICtrl>("as_light_save_preset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSavePreset(); });
    getChild<LLUICtrl>("as_light_load_preset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLoadPreset(); });

    mFreezeAnimationsCheck->setValue(LLMotionController::getCurrentTimeFactor() == 0.f);
    mBeaconCheck->setValue(gSavedSettings.getBOOL("ASRenderLightBeacon"));
    mMasterEnabled = gSavedSettings.getBOOL("ASLightRigMasterEnabled");
    mMasterEnabledCheck->setValue(mMasterEnabled);
    mRenderBackendCombo->setValue(gSavedSettings.getString("ASLightRigRenderBackend"));
    // Eye-specular masking lives in the direct shader path only.
    getChild<LLUICtrl>("as_light_no_eye_specular")->setEnabled(ASLightRigRenderer::usesShaderBackend());
    mShowOtherAvatarsCheck->setValue(gSavedSettings.getBOOL("ASLightRigShowOtherAvatars"));
    mListFilterCombo->setValue("active");

    // The spot section is the bottom band of the XUI, starting at its header;
    // collapsing keeps everything above it plus the same bottom margin.
    mFullHeight = getRect().getHeight();
    const S32 SPOT_SECTION_MARGIN = 22;
    mCollapsedHeight = mFullHeight - getChild<LLView>("as_light_spot_label")->getRect().mTop + SPOT_SECTION_MARGIN;
    updateSpotSectionVisibility();

    setControlsEnabled(false);
    refreshPresetList();
    loadLightsFromFile(getAutosaveFilename());

    gIdleCallbacks.addFunction(onIdle, this);

    return true;
}

void ASFloaterMyLights::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    // A saved rect may come from the other backend's height.
    updateSpotSectionVisibility();
}

void ASFloaterMyLights::onClose(bool app_quitting)
{
    // Lights are meant to survive a plain close/reopen -- only destroy them
    // when the viewer itself is quitting, since the underlying objects
    // can't outlive the process anyway (no session persistence besides an
    // explicit "Save preset..."). On quit, silently auto-save the current
    // rig to a fixed per-account file first, so it's restored automatically
    // next login without the user needing to manage a named preset for it.
    if (app_quitting)
    {
        saveLightsToFile(getAutosaveFilename());

        for (auto& rig : mLights)
        {
            rig->destroy();
        }
        mLights.clear();
        mSelectedLightId.setNull();
        rebuildList();
        setControlsEnabled(false);
    }

    // Isolate-background mode survives a plain close/reopen, same as the
    // lights themselves -- closing the floater is just hiding the panel,
    // not leaving photography mode. It must never survive an actual viewer
    // quit though, since ASBackgroundIsolate relies on static state that
    // would otherwise leave next session's world view stuck black/white
    // with no floater open to explain why.
    if (app_quitting)
    {
        ASBackgroundIsolate::setActive(false, LLColor4::black);
        ASBackgroundIsolate::restoreAllHiddenDrawables();
        mBackgroundCombo->setValue("None");
    }

    LLFloater::onClose(app_quitting);
}

// static
void ASFloaterMyLights::renderAllLightBeacons()
{
    if (!gSavedSettings.getBOOL("ASRenderLightBeacon"))
    {
        return;
    }

    ASFloaterMyLights* self = LLFloaterReg::findTypedInstance<ASFloaterMyLights>("as_my_lights");
    if (!self)
    {
        return;
    }

    static const F32 ARM_LENGTH = 0.15f;

    LLGLSUIDefault gls_ui;
    LLGLDepthTest gls_depth(GL_TRUE);
    gUIProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.setLineWidth(2.f);
    gGL.begin(LLRender::LINES);
    for (const auto& rig : self->mLights)
    {
        if ((!rig->isCreated() && !ASLightRigRenderer::usesShaderBackend()) ||
            !rig->isEnabled() || rig->isDormant())
        {
            continue;
        }
        // Beacon color matches this light's own color, so it's easy to tell
        // which beacon belongs to which light when several are on screen.
        gGL.color4fv(linearColor4(LLColor4(rig->mColor, 1.f)).mV);
        const LLVector3 pos = rig->getObjectPositionAgent();
        gGL.vertex3f(pos.mV[VX] - ARM_LENGTH, pos.mV[VY], pos.mV[VZ]);
        gGL.vertex3f(pos.mV[VX] + ARM_LENGTH, pos.mV[VY], pos.mV[VZ]);
        gGL.vertex3f(pos.mV[VX], pos.mV[VY] - ARM_LENGTH, pos.mV[VZ]);
        gGL.vertex3f(pos.mV[VX], pos.mV[VY] + ARM_LENGTH, pos.mV[VZ]);
        gGL.vertex3f(pos.mV[VX], pos.mV[VY], pos.mV[VZ] - ARM_LENGTH);
        gGL.vertex3f(pos.mV[VX], pos.mV[VY], pos.mV[VZ] + ARM_LENGTH);
        if (rig->mSpot && !ASLightRigRenderer::usesShaderBackend())
        {
            // Spot: one longer arm along the projection direction, growing
            // with the light's strength (1 m per 20 m of radius x intensity).
            const F32 aim_length = 0.4f + rig->mRadius * rig->mIntensity / 20.f;
            const LLVector3 tip = pos + rig->getAimDirection() * aim_length;
            gGL.vertex3fv(pos.mV);
            gGL.vertex3fv(tip.mV);
        }
    }
    gGL.end();
}

// static
void ASFloaterMyLights::onIdle(void* userdata)
{
    ASFloaterMyLights* self = static_cast<ASFloaterMyLights*>(userdata);
    self->updateLights();
}

void ASFloaterMyLights::updateLights()
{
    // The toolbar checkbox writes the shared master setting directly, so
    // mirror external changes into the live rig without altering its setup.
    const bool master_enabled = gSavedSettings.getBOOL("ASLightRigMasterEnabled");
    if (master_enabled != mMasterEnabled)
    {
        mMasterEnabled = master_enabled;
        mMasterEnabledCheck->setValue(mMasterEnabled);
        for (auto& rig : mLights)
        {
            rig->setEnabled(mMasterEnabled);
        }
    }

    std::vector<ASLightRigRenderer::Light> shader_lights;
    std::vector<bool> dormant;
    dormant.reserve(mLights.size());
    for (auto& rig : mLights)
    {
        rig->updateTransform();
        dormant.push_back(rig->isDormant());
        // Dormant fixed lights have no position in any loaded region.
        if (rig->isEnabled() && !rig->isDormant())
        {
            ASLightRigRenderer::Light light;
            light.position_agent = rig->getObjectPositionAgent();
            light.color_srgb = rig->mColor;
            light.intensity = rig->mIntensity;
            light.radius = rig->mRadius;
            light.falloff = rig->mFalloff;
            shader_lights.push_back(light);
        }
    }
    ASLightRigRenderer::setLights(shader_lights);

    // After a teleport, show the lights of the arrival region: switch to
    // Active here and select its first light (if any).
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    const bool teleport_done = mWasTeleporting && !teleporting;
    mWasTeleporting = teleporting;

    // "Active here" depends on the agent's region (teleports and walking
    // across borders), and the "(away: ...)" tags on dormancy, which changes
    // as regions load and unload.
    const LLViewerRegion* agent_region = gAgent.getRegion();
    const std::string region_name = agent_region ? agent_region->getName() : std::string();
    const bool region_changed = (region_name != mLastRegionName);
    mLastRegionName = region_name;

    if (teleport_done)
    {
        mLastDormant = dormant;
        mListFilterCombo->setValue("active");
        selectFirstListedLight();
    }
    else if (region_changed || dormant != mLastDormant)
    {
        mLastDormant = dormant;
        rebuildList();
        refreshControlsFromSelectedLight();
    }

    if (mAutosaveDirty && mAutosaveTimer.getElapsedTimeF32() > AUTOSAVE_DELAY)
    {
        mAutosaveDirty = false;
        saveLightsToFile(getAutosaveFilename());
    }

    // Keep ASBackgroundIsolate's exempt-object set current -- cheap (a
    // small set copy) and simplest way to handle lights being added/
    // removed/enabled while isolate mode is active, without a separate
    // change-notification path.
    ASBackgroundIsolate::setLightRigIds(getLightRigObjectIds());
}

std::set<LLUUID> ASFloaterMyLights::getLightRigObjectIds() const
{
    std::set<LLUUID> ids;
    for (const auto& rig : mLights)
    {
        if (rig->isCreated())
        {
            ids.insert(rig->getObjectId());
        }
    }
    return ids;
}

//
// List panel
//

void ASFloaterMyLights::onAddLight()
{
    auto rig = std::make_unique<ASLightRig>();
    rig->mName = llformat("Light %d", (int)mLights.size() + 1);
    rig->setEnabled(mMasterEnabled);
    rig->create();

    // New lights follow the avatar, so they are always listed.
    mSelectedLightId = rig->mId;
    mLights.push_back(std::move(rig));

    rebuildList();
    refreshControlsFromSelectedLight();
    markDirty();
}

void ASFloaterMyLights::onDeleteLight()
{
    LLScrollListItem* item = mLightList->getFirstSelected();
    if (!item)
    {
        return;
    }

    const LLUUID id = item->getValue().asUUID();
    auto it = std::find_if(mLights.begin(), mLights.end(),
        [&id](const std::unique_ptr<ASLightRig>& rig) { return rig->mId == id; });
    if (it != mLights.end())
    {
        (*it)->destroy();
        mLights.erase(it);
    }

    mLightList->deleteSelectedItems();

    // Select the first light still listed, if any.
    mSelectedLightId.setNull();
    for (const auto& rig : mLights)
    {
        if (isListed(rig.get()))
        {
            mSelectedLightId = rig->mId;
            break;
        }
    }
    rebuildList();
    refreshControlsFromSelectedLight();
    markDirty();
}

void ASFloaterMyLights::confirmDeleteLights(const std::string& message,
                                            std::function<bool(const ASLightRig&)> pred)
{
    S32 count = 0;
    for (const auto& rig : mLights)
    {
        count += pred(*rig) ? 1 : 0;
    }
    if (count == 0)
    {
        return;
    }

    LLSD args;
    args["MESSAGE"] = llformat("%s (%d light%s)", message.c_str(), count, count == 1 ? "" : "s");
    LLHandle<LLFloater> handle = getHandle();
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(),
        [handle, pred](const LLSD& notification, const LLSD& response)
        {
            ASFloaterMyLights* self = static_cast<ASFloaterMyLights*>(handle.get());
            if (!self || LLNotificationsUtil::getSelectedOption(notification, response) != 0)
            {
                return;
            }
            for (auto it = self->mLights.begin(); it != self->mLights.end();)
            {
                if (pred(**it))
                {
                    (*it)->destroy();
                    it = self->mLights.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            if (!self->getSelectedLight())
            {
                self->mSelectedLightId.setNull();
            }
            self->rebuildList();
            self->refreshControlsFromSelectedLight();
            self->markDirty();
        });
}

void ASFloaterMyLights::onClearThisRegion()
{
    const LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }
    const std::string name = region->getName();
    confirmDeleteLights("Delete every fixed light placed in " + name + "?",
        [name](const ASLightRig& rig)
        {
            return rig.isFixed() && rig.mRegionName == name && rig.mGrid == ASLightRig::currentGrid();
        });
}

void ASFloaterMyLights::onClearAwayLights()
{
    confirmDeleteLights("Delete every fixed light in regions that are not loaded?",
        [](const ASLightRig& rig) { return rig.isDormant(); });
}

void ASFloaterMyLights::onListFilterChanged()
{
    rebuildList();
    refreshControlsFromSelectedLight();
}

bool ASFloaterMyLights::isListed(const ASLightRig* rig) const
{
    // Active here: worn (following) lights and fixed lights placed in the
    // agent's own region. Lights in loaded neighbours still shine but are
    // listed only under All lights.
    return mListFilterCombo->getValue().asString() == "all" ||
           !rig->isFixed() || rig->belongsToRegion(gAgent.getRegion());
}

void ASFloaterMyLights::selectFirstListedLight()
{
    mSelectedLightId.setNull();
    for (const auto& rig : mLights)
    {
        if (isListed(rig.get()))
        {
            mSelectedLightId = rig->mId;
            break;
        }
    }
    rebuildList();
    refreshControlsFromSelectedLight();
}

// static
std::string ASFloaterMyLights::getRowLabel(const ASLightRig* rig)
{
    std::string label = rig->mName;
    if (!rig->isLightOn())
    {
        label += " (off)";
    }
    if (rig->isDormant())
    {
        label += " (away: " + rig->mRegionName + ")";
    }
    return label;
}

void ASFloaterMyLights::onLightSelected()
{
    LLScrollListItem* item = mLightList->getFirstSelected();
    if (!item)
    {
        return;
    }
    mSelectedLightId = item->getValue().asUUID();
    refreshControlsFromSelectedLight();
}

void ASFloaterMyLights::selectLight(const LLUUID& id)
{
    mSelectedLightId = id;
    mLightList->setSelectedByValue(LLSD(id), true);
    refreshControlsFromSelectedLight();
}

ASLightRig* ASFloaterMyLights::getSelectedLight()
{
    for (auto& rig : mLights)
    {
        if (rig->mId == mSelectedLightId)
        {
            return rig.get();
        }
    }
    return nullptr;
}

void ASFloaterMyLights::onNameChanged()
{
    ASLightRig* rig = getSelectedLight();
    if (!rig)
    {
        return;
    }
    rig->mName = mNameEditor->getText();
    refreshListRow(rig);
    markDirty();
}

void ASFloaterMyLights::refreshListRow(const ASLightRig* rig)
{
    LLScrollListItem* item = mLightList->getItem(LLSD(rig->mId));
    if (item)
    {
        item->getColumn(0)->setValue(getRowLabel(rig));
    }
}

void ASFloaterMyLights::rebuildList()
{
    mLightList->deleteAllItems();
    bool selected_listed = false;
    for (const auto& rig : mLights)
    {
        if (!isListed(rig.get()))
        {
            continue;
        }
        LLSD row;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = getRowLabel(rig.get());
        row["value"] = rig->mId;
        mLightList->addElement(row, ADD_BOTTOM);
        selected_listed |= (rig->mId == mSelectedLightId);
    }

    // A selection hidden by the filter is dropped so the editor never shows
    // a light that is not in the list.
    if (selected_listed)
    {
        mLightList->setSelectedByValue(LLSD(mSelectedLightId), true);
    }
    else
    {
        mSelectedLightId.setNull();
    }
}

//
// Edit controls
//

void ASFloaterMyLights::setControlsEnabled(bool enabled)
{
    mNameEditor->setEnabled(enabled);
    mDistanceSlider->setEnabled(enabled);
    mHeightSlider->setEnabled(enabled);
    mAzimuthSlider->setEnabled(enabled);
    mIntensitySlider->setEnabled(enabled);
    mRadiusSlider->setEnabled(enabled);
    mFalloffSlider->setEnabled(enabled);
    mColorSwatch->setEnabled(enabled);
    getChild<LLUICtrl>("as_light_preset_head")->setEnabled(enabled);
    getChild<LLUICtrl>("as_light_preset_chest")->setEnabled(enabled);
    getChild<LLUICtrl>("as_light_preset_back")->setEnabled(enabled);
    getChild<LLUICtrl>("as_light_delete")->setEnabled(enabled);
    mLightOnCheck->setEnabled(enabled);
    mPlacementCombo->setEnabled(enabled);
    updatePlacementControls();
    updateSpotControlsEnabled();
}

void ASFloaterMyLights::updatePlacementControls()
{
    static const char* const FOLLOW_CONTROLS[] = {
        "as_light_preset_head", "as_light_preset_chest", "as_light_preset_back",
        "as_light_distance", "as_light_height", "as_light_azimuth" };
    static const char* const FIXED_CONTROLS[] = {
        "as_light_place_camera", "as_light_place_focus",
        "as_light_pos_x", "as_light_pos_y", "as_light_pos_z", "as_light_region" };

    const ASLightRig* rig = getSelectedLight();
    const bool fixed = rig && rig->isFixed();
    for (const char* name : FOLLOW_CONTROLS)
    {
        getChild<LLView>(name)->setVisible(!fixed);
    }
    for (const char* name : FIXED_CONTROLS)
    {
        getChild<LLView>(name)->setVisible(fixed);
    }

    // Placing works even while dormant (it re-places the light here);
    // offsets need the home region to be loaded to be seen.
    const bool editable_pos = fixed && !rig->isDormant();
    getChild<LLUICtrl>("as_light_place_camera")->setEnabled(rig != nullptr);
    getChild<LLUICtrl>("as_light_place_focus")->setEnabled(rig != nullptr);
    mPosXSlider->setEnabled(editable_pos);
    mPosYSlider->setEnabled(editable_pos);
    mPosZSlider->setEnabled(editable_pos);
}

void ASFloaterMyLights::updateSpotControlsEnabled()
{
    // Spots exist only as LLVOVolume projectors; the shader backend is point-only.
    const ASLightRig* rig = getSelectedLight();
    const bool can_spot = rig && !ASLightRigRenderer::usesShaderBackend();
    const bool spot_on = can_spot && rig->mSpot;
    mSpotCheck->setEnabled(can_spot);
    mAimJointCombo->setEnabled(spot_on);
    mAimYawSlider->setEnabled(spot_on);
    mAimPitchSlider->setEnabled(spot_on);
    mSpotFOVSlider->setEnabled(spot_on);
    mSpotFocusSlider->setEnabled(spot_on);
    mSpotAmbianceSlider->setEnabled(spot_on);
    mSpotTexturePicker->setEnabled(spot_on);
}

void ASFloaterMyLights::refreshControlsFromSelectedLight()
{
    ASLightRig* rig = getSelectedLight();
    if (!rig)
    {
        setControlsEnabled(false);
        return;
    }

    setControlsEnabled(true);

    mUpdatingControls = true;
    mNameEditor->setText(rig->mName);
    mLightOnCheck->setValue(rig->isLightOn());
    mPlacementCombo->setValue(rig->isFixed() ? "fixed" : "follow");
    mPosXSlider->setValue(rig->mFixedOffset.mV[VX]);
    mPosYSlider->setValue(rig->mFixedOffset.mV[VY]);
    mPosZSlider->setValue(rig->mFixedOffset.mV[VZ]);
    getChild<LLUICtrl>("as_light_region")->setValue(
        !rig->isFixed() ? std::string()
        : rig->isDormant() ? "Region: " + rig->mRegionName + " (away)"
        : "Region: " + rig->mRegionName);
    mDistanceSlider->setValue(rig->mDistance);
    mHeightSlider->setValue(rig->mHeight);
    mAzimuthSlider->setValue(rig->mAzimuth);
    mIntensitySlider->setValue(rig->mIntensity);
    mRadiusSlider->setValue(rig->mRadius);
    mFalloffSlider->setValue(rig->mFalloff);
    mColorSwatch->set(LLColor4(rig->mColor));
    mSpotCheck->setValue(rig->mSpot);
    mAimJointCombo->setValue(rig->mAimJoint);
    mAimYawSlider->setValue(rig->mAimYaw);
    mAimPitchSlider->setValue(rig->mAimPitch);
    mSpotFOVSlider->setValue(rig->mSpotFOV * RAD_TO_DEG);
    mSpotFocusSlider->setValue(rig->mSpotFocus);
    mSpotAmbianceSlider->setValue(rig->mSpotAmbiance);
    mSpotTexturePicker->setImageAssetID(rig->mSpotTexture);
    mUpdatingControls = false;
    updatePlacementControls();
    updateSpotControlsEnabled();
}

void ASFloaterMyLights::onLightOnChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->setLightOn(mLightOnCheck->get());
    refreshListRow(rig);
    markDirty();
}

void ASFloaterMyLights::onPlacementChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    if (mPlacementCombo->getValue().asString() == "fixed")
    {
        // Freeze the light where it is now; a light that has never been
        // positioned (switched off since creation) goes to the camera.
        rig->makeFixedHere(gAgentCamera.getCameraPositionGlobal());
    }
    else
    {
        rig->makeFollow();
    }
    refreshListRow(rig);
    refreshControlsFromSelectedLight();
    markDirty();
}

void ASFloaterMyLights::placeSelectedAt(const LLVector3d& pos_global)
{
    ASLightRig* rig = getSelectedLight();
    if (!rig || !rig->placeAt(pos_global))
    {
        return; // no loaded region contains pos_global
    }
    refreshListRow(rig);
    refreshControlsFromSelectedLight();
    markDirty();
}

void ASFloaterMyLights::onPlaceAtCamera()
{
    placeSelectedAt(gAgentCamera.getCameraPositionGlobal());
}

void ASFloaterMyLights::onPlaceAtCameraFocus()
{
    placeSelectedAt(gAgentCamera.getFocusGlobal());
}

void ASFloaterMyLights::onFixedOffsetChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig || !rig->isFixed()) return;
    rig->setFixedOffset(LLVector3((F32)mPosXSlider->getValue().asReal(),
                                  (F32)mPosYSlider->getValue().asReal(),
                                  (F32)mPosZSlider->getValue().asReal()));
    markDirty();
}

void ASFloaterMyLights::onSpotChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mSpot = mSpotCheck->get();
    rig->mAimJoint = mAimJointCombo->getValue().asString();
    rig->mAimYaw = (F32)mAimYawSlider->getValue().asReal();
    rig->mAimPitch = (F32)mAimPitchSlider->getValue().asReal();
    rig->mSpotFOV = (F32)mSpotFOVSlider->getValue().asReal() * DEG_TO_RAD;
    rig->mSpotFocus = (F32)mSpotFocusSlider->getValue().asReal();
    rig->mSpotAmbiance = (F32)mSpotAmbianceSlider->getValue().asReal();
    rig->applyParams();
    rig->updateTransform();
    updateSpotControlsEnabled();
    markDirty();
}

void ASFloaterMyLights::onSpotTextureChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mSpotTexture = mSpotTexturePicker->getImageAssetID(); // null = plain white
    rig->applyParams();
    markDirty();
}

void ASFloaterMyLights::onDistanceChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mDistance = (F32)mDistanceSlider->getValue().asReal();
    rig->updateTransform();
    markDirty();
}

void ASFloaterMyLights::onHeightChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mHeight = (F32)mHeightSlider->getValue().asReal();
    rig->updateTransform();
    markDirty();
}

void ASFloaterMyLights::onAzimuthChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mAzimuth = (F32)mAzimuthSlider->getValue().asReal();
    rig->updateTransform();
    markDirty();
}

void ASFloaterMyLights::onIntensityChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mIntensity = (F32)mIntensitySlider->getValue().asReal();
    rig->applyParams();
    markDirty();
}

void ASFloaterMyLights::onRadiusChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mRadius = (F32)mRadiusSlider->getValue().asReal();
    rig->applyParams();
    markDirty();
}

void ASFloaterMyLights::onFalloffChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mFalloff = (F32)mFalloffSlider->getValue().asReal();
    rig->applyParams();
    markDirty();
}

void ASFloaterMyLights::onColorChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mColor = LLColor3(mColorSwatch->get());
    rig->applyParams();
    markDirty();
}

void ASFloaterMyLights::onPresetHead()
{
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mAnchorJoint = "mHead";
    rig->mDistance = 0.4f;
    rig->mHeight = 0.f;
    rig->mAzimuth = 0.f;
    refreshControlsFromSelectedLight();
    rig->updateTransform();
    markDirty();
}

void ASFloaterMyLights::onPresetChest()
{
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mAnchorJoint = "mChest";
    rig->mDistance = 0.5f;
    rig->mHeight = 0.f;
    rig->mAzimuth = 0.f;
    refreshControlsFromSelectedLight();
    rig->updateTransform();
    markDirty();
}

void ASFloaterMyLights::onPresetBack()
{
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mAnchorJoint = "mChest";
    rig->mDistance = 0.5f;
    rig->mHeight = 0.f;
    rig->mAzimuth = 180.f;
    refreshControlsFromSelectedLight();
    rig->updateTransform();
    markDirty();
}

//
// Master switch / background / freeze animations / beacon / poser
//

void ASFloaterMyLights::onMasterEnabledChanged()
{
    mMasterEnabled = mMasterEnabledCheck->get();
    gSavedSettings.setBOOL("ASLightRigMasterEnabled", mMasterEnabled);
    for (auto& rig : mLights)
    {
        rig->setEnabled(mMasterEnabled);
    }
}

void ASFloaterMyLights::onRenderBackendChanged()
{
    gSavedSettings.setString("ASLightRigRenderBackend", mRenderBackendCombo->getValue().asString());
    getChild<LLUICtrl>("as_light_no_eye_specular")->setEnabled(ASLightRigRenderer::usesShaderBackend());
    updateLights();
    updateSpotSectionVisibility();
    updateSpotControlsEnabled();
}

void ASFloaterMyLights::updateSpotSectionVisibility()
{
    static const char* const SPOT_CONTROLS[] = {
        "as_light_spot_label", "as_light_spot", "as_light_aim_joint_label", "as_light_aim_joint",
        "as_light_aim_yaw", "as_light_aim_pitch", "as_light_spot_fov", "as_light_spot_focus",
        "as_light_spot_ambiance", "as_light_spot_texture_label", "as_light_spot_texture" };

    const bool show = !ASLightRigRenderer::usesShaderBackend();
    for (const char* name : SPOT_CONTROLS)
    {
        getChild<LLView>(name)->setVisible(show);
    }

    // Keep the top edge in place: children follow left|top.
    const S32 height = show ? mFullHeight : mCollapsedHeight;
    LLRect rect = getRect();
    if (height > 0 && rect.getHeight() != height)
    {
        rect.mBottom = rect.mTop - height;
        setShape(rect);
    }
}

void ASFloaterMyLights::onBackgroundModeChanged()
{
    const std::string mode = mBackgroundCombo->getSelectedItemLabel();

    if (mode == "Normal Scene")
    {
        ASBackgroundIsolate::setActive(false, LLColor4::black);
        ASBackgroundIsolate::restoreAllHiddenDrawables();
        return;
    }

    ASBackgroundIsolate::setLightRigIds(getLightRigObjectIds());

    LLColor4 color(0.f, 0.f, 0.f, 1.f);
    if (mode == "All White")
    {
        color = LLColor4(1.f, 1.f, 1.f, 1.f);
    }
    else if (mode == "Custom")
    {
        color = mBackgroundColorSwatch->get();
    }
    ASBackgroundIsolate::setActive(true, color);
}

void ASFloaterMyLights::onBackgroundColorChanged()
{
    if (mBackgroundCombo->getSelectedItemLabel() != "Custom")
    {
        return;
    }
    ASBackgroundIsolate::setLightRigIds(getLightRigObjectIds());
    ASBackgroundIsolate::setActive(true, mBackgroundColorSwatch->get());
}

void ASFloaterMyLights::onShowOtherAvatarsChanged()
{
    gSavedSettings.setBOOL("ASLightRigShowOtherAvatars", mShowOtherAvatarsCheck->get());
    ASBackgroundIsolate::refreshHiddenDrawables();
}

void ASFloaterMyLights::onFreezeAnimationsChanged()
{
    set_all_animation_time_factors(mFreezeAnimationsCheck->get() ? 0.f : 1.f);
}

void ASFloaterMyLights::onBeaconToggled()
{
    gSavedSettings.setBOOL("ASRenderLightBeacon", mBeaconCheck->get());
}

void ASFloaterMyLights::onOpenPoser()
{
    LLFloaterReg::toggleInstance("fs_poser");
}

//
// Presets
//

std::string ASFloaterMyLights::getPresetDir() const
{
    std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, PRESET_SUBDIR, "");
    LLFile::mkdir(dir);
    return dir;
}

void ASFloaterMyLights::onSavePreset()
{
    std::string name = mPresetNameEditor->getText();
    if (name.empty())
    {
        return;
    }

    const std::string filename = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, PRESET_SUBDIR, name + ".xml");
    saveLightsToFile(filename, mPresetRegionOnlyCheck->get());

    refreshPresetList();
}

void ASFloaterMyLights::onLoadPreset()
{
    const std::string name = mPresetCombo->getSelectedItemLabel();
    if (name.empty())
    {
        return;
    }

    const std::string filename = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, PRESET_SUBDIR, name + ".xml");
    loadLightsFromFile(filename);
    markDirty();
}

void ASFloaterMyLights::markDirty()
{
    mAutosaveDirty = true;
    mAutosaveTimer.reset();
}

void ASFloaterMyLights::saveLightsToFile(const std::string& filename, bool region_only) const
{
    // Full rig: an array of lights. Region-only: a map naming the region,
    // with just the fixed lights placed there.
    LLSD lights(LLSD::emptyArray());
    const LLViewerRegion* region = region_only ? gAgent.getRegion() : nullptr;
    if (region_only && !region)
    {
        return;
    }
    for (const auto& rig : mLights)
    {
        if (!region_only || rig->belongsToRegion(region))
        {
            lights.append(rig->toLLSD());
        }
    }

    LLSD data = lights;
    if (region_only)
    {
        data = LLSD::emptyMap();
        data["region"] = region->getName();
        data["grid"] = ASLightRig::currentGrid();
        data["lights"] = lights;
    }

    llofstream file(filename.c_str());
    if (file.is_open())
    {
        LLSDSerialize::toPrettyXML(data, file);
        file.close();
    }
}

void ASFloaterMyLights::loadLightsFromFile(const std::string& filename)
{
    if (!LLFile::isfile(filename))
    {
        return;
    }

    llifstream file(filename.c_str());
    LLSD data;
    LLSDSerialize::fromXMLDocument(data, file);
    file.close();

    // A region-only preset replaces just that region's fixed lights; a full
    // rig (array) replaces every light.
    const bool region_only = data.isMap();
    const std::string region_name = region_only ? data["region"].asString() : std::string();
    const std::string grid = region_only ? data["grid"].asString() : std::string();
    const LLSD lights = region_only ? data["lights"] : data;

    for (auto it = mLights.begin(); it != mLights.end();)
    {
        const bool replace = !region_only ||
            ((*it)->isFixed() && (*it)->mRegionName == region_name && (*it)->mGrid == grid);
        if (replace)
        {
            (*it)->destroy();
            it = mLights.erase(it);
        }
        else
        {
            ++it;
        }
    }
    mSelectedLightId.setNull();

    for (LLSD::array_const_iterator it = lights.beginArray(); it != lights.endArray(); ++it)
    {
        auto rig = std::make_unique<ASLightRig>();
        rig->fromLLSD(*it);
        rig->setEnabled(mMasterEnabled);
        rig->create();       // no-op while a fixed light's region is unloaded
        rig->updateTransform();
        mLights.push_back(std::move(rig));
    }

    // Select the first light that the list filter shows.
    for (const auto& rig : mLights)
    {
        if (isListed(rig.get()))
        {
            mSelectedLightId = rig->mId;
            break;
        }
    }
    rebuildList();
    refreshControlsFromSelectedLight();
}

std::string ASFloaterMyLights::getAutosaveFilename() const
{
    return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "as_my_lights_autosave.xml");
}

void ASFloaterMyLights::refreshPresetList()
{
    mPresetCombo->removeall();

    const std::string dir = getPresetDir();
    LLDirIterator iter(dir, "*.xml");
    std::string filename;
    while (iter.next(filename))
    {
        std::string name = filename;
        std::string::size_type ext = name.rfind(".xml");
        if (ext != std::string::npos)
        {
            name = name.substr(0, ext);
        }
        mPresetCombo->add(name);
    }
}
