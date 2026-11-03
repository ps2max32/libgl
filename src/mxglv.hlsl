// The vertex program of the GL layer (mxgl.c): one
// textured, Gouraud-shaded, perspective-correct program for every draw.
// mvp takes the vertex to GS coordinates times w (the GL matrices with the
// viewport, the depth range and the GS window offset in it); the EE has
// clipped the triangles to the near and far planes and the guard band.
// cscale scales the GL colour (0..255) to the GS's (0x80 is 1.0 when
// modulated by a texture, and for alpha). tms, tmt: the texture matrix's
// columns for s and t (xy), and its translation of S in tms.z, of T in
// tmt.z. cmul, cadd: the colour is col * cmul + cadd before the scale
// (libgl: 1 and 0); a program of a game's (glUseProgram) with these six
// uniforms has this program's data layout, so switching to it and back
// costs no FLUSH. The draws are triangle strips:
// pos.w is 1 for a vertex that ends no triangle (ADC: the first two of a
// strip, so one batch holds many strips: a polygon is a strip of its
// vertices, a triangle a strip of three).
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
Out mxglv(In v) {
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
