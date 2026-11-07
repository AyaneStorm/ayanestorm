/**
 * @file asrworld.h
 * @brief Keeps the new renderer's GPU scene in step with the viewer's objects.
 *
 * The pipeline reports what changed, and update() applies it once per frame, so an
 * unchanged object costs nothing. Each unique prim shape is built once, at every LOD,
 * on worker threads, and all prims of that shape share it. See section 4 of
 * doc/ayanestorm-new-renderer-plan.md.
 *
 * Plain prims are drawn so far. Sculpts, mesh, flexible prims, attachments and rigged
 * objects are left out until their own steps.
 */
#ifndef ASR_WORLD_H
#define ASR_WORLD_H

class LLDrawable;

namespace ASRWorld
{
    void start();
    void stop();

    // Its shape, textures or faces changed, or it is new.
    void objectChanged(LLDrawable* drawable);

    // It moved, and everything linked to it moves with it.
    void objectMoved(LLDrawable* drawable);

    void objectRemoved(LLDrawable* drawable);

    // Agent space has a new origin, so every object has a new position in it.
    void worldShifted();

    // Applies everything reported since the last call. Main thread, once per frame.
    void update();
}

#endif // ASR_WORLD_H
