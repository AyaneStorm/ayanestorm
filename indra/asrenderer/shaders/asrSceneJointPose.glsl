/**
 * asrSceneJointPose.glsl
 * Continue rotation without extrapolating scale/shear. One result per joint per
 * frame serves all skins and attachments on Vulkan and Metal.
 * Author: chanayane@firestorm
 */

vec3 rotateJointVector(vec4 rotation, vec3 value)
{
    const vec3 twiceCross = 2.0 * cross(rotation.xyz, value);
    return value + rotation.w * twiceCross + cross(rotation.xyz, twiceCross);
}

Rows extrapolateJoint(Joint pose, float lead)
{
    const float sine = length(pose.rotationDelta.xyz);
    const float angle = lead * atan(sine, pose.rotationDelta.w);
    const float factor = sine > 1e-6 ? sin(angle) / sine : lead;
    const vec4 rotation = vec4(pose.rotationDelta.xyz * factor, cos(angle));
    Rows result;
    for (int column = 0; column < 3; ++column)
    {
        const vec3 value = rotateJointVector(rotation, vec3(pose.newest.r[0][column], pose.newest.r[1][column], pose.newest.r[2][column]));
        result.r[0][column] = value.x;
        result.r[1][column] = value.y;
        result.r[2][column] = value.z;
    }
    for (int row = 0; row < 3; ++row)
    {
        result.r[row].w = pose.newest.r[row].w + (pose.newest.r[row].w - pose.previous.r[row].w) * lead;
    }
    return result;
}
