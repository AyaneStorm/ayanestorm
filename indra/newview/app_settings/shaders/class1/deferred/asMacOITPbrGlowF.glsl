// AyaneStorm OIT shader. Author: chanayane@firestorm.
// Mac OIT PBR glow capture: replaces the PBR glow fragment shader; the glow
// weighting lives in asMacOITCaptureF.glsl.
/*[EXTRA_CODE_HERE]*/

uniform sampler2D diffuseMap;
uniform vec3 emissiveColor;
uniform sampler2D emissiveMap;
uniform float minimum_alpha;
in vec4 vertex_emissive;
in vec2 base_color_texcoord;
in vec2 emissive_texcoord;
vec3 srgb_to_linear(vec3 c);
void macoit_store_glow(float glow);

void main()
{
    vec4 basecolor = texture(diffuseMap, base_color_texcoord);
    if (basecolor.a < minimum_alpha)
    {
        discard;
    }
    vec3 emissive = emissiveColor *
        srgb_to_linear(texture(emissiveMap, emissive_texcoord).rgb);
    macoit_store_glow(max(max(emissive.r, emissive.g), emissive.b) *
                      vertex_emissive.a);
}
