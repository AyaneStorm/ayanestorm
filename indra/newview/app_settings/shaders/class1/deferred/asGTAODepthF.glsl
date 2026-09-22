/**
 * AyaneStorm GTAO depth preparation. Author: chanayane@firestorm.
 * Derived from the Intel XeGTAO implementation (see LICENSE in that repository).
 */
out float frag_depth;
in vec2 vary_fragcoord;

uniform sampler2D source_depth;
uniform vec2 gtao_projection_depth;
uniform int gtao_orthographic;

void main()
{
    float raw_depth = texture(source_depth, vary_fragcoord).r;
    float ndc_z = raw_depth * 2.0 - 1.0;
    float view_depth;
    if (gtao_orthographic != 0)
    {
        float view_z = (ndc_z - gtao_projection_depth.y) / gtao_projection_depth.x;
        view_depth = -view_z;
    }
    else
    {
        view_depth = gtao_projection_depth.y / (ndc_z + gtao_projection_depth.x);
    }
    frag_depth = (isnan(view_depth) || isinf(view_depth) || view_depth <= 0.0 || raw_depth >= 1.0)
        ? 65504.0 : min(view_depth, 65504.0);
}
