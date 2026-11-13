// glconf's vertex program with inputs of its own (generic attributes): a
// position of four shorts, a second one to go to (blend), a colour of four
// bytes, and libgl's ADC.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 cscale;
    float4 blend; // x: the share of posb
};

struct In {
    int4 posa : POSITION_V4_16;
    int4 posb : TEXCOORD_V4_16;
    uint4 col : COLOR_V4_8U;
    uint4 adc : ADC_V4_8U; // libgl's: 1 where no triangle ends
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out glattr(In v) {
    float3 a = float3(v.posa.xyz), b = float3(v.posb.xyz);
    float4 p = mul(mvp, float4(a + (b - a) * blend.x, 1));
    float q = rcp(p.w);
    Out o;
    o.rgbaq = vu::rgba8(float4(v.col) * cscale);
    o.xyz2 = vu::ftoi4(p * q);
    o.xyz2.w = vu::adc(v.adc.x);
    return o;
}
