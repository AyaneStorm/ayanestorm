/**
 * @file asfloaterbonedeformer.h
 * @author chanayane@firestorm
 * @brief Viewer-local per-joint position and scale override editor.
 */

#ifndef AS_FLOATERBONEDEFORMER_H
#define AS_FLOATERBONEDEFORMER_H

#include "llfloater.h"
#include "lluuid.h"
#include "v3math.h"

#include <map>
#include <memory>
#include <vector>

class FSPoserAnimator;
class LLButton;
class LLJoint;
class LLScrollingPanelList;
class LLVOAvatarSelf;

struct ASJointOverrideState
{
    LLVector3 mPositionOffset{ 0.f, 0.f, 0.f };
    LLVector3 mScale{ 1.f, 1.f, 1.f };
    LLVector3 mBasePositionOffset{ 0.f, 0.f, 0.f };
    LLVector3 mBaseScale{ 1.f, 1.f, 1.f };
    bool mHasPosition{ false };
    bool mHasScale{ false };
};

class ASFloaterBoneDeformer : public LLFloater
{
public:
    using override_map_t = std::map<LLJoint*, ASJointOverrideState>;

    ASFloaterBoneDeformer(const LLSD& key);
    ~ASFloaterBoneDeformer() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

    ASJointOverrideState getOverrideState(LLJoint* joint) const;
    void setPositionOffset(LLJoint* joint, const LLVector3& offset);
    void setScale(LLJoint* joint, const LLVector3& scale);
    void resetPositionAxis(LLJoint* joint, S32 axis);
    void resetScaleAxis(LLJoint* joint, S32 axis);
    void resetJoint(LLJoint* joint);
    void resetAll();
    void beginUndoTransaction();
    void endUndoTransaction();

    const override_map_t& getOverrides() const { return mOverrides; }
    const LLUUID& getSessionFakeMeshId() const { return mSessionFakeMeshId; }

private:
    void buildJointRows();
    void clearJointRows();
    void removePositionOverride(LLJoint* joint);
    void removeScaleOverride(LLJoint* joint);
    void refreshAvatarAfterPositionChange(LLJoint* joint, bool active_override_changed);
    void recordUndoState();
    void onUndo();
    void applyOverrideState(const override_map_t& state);
    void clearOverrides(bool update_panels);
    void updateButtons();
    void onToggleTPose();

    LLUUID mSessionFakeMeshId;
    override_map_t mOverrides;
    std::unique_ptr<FSPoserAnimator> mJointTable;
    LLVOAvatarSelf* mAvatar{ nullptr };
    std::map<S32, LLScrollingPanelList*> mCategoryLists;
    std::vector<override_map_t> mUndoHistory;
    LLButton* mUndoButton{ nullptr };
    bool mUndoTransactionOpen{ false };
    bool mUndoTransactionRecorded{ false };
    bool mApplyingHistory{ false };
    bool mTPoseManaged{ false };
    bool mPoseStandWasVisible{ false };
    std::string mPreviousPoseStandSelection;
};

#endif // AS_FLOATERBONEDEFORMER_H
