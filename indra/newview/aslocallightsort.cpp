/**
 * @file aslocallightsort.cpp
 * @author Kissmy Spicoli
 * @brief Optional subject-based priority for deferred local lights.
 */
#include "llviewerprecompiledheaders.h"
#include "aslocallightsort.h"

#include <algorithm>
#include <cfloat>

#include "llagent.h"
#include "llagentcamera.h"
#include "lldrawable.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvovolume.h"

namespace
{
    // Last Alt-click pick. Kept relative to the picked object so the
    // subject follows a moving object or avatar.
    struct Subject
    {
        bool valid = false;
        LLUUID object_id;         // null for land hits
        LLVector3 local_offset;   // hit in the object's render frame
        LLVector3d pos_global;    // last known hit, used if the object is gone
    };
    Subject sSubject;

    // Current subject in agent coordinates. False when there is none.
    bool get_subject_agent(LLVector3& out)
    {
        if (!sSubject.valid)
        {
            return false;
        }
        if (gAgentCamera.getFocusOnAvatar())
        {
            // Focus returned to the avatar (walk, Esc, camera reset): drop it.
            sSubject.valid = false;
            return false;
        }
        if (sSubject.object_id.notNull())
        {
            LLViewerObject* obj = gObjectList.findObject(sSubject.object_id);
            if (obj && !obj->isDead())
            {
                out = obj->getRenderPosition() + sSubject.local_offset * obj->getRenderRotation();
                sSubject.pos_global = gAgent.getPosGlobalFromAgent(out);
                return true;
            }
        }
        // Converted at use time so region crossings do not skew it.
        out = gAgent.getPosAgentFromGlobal(sSubject.pos_global);
        return true;
    }

    // calc_light_dist() measured from the subject instead of the camera.
    F32 subject_light_dist(LLDrawable* drawable, const LLVector3& subject)
    {
        LLVOVolume* light = drawable->getVOVolume();
        if (!light || light->getLightIntensity() < .001f)
        {
            return FLT_MAX; // contributes nothing: last
        }
        if (light->isSelected())
        {
            return 0.f; // selected lights get highest priority
        }
        F32 radius = light->getLightRadius();
        F32 dist = llmax(dist_vec(light->getRenderPosition(), subject) - radius, 0.f);
        if (drawable->isState(LLDrawable::ACTIVE))
        {
            // moving lights get a little higher priority, as upstream
            dist = llmax(dist - radius * 0.25f, 0.f);
        }
        return dist;
    }
}

bool ASLocalLightSort::enabled()
{
    static LLCachedControl<bool> subject_sort(gSavedSettings, "ASRenderLocalLightSubjectSort", false);
    return subject_sort;
}

void ASLocalLightSort::setSubject(const LLPickInfo& pick)
{
    if (pick.mPosGlobal.isExactlyZero())
    {
        return; // sky or invalid point: keep the current subject
    }
    sSubject.valid = true;
    sSubject.pos_global = pick.mPosGlobal; // ray hit under the pointer
    sSubject.object_id.setNull();
    sSubject.local_offset.clearVec();

    LLPointer<LLViewerObject> obj = pick.getObject();
    if (obj.notNull() && !obj->isDead())
    {
        LLVector3 hit_agent = gAgent.getPosAgentFromGlobal(pick.mPosGlobal);
        sSubject.object_id = obj->getID();
        sSubject.local_offset = (hit_agent - obj->getRenderPosition()) * ~obj->getRenderRotation();
    }
}

void ASLocalLightSort::order(const LLPipeline::light_set_t& lights,
                             std::vector<const LLPipeline::Light*>& out)
{
    out.clear();
    out.reserve(lights.size());

    if (!enabled())
    {
        for (const LLPipeline::Light& light : lights)
        {
            out.push_back(&light);
        }
        return;
    }

    LLVector3 subject;
    if (!get_subject_agent(subject))
    {
        subject = LLViewerCamera::getInstance()->getOrigin(); // no subject: upstream order
    }

    // Local to this member so it may name the protected LLPipeline::Light.
    struct Entry
    {
        bool fading_out; // out of camera range: after every in-range light
        F32 dist;
        const LLPipeline::Light* light;
    };

    std::vector<Entry> entries;
    entries.reserve(lights.size());
    for (const LLPipeline::Light& light : lights)
    {
        entries.push_back({ light.fade < 0.f, subject_light_dist(light.drawable, subject), &light });
    }

    // Same tie-break as Light::compare: drawable address.
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b)
              {
                  if (a.fading_out != b.fading_out)
                  {
                      return !a.fading_out;
                  }
                  if (a.dist != b.dist)
                  {
                      return a.dist < b.dist;
                  }
                  return a.light->drawable < b.light->drawable;
              });

    for (const Entry& entry : entries)
    {
        out.push_back(entry.light);
    }
}
