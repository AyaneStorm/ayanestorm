// <AS:Chanayane> Exact OIT ordered PBR glow capture
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
struct OITNode { vec4 color; vec4 glow; float depth; uint next; uint blend; uint sequence; };
layout(std430, binding = 0) buffer OITNodes { OITNode oitNodes[]; };
layout(std430, binding = 1) buffer OITControl { uint oitNodeCount; uint oitNodeCapacity; uint oitOverflow; uint oitPad; };
void exact_oit_store_glow(float glow)
{
    uint index = atomicAdd(oitNodeCount, 1u);
    if (index >= oitNodeCapacity) { atomicOr(oitOverflow, 1u); return; }
    oitNodes[index].color = vec4(0.0);
    oitNodes[index].glow = vec4(glow, 0.0, 0.0, 0.0);
    oitNodes[index].depth = gl_FragCoord.z;
    oitNodes[index].blend = 0xffffffffu;
    oitNodes[index].sequence = index;
    oitNodes[index].next = imageAtomicExchange(oitHeadPointers, ivec2(gl_FragCoord.xy), index);
    // <AS:Chanayane> Glow nodes participate in the same exact ordered list count.
    uint pixel_count = imageAtomicAdd(oitListCounts, ivec2(gl_FragCoord.xy), 1u) + 1u;
    atomicMax(oitPad, pixel_count);
    // </AS:Chanayane>
}

void main()
{
    vec4 basecolor = texture(diffuseMap, base_color_texcoord);
    if (basecolor.a < minimum_alpha) discard;
    vec3 emissive = emissiveColor * srgb_to_linear(texture(emissiveMap, emissive_texcoord).rgb);
    exact_oit_store_glow(max(max(emissive.r, emissive.g), emissive.b) * vertex_emissive.a);
}
// </AS:Chanayane>
