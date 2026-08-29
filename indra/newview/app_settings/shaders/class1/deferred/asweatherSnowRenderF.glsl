/**
 * @file asweatherSnowRenderF.glsl
 * @author chanayane@firestorm
 * @brief Soft original procedural flake appearance tinted by active EEP light.
 */

uniform vec3 snow_light_color;
uniform int snow_quality;

in vec2 snow_uv;
in vec4 snow_color;
out vec4 frag_color;

void main()
{
    vec2 point = snow_uv * 2.0 - 1.0;
    float radius = length(point);
    float core = 1.0 - smoothstep(0.30, 0.92, radius);
    float arms = 1.0 - smoothstep(0.08, 0.23,
        min(abs(point.x), min(abs(dot(point, vec2(0.5, 0.866025))),
                              abs(dot(point, vec2(0.5, -0.866025))))));
    float arm_mask = arms * (1.0 - smoothstep(0.35, 1.0, radius));
    float shape = snow_quality <= 0 ?
        1.0 - smoothstep(0.18, 0.88, radius) : max(core, arm_mask);
    if (snow_quality >= 2)
    {
        // Keep the secondary arms thick enough to survive the few-pixel
        // footprint of ordinary distant flakes; the previous fine lines were
        // visually indistinguishable from Medium after rasterization.
        float fine_arms = 1.0 - smoothstep(0.075, 0.19,
            min(abs(dot(point, vec2(0.866025, 0.5))),
                min(abs(dot(point, vec2(0.0, 1.0))),
                    abs(dot(point, vec2(0.866025, -0.5))))));
        float crystal = fine_arms * (1.0 - smoothstep(0.32, 0.98, radius));
        shape = max(shape, crystal * 0.92);
    }
    float alpha = shape * snow_color.a;
    if (alpha < 0.01)
    {
        discard;
    }
    frag_color = vec4(snow_light_color * snow_color.rgb, alpha);
}
