/**
 * @file asweatherSnowRenderF.glsl
 * @author chanayane@firestorm
 * @brief Soft original procedural flake appearance tinted by active EEP light.
 */

uniform vec3 snow_light_color;
uniform int snow_shape;

in vec2 snow_uv;
in vec4 snow_color;
out vec4 frag_color;

void main()
{
    vec2 point = snow_uv * 2.0 - 1.0;
    float radius = length(point);
    bool simple_shape = snow_shape <= 0 || snow_color.r < 0.5;
    float shape = 1.0 - smoothstep(0.18, 0.88, radius);
    if (!simple_shape)
    {
        float core = 1.0 - smoothstep(0.30, 0.92, radius);
        float arms = 1.0 - smoothstep(0.08, 0.23,
            min(abs(point.x), min(abs(dot(point, vec2(0.5, 0.866025))),
                                  abs(dot(point, vec2(0.5, -0.866025))))));
        shape = max(core, arms * (1.0 - smoothstep(0.35, 1.0, radius)));
        if (snow_shape >= 2)
        {
            // Preserve real snow's sixfold symmetry. Fold the polar angle into
            // one sector, then add paired side branches to every main arm.
            float sector = abs(mod(atan(point.y, point.x) + 0.523598776,
                                   1.047197551) - 0.523598776);
            vec2 hex_point = vec2(radius * cos(sector), radius * sin(sector));
            float branch_line = abs(hex_point.y -
                                    abs(hex_point.x - 0.53) * 0.48);
            float branches = 1.0 - smoothstep(0.035, 0.105, branch_line);
            float branch_range = smoothstep(0.25, 0.36, hex_point.x) *
                                 (1.0 - smoothstep(0.68, 0.82, hex_point.x));
            shape = max(shape, branches * branch_range * 0.88);
        }
    }
    float alpha = shape * snow_color.a;
    if (alpha < 0.01)
    {
        discard;
    }
    frag_color = vec4(snow_light_color, alpha);
}
