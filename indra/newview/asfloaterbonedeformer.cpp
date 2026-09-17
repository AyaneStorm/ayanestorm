/**
 * @file asfloaterbonedeformer.cpp
 * @author chanayane@firestorm
 * @brief See asfloaterbonedeformer.h
 */

#include "llviewerprecompiledheaders.h"

#include "asfloaterbonedeformer.h"

#include "asbonedeformerbaker.h"
#include "asscrollingpaneljointdeformer.h"
#include "fsposeranimator.h"
#include "llagent.h"
#include "llagentwearables.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llfilesystem.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "lljoint.h"
#include "llmodel.h"
#include "llnotecard.h"
#include "llnotificationsutil.h"
#include "llpreviewnotecard.h"
#include "llscrollingpanellist.h"
#include "lltexteditor.h"
#include "lltextbox.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
    const LLUUID EDIT_POSE_ID("2029d88f-4efc-d72d-18d1-274caf9e9bc0");
    const LLUUID PREVIEW_OVERRIDE_ID("ffffffff-ffff-ffff-ffff-ffffffffffff");
    constexpr const char* DEFORMER_NAME_PREFIX = "AS Deformer -";

    constexpr const char* CONFIG_HEADER = "AYANESTORM_SHAPE_DEFORMER 1";

    struct ASNotecardLoadRequest
    {
        LLHandle<LLFloater> mFloater;
    };
}

ASFloaterBoneDeformer::ASFloaterBoneDeformer(const LLSD& key)
:   LLFloater(key),
    mJointTable(std::make_unique<FSPoserAnimator>())
{
    // A stable maximum UUID keeps this editable preview above the worn source deformer.
    mSessionFakeMeshId = PREVIEW_OVERRIDE_ID;
}

ASFloaterBoneDeformer::~ASFloaterBoneDeformer()
{
    clearOverrides(false);
}

bool ASFloaterBoneDeformer::postBuild()
{
    mCategoryLists[BODY] = getChild<LLScrollingPanelList>("body_joint_list");
    mCategoryLists[FACE] = getChild<LLScrollingPanelList>("face_joint_list");
    mCategoryLists[HANDS] = getChild<LLScrollingPanelList>("hands_joint_list");
    mCategoryLists[MISC] = getChild<LLScrollingPanelList>("misc_joint_list");
    mCategoryLists[COL_VOLUMES] = getChild<LLScrollingPanelList>("collision_joint_list");

    mUndoButton = getChild<LLButton>("undo");
    mUndoButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUndo(); });
    mRedoButton = getChild<LLButton>("redo");
    mRedoButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRedo(); });
    getChild<LLButton>("reset_all")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { resetAll(); });
    getChild<LLButton>("toggle_edit_pose")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onToggleEditPose(); });
    getChild<LLButton>("show_shape_in_inventory")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onShowShapeInInventory(); });
    getChild<LLButton>("bake_upload")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onBakeAndUpload(); });
    getChild<LLButton>("bypass_all")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onToggleBypassAll(); });
    getChild<LLButton>("bypass_scales")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onToggleBypassScales(); });
    getChild<LLButton>("export_notecard")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onExportNotecard(); });
    getChild<LLButton>("import_notecard")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onImportNotecard(); });
    getChild<LLButton>("load_worn")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onLoadWornDeformer(); });
    getChild<LLCheckBoxCtrl>("show_scales")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onToggleShowScales(); });
    mBaker = std::make_shared<ASBoneDeformerBaker>(
        [this]() { onBakedOverridesApplied(); }, [this]() { updateButtons(); });

    buildJointRows();
    refreshShapeInfo();
    updateButtons();
    return true;
}

void ASFloaterBoneDeformer::draw()
{
    LLFloater::draw();
    if (!mPreviewReloadPromptPending && previewOverridesNeedReload())
    {
        showPreviewReloadPrompt();
    }
}

void ASFloaterBoneDeformer::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    if (!mAvatar || mAvatar != gAgentAvatarp || mCategoryLists.empty())
    {
        // A replaced self-avatar invalidates every cached LLJoint pointer.
        mOverrides.clear();
        mBypassedJoints.clear();
        mUndoHistory.clear();
        mRedoHistory.clear();
        mSessionFakeMeshId = PREVIEW_OVERRIDE_ID;
        buildJointRows();
    }
    for (auto& entry : mCategoryLists)
    {
        if (entry.second)
        {
            entry.second->updatePanels(true);
        }
    }
    refreshShapeInfo();
    updateButtons();
}

void ASFloaterBoneDeformer::onClose(bool app_quitting)
{
    // The preview is intentionally scoped to the open floater session.
    clearOverrides(true);
    mBypassedJoints.clear();
    mPreviewReloadPromptPending = false;
    mUndoHistory.clear();
    mRedoHistory.clear();
    updateButtons();
    if (mEditPoseManaged)
    {
        onToggleEditPose();
    }
    LLFloater::onClose(app_quitting);
}

void ASFloaterBoneDeformer::buildJointRows()
{
    clearJointRows();
    mJointCategories.clear();
    mAvatar = isAgentAvatarValid() ? gAgentAvatarp : nullptr;
    if (!mAvatar)
    {
        return;
    }

    const auto add_joint_row = [this](const std::string& joint_name, S32 category)
    {
        LLJoint* joint = mAvatar->getJoint(joint_name);
        if (!joint)
        {
            LL_WARNS("ASBoneDeformer") << "Joint not found: " << joint_name << LL_ENDL;
            return;
        }

        auto list_it = mCategoryLists.find(category);
        if (list_it == mCategoryLists.end() || !list_it->second)
        {
            return;
        }

        LLPanel::Params params;
        params.name(joint_name);
        mJointCategories[joint] = category;
        list_it->second->addPanel(new ASScrollingPanelJointDeformer(params, this, joint), true);
    };

    for (const FSPoserAnimator::FSPoserJoint& metadata : mJointTable->PoserJoints)
    {
        const S32 category = metadata.boneType() == WHOLEAVATAR ? BODY : metadata.boneType();
        add_joint_row(metadata.jointName(), category);
    }

    // The poser table has 155 of avatar_skeleton.xml's 159 override targets.
    // Keep its ordering/categories, then add its four omissions explicitly.
    add_joint_row("mFaceEyeAltLeft", FACE);
    add_joint_row("mFaceEyeAltRight", FACE);
    add_joint_row("mFaceJawShaper", FACE);
    add_joint_row("LOWER_BACK", COL_VOLUMES);
}

void ASFloaterBoneDeformer::clearJointRows()
{
    for (auto& entry : mCategoryLists)
    {
        if (entry.second)
        {
            entry.second->clearPanels();
        }
    }
}

ASJointOverrideState ASFloaterBoneDeformer::getOverrideState(LLJoint* joint) const
{
    auto found = mOverrides.find(joint);
    if (found != mOverrides.end())
    {
        return found->second;
    }

    ASJointOverrideState state;
    if (joint)
    {
        // Reflect the live shape/attachment result until this editor owns an override.
        state.mPositionOffset = joint->getPosition() - joint->getDefaultPosition();
        state.mScale = joint->getScale();
        state.mBasePositionOffset = state.mPositionOffset;
        state.mBaseScale = state.mScale;
    }
    return state;
}

void ASFloaterBoneDeformer::setPositionOffset(LLJoint* joint, const LLVector3& offset)
{
    if (!joint || mAvatar != gAgentAvatarp)
    {
        return;
    }

    recordUndoState();
    auto found = mOverrides.find(joint);
    if (found == mOverrides.end())
    {
        found = mOverrides.emplace(joint, getOverrideState(joint)).first;
    }
    ASJointOverrideState& state = found->second;
    state.mPositionOffset = offset;
    state.mHasPosition = true;

    if (!isJointBypassed(joint))
    {
        bool active_override_changed = false;
        joint->addAttachmentPosOverride(joint->getDefaultPosition() + offset,
                                        mSessionFakeMeshId, "AS bone deformer",
                                        active_override_changed);
        refreshAvatarAfterPositionChange(joint, active_override_changed);
    }
    updateButtons();
}

void ASFloaterBoneDeformer::setScale(LLJoint* joint, const LLVector3& scale)
{
    if (!joint || mAvatar != gAgentAvatarp)
    {
        return;
    }

    recordUndoState();
    auto found = mOverrides.find(joint);
    if (found == mOverrides.end())
    {
        found = mOverrides.emplace(joint, getOverrideState(joint)).first;
    }
    ASJointOverrideState& state = found->second;
    state.mScale = scale;
    state.mHasScale = true;
    if (!isJointBypassed(joint) && !mBypassScales)
    {
        joint->addAttachmentScaleOverride(scale, mSessionFakeMeshId, "AS bone deformer");
    }
    updateButtons();
}

void ASFloaterBoneDeformer::resetPositionAxis(LLJoint* joint, S32 axis)
{
    auto found = mOverrides.find(joint);
    if (found == mOverrides.end() || !found->second.mHasPosition || axis < VX || axis > VZ)
    {
        return;
    }

    recordUndoState();
    ASJointOverrideState& state = found->second;
    state.mPositionOffset.mV[axis] = state.mBasePositionOffset.mV[axis];
    if (state.mPositionOffset == state.mBasePositionOffset)
    {
        removePositionOverride(joint);
        state.mHasPosition = false;
    }
    else
    {
        if (!isJointBypassed(joint))
        {
            bool changed = false;
            joint->addAttachmentPosOverride(joint->getDefaultPosition() + state.mPositionOffset,
                                            mSessionFakeMeshId, "AS bone deformer", changed);
            refreshAvatarAfterPositionChange(joint, changed);
        }
    }
    if (!state.mHasPosition && !state.mHasScale)
    {
        mOverrides.erase(found);
        mBypassedJoints.erase(joint);
    }
    updateButtons();
}

void ASFloaterBoneDeformer::resetScaleAxis(LLJoint* joint, S32 axis)
{
    auto found = mOverrides.find(joint);
    if (found == mOverrides.end() || !found->second.mHasScale || axis < VX || axis > VZ)
    {
        return;
    }

    recordUndoState();
    ASJointOverrideState& state = found->second;
    state.mScale.mV[axis] = state.mBaseScale.mV[axis];
    if (state.mScale == state.mBaseScale)
    {
        removeScaleOverride(joint);
        state.mHasScale = false;
    }
    else
    {
        if (!isJointBypassed(joint) && !mBypassScales)
        {
            joint->addAttachmentScaleOverride(state.mScale, mSessionFakeMeshId, "AS bone deformer");
        }
    }
    if (!state.mHasPosition && !state.mHasScale)
    {
        mOverrides.erase(found);
        mBypassedJoints.erase(joint);
    }
    updateButtons();
}

void ASFloaterBoneDeformer::removePositionOverride(LLJoint* joint)
{
    bool active_override_changed = false;
    joint->removeAttachmentPosOverride(mSessionFakeMeshId, "AS bone deformer",
                                       active_override_changed);
    refreshAvatarAfterPositionChange(joint, active_override_changed);
}

void ASFloaterBoneDeformer::removeScaleOverride(LLJoint* joint)
{
    joint->removeAttachmentScaleOverride(mSessionFakeMeshId, "AS bone deformer");
}

void ASFloaterBoneDeformer::applyPreviewOverride(LLJoint* joint, const ASJointOverrideState& state)
{
    if (!joint || isJointBypassed(joint))
    {
        return;
    }
    if (state.mHasPosition)
    {
        bool changed = false;
        joint->addAttachmentPosOverride(joint->getDefaultPosition() + state.mPositionOffset,
                                        mSessionFakeMeshId, "AS bone deformer", changed);
        refreshAvatarAfterPositionChange(joint, changed);
    }
    if (state.mHasScale && !mBypassScales)
    {
        joint->addAttachmentScaleOverride(state.mScale, mSessionFakeMeshId, "AS bone deformer");
    }
}

void ASFloaterBoneDeformer::refreshPreviewOverrides()
{
    if (mAvatar != gAgentAvatarp)
    {
        return;
    }
    for (const auto& entry : mOverrides)
    {
        if (entry.second.mHasPosition)
        {
            removePositionOverride(entry.first);
        }
        if (entry.second.mHasScale)
        {
            removeScaleOverride(entry.first);
        }
    }
    for (const auto& entry : mOverrides)
    {
        applyPreviewOverride(entry.first, entry.second);
    }
}

bool ASFloaterBoneDeformer::previewOverridesNeedReload() const
{
    if (mAvatar != gAgentAvatarp || mBypassAll)
    {
        return false;
    }
    for (const auto& entry : mOverrides)
    {
        LLJoint* joint = entry.first;
        if (isJointBypassed(joint))
        {
            continue;
        }
        if (entry.second.mHasPosition &&
            joint->m_attachmentPosOverrides.getMap().find(mSessionFakeMeshId) ==
                joint->m_attachmentPosOverrides.getMap().end())
        {
            return true;
        }
        if (entry.second.mHasScale && !mBypassScales &&
            joint->m_attachmentScaleOverrides.getMap().find(mSessionFakeMeshId) ==
                joint->m_attachmentScaleOverrides.getMap().end())
        {
            return true;
        }
    }
    return false;
}

void ASFloaterBoneDeformer::showPreviewReloadPrompt()
{
    mPreviewReloadPromptPending = true;
    refreshShapeInfo();
    const LLHandle<LLFloater> handle = getHandle();
    LLNotificationsUtil::add("ASBoneDeformerShapeChanged", LLSD(), LLSD(),
        [handle](const LLSD& notification, const LLSD& response)
        {
            ASFloaterBoneDeformer* floater =
                dynamic_cast<ASFloaterBoneDeformer*>(handle.get());
            if (!floater)
            {
                return false;
            }
            floater->mPreviewReloadPromptPending = false;
            if (floater->getVisible() &&
                LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                floater->refreshShapeInfo();
                floater->refreshPreviewOverrides();
            }
            return false;
        });
}

void ASFloaterBoneDeformer::resetJoint(LLJoint* joint)
{
    auto found = mOverrides.find(joint);
    if (found == mOverrides.end() || mAvatar != gAgentAvatarp)
    {
        return;
    }

    recordUndoState();
    if (found->second.mHasPosition)
    {
        removePositionOverride(joint);
    }
    if (found->second.mHasScale)
    {
        removeScaleOverride(joint);
    }
    mOverrides.erase(found);
    mBypassedJoints.erase(joint);
    updateButtons();
}

void ASFloaterBoneDeformer::resetAll()
{
    if (mOverrides.empty())
    {
        return;
    }
    recordUndoState();
    mBypassedJoints.clear();
    clearOverrides(true);
}

bool ASFloaterBoneDeformer::isJointBypassed(LLJoint* joint) const
{
    return mBypassAll || mBypassedJoints.count(joint) != 0;
}

bool ASFloaterBoneDeformer::isJointExplicitlyBypassed(LLJoint* joint) const
{
    return mBypassedJoints.count(joint) != 0;
}

void ASFloaterBoneDeformer::setJointBypassed(LLJoint* joint, bool bypassed)
{
    if (!joint)
    {
        return;
    }
    if (bypassed)
    {
        mBypassedJoints.insert(joint);
    }
    else
    {
        mBypassedJoints.erase(joint);
    }
    refreshPreviewOverrides();
}

void ASFloaterBoneDeformer::clearOverrides(bool update_panels)
{
    if (mAvatar == gAgentAvatarp)
    {
        for (const auto& entry : mOverrides)
        {
            if (entry.second.mHasPosition)
            {
                removePositionOverride(entry.first);
            }
            if (entry.second.mHasScale)
            {
                removeScaleOverride(entry.first);
            }
        }
    }
    mOverrides.clear();

    if (update_panels)
    {
        for (auto& entry : mCategoryLists)
        {
            if (entry.second)
            {
                entry.second->updatePanels(true);
            }
        }
    }
    updateButtons();
}

void ASFloaterBoneDeformer::beginUndoTransaction()
{
    mUndoTransactionOpen = true;
    mUndoTransactionRecorded = false;
}

void ASFloaterBoneDeformer::endUndoTransaction()
{
    mUndoTransactionOpen = false;
    mUndoTransactionRecorded = false;
}

void ASFloaterBoneDeformer::recordUndoState()
{
    if (mApplyingHistory || (mUndoTransactionOpen && mUndoTransactionRecorded))
    {
        return;
    }

    mUndoHistory.push_back(mOverrides);
    trimHistory(mUndoHistory);
    mRedoHistory.clear();
    mUndoTransactionRecorded = mUndoTransactionOpen;
}

void ASFloaterBoneDeformer::onUndo()
{
    if (mUndoHistory.empty() || mAvatar != gAgentAvatarp)
    {
        return;
    }

    const override_map_t previous = mUndoHistory.back();
    mUndoHistory.pop_back();
    mRedoHistory.push_back(mOverrides);
    trimHistory(mRedoHistory);
    applyOverrideState(previous);
}

void ASFloaterBoneDeformer::onRedo()
{
    if (mRedoHistory.empty() || mAvatar != gAgentAvatarp)
    {
        return;
    }

    const override_map_t next = mRedoHistory.back();
    mRedoHistory.pop_back();
    mUndoHistory.push_back(mOverrides);
    trimHistory(mUndoHistory);
    applyOverrideState(next);
}

void ASFloaterBoneDeformer::trimHistory(std::vector<override_map_t>& history)
{
    const S32 limit = llclamp(gSavedSettings.getS32("ASBoneDeformerUndoLevels"), 1, 99);
    if (history.size() > static_cast<size_t>(limit))
    {
        history.erase(history.begin(), history.begin() + (history.size() - limit));
    }
}

void ASFloaterBoneDeformer::applyOverrideState(const override_map_t& state)
{
    mApplyingHistory = true;
    clearOverrides(false);
    mOverrides = state;
    for (auto it = mBypassedJoints.begin(); it != mBypassedJoints.end();)
    {
        if (mOverrides.find(*it) == mOverrides.end())
        {
            it = mBypassedJoints.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (const auto& entry : mOverrides)
    {
        applyPreviewOverride(entry.first, entry.second);
    }
    mApplyingHistory = false;

    for (auto& entry : mCategoryLists)
    {
        if (entry.second)
        {
            entry.second->updatePanels(true);
        }
    }
    updateButtons();
}

void ASFloaterBoneDeformer::updateButtons()
{
    if (mUndoButton)
    {
        mUndoButton->setEnabled(!mUndoHistory.empty());
    }
    if (mRedoButton)
    {
        mRedoButton->setEnabled(!mRedoHistory.empty());
    }
    if (LLButton* reset = findChild<LLButton>("reset_all"))
    {
        reset->setEnabled(!mOverrides.empty());
    }
    if (LLButton* bake = findChild<LLButton>("bake_upload"))
    {
        const bool has_edits = std::any_of(
            mOverrides.begin(), mOverrides.end(),
            [](const auto& entry)
            {
                return entry.second.mHasPosition || entry.second.mHasScale;
            });
        bake->setEnabled(has_edits && (!mBaker || !mBaker->isBusy()));
    }
}

void ASFloaterBoneDeformer::refreshShapeInfo()
{
    mShapeItemId = gAgentWearables.getWearableItemID(LLWearableType::WT_SHAPE, 0);
    const LLViewerInventoryItem* shape_item = gInventory.getItem(mShapeItemId);
    getChild<LLTextBox>("shape_notice")->setTextArg(
        "[SHAPE_NAME]", shape_item ? shape_item->getName() : getString("unknown_shape"));
    getChild<LLButton>("show_shape_in_inventory")->setEnabled(shape_item != nullptr);
}

void ASFloaterBoneDeformer::onShowShapeInInventory()
{
    if (mShapeItemId.notNull() && gInventory.getItem(mShapeItemId))
    {
        LLInventoryPanel::openInventoryPanelAndSetSelection(true, mShapeItemId, true);
    }
}

void ASFloaterBoneDeformer::onBakeAndUpload()
{
    if (!mBaker || mBaker->isBusy() || mAvatar != gAgentAvatarp)
    {
        return;
    }

    if (mBypassAll || mBypassScales || !mBypassedJoints.empty())
    {
        LLNotificationsUtil::add("ASBoneDeformerBypassBakeWarning", LLSD(), LLSD(),
            boost::bind(&ASFloaterBoneDeformer::onBypassBakeWarning, this, _1, _2));
        return;
    }
    bakeAndUpload();
}

bool ASFloaterBoneDeformer::onBypassBakeWarning(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
    {
        bakeAndUpload();
    }
    return false;
}

void ASFloaterBoneDeformer::bakeAndUpload()
{
    if (!mBaker || mBaker->isBusy() || mAvatar != gAgentAvatarp)
    {
        return;
    }

    std::vector<ASBoneDeformerBakeJoint> joints;
    for (const auto& entry : mOverrides)
    {
        if (entry.second.mHasPosition || entry.second.mHasScale)
        {
            joints.push_back({
                entry.first,
                entry.first->getDefaultPosition() + entry.second.mPositionOffset,
                entry.second.mScale,
                entry.second.mHasPosition,
                entry.second.mHasScale
            });
        }
    }

    const LLViewerInventoryItem* shape_item = gInventory.getItem(mShapeItemId);
    if (joints.empty() || !shape_item ||
        !mBaker->start(joints, shape_item->getName()))
    {
        LLNotificationsUtil::add("ASBoneDeformerBakeUnavailable");
    }
}

void ASFloaterBoneDeformer::onBakedOverridesApplied()
{
    if (mAvatar != gAgentAvatarp)
    {
        return;
    }
    for (auto it = mOverrides.begin(); it != mOverrides.end();)
    {
        if (it->second.mHasPosition)
        {
            removePositionOverride(it->first);
            it->second.mHasPosition = false;
        }
        if (it->second.mHasScale)
        {
            removeScaleOverride(it->first);
            it->second.mHasScale = false;
        }
        it = mOverrides.erase(it);
    }
    mUndoHistory.clear();
    mRedoHistory.clear();
    mBypassedJoints.clear();
    for (auto& entry : mCategoryLists)
    {
        if (entry.second)
        {
            entry.second->updatePanels(true);
        }
    }
    updateButtons();
}

void ASFloaterBoneDeformer::onToggleShowScales()
{
    mShowScales = getChild<LLCheckBoxCtrl>("show_scales")->getValue().asBoolean();
    for (auto& entry : mCategoryLists)
    {
        if (entry.second)
        {
            entry.second->updatePanels(true);
        }
    }
}

void ASFloaterBoneDeformer::onToggleBypassAll()
{
    mBypassAll = getChild<LLButton>("bypass_all")->getToggleState();
    refreshPreviewOverrides();
}

void ASFloaterBoneDeformer::onToggleBypassScales()
{
    mBypassScales = getChild<LLButton>("bypass_scales")->getToggleState();
    refreshPreviewOverrides();
}

void ASFloaterBoneDeformer::onLoadWornDeformer()
{
    if (!isAgentAvatarValid() || mAvatar != gAgentAvatarp)
    {
        return;
    }

    std::vector<LLViewerObject*> matches;
    for (const auto& attachment_entry : gAgentAvatarp->mAttachmentPoints)
    {
        LLViewerJointAttachment* attachment = attachment_entry.second;
        if (!attachment)
        {
            continue;
        }
        for (const LLPointer<LLViewerObject>& object_ptr : attachment->mAttachedObjects)
        {
            LLViewerObject* object = object_ptr.get();
            const LLViewerInventoryItem* item = object
                ? gInventory.getItem(object->getAttachmentItemID()) : nullptr;
            if (item && item->getName().compare(0, std::strlen(DEFORMER_NAME_PREFIX),
                                                DEFORMER_NAME_PREFIX) == 0)
            {
                matches.push_back(object);
            }
        }
    }

    LLSD args;
    if (matches.empty())
    {
        args["REASON"] = "No worn attachment has an inventory name beginning with \"AS Deformer -\".";
        LLNotificationsUtil::add("ASBoneDeformerLoadWornError", args);
        return;
    }
    if (matches.size() > 1)
    {
        args["REASON"] = "More than one matching deformer is worn. Detach all but the one to load.";
        LLNotificationsUtil::add("ASBoneDeformerLoadWornError", args);
        return;
    }

    std::vector<const LLMeshSkinInfo*> skins;
    const auto collect_skins = [&skins](LLViewerObject* root)
    {
        std::vector<LLViewerObject*> pending{ root };
        while (!pending.empty())
        {
            LLViewerObject* object = pending.back();
            pending.pop_back();
            if (LLVOVolume* volume = dynamic_cast<LLVOVolume*>(object))
            {
                if (volume->isMesh() && volume->getSkinInfo())
                {
                    skins.push_back(volume->getSkinInfo());
                }
            }
            for (LLViewerObject* child : object->getChildren())
            {
                if (child)
                {
                    pending.push_back(child);
                }
            }
        }
    };
    collect_skins(matches.front());
    if (skins.empty())
    {
        args["REASON"] = "The matching attachment's mesh or skin data is not loaded yet. Try again after it finishes loading.";
        LLNotificationsUtil::add("ASBoneDeformerLoadWornError", args);
        return;
    }

    override_map_t loaded;
    for (const LLMeshSkinInfo* skin : skins)
    {
        const size_t joint_count = skin->mJointNames.size();
        if (skin->mAlternateBindMatrix.size() != joint_count)
        {
            continue;
        }
        const bool has_as_scales = skin->mASJointScaleVersion == 1 &&
                                   skin->mASJointScaleOverrides.size() == joint_count;
        for (size_t index = 0; index < joint_count; ++index)
        {
            LLJoint* joint = mAvatar->getJoint(skin->mJointNames[index]);
            if (!joint || mJointCategories.find(joint) == mJointCategories.end())
            {
                continue;
            }

            const LLVector3 target_position(skin->mAlternateBindMatrix[index].getTranslation());
            const bool has_position = joint->aboveJointPosThreshold(target_position);
            const LLVector3 scale = has_as_scales
                ? skin->mASJointScaleOverrides[index] : LLVector3::zero;
            const bool has_scale = scale != LLVector3::zero;
            if (!has_position && !has_scale)
            {
                continue;
            }

            auto inserted = loaded.emplace(joint, ASJointOverrideState());
            ASJointOverrideState& state = inserted.first->second;
            if (inserted.second)
            {
                state.mBasePositionOffset = joint->m_attachmentPosOverrides.count()
                    ? joint->m_posBeforeOverrides - joint->getDefaultPosition()
                    : joint->getPosition() - joint->getDefaultPosition();
                state.mBaseScale = joint->m_attachmentScaleOverrides.count()
                    ? joint->m_scaleBeforeOverrides : joint->getScale();
                state.mPositionOffset = state.mBasePositionOffset;
                state.mScale = state.mBaseScale;
            }
            if (has_position)
            {
                state.mPositionOffset = target_position - joint->getDefaultPosition();
                state.mHasPosition = true;
            }
            if (has_scale)
            {
                state.mScale = scale;
                state.mHasScale = true;
            }
        }
    }

    if (loaded.empty())
    {
        args["REASON"] = "The matching attachment contains no readable position or AyaneStorm scale overrides.";
        LLNotificationsUtil::add("ASBoneDeformerLoadWornError", args);
        return;
    }

    recordUndoState();
    applyOverrideState(loaded);
    args["COUNT"] = static_cast<S32>(loaded.size());
    mPreviewReloadPromptPending = true;
    const LLHandle<LLFloater> handle = getHandle();
    LLNotificationsUtil::add("ASBoneDeformerLoadWornSucceeded", args, LLSD(),
        [handle](const LLSD& notification, const LLSD& response)
        {
            ASFloaterBoneDeformer* floater =
                dynamic_cast<ASFloaterBoneDeformer*>(handle.get());
            if (floater)
            {
                floater->mPreviewReloadPromptPending = false;
                if (floater->getVisible() &&
                    LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                {
                    floater->refreshPreviewOverrides();
                }
            }
            return false;
        });
}

std::string ASFloaterBoneDeformer::serializeConfig() const
{
    std::ostringstream output;
    output << CONFIG_HEADER << '\n';
    output << "# One joint per line: joint NAME HAS_POSITION PX PY PZ HAS_SCALE SX SY SZ\n";
    output << std::setprecision(std::numeric_limits<F32>::max_digits10);
    for (const auto& entry : mOverrides)
    {
        const ASJointOverrideState& state = entry.second;
        if (!state.mHasPosition && !state.mHasScale)
        {
            continue;
        }
        output << "joint " << entry.first->getName() << ' '
               << (state.mHasPosition ? 1 : 0) << ' '
               << state.mPositionOffset.mV[VX] << ' ' << state.mPositionOffset.mV[VY] << ' '
               << state.mPositionOffset.mV[VZ] << ' ' << (state.mHasScale ? 1 : 0) << ' '
               << state.mScale.mV[VX] << ' ' << state.mScale.mV[VY] << ' '
               << state.mScale.mV[VZ] << '\n';
    }
    return output.str();
}

bool ASFloaterBoneDeformer::importConfig(const std::string& text, std::string& error)
{
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line))
    {
        error = "Line 1: the notecard is empty.";
        return false;
    }
    if (!line.empty() && line.back() == '\r')
    {
        line.pop_back();
    }
    if (line != CONFIG_HEADER)
    {
        error = "Line 1: expected \"" + std::string(CONFIG_HEADER) + "\".";
        return false;
    }

    std::map<std::string, LLJoint*> joints_by_name;
    for (const auto& entry : mJointCategories)
    {
        joints_by_name[entry.first->getName()] = entry.first;
    }

    override_map_t imported;
    S32 line_number = 1;
    while (std::getline(input, line))
    {
        ++line_number;
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        std::istringstream fields(line);
        std::string keyword;
        std::string joint_name;
        S32 has_position = 0;
        S32 has_scale = 0;
        F32 px, py, pz, sx, sy, sz;
        if (!(fields >> keyword >> joint_name >> has_position >> px >> py >> pz
                     >> has_scale >> sx >> sy >> sz) || keyword != "joint")
        {
            error = llformat("Line %d: expected joint NAME HAS_POSITION PX PY PZ HAS_SCALE SX SY SZ.", line_number);
            return false;
        }
        std::string extra;
        if (fields >> extra)
        {
            error = llformat("Line %d: unexpected extra field \"%s\".", line_number, extra.c_str());
            return false;
        }
        if ((has_position != 0 && has_position != 1) || (has_scale != 0 && has_scale != 1))
        {
            error = llformat("Line %d: HAS_POSITION and HAS_SCALE must be 0 or 1.", line_number);
            return false;
        }
        auto joint = joints_by_name.find(joint_name);
        if (joint == joints_by_name.end())
        {
            error = llformat("Line %d: unknown joint \"%s\".", line_number, joint_name.c_str());
            return false;
        }
        if (imported.count(joint->second))
        {
            error = llformat("Line %d: duplicate joint \"%s\".", line_number, joint_name.c_str());
            return false;
        }
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz) ||
            !std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz))
        {
            error = llformat("Line %d: all values must be finite numbers.", line_number);
            return false;
        }
        if (has_position && (px < -0.25f || px > 0.25f || py < -0.25f || py > 0.25f || pz < -0.25f || pz > 0.25f))
        {
            error = llformat("Line %d: position values must be between -0.25 and 0.25.", line_number);
            return false;
        }
        if (has_scale && (sx < 0.000001f || sx > 10.f || sy < 0.000001f || sy > 10.f || sz < 0.000001f || sz > 10.f))
        {
            error = llformat("Line %d: scale values must be between 0.000001 and 10.", line_number);
            return false;
        }

        ASJointOverrideState state = getOverrideState(joint->second);
        state.mPositionOffset.set(px, py, pz);
        state.mScale.set(sx, sy, sz);
        state.mHasPosition = has_position != 0;
        state.mHasScale = has_scale != 0;
        if (state.mHasPosition || state.mHasScale)
        {
            imported[joint->second] = state;
        }
    }

    if (imported.empty())
    {
        error = "The notecard contains no active joint edits.";
        return false;
    }

    recordUndoState();
    applyOverrideState(imported);
    return true;
}

void ASFloaterBoneDeformer::onExportNotecard()
{
    if (mOverrides.empty())
    {
        LLSD args;
        args["REASON"] = "There are no edits to export.";
        LLNotificationsUtil::add("ASBoneDeformerConfigError", args);
        return;
    }

    const std::string config = serializeConfig();
    const LLHandle<LLFloater> handle = getHandle();
    menu_create_inventory_item(nullptr, LLUUID::null, LLSD("notecard"), LLUUID::null,
        [handle, config](const LLUUID& item_id)
        {
            if (!handle.get())
            {
                return;
            }
            LLPreviewNotecard* preview = LLFloaterReg::showTypedInstance<LLPreviewNotecard>(
                "preview_notecard", LLSD(item_id), TAKE_FOCUS_YES);
            if (!preview || !preview->getEditor())
            {
                return;
            }
            preview->getEditor()->setText(LLStringUtil::null);
            preview->getEditor()->insertText(config);
            preview->saveItem();
            LLSD updates;
            updates["name"] = "AyaneStorm Shape Transformer";
            updates["desc"] = "Editable Advanced Shape Deformer configuration";
            update_inventory_item(item_id, updates, nullptr);
            LLInventoryPanel::openInventoryPanelAndSetSelection(true, item_id, true);
        });
}

void ASFloaterBoneDeformer::onImportNotecard()
{
    LLInventoryPanel* panel = LLInventoryPanel::getActiveInventoryPanel(false, true);
    if (!panel || panel->getSelectedItems().size() != 1)
    {
        LLNotificationsUtil::add("ASBoneDeformerSelectNotecard");
        return;
    }

    LLFolderViewItem* selected = *panel->getSelectedItems().begin();
    const LLFolderViewModelItemInventory* model_item =
        static_cast<const LLFolderViewModelItemInventory*>(selected->getViewModelItem());
    const LLViewerInventoryItem* item = model_item ? gInventory.getItem(model_item->getUUID()) : nullptr;
    if (!item || item->getType() != LLAssetType::AT_NOTECARD || item->getAssetUUID().isNull() || !gAssetStorage)
    {
        LLNotificationsUtil::add("ASBoneDeformerSelectNotecard");
        return;
    }

    ASNotecardLoadRequest* request = new ASNotecardLoadRequest{ getHandle() };
    gAssetStorage->getInvItemAsset(LLHost(), gAgent.getID(), gAgent.getSessionID(),
        item->getPermissions().getOwner(), LLUUID::null, item->getUUID(), item->getAssetUUID(),
        LLAssetType::AT_NOTECARD, &ASFloaterBoneDeformer::onNotecardLoadComplete,
        request, true);
}

void ASFloaterBoneDeformer::onNotecardLoadComplete(const LLUUID& asset_uuid, LLAssetType::EType type,
                                                    void* user_data, S32 status, LLExtStat)
{
    std::unique_ptr<ASNotecardLoadRequest> request(static_cast<ASNotecardLoadRequest*>(user_data));
    ASFloaterBoneDeformer* floater = dynamic_cast<ASFloaterBoneDeformer*>(request->mFloater.get());
    if (!floater)
    {
        return;
    }

    std::string error;
    if (status == 0)
    {
        LLFileSystem file(asset_uuid, type, LLFileSystem::READ);
        std::vector<char> buffer(file.getSize());
        if (!buffer.empty())
        {
            file.read(reinterpret_cast<U8*>(buffer.data()), static_cast<S32>(buffer.size()));
        }
        std::string raw(buffer.begin(), buffer.end());
        std::istringstream stream(raw);
        LLNotecard notecard;
        if (!notecard.importStream(stream))
        {
            error = "The selected notecard asset could not be decoded.";
        }
        else if (floater->importConfig(notecard.getText(), error))
        {
            LLNotificationsUtil::add("ASBoneDeformerConfigImported");
            return;
        }
    }
    else
    {
        error = llformat("The notecard could not be loaded (asset error %d).", status);
    }

    LLSD args;
    args["REASON"] = error;
    LLNotificationsUtil::add("ASBoneDeformerConfigError", args);
}

void ASFloaterBoneDeformer::onToggleEditPose()
{
    if (mEditPoseManaged)
    {
        if (LLFloaterReg::instanceVisible("fs_posestand"))
        {
            LLFloaterReg::hideInstance("fs_posestand");
        }
        gSavedSettings.setString("FSPoseStandLastSelectedPose", mPreviousPoseStandSelection);
        if (mPoseStandWasVisible)
        {
            LLFloaterReg::showInstance("fs_posestand");
        }
        mEditPoseManaged = false;
        mPoseStandWasVisible = false;
        return;
    }

    mPoseStandWasVisible = LLFloaterReg::instanceVisible("fs_posestand");
    mPreviousPoseStandSelection = gSavedSettings.getString("FSPoseStandLastSelectedPose");
    gSavedSettings.setString("FSPoseStandLastSelectedPose", EDIT_POSE_ID.asString());
    LLFloaterReg::showInstance("fs_posestand");
    mEditPoseManaged = true;
}

void ASFloaterBoneDeformer::refreshAvatarAfterPositionChange(LLJoint* joint,
                                                             bool active_override_changed)
{
    if (active_override_changed && joint->getName() == "mPelvis" && mAvatar)
    {
        mAvatar->postPelvisSetRecalc();
    }
}
