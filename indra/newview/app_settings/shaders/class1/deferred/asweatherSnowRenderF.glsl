/**
 * @file asweatherSnowRenderF.glsl
 * @author chanayane@firestorm
 * @brief Soft original procedural flake appearance tinted by active EEP light.
 */

uniform vec3 snow_light_color;

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
    float shape = max(core, arms * (1.0 - smoothstep(0.35, 1.0, radius)));
    float alpha = shape * snow_color.a;
    if (alpha < 0.01)
    {
        discard;
    }
    frag_color = vec4(snow_light_color * snow_color.rgb, alpha);
}
