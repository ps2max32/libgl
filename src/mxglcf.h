/* the layout of mxglcf.vsm, by hand: mxglv's with room for clipping and more uniforms */
#ifndef MXGLCF_H
#define MXGLCF_H
/* vertex program mxglv: VU data memory layout, in quadwords */
#define VU_MXGLCF_MVP 0
#define VU_MXGLCF_CSCALE 4
#define VU_MXGLCF_TMS 5
#define VU_MXGLCF_TMT 6
#define VU_MXGLCF_CMUL 7
#define VU_MXGLCF_CADD 8
#define VU_MXGLCF_ENTRY 0  /* micro address, 64-bit units; MSCAL (ITOP = the context) for the first batch of a draw, MSCNT for the others */
#define VU_MXGLCF_BUF_QW 432  /* double buffer size */
#define VU_MXGLCF_UNI_QW 17  /* uniforms, after the context's GIF tag */
#define VU_MXGLCF_CTX_AT 864  /* the contexts: GIF tag (NLOOP 0), uniforms */
#define VU_MXGLCF_CTX_QW 18  /* per context */
#define VU_MXGLCF_NCTX 2  /* contexts (1: a FLUSHE before writing it) */
#define VU_MXGLCF_ITEM_QW 3  /* quadwords per vertex in */
#define VU_MXGLCF_OUT_QW 3  /* GIF registers per vertex */
#define VU_MXGLCF_MAX_N 70  /* vertices per batch */
#define VU_MXGLCF_ALIGN 4  /* batches start at multiples of it */
#define VU_MXGLCF_COUNTS 0  /* x vertices, z index quadwords, w indices */
#define VU_MXGLCF_IN_AT 1  /* first item */
#define VU_MXGLCF_OUT_AT 220  /* the output GIF tag */
#define VU_MXGLCF_NREG 3
#define VU_MXGLCF_REGS 0x412ull  /* GIF register list */
#define VU_MXGLCF_NIN 3
#define VU_MXGLCF_IN_POSITION 0  /* v4_32, 16 bytes */
#define VU_MXGLCF_IN_TEXCOORD 1  /* v2_32, 8 bytes */
#define VU_MXGLCF_IN_COLOR 2  /* v4_8u, 4 bytes */
#define VU_MXGLCF_INPUTS {{0xc, 0, 16, 0x0}, {0x4, 0, 8, 0x0}, {0xe, 1, 4, 0x0}}  /* UNPACK format, unsigned, bytes, STMASK (fields 1.0 from COL, values from ROW) */
#define VU_MXGLCF_ROW {0, 0, 0, 0}  /* ROW of the masked UNPACKs */

#endif
