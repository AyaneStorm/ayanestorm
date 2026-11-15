/**
 * asrSkyStarsF.glsl
 * A star's sprite: EEP's star image times the star's light, dimmed by the air along its
 * direction, added to the lit image. Alpha adds nothing, so the glow stays as it was.
 * Author: chanayane@firestorm
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "asrSceneCommon.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];
#include "asrAtmosphere.glsl"

layout(location = 0) in vec2 vary_uv;
layout(location = 1) flat in vec3 vary_light;
layout(location = 2) flat in vec3 vary_direction;
layout(location = 3) flat in float vary_pixels;

layout(location = 0) out vec4 frag_color;

void main()
{
    ViewRef view = ViewRef(push.view);
    const Atmosphere a = loadAtmosphere(view.atmosphere);
    const uint bits = AtmosphereRef(view.atmosphere).starTexture;
    vec4 image = vec4(1.0);
    if (bits != NO_SKY_TEXTURE)
    {
        const uint slot = bits & 0xFFFFFu;
        const vec2 uvDx = vec2(1.0 / vary_pixels, 0.0);
        const vec2 uvDy = vec2(0.0, 1.0 / vary_pixels);
        giveFeedback(ivec2(gl_FragCoord.xy), slot, uvDx, uvDy);
        image = textureGrad(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT((bits >> 28) & 3u)]), vary_uv, uvDx, uvDy);
        image.rgb = srgbToLinear(image.rgb);
    }
    const vec3 air = sampleTransmittance(a, length(atmosphereCamera(a)), vary_direction.z);
    frag_color = vec4(min(image.rgb * image.a * vary_light * air, vec3(60000.0)), 0.0);
}
