/**
 * asrSkyStarsV.glsl
 * The stars: one sprite per star of the viewer's catalogue (ASStars), six vertices an
 * instance. A star behind the planet, the moon's disc or the sun's is not drawn; the rest lie
 * on the far plane, so the depth test keeps them where nothing else was drawn.
 * Sprites face the screen and are at least STAR_MIN_PIXELS wide, dimmed by the area they
 * gained, so a star smaller than a pixel never falls between pixel centres and blinks out.
 * Author: chanayane@firestorm
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "asrSceneCommon.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];
#include "asrAtmosphere.glsl"

const float STAR_MIN_PIXELS = 2.0;

layout(location = 0) out vec2 vary_uv;
layout(location = 1) flat out vec3 vary_light;     // the star's colour times its level, before the air
layout(location = 2) flat out vec3 vary_direction; // world
layout(location = 3) flat out float vary_pixels;   // the sprite's side in pixels

void main()
{
    ViewRef view = ViewRef(push.view);
    const Atmosphere a = loadAtmosphere(view.atmosphere);
    AtmosphereRef ref = AtmosphereRef(view.atmosphere);
    const SkyStar star = SkyStarsRef(ref.stars).star[gl_InstanceIndex];
    const vec3 direction = normalize(a.starRotation * star.direction.xyz);
    vary_uv = vec2(0.0);
    vary_light = vec3(0.0);
    vary_direction = direction;
    vary_pixels = 1.0;

    // OpenGL's twinkle: fract(x + y) of the star's position on its dome, scaled by a phase
    // that jumps every frame.
    const float twinkle = mix(1.0, fract((star.direction.x + star.direction.y) * ref.starRotation[0].w), ref.moonHalo.w);
    const float level = ref.moonHalo.z * star.color.w * twinkle / ExposureRef(view.exposure).exposure;
    const bool hidden = level <= 0.0
                     || raySphereIntersectNearest(atmosphereCamera(a), direction, a.bottomRadius) >= 0.0
                     || dot(direction, a.moonDirection) > cos(a.moonAngularRadius)
                     || dot(direction, a.sunDirection) > a.sunCosRadius;
    vec4 position = view.viewProjection * vec4(direction, 0.0);
    if (hidden || position.w <= 0.0)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0); // every corner on one point: nothing drawn
        return;
    }

    const float pixelAngle = 2.0 / (view.pixelSize.x * view.projection.x);
    const float side = star.direction.w;
    const float drawn = max(side, STAR_MIN_PIXELS * pixelAngle);
    const uint corner = uint(gl_VertexIndex);
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    const vec2 uv = corners[corner];
    position.xy += (uv - 0.5) * drawn * view.projection.xy * position.w;
    position.z = 0.0; // the far plane, in reversed depth
    gl_Position = position;

    vary_uv = uv;
    vary_light = star.color.rgb * level * (side * side) / (drawn * drawn);
    vary_pixels = drawn / pixelAngle;
}
