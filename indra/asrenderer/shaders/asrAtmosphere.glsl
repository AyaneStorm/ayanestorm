/**
 * asrAtmosphere.glsl
 * The sky's shared functions: the medium, the lookup table mappings and the ray march every
 * table is made with. Include after asrSceneCommon.glsl, with set 0's samplers declared.
 *
 * Ported from the sample code of Hillaire, 'A Scalable and Production Ready Sky and
 * Atmosphere Rendering Technique', 2020 (RenderSkyCommon.hlsl, RenderSkyRayMarching.hlsl,
 * SkyAtmosphereCommon.hlsl). Its licence:
 *
 * MIT License. Copyright (c) 2020 Epic Games, Inc.
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the 'Software'), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
 * to whom the Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Space: kilometres, Z up, the planet's centre at the origin. The camera sits on the Z axis
 * at its height: within SL's distances the planet's curve under the region is nothing, so
 * world directions are used as they are.
 */

// Sizes: asratmosphere.h.
const vec2 TRANSMITTANCE_LUT_SIZE = vec2(256.0, 64.0);
const float MULTISCATTER_LUT_SIZE = 32.0;
const vec2 SKY_VIEW_LUT_SIZE = vec2(192.0, 108.0);
const float AERIAL_LUT_SIZE = 32.0;
const uint NO_SKY_TEXTURE = 0xFFFFFFFFu;
const float PLANET_RADIUS_OFFSET = 0.01; // km
const float ATMOSPHERE_PI = 3.14159265358979;

// Read by the passes after the tables are made; written by asrAtmosphere*C.glsl at 11 to 14.
layout(set = 1, binding = 15) uniform texture2D transmittanceLut;
layout(set = 1, binding = 16) uniform texture2D multiscatterLut;
layout(set = 1, binding = 17) uniform texture2D skyViewLut;
layout(set = 1, binding = 18) uniform texture3D aerialLut;

// GpuAtmosphere in asratmosphere.h.
layout(buffer_reference, std430) readonly buffer AtmosphereRef
{
    vec4 rayleighScattering;   // w: bottom radius
    vec4 mieScattering;        // w: top radius
    vec4 mieExtinction;        // w: Mie g
    vec4 absorptionExtinction; // w: camera height
    vec4 groundAlbedo;         // w: multiple scattering factor
    vec4 sunDirection;         // w: cosine of the disc's radius
    vec4 sunIlluminance;       // w: the sun's texture bits
    vec4 profiles[9];          // Rayleigh, Mie, absorption: layer 0, layer 1, (width, -, -, -)
    vec4 sunUp;                // w: half the sun square's side, radians
    vec4 moonDirection;        // w: cosine of the radius past which nothing of the moon lies
    vec4 moonUp;               // w: the moon's angular radius
    vec4 moonIlluminance;      // w: the moon's texture bits
    vec4 aerial;               // x: the aerial volume's far edge, km; y: moon normal map bits; z: moon is a
                               // sphere; w: the moon's light on the sky against full moon
    vec4 moonLight;            // where the sphere is lit from; w: terminator softness
    vec4 moonLook;             // earthshine, relief, halo strength, halo radius
    vec4 moonHalo;             // x: halo softness, y: disc brightness against the exposure,
                               // z: star brightness against the exposure, w: twinkle
    vec4 starRotation[3];      // star frame to world, by columns; w: twinkle phase, Milky Way
                               // level, deep sky level
    uint64_t stars;            // SkyStar list (asratmosphere.h)
    uint starCount;
    uint starTexture;
    uint milkyWayTexture;
    uint deepSkyTexture;
    float milkyWaySaturation;
    float moonHeightScale;     // the height map's range in moon radii
    vec4 moonEarthshineColor;
    vec4 moonTint;             // over the disc and its halo
    vec4 sunGlow;              // EEP glow: size, focus
    uint cloudTexture;
    uint cloudTextureNext;
    float cloudBlend;
    float cloudScale;          // 0: no clouds
    vec4 cloudShape;           // variance, density, max_y, dome offset
    vec4 cloudPos1;            // xy, z density, w: the layer's sphere radius, km
    vec4 cloudPos2;            // xy, z density, w: EEP dome radius
    vec4 cloudColor;           // rgb: albedo
    vec4 cloudOffsets;         // xy: world offset, zw: self-shadow step
};

// SkyStar in asratmosphere.h.
struct SkyStar
{
    vec4 direction; // w: the sprite's side, radians
    vec4 color;     // w: brightness against the others
};
layout(buffer_reference, std430) readonly buffer SkyStarsRef
{
    SkyStar star[];
};

struct Atmosphere
{
    vec3 rayleighScattering;
    vec3 mieScattering;
    vec3 mieExtinction;
    vec3 mieAbsorption;
    vec3 absorptionExtinction;
    vec3 groundAlbedo;
    vec3 sunDirection;
    vec3 sunIlluminance;
    float bottomRadius;
    float topRadius;
    float mieG;
    float cameraHeight;
    float multipleScattering;
    float sunCosRadius;
    vec4 profiles[9];
    uint sunTexture;
    float sunTextureRadius;
    vec3 sunUp;
    uint moonTexture;
    vec3 moonDirection;
    vec3 moonUp;
    vec3 moonIlluminance;
    float moonCosRadius;
    float moonAngularRadius;
    float aerialDistance;
    uint moonNormalTexture;
    bool moonSphere;
    vec3 moonSkyLight; // what the moon gives the sky at its phase
    float moonPhaseLight; // that against full moon
    vec3 moonLightDirection;
    float moonTerminatorSoftness;
    float moonEarthshine;
    float moonRelief;
    float moonHaloStrength;
    float moonHaloRadius;
    float moonHaloSoftness;
    float moonBrightness;
    mat3 starRotation;  // star frame to world
    float milkyWayLevel;
    float deepSkyLevel;
    float milkyWaySaturation;
    uint milkyWayTexture;
    uint deepSkyTexture;
    float moonHeightScale;
    vec3 moonEarthshineColor;
    vec3 moonTint;
    vec2 sunGlow;
    uint cloudTexture;
    uint cloudTextureNext;
    float cloudBlend;
    float cloudScale;
    vec4 cloudShape;
    vec4 cloudPos1;
    vec4 cloudPos2;
    vec3 cloudAlbedo;
    vec2 cloudWorldOffset;
    vec2 cloudShadowOffset;
};

Atmosphere loadAtmosphere(uint64_t address)
{
    AtmosphereRef ref = AtmosphereRef(address);
    Atmosphere a;
    a.rayleighScattering = ref.rayleighScattering.xyz;
    a.mieScattering = ref.mieScattering.xyz;
    a.mieExtinction = ref.mieExtinction.xyz;
    a.mieAbsorption = max(ref.mieExtinction.xyz - ref.mieScattering.xyz, vec3(0.0));
    a.absorptionExtinction = ref.absorptionExtinction.xyz;
    a.groundAlbedo = ref.groundAlbedo.xyz;
    a.sunDirection = ref.sunDirection.xyz;
    a.sunIlluminance = ref.sunIlluminance.xyz;
    a.bottomRadius = ref.rayleighScattering.w;
    a.topRadius = ref.mieScattering.w;
    a.mieG = ref.mieExtinction.w;
    a.cameraHeight = ref.absorptionExtinction.w;
    a.multipleScattering = ref.groundAlbedo.w;
    a.sunCosRadius = ref.sunDirection.w;
    for (int i = 0; i < 9; ++i)
    {
        a.profiles[i] = ref.profiles[i];
    }
    a.sunTexture = floatBitsToUint(ref.sunIlluminance.w);
    a.sunTextureRadius = ref.sunUp.w;
    a.sunUp = ref.sunUp.xyz;
    a.moonTexture = floatBitsToUint(ref.moonIlluminance.w);
    a.moonDirection = ref.moonDirection.xyz;
    a.moonUp = ref.moonUp.xyz;
    a.moonIlluminance = ref.moonIlluminance.xyz;
    a.moonCosRadius = ref.moonDirection.w;
    a.moonAngularRadius = ref.moonUp.w;
    a.aerialDistance = ref.aerial.x;
    a.moonNormalTexture = floatBitsToUint(ref.aerial.y);
    a.moonSphere = floatBitsToUint(ref.aerial.z) != 0u;
    a.moonSkyLight = ref.moonIlluminance.xyz * ref.aerial.w;
    a.moonPhaseLight = ref.aerial.w;
    a.moonLightDirection = ref.moonLight.xyz;
    a.moonTerminatorSoftness = ref.moonLight.w;
    a.moonEarthshine = ref.moonLook.x;
    a.moonRelief = ref.moonLook.y;
    a.moonHaloStrength = ref.moonLook.z;
    a.moonHaloRadius = ref.moonLook.w;
    a.moonHaloSoftness = ref.moonHalo.x;
    a.moonBrightness = ref.moonHalo.y;
    a.starRotation = mat3(ref.starRotation[0].xyz, ref.starRotation[1].xyz, ref.starRotation[2].xyz);
    a.milkyWayLevel = ref.starRotation[1].w;
    a.deepSkyLevel = ref.starRotation[2].w;
    a.milkyWaySaturation = ref.milkyWaySaturation;
    a.milkyWayTexture = ref.milkyWayTexture;
    a.deepSkyTexture = ref.deepSkyTexture;
    a.moonHeightScale = ref.moonHeightScale;
    a.moonEarthshineColor = ref.moonEarthshineColor.xyz;
    a.moonTint = ref.moonTint.xyz;
    a.sunGlow = ref.sunGlow.xy;
    a.cloudTexture = ref.cloudTexture;
    a.cloudTextureNext = ref.cloudTextureNext;
    a.cloudBlend = ref.cloudBlend;
    a.cloudScale = ref.cloudScale;
    a.cloudShape = ref.cloudShape;
    a.cloudPos1 = ref.cloudPos1;
    a.cloudPos2 = ref.cloudPos2;
    a.cloudAlbedo = ref.cloudColor.rgb;
    a.cloudWorldOffset = ref.cloudOffsets.xy;
    a.cloudShadowOffset = ref.cloudOffsets.zw;
    return a;
}

// A pixel's direction in world space, from the camera through ndc.
vec3 viewDirection(ViewRef view, vec2 ndc)
{
    const vec3 camera = vec3(ndc.x / view.projection.x, -ndc.y / view.projection.y, -1.0);
    return normalize(camera.x * view.viewRows[0].xyz + camera.y * view.viewRows[1].xyz + camera.z * view.viewRows[2].xyz);
}

// The aerial perspective volume's slice z holds the air up to this distance, in km: its
// centre, spread as the square (asratmosphere.h).
float aerialSliceDistance(Atmosphere a, float z)
{
    const float s = (z + 0.5) / AERIAL_LUT_SIZE;
    return s * s * a.aerialDistance;
}

// The air between the camera and a point `metres` away seen at screen position uv
// (0 to 1 over the view), laid over its colour: the volume holds the light of sun and moon
// at their own brightness. Nearer than the first slice's centre fades to nothing, as the
// sample does.
vec3 applyAerial(Atmosphere a, vec3 color, vec2 uv, float metres)
{
    float slice = sqrt(metres * 0.001 / a.aerialDistance) * AERIAL_LUT_SIZE;
    float weight = 1.0;
    if (slice < 0.5)
    {
        weight = clamp(slice * 2.0, 0.0, 1.0);
        slice = 0.5;
    }
    const vec4 air = textureLod(sampler3D(aerialLut, samplers[0]), vec3(uv, slice / AERIAL_LUT_SIZE), 0.0);
    return color * mix(1.0, air.a, weight) + air.rgb * weight;
}

// The camera in atmosphere space.
vec3 atmosphereCamera(Atmosphere a)
{
    return vec3(0.0, 0.0, a.bottomRadius + a.cameraHeight);
}

// Bruneton's density profile: the first layer below its width, the second above.
float profileDensity(Atmosphere a, int profile, float height)
{
    const vec4 layer = height < a.profiles[profile * 3 + 2].x ? a.profiles[profile * 3] : a.profiles[profile * 3 + 1];
    return clamp(layer.x * exp(layer.y * height) + layer.z * height + layer.w, 0.0, 1.0);
}

// Distance to the nearest hit of a sphere at the origin ahead of the ray, or -1.
float raySphereIntersectNearest(vec3 origin, vec3 direction, float radius)
{
    const float b = 2.0 * dot(direction, origin);
    const float c = dot(origin, origin) - radius * radius;
    const float delta = b * b - 4.0 * c;
    if (delta < 0.0)
    {
        return -1.0;
    }
    const float sol0 = (-b - sqrt(delta)) * 0.5;
    const float sol1 = (-b + sqrt(delta)) * 0.5;
    if (sol0 < 0.0 && sol1 < 0.0)
    {
        return -1.0;
    }
    if (sol0 < 0.0)
    {
        return max(0.0, sol1);
    }
    if (sol1 < 0.0)
    {
        return max(0.0, sol0);
    }
    return max(0.0, min(sol0, sol1));
}

float fromUnitToSubUvs(float u, float resolution) { return (u + 0.5 / resolution) * (resolution / (resolution + 1.0)); }
float fromSubUvsToUnit(float u, float resolution) { return (u - 0.5 / resolution) * (resolution / (resolution - 1.0)); }

// Transmittance table (Bruneton 2017): x the view's zenith angle, y the height, both mapped
// so the horizon and the ground get the most texels.
void uvToTransmittanceParams(Atmosphere a, vec2 uv, out float viewHeight, out float viewZenithCos)
{
    const float H = sqrt(a.topRadius * a.topRadius - a.bottomRadius * a.bottomRadius);
    const float rho = H * uv.y;
    viewHeight = sqrt(rho * rho + a.bottomRadius * a.bottomRadius);
    const float dMin = a.topRadius - viewHeight;
    const float dMax = rho + H;
    const float d = dMin + uv.x * (dMax - dMin);
    viewZenithCos = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * viewHeight * d);
    viewZenithCos = clamp(viewZenithCos, -1.0, 1.0);
}

vec2 transmittanceParamsToUv(Atmosphere a, float viewHeight, float viewZenithCos)
{
    const float H = sqrt(max(0.0, a.topRadius * a.topRadius - a.bottomRadius * a.bottomRadius));
    const float rho = sqrt(max(0.0, viewHeight * viewHeight - a.bottomRadius * a.bottomRadius));
    const float discriminant = viewHeight * viewHeight * (viewZenithCos * viewZenithCos - 1.0) + a.topRadius * a.topRadius;
    const float d = max(0.0, -viewHeight * viewZenithCos + sqrt(max(discriminant, 0.0)));
    const float dMin = a.topRadius - viewHeight;
    const float dMax = rho + H;
    return vec2((d - dMin) / (dMax - dMin), rho / H);
}

vec3 sampleTransmittance(Atmosphere a, float viewHeight, float viewZenithCos)
{
    return textureLod(sampler2D(transmittanceLut, samplers[0]), transmittanceParamsToUv(a, viewHeight, viewZenithCos), 0.0).rgb;
}

// The share of a light's disc (angular radius `radius`) above the horizon seen from `point`:
// lights set over their disc's width, from one radius above the horizon to one below (about
// +3 to -3 degrees at sun scale 1, as the user wanted). Cutting at the centre switched the
// light off between 0 and -0.01.
float lightAboveHorizon(Atmosphere a, vec3 point, vec3 light, float radius)
{
    const float r = length(point);
    const float sinHorizon = -sqrt(max(1.0 - (a.bottomRadius / r) * (a.bottomRadius / r), 0.0));
    const float elevation = asin(clamp(dot(light, point / r), -1.0, 1.0)) - asin(sinHorizon);
    // The part of a disc above a line x radii from its centre: a circular segment.
    const float x = clamp(elevation / max(radius, 1e-4), -1.0, 1.0);
    return 0.5 + (x * sqrt(1.0 - x * x) + asin(x)) / ATMOSPHERE_PI;
}

// A light's transmittance from the top of the air to `point`, times the share of its disc
// above that point's horizon; once the centre has set, the visible part takes the horizon's.
vec3 lightThroughAir(Atmosphere a, vec3 point, vec3 light, float radius)
{
    const float visible = lightAboveHorizon(a, point, light, radius);
    if (visible <= 0.0)
    {
        return vec3(0.0);
    }
    const float r = length(point);
    const float sinHorizon = -sqrt(max(1.0 - (a.bottomRadius / r) * (a.bottomRadius / r), 0.0));
    return sampleTransmittance(a, r, max(dot(light, point / r), sinHorizon + 1e-4)) * visible;
}

vec3 sampleMultiscatter(Atmosphere a, vec3 position, float sunZenithCos)
{
    vec2 uv = clamp(vec2(sunZenithCos * 0.5 + 0.5, (length(position) - a.bottomRadius) / (a.topRadius - a.bottomRadius)), 0.0, 1.0);
    uv = vec2(fromUnitToSubUvs(uv.x, MULTISCATTER_LUT_SIZE), fromUnitToSubUvs(uv.y, MULTISCATTER_LUT_SIZE));
    return textureLod(sampler2D(multiscatterLut, samplers[0]), uv, 0.0).rgb;
}

// Sky view table: x the angle to the sun around the zenith, y the zenith angle, non-linear
// so the horizon gets the most texels. The lower half is below the horizon.
void uvToSkyViewParams(Atmosphere a, vec2 uv, float viewHeight, out float viewZenithCos, out float lightViewCos)
{
    uv = vec2(fromSubUvsToUnit(uv.x, SKY_VIEW_LUT_SIZE.x), fromSubUvsToUnit(uv.y, SKY_VIEW_LUT_SIZE.y));
    const float vHorizon = sqrt(max(viewHeight * viewHeight - a.bottomRadius * a.bottomRadius, 0.0));
    const float beta = acos(clamp(vHorizon / viewHeight, -1.0, 1.0));
    const float zenithHorizonAngle = ATMOSPHERE_PI - beta;
    if (uv.y < 0.5)
    {
        float coord = 1.0 - 2.0 * uv.y;
        coord = 1.0 - coord * coord;
        viewZenithCos = cos(zenithHorizonAngle * coord);
    }
    else
    {
        float coord = uv.y * 2.0 - 1.0;
        viewZenithCos = cos(zenithHorizonAngle + beta * coord * coord);
    }
    lightViewCos = -(uv.x * uv.x * 2.0 - 1.0);
}

vec2 skyViewParamsToUv(Atmosphere a, bool intersectGround, float viewZenithCos, float lightViewCos, float viewHeight)
{
    const float vHorizon = sqrt(max(viewHeight * viewHeight - a.bottomRadius * a.bottomRadius, 0.0));
    const float beta = acos(clamp(vHorizon / viewHeight, -1.0, 1.0));
    const float zenithHorizonAngle = ATMOSPHERE_PI - beta;
    vec2 uv;
    if (!intersectGround)
    {
        const float coord = 1.0 - acos(clamp(viewZenithCos, -1.0, 1.0)) / zenithHorizonAngle;
        uv.y = (1.0 - sqrt(max(coord, 0.0))) * 0.5;
    }
    else
    {
        const float coord = (acos(clamp(viewZenithCos, -1.0, 1.0)) - zenithHorizonAngle) / beta;
        uv.y = sqrt(max(coord, 0.0)) * 0.5 + 0.5;
    }
    uv.x = sqrt(-lightViewCos * 0.5 + 0.5);
    return vec2(fromUnitToSubUvs(uv.x, SKY_VIEW_LUT_SIZE.x), fromUnitToSubUvs(uv.y, SKY_VIEW_LUT_SIZE.y));
}

// The sky view texture holds three tables of SKY_VIEW_LUT_SIZE, one above the other: the sky
// lit by the sun with no Mie single scattering (light 0), by the moon (light 1), then the
// sun's Mie single scattering with no phase (2), which skyRadiance() shapes per pixel.
const uint SKY_LIGHT_SUN = 0u;
const uint SKY_LIGHT_MOON = 1u;
const uint SKY_LIGHT_SUN_MIE = 2u;
const uint SKY_VIEW_LUT_LIGHTS = 3u; // asratmosphere.h

// The sky seen from the camera in a world direction, lit by one light of brightness 1.
vec3 sampleSkyView(Atmosphere a, vec3 direction, vec3 lightDirection, uint light)
{
    const vec3 position = atmosphereCamera(a);
    const float viewHeight = length(position);
    const vec3 up = position / viewHeight;
    const float viewZenithCos = dot(direction, up);
    // The light's angle around the zenith; straight up or down any angle is right.
    const vec2 viewOnPlane = direction.xy;
    const vec2 lightOnPlane = lightDirection.xy;
    const float lengths = length(viewOnPlane) * length(lightOnPlane);
    // Clamped: in line with the light, Metal's fast math lands a hair past 1, and the
    // table's sqrt(0.5 - 0.5 * cos) became NaN along the light's azimuth.
    const float lightViewCos = lengths > 1e-6 ? clamp(dot(viewOnPlane, lightOnPlane) / lengths, -1.0, 1.0) : 1.0;
    const bool intersectGround = raySphereIntersectNearest(position, direction, a.bottomRadius) >= 0.0;
    vec2 uv = skyViewParamsToUv(a, intersectGround, viewZenithCos, lightViewCos, viewHeight);
    uv.y = (uv.y + float(light)) / float(SKY_VIEW_LUT_LIGHTS); // texel centres of its own table: no bleeding across
    return textureLod(sampler2D(skyViewLut, samplers[0]), uv, 0.0).rgb;
}

float rayleighPhase(float cosTheta)
{
    return 3.0 / (16.0 * ATMOSPHERE_PI) * (1.0 + cosTheta * cosTheta);
}

// Cornette-Shanks, as the sample uses.
float miePhase(float g, float cosTheta)
{
    const float k = 3.0 / (8.0 * ATMOSPHERE_PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cosTheta * cosTheta) / pow(1.0 + g * g - 2.0 * g * -cosTheta, 1.5);
}

// OpenGL's glow on the haze (skyF.glsl): size and focus as EEP gives them, the 0.001
// minimum angle (taken before the size, as OpenGL does), the 100000 cap of
// atmosphericsFuncs.glsl and the 0.25 anti-solar floor.
float eepGlow(vec2 glow, float cosTheta)
{
    return min(pow(max(1.0 - cosTheta, 0.001) * glow.x, glow.y), 100000.0) + 0.25;
}

// The sky seen from the camera in a world direction, sun and moon at their own brightness.
// The sun's Mie single scattering takes its phase here, per pixel, so its peak is as sharp
// as the screen and not the table, times this EEP's glow against the default EEP's.
vec3 skyRadiance(Atmosphere a, vec3 direction)
{
    const float sunCos = dot(direction, a.sunDirection);
    const float glow = eepGlow(a.sunGlow, sunCos) / eepGlow(vec2(5.0, -0.48), sunCos);
    const vec3 sun = sampleSkyView(a, direction, a.sunDirection, SKY_LIGHT_SUN)
                     + sampleSkyView(a, direction, a.sunDirection, SKY_LIGHT_SUN_MIE) * (miePhase(a.mieG, -sunCos) * glow);
    return sun * a.sunIlluminance + sampleSkyView(a, direction, a.moonDirection, SKY_LIGHT_MOON) * a.moonSkyLight;
}

struct Medium
{
    vec3 scattering;
    vec3 extinction;
    vec3 scatteringMie;
    vec3 scatteringRay;
};

Medium sampleMedium(Atmosphere a, vec3 position)
{
    const float height = length(position) - a.bottomRadius;
    const float densityRay = profileDensity(a, 0, height);
    const float densityMie = profileDensity(a, 1, height);
    const float densityOzo = profileDensity(a, 2, height);
    Medium m;
    m.scatteringMie = densityMie * a.mieScattering;
    m.scatteringRay = densityRay * a.rayleighScattering;
    m.scattering = m.scatteringMie + m.scatteringRay;
    m.extinction = densityMie * a.mieExtinction + m.scatteringRay + densityOzo * a.absorptionExtinction;
    return m;
}

// Moves a point outside the atmosphere onto its top along the ray; false if it misses.
bool moveToTopAtmosphere(inout vec3 position, vec3 direction, float topRadius)
{
    const float height = length(position);
    if (height > topRadius)
    {
        const float tTop = raySphereIntersectNearest(position, direction, topRadius);
        if (tTop < 0.0)
        {
            return false;
        }
        position = position + direction * tTop - position / height * PLANET_RADIUS_OFFSET;
    }
    return true;
}

struct Scattering
{
    vec3 luminance;     // for a sun of brightness 1
    vec3 opticalDepth;
    vec3 transmittance;
    vec3 multiScatterAs1;
    vec3 mieSingle; // the light's Mie single scattering with no phase: luminance holds it times the phase
};

// The sample's IntegrateScatteredLuminance: marches the ray to the ground, the top of the
// atmosphere or maxDistance, whichever comes first. useMultiscatter reads the multiple
// scattering table, which the passes made before it allow. ground adds the light the
// ground bounces where the ray ends on it.
Scattering integrateScattering(Atmosphere a, vec3 position, vec3 direction, vec3 sunDirection, bool ground, float sampleCount,
                               bool variableSamples, bool mieRayPhase, bool useMultiscatter, float maxDistance)
{
    Scattering result;
    result.luminance = vec3(0.0);
    result.opticalDepth = vec3(0.0);
    result.transmittance = vec3(1.0);
    result.multiScatterAs1 = vec3(0.0);
    result.mieSingle = vec3(0.0);

    const float tBottom = raySphereIntersectNearest(position, direction, a.bottomRadius);
    const float tTop = raySphereIntersectNearest(position, direction, a.topRadius);
    float tMax = 0.0;
    if (tBottom < 0.0)
    {
        if (tTop < 0.0)
        {
            return result;
        }
        tMax = tTop;
    }
    else if (tTop > 0.0)
    {
        tMax = min(tTop, tBottom);
    }
    tMax = min(tMax, maxDistance);

    float samples = sampleCount;
    float samplesFloor = sampleCount;
    float tMaxFloor = tMax;
    if (variableSamples)
    {
        samples = mix(4.0, 14.0, clamp(tMax * 0.01, 0.0, 1.0)); // the sample's RayMarchMinMaxSPP
        samplesFloor = floor(samples);
        tMaxFloor = tMax * samplesFloor / samples;
    }

    const float cosTheta = dot(sunDirection, direction);
    const float miePhaseValue = miePhase(a.mieG, -cosTheta); // direction is an 'in' direction
    const float rayleighPhaseValue = rayleighPhase(cosTheta);
    const float uniformPhase = 1.0 / (4.0 * ATMOSPHERE_PI);

    vec3 luminance = vec3(0.0);
    vec3 throughput = vec3(1.0);
    vec3 opticalDepth = vec3(0.0);
    float t = 0.0;
    float dt = tMax / samples;
    const float segmentT = 0.3;
    for (float s = 0.0; s < samples; s += 1.0)
    {
        if (variableSamples)
        {
            float t0 = s / samplesFloor;
            float t1 = (s + 1.0) / samplesFloor;
            t0 = tMaxFloor * t0 * t0;
            t1 = t1 * t1 > 1.0 ? tMax : tMaxFloor * t1 * t1;
            t = t0 + (t1 - t0) * segmentT;
            dt = t1 - t0;
        }
        else
        {
            const float newT = tMax * (s + segmentT) / samples;
            dt = newT - t;
            t = newT;
        }
        const vec3 p = position + t * direction;
        const Medium medium = sampleMedium(a, p);
        const vec3 sampleOpticalDepth = medium.extinction * dt;
        const vec3 stepTransmittance = exp(-sampleOpticalDepth);
        opticalDepth += sampleOpticalDepth;

        const float pHeight = length(p);
        const vec3 up = p / pHeight;
        const float sunZenithCos = dot(sunDirection, up);
        const vec3 transmittanceToSun = sampleTransmittance(a, pHeight, sunZenithCos);
        const vec3 phaseTimesScattering = mieRayPhase ? medium.scatteringMie * miePhaseValue + medium.scatteringRay * rayleighPhaseValue
                                                      : medium.scattering * uniformPhase;
        const float tEarth = raySphereIntersectNearest(p - PLANET_RADIUS_OFFSET * up, sunDirection, a.bottomRadius);
        const float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;
        const vec3 multiscattered = useMultiscatter ? sampleMultiscatter(a, p, sunZenithCos) * a.multipleScattering : vec3(0.0);

        const vec3 S = earthShadow * transmittanceToSun * phaseTimesScattering + multiscattered * medium.scattering;
        const vec3 extinction = max(medium.extinction, vec3(1e-9));
        // The power series form: integrated over the step (Frostbite 2015, slide 28).
        result.multiScatterAs1 += throughput * (medium.scattering - medium.scattering * stepTransmittance) / extinction;
        luminance += throughput * (S - S * stepTransmittance) / extinction;
        const vec3 mieS = earthShadow * transmittanceToSun * medium.scatteringMie;
        result.mieSingle += throughput * (mieS - mieS * stepTransmittance) / extinction;
        throughput *= stepTransmittance;
    }

    if (ground && tMax == tBottom && tBottom > 0.0)
    {
        const vec3 p = position + tBottom * direction;
        const float pHeight = length(p);
        const vec3 up = p / pHeight;
        const float sunZenithCos = dot(sunDirection, up);
        const vec3 transmittanceToSun = sampleTransmittance(a, pHeight, sunZenithCos);
        luminance += transmittanceToSun * throughput * clamp(sunZenithCos, 0.0, 1.0) * a.groundAlbedo / ATMOSPHERE_PI;
    }

    result.luminance = luminance;
    result.opticalDepth = opticalDepth;
    result.transmittance = throughput;
    return result;
}
