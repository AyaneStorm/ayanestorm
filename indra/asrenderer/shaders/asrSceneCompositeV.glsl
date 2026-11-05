/**
 * asrSceneCompositeV.glsl
 * One triangle over the whole target.
 */
#version 460

void main()
{
    const vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
