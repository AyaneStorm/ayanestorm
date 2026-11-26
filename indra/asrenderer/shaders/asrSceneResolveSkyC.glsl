// The resolve over tiles holding sky: where nothing was drawn.
#version 460
#extension GL_GOOGLE_include_directive : require
#define RESOLVE_KIND 0 // RESOLVE_KIND_SKY
#include "asrSceneResolve.glsl"
