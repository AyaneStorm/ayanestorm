/**
 * @file asrworld.h
 * @brief Keeps the new renderer's GPU scene in step with the viewer's objects.
 *
 * The pipeline reports what changed, and update() applies it once per frame, so an
 * unchanged object costs nothing. Each unique prim shape is built once, at every LOD,
 * on worker threads, and all prims of that shape share it. See section 4 of
 * doc/ayanestorm-new-renderer-plan.md.
 *
 * Prims, sculpts and mesh are drawn so far. Flexible prims, attachments and rigged objects
 * are left out until their own steps.
 */
#ifndef ASR_WORLD_H
#define ASR_WORLD_H

#include "stdtypes.h"

class LLDrawable;
class LLVector3;
class LLVolume;
class LLVolumeParams;

namespace ASRWorld
{
    void start();
    void stop();

    // Its shape, textures or faces changed, or it is new.
    void objectChanged(LLDrawable* drawable);

    // It moved, and everything linked to it moves with it.
    void objectMoved(LLDrawable* drawable);

    void objectRemoved(LLDrawable* drawable);

    // One LOD of a mesh came in, or the server has none. The volume is not changed after this.
    void meshLoaded(const LLVolumeParams& params, LLVolume* volume, S32 lod);
    void meshUnavailable(const LLVolumeParams& params, S32 lod);

    // Agent space has a new origin: every position in it moved by this offset.
    void worldShifted(const LLVector3& offset);

    // Applies everything reported since the last call. Main thread, once per frame.
    void update();

    // Temporary: the rest of the main thread loop, so the log can account for all of it.
    enum MainPart { MAIN_TEXTURES, MAIN_CAMERA, MAIN_CREATE, MAIN_GEOMETRY, MAIN_UI, MAIN_PART_COUNT };
    void addMainTime(MainPart part, F32 seconds);
}

#endif // ASR_WORLD_H
