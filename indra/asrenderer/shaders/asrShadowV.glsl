/**
 * asrShadowV.glsl
 * Solid shadow casters, into every shadow view in one draw. See asrSceneVisibilityVertex.glsl.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#define SHADOW
#include "asrSceneVisibilityVertex.glsl"
