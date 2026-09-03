// <AS:Chanayane> Exact OIT ordered PBR glow capture: wave-level node allocation
// variant. exact_oit_reserve() and OITControl are declared in the linked
// exactOITReserveSubgroupF.glsl object instead of here; see fsexactoit.cpp.
/*[EXTRA_CODE_HERE]*/

uniform sampler2D diffuseMap;
uniform vec3 emissiveColor;
uniform sampler2D emissiveMap;
uniform float minimum_alpha;
in vec4 vertex_emissive;
in vec2 base_color_texcoord;
in vec2 emissive_texcoord;
vec3 srgb_to_linear(vec3 c);

layout(early_fragment_tests) in;
layout(binding = 0, r32ui) uniform coherent uimage2D oitHeadPointers;
layout(binding = 1, r32ui) uniform coherent uimage2D oitListCounts;
// <AS:Chanayane> Lossless 32-byte node: scalar glow and index-derived sequence.
struct OITNode { vec4 color; float glow; float depth; uint next; uint blend; };
// </AS:Chanayane>
layout(std430, binding = 0) buffer OITNodes { OITNode oitNodes[]; };
// Both declared in the linked exactOITReserveSubgroupF.glsl object, which owns
// the only OITControl declaration (a binding declared in two linked objects
// is a link error).
uint exact_oit_reserve(bool need);
void exact_oit_wave_max_pad(uint pixel_count);
// <AS:Chanayane> The caller (main(), below) already filters glow==0 before
// reaching here, so every lane that calls this function needs a node.
void exact_oit_store_glow(float glow)
{
    if (gl_HelperInvocation) return;
    uint index = exact_oit_reserve(true);
    if (index == 0xffffffffu) return;
    oitNodes[index].color = vec4(0.0);
    oitNodes[index].glow = glow;
    oitNodes[index].depth = gl_FragCoord.z;
    oitNodes[index].blend = 0xffffffffu;
    oitNodes[index].next = imageAtomicExchange(oitHeadPointers, ivec2(gl_FragCoord.xy), index);
    // Glow nodes participate in the same exact ordered list count.
    uint pixel_count = imageAtomicAdd(oitListCounts, ivec2(gl_FragCoord.xy), 1u) + 1u;
    exact_oit_wave_max_pad(pixel_count);
}
// </AS:Chanayane>

void main()
{
    vec4 basecolor = texture(diffuseMap, base_color_texcoord);
    if (basecolor.a < minimum_alpha) discard;
    vec3 emissive = emissiveColor * srgb_to_linear(texture(emissiveMap, emissive_texcoord).rgb);
    float glow = max(max(emissive.r, emissive.g), emissive.b) * vertex_emissive.a;
    // A zero glow node adds exactly 0 in the composite: allocate nothing. Exact.
    if (glow == 0.0) return;
    exact_oit_store_glow(glow);
}
// </AS:Chanayane>
