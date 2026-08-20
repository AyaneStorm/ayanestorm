/**
 * @file asVolumetricLocalLightF.glsl
 * @brief AyaneStorm bounded, unshadowed local-light volumetric scatter.
 * @author chanayane@firestorm
 *
 * Local lights have no general omnidirectional shadow maps in the viewer.
 * This opt-in approximation therefore models illuminated fog inside each
 * light sphere and deliberately makes no claim of wall occlusion.
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;
in vec2 vary_fragcoord;

uniform int local_light_count;
uniform vec4 local_light[64];       // agent-space xyz, radius in w
uniform vec4 local_light_color[64]; // linear RGB, legacy falloff in w
uniform float local_light_intensity;
uniform int debug_mode;
uniform mat4 modelview_matrix;

vec4 getPosition(vec2 pos_screen);

const int LOCAL_STEPS = 8;
// The viewer's local-light colors are scene-radiance values intended for
// direct surface lighting. Fog integrates that energy over distance, so using
// it unchanged overwhelms the HDR target even at the bottom of the UI range.
// Keep the user setting in an artist-friendly 0..6 range and normalize the
// approximation here instead.
const float LOCAL_SCATTER_RADIANCE_SCALE = 0.02;

void main()
{
    vec3 ray_end = getPosition(vary_fragcoord).xyz;
    float scene_distance = min(length(ray_end), 128.0);
    vec3 ray_dir = ray_end / max(length(ray_end), 1e-4);
    vec3 result = vec3(0.0);
    float volume_hits = 0.0;

    for (int light_index = 0; light_index < local_light_count; ++light_index)
    {
        vec3 center = (modelview_matrix * vec4(local_light[light_index].xyz, 1.0)).xyz;
        float radius = local_light[light_index].w;

        float along = dot(center, ray_dir);
        float center_distance_sq = dot(center, center) - along * along;
        float half_chord_sq = radius * radius - center_distance_sq;
        if (half_chord_sq <= 0.0)
        {
            continue;
        }

        float half_chord = sqrt(half_chord_sq);
        float entry = max(along - half_chord, 0.0);
        float exit_distance = min(along + half_chord, scene_distance);
        if (exit_distance <= entry)
        {
            continue;
        }

        volume_hits += 1.0;

        float step_length = (exit_distance - entry) / float(LOCAL_STEPS);
        float accumulated = 0.0;
        for (int sample_index = 0; sample_index < LOCAL_STEPS; ++sample_index)
        {
            float distance_along_ray = entry + (float(sample_index) + 0.5) * step_length;
            vec3 sample_position = ray_dir * distance_along_ray;
            float normalized_distance = length(sample_position - center) / radius;
            float attenuation = max(1.0 - normalized_distance, 0.0);
            attenuation = pow(attenuation, mix(1.0, 4.0,
                              clamp(local_light_color[light_index].w, 0.0, 1.0)));
            accumulated += attenuation;
        }

        float integrated = (accumulated / float(LOCAL_STEPS)) *
                           ((exit_distance - entry) / max(radius, 0.001));
        result += local_light_color[light_index].rgb * integrated;
    }

    if (debug_mode == 9)
    {
        // Fraction of selected light volumes intersecting this visible ray.
        frag_color = vec4(vec3(volume_hits / max(float(local_light_count), 1.0)), 0.0);
        return;
    }

    // Mode 8 reaches this output through replace compositing, exposing only
    // local scatter. Mode 0 additively combines the same signal normally.
    frag_color = vec4(result * local_light_intensity * LOCAL_SCATTER_RADIANCE_SCALE, 0.0);
}
