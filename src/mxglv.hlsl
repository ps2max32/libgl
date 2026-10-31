// The vertex program of the GL layer (mxgl.c): one
// textured, Gouraud-shaded, perspective-correct program for every draw.
// mvp takes the vertex to GS coordinates times w (the GL matrices with the
// viewport, the depth range and the GS window offset in it); the EE has
// clipped the triangles to the near and far planes and the guard band.
// cscale scales the GL colour (0..255) to the GS's (0x80 is 1.0 when
// modulated by a texture, and for alpha). The draws are triangle strips:
// pos.w is 1 for a vertex that ends no triangle (ADC: the first two of a
// strip, so one batch holds many strips: a polygon is a strip of its
// vertices, a triangle a strip of three).
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 cscale;
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
Out mxglv(In v) {
    float4 p = mul(mvp, float4(v.pos.xyz, 1));
    float q = rcp(p.w);
    Out o;
    o.st = float4(v.st * q, q, 0);
    o.rgbaq = vu::rgba8(float4(v.col) * cscale);
    o.xyz2 = vu::ftoi4(p * q);
    o.xyz2.w = vu::adc(uint(v.pos.w));
    return o;
}
