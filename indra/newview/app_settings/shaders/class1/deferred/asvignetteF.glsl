/**
 * @file asvignetteF.glsl
 * @author chanayane@firestorm
 * @brief Multiplicative screen-space vignette.
 */

out vec4 frag_color;

uniform vec2 screen_res;
uniform float vignette_strength;
uniform float vignette_radius;
uniform float vignette_smoothness;
uniform float vignette_shape;

in vec2 vary_fragcoord;

void main()
{
    // A radius of one is a circle with the shorter viewport dimension as its
    // diameter. Shape stretches the selected major axis by up to two times.
    vec2 point = (vary_fragcoord - vec2(0.5)) * screen_res * (2.0 / min(screen_res.x, screen_res.y));
    if (vignette_shape < 0.0)
    {
        point.x /= 1.0 - vignette_shape;
    }
    else
    {
        point.y /= 1.0 + vignette_shape;
    }

    float darkness;
    if (vignette_radius <= 0.0001)
    {
        darkness = 1.0;
    }
    else if (vignette_smoothness <= 0.0001)
    {
        darkness = step(vignette_radius, length(point));
    }
    else
    {
        float inner_radius = vignette_radius * (1.0 - vignette_smoothness);
        darkness = smoothstep(inner_radius, vignette_radius, length(point));
    }

    float multiplier = 1.0 - vignette_strength * darkness;
    frag_color = vec4(vec3(multiplier), 1.0);
}
