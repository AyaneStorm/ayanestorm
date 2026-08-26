/**
 * @file aslensflareF.glsl
 * @author chanayane@firestorm
 * @brief Screen-space celestial lens flares inspired by lensflares.shader.
 */

out vec4 frag_color;

uniform sampler2D depthMap;
uniform vec2 screen_res;
uniform vec2 flare_source_position[2];
uniform vec3 flare_source_color[2];
uniform float flare_source_strength[2];
uniform float flare_saturation;
uniform vec3 flare_snapshot_tile;
uniform int flare_snapshot_render;

in vec2 vary_fragcoord;

float saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

float circle(vec2 point, float radius)
{
    return length(point) - radius;
}

float hexagon(vec2 point, float radius)
{
    const vec3 shape = vec3(-0.866025404, 0.5, 0.577350269);
    point = abs(point);
    point -= 2.0 * min(dot(shape.xy, point), 0.0) * shape.xy;
    point -= vec2(clamp(point.x, -shape.z * radius, shape.z * radius), radius);
    return length(point) * sign(point.y);
}

vec3 ghost(vec2 point, vec2 source, float radius, float offset, float focus)
{
    vec2 center = (source - vec2(0.5)) * offset;
    vec2 local = point - center;
    float aperture = mix(circle(local, radius), hexagon(local, radius), 0.4);
    float ring = pow(saturate(1.0 - abs(length(local) - radius) * 8.0), 5.2) * 0.2;
    return mix(vec3(ring), vec3(pow(saturate(1.0 - aperture), 200.0)), focus);
}

float sourceVisibility(vec2 source)
{
    // Other snapshot tiles do not contain the depth texel at the celestial
    // source. The scene is therefore composited consistently without the
    // viewport-only depth test when assembling a tiled snapshot.
    if (flare_snapshot_render != 0)
    {
        return 1.0;
    }

    vec2 texel = 1.0 / screen_res;
    float visible = 0.0;
    visible += step(0.999, texture(depthMap, source).r) * 0.40;
    visible += step(0.999, texture(depthMap, source + vec2(texel.x * 3.0, 0.0)).r) * 0.15;
    visible += step(0.999, texture(depthMap, source - vec2(texel.x * 3.0, 0.0)).r) * 0.15;
    visible += step(0.999, texture(depthMap, source + vec2(0.0, texel.y * 3.0)).r) * 0.15;
    visible += step(0.999, texture(depthMap, source - vec2(0.0, texel.y * 3.0)).r) * 0.15;
    return visible;
}

vec3 lensFlare(vec2 point, vec2 source)
{
    // Keep only displaced aperture ghosts. The reference source burst and
    // offset-1 halo alter the celestial disc and look especially wrong when
    // the source is partly hidden by nearby geometry.
    vec3 ghosts = ghost(point, source, 0.50, 0.57, 0.0) * 0.15;
    ghosts += ghost(point, source, 0.010, 0.27, 0.3) * vec3(0.05, 0.50, 0.0);
    ghosts += ghost(point, source, 0.030, -0.23, 0.2) * vec3(0.50, 0.10, 0.0);
    ghosts += ghost(point, source, 0.070, -0.57, 0.2) * vec3(0.05, 0.0, 0.45);
    ghosts += ghost(point, source, 0.085, -1.07, 0.15) * vec3(0.0, 0.20, 0.05);
    ghosts += ghost(point, source, 0.030, -1.64, 0.2) * vec3(0.0, 0.30, 0.50);
    ghosts += ghost(point, source, 0.010, -1.76, 0.04) * vec3(0.10, 0.0, 0.65);

    ghosts = max(ghosts, vec3(0.0));
    float neutral_ghost = dot(ghosts, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(neutral_ghost), ghosts, flare_saturation);
}

void main()
{
    // Convert the current snapshot tile back to full-image UV coordinates.
    vec2 full_fragcoord = (vary_fragcoord + flare_snapshot_tile.yz) / flare_snapshot_tile.x;
    vec2 point = full_fragcoord - vec2(0.5);
    point.x *= screen_res.x / screen_res.y;
    vec3 color = vec3(0.0);

    for (int source_index = 0; source_index < 2; ++source_index)
    {
        if (flare_source_strength[source_index] > 0.0)
        {
            vec2 source = flare_source_position[source_index];
            vec2 aspect_source = source;
            aspect_source.x = 0.5 + (source.x - 0.5) * screen_res.x / screen_res.y;
            float edge_fade = saturate((0.75 - length(aspect_source - vec2(0.5))) * 4.0);
            color += lensFlare(point, aspect_source) * flare_source_color[source_index] *
                     flare_source_strength[source_index] * sourceVisibility(source) * edge_fade;
        }
    }

    frag_color = vec4(color, 0.0);
}
