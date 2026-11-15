/**
 * asrSceneResolveC.glsl
 * Turns the visibility buffer into G-buffer pixels: each visible pixel is shaded once,
 * whatever was drawn over or under it. Barycentrics and their screen derivatives come
 * from the triangle's three vertices, which gives texture sampling the same gradients a
 * fragment shader would have had.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];
layout(set = 1, binding = 0, r32ui) uniform readonly uimage2D visibility;
layout(set = 1, binding = 1, rgba8) uniform writeonly image2D albedo;
layout(set = 1, binding = 2, rgba16f) uniform writeonly image2D normals;
// The opaque image Mac OIT composites over. Until lighting in P4: albedo with a soft light
// from the normal so shapes read, and the sky where nothing is drawn. Alpha is glow.
layout(set = 1, binding = 8, rgba16f) uniform writeonly image2D lit;
#include "asrAtmosphere.glsl"

vec4 sampleBits(ivec2 pixel, uint bits, vec2 uv, vec2 uvDx, vec2 uvDy); // below

// A sun or moon image: a square of half side `radius` facing the camera, its top towards
// `up`. rgb: the image in linear light, a: its cover; 0 outside the square. Mips follow the
// pixel's size on the sky, and the texture reports what it needs like any other.
vec4 skyImage(ivec2 pixel, uint bits, vec3 bodyDirection, vec3 up, float radius, vec3 direction, float pixelAngle)
{
    const vec3 bodyUp = normalize(up - bodyDirection * dot(up, bodyDirection));
    const vec3 bodyRight = cross(bodyDirection, bodyUp);
    const float along = dot(direction, bodyDirection);
    const vec2 plane = vec2(dot(direction, bodyRight), dot(direction, bodyUp)) / along;
    const vec2 uv = vec2(plane.x, -plane.y) / (2.0 * radius) + 0.5;
    if (along <= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        return vec4(0.0);
    }
    const float uvStep = pixelAngle / (2.0 * radius);
    const vec4 texel = sampleBits(pixel, bits, uv, vec2(uvStep, 0.0), vec2(0.0, uvStep));
    return vec4(srgbToLinear(texel.rgb), texel.a);
}

// The sun with no image: darker at its edge (limb darkening, linear with u = 0.6), carrying
// its whole illuminance over its solid angle.
vec3 sunDisc(Atmosphere a, float cosAngle)
{
    const float sinRadius2 = 1.0 - a.sunCosRadius * a.sunCosRadius;
    const float r2 = clamp((1.0 - cosAngle * cosAngle) / max(sinRadius2, 1e-12), 0.0, 1.0);
    const float u = 0.6;
    const float darkening = (1.0 - u * (1.0 - sqrt(1.0 - r2))) / (1.0 - u / 3.0);
    return a.sunIlluminance * (darkening / (2.0 * ATMOSPHERE_PI * (1.0 - a.sunCosRadius)));
}

// The default moon as a sphere: each pixel of its disc finds its point on the sphere, lit
// with Lommel-Seeliger, the law of dusty bodies like the moon, which keeps a full moon as
// bright at its edge as in its middle (2 mu0 / (mu0 + mu), 1 there). Surface colour and
// normals are equirectangular maps of the whole moon, the side facing the Earth in the
// middle, its north towards moonUp. Lit from moonLightDirection, which its phase gives, not
// the sun. rgb: against the disc's brightness, a: cover, the edge smoothed over a pixel.
// The moon map's mean colour, linear: its texture's last mip. White without a texture.
vec3 moonMeanColor(Atmosphere a)
{
    if (a.moonTexture == NO_SKY_TEXTURE)
    {
        return vec3(1.0);
    }
    const uint slot = a.moonTexture & 0xFFFFFu;
    return srgbToLinear(textureLod(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT((a.moonTexture >> 28) & 3u)]), vec2(0.5), 16.0).rgb);
}

// The face's mean colour as it is shown, against the disc's brightness: the sphere's albedo
// is scaled to a mean of 1 (see moonSphere()), a flat image shows as it is.
vec3 moonFaceColor(Atmosphere a)
{
    const vec3 mean = moonMeanColor(a);
    return a.moonSphere ? mean / max(dot(mean, vec3(0.2126, 0.7152, 0.0722)), 0.01) : mean;
}

// Where on the moon's maps a point of the unit sphere is (see moonSphere()).
vec2 moonUV(vec3 point, vec3 right, vec3 up, vec3 toViewer)
{
    return vec2(atan(dot(point, right), dot(point, toViewer)) / (2.0 * ATMOSPHERE_PI) + 0.5,
                0.5 - asin(clamp(dot(point, up), -1.0, 1.0)) / ATMOSPHERE_PI);
}

// The relief's shadow at a point of the moon: 1 lit, 0 in shadow. From the point, a ray goes
// towards the light, climbing at the light's height above the local horizon while the sphere
// curves away beneath it (d^2 / 2 at d radii along); it is compared with the height map in
// the normal map's alpha (heights over moonHeightScale radii). Steps grow with distance and
// read coarser mips as they do. The edge is softened over the sun's apparent radius: a
// mountain covering part of the sun leaves part of its light, the real penumbra.
// arc: one pixel's size on the sphere, radians.
float moonShadow(Atmosphere a, vec3 normal, vec3 right, vec3 up, vec3 toViewer, vec2 uv, float arc)
{
    // The light's radius as seen from the moon: the sun's, widened by the terminator's
    // softness, which wraps the shading past the terminator by as much (moonSphere()). The
    // shadows soften with it, instead of cutting the wrapped light off at the terminator.
    const float SUN_RADIUS = 0.00465 + a.moonTerminatorSoftness; // radians
    const float MAX_DISTANCE = 0.2;   // radii: the ray is then past the highest relief
    const vec3 light = a.moonLightDirection;
    const float sinElevation = dot(light, normal);
    const vec3 along = light - normal * sinElevation;
    const float cosElevation = length(along);
    if (cosElevation < 1e-4)
    {
        return 1.0; // the light overhead: nothing casts
    }
    const vec3 towards = along / cosElevation;
    const float slope = sinElevation / cosElevation;

    const uint slot = a.moonNormalTexture & 0xFFFFFu;
    const uint samplerIndex = (a.moonNormalTexture >> 28) & 3u;
    const float texelArc = 2.0 * ATMOSPHERE_PI / float(textureSize(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(samplerIndex)]), 0).x);
    const float start = max(texelArc, arc);
    const float startLod = log2(max(arc / texelArc, 1.0));
    const float scale = a.moonHeightScale;
    const float h0 = textureLod(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(samplerIndex)]), uv, startLod).a * scale;

    float shadow = 1.0;
    float d = start;
    for (int i = 0; i < 32 && d < MAX_DISTANCE; ++i)
    {
        const float rise = d * slope + 0.5 * d * d;
        if (rise > scale)
        {
            break; // above every mountain
        }
        const vec3 point = normal * cos(d) + towards * sin(d);
        // The step's own length picks the mip, so far samples see what lies across the step.
        const float lod = log2(max(d * 0.15 / texelArc, 1.0));
        const float terrain = textureLod(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(samplerIndex)]),
                                         moonUV(point, right, up, toViewer), max(lod, startLod)).a * scale;
        shadow = min(shadow, clamp(0.5 + (h0 + rise - terrain) / (2.0 * d * SUN_RADIUS), 0.0, 1.0));
        if (shadow <= 0.0)
        {
            break;
        }
        d *= 1.15;
    }
    return shadow;
}

vec4 moonSphere(ivec2 pixel, Atmosphere a, vec3 direction, float pixelAngle)
{
    const vec3 toViewer = -a.moonDirection;
    const vec3 up = normalize(a.moonUp - a.moonDirection * dot(a.moonUp, a.moonDirection));
    const vec3 right = cross(up, toViewer);
    const vec2 disc = vec2(dot(direction, right), dot(direction, up)) / (dot(direction, a.moonDirection) * a.moonAngularRadius);
    const float r = length(disc);
    const float cover = clamp((1.0 - r) * a.moonAngularRadius / pixelAngle + 0.5, 0.0, 1.0);
    if (cover <= 0.0)
    {
        return vec4(0.0);
    }
    const vec2 onDisc = r > 1.0 ? disc / r : disc;
    const vec3 normal = onDisc.x * right + onDisc.y * up + sqrt(max(1.0 - dot(onDisc, onDisc), 0.0)) * toViewer;

    // Longitude grows to the viewer's right (east, as the moon is mapped), latitude north.
    const float longitude = atan(dot(normal, right), dot(normal, toViewer));
    const float latitude = asin(clamp(dot(normal, up), -1.0, 1.0));
    const vec2 uv = vec2(longitude / (2.0 * ATMOSPHERE_PI) + 0.5, 0.5 - latitude / ATMOSPHERE_PI);
    // A pixel spans pixelAngle / radius radians of the sphere at the disc's centre.
    const float arc = pixelAngle / a.moonAngularRadius;
    const vec2 uvDx = vec2(arc / (2.0 * ATMOSPHERE_PI), 0.0);
    const vec2 uvDy = vec2(0.0, arc / ATMOSPHERE_PI);

    vec3 albedo = vec3(1.0);
    if (a.moonTexture != NO_SKY_TEXTURE)
    {
        // Scaled to a mean of 1: the real moon reflects about 12%, and moonBrightness is what
        // its face shows at on average, as a grey OpenGL image of the moon does.
        const vec3 mean = moonMeanColor(a);
        albedo = srgbToLinear(sampleBits(pixel, a.moonTexture, uv, uvDx, uvDy).rgb) / max(dot(mean, vec3(0.2126, 0.7152, 0.0722)), 0.01);
    }
    vec3 surface = normal;
    if (a.moonNormalTexture != NO_SKY_TEXTURE && a.moonRelief > 0.0)
    {
        const vec3 east = normalize(cross(up, normal) + 1e-6 * right);
        const vec3 north = cross(normal, east);
        vec3 tangent = sampleBits(pixel, a.moonNormalTexture, uv, uvDx, uvDy).rgb * 2.0 - 1.0;
        tangent.xy *= a.moonRelief;
        surface = normalize(tangent.x * east + tangent.y * north + max(tangent.z, 0.0) * normal);
    }
    // The terminator softened as ASMoonPhaseSoftness asks: the lit side wraps past it.
    const float soft = a.moonTerminatorSoftness;
    const float mu0 = clamp((dot(surface, a.moonLightDirection) + soft) / (1.0 + soft), 0.0, 1.0);
    const float mu = max(dot(surface, toViewer), 0.05);
    float lit = 2.0 * mu0 / (mu0 + mu);
    if (lit > 0.0 && a.moonHeightScale > 0.0 && a.moonNormalTexture != NO_SKY_TEXTURE && a.moonRelief > 0.0)
    {
        lit *= mix(1.0, moonShadow(a, normal, right, up, toViewer, uv, arc), min(a.moonRelief, 1.0));
    }
    return vec4(albedo * (lit + a.moonEarthshine * a.moonEarthshineColor), cover);
}

// The halo around the moon (ASMoonHalo*), over the air's own glow: from the disc's edge out
// to moonHaloRadius radii, against the disc's brightness, fading as its softness asks.
// Returns its share of the disc's brightness.
vec3 moonHalo(Atmosphere a, vec3 direction, float moonCos)
{
    // x: 0 at the limb, 1 at the halo's radius. A glow is brightest at the limb and falls
    // steeply, then trails away: exponential, reaching 5% at the radius for softness 1 (more
    // for softer), faded to nothing over the last third so there is no edge. It shines with
    // the light the moon gives at its phase: a crescent's halo is faint.
    // Inside the limb it holds its limb value: the disc's antialiased edge blends over it, and
    // a zero there would leave a dark ring between disc and halo.
    const float x = max((acos(clamp(moonCos, -1.0, 1.0)) / a.moonAngularRadius - 1.0) / (a.moonHaloRadius - 1.0), 0.0);
    if (a.moonHaloStrength <= 0.0 || x >= 1.0)
    {
        return vec3(0.0);
    }
    const float fade = 1.0 - smoothstep(0.67, 1.0, x);
    // As OpenGL: it follows the phase, full where the nearest limb is sunlit, 15% beside
    // the dark one. The limb is read slightly inside the edge, at 0.85 radius, where the
    // sphere still faces the viewer, softened by the terminator and the halo's softness.
    const vec3 aside = direction - a.moonDirection * moonCos;
    const vec3 side = dot(aside, aside) > 1e-12 ? normalize(aside) : vec3(0.0);
    const float limbLight = dot(0.85 * side - 0.527 * a.moonDirection, a.moonLightDirection);
    const float width = max(a.moonTerminatorSoftness, 0.002) + 0.15 * a.moonHaloSoftness;
    const float limb = mix(0.15, 1.0, smoothstep(0.0, 1.0, (limbLight + width) / (2.0 * width)));
    // Against the face's mean colour as it shows: strength 1 glows at 5% of it at the limb,
    // a real glow's share; ASMoonHaloStrength's 0 to 2 then spans what looks right.
    return moonFaceColor(a) * (0.05 * a.moonHaloStrength * a.moonPhaseLight * exp(-3.0 * x / a.moonHaloSoftness) * fade * limb);
}

// The glow behind the stars (ASMilkyWay): the Milky Way and deep sky maps read in the stars'
// frame, against the exposure as the stars are. The maps hold value^(1/2.2). Mips follow the
// pixel's size on the sky, wider in u towards the poles.
vec3 milkyWay(ivec2 pixel, Atmosphere a, vec3 direction, float pixelAngle)
{
    const vec3 star = transpose(a.starRotation) * direction;
    const float cosDeclination = max(length(star.xy), 0.01);
    const vec2 uv = vec2(atan(star.y, star.x) / (2.0 * ATMOSPHERE_PI) + 0.5, asin(clamp(star.z, -1.0, 1.0)) / ATMOSPHERE_PI + 0.5);
    const vec2 uvDx = vec2(pixelAngle / (2.0 * ATMOSPHERE_PI * cosDeclination), 0.0);
    const vec2 uvDy = vec2(0.0, pixelAngle / ATMOSPHERE_PI);
    vec3 color = vec3(0.0);
    if (a.milkyWayLevel > 0.0)
    {
        color += pow(sampleBits(pixel, a.milkyWayTexture, uv, uvDx, uvDy).rgb, vec3(2.2)) * a.milkyWayLevel;
    }
    if (a.deepSkyLevel > 0.0)
    {
        color += pow(sampleBits(pixel, a.deepSkyTexture, uv, uvDx, uvDy).rgb, vec3(2.2)) * a.deepSkyLevel;
    }
    const float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = max(vec3(luminance) + (color - vec3(luminance)) * a.milkyWaySaturation, vec3(0.0));
    return color / ExposureRef(ViewRef(push.view).exposure).exposure;
}

// The background: the sky lit by sun and moon, and behind the air, dimmed by it and hidden
// by the planet, the moon, then the sun over it. pixelAngle: a pixel's size on the sky,
// radians. The stars are the viewer's catalogue (ASStars), drawn by their own pass.
vec3 background(ivec2 pixel, Atmosphere a, vec3 direction, float pixelAngle)
{
    vec3 luminance = skyRadiance(a, direction);
    const vec3 position = atmosphereCamera(a);
    if (raySphereIntersectNearest(position, direction, a.bottomRadius) >= 0.0)
    {
        return min(luminance, vec3(60000.0));
    }
    vec3 space = vec3(0.0);
    if (a.milkyWayLevel > 0.0 || a.deepSkyLevel > 0.0)
    {
        space = milkyWay(pixel, a, direction, pixelAngle);
    }

    const float moonCos = dot(direction, a.moonDirection);
    if (moonCos > a.moonCosRadius)
    {
        // Against the exposure, so the disc shows its face at any time (SkyInput::moonBrightness).
        const vec3 moonLuminance = a.moonTint * (a.moonBrightness / ExposureRef(ViewRef(push.view).exposure).exposure);
        space += moonLuminance * moonHalo(a, direction, moonCos);
        if (a.moonSphere)
        {
            const vec4 moon = moonSphere(pixel, a, direction, pixelAngle);
            space = mix(space, moon.rgb * moonLuminance, moon.a);
        }
        else if (a.moonTexture != NO_SKY_TEXTURE)
        {
            const vec4 moon = skyImage(pixel, a.moonTexture, a.moonDirection, a.moonUp, a.moonAngularRadius, direction, pixelAngle);
            space = mix(space, moon.rgb * moonLuminance, moon.a);
        }
        else if (moonCos > cos(a.moonAngularRadius))
        {
            space = vec3(moonLuminance);
        }
    }

    const float sunCos = dot(direction, a.sunDirection);
    if (a.sunTexture != NO_SKY_TEXTURE)
    {
        if (sunCos > cos(a.sunTextureRadius * 1.41422))
        {
            const vec4 sun = skyImage(pixel, a.sunTexture, a.sunDirection, a.sunUp, a.sunTextureRadius, direction, pixelAngle);
            space = mix(space, sun.rgb * a.sunIlluminance / (ATMOSPHERE_PI * a.sunTextureRadius * a.sunTextureRadius), sun.a);
        }
    }
    else if (sunCos > a.sunCosRadius)
    {
        space = sunDisc(a, sunCos);
    }

    luminance += sampleTransmittance(a, length(position), direction.z) * space;
    return min(luminance, vec3(60000.0)); // inside half floats
}

vec4 unlit(vec3 albedo, vec3 normal)
{
    return vec4(placeholderShade(albedo, normal), 0.0);
}

struct Interpolator
{
    vec3 weights;
    vec3 ddx;
    vec3 ddy;
};

// Perspective-correct weights of the pixel inside the triangle, and how they change one
// pixel to the right and one pixel down.
Interpolator interpolator(vec4 p0, vec4 p1, vec4 p2, vec2 ndc, vec2 pixelStep)
{
    const vec3 invW = 1.0 / vec3(p0.w, p1.w, p2.w);
    const vec2 n0 = p0.xy * invW.x;
    const vec2 n1 = p1.xy * invW.y;
    const vec2 n2 = p2.xy * invW.z;

    const float invDet = 1.0 / determinant(mat2(n2 - n1, n0 - n1));
    const vec3 dx = vec3(n1.y - n2.y, n2.y - n0.y, n0.y - n1.y) * invDet * invW;
    const vec3 dy = vec3(n2.x - n1.x, n0.x - n2.x, n1.x - n0.x) * invDet * invW;
    const float dxSum = dx.x + dx.y + dx.z;
    const float dySum = dy.x + dy.y + dy.z;

    const vec2 delta = ndc - n0;
    const float pixelInvW = invW.x + delta.x * dxSum + delta.y * dySum;
    const vec3 raw = vec3(invW.x, 0.0, 0.0) + delta.x * dx + delta.y * dy;

    Interpolator result;
    result.weights = raw / pixelInvW;
    result.ddx = (raw + dx * pixelStep.x) / (pixelInvW + dxSum * pixelStep.x) - result.weights;
    result.ddy = (raw + dy * pixelStep.y) / (pixelInvW + dySum * pixelStep.y) - result.weights;
    return result;
}

// Temporary, while the Mac's GPU page faults here are found: every index and address read
// through is checked first, and a pixel that fails is drawn magenta and read no further.
// The bits are GUARD_* of asrscene.h. They sit at the end of the counts, which are read back.
const uint GUARD_ENTRY = 1u;
const uint GUARD_INSTANCE = 2u;
const uint GUARD_CLUSTER = 4u;
const uint GUARD_MATERIAL = 8u;
const uint GUARD_ADDRESSES = 16u;
const uint64_t CHUNK_BYTES = 64ul * 1024ul * 1024ul; // SCENE_CHUNK_BYTES
const uint64_t GUARD_OFFSET = 816ul;                  // offsetof(GpuSceneCounts, guardBits)

layout(buffer_reference, std430) buffer GuardRef
{
    uint bits;
    uint pixels;
    uint value;
    uint unused;
};

void reject(ivec2 pixel, uint bit, uint value)
{
    GuardRef guard = GuardRef(push.output_counts + GUARD_OFFSET);
    atomicOr(guard.bits, bit);
    atomicAdd(guard.pixels, 1u);
    atomicMax(guard.value, value);
    imageStore(albedo, pixel, vec4(1.0, 0.0, 1.0, 1.0));
    imageStore(normals, pixel, vec4(0.0, 0.0, 1.0, 1.0));
    imageStore(lit, pixel, vec4(1.0, 0.0, 1.0, 0.0));
}

// A texture by its bits (see uiTextureBits()), with the steps of its UVs one pixel across and down.
vec4 sampleBits(ivec2 pixel, uint bits, vec2 uv, vec2 uvDx, vec2 uvDy)
{
    const uint slot = bits & 0xFFFFFu;
    const uint sampler_index = (bits >> 28) & 3u;
    giveFeedback(pixel, slot, uvDx, uvDy);
    return textureGrad(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(sampler_index)]), uv, uvDx, uvDy);
}

// A terrain layer through its matrix, from a pair of region coordinates and their steps.
vec4 sampleLayer(ivec2 pixel, TerrainLayer layer, vec2 at, vec2 atDx, vec2 atDy)
{
    const vec2 uv = vec2(dot(layer.uvRow0, vec3(at, 1.0)), dot(layer.uvRow1, vec3(at, 1.0)));
    const vec2 uvDx = vec2(dot(layer.uvRow0.xy, atDx), dot(layer.uvRow1.xy, atDx));
    const vec2 uvDy = vec2(dot(layer.uvRow0.xy, atDy), dot(layer.uvRow1.xy, atDy));
    return sampleBits(pixel, layer.texture, uv, uvDx, uvDy) * layer.color;
}

// As pbrterrainUtilF.glsl's triplanar sampling: each plane's coordinates, flipped on the
// side facing away, weighted by the normal. A face looking down samples the corner texel,
// as there.
vec4 sampleLayerPlanes(ivec2 pixel, Terrain terrain, uint index, vec3 region, vec3 regionDx, vec3 regionDy, vec3 normal)
{
    const TerrainLayer layer = terrain.layers[index];
    if (terrain.planes != 3u)
    {
        return sampleLayer(pixel, layer, region.xy, regionDx.xy, regionDy.xy);
    }

    const float TRIPLANAR_MIX_THRESHOLD = 0.01;
    vec3 weight = pow(abs(normal), vec3(terrain.sharpness));
    weight /= weight.x + weight.y + weight.z;
    weight -= vec3(TRIPLANAR_MIX_THRESHOLD);
    weight = max(weight, vec3(0.0));
    weight /= weight.x + weight.y + weight.z;

    vec4 color = vec4(0.0);
    if (weight.x > 0.0)
    {
        const vec2 flip = normal.x > 0.0 ? vec2(1.0) : vec2(-1.0, 1.0);
        color += weight.x * sampleLayer(pixel, layer, region.yz * flip, regionDx.yz * flip, regionDy.yz * flip);
    }
    if (weight.y > 0.0)
    {
        const vec2 flip = normal.y > 0.0 ? vec2(-1.0, 1.0) : vec2(1.0);
        color += weight.y * sampleLayer(pixel, layer, region.xz * flip, regionDx.xz * flip, regionDy.xz * flip);
    }
    if (weight.z > 0.0)
    {
        if (normal.z > 0.0)
        {
            color += weight.z * sampleLayer(pixel, layer, region.xy, regionDx.xy, regionDy.xy);
        }
        else
        {
            const uint slot = layer.texture & 0xFFFFFu;
            const uint sampler_index = (layer.texture >> 28) & 3u;
            color += weight.z * textureLod(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(sampler_index)]), vec2(0.0), 0.0) * layer.color;
        }
    }
    return color;
}

// The ground's colour, as terrainF.glsl and pbrterrainF.glsl blend it. A vertex's UV is its
// composition, from 0 to 3, and a noise value; the alpha ramp turns them into how much each
// layer shows. Only layers that show are sampled, which gives the same result.
vec4 terrainColor(ivec2 pixel, Terrain terrain, vec3 region, vec3 regionDx, vec3 regionDy, vec2 blend, vec2 blendDx, vec2 blendDy, vec3 normal)
{
    const float alpha1 = sampleBits(pixel, terrain.ramp, blend, blendDx, blendDy).a;
    const float alpha2 = sampleBits(pixel, terrain.ramp, blend - vec2(2.0, 0.0), blendDx, blendDy).a;
    const float alphaFinal = sampleBits(pixel, terrain.ramp, blend - vec2(1.0, 0.0), blendDx, blendDy).a;
    vec4 weights = vec4(alphaFinal * alpha1, alphaFinal * (1.0 - alpha1), (1.0 - alphaFinal) * alpha2, (1.0 - alphaFinal) * (1.0 - alpha2));

    if (terrain.kind == TERRAIN_MATERIALS)
    {
        // get_terrain_mix_weights(): what is under the threshold drops out, and the rest
        // is scaled back up to one.
        const float RAMP_MIX_THRESHOLD = 0.1;
        weights = max(weights - vec4(RAMP_MIX_THRESHOLD), vec4(0.0));
        weights /= weights.x + weights.y + weights.z + weights.w;
    }

    vec4 color = vec4(0.0);
    for (uint i = 0u; i < TERRAIN_LAYERS; ++i)
    {
        if (weights[i] > 0.0)
        {
            color += weights[i] * sampleLayerPlanes(pixel, terrain, i, region, regionDx, regionDy, normal);
        }
    }
    return color;
}

void main()
{
    const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ViewRef view = ViewRef(push.view);
    if (pixel.x >= int(view.pixelSize.x) || pixel.y >= int(view.pixelSize.y))
    {
        return;
    }

    const uint id = imageLoad(visibility, pixel).r;
    if (id == 0xFFFFFFFFu)
    {
        imageStore(albedo, pixel, vec4(0.0, 0.0, 0.0, 0.0));
        imageStore(normals, pixel, vec4(0.0));
        const vec2 ndc = (vec2(pixel) + 0.5) * (2.0 / view.pixelSize.xy) - 1.0;
        const float pixelAngle = 2.0 / (view.pixelSize.x * view.projection.x);
        imageStore(lit, pixel, vec4(background(pixel, loadAtmosphere(view.atmosphere), viewDirection(view, ndc), pixelAngle), 0.0));
        return;
    }

    const uint triangle = id & 63u;
    const uint entry = id >> 6;
    if (entry >= view.visibleEnd)
    {
        reject(pixel, GUARD_ENTRY, entry);
        return;
    }
    const ListEntry visible = ListRef(push.visible).items[entry];
    if (visible.instance >= view.instanceEnd)
    {
        reject(pixel, GUARD_INSTANCE, visible.instance);
        return;
    }
    if (visible.cluster >= view.clusterEnd)
    {
        reject(pixel, GUARD_CLUSTER, visible.cluster);
        return;
    }
    const Cluster cluster = ClustersRef(push.clusters).items[visible.cluster];
    const Instance instance = InstancesRef(push.instances).items[visible.instance];
    if (instance.material >= view.materialEnd)
    {
        reject(pixel, GUARD_MATERIAL, instance.material);
        return;
    }
    // One blob holds a cluster's indices, then positions, then attributes, in one chunk.
    // A skinned cluster's positions are its own, elsewhere.
    const bool skinned = (instance.flags & INSTANCE_SKINNED) != 0u;
    if (!skinned && (cluster.positions < cluster.indices || cluster.attributes < cluster.positions || cluster.attributes - cluster.indices >= CHUNK_BYTES))
    {
        reject(pixel, GUARD_ADDRESSES, uint(min((cluster.attributes - cluster.indices) >> 20, 0xFFFFFFFFul)));
        return;
    }
    const Material material = MaterialsRef(push.materials).items[instance.material];

    const uint i0 = clusterIndex(cluster.indices, triangle * 3u);
    const uint i1 = clusterIndex(cluster.indices, triangle * 3u + 1u);
    const uint i2 = clusterIndex(cluster.indices, triangle * 3u + 2u);
    const mat4 viewProjection = view.viewProjection;
    const mat3 world = mat3(transformPoint(instance.rows, packedPosition(cluster.positions, i0)),
                            transformPoint(instance.rows, packedPosition(cluster.positions, i1)),
                            transformPoint(instance.rows, packedPosition(cluster.positions, i2)));
    const vec4 p0 = viewProjection * vec4(world[0], 1.0);
    const vec4 p1 = viewProjection * vec4(world[1], 1.0);
    const vec4 p2 = viewProjection * vec4(world[2], 1.0);

    const vec2 pixelStep = 2.0 * view.pixelSize.zw;
    const vec2 ndc = (vec2(pixel) + 0.5) * pixelStep - 1.0;
    const Interpolator bary = interpolator(p0, p1, p2, ndc, pixelStep);

    // An attribute is four words: normal, tangent, u, v.
    WordsRef attributes = WordsRef(cluster.attributes);
    const uvec3 base = uvec3(i0, i1, i2) * 4u;
    vec2 uv0 = vec2(uintBitsToFloat(attributes.words[base.x + 2u]), uintBitsToFloat(attributes.words[base.x + 3u]));
    vec2 uv1 = vec2(uintBitsToFloat(attributes.words[base.y + 2u]), uintBitsToFloat(attributes.words[base.y + 3u]));
    vec2 uv2 = vec2(uintBitsToFloat(attributes.words[base.z + 2u]), uintBitsToFloat(attributes.words[base.z + 3u]));

    const uvec3 normalWords = uvec3(attributes.words[base.x], attributes.words[base.y], attributes.words[base.z]);
    vec3 n0 = octahedralDecode(float(int(normalWords.x << 16) >> 16) / 32767.0, float(int(normalWords.x) >> 16) / 32767.0);
    vec3 n1 = octahedralDecode(float(int(normalWords.y << 16) >> 16) / 32767.0, float(int(normalWords.y) >> 16) / 32767.0);
    vec3 n2 = octahedralDecode(float(int(normalWords.z << 16) >> 16) / 32767.0, float(int(normalWords.z) >> 16) / 32767.0);
    if (skinned)
    {
        // Only skinned positions are stored: normals turn here with the same palette, as the
        // viewer's rigged shaders turn them. The box matrix only scales, so it leaves them be.
        const Binding binding = BindingsRef(view.skinTables[SKIN_TABLE_BINDINGS]).items[instance.binding];
        n0 = skinNormal(view, binding, i0, n0);
        n1 = skinNormal(view, binding, i1, n1);
        n2 = skinNormal(view, binding, i2, n2);
    }

    const vec3 objectNormal = mat3(n0, n1, n2) * bary.weights;

    vec4 color;
    if (material.mapping == MATERIAL_TERRAIN)
    {
        // Ground: its position in region metres places the layers, and its UVs blend them.
        const mat3 region = mat3(packedPosition(cluster.positions, i0) * material.planarScale + material.planarOffset,
                                 packedPosition(cluster.positions, i1) * material.planarScale + material.planarOffset,
                                 packedPosition(cluster.positions, i2) * material.planarScale + material.planarOffset);
        const mat3x2 blends = mat3x2(uv0, uv1, uv2);
        const Terrain terrain = TerrainsRef(push.terrains).items[material.texture];
        color = terrainColor(pixel, terrain, region * bary.weights, region * bary.ddx, region * bary.ddy, blends * bary.weights, blends * bary.ddx,
                             blends * bary.ddy, objectNormal);
    }
    else
    {
        if (material.mapping == MATERIAL_PLANAR)
        {
            uv0 = planarUV(n0, packedPosition(cluster.positions, i0) * material.planarScale + material.planarOffset);
            uv1 = planarUV(n1, packedPosition(cluster.positions, i1) * material.planarScale + material.planarOffset);
            uv2 = planarUV(n2, packedPosition(cluster.positions, i2) * material.planarScale + material.planarOffset);
        }
        const mat3x2 uvs = mat3x2(uv0, uv1, uv2);

        const vec2 faceUV = uvs * bary.weights;
        const vec2 uv = vec2(dot(material.uvRow0, vec3(faceUV, 1.0)), dot(material.uvRow1, vec3(faceUV, 1.0)));
        const vec2 faceDx = uvs * bary.ddx;
        const vec2 faceDy = uvs * bary.ddy;
        const vec2 uvDx = vec2(dot(material.uvRow0.xy, faceDx), dot(material.uvRow1.xy, faceDx));
        const vec2 uvDy = vec2(dot(material.uvRow0.xy, faceDy), dot(material.uvRow1.xy, faceDy));
        color = material.color * sampleBits(pixel, material.texture, uv, uvDx, uvDy);
    }

    vec3 worldNormal = worldNormalOf(instance, objectNormal);

    // The back of a two-sided surface faces the other way. In this space the front
    // of a triangle turns clockwise, and a flipped instance swaps the two.
    if ((instance.flags & INSTANCE_DOUBLE_SIDED) != 0u)
    {
        const vec2 ndc0 = p0.xy / p0.w;
        const vec2 ndc1 = p1.xy / p1.w;
        const vec2 ndc2 = p2.xy / p2.w;
        const bool clockwise = determinant(mat2(ndc1 - ndc0, ndc2 - ndc0)) < 0.0;
        if (clockwise == ((instance.flags & INSTANCE_FLIPPED) != 0u))
        {
            worldNormal = -worldNormal;
        }
    }

    imageStore(albedo, pixel, vec4(color.rgb, 1.0));
    imageStore(normals, pixel, vec4(worldNormal, 1.0));
    vec4 shown = unlit(color.rgb, worldNormal);
    shown.rgb = applyAerial(loadAtmosphere(view.atmosphere), shown.rgb, (vec2(pixel) + 0.5) * view.pixelSize.zw,
                            distance(world * bary.weights, view.camera.xyz));
    if (view.debugView == DEBUG_VIEW_NORMALS)
    {
        shown = vec4(worldNormal * 0.5 + 0.5, 1.0);
    }
    else if (view.debugView == DEBUG_VIEW_ALBEDO)
    {
        shown = vec4(color.rgb, 1.0);
    }
    else if (view.debugView == DEBUG_VIEW_ALPHA_MODE)
    {
        shown = (instance.flags & INSTANCE_MASKED) != 0u ? vec4(0.1, 0.8, 0.1, 1.0) : vec4(0.5, 0.5, 0.5, 1.0);
    }
    else if (view.debugView == DEBUG_VIEW_CLUSTERS)
    {
        // One colour per drawn cluster: a patch with straight cluster edges is culling,
        // a ragged one is two surfaces fighting in depth.
        uint hash = visible.cluster * 747796405u + 2891336453u;
        hash = ((hash >> ((hash >> 28u) + 4u)) ^ hash) * 277803737u;
        hash ^= hash >> 22u;
        shown = vec4(vec3(hash & 255u, (hash >> 8) & 255u, (hash >> 16) & 255u) / 255.0, 1.0);
    }
    imageStore(lit, pixel, shown);
}
