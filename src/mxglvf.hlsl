// mxglv.hlsl with GL_LINEAR fog: the same uniforms, so switching between them
// costs no FLUSH; tms.w and tmt.w, which mxglv does not use, are a and b of
// the GS's fog value F = a * d + b (255: no fog, 0: all fog colour; the GS
// interpolates it across the triangle). d, the eye distance, is the vertex's
// w: GL's eye z under a perspective projection.
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
Out mxglvf(In v) {
    float4 p = mul(mvp, float4(v.pos.xyz, 1));
    float q = rcp(p.w);
    float2 st = tms.xy * v.st.x + tmt.xy * v.st.y + float2(tms.z, tmt.z);
    float f = clamp(tms.w * p.w + tmt.w, 0.0, 255.0);
    Out o;
    o.st = float4(st * q, q, 0);
    o.rgbaq = vu::rgba8((float4(v.col) * cmul + cadd) * cscale);
    // packed XYZF2: F in bits 4..11 of w and ADC in bit 15, so w is
    // ftoi4(F + 2048 * adc). Z is in bits 4..27 of z, where XYZ2 has it in
    // bits 0..31: times 16, to compare with what is drawn without fog
    o.xyzf2 = vu::ftoi4(float4(p.xy * q, p.z * q * 16.0, f + v.pos.w * 2048.0));
    return o;
}
