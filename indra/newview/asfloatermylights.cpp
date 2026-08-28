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
#include "llrender.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsdserialize.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llfloaterreg.h"
#include "llviewermenu.h"

#include <set>

namespace
{
    const std::string PRESET_SUBDIR = "as_light_presets";
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
    mMasterEnabledCheck(nullptr),
    mRenderBackendCombo(nullptr),
    mBackgroundCombo(nullptr),
    mBackgroundColorSwatch(nullptr),
    mFreezeAnimationsCheck(nullptr),
    mBeaconCheck(nullptr),
    mPresetNameEditor(nullptr),
    mPresetCombo(nullptr),
    mUpdatingControls(false)
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
    mMasterEnabledCheck = getChild<LLCheckBoxCtrl>("as_light_master_enabled");
    mRenderBackendCombo = getChild<LLComboBox>("as_light_render_backend");
    mBackgroundCombo = getChild<LLComboBox>("as_light_background");
    mBackgroundColorSwatch = getChild<LLColorSwatchCtrl>("as_light_background_color");
    mFreezeAnimationsCheck = getChild<LLCheckBoxCtrl>("as_light_freeze_anim");
    mBeaconCheck = getChild<LLCheckBoxCtrl>("as_light_show_beacon");
    mPresetNameEditor = getChild<LLLineEditor>("as_light_preset_name");
    mPresetCombo = getChild<LLComboBox>("as_light_preset_combo");

    mLightList->setCommitOnSelectionChange(true);
    mLightList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLightSelected(); });
    getChild<LLUICtrl>("as_light_add")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddLight(); });
    getChild<LLUICtrl>("as_light_delete")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDeleteLight(); });
    mNameEditor->setCommitCallback([this](LLUICtrl*, const LLSD&) { onNameChanged(); });

    mDistanceSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDistanceChanged(); });
    mHeightSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onHeightChanged(); });
    mAzimuthSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAzimuthChanged(); });
    mIntensitySlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onIntensityChanged(); });
    mRadiusSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRadiusChanged(); });
    mFalloffSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFalloffChanged(); });
    mColorSwatch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onColorChanged(); });

    getChild<LLUICtrl>("as_light_preset_head")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetHead(); });
    getChild<LLUICtrl>("as_light_preset_chest")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetChest(); });
    getChild<LLUICtrl>("as_light_preset_back")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetBack(); });

    mMasterEnabledCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMasterEnabledChanged(); });
    mRenderBackendCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRenderBackendChanged(); });
    mBackgroundCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBackgroundModeChanged(); });
    mBackgroundColorSwatch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBackgroundColorChanged(); });
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

    setControlsEnabled(false);
    refreshPresetList();
    loadLightsFromFile(getAutosaveFilename());

    gIdleCallbacks.addFunction(onIdle, this);

    return true;
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
            !rig->isEnabled())
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
    std::vector<ASLightRigRenderer::Light> shader_lights;
    for (auto& rig : mLights)
    {
        rig->updateTransform();
        if (rig->isEnabled())
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

    LLSD row;
    row["columns"][0]["column"] = "name";
    row["columns"][0]["value"] = rig->mName;
    row["value"] = rig->mId;
    mLightList->addElement(row, ADD_BOTTOM);

    mSelectedLightId = rig->mId;
    mLights.push_back(std::move(rig));

    mLightList->setSelectedByValue(LLSD(mSelectedLightId), true);
    refreshControlsFromSelectedLight();
    setControlsEnabled(true);
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

    if (!mLights.empty())
    {
        selectLight(mLights.front()->mId);
    }
    else
    {
        mSelectedLightId.setNull();
        setControlsEnabled(false);
    }
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
}

void ASFloaterMyLights::refreshListRow(const ASLightRig* rig)
{
    LLScrollListItem* item = mLightList->getItem(LLSD(rig->mId));
    if (item)
    {
        item->getColumn(0)->setValue(rig->mName);
    }
}

void ASFloaterMyLights::rebuildList()
{
    mLightList->deleteAllItems();
    for (const auto& rig : mLights)
    {
        LLSD row;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = rig->mName;
        row["value"] = rig->mId;
        mLightList->addElement(row, ADD_BOTTOM);
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
    mDistanceSlider->setValue(rig->mDistance);
    mHeightSlider->setValue(rig->mHeight);
    mAzimuthSlider->setValue(rig->mAzimuth);
    mIntensitySlider->setValue(rig->mIntensity);
    mRadiusSlider->setValue(rig->mRadius);
    mFalloffSlider->setValue(rig->mFalloff);
    mColorSwatch->set(LLColor4(rig->mColor));
    mUpdatingControls = false;
}

void ASFloaterMyLights::onDistanceChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mDistance = (F32)mDistanceSlider->getValue().asReal();
    rig->updateTransform();
}

void ASFloaterMyLights::onHeightChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mHeight = (F32)mHeightSlider->getValue().asReal();
    rig->updateTransform();
}

void ASFloaterMyLights::onAzimuthChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mAzimuth = (F32)mAzimuthSlider->getValue().asReal();
    rig->updateTransform();
}

void ASFloaterMyLights::onIntensityChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mIntensity = (F32)mIntensitySlider->getValue().asReal();
    rig->applyParams();
}

void ASFloaterMyLights::onRadiusChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mRadius = (F32)mRadiusSlider->getValue().asReal();
    rig->applyParams();
}

void ASFloaterMyLights::onFalloffChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mFalloff = (F32)mFalloffSlider->getValue().asReal();
    rig->applyParams();
}

void ASFloaterMyLights::onColorChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mColor = LLColor3(mColorSwatch->get());
    rig->applyParams();
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
    updateLights();
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
    saveLightsToFile(filename);

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
}

void ASFloaterMyLights::saveLightsToFile(const std::string& filename) const
{
    LLSD data(LLSD::emptyArray());
    for (const auto& rig : mLights)
    {
        data.append(rig->toLLSD());
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

    for (auto& rig : mLights)
    {
        rig->destroy();
    }
    mLights.clear();
    mSelectedLightId.setNull();

    for (LLSD::array_const_iterator it = data.beginArray(); it != data.endArray(); ++it)
    {
        auto rig = std::make_unique<ASLightRig>();
        rig->fromLLSD(*it);
        rig->setEnabled(mMasterEnabled);
        rig->create();
        if (mSelectedLightId.isNull())
        {
            mSelectedLightId = rig->mId;
        }
        mLights.push_back(std::move(rig));
    }

    rebuildList();
    if (!mLights.empty())
    {
        selectLight(mLights.front()->mId);
    }
    else
    {
        setControlsEnabled(false);
    }
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
