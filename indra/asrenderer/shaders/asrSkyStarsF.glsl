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
    // Without an image, a soft round dot, as EEP's default star image is.
    const float r = min(length(vary_uv - 0.5) * 2.0, 1.0);
    vec4 image = vec4(1.0, 1.0, 1.0, (1.0 - r * r) * (1.0 - r * r));
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
    // Stars are lost in the sunlit sky behind them as the eye loses them at dawn (user: stars
    // over a sky already lit by the sun), from STARS_LOST_FROM to STARS_LOST_BY cd/m2, between
    // nautical and civil twilight, so any EEP's stars leave by day. The sun's light only: the
    // EEP moon is far brighter than a real one and would drown every moonlit night's stars.
    const float STARS_LOST_FROM = 0.02, STARS_LOST_BY = 2.0;
    const float SKY_CANDELAS = 120000.0; // a radiance of 1 in the sky's units, in cd/m2
    const vec3 behind = twilightSunSky(a, ExposureRef(view.exposure).twilightBand, vary_direction);
    const float candelas = max(dot(behind, vec3(0.2126, 0.7152, 0.0722)) * SKY_CANDELAS, 1e-9);
    const float shown = 1.0 - smoothstep(log(STARS_LOST_FROM), log(STARS_LOST_BY), log(candelas));
    frag_color = vec4(min(image.rgb * image.a * vary_light * air * shown, vec3(60000.0)), 0.0);
}
