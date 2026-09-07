/**
 * @file aschromaticaberrationF.glsl
 * @author chanayane@firestorm
 * @brief Radial RGB separation, preserving green and source alpha.
 */
uniform sampler2D diffuseRect;
uniform vec2 screen_res;
uniform float aberration_strength;
uniform float aberration_falloff;
uniform vec2 aberration_center;
in vec2 vary_fragcoord;
out vec4 frag_color;

void main()
{
    // Use pixel-space radial distance for consistent geometry across aspect ratios.
    vec2 radial = (vary_fragcoord - aberration_center) * screen_res;
    float distance_from_center = length(radial);
    float radius = clamp(distance_from_center / (0.5 * length(screen_res)), 0.0, 1.0);
    vec2 direction = radial / max(distance_from_center, 0.0001);
    // Strength is each channel's corner displacement at a 1080-pixel short edge.
    // Scaling by resolution keeps the appearance consistent in larger snapshots.
    vec2 offset = direction * aberration_strength * (min(screen_res.x, screen_res.y) / 1080.0)
                  * pow(radius, aberration_falloff) / screen_res;
    vec2 lower = 0.5 / screen_res;
    vec2 upper = vec2(1.0) - lower;
    vec4 color = texture(diffuseRect, clamp(vary_fragcoord, lower, upper));
    color.r = texture(diffuseRect, clamp(vary_fragcoord + offset, lower, upper)).r;
    color.b = texture(diffuseRect, clamp(vary_fragcoord - offset, lower, upper)).b;
    frag_color = color;
}
