/**
 * @file asfloatermylight.cpp
 * @author chanayane@firestorm
 * @brief See asfloatermylight.h
 */

#include "llviewerprecompiledheaders.h"

#include "asfloatermylight.h"

#include <algorithm>

#include "asbackgroundisolate.h"
#include "llcallbacklist.h"
#include "llcharacter.h"
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
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <set>

extern LLPipeline gPipeline;

namespace
{
    // Whether the isolate render-type toggles / per-object hiding are
    // currently applied -- the actual backdrop color/paint is owned by
    // ASBackgroundIsolate (a late-pipeline shader pass, see
    // asbackgroundisolate.h), which also gates lens flare/vignette/
    // volumetric lighting so nothing draws over the solid isolate color.
    bool sBackgroundOverrodeRenderTypes = false;
    // UUIDs of other avatars we forced to AV_DO_NOT_RENDER, so we restore
    // exactly and only those, without disturbing a user's own mute/render
    // choices for avatars we never touched.
    std::set<LLUUID> sHiddenAvatarIds;
    // UUIDs of non-self objects we force-hid via LLPipeline::hideObject, so
    // we restore exactly and only those on exit.
    std::set<LLUUID> sHiddenObjectIds;

    const std::string PRESET_SUBDIR = "as_light_presets";

    // Render types covering non-self scene content that are safe to disable
    // outright. RENDER_TYPE_SKY/RENDER_TYPE_WL_SKY are deliberately excluded:
    // disabling them stops LLVOSky::updateSky() from refreshing atmospherics
    // and leaves the reflection-probe manager baking ambient/irradiance
    // cubemaps from a degenerate (sky/terrain/water-less) scene, which every
    // surface then samples for lighting -- this is what caused the
    // camera-reactive magenta tint. Sky is instead painted over visually by
    // ASBackgroundIsolate::render() (asbackgroundisolate.cpp), a solid-color
    // shader pass drawn after the whole post-process chain, so sky/
    // atmosphere/probes keep rendering normally under the hood while being
    // invisible on screen.
    // RENDER_TYPE_AVATAR and RENDER_TYPE_VOLUME are deliberately excluded
    // too: every LLVOVolume drawable (rigged mesh attachments included)
    // carries RENDER_TYPE_VOLUME regardless of whether it belongs to the
    // self avatar, so disabling it hid the self avatar's own worn mesh
    // clothing/attachments along with everyone else's builds. Both other
    // avatars and other people's/world's non-avatar objects are instead
    // hidden per-instance below, leaving self (avatar + attachments) and
    // our own light-rig objects untouched.
    // RENDER_TYPE_PARTICLES stays disabled here rather than per-instance:
    // particles render through their own LLVOPartGroup drawable, entirely
    // separate from the emitting source object, so hiding the source via
    // refreshHiddenObjects() below has no effect on its particles -- and our
    // own light rig never emits particles, so there's no self-owned case
    // that needs preserving.
    const U32 sIsolateRenderTypes[] = {
        LLPipeline::RENDER_TYPE_CLOUDS,
        LLPipeline::RENDER_TYPE_TERRAIN,
        LLPipeline::RENDER_TYPE_GRASS,
        LLPipeline::RENDER_TYPE_TREE,
        LLPipeline::RENDER_TYPE_WATER,
        LLPipeline::RENDER_TYPE_VOIDWATER,
        LLPipeline::RENDER_TYPE_WATEREXCLUSION,
        LLPipeline::RENDER_TYPE_PARTICLES,
    };

    void hideOtherAvatars()
    {
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
            if (avatar && !avatar->isSelf() && !avatar->isDead())
            {
                avatar->setVisualMuteSettings(LLVOAvatar::AV_DO_NOT_RENDER);
                sHiddenAvatarIds.insert(avatar->getID());
            }
        }
    }

    void showOtherAvatars()
    {
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
            if (avatar && sHiddenAvatarIds.count(avatar->getID()))
            {
                avatar->setVisualMuteSettings(LLVOAvatar::AV_RENDER_NORMALLY);
            }
        }
        sHiddenAvatarIds.clear();
    }

    // Hides every non-avatar object in the scene that isn't attached to the
    // self avatar and isn't one of our own light-rig objects, via the
    // per-drawable FORCE_INVISIBLE state (LLPipeline::hideObject) -- the
    // same non-destructive, server-desync-free mechanism the pathfinding
    // floaters already use to hide objects, rather than the render-type
    // mask (which has no way to distinguish "my attachment" from "someone
    // else's object", since they share the same RENDER_TYPE_VOLUME bucket).
    //
    // LLViewerObject::processUpdateMessage() silently clears FORCE_INVISIBLE
    // on any inbound object-update packet for a non-orphaned object -- which
    // happens continuously during normal gameplay (terse updates, interest
    // list refreshes) -- so a single one-shot hide pass gets undone within a
    // frame or two of routine sim traffic. This must be called repeatedly
    // (every idle tick, see updateLights()) for the duration of isolate mode
    // to keep re-asserting the hide, not just once on entry. Safe/cheap to
    // call every frame: re-hiding an already-hidden object is a harmless
    // state re-set, and newly-appeared objects get caught on the next tick.
    void refreshHiddenObjects(const std::set<LLUUID>& exemptIds)
    {
        const S32 count = gObjectList.getNumObjects();
        S32 no_drawable = 0;
        S32 hidden = 0;
        for (S32 i = 0; i < count; ++i)
        {
            LLViewerObject* obj = gObjectList.getObject(i);
            if (!obj || obj->isDead() || obj->asAvatar())
            {
                continue;
            }
            if (exemptIds.count(obj->getID()))
            {
                continue;
            }
            LLVOAvatar* ancestor = obj->getAvatarAncestor();
            if (ancestor && ancestor->isSelf())
            {
                continue;
            }
            // <AS:Chanayane> temporary diagnostic: confirm whether leftover
            // visible objects have a null mDrawable at scan time (hideObject
            // silently no-ops in that case) versus something else clearing
            // FORCE_INVISIBLE after a successful hide.
            if (!obj->mDrawable)
            {
                ++no_drawable;
                if (no_drawable <= 5)
                {
                    LL_INFOS("ASMyLight") << "refreshHiddenObjects: no drawable yet for "
                        << obj->getID() << " pcode=" << (S32)obj->getPCode() << LL_ENDL;
                }
                continue;
            }
            ++hidden;
            // </AS:Chanayane>
            gPipeline.hideObject(obj->getID());
            sHiddenObjectIds.insert(obj->getID());
        }
        // <AS:Chanayane> temporary diagnostic
        static S32 sTickCount = 0;
        if ((++sTickCount % 90) == 1)
        {
            LL_INFOS("ASMyLight") << "refreshHiddenObjects: scanned=" << count
                << " hidden=" << hidden << " no_drawable=" << no_drawable << LL_ENDL;
        }
        // </AS:Chanayane>
    }

    void showOtherObjects()
    {
        // <AS:Chanayane> temporary diagnostic
        LL_INFOS("ASMyLight") << "showOtherObjects: restoring " << sHiddenObjectIds.size() << " objects" << LL_ENDL;
        // </AS:Chanayane>
        for (const LLUUID& id : sHiddenObjectIds)
        {
            gPipeline.restoreHiddenObject(id);
        }
        sHiddenObjectIds.clear();
    }

    void enterBackgroundIsolateRenderState(const std::set<LLUUID>& exemptIds)
    {
        if (sBackgroundOverrodeRenderTypes)
        {
            return;
        }
        for (U32 type : sIsolateRenderTypes)
        {
            if (gPipeline.hasRenderType(type)) gPipeline.toggleRenderType(type);
        }

        hideOtherAvatars();
        refreshHiddenObjects(exemptIds);

        sBackgroundOverrodeRenderTypes = true;
    }

    void exitBackgroundIsolateRenderState()
    {
        if (!sBackgroundOverrodeRenderTypes)
        {
            return;
        }
        // Flip the flag first so refreshBackgroundIsolateHiding() (called
        // from the per-frame idle tick) can never re-hide an object we're
        // about to restore below, regardless of call ordering.
        sBackgroundOverrodeRenderTypes = false;

        for (U32 type : sIsolateRenderTypes)
        {
            if (!gPipeline.hasRenderType(type)) gPipeline.toggleRenderType(type);
        }

        showOtherAvatars();
        showOtherObjects();
    }

    // Called every idle tick while isolate mode is active (see
    // ASFloaterMyLight::updateLights()) to keep re-asserting the hide state
    // against the sim's routine object-update traffic clearing it. No-op if
    // isolate mode isn't currently active.
    void refreshBackgroundIsolateHiding(const std::set<LLUUID>& exemptIds)
    {
        if (!sBackgroundOverrodeRenderTypes)
        {
            return;
        }
        hideOtherAvatars();
        refreshHiddenObjects(exemptIds);
    }
}

ASFloaterMyLight::ASFloaterMyLight(const LLSD& key)
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
    mBackgroundCombo(nullptr),
    mFreezeAnimationsCheck(nullptr),
    mBeaconCheck(nullptr),
    mPresetNameEditor(nullptr),
    mPresetCombo(nullptr),
    mUpdatingControls(false)
{
}

ASFloaterMyLight::~ASFloaterMyLight()
{
    gIdleCallbacks.deleteFunction(onIdle, this);
}

bool ASFloaterMyLight::postBuild()
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
    mBackgroundCombo = getChild<LLComboBox>("as_light_background");
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
    mBackgroundCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBackgroundModeChanged(); });
    mFreezeAnimationsCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFreezeAnimationsChanged(); });
    mBeaconCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBeaconToggled(); });
    getChild<LLUICtrl>("as_light_open_poser")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onOpenPoser(); });

    getChild<LLUICtrl>("as_light_save_preset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSavePreset(); });
    getChild<LLUICtrl>("as_light_load_preset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLoadPreset(); });

    mFreezeAnimationsCheck->setValue(LLMotionController::getCurrentTimeFactor() == 0.f);
    mBeaconCheck->setValue(gSavedSettings.getBOOL("ASRenderLightBeacon"));
    mMasterEnabled = gSavedSettings.getBOOL("ASLightRigMasterEnabled");
    mMasterEnabledCheck->setValue(mMasterEnabled);

    setControlsEnabled(false);
    refreshPresetList();
    loadLightsFromFile(getAutosaveFilename());

    gIdleCallbacks.addFunction(onIdle, this);

    return true;
}

void ASFloaterMyLight::onClose(bool app_quitting)
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
    // quit though, since ASBackgroundIsolate/exitBackgroundIsolateRenderState()
    // rely on static state that would otherwise leave next session's world
    // view stuck black/white with no floater open to explain why.
    if (app_quitting)
    {
        ASBackgroundIsolate::setActive(false, LLColor4::black);
        exitBackgroundIsolateRenderState();
        mBackgroundCombo->setValue("None");
    }

    LLFloater::onClose(app_quitting);
}

// static
void ASFloaterMyLight::renderAllLightBeacons()
{
    if (!gSavedSettings.getBOOL("ASRenderLightBeacon"))
    {
        return;
    }

    ASFloaterMyLight* self = LLFloaterReg::findTypedInstance<ASFloaterMyLight>("as_my_light");
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
        if (!rig->isCreated() || !rig->isEnabled())
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
void ASFloaterMyLight::onIdle(void* userdata)
{
    ASFloaterMyLight* self = static_cast<ASFloaterMyLight*>(userdata);
    self->updateLights();
}

void ASFloaterMyLight::updateLights()
{
    for (auto& rig : mLights)
    {
        rig->updateTransform();
    }

    // Re-assert isolate-mode object hiding every tick -- the sim's routine
    // object-update traffic (terse updates, interest-list refreshes) clears
    // LLDrawable::FORCE_INVISIBLE on affected objects within a frame or two
    // of a one-shot hide, so this has to keep re-hiding for the whole time
    // isolate mode is active, not just once when it's turned on. No-op if
    // isolate mode isn't currently active.
    refreshBackgroundIsolateHiding(getLightRigObjectIds());
}

std::set<LLUUID> ASFloaterMyLight::getLightRigObjectIds() const
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

void ASFloaterMyLight::onAddLight()
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

void ASFloaterMyLight::onDeleteLight()
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

void ASFloaterMyLight::onLightSelected()
{
    LLScrollListItem* item = mLightList->getFirstSelected();
    if (!item)
    {
        return;
    }
    mSelectedLightId = item->getValue().asUUID();
    refreshControlsFromSelectedLight();
}

void ASFloaterMyLight::selectLight(const LLUUID& id)
{
    mSelectedLightId = id;
    mLightList->setSelectedByValue(LLSD(id), true);
    refreshControlsFromSelectedLight();
}

ASLightRig* ASFloaterMyLight::getSelectedLight()
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

void ASFloaterMyLight::onNameChanged()
{
    ASLightRig* rig = getSelectedLight();
    if (!rig)
    {
        return;
    }
    rig->mName = mNameEditor->getText();
    refreshListRow(rig);
}

void ASFloaterMyLight::refreshListRow(const ASLightRig* rig)
{
    LLScrollListItem* item = mLightList->getItem(LLSD(rig->mId));
    if (item)
    {
        item->getColumn(0)->setValue(rig->mName);
    }
}

void ASFloaterMyLight::rebuildList()
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

void ASFloaterMyLight::setControlsEnabled(bool enabled)
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

void ASFloaterMyLight::refreshControlsFromSelectedLight()
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

void ASFloaterMyLight::onDistanceChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mDistance = (F32)mDistanceSlider->getValue().asReal();
    rig->updateTransform();
}

void ASFloaterMyLight::onHeightChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mHeight = (F32)mHeightSlider->getValue().asReal();
    rig->updateTransform();
}

void ASFloaterMyLight::onAzimuthChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mAzimuth = (F32)mAzimuthSlider->getValue().asReal();
    rig->updateTransform();
}

void ASFloaterMyLight::onIntensityChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mIntensity = (F32)mIntensitySlider->getValue().asReal();
    rig->applyParams();
}

void ASFloaterMyLight::onRadiusChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mRadius = (F32)mRadiusSlider->getValue().asReal();
    rig->applyParams();
}

void ASFloaterMyLight::onFalloffChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mFalloff = (F32)mFalloffSlider->getValue().asReal();
    rig->applyParams();
}

void ASFloaterMyLight::onColorChanged()
{
    if (mUpdatingControls) return;
    ASLightRig* rig = getSelectedLight();
    if (!rig) return;
    rig->mColor = LLColor3(mColorSwatch->get());
    rig->applyParams();
}

void ASFloaterMyLight::onPresetHead()
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

void ASFloaterMyLight::onPresetChest()
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

void ASFloaterMyLight::onPresetBack()
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

void ASFloaterMyLight::onMasterEnabledChanged()
{
    mMasterEnabled = mMasterEnabledCheck->get();
    gSavedSettings.setBOOL("ASLightRigMasterEnabled", mMasterEnabled);
    for (auto& rig : mLights)
    {
        rig->setEnabled(mMasterEnabled);
    }
}

void ASFloaterMyLight::onBackgroundModeChanged()
{
    const std::string mode = mBackgroundCombo->getSelectedItemLabel();

    if (mode == "None")
    {
        exitBackgroundIsolateRenderState();
        ASBackgroundIsolate::setActive(false, LLColor4::black);
        return;
    }

    enterBackgroundIsolateRenderState(getLightRigObjectIds());

    const LLColor4 color = (mode == "All White")
        ? LLColor4(1.f, 1.f, 1.f, 1.f)
        : LLColor4(0.f, 0.f, 0.f, 1.f);
    ASBackgroundIsolate::setActive(true, color);
}

void ASFloaterMyLight::onFreezeAnimationsChanged()
{
    set_all_animation_time_factors(mFreezeAnimationsCheck->get() ? 0.f : 1.f);
}

void ASFloaterMyLight::onBeaconToggled()
{
    gSavedSettings.setBOOL("ASRenderLightBeacon", mBeaconCheck->get());
}

void ASFloaterMyLight::onOpenPoser()
{
    LLFloaterReg::toggleInstance("fs_poser");
}

//
// Presets
//

std::string ASFloaterMyLight::getPresetDir() const
{
    std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, PRESET_SUBDIR, "");
    LLFile::mkdir(dir);
    return dir;
}

void ASFloaterMyLight::onSavePreset()
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

void ASFloaterMyLight::onLoadPreset()
{
    const std::string name = mPresetCombo->getSelectedItemLabel();
    if (name.empty())
    {
        return;
    }

    const std::string filename = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, PRESET_SUBDIR, name + ".xml");
    loadLightsFromFile(filename);
}

void ASFloaterMyLight::saveLightsToFile(const std::string& filename) const
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

void ASFloaterMyLight::loadLightsFromFile(const std::string& filename)
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

std::string ASFloaterMyLight::getAutosaveFilename() const
{
    return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "as_my_light_autosave.xml");
}

void ASFloaterMyLight::refreshPresetList()
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
