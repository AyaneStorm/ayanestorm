/**
 * @file asDoFLiveCompleteF.glsl
 * @author chanayane@firestorm
 * @brief Live DoF: one mip level of the completed bins.
 *
 * Every bin behind N2 is read "alone", completed where nearer bins hide it,
 * by the push-pull recurrence
 *     c(l) = S (1 + k) / (V + k) + k (1 - V) / (V + k) c(l + 1),
 * ending with S / V at the top level (scripts/testing/dof_live_reference.py,
 * build_completed()). What the bin shows (V = 1) is its own sum; what is
 * hidden takes the visible part's own density S / V, blended continuously
 * toward the coarser levels as less is visible: a hole (V = 0) fills from
 * the content around it, and a surface split softly between two bins (on a
 * blur ramp) stays opaque in each. Plain push-pull (c(l) = S + (1 - V)
 * c(l + 1)) averaged the empty space around an object into its hidden part:
 * the far background leaked through the avatar as a grey veil in squares
 * (coarse mip texels). A threshold rule ("first level where half the
 * footprint shows the bin") fails in any hole as large as the visible part.
 *
 * Drawn once per level, top down; the gathers, tiles and composite then read
 * a bin with one trilinear fetch. Running the recurrence per tap read up to
 * three textures per level, about ten levels behind an avatar, for the same
 * result against brute force (doc, phase 5).
 *
 * Outputs: completed N1, F, B1, B2 (S.rgb, W), and the energies (E_N1, E_B1,
 * M_B2), each completed with its own bin's visibility.
 */

layout(location = 0) out vec4 done_near1;
layout(location = 1) out vec4 done_focus;
layout(location = 2) out vec4 done_back1;
layout(location = 3) out vec4 done_back2;
layout(location = 4) out vec4 done_energy;

// Raw bin sums (asDoFLiveReduceF.glsl), read at this level.
uniform sampler2D shadowMap0;   // N1 (S.rgb, W)
uniform sampler2D shadowMap1;   // F
uniform sampler2D shadowMap2;   // B1
uniform sampler2D shadowMap3;   // B2
uniform sampler2D shadowMap4;   // energies (E_N2, E_N1, E_B1, M_B2)
uniform sampler2D shadowMap5;   // visibility (V_N1, V_F, V_B1, V_B2)
// The completed level above (base level set to it), below the top only.
uniform sampler2D diffuseRect;  // N1
uniform sampler2D specularRect; // F
uniform sampler2D emissiveRect; // B1
uniform sampler2D bloomMap;     // B2
uniform sampler2D lightMap;     // energies (E_N1, E_B1, M_B2)
uniform int level;
uniform int top_level;

// k: weight of the coarser estimate against the level's own density.
const float LIVE_COMPLETE_PRIOR = 0.05;

vec4 complete(vec4 sums, float visibility, vec4 coarser, bool top)
{
    if (top)
    {
        return visibility > 0.000001 ? sums / visibility : vec4(0.0);
    }
    float density = 1.0 / (visibility + LIVE_COMPLETE_PRIOR);
    return sums * ((1.0 + LIVE_COMPLETE_PRIOR) * density) +
           coarser * (LIVE_COMPLETE_PRIOR * (1.0 - visibility) * density);
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    bool top = level >= top_level;
    vec2 uv = (vec2(p) + 0.5) / vec2(textureSize(shadowMap0, level));
    vec4 visibility = texelFetch(shadowMap5, p, level);
    vec4 energy = texelFetch(shadowMap4, p, level);
    vec4 coarse_energy = top ? vec4(0.0) : textureLod(lightMap, uv, 0.0);
    done_near1 = complete(texelFetch(shadowMap0, p, level), visibility.x,
                          top ? vec4(0.0) : textureLod(diffuseRect, uv, 0.0), top);
    done_focus = complete(texelFetch(shadowMap1, p, level), visibility.y,
                          top ? vec4(0.0) : textureLod(specularRect, uv, 0.0), top);
    done_back1 = complete(texelFetch(shadowMap2, p, level), visibility.z,
                          top ? vec4(0.0) : textureLod(emissiveRect, uv, 0.0), top);
    done_back2 = complete(texelFetch(shadowMap3, p, level), visibility.w,
                          top ? vec4(0.0) : textureLod(bloomMap, uv, 0.0), top);
    done_energy = vec4(complete(vec4(energy.y), visibility.x, vec4(coarse_energy.x), top).x,
                       complete(vec4(energy.z), visibility.z, vec4(coarse_energy.y), top).x,
                       complete(vec4(energy.w), visibility.w, vec4(coarse_energy.z), top).x,
                       0.0);
}
