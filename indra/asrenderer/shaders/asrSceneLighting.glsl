/**
 * asrSceneLighting.glsl
 * Surface shading shared by the resolve and every Mac OIT layer, so opaque and blended
 * surfaces light alike. Included after asrSceneCommon.glsl and asrAtmosphere.glsl.
 *
 * LL's legacy materials follow OpenGL's deferred path (softenLightF.glsl, materialF.glsl):
 * normalized Blinn-Phong for the sun and moon with OpenGL's Fresnel and shadowing terms, its
 * glossy and legacy environment terms, and the emissive mix. OpenGL's light values are
 * radiance-like (a white card facing the sun shows the sun's colour), ours are irradiance in
 * the sun's units, so its sunlight is ours over pi. Until reflection probes (P4 step 7) the
 * environment is the sky alone: its radiance in the reflected direction, going over to the
 * ambient's for low glossiness, as a probe's rough mips would.
 */

// OpenGL's RenderSpecularExponent: the Blinn-Phong exponent is glossiness squared times this.
const float LEGACY_SPECULAR_EXPONENT = 368.0;

// What a legacy material gives the shading, from its maps.
struct LegacySurface
{
    vec4 diffuse;  // sRGB colour; alpha unused here
    vec3 normal;   // world, facing the camera's side for two-sided faces
    vec3 specular; // linear
    float glossiness;
    float envIntensity;
    float emissive; // 0 lit, 1 its own colour unlit
};

// OpenGL's lightFunc table (LLPipeline::createLUTBuffers()), computed in place.
float legacyBlinnPhong(float nh, float glossiness)
{
    const float n = glossiness * glossiness * LEGACY_SPECULAR_EXPONENT;
    return pow(nh, n) * ((n + 2.0) * (n + 4.0)) / (8.0 * ATMOSPHERE_PI * (exp2(-n * 0.5) + n));
}

// A light's highlight, as softenLightF.glsl's: `light` is its irradiance on a surface facing it.
vec3 legacyHighlight(LegacySurface s, vec3 toCamera, vec3 lightDirection, vec3 light)
{
    const vec3 h = normalize(lightDirection + toCamera);
    const float nl = dot(s.normal, lightDirection);
    const float nh = dot(s.normal, h);
    if (nl <= 0.0 || nh <= 0.0)
    {
        return vec3(0.0);
    }
    const float nv = max(dot(s.normal, toCamera), 0.0);
    const float vh = max(dot(toCamera, h), 1e-4);
    const float lit = min(nl * 6.0, 1.0);
    const float fresnel = pow(1.0 - vh, 5.0) * 0.4 + 0.5;
    const float shadowing = max(0.0, min(2.0 * nh * nv / vh, 2.0 * nh * nl / vh));
    const float scale = fresnel * legacyBlinnPhong(nh, s.glossiness) * shadowing / (nh * nl);
    // OpenGL: lit * scol * sunlight * spec, its sunlight being our irradiance over pi.
    return lit * scale * light * (1.0 / ATMOSPHERE_PI) * s.specular;
}

// The sky's radiance towards `direction`, as the ambient was measured.
vec3 environmentSky(Atmosphere a, vec3 direction)
{
    return twilightSky(a, ExposureRef(ViewRef(push.view).exposure).twilightBand, direction);
}

// A legacy surface lit, in the sun's units before exposure.
vec3 shadeLegacy(Atmosphere a, LegacySurface s, vec3 toCamera)
{
    ExposureRef e = ExposureRef(ViewRef(push.view).exposure);
    const vec3 base = srgbToLinear(s.diffuse.rgb);
    vec3 color = base * surfaceIrradiance(s.normal) * (1.0 / ATMOSPHERE_PI);

    const vec3 reflected = reflect(-toCamera, s.normal);
    // The environment looking along the reflection: the sky above, the ground's bounce below
    // (the ambient's own lower half), and over to the ambient as glossiness drops.
    vec3 sharp = reflected.z > 0.0 ? environmentSky(a, reflected) : surfaceIrradiance(reflected) * (1.0 / ATMOSPHERE_PI);
    if (s.glossiness > 0.0)
    {
        color += legacyHighlight(s, toCamera, e.sunDirection, e.sunLight);
        color += legacyHighlight(s, toCamera, e.moonDirection, e.moonLight);
        // applyGlossEnv(): the glossy environment, its fake energy conservation measured on
        // the exposed colour, as OpenGL's colours are near display values.
        const vec3 glossy = mix(surfaceIrradiance(reflected) * (1.0 / ATMOSPHERE_PI), sharp, s.glossiness);
        float fresnel = clamp(1.0 - dot(toCamera, s.normal), 0.3, 1.0);
        fresnel *= fresnel * s.glossiness;
        const vec3 exposed = clamp(color * e.exposure, 0.0, 1.0);
        color += glossy * 0.5 * s.specular * fresnel * (vec3(1.0) - exposed) * 0.5;
    }

    // Emissive: its own colour, shown as OpenGL shows an unlit colour, the same on screen in
    // any light: so before exposure, over it.
    color = mix(color, base / max(e.exposure, 1e-20), s.emissive);

    if (s.envIntensity > 0.0)
    {
        // applyLegacyEnv().
        float fresnel = 1.0 - dot(toCamera, s.normal);
        fresnel = min(fresnel * fresnel + s.envIntensity, 1.0);
        color = mix(color, sharp * (s.envIntensity * fresnel) * 0.5, s.envIntensity);
    }
    return color;
}

// A material's tangent-space normal map turned into the world. `tangent` is the world
// tangent, `handedness` the bitangent's sign, both as the vertex carries them.
vec3 applyNormalMap(vec3 normal, vec3 tangent, float handedness, vec3 mapped)
{
    // A mesh without tangents (zero, or along the normal) keeps its normal: no NaN reaches bloom.
    const vec3 along = tangent - normal * dot(normal, tangent);
    if (dot(along, along) < 1e-8)
    {
        return normal;
    }
    const vec3 t = normalize(along);
    const vec3 b = handedness * cross(normal, t);
    const vec3 result = mapped.x * t + mapped.y * b + mapped.z * normal;
    return dot(result, result) > 1e-8 ? normalize(result) : normal;
}
