/**
 * Conservative transparent-group AABB proxy for AVBOIT occupancy.
 */

uniform mat4 modelview_projection_matrix;
uniform vec3 box_center;
uniform vec3 box_size;

in vec3 position;

void main()
{
    vec3 world_position = position * box_size + box_center;
    gl_Position = modelview_projection_matrix * vec4(world_position, 1.0);
}
