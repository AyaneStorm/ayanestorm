/**
 * @file wboitWeight.glsl
 * <AS:Chanayane> WBOIT weight function — McGuire & Bavoil 2013
 */

float wboit_weight(float a, float depth)
{
    return clamp(pow(clamp(a, 0.0, 1.0) + 0.01, 1.5) * 1e4 *
                 pow(1.0 - depth * 0.9, 12.0), 1e-2, 3e3);
}
