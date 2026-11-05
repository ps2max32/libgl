// mxglv.hlsl with GL_EXP or GL_EXP2 fog: as mxglvf.hlsl, with tms.w the
// density and tmt.w 0 for GL_EXP, 1 for GL_EXP2. e^-x as (1 + x/32)^-32: five
// multiplications and a reciprocal, within 1/128 of it (the GS's F has 8 bits).
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
    int4 xyzf2 : GIF_XYZF2;
};

[shader("vertex")]
Out mxglve(In v) {
    float4 p = mul(mvp, float4(v.pos.xyz, 1));
    float q = rcp(p.w);
    float2 st = tms.xy * v.st.x + tmt.xy * v.st.y + float2(tms.z, tmt.z);
    float x = tms.w * p.w;
    x = x + tmt.w * (x * x - x);
    float y = 1.0 + x * (1.0 / 32.0);
    y = y * y;
    y = y * y;
    y = y * y;
    y = y * y;
    y = y * y;
    float f = 255.0 * rcp(y);
    Out o;
    o.st = float4(st * q, q, 0);
    o.rgbaq = vu::rgba8((float4(v.col) * cmul + cadd) * cscale);
    // packed XYZF2: F in bits 4..11 of w and ADC in bit 15, so w is
    // ftoi4(F + 2048 * adc). Z is in bits 4..27 of z, where XYZ2 has it in
    // bits 0..31: times 16, to compare with what is drawn without fog
    o.xyzf2 = vu::ftoi4(float4(p.xy * q, p.z * q * 16.0, f + v.pos.w * 2048.0));
    return o;
}
