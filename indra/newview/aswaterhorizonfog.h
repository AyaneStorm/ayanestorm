/**
 * @file aswaterhorizonfog.h
 * @author chanayane@firestorm
 * @brief AyaneStorm water-horizon fog uniform setup.
 */

#ifndef AS_WATER_HORIZON_FOG_H
#define AS_WATER_HORIZON_FOG_H

class LLGLSLShader;

namespace ASWaterHorizonFog
{
    // Upload viewer-local fog controls while the water shader is bound.
    void uploadUniforms(LLGLSLShader& shader);
}

#endif
