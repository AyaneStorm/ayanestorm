/**
 * @file aslocallightsort.h
 * @author Kissmy Spicoli
 * @brief Optional subject-based priority for deferred local lights.
 *
 * LLPipeline::mNearbyLights is ordered by distance from the camera to the
 * edge of each light's radius, and renderDeferredLighting() renders the
 * first RenderLocalLightCount entries. With ASRenderLocalLightSubjectSort
 * on, the cap is applied in order of distance from the subject instead,
 * so lights around the subject win over lights around the camera.
 *
 * The subject is the point hit by the Alt-click (or Camera tool) pick ray
 * through the pointer (LLPickInfo::mPosGlobal), not the camera focus point:
 * LLAgentCamera::setFocusGlobal() replaces the hit on objects with a point
 * near the object's center (calcFocusOffset()). The hit is kept relative to
 * the picked object so it follows it, and lasts while the camera focus is
 * off-avatar. Without a subject the camera origin is used (upstream order).
 *
 * Candidate selection, range test and fading stay camera-based: lights
 * fading out of camera range (fade < 0) always sort after in-range lights.
 * The score mirrors calc_light_dist(): max(|pos - subject| - radius, 0),
 * selected lights 0, moving lights minus 0.25 * radius, dim lights last.
 */
#ifndef AS_LOCAL_LIGHT_SORT_H
#define AS_LOCAL_LIGHT_SORT_H

#include <vector>

#include "pipeline.h"

class LLPickInfo;

// A class (not a namespace) so LLPipeline can befriend it: Light and
// light_set_t are protected.
class ASLocalLightSort
{
public:
    // ASRenderLocalLightSubjectSort. When true, the caller also counts only
    // lights that pass its off-screen and dark checks against the cap.
    static bool enabled();

    // Records the Alt-click / Camera tool pick as the subject. Called from
    // LLToolCamera::pickCallback() where it moves the camera focus.
    static void setSubject(const LLPickInfo& pick);

    // Fills out with the entries of lights in render-priority order: set
    // order (camera distance) when disabled, subject distance when enabled.
    static void order(const LLPipeline::light_set_t& lights,
                      std::vector<const LLPipeline::Light*>& out);
};

#endif // AS_LOCAL_LIGHT_SORT_H
