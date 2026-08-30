/**
 * @file ashorizonscattering.h
 * @author chanayane@firestorm
 * @brief Viewer-local analytic horizon-scattering sky layer.
 */

#ifndef AS_HORIZON_SCATTERING_H
#define AS_HORIZON_SCATTERING_H

#include "llglslshader.h"

#include <vector>

namespace ASHorizonScattering
{
    enum BlendMode
    {
        BLEND_ADDITIVE = 0,
        BLEND_ATMOSPHERIC = 1,
        BLEND_REPLACE = 2
    };

    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();

    // Owns configuration, dome geometry, transform, and blend state. The
    // caller supplies only whether the established sky path selected HDRI.
    void render(bool hdri_sky_active);
    // Returns an AS-owned viewer-cloud shader only while horizon scattering
    // is active; otherwise the caller retains the original EEP cloud shader.
    LLGLSLShader* getCloudShader(bool hdri_sky_active);
    BlendMode getBlendMode();

}

#endif // AS_HORIZON_SCATTERING_H
