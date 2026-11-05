/**
 * asrSceneCompositeF.glsl
 * The unlit view of the G-buffer: albedo, with a soft light from the normal so shapes
 * read. Real lighting replaces this in P4.
 */
#version 460

layout(set = 1, binding = 1, rgba8) uniform readonly image2D albedo;
layout(set = 1, binding = 2, rgba16f) uniform readonly image2D normals;

layout(location = 0) out vec4 frag_color;

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const vec4 color = imageLoad(albedo, pixel);
    if (color.a == 0.0)
    {
        frag_color = vec4(0.32, 0.45, 0.62, 1.0);
        return;
    }
    const vec3 normal = imageLoad(normals, pixel).xyz;
    const float shade = 0.65 + 0.35 * dot(normal, normalize(vec3(0.4, 0.3, 0.87)));
    frag_color = vec4(color.rgb * shade, 1.0);
}
