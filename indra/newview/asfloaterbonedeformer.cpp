/**
 * @file asfloaterbonedeformer.cpp
 * @author chanayane@firestorm
 * @brief See asfloaterbonedeformer.h
 */

#include "llviewerprecompiledheaders.h"

#include "asfloaterbonedeformer.h"

#include "asscrollingpaneljointdeformer.h"
#include "fsposeranimator.h"
#include "llagent.h"
#include "llbutton.h"
#include "llfloaterreg.h"
#include "lljoint.h"
#include "llscrollingpanellist.h"
#include "llvoavatarself.h"
#include "llviewercontrol.h"

namespace
{
    const LLUUID EDIT_POSE_ID("2029d88f-4efc-d72d-18d1-274caf9e9bc0");
}

ASFloaterBoneDeformer::ASFloaterBoneDeformer(const LLSD& key)
:   LLFloater(key),
    mJointTable(std::make_unique<FSPoserAnimator>())
{
    mSessionFakeMeshId.generate();
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

    buildJointRows();
    updateButtons();
    return true;
}

void ASFloaterBoneDeformer::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    if (!mAvatar || mAvatar != gAgentAvatarp || mCategoryLists.empty())
    {
        // A replaced self-avatar invalidates every cached LLJoint pointer.
        mOverrides.clear();
        mUndoHistory.clear();
        mRedoHistory.clear();
        mSessionFakeMeshId.generate();
        buildJointRows();
    }
    for (auto& entry : mCategoryLists)
    {
        if (entry.second)
        {
            entry.second->updatePanels(true);
        }
    }
    updateButtons();
}

void ASFloaterBoneDeformer::onClose(bool app_quitting)
{
    // The preview is intentionally scoped to the open floater session.
    clearOverrides(true);
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

    bool active_override_changed = false;
    joint->addAttachmentPosOverride(joint->getDefaultPosition() + offset,
                                    mSessionFakeMeshId, "AS bone deformer",
                                    active_override_changed);

    refreshAvatarAfterPositionChange(joint, active_override_changed);
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
    joint->addAttachmentScaleOverride(scale, mSessionFakeMeshId, "AS bone deformer");
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
        bool changed = false;
        joint->addAttachmentPosOverride(joint->getDefaultPosition() + state.mPositionOffset,
                                        mSessionFakeMeshId, "AS bone deformer", changed);
        refreshAvatarAfterPositionChange(joint, changed);
    }
    if (!state.mHasPosition && !state.mHasScale)
    {
        mOverrides.erase(found);
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
        joint->addAttachmentScaleOverride(state.mScale, mSessionFakeMeshId, "AS bone deformer");
    }
    if (!state.mHasPosition && !state.mHasScale)
    {
        mOverrides.erase(found);
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
    updateButtons();
}

void ASFloaterBoneDeformer::resetAll()
{
    if (mOverrides.empty())
    {
        return;
    }
    recordUndoState();
    clearOverrides(true);
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
    for (const auto& entry : mOverrides)
    {
        if (entry.second.mHasPosition)
        {
            bool changed = false;
            entry.first->addAttachmentPosOverride(entry.first->getDefaultPosition() + entry.second.mPositionOffset,
                                                   mSessionFakeMeshId, "AS bone deformer", changed);
            refreshAvatarAfterPositionChange(entry.first, changed);
        }
        if (entry.second.mHasScale)
        {
            entry.first->addAttachmentScaleOverride(entry.second.mScale, mSessionFakeMeshId,
                                                    "AS bone deformer");
        }
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
