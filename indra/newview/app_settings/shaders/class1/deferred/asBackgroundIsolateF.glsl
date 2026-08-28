/**
 * @file asBackgroundIsolateF.glsl
 * @author chanayane@firestorm
 * @brief Solid-color background isolate pass for the self-lighting floater's
 *        photography mode.
 */

out vec4 frag_color;

uniform sampler2D depthMap;
uniform sampler2D exposureMap;
uniform vec4 isolate_color;
uniform float exposure;
uniform bool base_layer;

in vec2 vary_fragcoord;

void main()
{
    // Far-plane/unoccluded depth reads as ~1.0 here, same convention already
    // used by the AS lens flare pass's sourceVisibility() test -- anything
    // closer than that is opaque scene geometry (the avatar, its
    // attachments, our own invisible light-rig objects, or alpha-blended
    // content ExactOIT/AVBOIT resolved while isolate mode is active, which
    // both now write a near-plane depth specifically so this test sees them
    // correctly) and must be left untouched so it keeps showing through
    // unmodified.
    float depth = texture(depthMap, vary_fragcoord).r;
    float is_background = step(0.999, depth);

    vec3 color = isolate_color.rgb;
    if (base_layer)
    {
        // The swatch/late pass color is display-space sRGB, while mRT->screen
        // is linear HDR and will be exposed and gamma-corrected later. Convert
        // to linear and cancel the current exposure so transparent content
        // sees the same under-color as the final solid backdrop rather than a
        // much lighter/saturated transformed version.
        bvec3 low = lessThanEqual(color, vec3(0.04045));
        vec3 linear_low = color / 12.92;
        vec3 linear_high = pow((color + 0.055) / 1.055, vec3(2.4));
        color = mix(linear_high, linear_low, low);
        float final_exposure = max(exposure * texture(exposureMap, vec2(0.5)).r, 0.0001);
        color /= final_exposure;
    }

    frag_color = vec4(color, isolate_color.a * is_background);
}
