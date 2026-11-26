/**
 * asrSkyClouds.glsl
 * EEP's 2D clouds: OpenGL's cloud shapes (cloudsF.glsl) where a view ray meets OpenGL's cloud
 * dome, lit by this sky: sun and moon after the air above the layer, and the sky's own light.
 * The low tier, and the fallback when the volumetric layer is off. Included after
 * asrAtmosphere.glsl by shaders that declare textures[] and samplers[].
 */

// Where a ray from the camera meets the dome: a sphere whose top lies the layer height above
// the camera and whose centre lies dome_offset radii below it (ASWorldClouds::radius()). Its
// directions from the centre are OpenGL's, whatever the radius, so its UVs are too.
struct CloudHit
{
    vec2 uv;        // cloudsV.glsl's vary_texcoord0
    float fade;     // cloudsF.glsl's altitude blend towards the horizon
    float distance; // km from the camera
};

CloudHit cloudHit(Atmosphere a, vec3 direction)
{
    const float radius = a.cloudPos1.w;
    const float below = a.cloudShape.w * radius;
    const float t = -below * direction.z + sqrt(max(radius * radius - below * below * (1.0 - direction.z * direction.z), 0.0));
    const vec3 n = (direction * t + vec3(0.0, 0.0, below)) / radius;
    CloudHit hit;
    // llvowlsky.cpp maps the dome by ((1 - z) / 2, (1 - x) / 2) of its own axes; turned into
    // the world (lldrawpoolwlsky.cpp), its z is world x and its x world y. cloudsV.glsl then
    // flips u, scales about the centre and adds the world offset.
    const vec2 tc = vec2(-(1.0 - n.x) * 0.5, (1.0 - n.y) * 0.5);
    hit.uv = (tc - 0.5) / a.cloudScale + 0.5 + a.cloudWorldOffset;
    const float above = a.cloudPos2.w * (n.z - a.cloudShape.w) + 50.0; // rel_pos.y, in EEP's dome units
    hit.fade = above < 0.0 ? 0.0 : clamp((above + 512.0) / a.cloudShape.z, 0.0, 1.0);
    hit.distance = t;
    return hit;
}

// The steps of hit.uv one pixel across and down, a pixel being pixelAngle radians on the sky.
void cloudSteps(Atmosphere a, vec3 direction, float pixelAngle, CloudHit hit, out vec2 uvDx, out vec2 uvDy)
{
    const vec3 side = abs(direction.z) < 0.999 ? normalize(cross(direction, vec3(0.0, 0.0, 1.0))) : vec3(1.0, 0.0, 0.0);
    const vec3 rise = cross(side, direction);
    uvDx = cloudHit(a, normalize(direction + side * pixelAngle)).uv - hit.uv;
    uvDy = cloudHit(a, normalize(direction + rise * pixelAngle)).uv - hit.uv;
}

float cloudNoise(Atmosphere a, vec2 uv, vec2 uvDx, vec2 uvDy)
{
    const uint bits = a.cloudTexture;
    float n = textureGrad(sampler2D(textures[nonuniformEXT(bits & 0xFFFFFu)], samplers[nonuniformEXT((bits >> 28) & 3u)]), uv, uvDx, uvDy).x;
    if (a.cloudBlend > 0.0)
    {
        const uint next = a.cloudTextureNext;
        n = mix(n, textureGrad(sampler2D(textures[nonuniformEXT(next & 0xFFFFFu)], samplers[nonuniformEXT((next >> 28) & 3u)]), uv, uvDx, uvDy).x, a.cloudBlend);
    }
    return n;
}

// cloudsF.glsl's shape, line for line. cover: its alpha1, how much of the background the cloud
// hides. shadow: its alpha2, the share of the direct light the cloud takes from itself; only
// read with withShadow.
struct CloudShape
{
    float cover;
    float shadow;
};

CloudShape cloudShape(Atmosphere a, CloudHit hit, vec2 uvDx, vec2 uvDy, bool withShadow)
{
    vec2 uv1 = hit.uv;
    vec2 uv2 = hit.uv + a.cloudShadowOffset;
    vec2 uv3 = uv1 * 16.0;
    const vec2 uv4 = uv2 * 16.0;
    const float variance = a.cloudShape.x * (1.0 - a.cloudScale * 0.25);
    const vec2 disturbance = vec2(cloudNoise(a, uv1 / 8.0, uvDx / 8.0, uvDy / 8.0),
                                  cloudNoise(a, (uv3 + uv1) / 16.0, uvDx * (17.0 / 16.0), uvDy * (17.0 / 16.0))) * variance;
    const vec2 disturbance2 = vec2(cloudNoise(a, (uv1 + uv3) / 4.0, uvDx * (17.0 / 4.0), uvDy * (17.0 / 4.0)),
                                   cloudNoise(a, (uv4 + uv2) / 8.0, uvDx * (17.0 / 8.0), uvDy * (17.0 / 8.0))) * variance;
    uv1 += a.cloudPos1.xy + disturbance * 0.2;
    uv2 += a.cloudPos1.xy;
    uv3 += a.cloudPos2.xy;
    const float densityVariance = min(1.0, (disturbance.x * 2.0 + disturbance.y * 2.0 + disturbance2.x + disturbance2.y) * 4.0);
    const float density = a.cloudShape.y * (1.0 - densityVariance * densityVariance);

    CloudShape shape;
    float alpha1 = (cloudNoise(a, uv1, uvDx, uvDy) - 0.5) + (cloudNoise(a, uv3, uvDx * 16.0, uvDy * 16.0) - 0.5) * a.cloudPos2.z;
    alpha1 = min(max(alpha1 + density, 0.0) * 10.0 * a.cloudPos1.z, 1.0);
    alpha1 = 1.0 - alpha1 * alpha1;
    alpha1 = 1.0 - alpha1 * alpha1;
    shape.cover = clamp(alpha1 * hit.fade, 0.0, 1.0);
    shape.shadow = 0.0;
    if (withShadow && shape.cover > 0.0)
    {
        float alpha2 = cloudNoise(a, uv2, uvDx, uvDy) - 0.5;
        alpha2 = min(max(alpha2 + density, 0.0) * 2.5 * a.cloudPos1.z, 1.0);
        alpha2 = 1.0 - alpha2;
        shape.shadow = 1.0 - alpha2 * alpha2;
    }
    return shape;
}

float henyeyGreenstein(float g, float cosTheta)
{
    return (1.0 - g * g) / (4.0 * ATMOSPHERE_PI * pow(1.0 + g * g - 2.0 * g * cosTheta, 1.5));
}

// What one light gives the cloud at `point` towards the camera, per unit of the light's
// illuminance: the air above the layer, the planet's shadow, and two lobes (Frostbite's): a
// strong one forward, the silver lining towards the light, and a weak one back. A sheet is
// lit, not filled, so the phase is taken four times: an even phase then gives a Lambert
// surface's 1 / pi.
// The planet's shadow fades over the light's disc as it sets for that point
// (lightAboveHorizon), instead of cutting the light at its centre, which drew a hard dark
// band up the clouds at sunset.
vec3 cloudLightFrom(Atmosphere a, vec3 point, vec3 light, float radius, vec3 direction)
{
    const float c = dot(direction, light);
    const float phase = mix(henyeyGreenstein(-0.3, c), henyeyGreenstein(0.8, c), 0.7);
    return lightThroughAir(a, point, light, radius) * (4.0 * phase);
}

// The cloud laid over `behind` (the sky with what lies beyond the air) seen from the camera.
// The air in front dims the cloud and adds its own light: the share of `sky` (the sky's light
// alone in this direction) that lies before the cloud, by how much of the ray's extinction
// does. skyIrradiance: the sky's light on a horizontal surface at the camera (GpuExposure),
// the cloud's ambient.
const int CLOUD_FRONT_STEPS = 8;

vec3 cloudOver(Atmosphere a, vec3 direction, CloudHit hit, CloudShape shape, vec3 sky, vec3 behind, vec3 skyIrradiance)
{
    const vec3 camera = atmosphereCamera(a);
    const vec3 point = camera + direction * hit.distance;
    const vec3 direct = cloudLightFrom(a, point, a.sunDirection, acos(a.sunCosRadius), direction) * a.sunIlluminance
                      + cloudLightFrom(a, point, a.moonDirection, a.moonAngularRadius, direction) * a.moonSkyLight;
    const vec3 cloud = a.cloudAlbedo * (direct * (1.0 - shape.shadow) + skyIrradiance / ATMOSPHERE_PI);

    // The air in front, marched: a few km at most. Dividing two transmittance table reads
    // instead banded near the horizon, where the table's rows meet.
    const float step = hit.distance / float(CLOUD_FRONT_STEPS);
    vec3 depth = vec3(0.0);
    for (int i = 0; i < CLOUD_FRONT_STEPS; ++i)
    {
        depth += sampleMedium(a, camera + direction * (step * (float(i) + 0.5))).extinction;
    }
    const vec3 front = exp(-depth * step);
    const vec3 whole = sampleTransmittance(a, length(camera), direction.z);
    const vec3 air = sky * clamp((1.0 - front) / max(1.0 - whole, vec3(1e-4)), 0.0, 1.0);
    return mix(behind, air + front * cloud, shape.cover);
}
