/* the layout of glvsm.vsm, by hand: that of a compiled program (src/mxglv.hlsl) */
#ifndef GLVSM_H
#define GLVSM_H
/* vertex program mxglv: VU data memory layout, in quadwords */
#define VU_GLVSM_MVP 0
#define VU_GLVSM_CSCALE 4
#define VU_GLVSM_TMS 5
#define VU_GLVSM_TMT 6
#define VU_GLVSM_CMUL 7
#define VU_GLVSM_CADD 8
#define VU_GLVSM_ENTRY 0  /* micro address, 64-bit units; MSCAL (ITOP = the context) for the first batch of a draw, MSCNT for the others */
#define VU_GLVSM_BUF_QW 502  /* double buffer size */
#define VU_GLVSM_UNI_QW 9  /* uniforms, after the context's GIF tag */
#define VU_GLVSM_CTX_AT 1004  /* the contexts: GIF tag (NLOOP 0), uniforms */
#define VU_GLVSM_CTX_QW 10  /* per context */
#define VU_GLVSM_NCTX 2  /* contexts (1: a FLUSHE before writing it) */
#define VU_GLVSM_ITEM_QW 3  /* quadwords per vertex in */
#define VU_GLVSM_OUT_QW 3  /* GIF registers per vertex */
#define VU_GLVSM_MAX_N 81  /* vertices per batch */
#define VU_GLVSM_ALIGN 4  /* batches start at multiples of it */
#define VU_GLVSM_COUNTS 0  /* x vertices, z index quadwords, w indices */
#define VU_GLVSM_IN_AT 1  /* first item */
#define VU_GLVSM_OUT_AT 253  /* the output GIF tag */
#define VU_GLVSM_NREG 3
#define VU_GLVSM_REGS 0x512ull  /* GIF register list */
#define VU_GLVSM_NIN 3
#define VU_GLVSM_IN_POSITION 0  /* v4_32, 16 bytes */
#define VU_GLVSM_IN_TEXCOORD 1  /* v2_32, 8 bytes */
#define VU_GLVSM_IN_COLOR 2  /* v4_8u, 4 bytes */
#define VU_GLVSM_INPUTS {{0xc, 0, 16, 0x0}, {0x4, 0, 8, 0x0}, {0xe, 1, 4, 0x0}}  /* UNPACK format, unsigned, bytes, STMASK (fields 1.0 from COL, values from ROW) */
#define VU_GLVSM_ROW {0, 0, 0, 0}  /* ROW of the masked UNPACKs */

#endif
