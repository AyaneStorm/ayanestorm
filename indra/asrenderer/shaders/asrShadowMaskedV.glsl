/**
 * asrShadowMaskedV.glsl
 * Shadow casters with holes: masked and blended surfaces. See asrSceneVisibilityVertex.glsl.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#define SHADOW
#define MASKED
#include "asrSceneVisibilityVertex.glsl"
