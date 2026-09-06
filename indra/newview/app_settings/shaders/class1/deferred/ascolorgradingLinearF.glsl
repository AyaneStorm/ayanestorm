/**
 * @file ascolorgradingLinearF.glsl
 * @author chanayane@firestorm
 * @brief Scene-linear exposure and creative white balance.
 */
uniform int as_color_grade_linear_enabled;
uniform float as_color_grade_exposure;
uniform mat3 as_color_grade_white_balance;

vec3 asApplyLinearColorGrade(vec3 color)
{
    if (as_color_grade_linear_enabled == 0)
    {
        return color;
    }
    return max(as_color_grade_white_balance * color * exp2(as_color_grade_exposure), vec3(0.0));
}
