/**
 * @file wboitCompositeF.glsl
 * <AS:Chanayane> WBOIT composite pass — blend accumulated transparency over opaque scene.
 * McGuire & Bavoil 2013.
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;   // slot 0 — wboitAccum (weighted color+alpha sum, RGBA16F)
uniform sampler2D specularRect;  // slot 1 — wboitReveal (weighted transmittance, R16F)

in vec2 vary_fragcoord;

void main()
{
    vec2 uv = vary_fragcoord.xy;

    vec4 accum  = texture(diffuseRect,  uv);
    float reveal = texture(specularRect, uv).r;

    // Guard against near-zero accum.a (fully transparent region)
    if (accum.a < 1e-5 && abs(reveal - 1.0) < 1e-5)
    {
        discard;
    }

    vec3 average_color = accum.rgb / max(accum.a, 1e-5);
    float alpha = 1.0 - reveal;

    frag_color = max(vec4(average_color, alpha), vec4(0));
}
