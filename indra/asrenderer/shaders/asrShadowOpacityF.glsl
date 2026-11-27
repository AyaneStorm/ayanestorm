/**
 * asrShadowOpacityF.glsl
 * A shadow caster's partly covered texels (hair, lace, leaf edges), into the opacity map.
 * See asrShadowAlpha.glsl.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#define OPACITY
#include "asrShadowAlpha.glsl"
