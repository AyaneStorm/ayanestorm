/**
 * asrShadowShiftV.glsl
 * One triangle over each cascade's layer, an instance per cascade. See asrShadowShiftF.glsl.
 */
#version 460
#extension GL_ARB_shader_viewport_layer_array : require

layout(location = 0) flat out uint vary_layer;

void main()
{
    const vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
    gl_Layer = gl_InstanceIndex;
    vary_layer = uint(gl_InstanceIndex);
}
