/**
 * @file aslightrigrenderer.h
 * @author chanayane@firestorm
 * @brief Direct deferred-light backend for My Lights, with no viewer objects.
 */

#ifndef AS_LIGHTRIGRENDERER_H
#define AS_LIGHTRIGRENDERER_H

#include <vector>

#include "v3color.h"
#include "v3math.h"

class LLPipeline;

class ASLightRigRenderer
{
public:
    struct Light
    {
        LLVector3 position_agent;
        LLColor3 color_srgb;
        F32 intensity;
        F32 radius;
        F32 falloff;
    };

    static bool usesShaderBackend();

    // Replaces the small CPU-side snapshot consumed by the render pass.
    static void setLights(const std::vector<Light>& lights);

    // Adds the snapshot through the viewer's stock deferred point-light
    // shaders. The caller already has the deferred light target bound.
    static void render(LLPipeline& pipeline, F32 light_scale);

    // Reserves hardware-light slots for the same synthetic lights so forward
    // transparency receives the contribution LLVOVolume lights normally get
    // through LLPipeline::mNearbyLights. Returns the next free slot.
    static S32 appendForwardLights(LLPipeline& pipeline, S32 first_slot, F32 light_scale);
};

#endif // AS_LIGHTRIGRENDERER_H
