/**
 * @file asrworld.h
 * @brief Keeps the new renderer's GPU scene in step with the viewer's objects.
 *
 * The pipeline reports what changed, and update() applies it once per frame, so an
 * unchanged object costs nothing. Each unique prim shape is built once, at every LOD,
 * on worker threads, and all prims of that shape share it. See section 4 of
 * doc/ayanestorm-new-renderer-plan.md.
 *
 * Prims, sculpts, mesh, terrain, attachments, rigged mesh and the system avatar body
 * (asravatarbody.cpp) are drawn so far, worn or animated, on each avatar's skeleton on the GPU.
 * Flexible prims are left out until their own step.
 */
#ifndef ASR_WORLD_H
#define ASR_WORLD_H

#include "stdtypes.h"

class LLDrawable;
class LLVector3;
class LLViewerFetchedTexture;
class LLVolume;
class LLVolumeParams;
class LLVOAvatar;
class LLViewerObject;

namespace ASRWorld
{
    void start();
    void stop();

    // Its shape, textures or faces changed, or it is new.
    void objectChanged(LLDrawable* drawable);

    // It moved, and everything linked to it moves with it.
    void objectMoved(LLDrawable* drawable);

    void objectRemoved(LLDrawable* drawable);

    // An avatar's body morph masks were made again, in place, from its bakes.
    void avatarMasksChanged(LLVOAvatar* avatar);

    // Every how many frames the avatar animates, by distance rank; 0 when the renderer
    // does not set it (off, your own avatar, not drawn by it), so the viewer's rules apply.
    S32 animationPeriod(LLVOAvatar* avatar);

    // The viewer animated the avatar this frame: its joints and body weights can have moved.
    void avatarAnimated(LLVOAvatar* avatar);

    // The native frame, in place of LLPipeline::updateGL(): runs the terrain patches' part of
    // that queue, so the viewer can finish a patch's update and stop recomputing its normals
    // every frame. The rest stays queued, as it would draw with GL.
    void updateTerrainGL();

    // One LOD of a mesh came in, or the server has none. The volume is not changed after this.
    void meshLoaded(const LLVolumeParams& params, LLVolume* volume, S32 lod);
    void meshUnavailable(const LLVolumeParams& params, S32 lod);

    // Agent space has a new origin: every position in it moved by this offset.
    void worldShifted(const LLVector3& offset);

    // What a normal texture is for, now. Its faces say how large it would be wanted, but only
    // what the renderer draws is worth memory, and every image takes one of a limited number of
    // slots. Wanted keeps the size, NotDrawn wants no size, and Release also gives up its image,
    // which is reloaded from the cache if it comes back into view.
    // size is what the faces gave, as a virtual size, and bias the divisor they use under
    // memory pressure. When the GPU saw the texture, size becomes what it really needed; when
    // the faces are in view but the GPU drew none of it, it is hidden, and wants less.
    enum class TextureUse { Wanted, NotDrawn, Release };
    TextureUse judgeTexture(LLViewerFetchedTexture* image, bool inView, F32 bias, F32& size);
    // Distance-based mip floor for textures used exclusively by small avatar details.
    S32 avatarDetailDiscard(LLViewerFetchedTexture* image);
    // In view and near the camera: always allowed to update, even past the texture limit.
    bool isNearTexture(LLViewerFetchedTexture* image);
    // Near surfaces always, and far ones at high screen demand when they fit, refine above the soft target.
    bool canRefineVisibleTexture(LLViewerFetchedTexture* image, S32 discard);
    // Crowd detail ranking: the factor on the LOD factor for the meshes an avatar wears.
    // 1 for anything not worn, and while the renderer is off.
    F32 avatarLodScale(LLViewerObject* object);
    void releaseTexture(LLViewerFetchedTexture* image);

    // Applies everything reported since the last call. Main thread, once per frame.
    void update();

    // Temporary: the rest of the main thread loop, so the log can account for all of it.
    enum MainPart
    {
        MAIN_TEXTURES, MAIN_CAMERA, MAIN_CREATE, MAIN_GEOMETRY, MAIN_UI,
        MAIN_NETWORK, MAIN_IDLE_CALLBACKS, MAIN_WINDOW, MAIN_OBJECT_LIST, MAIN_CLEANUP, MAIN_MOVE, MAIN_PARTICLES, MAIN_ANGLES,
        MAIN_EVENTS, MAIN_IDLE_WHOLE, MAIN_SNAPSHOTS, MAIN_TEXTURE_THREADS, MAIN_MESH_UPDATE,
        MAIN_DISPLAY_WHOLE, MAIN_SLEEP_WHOLE,
        MAIN_IDLE_START, MAIN_AGENT_POSITION, MAIN_REGIONS, MAIN_CAMERA_MEDIA, MAIN_AUDIO,
        MAIN_PART_COUNT
    };
    void addMainTime(MainPart part, F32 seconds);
}

#endif // ASR_WORLD_H
