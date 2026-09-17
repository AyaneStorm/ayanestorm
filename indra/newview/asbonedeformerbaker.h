/**
 * @file asbonedeformerbaker.h
 * @author chanayane@firestorm
 * @brief Builds and uploads a standard-position/AyaneStorm-scale deformer mesh.
 */

#ifndef AS_BONEDEFORMERBAKER_H
#define AS_BONEDEFORMERBAKER_H

#include "llinventoryobserver.h"
#include "llframetimer.h"
#include "llmodel.h"
#include "lluploadfloaterobservers.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

class LLJoint;

struct ASBoneDeformerBakeJoint
{
    LLJoint* mJoint{ nullptr };
    LLVector3 mPosition;
    LLVector3 mScale{ 1.f, 1.f, 1.f };
    bool mHasPosition{ false };
    bool mHasScale{ false };
};

class ASBoneDeformerBaker final : public LLWholeModelFeeObserver,
                                  public LLWholeModelUploadObserver,
                                  public LLInventoryObserver,
                                  public std::enable_shared_from_this<ASBoneDeformerBaker>
{
public:
    using applied_callback_t = std::function<void()>;
    using state_callback_t = std::function<void()>;

    ASBoneDeformerBaker(applied_callback_t applied_callback,
                       state_callback_t state_callback);
    ~ASBoneDeformerBaker() override;

    bool start(const std::vector<ASBoneDeformerBakeJoint>& joints,
               const std::string& shape_name);
    bool isBusy() const { return mBusy; }

    void onModelPhysicsFeeReceived(const LLSD& result, std::string upload_url) override;
    void setModelPhysicsFeeErrorStatus(S32 status, const std::string& reason,
                                       const LLSD& result) override;
    void onModelUploadSuccess() override;
    void onModelUploadFailure() override;
    void changed(U32 mask) override;

private:
    bool buildModel();
    void handleFeeReceived();
    void handleFeeError();
    void upload();
    void setBusy(bool busy);
    bool findAndAttachInventoryItem();
    bool pollAttachmentOverrides();

    std::vector<ASBoneDeformerBakeJoint> mJoints;
    std::vector<LLModelInstance> mUploadData;
    std::map<std::string, std::string> mLodSources;
    LLVector3 mUploadScale{ 1.f, 1.f, 1.f };
    LLUUID mDestinationFolderId;
    std::string mShapeName;
    std::string mItemName;
    std::string mUploadUrl;
    LLSD mFeeResult;
    std::string mFeeError;
    S32 mPositionCount{ 0 };
    S32 mScaleCount{ 0 };
    bool mBusy{ false };
    bool mObservingInventory{ false };
    bool mAttachmentRequested{ false };
    LLFrameTimer mAttachmentTimer;
    std::set<LLUUID> mActiveMeshesBeforeAttach;
    applied_callback_t mAppliedCallback;
    state_callback_t mStateCallback;
};

#endif // AS_BONEDEFORMERBAKER_H
