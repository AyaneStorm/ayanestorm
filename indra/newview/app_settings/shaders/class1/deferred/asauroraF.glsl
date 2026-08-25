/**
 * @file asauroraF.glsl
 * @author chanayane@firestorm
 * @brief Original AyaneStorm procedural northern-lights shader.
 */

in vec3 vary_aurora_direction;

uniform float aurora_time;
uniform float aurora_intensity;
uniform float aurora_speed;
uniform float aurora_scale;
uniform float aurora_coverage;
uniform float aurora_height;
uniform float aurora_thickness;
uniform int aurora_steps;
uniform vec2 aurora_world_origin;
uniform vec3 aurora_low_color;
uniform vec3 aurora_high_color;
uniform vec2 aurora_seed_offset;

out vec4 frag_data[4];

float as_hash(vec2 cell)
{
    vec3 p = fract(vec3(cell.xyx) * vec3(0.1031, 0.11369, 0.13787));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float as_value_noise(vec2 point)
{
    vec2 cell = floor(point);
    vec2 local = fract(point);
    local = local * local * (3.0 - 2.0 * local);
    float a = as_hash(cell);
    float b = as_hash(cell + vec2(1.0, 0.0));
    float c = as_hash(cell + vec2(0.0, 1.0));
    float d = as_hash(cell + vec2(1.0, 1.0));
    return mix(mix(a, b, local.x), mix(c, d, local.x), local.y);
}

float as_curtain_field(vec2 point, float time)
{
    float occupancy = as_value_noise(point * 0.045 + aurora_seed_offset
                                   + vec2(time * 0.004, time * 0.002));
    float coverage_edge = mix(0.90, 0.34, aurora_coverage);
    float coverage_mask = smoothstep(coverage_edge, coverage_edge + 0.10, occupancy)
                        * smoothstep(0.0, 0.05, aurora_coverage);

    point += aurora_seed_offset * 2.31;
    float broad = as_value_noise(point * 0.19 + vec2(time * 0.025, -time * 0.012));
    point.x += (broad - 0.5) * 5.0;
    float ridge = abs(as_value_noise(point * vec2(0.72, 0.16) + vec2(time * 0.06, 0.0)) - 0.5);
    float folds = 1.0 - smoothstep(0.035, 0.30, ridge);
    float detail = as_value_noise(point * vec2(2.7, 0.55) - vec2(time * 0.11, time * 0.018));
    return coverage_mask * folds * mix(0.25, 1.0, detail * detail);
}

void main()
{
    vec3 ray = normalize(vary_aurora_direction);
    vec3 color = vec3(0.0);
    float opacity = 0.0;

    if (ray.y > 0.015)
    {
        float horizon = smoothstep(0.015, 0.18, ray.y);
        float time = aurora_time * aurora_speed;
        float jitter = as_hash(gl_FragCoord.xy) - 0.5;

        for (int sample_index = 0; sample_index < 64; ++sample_index)
        {
            if (sample_index >= aurora_steps)
            {
                break;
            }

            float sample_fraction = (float(sample_index) + 0.5 + jitter * 0.55) / float(aurora_steps);
            float shell = aurora_height + (sample_fraction - 0.5) * aurora_thickness;
            float distance_to_shell = shell / max(ray.y, 0.015);
            vec2 world = aurora_world_origin * 0.0025 + ray.xz * distance_to_shell * 18.0;
            float field = as_curtain_field(world / aurora_scale, time);
            float vertical_profile = 1.0 - abs(sample_fraction * 2.0 - 1.0);
            float density = field * mix(0.25, 1.0, vertical_profile);
            float alpha = density * aurora_intensity * horizon * (1.6 / float(aurora_steps));
            vec3 emission = mix(aurora_low_color, aurora_high_color,
                                clamp(sample_fraction + field * 0.25, 0.0, 1.0));
            color += (1.0 - opacity) * emission * alpha;
            opacity += (1.0 - opacity) * alpha;
        }
    }

    frag_data[1] = vec4(0.0);
    // This pass is additively blended over an already initialized sky
    // G-buffer. Leave its normal/flag target unchanged by adding zero.
    frag_data[2] = vec4(0.0);
#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0.0);
    frag_data[3] = vec4(color, 0.0);
#else
    frag_data[0] = vec4(color, 0.0);
    frag_data[3] = vec4(0.0);
#endif
}
