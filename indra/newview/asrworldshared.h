/**
 * @file asrworldshared.h
 * @brief The few parts of asrworld.cpp that asravatarbody.cpp uses: the scene, and the
 * shared materials and their textures, so a body's textures follow the same slots,
 * feedback and decode visits as every other face.
 * Author: chanayane@firestorm
 */
#ifndef ASR_WORLD_SHARED_H
#define ASR_WORLD_SHARED_H

#include "stdtypes.h"

class LLDrawable;
class LLMatrix4;
class LLViewerTexture;
struct ASRSharedMaterial;

namespace asr
{
class SceneQueue;
struct MaterialDesc;
}

namespace ASRWorldShared
{
    asr::SceneQueue& scene();

    // One user more or less of the material made from desc and image. user is whose
    // textures these are, for the texture's own count of users.
    // System eyes share their drawable with the rest of the body. Identify each use.
    ASRSharedMaterial* useMaterial(const asr::MaterialDesc& desc, LLViewerTexture* image, LLDrawable* user, bool eye);
    void releaseMaterial(ASRSharedMaterial* material, LLDrawable* user, bool eye);
    U32 materialId(const ASRSharedMaterial* material);

    // Keeps the material's texture loading, as the object sweep does for faces.
    void visitMaterialTexture(ASRSharedMaterial* material);

    // Three rows of a row-vector matrix, as the GPU takes them.
    void rowsOf(const LLMatrix4& matrix, F32* rows);
}

#endif
