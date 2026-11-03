// glconf's vertex program (GL_PS2_vertex_program_binary): libgl's own
// (src/mxglv.hlsl) with the colour as col * cmul + cadd before the
// texture environment's scale.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 cscale;
    float4 tms;
    float4 tmt;
    float4 cmul;
    float4 cadd;
};

struct In {
    float4 pos : POSITION;
    float2 st : TEXCOORD;
    uint4 col : COLOR_V4_8U;
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out glprog(In v) {
    float4 p = mul(mvp, float4(v.pos.xyz, 1));
    float q = rcp(p.w);
    float2 st = tms.xy * v.st.x + tmt.xy * v.st.y + float2(tms.z, tmt.z);
    Out o;
    o.st = float4(st * q, q, 0);
    o.rgbaq = vu::rgba8((float4(v.col) * cmul + cadd) * cscale);
    o.xyz2 = vu::ftoi4(p * q);
    o.xyz2.w = vu::adc(uint(v.pos.w));
    return o;
}
