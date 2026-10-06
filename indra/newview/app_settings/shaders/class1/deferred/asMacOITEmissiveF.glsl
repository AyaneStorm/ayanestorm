// AyaneStorm OIT shader. Author: chanayane@firestorm.
// Mac OIT emissive capture: replaces the emissive fragment shader; the glow
// weighting lives in asMacOITCaptureF.glsl.
/*[EXTRA_CODE_HERE]*/

void macoit_store_glow(float glow);

in vec4 vertex_color;
in vec2 vary_texcoord0;

void main()
{
    macoit_store_glow(diffuseLookup(vary_texcoord0.xy).a * vertex_color.a);
}
