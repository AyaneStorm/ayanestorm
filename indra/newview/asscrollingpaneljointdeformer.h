/**
 * @file asscrollingpaneljointdeformer.h
 * @author chanayane@ayanestorm
 * @brief One joint row in the Advanced Shape Deformer.
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
