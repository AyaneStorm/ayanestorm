/**
 * @file asBackgroundIsolateF.glsl
 * @author chanayane@firestorm
 * @brief Solid-color background isolate pass for the self-lighting floater's
 *        photography mode.
 */

out vec4 frag_color;

uniform sampler2D depthMap;
uniform vec4 isolate_color;

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
    frag_color = vec4(isolate_color.rgb, isolate_color.a * is_background);
}
