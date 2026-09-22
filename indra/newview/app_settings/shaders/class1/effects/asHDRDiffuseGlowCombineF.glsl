/**
 * @file asHDRDiffuseGlowCombineF.glsl
 * @author chanayane@firestorm
 * @brief Add scene-linear bloom without modifying authored material-glow alpha.
 */

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform sampler2D emissiveRect;

in vec2 tc;

void main()
{
    vec4 scene = texture(diffuseRect, tc);
    vec3 bloom = texture(emissiveRect, tc).rgb;
    vec3 combined = max(scene.rgb + bloom, vec3(0.0));

    // Guard the FP16 target as one RGB vector. This is only a storage-safety
    // limit; ordinary highlight shaping remains the following tone map's job.
    float peak = max(combined.r, max(combined.g, combined.b));
    combined *= min(1.0, 65000.0 / max(peak, 0.000001));
    frag_color = vec4(combined, scene.a);
}
