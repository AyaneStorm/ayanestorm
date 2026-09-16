/**
 * @file asscrollingpaneljointdeformer.cpp
 * @author chanayane@firestorm
 * @brief See asscrollingpaneljointdeformer.h
 */

#include "llviewerprecompiledheaders.h"

#include "asscrollingpaneljointdeformer.h"

#include "asfloaterbonedeformer.h"
#include "llbutton.h"
#include "lljoint.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluictrl.h"

ASScrollingPanelJointDeformer::ASScrollingPanelJointDeformer(const LLPanel::Params& params,
                                                             ASFloaterBoneDeformer* owner,
                                                             LLJoint* joint)
:   LLScrollingPanel(params),
    mOwner(owner),
    mJoint(joint)
{
    buildFromFile("panel_as_joint_deformer.xml");
    getChild<LLTextBox>("joint_name")->setText(joint->getName());

    for (const char* name : { "pos_x", "pos_y", "pos_z" })
    {
        LLUICtrl* control = getChild<LLUICtrl>(name);
        control->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onPositionChanged(); });
    }
    for (const char* name : { "scale_x", "scale_y", "scale_z" })
    {
        LLUICtrl* control = getChild<LLUICtrl>(name);
        control->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onScaleChanged(); });
    }
    getChild<LLButton>("reset_pos_x")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetPositionAxis(VX); });
    getChild<LLButton>("reset_pos_y")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetPositionAxis(VY); });
    getChild<LLButton>("reset_pos_z")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetPositionAxis(VZ); });
    getChild<LLButton>("reset_scale_x")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetScaleAxis(VX); });
    getChild<LLButton>("reset_scale_y")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetScaleAxis(VY); });
    getChild<LLButton>("reset_scale_z")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetScaleAxis(VZ); });
    getChild<LLButton>("reset_joint")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onReset(); });

    mBuilt = true;
    layoutControls();
    updatePanel(true);
    setVisible(false);
    setBorderVisible(false);
}

void ASScrollingPanelJointDeformer::updatePanel(bool allow_modify)
{
    mUpdating = true;
    const ASJointOverrideState state = mOwner->getOverrideState(mJoint);
    writeVector("pos_x", "pos_y", "pos_z", state.mPositionOffset);
    writeVector("scale_x", "scale_y", "scale_z", state.mScale);
    getChild<LLButton>("reset_joint")->setEnabled(state.mHasPosition || state.mHasScale);
    getChild<LLButton>("reset_pos_x")->setEnabled(state.mHasPosition && state.mPositionOffset.mV[VX] != state.mBasePositionOffset.mV[VX]);
    getChild<LLButton>("reset_pos_y")->setEnabled(state.mHasPosition && state.mPositionOffset.mV[VY] != state.mBasePositionOffset.mV[VY]);
    getChild<LLButton>("reset_pos_z")->setEnabled(state.mHasPosition && state.mPositionOffset.mV[VZ] != state.mBasePositionOffset.mV[VZ]);
    getChild<LLButton>("reset_scale_x")->setEnabled(state.mHasScale && state.mScale.mV[VX] != state.mBaseScale.mV[VX]);
    getChild<LLButton>("reset_scale_y")->setEnabled(state.mHasScale && state.mScale.mV[VY] != state.mBaseScale.mV[VY]);
    getChild<LLButton>("reset_scale_z")->setEnabled(state.mHasScale && state.mScale.mV[VZ] != state.mBaseScale.mV[VZ]);
    mUpdating = false;
}

void ASScrollingPanelJointDeformer::draw()
{
    bool slider_held = false;
    for (const char* name : { "pos_x", "pos_y", "pos_z", "scale_x", "scale_y", "scale_z" })
    {
        slider_held = slider_held || getChild<LLSliderCtrl>(name)->isMouseHeldDown();
    }
    if (!slider_held && mUndoDragOpen)
    {
        mOwner->endUndoTransaction();
        mUndoDragOpen = false;
    }

    if (getParent())
    {
        const S32 target_width = getParent()->getRect().getWidth() - 4;
        if (target_width > 0 && target_width != getRect().getWidth())
        {
            reshape(target_width, getRect().getHeight(), false);
        }
    }
    LLScrollingPanel::draw();
}

void ASScrollingPanelJointDeformer::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLScrollingPanel::reshape(width, height, called_from_parent);
    if (mBuilt)
    {
        layoutControls();
    }
}

void ASScrollingPanelJointDeformer::onPositionChanged()
{
    if (!mUpdating)
    {
        if (!mUndoDragOpen &&
            (getChild<LLSliderCtrl>("pos_x")->isMouseHeldDown() ||
             getChild<LLSliderCtrl>("pos_y")->isMouseHeldDown() ||
             getChild<LLSliderCtrl>("pos_z")->isMouseHeldDown()))
        {
            mOwner->beginUndoTransaction();
            mUndoDragOpen = true;
        }
        mOwner->setPositionOffset(mJoint, readVector("pos_x", "pos_y", "pos_z"));
        updatePanel(true);
    }
}

void ASScrollingPanelJointDeformer::onScaleChanged()
{
    if (!mUpdating)
    {
        if (!mUndoDragOpen &&
            (getChild<LLSliderCtrl>("scale_x")->isMouseHeldDown() ||
             getChild<LLSliderCtrl>("scale_y")->isMouseHeldDown() ||
             getChild<LLSliderCtrl>("scale_z")->isMouseHeldDown()))
        {
            mOwner->beginUndoTransaction();
            mUndoDragOpen = true;
        }
        mOwner->setScale(mJoint, readVector("scale_x", "scale_y", "scale_z"));
        updatePanel(true);
    }
}

void ASScrollingPanelJointDeformer::onResetPositionAxis(S32 axis)
{
    mOwner->resetPositionAxis(mJoint, axis);
    updatePanel(true);
}

void ASScrollingPanelJointDeformer::onResetScaleAxis(S32 axis)
{
    mOwner->resetScaleAxis(mJoint, axis);
    updatePanel(true);
}

void ASScrollingPanelJointDeformer::onReset()
{
    mOwner->resetJoint(mJoint);
    updatePanel(true);
}

void ASScrollingPanelJointDeformer::layoutControls()
{
    const S32 width = getRect().getWidth();
    const S32 midpoint = width / 2;
    const S32 margin = 8;
    const S32 gap = 4;
    const S32 reset_width = 18;

    const char* position_controls[] = { "pos_x", "pos_y", "pos_z" };
    const char* position_resets[] = { "reset_pos_x", "reset_pos_y", "reset_pos_z" };
    const char* scale_controls[] = { "scale_x", "scale_y", "scale_z" };
    const char* scale_resets[] = { "reset_scale_x", "reset_scale_y", "reset_scale_z" };

    for (S32 axis = VX; axis <= VZ; ++axis)
    {
        LLRect reset_rect = getChild<LLButton>(position_resets[axis])->getRect();
        reset_rect.mRight = midpoint - gap;
        reset_rect.mLeft = reset_rect.mRight - reset_width;
        getChild<LLButton>(position_resets[axis])->setRect(reset_rect);

        LLRect control_rect = getChild<LLUICtrl>(position_controls[axis])->getRect();
        control_rect.mLeft = margin;
        control_rect.mRight = reset_rect.mLeft - gap;
        getChild<LLUICtrl>(position_controls[axis])->setRect(control_rect);

        reset_rect = getChild<LLButton>(scale_resets[axis])->getRect();
        reset_rect.mRight = width - margin;
        reset_rect.mLeft = reset_rect.mRight - reset_width;
        getChild<LLButton>(scale_resets[axis])->setRect(reset_rect);

        control_rect = getChild<LLUICtrl>(scale_controls[axis])->getRect();
        control_rect.mLeft = midpoint + gap;
        control_rect.mRight = reset_rect.mLeft - gap;
        getChild<LLUICtrl>(scale_controls[axis])->setRect(control_rect);
    }

    LLRect name_rect = getChild<LLTextBox>("joint_name")->getRect();
    name_rect.mRight = width - 90;
    getChild<LLTextBox>("joint_name")->setRect(name_rect);
    LLRect reset_joint_rect = getChild<LLButton>("reset_joint")->getRect();
    reset_joint_rect.mRight = width - margin;
    reset_joint_rect.mLeft = reset_joint_rect.mRight - 70;
    getChild<LLButton>("reset_joint")->setRect(reset_joint_rect);
}

LLVector3 ASScrollingPanelJointDeformer::readVector(const char* x_name, const char* y_name,
                                                    const char* z_name) const
{
    return LLVector3(static_cast<F32>(getChild<LLUICtrl>(x_name)->getValue().asReal()),
                     static_cast<F32>(getChild<LLUICtrl>(y_name)->getValue().asReal()),
                     static_cast<F32>(getChild<LLUICtrl>(z_name)->getValue().asReal()));
}

void ASScrollingPanelJointDeformer::writeVector(const char* x_name, const char* y_name,
                                                const char* z_name, const LLVector3& value)
{
    getChild<LLUICtrl>(x_name)->setValue(value.mV[VX]);
    getChild<LLUICtrl>(y_name)->setValue(value.mV[VY]);
    getChild<LLUICtrl>(z_name)->setValue(value.mV[VZ]);
}
