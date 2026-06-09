/**
 * @file wboitWeight.glsl
 * <AS:Chanayane> WBOIT weight function — McGuire & Bavoil 2013
 */

float wboit_weight(float a, float depth)
{
    return clamp(pow(min(1.0, a * 10.0) + 0.01, 3.0) * 1e8 *
                 pow(1.0 - depth * 0.9, 3.0), 1e-2, 3e3);
}
