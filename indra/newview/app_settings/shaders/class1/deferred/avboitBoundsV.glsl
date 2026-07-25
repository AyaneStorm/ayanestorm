/**
 * Conservative transparent-group AABB proxy for AVBOIT occupancy.
 */

uniform mat4 modelview_projection_matrix;
uniform vec3 box_center;
uniform vec3 box_size;

in vec3 position;

#ifdef HAS_SKIN
mat4 getObjectSkinnedTransform();
uniform mat4 modelview_matrix;
uniform mat4 projection_matrix;
#endif

void main()
{
#ifdef HAS_SKIN
    mat4 skinned_transform =
        modelview_matrix * getObjectSkinnedTransform();
    gl_Position =
        projection_matrix * skinned_transform * vec4(position, 1.0);
#else
    vec3 world_position = position * box_size + box_center;
    gl_Position = modelview_projection_matrix * vec4(world_position, 1.0);
#endif
}
