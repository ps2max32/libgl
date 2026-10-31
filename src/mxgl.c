/* mxgl.c: an OpenGL 1.1 subset on the GS through maxsdk's frame pipeline
   (<mx/frame.h>), for the GL renderers of games (the Quake engines).

   - A draw (glDrawElements, glDrawArrays, glBegin/glEnd) is clipped on the
     EE: each vertex is taken to GS coordinates (times w) by the matrix mvp
     below, and the triangles that cross the near or far plane or leave the
     GS guard band are cut (Sutherland-Hodgman, all attributes linear). The
     triangles are written as three vertex streams (position, texture
     coordinates, colour) into the frame's arena, and VU1 runs the HLSL
     program mxglv.hlsl on them (mx_frame_shader_draw): transform, divide,
     ST * Q, colour scale.
   - GL state becomes GS registers (TEX0/1, CLAMP, ALPHA, TEST, ZBUF,
     SCISSOR), written by DIRECT after a FLUSH when they change.
   - Textures are kept in main memory in the GS's format (CT32, or CT16 when
     they have no alpha, or T8 for brightness textures: mxgl_TexBrightness,
     GL_LUMINANCE) with mipmap levels made here, and copied into GS memory
     in the stream (IMAGE by ref tags) when a draw needs one that is not
     there; when GS memory is full, all are dropped and it is filled again
     from the start.
   - The GS cannot multiply by the destination colour, only by the source
     alpha: the blends that do (lightmaps) are drawn as Cd * As with a
     brightness texture, whose CLUT puts its texels in alpha.
   - Display lists (mxgl_NewList ...) keep a draw's streams as a DMA chain
     the frame stream calls.
   - The screen is 640 x 448, NTSC interlaced FIELD mode: two 16-bit
     colour buffers (CT16S, dithered) and one 24-bit Z buffer, which leaves
     about 1.8 MiB of GS memory for textures.
   - MXGL_HIRES: 1920 x 1080 interlaced (1080i) by maxsdk's hires passes
     (<mx/hires.h>): one 1920 x 540 CT16S buffer drawn in three strips just
     ahead of the beam, every field, from the frame's stream recorded; Z
     (16 bits) in the next strip; each field's lines half a line apart, so
     the game's 1080 lines are all drawn. The passes own FRAME, ZBUF,
     XYOFFSET and SCISSOR: a draw without Z writes uses GS context 2 (ZMSK
     set), so the state registers are written in both contexts, and the GL
     scissor is not kept. About 1.8 MiB of GS memory is left for textures.

   Not done: lines and points, stencil, clip planes, polygon offset, colour
   mask, glReadPixels, the other blends with the destination colour (the
   draw is skipped). */

#include <stdlib.h>
#include <string.h>

#include <malloc.h>
#include <math.h>
#include <mx/mx.h>
#include <mx/hud.h>
#include <mx/sif.h>
#include <mx/tex.h>

#include <GL/gl.h>
#include "mxglv.h"

#include "mxgl.h"

extern const u8 mxglv_vu[], mxglv_vu_end[];

#ifdef MXGL_HIRES
#include <mx/hires.h>
#define FB_W 1920
#define FB_H 540
#define ZMAX 4095.0f            /* GS Z (16 bits) / 16: ftoi4 */
#define NARENA MX_HIRES_SLOTS   /* an arena per record */
#define HUD_SCALE 3
#else
#define FB_W 640
#define FB_H 448
#define ZMAX 1048575.0f         /* GS Z (24 bits) / 16: ftoi4 */
#define NARENA 2
#define HUD_SCALE 1
#endif
/* the window the game sees (mxgl_window), mapped onto the whole buffer */
static int GL_W = MXGL_WIDTH, GL_H = MXGL_HEIGHT;
#define GB_LO (2048.0f - 1900.0f) /* the guard band, GS pixels */
#define GB_HI (2048.0f + 1900.0f)

/* ---- frame stream, arenas ---------------------------------------------------- */

#define RING_BYTES (64 * 1024) /* the stream's slots (chain mode) */
static u8 ring[RING_BYTES] MX_ALIGNED(64);
#define ARENA_BYTES (512 * 1024)
#define REC_BYTES (MX_HIRES_SLOTS * 512 * 1024) /* hires: the records */
static u8 *arena[NARENA];
static u32 arena_pos;
static int in_frame, buf, frame_par;
static struct mx_shader shader;
static struct mx_rt rt[2];
static struct mx_display disp[2];
static u32 dropped_draws;

/* counts of the frame being built, and those of the frame before (stats) */
struct mxgl_stats {
  u32 draws, tris, uploads, upload_kb, wraps;
  u32 gl_cycles; /* EE cycles in draw_tris */
  u32 xf_cycles, tri_cycles, flush_cycles; /* its parts: transform, triangles, sending */
  u32 verts;
  u32 lists; /* draws by a CALL of a list's chain */
  u32 c_bounds, c_state, c_call; /* cycles of the calls' parts */
};
static struct mxgl_stats st_cur, st_last;
int mxgl_show_stats = 1;

/* ---- textures ---------------------------------------------------------------- */

#define MAX_TEX 2048
/* A texture and its MIPMAP levels, down to 8 x 8 (bilinear filters are
   wrong below; also whole quadwords for IMAGE), MX_TEX_LEVELS at most. The
   engine sends the levels (glTexImage2D level n); levels has those that
   made here from level 0 (box filter) for every texture, whether the engine
   asked for levels or not (its level n calls are ignored). */
struct tex {
  int used;
  u32 w, h, lw, lh;      /* level 0: size, log2 */
  u32 psm;
  u32 max_levels;        /* the levels it can have */
  u32 levels;            /* the levels that came (1 .. max_levels) */
  u32 off[MX_TEX_LEVELS]; /* each level in data */
  u32 bytes;             /* all levels */
  void *data;            /* GS format, 64-byte aligned */
  struct mx_tex mt;      /* the levels in GS memory, valid if gen == vram_gen */
  u32 gen;
  int alpha;
  int clamp_s, clamp_t;
  int lin_mag, lin_min;  /* filters: 1 linear */
  int mip;               /* MIN_FILTER: 0 no levels, 1 nearest level, 2 both levels */
};
static struct tex texs[MAX_TEX];
static u32 vram_lo, vram_pos, vram_gen = 1;
#define VRAM_END 16384u /* 64-word blocks in 4 MiB */

/* ---- GL state ------------------------------------------------------------- */

static struct {
  float mv[8][16], pr[4][16];
  int mv_top, pr_top;
  GLenum mode;
  int vp[4], sc[4];
  float dn, df;
  int tex2d, blend, atest, dtest, cull, scissor;
  GLenum sf, dfac, afunc, dfunc, cullface, texenv;
  float aref;
  int dmask;
  int cshift; /* mxgl_ColorShift */
  float color[4];
  float clear[4];
  /* arrays */
  const float *vptr;
  int vsize, vstride;
  const float *tptr;
  int tstride;
  const u8 *cptr;
  int cstride;
  int va, ta, ca;
  GLuint bound;
  int cunit;
} gl;

/* bumped by each register write (write_regs), and when the copy below is
   dropped: the list calls' state is right while it stays */
static u32 gs_serial;
/* GS registers last written in this frame */
static struct {
  u64 tex0, tex1, clamp, alpha, test, zbuf, scissor, mip1, mip2;
  int valid;
} gs;

/* ---- vectors (VU0 macro mode: -mvu0) ---------------------------------------- */

typedef float v4f __attribute__((vector_size(16)));

/* m * (x, y, z, 1), m column-major (4 columns of 4) */
static inline v4f xform(const v4f *m, float x, float y, float z) {
  return m[0] * x + m[1] * y + m[2] * z + m[3];
}

/* ---- matrices ------------------------------------------------------------- */

static void mat_ident(float *m) {
  memset(m, 0, 64);
  m[0] = m[5] = m[10] = m[15] = 1;
}

/* r = a * b, column-major */
static void mat_mul(float *r, const float *a, const float *b) {
  float t[16];
  for (int c = 0; c < 4; c++)
    for (int rr = 0; rr < 4; rr++)
      t[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] +
                      a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
  memcpy(r, t, 64);
}

static float *cur_mat(void) {
  return gl.mode == GL_PROJECTION ? gl.pr[gl.pr_top] : gl.mv[gl.mv_top];
}

/* ---- the stream ---------------------------------------------------------- */

static void clut_send(void);

static void frame_start(void) {
  if (in_frame)
    return;
#ifdef MXGL_HIRES
  /* the passes set the target and clear the strips; the arena is the
     record's (kept while the record is played) */
  mx_frame_begin_n();
  frame_par = (int)mx_hires_slot();
  mx_hires_clear((u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255), (u32)(gl.clear[2] * 255));
  /* the frame before overwrote textures (GS memory refilled): this one
     uploads all it uses, as its passes are played again after the refill */
  if (st_last.wraps) {
    vram_gen++;
    vram_pos = vram_lo;
  }
  u64 *e = mx_frame_direct(5);
  e = mx_e_ad_tag(e, 4, 1);
#else
  buf = (int)(mx_frame_begin_n() & 1);
  mx_frame_target(&rt[buf], 0);
  frame_par ^= 1;
  /* TEST off for the clear, PRMODECONT, COLCLAMP, dithering into 16 bits */
  u64 *e = mx_frame_direct(6 + 5);
  e = mx_e_ad_tag(e, 5, 0);
  e = mx_e_ad(e, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
#endif
  e = mx_e_ad(e, MX_GS_REG_PRMODECONT, 1);
  e = mx_e_ad(e, MX_GS_REG_COLCLAMP, 1);
  e = mx_e_ad(e, MX_GS_REG_DIMX, 0x6071243571603524ull);
  e = mx_e_ad(e, MX_GS_REG_DTHE, 1);
#ifndef MXGL_HIRES
  e = mx_rt_e_clear(e, &rt[buf], 0, (u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255),
                    (u32)(gl.clear[2] * 255), 1);
#endif
  mx_frame_commit(e);
  in_frame = 1;
  arena_pos = 0;
  gs.valid = 0, gs_serial++;
  mx_frame_shader_setup(&shader);
  /* TEXA: CT16 alpha bit 1 is 0x80, 0 is 0 */
  u64 *c = mx_frame_direct(1 + 2);
  c = mx_e_ad_tag(c, 2, 1);
  c = mx_e_ad(c, MX_GS_REG_TEXA, 0x80ull << 32);
  c = mx_e_ad(c, MX_GS_REG_PABE, 0);
  mx_frame_commit(c);
  clut_send();
}

/* the counters, top left: frames a second (over the last second), EE time
   from frame to frame, draws, triangles, textures copied into GS memory,
   times GS memory was refilled, arena and GS memory used */
static void draw_stats(void) {
  static u32 v_last, f_last, fps10, t_last, ee_us;
  u32 now = mx_count();
  ee_us = (now - t_last) / (MX_EE_HZ / 1000000);
  t_last = now;
  int log_now = 0;
  if (mx_gs.vsync - v_last >= mx_gs.hz) {
    fps10 = (mx_gs.frames - f_last) * 10 * mx_gs.hz / (mx_gs.vsync - v_last);
    v_last = mx_gs.vsync, f_last = mx_gs.frames;
    log_now = 1;
  }
  /* full screen, no tests */
  u64 *c = mx_frame_reserve(5);
  c = mx_e_cnt(c, 3, mx_vif_flush(), mx_vif_direct(3));
  c = mx_e_ad_tag(c, 2, 1);
#ifdef MXGL_HIRES
  c = mx_e_ad(c, MX_GS_REG_ALPHA_1, mx_gs_alpha(0, 1, 0, 1, 0)); /* the box: over */
#else
  c = mx_e_ad(c, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, FB_W - 1, 0, FB_H - 1));
#endif
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  mx_frame_commit(c);
  gs.valid = 0, gs_serial++;
  char a[96], b[96];
  char *p = mx_hud_cat(a, "FPS ");
  p = mx_hud_u1(p, fps10, 10);
  p = mx_hud_cat(p, "  EE ");
  p = mx_hud_u1(p, ee_us / 100, 10);
  p = mx_hud_cat(p, " MS  GL ");
  p = mx_hud_u1(p, st_last.gl_cycles / (MX_EE_HZ / 10000), 10);
  p = mx_hud_cat(p, " MS  DRAWS ");
  p = mx_hud_u(p, st_last.draws);
  p = mx_hud_cat(p, "  TRIS ");
  p = mx_hud_u(p, st_last.tris);
  p = mx_hud_cat(p, "  LISTS ");
  mx_hud_u(p, st_last.lists);
  p = mx_hud_cat(b, "TEX ");
  p = mx_hud_u(p, st_last.uploads);
  p = mx_hud_cat(p, "/");
  p = mx_hud_u(p, st_last.upload_kb);
  p = mx_hud_cat(p, "KB  REFILL ");
  p = mx_hud_u(p, st_last.wraps);
  p = mx_hud_cat(p, "  ARENA ");
  p = mx_hud_u(p, arena_pos / 1024);
  p = mx_hud_cat(p, "KB  VRAM ");
  mx_hud_u(p, (vram_pos - vram_lo) / 4);
  static u32 ring0, buf0;
  if (log_now) {
    u32 c = MX_EE_HZ / 10000;
    mx_printf("stats: %s | %s\n", a, b);
    mx_printf("stats: xf %u.%u tri %u.%u (incl flush %u.%u) ms, verts %u, wait ring %u ms buf %u ms "
              "(since last)\n",
              st_last.xf_cycles / c / 10, st_last.xf_cycles / c % 10, st_last.tri_cycles / c / 10,
              st_last.tri_cycles / c % 10, st_last.flush_cycles / c / 10,
              st_last.flush_cycles / c % 10, st_last.verts,
              (mx_frame_stats.wait_ring - ring0) / (MX_EE_HZ / 1000),
              (mx_frame_stats.wait_buffer - buf0) / (MX_EE_HZ / 1000));
    ring0 = mx_frame_stats.wait_ring, buf0 = mx_frame_stats.wait_buffer;
    mx_printf("stats: lists: bounds %u us, state %u us, call %u us\n", st_last.c_bounds / 295,
              st_last.c_state / 295, st_last.c_call / 295);
#ifdef MXGL_HIRES
    mx_printf("stats: hires: fields %u, passes %u, late %u, records shown %u, largest %u KB of %u\n",
              mx_hires_stats.fields, mx_hires_stats.passes, mx_hires_stats.late,
              mx_hires_stats.shown, mx_hires_stats.rec_max / 1024, REC_BYTES / MX_HIRES_SLOTS / 1024);
#endif
  }
  mx_hud_box(4 * HUD_SCALE, 2, 360 * HUD_SCALE, 20 * HUD_SCALE, 0x60000000);
  mx_hud_text(8 * HUD_SCALE, 4, a, 0x80ff80, HUD_SCALE);
  mx_hud_text(8 * HUD_SCALE, 4 + 9 * HUD_SCALE, b, 0x80ff80, HUD_SCALE);
}

void mxgl_end_frame(void) {
  if (!in_frame)
    frame_start();
  if (mxgl_show_stats)
    draw_stats();
  st_last = st_cur;
  memset(&st_cur, 0, sizeof st_cur);
  mx_frame_end();
  in_frame = 0;
}

/* register writes after a FLUSH (the VU1 batches before are drawn first) */
static void write_regs(const u32 *reg, const u64 *val, int n) {
  gs_serial++;
  if (!n)
    return;
#ifdef MXGL_HIRES
  /* the same in context 2 (the draws without Z writes) */
  u32 reg2[24];
  u64 val2[24];
  for (int i = 0; i < n; i++) {
    reg2[2 * i] = reg[i], val2[2 * i] = val[i];
    reg2[2 * i + 1] = reg[i] + 1, val2[2 * i + 1] = val[i];
  }
  reg = reg2, val = val2, n *= 2;
#endif
  u64 *c = mx_frame_reserve((u32)n + 2);
  c = mx_e_cnt(c, (u32)n + 1, mx_vif_flush(), mx_vif_direct((u32)n + 1));
  c = mx_e_ad_tag(c, (u32)n, 1);
  for (int i = 0; i < n; i++)
    c = mx_e_ad(c, reg[i], val[i]);
  mx_frame_commit(c);
}

/* ---- textures in GS memory --------------------------------------------- */

static u32 pow2_ceil(u32 v) {
  u32 p = 1;
  while (p < v)
    p <<= 1;
  return p;
}

static u32 tex_bpp(const struct tex *t) {
  return t->psm == MX_GS_PSM_CT32 ? 4 : t->psm == MX_GS_PSM_T8 ? 1 : 2;
}

/* the levels a draw reads: all that came, if the filter uses levels */
static u32 tex_levels(const struct tex *t) { return t->levels; }

/* an IMAGE transfer of w x h texels of psm at dbp (FLUSH first if flush):
   a DIRECT of BITBLTBUF, TRXPOS, TRXREG, TRXDIR; then per piece a DIRECT
   of its IMAGE tag and a ref tag whose own DIRECT sends the texels (a
   DIRECT must not go on past a tag: the next tag's upper half is VIFcodes) */
static void image_xfer(u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, u32 bpp, const void *data,
                       int flush) {
  u32 qwc = w * h * bpp / 16;
  st_cur.upload_kb += qwc / 64;
  u64 *c = mx_frame_reserve(6 + 3 * (qwc / 0x7FFF + 1));
  const u8 *d = data;
  c = mx_e_cnt(c, 5, flush ? mx_vif_flush() : mx_vif_nop(), mx_vif_direct(5));
  c = mx_e_ad_tag(c, 4, 0);
  c = mx_e_ad(c, MX_GS_REG_BITBLTBUF, mx_gs_bitbltbuf(0, 0, 0, dbp, dbw, psm));
  c = mx_e_ad(c, MX_GS_REG_TRXPOS, mx_gs_trxpos(0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_TRXREG, mx_gs_trxreg(w, h));
  c = mx_e_ad(c, MX_GS_REG_TRXDIR, 0);
  while (qwc) {
    u32 n = qwc > 0x7FFF ? 0x7FFF : qwc;
    c = mx_e_cnt(c, 1, mx_vif_nop(), mx_vif_direct(1));
    c = mx_e_giftag(c, n, n == qwc, -1, MX_GIF_FLG_IMAGE, 0, 0);
    c = mx_e_ref(c, d, n, mx_vif_nop(), mx_vif_direct(n));
    d += n * 16;
    qwc -= n;
  }
  mx_frame_commit(c);
}

static void texflush(void) {
  u64 *c = mx_frame_reserve(3);
  c = mx_e_cnt(c, 2, 0, mx_vif_direct(2));
  c = mx_e_ad_tag(c, 1, 1);
  c = mx_e_ad(c, MX_GS_REG_TEXFLUSH, 0);
  mx_frame_commit(c);
}

/* the CLUTs of the brightness textures (T8): entry i is grey i, and in
   alpha (0x80 is 1.0 in the blend Cd * As): i (up to 2x, for overbright),
   i / 2 (1x), (255 - i) / 2 (1x of the inverse, blendFunc ZERO
   ONE_MINUS_SRC_COLOR); in CSM1's order (bits 3 and 4 of the index
   swapped), each 16 x 16 CT32 (4 blocks) */
enum { CLUT_2X, CLUT_1X, CLUT_INV, NCLUT };
static u32 clut_cbp;
static u32 clut[NCLUT][256] MX_ALIGNED(64);

static void clut_make(void) {
  for (u32 i = 0; i < 256; i++) {
    u32 at = (i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1, g = i | i << 8 | i << 16;
    clut[CLUT_2X][at] = g | i << 24;
    clut[CLUT_1X][at] = g | ((i + 1) >> 1) << 24;
    clut[CLUT_INV][at] = g | ((256 - i) >> 1) << 24;
  }
  mx_dcache_writeback(clut, sizeof clut);
}

static void clut_send(void) {
  for (int k = 0; k < NCLUT; k++)
    image_xfer(clut_cbp + 4 * (u32)k, 1, MX_GS_PSM_CT32, 16, 16, 4, clut[k], k == 0);
  texflush();
}

static void tex_upload(struct tex *t) {
  u32 nl = tex_levels(t);
  struct mx_tex probe;
  u32 blocks = mx_tex_init(&probe, 0, t->psm, t->lw, t->lh, nl, MX_TEX_PACKED);
  if (blocks > VRAM_END - vram_lo) {
    t->gen = 0;
    return;
  }
  /* the packed layout keeps its alignments if the base is aligned to the
     whole (a page at most) */
  u32 align = blocks >= 32 ? 32 : pow2_ceil(blocks);
  u32 at = (vram_pos + align - 1) & ~(align - 1);
  if (at + blocks > VRAM_END) {
    vram_gen++;
    st_cur.wraps++;
    at = (vram_lo + align - 1) & ~(align - 1);
  }
  vram_pos = at + blocks;
  mx_tex_init(&t->mt, at, t->psm, t->lw, t->lh, nl, MX_TEX_PACKED);
  t->gen = vram_gen;
  st_cur.uploads++;

  for (u32 lv = 0; lv < nl; lv++)
    image_xfer(t->mt.tbp[lv], t->mt.tbw[lv], t->psm, t->w >> lv, t->h >> lv, tex_bpp(t),
               (const u8 *)t->data + t->off[lv], lv == 0);
  texflush();
}

/* ---- draw state ---------------------------------------------------------- */

static int clut_pick; /* the CLUT of a brightness texture (blend_mode) */

static int blend_mode(u64 *alpha, const struct tex *t) {
  GLenum s = gl.sf, d = gl.dfac;
  if (s == GL_ONE && d == GL_ZERO)
    return 0;
  if (s == GL_SRC_ALPHA && d == GL_ONE_MINUS_SRC_ALPHA) {
    *alpha = mx_gs_alpha(0, 1, 0, 1, 0);
    return 1;
  }
  if ((s == GL_ONE || s == GL_DST_COLOR) && d == GL_ONE) {
    *alpha = mx_gs_alpha(0, 2, 2, 1, 0x80);
    return 1;
  }
  if (s == GL_SRC_ALPHA && d == GL_ONE) {
    *alpha = mx_gs_alpha(0, 2, 0, 1, 0);
    return 1;
  }
  if (s == GL_ONE_MINUS_SRC_ALPHA && d == GL_SRC_ALPHA) {
    *alpha = mx_gs_alpha(1, 0, 0, 0, 0);
    return 1;
  }
  if (s == GL_ZERO && d == GL_ONE_MINUS_SRC_ALPHA) {
    *alpha = mx_gs_alpha(2, 1, 0, 1, 0);
    return 1;
  }
  if (s == GL_ONE && d == GL_ONE_MINUS_SRC_ALPHA) {
    *alpha = mx_gs_alpha(0, 1, 0, 1, 0);
    return 1;
  }
  if (s == GL_ZERO && d == GL_ONE) {
    *alpha = mx_gs_alpha(1, 1, 2, 1, 0); /* Cd */
    return 1;
  }
  if (s == GL_ONE && d == GL_ONE_MINUS_SRC_COLOR) {
    *alpha = mx_gs_alpha(0, 2, 2, 1, 0x80); /* about additive */
    return 1;
  }
  /* DST_COLOR ZERO, ZERO SRC_COLOR: the product with the destination,
     Cd * As, of a brightness texture (mxgl_TexBrightness), up to 2x */
  if (((s == GL_DST_COLOR && d == GL_ZERO) || (s == GL_ZERO && d == GL_SRC_COLOR)) && t &&
      t->psm == MX_GS_PSM_T8) {
    *alpha = mx_gs_alpha(1, 2, 0, 2, 0);
    clut_pick = gl.cshift ? CLUT_2X : CLUT_1X;
    return 1;
  }
  if (s == GL_ZERO && d == GL_ONE_MINUS_SRC_COLOR && t && t->psm == MX_GS_PSM_T8) {
    *alpha = mx_gs_alpha(1, 2, 0, 2, 0);
    clut_pick = CLUT_INV;
    return 1;
  }
  /* other products with the destination colour */
  return -1;
}

/* Brings the GS registers up to the GL state; returns PRIM's ABE and TME
   in *prim and 0, or -1 if the draw cannot be done. */
static int set_state(u32 *prim, float *cscale, s32 lod_k) {
  struct tex *t = NULL;
  if (gl.tex2d && gl.bound && gl.bound < MAX_TEX && texs[gl.bound].used && texs[gl.bound].data)
    t = &texs[gl.bound];

  u64 alpha = 0;
  clut_pick = CLUT_1X;
  int abe = gl.blend ? blend_mode(&alpha, t) : 0;
  if (abe < 0)
    return -1;

  u32 reg[10];
  u64 val[10];
  int n = 0;
  if (t) {
    if (t->gen != vram_gen) {
      tex_upload(t);
      if (t->gen != vram_gen)
        return -1;
    }
    u32 tfx = gl.texenv == GL_REPLACE || gl.texenv == GL_DECAL ? 1 : 0;
    if (t->psm == MX_GS_PSM_T8)
      t->mt.cbp = clut_cbp + 4 * (u32)clut_pick;
    u64 tex0 = mx_tex_tex0(&t->mt, 1, tfx);
    /* MMIN: 0/1 nearest/linear; with levels 2/3 (nearest texel) or 4/5
       (linear), the nearest level or both (trilinear): both unless the
       engine asked for the nearest (r_textureMode *_MIPMAP_NEAREST) */
    u32 mmin = (u32)t->lin_min;
    if (t->mt.levels > 1)
      mmin = (t->lin_min ? 4 : 2) + (t->mip != 1);
    u64 tex1 = mx_tex_tex1(&t->mt, (u32)t->lin_mag, mmin, 0, t->mt.levels > 1 ? lod_k : 0);
    if (t->mt.levels > 1) {
      u64 m1 = mx_tex_miptbp1(&t->mt), m2 = mx_tex_miptbp2(&t->mt);
      if (!gs.valid || tex0 != gs.tex0 || m1 != gs.mip1)
        reg[n] = MX_GS_REG_MIPTBP1_1, val[n++] = gs.mip1 = m1;
      if (!gs.valid || tex0 != gs.tex0 || m2 != gs.mip2)
        reg[n] = MX_GS_REG_MIPTBP2_1, val[n++] = gs.mip2 = m2;
    }
    u64 clamp = mx_gs_clamp((u32)t->clamp_s, (u32)t->clamp_t, 0, 0, 0, 0);
    if (!gs.valid || tex0 != gs.tex0)
      reg[n] = MX_GS_REG_TEX0_1, val[n++] = gs.tex0 = tex0;
    if (!gs.valid || tex1 != gs.tex1)
      reg[n] = MX_GS_REG_TEX1_1, val[n++] = gs.tex1 = tex1;
    if (!gs.valid || clamp != gs.clamp)
      reg[n] = MX_GS_REG_CLAMP_1, val[n++] = gs.clamp = clamp;
  }
  if (abe && (!gs.valid || alpha != gs.alpha))
    reg[n] = MX_GS_REG_ALPHA_1, val[n++] = gs.alpha = alpha;

  u32 ate = 0, atst = 1, aref = 0;
  if (gl.atest) {
    ate = 1;
    aref = (u32)(gl.aref * 128.0f + 0.5f);
    switch (gl.afunc) {
    case GL_GREATER: atst = 6; break;
    case GL_GEQUAL: atst = 5; break;
    case GL_LESS: atst = 2; break;
    case GL_LEQUAL: atst = 3; break;
    case GL_EQUAL: atst = 4; break;
    case GL_NOTEQUAL: atst = 7; break;
    case GL_NEVER: atst = 0; break;
    default: ate = 0; break;
    }
  }
  u32 ztst = 1;
  if (gl.dtest)
    ztst = gl.dfunc == GL_ALWAYS ? 1 : gl.dfunc == GL_LESS || gl.dfunc == GL_GREATER ? 3 : 2;
  u64 test = mx_gs_test((int)ate, atst, aref, 0, 0, 0, 1, ztst);
  if (!gs.valid || test != gs.test)
    reg[n] = MX_GS_REG_TEST_1, val[n++] = gs.test = test;

  int zmsk = !(gl.dtest && gl.dmask);
#ifndef MXGL_HIRES
  const struct mx_rt *target = &rt[buf];
  u64 zbuf = mx_gs_zbuf(target->zbp, target->zpsm, (u32)zmsk);
  if (!gs.valid || zbuf != gs.zbuf)
    reg[n] = MX_GS_REG_ZBUF_1, val[n++] = gs.zbuf = zbuf;
#endif

  int x0 = 0, x1 = FB_W - 1, y0 = 0, y1 = FB_H - 1;
  if (gl.scissor) {
    x0 = gl.sc[0] * FB_W / GL_W, x1 = (gl.sc[0] + gl.sc[2]) * FB_W / GL_W - 1;
    y0 = (GL_H - (gl.sc[1] + gl.sc[3])) * FB_H / GL_H, y1 = (GL_H - gl.sc[1]) * FB_H / GL_H - 1;
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > FB_W - 1 ? FB_W - 1 : x1, y1 = y1 > FB_H - 1 ? FB_H - 1 : y1;
    if (x1 < x0 || y1 < y0)
      return -1;
  }
#ifndef MXGL_HIRES
  u64 sc = mx_gs_scissor((u32)x0, (u32)x1, (u32)y0, (u32)y1);
  if (!gs.valid || sc != gs.scissor)
    reg[n] = MX_GS_REG_SCISSOR_1, val[n++] = gs.scissor = sc;
#endif

  write_regs(reg, val, n);
  gs.valid = 1;

#ifdef MXGL_HIRES
  /* context 2: no Z writes */
  *prim = (u32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, t != NULL, 0, abe, 0, 0, zmsk, 0);
#else
  (void)zmsk;
  *prim = (u32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, t != NULL, 0, abe, 0, 0, 0, 0);
#endif
  if (t) {
    /* 0x80 is 1.0 when modulated: up to 2x with a colour shift */
    cscale[0] = cscale[1] = cscale[2] = gl.cshift ? 1.0f : 0.5f;
  } else {
    cscale[0] = cscale[1] = cscale[2] = 1.0f;
  }
  cscale[3] = 128.0f / 255.0f;
  return 0;
}

/* GS coordinates from the GL matrices, viewport and depth range */
static void make_mvp(float *m) {
  float vpm[16];
  memset(vpm, 0, 64);
  float vx = (float)gl.vp[0], vy = (float)gl.vp[1], vw = (float)gl.vp[2], vh = (float)gl.vp[3];
  float r = (float)FB_H / (float)GL_H, rx = (float)FB_W / (float)GL_W;
  vpm[0] = vw / 2 * rx;
  vpm[12] = 2048.0f - FB_W / 2 + (vx + vw / 2) * rx;
  vpm[5] = -vh / 2 * r;
  vpm[13] = 2048.0f - FB_H / 2 + (GL_H - vy - vh / 2) * r;
  vpm[10] = -ZMAX * (gl.df - gl.dn) / 2;
  vpm[14] = ZMAX * (1 - gl.dn - (gl.df - gl.dn) / 2);
  vpm[15] = 1;
  float pm[16];
  mat_mul(pm, gl.pr[gl.pr_top], gl.mv[gl.mv_top]);
  mat_mul(m, vpm, pm);
}

/* ---- clipping ------------------------------------------------------------- */

struct cv {
  float p[4];  /* GS coordinates times w */
  float x[3];  /* the vertex */
  float st[2];
  float c[4];
};

static float zn, zf; /* the planes' Z (times w) */

static float plane_d(const float *p, int k) {
  switch (k) {
  case 0: return p[0] - GB_LO * p[3];
  case 1: return GB_HI * p[3] - p[0];
  case 2: return p[1] - GB_LO * p[3];
  case 3: return GB_HI * p[3] - p[1];
  case 4: return zn * p[3] - p[2]; /* near: Z <= zn */
  default: return p[2] - zf * p[3]; /* far: Z >= zf */
  }
}

static u32 outcode(const float *p) {
  u32 o = 0;
  for (int k = 0; k < 6; k++)
    if (plane_d(p, k) < 0)
      o |= 1u << k;
  return o;
}

/* ---- output ----------------------------------------------------------------- */

/* the triangles of a draw are gathered here and copied into the arena in
   chunks of whole triangles (flush_out) */
#define OUT_MAX (VU_MXGLV_MAX_N * 24)
static float o_pos[OUT_MAX][3];
static float o_st[OUT_MAX][2];
static u8 o_col[OUT_MAX][4];
static u32 o_n;
static u32 o_prim;
static float o_uni[VU_MXGLV_UNI_QW][4] MX_ALIGNED(16);

static void *arena_take(u32 bytes) {
  bytes = (bytes + 63) & ~63u;
  if (arena_pos + bytes > ARENA_BYTES)
    return NULL;
  void *p = arena[frame_par] + arena_pos;
  arena_pos += bytes;
  return p;
}

static void flush_out(void) {
  if (!o_n)
    return;
  u32 pad = VU_MXGLV_ALIGN;
  float(*pos)[3] = arena_take((o_n + pad) * 12);
  float(*st)[2] = arena_take((o_n + pad) * 8);
  u8(*col)[4] = arena_take((o_n + pad) * 4);
  if (!pos || !st || !col) {
    dropped_draws++;
    o_n = 0;
    return;
  }
  u32 tf0 = mx_count();
  memcpy(pos, o_pos, o_n * 12);
  memcpy(st, o_st, o_n * 8);
  memcpy(col, o_col, o_n * 4);
  mx_dcache_writeback(pos, (o_n + pad) * 12);
  mx_dcache_writeback(st, (o_n + pad) * 8);
  mx_dcache_writeback(col, (o_n + pad) * 4);
  const void *streams[3] = {pos, st, col};
  mx_frame_shader_draw(&shader, o_prim, o_uni, streams, o_n);
  st_cur.flush_cycles += mx_count() - tf0;
  st_cur.draws++;
  st_cur.tris += o_n / 3;
  o_n = 0;
}

static void out_vertex(const struct cv *v) {
  memcpy(o_pos[o_n], v->x, 12);
  o_st[o_n][0] = v->st[0], o_st[o_n][1] = v->st[1];
  for (int i = 0; i < 4; i++) {
    float c = v->c[i];
    o_col[o_n][i] = (u8)(c < 0 ? 0 : c > 255 ? 255 : c);
  }
  o_n++;
}

static void lerp(struct cv *r, const struct cv *a, const struct cv *b, float t) {
  for (int i = 0; i < 4; i++)
    r->p[i] = a->p[i] + (b->p[i] - a->p[i]) * t;
  for (int i = 0; i < 3; i++)
    r->x[i] = a->x[i] + (b->x[i] - a->x[i]) * t;
  for (int i = 0; i < 2; i++)
    r->st[i] = a->st[i] + (b->st[i] - a->st[i]) * t;
  for (int i = 0; i < 4; i++)
    r->c[i] = a->c[i] + (b->c[i] - a->c[i]) * t;
}

/* 1: the triangle is culled (window-space winding) */
static int culled(const float *a, const float *b, const float *c) {
  if (!gl.cull)
    return 0;
  /* the area's sign from (x, y, w) without dividing (all w > 0): the
     determinant of the three; GS y points down, so counter-clockwise in GL
     is negative here */
  float det = a[0] * (b[1] * c[3] - b[3] * c[1]) - a[1] * (b[0] * c[3] - b[3] * c[0]) +
              a[3] * (b[0] * c[1] - b[1] * c[0]);
  int front = det < 0;
  if (gl.cullface == GL_FRONT_AND_BACK)
    return 1;
  return gl.cullface == GL_FRONT ? front : !front;
}

static void clip_tri(const struct cv *a, const struct cv *b, const struct cv *c, u32 planes) {
  struct cv buf0[12], buf1[12];
  struct cv *in = buf0, *out = buf1;
  int n = 3;
  in[0] = *a, in[1] = *b, in[2] = *c;
  for (int k = 0; k < 6 && n >= 3; k++) {
    if (!(planes & (1u << k)))
      continue;
    int m = 0;
    for (int i = 0; i < n; i++) {
      const struct cv *p = &in[i], *q = &in[(i + 1) % n];
      float dp = plane_d(p->p, k), dq = plane_d(q->p, k);
      if (dp >= 0)
        out[m++] = *p;
      if ((dp >= 0) != (dq >= 0) && m < 12)
        lerp(&out[m++], p, q, dp / (dp - dq));
    }
    struct cv *t = in;
    in = out, out = t, n = m;
  }
  if (n < 3)
    return;
  if (culled(in[0].p, in[1].p, in[2].p))
    return;
  if (o_n + 3 * (u32)(n - 2) > OUT_MAX)
    flush_out();
  for (int i = 1; i + 1 < n; i++) {
    out_vertex(&in[0]);
    out_vertex(&in[i]);
    out_vertex(&in[i + 1]);
  }
}

/* ---- the draw -------------------------------------------------------------- */

static float (*vp_cache)[4];
static u8 *vo_cache;
static u32 v_cache_n;
static float (*vp_ram)[4];
static u8 *vo_ram;
/* the first 8 KiB of the scratchpad (the frame stream has the second):
   the projected vertices and outcodes of a draw that fits */
#define SPR_VERTS 480
#define SPR_VP ((float(*)[4])0x70000000)
#define SPR_VO ((u8 *)(0x70000000 + SPR_VERTS * 16))

static void fetch(struct cv *v, int i, const float *mvp) {
  const float *x = (const float *)((const u8 *)gl.vptr + i * gl.vstride);
  v->x[0] = x[0], v->x[1] = x[1], v->x[2] = gl.vsize > 2 ? x[2] : 0;
  memcpy(v->p, vp_cache[i], 16);
  if (gl.ta && gl.tptr) {
    const float *t = (const float *)((const u8 *)gl.tptr + i * gl.tstride);
    v->st[0] = t[0], v->st[1] = t[1];
  } else {
    v->st[0] = v->st[1] = 0;
  }
  if (gl.ca && gl.cptr) {
    const u8 *c = gl.cptr + i * gl.cstride;
    for (int k = 0; k < 4; k++)
      v->c[k] = c[k];
  } else {
    for (int k = 0; k < 4; k++)
      v->c[k] = gl.color[k] * 255.0f;
  }
  (void)mvp;
}

/* log2 of x > 0 from its bits: the exponent, and a parabola through the
   mantissa (error below 0.005: K is in 1/16) */
static inline float fast_log2(float x) {
  union {
    float f;
    u32 u;
  } v = {x};
  float e = (float)((s32)(v.u >> 23 & 255) - 127);
  v.u = (v.u & 0x007FFFFFu) | 0x3F800000u; /* the mantissa in [1, 2) */
  float m = v.f - 1.0f;
  return e + m * (1.3465557f - 0.34655570f * m);
}

/* log2 of pixels per unit at w = 1 of the current projection and viewport
   (the mean of x and y) */
static float proj_lpp(void) {
  const float *pr = gl.pr[gl.pr_top];
  float a = pr[0] * (float)gl.vp[2] * 0.5f * FB_W / GL_W * pr[5] * (float)gl.vp[3] * 0.5f * FB_H / GL_H;
  return 0.5f * fast_log2(a < 0 ? -a : a);
}

/* K of TEX1 for a draw: the GS picks LOD = log2(1/Q) + K = log2(w) + K per
   pixel, so K = log2(texels a pixel at w = 1) = log2(texels per unit of the
   surface) - log2(pixels per unit at w = 1). Texels per unit from the
   texture area over the surface area of the first 16 triangles, in the
   vertices' own space (the modelview of the world and the models keeps
   sizes). Not from the triangles on the screen: a surface seen at a slant
   (a floor) would get its smallest level everywhere. */
static s32 lod_k(const u32 *idx, int nidx, int nverts) {
  struct tex *t = gl.tex2d && gl.bound && gl.bound < MAX_TEX ? &texs[gl.bound] : NULL;
  if (!t || !t->used || t->levels < 2 || !gl.ta || !gl.tptr)
    return 0;
  float at = 0, ao = 0;
  int n = 0;
  for (int i = 0; i + 2 < nidx && n < 16; i += 3) {
    u32 i0 = idx[i], i1 = idx[i + 1], i2 = idx[i + 2];
    if ((int)i0 >= nverts || (int)i1 >= nverts || (int)i2 >= nverts)
      continue;
    const float *a = (const float *)((const u8 *)gl.vptr + i0 * gl.vstride);
    const float *b = (const float *)((const u8 *)gl.vptr + i1 * gl.vstride);
    const float *c = (const float *)((const u8 *)gl.vptr + i2 * gl.vstride);
    float az = gl.vsize > 2 ? a[2] : 0, bz = gl.vsize > 2 ? b[2] : 0, cz = gl.vsize > 2 ? c[2] : 0;
    float ux = b[0] - a[0], uy = b[1] - a[1], uz = bz - az;
    float vx = c[0] - a[0], vy = c[1] - a[1], vz = cz - az;
    float mx = uy * vz - uz * vy, my = uz * vx - ux * vz, mz = ux * vy - uy * vx;
    ao += sqrtf(mx * mx + my * my + mz * mz);
    const float *ta = (const float *)((const u8 *)gl.tptr + i0 * gl.tstride);
    const float *tb = (const float *)((const u8 *)gl.tptr + i1 * gl.tstride);
    const float *tc = (const float *)((const u8 *)gl.tptr + i2 * gl.tstride);
    float x = (tb[0] - ta[0]) * (tc[1] - ta[1]) - (tc[0] - ta[0]) * (tb[1] - ta[1]);
    at += x < 0 ? -x : x;
    n++;
  }
  if (ao < 1e-6f || at < 1e-12f)
    return 0;
  float k = (0.5f * fast_log2(at * (float)t->w * (float)t->h / ao) - proj_lpp()) * 16.0f;
  s32 ki = (s32)(k + (k < 0 ? -0.5f : 0.5f));
  return ki < -2048 ? -2048 : ki > 2047 ? 2047 : ki;
}

static void draw_tris_(const u32 *idx, int nidx, int nverts);
static void draw_tris(const u32 *idx, int nidx, int nverts) {
  u32 t0 = mx_count();
  draw_tris_(idx, nidx, nverts);
  st_cur.gl_cycles += mx_count() - t0;
}

static int dl_compiling;
static void dl_record(const u32 *idx, int nidx, int nverts);

static void draw_tris_(const u32 *idx, int nidx, int nverts) {
  if (nidx < 3 || !gl.va || !gl.vptr)
    return;
  if (dl_compiling) {
    dl_record(idx, nidx, nverts);
    return;
  }
  frame_start();
  float m[16];
  make_mvp(m);
  memcpy(o_uni[VU_MXGLV_MVP], m, 64);
  zn = ZMAX * (1 - gl.dn);
  zf = ZMAX * (1 - gl.df);

  /* the vertices in GS coordinates, and their outcodes */
  if (nverts <= SPR_VERTS) {
    vp_cache = SPR_VP, vo_cache = SPR_VO;
  } else {
    if ((u32)nverts > v_cache_n) {
      free(vp_ram);
      free(vo_ram);
      v_cache_n = (u32)nverts + 256;
      vp_ram = memalign(16, v_cache_n * 16);
      vo_ram = malloc(v_cache_n);
    }
    vp_cache = vp_ram, vo_cache = vo_ram;
  }
  u32 tx0 = mx_count();
  st_cur.verts += (u32)nverts;
  v4f mc[4];
  memcpy(mc, m, 64);
  for (int i = 0; i < nverts; i++) {
    const float *x = (const float *)((const u8 *)gl.vptr + i * gl.vstride);
    float z = gl.vsize > 2 ? x[2] : 0;
    v4f p = xform(mc, x[0], x[1], z);
    *(v4f *)vp_cache[i] = p;
    vo_cache[i] = (u8)outcode(vp_cache[i]);
  }

  st_cur.xf_cycles += mx_count() - tx0;
  if (set_state(&o_prim, o_uni[VU_MXGLV_CSCALE], lod_k(idx, nidx, nverts)) < 0)
    return;
  u32 tt0 = mx_count();
  o_n = 0;
  for (int i = 0; i + 2 < nidx; i += 3) {
    u32 i0 = idx[i], i1 = idx[i + 1], i2 = idx[i + 2];
    if ((int)i0 >= nverts || (int)i1 >= nverts || (int)i2 >= nverts)
      continue;
    u32 o0 = vo_cache[i0], o1 = vo_cache[i1], o2 = vo_cache[i2];
    if (o0 & o1 & o2)
      continue;
    struct cv v0, v1, v2;
    if (!(o0 | o1 | o2)) {
      if (culled(vp_cache[i0], vp_cache[i1], vp_cache[i2]))
        continue;
      fetch(&v0, (int)i0, m), fetch(&v1, (int)i1, m), fetch(&v2, (int)i2, m);
      if (o_n + 3 > OUT_MAX)
        flush_out();
      out_vertex(&v0), out_vertex(&v1), out_vertex(&v2);
    } else {
      fetch(&v0, (int)i0, m), fetch(&v1, (int)i1, m), fetch(&v2, (int)i2, m);
      clip_tri(&v0, &v1, &v2, o0 | o1 | o2);
    }
  }
  st_cur.tri_cycles += mx_count() - tt0;
  flush_out();
}

/* ---- GL entry points ------------------------------------------------------ */

static u32 *idx_tmp;
static int idx_tmp_n;
static u32 *idx_room(int n) {
  if (n > idx_tmp_n) {
    free(idx_tmp);
    idx_tmp_n = n + 1024;
    idx_tmp = malloc((size_t)idx_tmp_n * 4);
  }
  return idx_tmp;
}

/* ---- display lists ------------------------------------------------------------ */

/* A list holds the draws made between mxgl_NewList and mxgl_EndList (the
   state calls in between are done as usual: the renderer keeps a copy of
   the GL state). Each draw becomes an entry: its triangles as the three
   vertex streams, its GL state, its bounds and a mesh chain of its VU1
   batches (mx_shader_mesh), all in the pool, made once. A call
   (mxgl_CallList) takes the matrices current then: an entry whose bounds
   are inside the near and far planes and the guard band is a context and a
   CALL of its chain in the frame stream; one across them goes the way of
   any draw (clipped on the EE); one outside them is left out. An entry whose
   triangles all lie in one plane, facing one way, is culled as a whole. */

struct dl_state {
  int tex2d, blend, atest, dtest, dmask, cull;
  GLuint bound;
  GLenum texenv, sf, dfac, afunc, dfunc, cullface;
  float aref;
  int cshift;
};

struct dl_entry {
  struct dl_entry *next;
  struct dl_state st;
  float (*pos)[3];
  float (*st2)[2];
  u8 (*col)[4];
  u32 n;            /* vertices (3 a triangle) */
  struct mx_chain ch;
  float bmin[3], bmax[3];
  v4f center, half; /* the bounds as centre (w 1) and half sizes */
  int planar;       /* the first triangle stands for all (culling) */
  float log2_density; /* log2 of texels per unit (both ways), for K; -1e9: none */
};

#define DL_MAX 4096
#define DL_POOL_BYTES (2 * 1024 * 1024)
static struct {
  struct dl_entry *first;
  u32 used;
} dlists[DL_MAX];
static u8 *dl_pool;
static u32 dl_pool_pos;
static int dl_cur, dl_failed, dl_dirty;
static struct dl_entry **dl_tail;

static void *dl_take(u32 bytes) {
  bytes = (bytes + 63) & ~63u;
  if (dl_pool_pos + bytes > DL_POOL_BYTES)
    return NULL;
  void *p = dl_pool + dl_pool_pos;
  dl_pool_pos += bytes;
  return p;
}

static void dl_get_state(struct dl_state *s) {
  s->tex2d = gl.tex2d, s->blend = gl.blend, s->atest = gl.atest, s->dtest = gl.dtest;
  s->dmask = gl.dmask, s->cull = gl.cull, s->bound = gl.bound, s->texenv = gl.texenv;
  s->sf = gl.sf, s->dfac = gl.dfac, s->afunc = gl.afunc, s->dfunc = gl.dfunc;
  s->cullface = gl.cullface, s->aref = gl.aref, s->cshift = gl.cshift;
}

static void dl_set_state(const struct dl_state *s) {
  gl.tex2d = s->tex2d, gl.blend = s->blend, gl.atest = s->atest, gl.dtest = s->dtest;
  gl.dmask = s->dmask, gl.cull = s->cull, gl.bound = s->bound, gl.texenv = s->texenv;
  gl.sf = s->sf, gl.dfac = s->dfac, gl.afunc = s->afunc, gl.dfunc = s->dfunc;
  gl.cullface = s->cullface, gl.aref = s->aref, gl.cshift = s->cshift;
}

static void dl_record(const u32 *idx, int nidx, int nverts) {
  if (dl_failed)
    return;
  u32 n = 0;
  for (int i = 0; i + 2 < nidx; i += 3)
    if ((int)idx[i] < nverts && (int)idx[i + 1] < nverts && (int)idx[i + 2] < nverts)
      n += 3;
  if (!n)
    return;
  u32 pad = VU_MXGLV_ALIGN;
  u32 prim = MX_GS_PRIM_TRIANGLE;
  struct dl_entry *e = dl_take(sizeof *e);
  float(*pos)[3] = dl_take((n + pad) * 12);
  float(*st2)[2] = dl_take((n + pad) * 8);
  u8(*col)[4] = dl_take((n + pad) * 4);
  u32 chain_bytes = mx_shader_mesh_qw(&shader, prim, n) * 16;
  void *chain_mem = dl_take(chain_bytes);
  if (!e || !pos || !st2 || !col || !chain_mem) {
    dl_failed = 1;
    return;
  }
  memset(e, 0, sizeof *e);
  dl_get_state(&e->st);
  e->pos = pos, e->st2 = st2, e->col = col, e->n = n;
  for (int k = 0; k < 3; k++)
    e->bmin[k] = 1e30f, e->bmax[k] = -1e30f;
  u32 o = 0;
  for (int i = 0; i + 2 < nidx; i += 3) {
    if ((int)idx[i] >= nverts || (int)idx[i + 1] >= nverts || (int)idx[i + 2] >= nverts)
      continue;
    for (int j = 0; j < 3; j++, o++) {
      u32 v = idx[i + j];
      const float *x = (const float *)((const u8 *)gl.vptr + v * gl.vstride);
      pos[o][0] = x[0], pos[o][1] = x[1], pos[o][2] = gl.vsize > 2 ? x[2] : 0;
      for (int k = 0; k < 3; k++) {
        if (pos[o][k] < e->bmin[k])
          e->bmin[k] = pos[o][k];
        if (pos[o][k] > e->bmax[k])
          e->bmax[k] = pos[o][k];
      }
      if (gl.ta && gl.tptr) {
        const float *t = (const float *)((const u8 *)gl.tptr + v * gl.tstride);
        st2[o][0] = t[0], st2[o][1] = t[1];
      } else {
        st2[o][0] = st2[o][1] = 0;
      }
      if (gl.ca && gl.cptr) {
        memcpy(col[o], gl.cptr + v * gl.cstride, 4);
      } else {
        for (int k = 0; k < 4; k++) {
          float c = gl.color[k] * 255.0f;
          col[o][k] = (u8)(c < 0 ? 0 : c > 255 ? 255 : c);
        }
      }
    }
  }
  /* one plane, one facing: the triangles' normals all along the first's,
     their corners on its plane */
  float nx = 0, ny = 0, nz = 0, d = 0;
  {
    const float *a = pos[0], *b = pos[1], *c = pos[2];
    float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    e->planar = l > 1e-4f;
    if (e->planar) {
      nx /= l, ny /= l, nz /= l;
      d = nx * a[0] + ny * a[1] + nz * a[2];
    }
  }
  for (u32 t = 0; t < n && e->planar; t += 3) {
    const float *a = pos[t], *b = pos[t + 1], *c = pos[t + 2];
    float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    float mx = uy * vz - uz * vy, my = uz * vx - ux * vz, mz = ux * vy - uy * vx;
    if (mx * nx + my * ny + mz * nz < 0)
      e->planar = 0;
    for (int j = 0; j < 3; j++) {
      float dist = nx * pos[t + j][0] + ny * pos[t + j][1] + nz * pos[t + j][2] - d;
      if (dist > 0.5f || dist < -0.5f)
        e->planar = 0;
    }
  }
  for (int k = 0; k < 3; k++) {
    e->center[k] = (e->bmin[k] + e->bmax[k]) * 0.5f;
    e->half[k] = (e->bmax[k] - e->bmin[k]) * 0.5f;
  }
  e->center[3] = 1, e->half[3] = 0;
  /* texels per unit: sqrt of the texture area over the surface area */
  e->log2_density = -1e9f;
  {
    struct tex *tx = gl.tex2d && gl.bound && gl.bound < MAX_TEX ? &texs[gl.bound] : NULL;
    float at = 0, ao = 0;
    for (u32 t = 0; tx && t < n; t += 3) {
      const float *a = pos[t], *b = pos[t + 1], *c = pos[t + 2];
      float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
      float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
      float mx = uy * vz - uz * vy, my = uz * vx - ux * vz, mz = ux * vy - uy * vx;
      ao += sqrtf(mx * mx + my * my + mz * mz);
      const float *ta = st2[t], *tb = st2[t + 1], *tc = st2[t + 2];
      float x = (tb[0] - ta[0]) * (tc[1] - ta[1]) - (tc[0] - ta[0]) * (tb[1] - ta[1]);
      at += (x < 0 ? -x : x) * (float)tx->w * (float)tx->h;
    }
    if (ao > 1e-6f && at > 1e-9f)
      e->log2_density = 0.5f * fast_log2(at / ao);
  }
  mx_dcache_writeback(pos, (n + pad) * 12);
  mx_dcache_writeback(st2, (n + pad) * 8);
  mx_dcache_writeback(col, (n + pad) * 4);
  struct mx_pbuf pb;
  mx_pbuf_init(&pb, chain_mem, chain_bytes);
  const void *streams[3] = {pos, st2, col};
  if (mx_shader_mesh(&e->ch, &pb, &shader, prim, streams, n) < 0) {
    dl_failed = 1;
    return;
  }
  *dl_tail = e;
  dl_tail = &e->next;
}

/* Starts list id (1 .. DL_MAX - 1); returns 0 if it cannot. */
int mxgl_NewList(int id) {
  static int warned;
  if (!dl_pool) /* taken by the first list (a game without lists does not pay) */
    dl_pool = memalign(64, DL_POOL_BYTES);
  if (!dl_pool && !warned++)
    mx_printf("mxgl: no memory for the display lists (%u KB): drawn without\n",
              DL_POOL_BYTES / 1024);
  if (id <= 0 || id >= DL_MAX || !dl_pool || dl_compiling)
    return 0;
  dlists[id].first = NULL;
  dl_tail = &dlists[id].first;
  dl_cur = id;
  dl_failed = 0;
  dl_compiling = 1;
  return 1;
}

/* Ends it; returns 1 if all its draws are in it (else it is empty). */
int mxgl_EndList(void) {
  if (!dl_compiling)
    return 0;
  dl_compiling = 0;
  dlists[dl_cur].used = !dl_failed;
  if (dl_failed)
    dlists[dl_cur].first = NULL;
  return !dl_failed;
}

/* Every list goes; the pool is empty again (after the frames in flight). */
void mxgl_DeleteAllLists(void) {
  u32 t0 = mx_count();
  while (mx_frame_done() < mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
    ;
  memset(dlists, 0, sizeof dlists);
  dl_pool_pos = 0;
  dl_dirty = 0;
}

/* K: log2(texels a pixel at w = 1) = log2(texels per unit) - log2(pixels
   per unit at w = 1, from the projection: lpp) */
static s32 dl_lod_k(const struct dl_entry *e, float lpp) {
  struct tex *t = gl.tex2d && gl.bound && gl.bound < MAX_TEX ? &texs[gl.bound] : NULL;
  if (!t || !t->used || t->levels < 2 || e->log2_density < -1e8f)
    return 0;
  float k = (e->log2_density - lpp) * 16.0f;
  s32 ki = (s32)(k + (k < 0 ? -0.5f : 0.5f));
  return ki < -2048 ? -2048 : ki > 2047 ? 2047 : ki;
}

static u32 *idx_room(int n);

void mxgl_CallList(int id) {
  if (id <= 0 || id >= DL_MAX || !dlists[id].used)
    return;
  u32 t0 = mx_count();
  frame_start();
  /* the matrix of the calls, made again when the matrices, the viewport or
     the depth range change; lpp: log2 of pixels per unit at w = 1 */
  static float m[16], lpp, key[16 + 16 + 6];
  /* the six planes (plane_d) as rows: plane k of p = M v is pl[k] . v */
  static v4f pl[6], pl_abs[6];
  float nk[16 + 16 + 6];
  memcpy(nk, gl.mv[gl.mv_top], 64);
  memcpy(nk + 16, gl.pr[gl.pr_top], 64);
  nk[32] = (float)gl.vp[0], nk[33] = (float)gl.vp[1], nk[34] = (float)gl.vp[2];
  nk[35] = (float)gl.vp[3], nk[36] = gl.dn, nk[37] = gl.df;
  if (memcmp(nk, key, sizeof key)) {
    memcpy(key, nk, sizeof key);
    make_mvp(m);
    const float *pr = gl.pr[gl.pr_top];
    float sx = pr[0] * (float)gl.vp[2] * 0.5f * FB_W / GL_W, sy = pr[5] * (float)gl.vp[3] * 0.5f * FB_H / GL_H;
    float a = sx * sy;
    lpp = 0.5f * fast_log2(a < 0 ? -a : a);
    zn = ZMAX * (1 - gl.dn);
    zf = ZMAX * (1 - gl.df);
    for (int c = 0; c < 4; c++) {
      const float *col = m + 4 * c;
      pl[0][c] = col[0] - GB_LO * col[3];
      pl[1][c] = GB_HI * col[3] - col[0];
      pl[2][c] = col[1] - GB_LO * col[3];
      pl[3][c] = GB_HI * col[3] - col[1];
      pl[4][c] = zn * col[3] - col[2];
      pl[5][c] = col[2] - zf * col[3];
    }
    for (int k = 0; k < 6; k++)
      for (int c = 0; c < 4; c++)
        pl_abs[k][c] = c < 3 ? (pl[k][c] < 0 ? -pl[k][c] : pl[k][c]) : 0;
  }
  zn = ZMAX * (1 - gl.dn);
  zf = ZMAX * (1 - gl.df);
  struct dl_state keep;
  dl_get_state(&keep);
  for (struct dl_entry *e = dlists[id].first; e; e = e->next) {
    u32 c0 = mx_count();
    /* the bounds against each plane: the centre's distance, and how far
       the box reaches along the plane's normal */
    u32 oand = 0, oor = 0;
    for (int k = 0; k < 6; k++) {
      v4f dc = pl[k] * e->center, rr = pl_abs[k] * e->half;
      float d = dc[0] + dc[1] + dc[2] + dc[3], r = rr[0] + rr[1] + rr[2];
      if (d + r < 0)
        oand = 1;
      if (d - r < 0)
        oor = 1;
    }
    if (oand)
      continue;
    dl_set_state(&e->st);
    if (oor) {
      /* across a plane: as any draw */
      const float *vp = gl.vptr, *tp = gl.tptr;
      const u8 *cp = gl.cptr;
      int vs = gl.vsize, vst = gl.vstride, tst = gl.tstride, cst = gl.cstride;
      int va = gl.va, ta = gl.ta, ca = gl.ca;
      gl.vptr = &e->pos[0][0], gl.vsize = 3, gl.vstride = 12, gl.va = 1;
      gl.tptr = &e->st2[0][0], gl.tstride = 8, gl.ta = 1;
      gl.cptr = &e->col[0][0], gl.cstride = 4, gl.ca = 1;
      u32 *ix = idx_room((int)e->n);
      for (u32 i = 0; i < e->n; i++)
        ix[i] = i;
      draw_tris_(ix, (int)e->n, (int)e->n);
      gl.vptr = vp, gl.tptr = tp, gl.cptr = cp;
      gl.vsize = vs, gl.vstride = vst, gl.tstride = tst, gl.cstride = cst;
      gl.va = va, gl.ta = ta, gl.ca = ca;
      continue;
    }
    if (e->planar && gl.cull) {
      v4f mc[4];
      memcpy(mc, m, 64);
      v4f p0 = xform(mc, e->pos[0][0], e->pos[0][1], e->pos[0][2]);
      v4f p1 = xform(mc, e->pos[1][0], e->pos[1][1], e->pos[1][2]);
      v4f p2 = xform(mc, e->pos[2][0], e->pos[2][1], e->pos[2][2]);
      if (culled((float *)&p0, (float *)&p1, (float *)&p2))
        continue;
    }
    static u32 prim;
    static float uni[VU_MXGLV_UNI_QW][4] MX_ALIGNED(16);
    static struct dl_state last_st;
    static s32 last_k;
    static u32 last_serial, last_gen;
    u32 c1 = mx_count();
    st_cur.c_bounds += c1 - c0;
    s32 k = dl_lod_k(e, lpp);
    /* the same state as the call before, nothing written since: the
       registers are right */
    if (!(gs_serial == last_serial && vram_gen == last_gen && k == last_k &&
          !memcmp(&last_st, &e->st, sizeof last_st))) {
      if (set_state(&prim, uni[VU_MXGLV_CSCALE], k) < 0)
        continue;
      last_st = e->st, last_k = k, last_gen = vram_gen, last_serial = gs_serial;
    }
    memcpy(uni[VU_MXGLV_MVP], m, 64);
    u32 c2 = mx_count();
    st_cur.c_state += c2 - c1;
    mx_frame_shader_call(&shader, prim, uni, &e->ch);
    st_cur.c_call += mx_count() - c2;
    st_cur.draws++;
    st_cur.lists++;
    st_cur.tris += e->n / 3;
  }
  dl_set_state(&keep);
  st_cur.gl_cycles += mx_count() - t0;
}

void APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
  if (mode != GL_TRIANGLES)
    return;
  u32 *ix = idx_room(count);
  int maxv = 0;
  for (int i = 0; i < count; i++) {
    u32 v = type == GL_UNSIGNED_SHORT ? ((const u16 *)indices)[i]
            : type == GL_UNSIGNED_BYTE ? ((const u8 *)indices)[i]
                                      : ((const u32 *)indices)[i];
    ix[i] = v;
    if ((int)v > maxv)
      maxv = (int)v;
  }
  draw_tris(ix, count, maxv + 1);
}

void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  (void)mode, (void)first, (void)count;
}

/* immediate mode */
#define IMM_MAX 4096
static struct {
  GLenum mode;
  int n;
  float xyz[IMM_MAX][4];
  float st[IMM_MAX][2];
  u8 col[IMM_MAX][4];
  float cur_st[2];
} imm;

void APIENTRY glBegin(GLenum mode) {
  imm.mode = mode;
  imm.n = 0;
}

static void imm_vertex(float x, float y, float z) {
  if (imm.n >= IMM_MAX)
    return;
  int i = imm.n++;
  imm.xyz[i][0] = x, imm.xyz[i][1] = y, imm.xyz[i][2] = z, imm.xyz[i][3] = 1;
  imm.st[i][0] = imm.cur_st[0], imm.st[i][1] = imm.cur_st[1];
  for (int k = 0; k < 4; k++) {
    float c = gl.color[k] * 255.0f;
    imm.col[i][k] = (u8)(c < 0 ? 0 : c > 255 ? 255 : c);
  }
}

void APIENTRY glEnd(void) {
  int n = imm.n;
  if (n < 3)
    return;
  u32 *ix = idx_room(n * 3);
  int k = 0;
  switch (imm.mode) {
  case GL_TRIANGLES:
    for (int i = 0; i + 2 < n; i += 3)
      ix[k++] = i, ix[k++] = i + 1, ix[k++] = i + 2;
    break;
  case GL_TRIANGLE_STRIP:
  case GL_QUAD_STRIP:
    for (int i = 0; i + 2 < n; i++) {
      if (i & 1)
        ix[k++] = i + 1, ix[k++] = i, ix[k++] = i + 2;
      else
        ix[k++] = i, ix[k++] = i + 1, ix[k++] = i + 2;
    }
    break;
  case GL_TRIANGLE_FAN:
  case GL_POLYGON:
    for (int i = 1; i + 1 < n; i++)
      ix[k++] = 0, ix[k++] = i, ix[k++] = i + 1;
    break;
  case GL_QUADS:
    for (int i = 0; i + 3 < n; i += 4) {
      ix[k++] = i, ix[k++] = i + 1, ix[k++] = i + 2;
      ix[k++] = i, ix[k++] = i + 2, ix[k++] = i + 3;
    }
    break;
  default:
    return;
  }
  /* draw from the immediate arrays */
  const float *vp = gl.vptr, *tp = gl.tptr;
  const u8 *cp = gl.cptr;
  int vs = gl.vsize, vst = gl.vstride, tst = gl.tstride, cst = gl.cstride;
  int va = gl.va, ta = gl.ta, ca = gl.ca;
  gl.vptr = &imm.xyz[0][0], gl.vsize = 3, gl.vstride = 16, gl.va = 1;
  gl.tptr = &imm.st[0][0], gl.tstride = 8, gl.ta = 1;
  gl.cptr = &imm.col[0][0], gl.cstride = 4, gl.ca = 1;
  draw_tris(ix, k, n);
  gl.vptr = vp, gl.tptr = tp, gl.cptr = cp;
  gl.vsize = vs, gl.vstride = vst, gl.tstride = tst, gl.cstride = cst;
  gl.va = va, gl.ta = ta, gl.ca = ca;
}

void APIENTRY glVertex3f(GLfloat x, GLfloat y, GLfloat z) { imm_vertex(x, y, z); }
void APIENTRY glVertex3fv(const GLfloat *v) { imm_vertex(v[0], v[1], v[2]); }
void APIENTRY glVertex2f(GLfloat x, GLfloat y) { imm_vertex(x, y, 0); }
void APIENTRY glTexCoord2f(GLfloat s, GLfloat t) { imm.cur_st[0] = s, imm.cur_st[1] = t; }
void APIENTRY glTexCoord2fv(const GLfloat *v) { imm.cur_st[0] = v[0], imm.cur_st[1] = v[1]; }
void APIENTRY glVertex2fv(const GLfloat *v) { imm_vertex(v[0], v[1], 0); }
void APIENTRY glVertex2i(GLint x, GLint y) { imm_vertex((float)x, (float)y, 0); }
void APIENTRY glVertex3i(GLint x, GLint y, GLint z) { imm_vertex((float)x, (float)y, (float)z); }
void APIENTRY glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  gl.color[0] = r, gl.color[1] = g, gl.color[2] = b, gl.color[3] = a;
}
void APIENTRY glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(r, g, b, 1); }
void APIENTRY glColor4fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], v[3]); }
void APIENTRY glColor3fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], 1); }
void APIENTRY glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
  glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}
void APIENTRY glColor3ub(GLubyte r, GLubyte g, GLubyte b) { glColor4ub(r, g, b, 255); }
void APIENTRY glColor3ubv(const GLubyte *v) { glColor4ub(v[0], v[1], v[2], 255); }
void APIENTRY glColor4ubv(const GLubyte *v) {
  glColor4f(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f);
}
void APIENTRY glArrayElement(GLint i) {
  if (!gl.vptr)
    return;
  const float *x = (const float *)((const u8 *)gl.vptr + i * gl.vstride);
  if (gl.ta && gl.tptr) {
    const float *t = (const float *)((const u8 *)gl.tptr + i * gl.tstride);
    imm.cur_st[0] = t[0], imm.cur_st[1] = t[1];
  }
  if (gl.ca && gl.cptr) {
    const u8 *c = gl.cptr + i * gl.cstride;
    glColor4f(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f);
  }
  imm_vertex(x[0], x[1], gl.vsize > 2 ? x[2] : 0);
}

static void set_cap(GLenum cap, int on) {
  switch (cap) {
  case GL_TEXTURE_2D:
    if (gl.cunit == 0)
      gl.tex2d = on;
    break;
  case GL_BLEND: gl.blend = on; break;
  case GL_ALPHA_TEST: gl.atest = on; break;
  case GL_DEPTH_TEST: gl.dtest = on; break;
  case GL_CULL_FACE: gl.cull = on; break;
  case GL_SCISSOR_TEST: gl.scissor = on; break;
  default: break;
  }
}
void APIENTRY glEnable(GLenum cap) { set_cap(cap, 1); }
void APIENTRY glDisable(GLenum cap) { set_cap(cap, 0); }

static void set_client(GLenum cap, int on) {
  if (cap == GL_VERTEX_ARRAY)
    gl.va = on;
  else if (cap == GL_TEXTURE_COORD_ARRAY)
    gl.ta = on;
  else if (cap == GL_COLOR_ARRAY)
    gl.ca = on;
}
void APIENTRY glEnableClientState(GLenum cap) { set_client(cap, 1); }
void APIENTRY glDisableClientState(GLenum cap) { set_client(cap, 0); }

void APIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
  gl.vptr = ptr, gl.vsize = size, gl.vstride = stride ? stride : size * 4;
}
void APIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
  gl.tptr = ptr, gl.tstride = stride ? stride : size * 4;
}
void APIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
  gl.cptr = ptr, gl.cstride = stride ? stride : size;
}

void APIENTRY glMatrixMode(GLenum mode) { gl.mode = mode; }
void APIENTRY glLoadIdentity(void) { mat_ident(cur_mat()); }
void APIENTRY glLoadMatrixf(const GLfloat *m) { memcpy(cur_mat(), m, 64); }
void APIENTRY glPushMatrix(void) {
  if (gl.mode == GL_PROJECTION) {
    if (gl.pr_top < 3)
      memcpy(gl.pr[gl.pr_top + 1], gl.pr[gl.pr_top], 64), gl.pr_top++;
  } else if (gl.mv_top < 7) {
    memcpy(gl.mv[gl.mv_top + 1], gl.mv[gl.mv_top], 64), gl.mv_top++;
  }
}
void APIENTRY glPopMatrix(void) {
  if (gl.mode == GL_PROJECTION) {
    if (gl.pr_top)
      gl.pr_top--;
  } else if (gl.mv_top) {
    gl.mv_top--;
  }
}
void APIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
  float t[16];
  mat_ident(t);
  t[12] = x, t[13] = y, t[14] = z;
  float *m = cur_mat();
  mat_mul(m, m, t);
}
void APIENTRY glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
  float o[16];
  mat_ident(o);
  o[0] = (float)(2 / (r - l));
  o[5] = (float)(2 / (t - b));
  o[10] = (float)(-2 / (f - n));
  o[12] = (float)(-(r + l) / (r - l));
  o[13] = (float)(-(t + b) / (t - b));
  o[14] = (float)(-(f + n) / (f - n));
  float *m = cur_mat();
  mat_mul(m, m, o);
}

void APIENTRY glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
  float o[16];
  memset(o, 0, sizeof o);
  o[0] = (float)(2 * n / (r - l));
  o[5] = (float)(2 * n / (t - b));
  o[8] = (float)((r + l) / (r - l));
  o[9] = (float)((t + b) / (t - b));
  o[10] = (float)(-(f + n) / (f - n));
  o[11] = -1;
  o[14] = (float)(-2 * f * n / (f - n));
  float *m = cur_mat();
  mat_mul(m, m, o);
}
void APIENTRY glMultMatrixf(const GLfloat *a) {
  float *m = cur_mat();
  mat_mul(m, m, a);
}
void APIENTRY glScalef(GLfloat x, GLfloat y, GLfloat z) {
  float t[16];
  mat_ident(t);
  t[0] = x, t[5] = y, t[10] = z;
  glMultMatrixf(t);
}
void APIENTRY glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) {
  float l = sqrtf(x * x + y * y + z * z);
  if (l == 0)
    return;
  x /= l, y /= l, z /= l;
  float r = a * (3.14159265f / 180.0f), c = cosf(r), s = sinf(r), k = 1 - c;
  float t[16] = {x * x * k + c,     y * x * k + z * s, z * x * k - y * s, 0,
                 x * y * k - z * s, y * y * k + c,     z * y * k + x * s, 0,
                 x * z * k + y * s, y * z * k - x * s, z * z * k + c,     0,
                 0,                 0,                 0,                 1};
  glMultMatrixf(t);
}

void APIENTRY glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  gl.vp[0] = x, gl.vp[1] = y, gl.vp[2] = w, gl.vp[3] = h;
}
void APIENTRY glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  gl.sc[0] = x, gl.sc[1] = y, gl.sc[2] = w, gl.sc[3] = h;
}
void APIENTRY glDepthRange(GLclampd n, GLclampd f) { gl.dn = (float)n, gl.df = (float)f; }

void APIENTRY glBlendFunc(GLenum s, GLenum d) { gl.sf = s, gl.dfac = d; }
void APIENTRY glAlphaFunc(GLenum f, GLclampf r) { gl.afunc = f, gl.aref = r; }
void APIENTRY glDepthFunc(GLenum f) { gl.dfunc = f; }
void APIENTRY glDepthMask(GLboolean m) { gl.dmask = m; }
void APIENTRY glCullFace(GLenum m) { gl.cullface = m; }
void APIENTRY glTexEnvf(GLenum t, GLenum p, GLfloat v) {
  if (p == GL_TEXTURE_ENV_MODE && gl.cunit == 0)
    gl.texenv = (GLenum)v;
}
void APIENTRY glTexEnvi(GLenum t, GLenum p, GLint v) { glTexEnvf(t, p, (GLfloat)v); }
void APIENTRY glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
  gl.clear[0] = r, gl.clear[1] = g, gl.clear[2] = b, gl.clear[3] = a;
}

void APIENTRY glClear(GLbitfield mask) {
  int was = in_frame;
  frame_start();
  if (!was && (mask & GL_COLOR_BUFFER_BIT))
    return; /* mx_frame_begin cleared colour and Z */
  if (!(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)))
    return;
#ifdef MXGL_HIRES
  /* a sprite over the scissor area (or all) in the passes' window: Z 0
     with context 1, without (ZMSK) with context 2; colour, or the
     destination's colour again (blend Cd) when only Z is cleared */
  int x0 = 0, x1 = FB_W, y0 = 0, y1 = FB_H;
  if (gl.scissor) {
    x0 = gl.sc[0] * FB_W / GL_W, x1 = (gl.sc[0] + gl.sc[2]) * FB_W / GL_W;
    y0 = (GL_H - (gl.sc[1] + gl.sc[3])) * FB_H / GL_H, y1 = (GL_H - gl.sc[1]) * FB_H / GL_H;
  }
  int z = (mask & GL_DEPTH_BUFFER_BIT) != 0, col = (mask & GL_COLOR_BUFFER_BIT) != 0;
  u64 *c = mx_frame_reserve(2 + 8);
  c = mx_e_cnt(c, 8, mx_vif_flush(), mx_vif_direct(8));
  c = mx_e_ad_tag(c, 7, 1);
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_TEST_1, !z), mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_ALPHA_1, !z), mx_gs_alpha(1, 1, 2, 1, 0));
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, !col, 0, 0, !z, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ,
              mx_gs_rgbaq((u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255),
                          (u32)(gl.clear[2] * 255), 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x0), mx_gs_py((u32)y0), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x1), mx_gs_py((u32)y1), 0));
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  mx_frame_commit(c);
#else
  /* a sprite over the scissor area (or all): Z 0, colour if asked */
  const struct mx_rt *t = &rt[buf];
  int x0 = 0, x1 = FB_W, y0 = 0, y1 = FB_H;
  if (gl.scissor) {
    x0 = gl.sc[0] * FB_W / GL_W, x1 = (gl.sc[0] + gl.sc[2]) * FB_W / GL_W;
    y0 = (GL_H - (gl.sc[1] + gl.sc[3])) * FB_H / GL_H, y1 = (GL_H - gl.sc[1]) * FB_H / GL_H;
  }
  u64 *c = mx_frame_reserve(2 + 9);
  c = mx_e_cnt(c, 9, mx_vif_flush(), mx_vif_direct(9));
  c = mx_e_ad_tag(c, 8, 1);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_ZBUF_1, mx_gs_zbuf(t->zbp, t->zpsm, !(mask & GL_DEPTH_BUFFER_BIT)));
  c = mx_e_ad(c, MX_GS_REG_FRAME_1,
              mx_gs_frame(t->fbp, t->fbw, t->psm, (mask & GL_COLOR_BUFFER_BIT) ? 0 : ~0u));
  c = mx_e_ad(c, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, FB_W - 1, 0, FB_H - 1));
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ,
              mx_gs_rgbaq((u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255),
                          (u32)(gl.clear[2] * 255), 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x0), mx_gs_py((u32)y0), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x1), mx_gs_py((u32)y1), 0));
  mx_frame_commit(c);
  c = mx_frame_direct(2);
  c = mx_e_ad_tag(c, 1, 1);
  c = mx_e_ad(c, MX_GS_REG_FRAME_1, mx_gs_frame(t->fbp, t->fbw, t->psm, 0));
  mx_frame_commit(c);
#endif
  gs.valid = 0, gs_serial++;
}

/* textures */
void APIENTRY glGenTextures(GLsizei n, GLuint *t) {
  for (int i = 0; i < n; i++) {
    t[i] = 0;
    for (int k = 1; k < MAX_TEX; k++)
      if (!texs[k].used) {
        memset(&texs[k], 0, sizeof texs[k]);
        texs[k].used = 1;
        texs[k].lin_mag = texs[k].lin_min = 1;
        t[i] = (GLuint)k;
        break;
      }
  }
}

/* data still read by frames in flight is freed two frames later */
#define DEFER_MAX 256
/* hires: a record is played until a later one replaces it, which may be
   three frames later */
#ifdef MXGL_HIRES
#define NDEFER 4
#else
#define NDEFER 3
#endif
static void *deferred[NDEFER][DEFER_MAX];
static int ndeferred[NDEFER];
static u32 defer_frame;
static void defer_free(void *p) {
  if (!p)
    return;
  int k = (int)(mx_gs.frames % NDEFER);
  if (ndeferred[k] < DEFER_MAX)
    deferred[k][ndeferred[k]++] = p;
  /* else: leaked */
}
static void defer_collect(void) {
  int k = (int)((mx_gs.frames + 1) % NDEFER); /* filled NDEFER - 1 frames ago */
  if (defer_frame == mx_gs.frames)
    return;
  defer_frame = mx_gs.frames;
  for (int i = 0; i < ndeferred[k]; i++)
    free(deferred[k][i]);
  ndeferred[k] = 0;
}

/* the lists name textures: when textures go, so do the lists (the renderer
   asks, mxgl_ListsDirty, and makes them again) */
int mxgl_ListsDirty(void) { return dl_dirty; }

void APIENTRY glDeleteTextures(GLsizei n, const GLuint *t) {
  dl_dirty = 1;
  for (int i = 0; i < n; i++)
    if (t[i] && t[i] < MAX_TEX) {
      defer_free(texs[t[i]].data);
      texs[t[i]].data = NULL;
      texs[t[i]].used = 0;
    }
}
void APIENTRY glBindTexture(GLenum target, GLuint t) {
  if (gl.cunit != 0)
    return;
  gl.bound = t;
  /* a name not from glGenTextures is made by binding it (GL 1.1; GLQuake
     numbers its textures itself) */
  if (t && t < MAX_TEX && !texs[t].used) {
    memset(&texs[t], 0, sizeof texs[t]);
    texs[t].used = 1;
    texs[t].lin_mag = texs[t].lin_min = 1;
  }
}

static u32 ilog2(u32 v) {
  u32 l = 0;
  while ((1u << l) < v)
    l++;
  return l;
}

/* RGBA8 (GL) into level lv of texture t at (x, y) */
static void convert(struct tex *t, u32 lv, int x0, int y0, int w, int h, const u8 *src) {
  u32 lw = t->w >> lv;
  u8 *dst = (u8 *)t->data + t->off[lv];
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const u8 *s = src + (y * w + x) * 4;
      u32 at = (u32)(y0 + y) * lw + (u32)(x0 + x);
      if (t->psm == MX_GS_PSM_T8) {
        u8 m = s[0] > s[1] ? s[0] : s[1];
        dst[at] = m > s[2] ? m : s[2];
      } else if (t->psm == MX_GS_PSM_CT32) {
        u32 a = (s[3] + 1u) >> 1;
        ((u32 *)dst)[at] = (u32)s[0] | (u32)s[1] << 8 | (u32)s[2] << 16 | a << 24;
      } else {
        ((u16 *)dst)[at] = (u16)(s[0] >> 3 | (s[1] >> 3) << 5 | (s[2] >> 3) << 10 | 0x8000);
      }
    }
}

/* levels 1 .. max_levels - 1 from level 0's RGBA8 texels: each the mean of
   2 x 2 of the level above */
static void make_levels(struct tex *t, const u8 *src) {
  u32 w = t->w, h = t->h;
  if (t->max_levels < 2)
    return;
  u8 *a = malloc((w / 2) * (h / 2) * 4), *b = malloc((w / 4) * (h / 4) * 4 + 4);
  if (!a || !b) {
    free(a), free(b);
    return;
  }
  const u8 *up = src;
  u8 *bufs[2] = {a, b};
  for (u32 lv = 1; lv < t->max_levels; lv++) {
    u32 nw = w >> 1, nh = h >> 1;
    u8 *dn = bufs[(lv - 1) & 1];
    for (u32 y = 0; y < nh; y++)
      for (u32 x = 0; x < nw; x++)
        for (u32 k = 0; k < 4; k++) {
          const u8 *p = up + ((2 * y) * w + 2 * x) * 4 + k;
          dn[(y * nw + x) * 4 + k] = (u8)((p[0] + p[4] + p[w * 4] + p[w * 4 + 4] + 2) >> 2);
        }
    convert(t, lv, 0, 0, (int)nw, (int)nh, dn);
    up = dn, w = nw, h = nh;
  }
  t->levels = t->max_levels;
  free(a), free(b);
}

/* the next texture is kept as its brightness (the largest of R, G and B),
   T8 with a CLUT of that in alpha, for blendFunc filter (Cd * As) */
static int tex_brightness;
void mxgl_TexBrightness(void) { tex_brightness = 1; }

/* the colour of the draws after is scaled by 1 << s (s 0 or 1), the
   overbright range of hardware gamma, done by the GS's modulate */
void mxgl_ColorShift(int s) { gl.cshift = s; }

/* texels of format (GL_RGB, GL_LUMINANCE, ...) as RGBA8 (malloc'ed), or
   NULL if they are RGBA8 already (or of a format not known) */
static u8 *to_rgba(GLenum format, GLsizei w, GLsizei h, const GLvoid *px) {
  int n = format == GL_RGB ? 3 : format == GL_LUMINANCE || format == GL_ALPHA ? 1
          : format == GL_LUMINANCE_ALPHA ? 2 : 0;
  if (!n || !px)
    return NULL;
  u8 *o = malloc((size_t)w * (size_t)h * 4);
  if (!o)
    return NULL;
  const u8 *s = px;
  for (int i = 0; i < w * h; i++, s += n) {
    u8 *d = o + i * 4;
    if (format == GL_RGB)
      d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = 255;
    else if (format == GL_ALPHA)
      d[0] = d[1] = d[2] = 255, d[3] = s[0];
    else
      d[0] = d[1] = d[2] = s[0], d[3] = n == 2 ? s[1] : 255;
  }
  return o;
}

void APIENTRY glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                                  GLint border, GLenum format, GLenum type, const GLvoid *px) {
  if (!gl.bound || gl.bound >= MAX_TEX)
    return;
  u8 *rgba = level == 0 ? to_rgba(format, w, h, px) : NULL;
  if (rgba) {
    /* a luminance texture is a brightness texture (the lightmaps of Quake) */
    if (format == GL_LUMINANCE)
      tex_brightness = 1;
    glTexImage2D(target, level, format == GL_RGB ? GL_RGB : GL_RGBA, w, h, border, GL_RGBA, type,
                 rgba);
    free(rgba);
    return;
  }
  struct tex *t = &texs[gl.bound];
  if (level > 0)
    return; /* made from level 0 */
  /* at least 8 x 8 (an IMAGE transfer of whole quadwords): scaled up */
  if (px && (w < 8 || h < 8)) {
    int nw = w < 8 ? 8 : w, nh = h < 8 ? 8 : h;
    u8 *big = malloc((size_t)nw * nh * 4);
    if (!big)
      return;
    for (int y = 0; y < nh; y++)
      for (int x = 0; x < nw; x++)
        memcpy(big + (y * nw + x) * 4, (const u8 *)px + ((y * h / nh) * w + x * w / nw) * 4, 4);
    glTexImage2D(target, level, ifmt, nw, nh, border, format, type, big);
    free(big);
    return;
  }
  if (w < 8)
    w = 8;
  if (h < 8)
    h = 8;
  /* alpha in the texels? */
  int alpha = 0;
  const u8 *s = px;
  if (s && ifmt != GL_RGB && ifmt != GL_RGB8 && ifmt != GL_RGB5 && ifmt != 3)
    for (int i = 0; i < w * h; i++)
      if (s[i * 4 + 3] != 255) {
        alpha = 1;
        break;
      }
  defer_free(t->data);
  t->w = (u32)w, t->h = (u32)h;
  t->lw = ilog2((u32)w), t->lh = ilog2((u32)h);
  t->alpha = alpha;
  t->psm = tex_brightness ? MX_GS_PSM_T8 : alpha ? MX_GS_PSM_CT32 : MX_GS_PSM_CT16;
  tex_brightness = 0;
  t->max_levels = mx_tex_max_levels(t->lw, t->lh, 8);
  t->levels = 1;
  t->bytes = 0;
  for (u32 lv = 0; lv < t->max_levels; lv++) {
    t->off[lv] = t->bytes;
    t->bytes += (((t->w >> lv) * (t->h >> lv) * tex_bpp(t)) + 63) & ~63u;
  }
  t->data = memalign(64, t->bytes);
  t->gen = 0;
  if (!t->data)
    return;
  if (s) {
    convert(t, 0, 0, 0, w, h, s);
    make_levels(t, s);
  } else {
    memset(t->data, 0, t->bytes);
  }
  mx_dcache_writeback(t->data, t->bytes);
}

void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                                     GLsizei h, GLenum format, GLenum type, const GLvoid *px) {
  if (level != 0 || !gl.bound || gl.bound >= MAX_TEX)
    return;
  struct tex *t = &texs[gl.bound];
  if (!t->data)
    return;
  u8 *rgba = to_rgba(format, w, h, px);
  if (rgba) {
    glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, type, rgba);
    free(rgba);
    return;
  }
  /* a new copy (frames in flight may still read the old one), level 0 only */
  void *d = memalign(64, t->bytes);
  if (!d)
    return;
  memcpy(d, t->data, t->bytes);
  defer_free(t->data);
  t->data = d;
  t->levels = 1;
  convert(t, 0, x, y, w, h, px);
  mx_dcache_writeback(t->data, t->bytes);
  t->gen = 0;
}

void APIENTRY glTexParameterf(GLenum target, GLenum p, GLfloat v) {
  if (!gl.bound || gl.bound >= MAX_TEX)
    return;
  struct tex *t = &texs[gl.bound];
  GLenum e = (GLenum)v;
  switch (p) {
  case GL_TEXTURE_WRAP_S: t->clamp_s = e != GL_REPEAT; break;
  case GL_TEXTURE_WRAP_T: t->clamp_t = e != GL_REPEAT; break;
  case GL_TEXTURE_MAG_FILTER: t->lin_mag = e != GL_NEAREST; break;
  case GL_TEXTURE_MIN_FILTER: {
    int mip = e == GL_NEAREST || e == GL_LINEAR                                  ? 0
              : e == GL_NEAREST_MIPMAP_NEAREST || e == GL_LINEAR_MIPMAP_NEAREST ? 1
                                                                                 : 2;
    t->lin_min = !(e == GL_NEAREST || e == GL_NEAREST_MIPMAP_NEAREST || e == GL_NEAREST_MIPMAP_LINEAR);
    t->mip = mip;
    break;
  }
  default: break;
  }
}
void APIENTRY glTexParameteri(GLenum target, GLenum p, GLint v) {
  glTexParameterf(target, p, (GLfloat)v);
}

/* queries and the rest */
GLenum APIENTRY glGetError(void) { return GL_NO_ERROR; }
void APIENTRY glGetIntegerv(GLenum p, GLint *v) {
  switch (p) {
  case GL_MAX_TEXTURE_SIZE: *v = MXGL_MAX_TEXTURE; break;
  default: *v = 0; break;
  }
}
void APIENTRY glGetBooleanv(GLenum p, GLboolean *v) { *v = 0; }
void APIENTRY glGetFloatv(GLenum p, GLfloat *v) {
  switch (p) {
  case GL_MODELVIEW_MATRIX: memcpy(v, gl.mv[gl.mv_top], 64); break;
  case GL_PROJECTION_MATRIX: memcpy(v, gl.pr[gl.pr_top], 64); break;
  default: *v = 0; break;
  }
}
GLboolean APIENTRY glIsEnabled(GLenum cap) {
  switch (cap) {
  case GL_TEXTURE_2D: return (GLboolean)gl.tex2d;
  case GL_BLEND: return (GLboolean)gl.blend;
  case GL_ALPHA_TEST: return (GLboolean)gl.atest;
  case GL_DEPTH_TEST: return (GLboolean)gl.dtest;
  case GL_CULL_FACE: return (GLboolean)gl.cull;
  case GL_SCISSOR_TEST: return (GLboolean)gl.scissor;
  default: return 0;
  }
}
const GLubyte *APIENTRY glGetString(GLenum name) {
  switch (name) {
  case GL_VENDOR: return (const GLubyte *)"maxsdk";
  case GL_RENDERER: return (const GLubyte *)"PS2 GS + VU1 (HLSL)";
  case GL_VERSION: return (const GLubyte *)"1.1";
  default: return (const GLubyte *)"";
  }
}
void APIENTRY glFinish(void) {}
/* state the GS does not have, or that does not change a picture here */
void APIENTRY glShadeModel(GLenum m) {}
void APIENTRY glDrawBuffer(GLenum m) {}
void APIENTRY glReadBuffer(GLenum m) {}
void APIENTRY glPolygonMode(GLenum f, GLenum m) {}
void APIENTRY glLineWidth(GLfloat w) {}
void APIENTRY glPointSize(GLfloat s) {}
void APIENTRY glHint(GLenum t, GLenum m) {}
void APIENTRY glPixelStorei(GLenum p, GLint v) {}
void APIENTRY glTexParameteriv(GLenum t, GLenum p, const GLint *v) {}
void APIENTRY glTexParameterfv(GLenum t, GLenum p, const GLfloat *v) {}
void APIENTRY glFlush(void) {}
void APIENTRY glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t,
                                  GLvoid *px) {
  memset(px, 0, (size_t)w * (size_t)h * (f == GL_RGB ? 3 : 4));
}
void APIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {}
void APIENTRY glStencilFunc(GLenum f, GLint r, GLuint m) {}
void APIENTRY glStencilOp(GLenum a, GLenum b, GLenum c) {}
void APIENTRY glStencilMask(GLuint m) {}
void APIENTRY glClearStencil(GLint s) {}
void APIENTRY glClearDepth(GLclampd d) {}
void APIENTRY glPolygonOffset(GLfloat a, GLfloat b) {}
void APIENTRY glClipPlane(GLenum p, const GLdouble *e) {}
void APIENTRY glLockArraysEXT(GLint first, GLsizei count) {}
void APIENTRY glUnlockArraysEXT(void) {}
void APIENTRY glActiveTextureARB(GLenum t) { gl.cunit = t == GL_TEXTURE0_ARB ? 0 : 1; }
void APIENTRY glClientActiveTextureARB(GLenum t) {}
void APIENTRY glMultiTexCoord2fARB(GLenum t, GLfloat s, GLfloat tt) {}

/* ---- set-up ----------------------------------------------------------- */


void mxgl_init(void) {
  static int done;
  if (done)
    return;
  done = 1;
#ifdef MXGL_HIRES
  /* the records of the frame stream (each played every field until the
     next replaces it) */
  void *rec = memalign(64, REC_BYTES);
  mx_vu_reset(2);
  if (!rec || mx_hires_init(&mx_video_1080i, FB_W, FB_H, MX_GS_PSM_CT16S, rec, REC_BYTES,
                            (void *)0x70002000, 0x2000) < 0)
    mx_printf("mxgl: no 1080i hires (memory %p)\n", rec);
  (void)ring, (void)rt, (void)disp, (void)buf;
#else
  mx_gs_init_video(&mx_video_ntsc);
  u32 zbp = mx_vram_alloc(mx_rt_pages(FB_W, FB_H, MX_GS_PSM_Z24));
  for (int j = 0; j < 2; j++) {
    u32 fbp = mx_vram_alloc(mx_rt_pages(FB_W, FB_H, MX_GS_PSM_CT16S));
    mx_rt_init(&rt[j], fbp, FB_W, FB_H, MX_GS_PSM_CT16S, zbp, MX_GS_PSM_Z24);
    mx_display_init(&disp[j], &rt[j]);
  }
  mx_gs_displays(disp, 2);
  mx_vu_reset(2);
  /* chain mode: the stream calls the display lists' chains */
  mx_frame_init_chain(ring, sizeof ring, (void *)0x70002000, 0x2000);
  /* frames as fast as they are made (the screen tears): the real rate */
  mx_frame_vsync(0);
#endif
  shader = MX_VU_SHADER(MXGLV, mxglv_vu, mxglv_vu_end);
  for (int i = 0; i < NARENA; i++)
    arena[i] = memalign(64, ARENA_BYTES);
  /* the HUD's font (256 x 8) in the first page after the buffers */
  mx_gs.w = FB_W, mx_gs.h = FB_H; /* the HUD's coordinates */
  mx_hud_init(mx_gs.tbp, FB_H);
  clut_cbp = mx_gs.tbp + mx_tex_blocks(256, 8, MX_GS_PSM_CT32);
  clut_make();
  vram_lo = vram_pos = clut_cbp + 4 * NCLUT;

  memset(&gl, 0, sizeof gl);
  for (int i = 0; i < 8; i++)
    mat_ident(gl.mv[i]);
  for (int i = 0; i < 4; i++)
    mat_ident(gl.pr[i]);
  gl.mode = GL_MODELVIEW;
  gl.vp[2] = GL_W, gl.vp[3] = GL_H;
  gl.df = 1;
  gl.sf = GL_ONE, gl.dfac = GL_ZERO;
  gl.dfunc = GL_LESS, gl.dmask = 1, gl.cullface = GL_BACK, gl.afunc = GL_ALWAYS;
  gl.texenv = GL_MODULATE;
  gl.color[0] = gl.color[1] = gl.color[2] = gl.color[3] = 1;

}

void mxgl_window(int w, int h) {
  GL_W = w, GL_H = h;
  gl.vp[2] = w, gl.vp[3] = h;
}

void mxgl_frame_done(void) {
  defer_collect();
  if (dropped_draws) {
    mx_printf("mxgl: %u draws dropped (arena full)\n", dropped_draws);
    dropped_draws = 0;
  }
}

int mxgl_screenshot(const char *path) {
#ifdef MXGL_HIRES
  /* both fields: 1920 x 1080 */
  return mx_hires_save_tga(path);
#endif
  /* the last frame on the screen, then the pipeline stopped (as the
     tutorials' tut_screenshot); its CT16S buffer as a 24-bit TGA */
  u32 t0 = mx_count();
  while (mx_gs.shown != mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
    ;
  mx_frame_stop();
  const struct mx_rt *t = &rt[(mx_gs.frames - 1) & 1];
  static u16 line[FB_W] MX_ALIGNED(64);
  static u8 out[FB_W * 3];
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0)
    return -1;
  u8 hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, FB_W & 0xFF, FB_W >> 8, FB_H & 0xFF, FB_H >> 8,
                24, 0x20};
  mx_fio_write(fd, hdr, sizeof hdr);
  for (u32 y = 0; y < FB_H; y++) {
    if (mx_gs_download(line, MX_GS_PAGE(t->fbp), t->fbw, t->psm, 0, y, FB_W, 1) < 0)
      break;
    for (u32 x = 0; x < FB_W; x++) {
      u16 p = line[x];
      out[x * 3 + 0] = (u8)(((p >> 10) & 31) << 3);
      out[x * 3 + 1] = (u8)(((p >> 5) & 31) << 3);
      out[x * 3 + 2] = (u8)((p & 31) << 3);
    }
    mx_fio_write(fd, out, sizeof out);
  }
  mx_fio_close(fd);
  return 0;
}
