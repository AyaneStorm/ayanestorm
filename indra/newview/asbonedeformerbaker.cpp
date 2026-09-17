/**
 * @file asbonedeformerbaker.cpp
 * @author chanayane@firestorm
 * @brief See asbonedeformerbaker.h
 */

#include "llviewerprecompiledheaders.h"

#include "asbonedeformerbaker.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llinventorybridge.h"
#include "llinventorymodel.h"
#include "lljoint.h"
#include "llmeshrepository.h"
#include "llnotificationsutil.h"
#include "llviewerjointattachment.h"
#include "llvoavatarself.h"

namespace
{
    constexpr S32 AVATAR_CENTER_ATTACHMENT_ID = 40;
    const std::string MATERIAL_NAME("Deformer");
}

ASBoneDeformerBaker::ASBoneDeformerBaker(applied_callback_t applied_callback,
                                         state_callback_t state_callback)
:   mAppliedCallback(std::move(applied_callback)),
    mStateCallback(std::move(state_callback))
{
}

ASBoneDeformerBaker::~ASBoneDeformerBaker()
{
    if (mObservingInventory)
    {
        gInventory.removeObserver(this);
    }
}

bool ASBoneDeformerBaker::start(const std::vector<ASBoneDeformerBakeJoint>& joints,
                                const std::string& shape_name)
{
    if (mBusy || joints.empty() || !isAgentAvatarValid() || !gMeshRepo.meshUploadEnabled())
    {
        return false;
    }

    mJoints = joints;
    mShapeName = shape_name;
    mPositionCount = 0;
    mScaleCount = 0;
    for (const ASBoneDeformerBakeJoint& entry : mJoints)
    {
        mPositionCount += entry.mHasPosition ? 1 : 0;
        mScaleCount += entry.mHasScale ? 1 : 0;
    }
    LLUUID bake_id;
    bake_id.generate();
    mItemName = "AS Deformer - " + shape_name + " - " + bake_id.asString().substr(0, 8);
    mDestinationFolderId = gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT);
    if (mDestinationFolderId.isNull() || !buildModel())
    {
        return false;
    }

    setBusy(true);
    mUploadUrl.clear();
    mFeeResult.clear();
    gMeshRepo.uploadModel(mUploadData, mLodSources, mUploadScale,
                          false, true, true, false, std::string(),
                          mDestinationFolderId, false,
                          getWholeModelFeeObserverHandle());
    return true;
}

bool ASBoneDeformerBaker::buildModel()
{
    LLJoint* carrier = gAgentAvatarp->getJoint("mPelvis");
    if (!carrier)
    {
        return false;
    }

    LLVolumeParams volume_params;
    LLPointer<LLModel> model = new LLModel(volume_params, 0.f);
    model->mLabel = mItemName;
    model->mRequestedLabel = mItemName;

    std::vector<LLVolumeFace::VertexData> vertices(3);
    const LLVector4a positions[] = {
        LLVector4a(-0.005f, 0.f, 0.f),
        LLVector4a(0.005f, 0.f, 0.f),
        LLVector4a(0.f, 0.f, 0.005f)
    };
    for (S32 i = 0; i < 3; ++i)
    {
        vertices[i].setPosition(positions[i]);
        vertices[i].setNormal(LLVector4a(0.f, -1.f, 0.f));
    }
    vertices[0].mTexCoord.setVec(0.f, 0.f);
    vertices[1].mTexCoord.setVec(1.f, 0.f);
    vertices[2].mTexCoord.setVec(0.5f, 1.f);
    std::vector<U16> indices{ 0, 1, 2 };
    LLVolumeFace face;
    face.fillFromLegacyData(vertices, indices);
    face.mExtents[0].set(-0.005f, 0.f, 0.f);
    face.mExtents[1].set(0.005f, 0.f, 0.005f);
    face.mCenter->set(0.f, 0.f, 0.0025f);
    model->addFace(face);
    model->mMaterialList.push_back(MATERIAL_NAME);

    std::vector<ASBoneDeformerBakeJoint> ordered_joints;
    ordered_joints.push_back({ carrier, carrier->getDefaultPosition(),
                               carrier->getDefaultScale(), false, false });
    for (const ASBoneDeformerBakeJoint& entry : mJoints)
    {
        if (entry.mJoint && entry.mJoint != carrier)
        {
            ordered_joints.push_back(entry);
        }
        else if (entry.mJoint == carrier)
        {
            ordered_joints.front() = entry;
        }
    }

    model->mSkinInfo.mBindShapeMatrix.setIdentity();
    model->mSkinInfo.mPelvisOffset = 0.f;
    model->mSkinInfo.mLockScaleIfJointPosition = false;
    model->mSkinInfo.mASJointScaleVersion = mScaleCount > 0 ? 1 : 0;
    for (const ASBoneDeformerBakeJoint& entry : ordered_joints)
    {
        model->mSkinInfo.mJointNames.push_back(entry.mJoint->getName());
        LLMatrix4 inverse_bind(entry.mJoint->getWorldMatrix());
        inverse_bind.invert();
        model->mSkinInfo.mInvBindMatrix.emplace_back(inverse_bind);
        const LLVector3 target_position = entry.mHasPosition
            ? entry.mPosition : entry.mJoint->getDefaultPosition();
        if (entry.mHasScale)
        {
            LLMatrix4 scale_metadata;
            scale_metadata.initAll(entry.mScale, LLQuaternion::DEFAULT, target_position);
            model->mSkinInfo.mAlternateBindMatrix.emplace_back(scale_metadata);
            model->mSkinInfo.mASJointScaleOverrides.push_back(entry.mScale);
        }
        else
        {
            inverse_bind.setTranslation(target_position);
            model->mSkinInfo.mAlternateBindMatrix.emplace_back(inverse_bind);
            model->mSkinInfo.mASJointScaleOverrides.push_back(LLVector3::zero);
        }
    }

    for (const LLVector4a& position : positions)
    {
        const LLVector3 position3(position.getF32ptr());
        model->mPosition.push_back(position3);
        model->mSkinWeights[position3].push_back(LLModel::JointWeight(0, 1.f));
    }

    material_map materials;
    materials[MATERIAL_NAME].mDiffuseColor.set(1.f, 1.f, 1.f, 0.f);
    LLModelInstance instance(model, mItemName, LLMatrix4(), materials);
    instance.mModel = model;
    for (S32 lod = 0; lod < LLModel::NUM_LODS; ++lod)
    {
        instance.mLOD[lod] = model;
    }
    mUploadData.clear();
    mUploadData.push_back(instance);
    return true;
}

void ASBoneDeformerBaker::onModelPhysicsFeeReceived(const LLSD& result,
                                                     std::string upload_url)
{
    mFeeResult = result;
    mUploadUrl = std::move(upload_url);
    const std::shared_ptr<ASBoneDeformerBaker> self = shared_from_this();
    doOnIdleOneTime([self]() { self->handleFeeReceived(); });
}

void ASBoneDeformerBaker::setModelPhysicsFeeErrorStatus(S32 status,
                                                        const std::string& reason,
                                                        const LLSD& result)
{
    mFeeError = reason;
    const std::shared_ptr<ASBoneDeformerBaker> self = shared_from_this();
    doOnIdleOneTime([self]() { self->handleFeeError(); });
}

void ASBoneDeformerBaker::handleFeeReceived()
{
    LLSD args;
    args["PRICE"] = mFeeResult["upload_price"].asInteger();
    args["SHAPE"] = mShapeName;
    args["POSITION_COUNT"] = mPositionCount;
    args["SCALE_COUNT"] = mScaleCount;
    const std::shared_ptr<ASBoneDeformerBaker> self = shared_from_this();
    LLNotificationsUtil::add("ASBoneDeformerUploadConfirmation", args, LLSD(),
        [self](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                self->upload();
            }
            else
            {
                self->setBusy(false);
            }
            return false;
        });
}

void ASBoneDeformerBaker::handleFeeError()
{
    LLNotificationsUtil::add("ASBoneDeformerUploadFailed",
                             LLSD().with("REASON", mFeeError));
    setBusy(false);
}

void ASBoneDeformerBaker::upload()
{
    gMeshRepo.uploadModel(mUploadData, mLodSources, mUploadScale,
                          false, true, true, false, mUploadUrl,
                          mDestinationFolderId, true,
                          LLHandle<LLWholeModelFeeObserver>(),
                          getWholeModelUploadObserverHandle());
}

void ASBoneDeformerBaker::onModelUploadSuccess()
{
    LLNotificationsUtil::add("ASBoneDeformerUploadSucceeded");
    if (!findAndAttachInventoryItem() && !mObservingInventory)
    {
        gInventory.addObserver(this);
        mObservingInventory = true;
    }
}

void ASBoneDeformerBaker::onModelUploadFailure()
{
    LLNotificationsUtil::add("ASBoneDeformerUploadFailed",
                             LLSD().with("REASON", "Upload rejected"));
    setBusy(false);
}

void ASBoneDeformerBaker::changed(U32 mask)
{
    if ((mask & (LLInventoryObserver::ADD | LLInventoryObserver::CREATE)) != 0)
    {
        findAndAttachInventoryItem();
    }
}

bool ASBoneDeformerBaker::findAndAttachInventoryItem()
{
    LLInventoryModel::cat_array_t categories;
    LLInventoryModel::item_array_t items;
    gInventory.collectDescendents(mDestinationFolderId, categories, items,
                                  LLInventoryModel::EXCLUDE_TRASH);
    LLViewerInventoryItem* match = nullptr;
    for (const LLPointer<LLViewerInventoryItem>& item_ptr : items)
    {
        LLViewerInventoryItem* item = item_ptr.get();
        if (item && item->getName() == mItemName &&
            item->getActualType() == LLAssetType::AT_OBJECT &&
            (!match || item->getCreationDate() > match->getCreationDate()))
        {
            match = item;
        }
    }
    if (!match || !isAgentAvatarValid())
    {
        return false;
    }

    if (mObservingInventory)
    {
        gInventory.removeObserver(this);
        mObservingInventory = false;
    }
    auto found = gAgentAvatarp->mAttachmentPoints.find(AVATAR_CENTER_ATTACHMENT_ID);
    if (found == gAgentAvatarp->mAttachmentPoints.end())
    {
        setBusy(false);
        return false;
    }
    mActiveMeshesBeforeAttach = gAgentAvatarp->mActiveOverrideMeshes;
    rez_attachment(match, found->second, false);
    mAttachmentRequested = true;
    mAttachmentTimer.reset();
    const std::shared_ptr<ASBoneDeformerBaker> self = shared_from_this();
    doOnIdleRepeating([self]() { return self->pollAttachmentOverrides(); });
    return true;
}

bool ASBoneDeformerBaker::pollAttachmentOverrides()
{
    if (!mAttachmentRequested || !isAgentAvatarValid())
    {
        setBusy(false);
        return true;
    }
    if (gAgentAvatarp->mActiveOverrideMeshes == mActiveMeshesBeforeAttach)
    {
        if (mAttachmentTimer.getElapsedTimeF32() > 60.f)
        {
            mAttachmentRequested = false;
            LLNotificationsUtil::add("ASBoneDeformerUploadFailed",
                                     LLSD().with("REASON", "The uploaded attachment did not register"));
            setBusy(false);
            return true;
        }
        return false;
    }
    mAttachmentRequested = false;
    if (mAppliedCallback)
    {
        mAppliedCallback();
    }
    setBusy(false);
    return true;
}

void ASBoneDeformerBaker::setBusy(bool busy)
{
    mBusy = busy;
    if (mStateCallback)
    {
        mStateCallback();
    }
}
