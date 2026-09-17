/**
 * @file asscrollingpaneljointdeformer.h
 * @author chanayane@firestorm
 * @brief One joint row in the Advanced Shape Deformer.
 */

#ifndef AS_SCROLLINGPANELJOINTDEFORMER_H
#define AS_SCROLLINGPANELJOINTDEFORMER_H

#include "llscrollingpanellist.h"
#include "v3math.h"

class ASFloaterBoneDeformer;
class LLJoint;
class LLUICtrl;

class ASScrollingPanelJointDeformer : public LLScrollingPanel
{
public:
    ASScrollingPanelJointDeformer(const LLPanel::Params& params,
                                  ASFloaterBoneDeformer* owner,
                                  LLJoint* joint);

    void updatePanel(bool allow_modify) override;
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

private:
    void onPositionChanged(S32 axis);
    void onScaleChanged(S32 axis);
    void onResetPositionAxis(S32 axis);
    void onResetScaleAxis(S32 axis);
    void onReset();
    void onBypass();
    void layoutControls();
    void writeVector(const char* x_name, const char* y_name, const char* z_name,
                     const LLVector3& value);

    ASFloaterBoneDeformer* mOwner;
    LLJoint* mJoint;
    bool mUpdating{ false };
    bool mBuilt{ false };
    bool mUndoDragOpen{ false };
};

#endif // AS_SCROLLINGPANELJOINTDEFORMER_H
