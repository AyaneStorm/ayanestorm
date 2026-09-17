/**
 * @file asfloaterbonedeformer.h
 * @author chanayane@firestorm
 * @brief Viewer-local per-joint position and scale override editor.
 */

#ifndef AS_FLOATERBONEDEFORMER_H
#define AS_FLOATERBONEDEFORMER_H

#include "llfloater.h"
#include "llassetstorage.h"
#include "lluuid.h"
#include "v3math.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

class FSPoserAnimator;
class ASBoneDeformerBaker;
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
    void draw() override;
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
    bool getShowScales() const { return mShowScales; }
    bool isJointBypassed(LLJoint* joint) const;
    bool isJointExplicitlyBypassed(LLJoint* joint) const;
    void setJointBypassed(LLJoint* joint, bool bypassed);

    const override_map_t& getOverrides() const { return mOverrides; }
    const LLUUID& getSessionFakeMeshId() const { return mSessionFakeMeshId; }

private:
    void buildJointRows();
    void clearJointRows();
    void removePositionOverride(LLJoint* joint);
    void removeScaleOverride(LLJoint* joint);
    void applyPreviewOverride(LLJoint* joint, const ASJointOverrideState& state);
    void refreshPreviewOverrides();
    bool previewOverridesNeedReload() const;
    void showPreviewReloadPrompt();
    void refreshAvatarAfterPositionChange(LLJoint* joint, bool active_override_changed);
    void recordUndoState();
    void onUndo();
    void onRedo();
    void applyOverrideState(const override_map_t& state);
    void trimHistory(std::vector<override_map_t>& history);
    void clearOverrides(bool update_panels);
    void updateButtons();
    void refreshShapeInfo();
    void onShowShapeInInventory();
    void onBakeAndUpload();
    void bakeAndUpload();
    bool onBypassBakeWarning(const LLSD& notification, const LLSD& response);
    void onBakedOverridesApplied();
    void onToggleShowScales();
    void onToggleBypassAll();
    void onToggleBypassScales();
    void onLoadWornDeformer();
    void onExportNotecard();
    void onImportNotecard();
    std::string serializeConfig() const;
    bool importConfig(const std::string& text, std::string& error);
    static void onNotecardLoadComplete(const LLUUID& asset_uuid, LLAssetType::EType type,
                                       void* user_data, S32 status, LLExtStat);
    void onToggleEditPose();

    LLUUID mSessionFakeMeshId;
    LLUUID mShapeItemId;
    override_map_t mOverrides;
    std::unique_ptr<FSPoserAnimator> mJointTable;
    std::shared_ptr<ASBoneDeformerBaker> mBaker;
    LLVOAvatarSelf* mAvatar{ nullptr };
    std::map<S32, LLScrollingPanelList*> mCategoryLists;
    std::map<LLJoint*, S32> mJointCategories;
    std::set<LLJoint*> mBypassedJoints;
    std::vector<override_map_t> mUndoHistory;
    std::vector<override_map_t> mRedoHistory;
    LLButton* mUndoButton{ nullptr };
    LLButton* mRedoButton{ nullptr };
    bool mUndoTransactionOpen{ false };
    bool mUndoTransactionRecorded{ false };
    bool mApplyingHistory{ false };
    bool mShowScales{ false };
    bool mBypassAll{ false };
    bool mBypassScales{ false };
    bool mPreviewReloadPromptPending{ false };
    bool mEditPoseManaged{ false };
    bool mPoseStandWasVisible{ false };
    std::string mPreviousPoseStandSelection;
};

#endif // AS_FLOATERBONEDEFORMER_H
