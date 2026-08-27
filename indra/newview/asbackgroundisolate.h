/**
 * @file asbackgroundisolate.h
 * @author chanayane@firestorm
 * @brief Viewer-local solid-color background isolate pass for the self-lighting
 *        floater's photography mode. Draws after the full post-process chain
 *        (tonemap, bloom, DoF, FXAA/SMAA, vignette) so the isolate color is
 *        immune to all of it, unlike a pass drawn earlier in the pipeline.
 */

#ifndef AS_BACKGROUNDISOLATE_H
#define AS_BACKGROUNDISOLATE_H

#include <set>
#include <vector>

#include "llglslshader.h"
#include "lluuid.h"
#include "v4color.h"

class LLRenderTarget;
class LLVertexBuffer;
class LLDrawable;

namespace ASBackgroundIsolate
{
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();

    // No-op unless isolate mode is currently active (set via
    // ASFloaterMyLight::setActive()). depth_target supplies the scene depth
    // used to tell background pixels (nothing opaque drawn there) apart from
    // the avatar, which must stay untouched.
    void render(LLRenderTarget& depth_target, LLVertexBuffer& screen_triangle);

    // Called by ASFloaterMyLight when isolate mode is turned on/off/recolored.
    void setActive(bool active, const LLColor4& color);

    // Called by ASFloaterMyLight whenever its light-rig object list changes,
    // so the live stateSort() allowlist check (below) always exempts exactly
    // the currently-live light-rig objects, not a stale snapshot.
    void setLightRigIds(const std::set<LLUUID>& ids);

    // Queried by other screen-space post effects (lens flare, vignette) that
    // must not draw over the solid isolate backdrop -- a lens flare "ghost"
    // or a vignette darkening the corners would both undermine the "only the
    // avatar, on a flat color" result isolate mode promises.
    bool isActive();

    // True if this drawable should be excluded from rendering while
    // isolate mode is active: everything except the self avatar, its own
    // attachments, and our own light-rig objects. Recomputed live from
    // current object/avatar state on every call (cheap early-out when
    // isolate mode is inactive), so it can never go stale on its own.
    bool shouldHideDrawable(LLDrawable* drawable);

    // Called from LLPipeline::stateSort(LLDrawable*, LLCamera&) for every
    // drawable, every frame -- the single chokepoint all drawables already
    // pass through regardless of network/update traffic. Sets or clears
    // LLDrawable::FORCE_INVISIBLE to match shouldHideDrawable()'s live
    // answer, and forces a geometry rebuild on state change so the result
    // takes effect the same frame. This is the actual hiding mechanism for
    // ordinary prim/mesh ("volume") drawables: their geometry is batched
    // once per spatial group into LLSpatialGroup::mDrawMap by
    // LLVolumeGeometryManager::rebuildGeom(), independent of any given
    // frame's stateSort()/setVisible() outcome -- only FORCE_INVISIBLE
    // (checked by rebuildGeom() itself) actually excludes a drawable from
    // that batch. Driving the flag from this always-live per-frame check
    // (rather than a one-shot call) is what makes it self-heal against the
    // sim's routine object-update traffic silently clearing the flag
    // (LLViewerObject::processUpdateMessage()), and needs no explicit
    // restore step when isolate mode turns off -- the flag just stops being
    // re-asserted and a rebuild is forced to reflect that immediately.
    void updateDrawableHiddenState(LLDrawable* drawable);

    // Called by ASFloaterMyLight right after setActive(false, ...) turns
    // isolate mode off. A hidden drawable only gets revisited by
    // updateDrawableHiddenState() (and so un-hidden) when its owning
    // spatial group is next traversed -- for a group that isn't otherwise
    // becoming geometry/LOD-dirty (e.g. a static build with the camera not
    // moving), that might not happen on its own for a long time. This
    // explicitly un-hides every drawable this module hid, so turning
    // isolate mode off is never dependent on incidental group traversal.
    void restoreAllHiddenDrawables();
}

#endif
