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
   - The screen (mxgl_set_mode) is SD, the console's TV mode, or 1080i:
     - SD: 640 x 448 (NTSC) or 640 x 512 (PAL), interlaced FIELD mode, two
       16-bit colour buffers (CT16S, dithered) and one 24-bit Z buffer,
       which leaves about 1.8 MiB (PAL 1.5) of GS memory for textures.
     - 1080i: 1920 x 1080 interlaced by maxsdk's hires passes
       (<mx/hires.h>): one 1920 x 540 CT16S buffer drawn in three strips
       just ahead of the beam, every field, from the frame's stream
       recorded; Z (16 bits) in the next strip; each field's lines half a
       line apart, so the game's 1080 lines are all drawn. The passes own
       FRAME, ZBUF, XYOFFSET and SCISSOR: a draw without Z writes uses GS
       context 2 (ZMSK set), so the state registers are written in both
       contexts, and the GL scissor is not kept. About 1.8 MiB of GS memory
       is left for textures.
     The mode can change between frames: the pipeline is started again,
     GS memory filled again.

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
#include <mx/vm.h>

#include <GL/gl.h>
#include "mxglv.h"

#include "mxgl.h"

extern const u8 mxglv_vu[], mxglv_vu_end[];

#include <mx/hires.h>
/* the picture (mxgl_set_mode): SD, 640 x 448 (NTSC) or 640 x 512 (PAL)
   with two buffers, or hires, 1920 x 540 a field (1080i) */
static int hires;
static int fb_w = 640, fb_h = 448;
#define ZMAX_SD 1048575.0f  /* GS Z (24 bits) / 16: ftoi4 */
#define ZMAX_HIRES 4095.0f  /* 16 bits */
static float zmax = ZMAX_SD;
#define NARENA MX_HIRES_SLOTS /* hires: an arena per record; SD uses two */
#define HUD_SCALE 1
/* the window the game sees (mxgl_window), mapped onto the whole buffer */
static int gl_w = 640, gl_h = 448;
#define GB_LO (2048.0f - 1900.0f) /* the guard band, GS pixels */
#define GB_HI (2048.0f + 1900.0f)

/* ---- frame stream, arenas ---------------------------------------------------- */

#define RING_BYTES (64 * 1024) /* the stream's slots (chain mode) */
static u8 ring[RING_BYTES] MX_ALIGNED(64);
/* the batches' vertices of a frame (Quake: up to ~230 KB); a game may set
   another size (MXGL_ARENA_BYTES: Descent 3 takes 256 KB) */
#ifdef MXGL_ARENA_BYTES
#define ARENA_BYTES (MXGL_ARENA_BYTES)
#else
#define ARENA_BYTES (512 * 1024)
#endif
#define REC_BYTES (MX_HIRES_SLOTS * 256 * 1024) /* hires: the records (Quake: up to ~60 KB) */
/* arenas 0 and 1 are always there; hires takes the records and arenas 2,
   3 in one block (hires_mem) while it is on */
static u8 *arena[NARENA];
static u8 *hires_mem;
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
  u32 calls;                     /* GL draws (draws: the VU1 batches they make) */
  u32 c_mvp, c_lod, c_set;       /* cycles: matrix, LOD, set_state */
  u32 clipped, c_clip;           /* triangles clipped on the EE, and their cycles */
  u32 refused;                   /* draws set_state could not do */
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
  int bright;            /* T8 brightness texture (CLUT_2X/1X/INV) */
  int pal;               /* T8 indices of the game's palette (mxgl_Palette) */
  int clamp_s, clamp_t;
  int lin_mag, lin_min;  /* filters: 1 linear */
  int mip;               /* MIN_FILTER: 0 no levels, 1 nearest level, 2 both levels */
  int dirty;             /* changed (glTexSubImage2D) while in GS memory: upload it again */
  u32 vram_blocks;       /* the room it has there */
  int twin;              /* its brightness copy: twins[twin - 1], or 0 */
};
static struct tex texs[MAX_TEX];
/* brightness copies of colour textures, made when a draw multiplies with
   the destination (tex_twin) */
#define MAX_TWIN 64
static struct tex twins[MAX_TWIN];
static void tex_drop_twin(struct tex *t);
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
  u32 col8;   /* color as bytes (R, G, B, A in memory order) */
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

/* bumped when the matrices, viewport or depth range may have changed (the
   draws' matrix is made again), and when texture state changes */
static u32 mat_serial = 1, tex_serial = 1, st_serial = 1;

/* mxgl_DrawPolygon queues the polygons (pq) and draws them together
   (draw_polys) before anything that could change how they are drawn: GL
   state, matrices, colour, textures, another draw, the frame's end */
#define PQ_MAX 256
static struct {
  const float *v;
  int n;
} pq[PQ_MAX];
static int pq_n, pq_stride, pq_st;
static void draw_polys(void);
static inline void pq_flush(void) {
  if (pq_n)
    draw_polys();
}

static float *cur_mat(void) {
  pq_flush();
  mat_serial++;
  return gl.mode == GL_PROJECTION ? gl.pr[gl.pr_top] : gl.mv[gl.mv_top];
}

/* ---- the stream ---------------------------------------------------------- */

static void clut_send(void);
static void flush_out(void);
static void batch_place(void);
static u32 s_at[3];

static void frame_start(void) {
  if (in_frame)
    return;
  u64 *e;
  if (hires) {
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
    /* TEXFLUSH: textures uploaded by PATH3 since (the once records) */
    e = mx_frame_direct(6);
    e = mx_e_ad_tag(e, 5, 1);
    e = mx_e_ad(e, MX_GS_REG_TEXFLUSH, 0);
  } else {
    buf = (int)(mx_frame_begin_n() & 1);
    mx_frame_target(&rt[buf], 0);
    frame_par ^= 1;
    /* TEST off for the clear, PRMODECONT, COLCLAMP, dithering into 16 bits */
    e = mx_frame_direct(6 + 5);
    e = mx_e_ad_tag(e, 5, 0);
    e = mx_e_ad(e, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  }
  e = mx_e_ad(e, MX_GS_REG_PRMODECONT, 1);
  e = mx_e_ad(e, MX_GS_REG_COLCLAMP, 1);
  e = mx_e_ad(e, MX_GS_REG_DIMX, 0x6071243571603524ull);
  e = mx_e_ad(e, MX_GS_REG_DTHE, 1);
  if (!hires)
    e = mx_rt_e_clear(e, &rt[buf], 0, (u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255),
                      (u32)(gl.clear[2] * 255), 1);
  mx_frame_commit(e);
  in_frame = 1;
  arena_pos = 0;
  s_at[0] = s_at[1] = s_at[2] = 0;
  batch_place();
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

/* The counters, top left, in three lines: frames shown a second;
   triangles, vertices and draws a frame; RAM (32 MiB) as a bar of its
   parts, with the VM in use and swap a second; GS memory (4 MiB) as a bar,
   with the texture room used, uploads and refills a frame. The values are
   made once a second (averages over it), and logged with more. */
/* text: 0x80 is full (the font's texels times the colour / 128) */
#define HUD_LABEL 0x506050
#define HUD_TEXT 0x808080
#define HUD_OK 0x508050
#define HUD_WARN 0x808040
#define HUD_BAD 0x805050
#define HUD_BAR_X 38
#define HUD_BAR_W 100
#define HUD_TEXT_X 146
/* bar parts (boxes: 0xff is full) */
#define BAR_EMPTY 0x202020
#define BAR_PROG 0x606060  /* the program, stacks; GS: the screen and Z */
#define BAR_CORE 0x4070ff  /* the core heap in use */
#define BAR_CORE_FREE 0x203878
#define BAR_VM 0x40c040    /* VM pages (malloc without VM); GS: textures */
#define BAR_PIN 0xff9020   /* pinned VM pages */
#define BAR_UP 0xffff60    /* GS: uploaded a frame */
#define BAR_REFILL 0xd03030 /* GS: textures while they refill */
#define BAR_PARTS 5

struct hud_bar {
  u32 total, amt[BAR_PARTS], rgb[BAR_PARTS];
};
static void hud_bar(u32 y, const struct hud_bar *b) {
  u32 x = HUD_BAR_X * HUD_SCALE, w = HUD_BAR_W * HUD_SCALE;
  mx_hud_box(x, y, w, 5, 0x80000000u | BAR_EMPTY);
  /* the ends of the parts from their sums: no gaps between them */
  u32 sum = 0, at = 0;
  for (int i = 0; i < BAR_PARTS; i++) {
    sum += b->amt[i];
    u32 end = (u32)((u64)(sum < b->total ? sum : b->total) * w / b->total);
    if (end > at)
      mx_hud_box(x + at, y, end - at, 5, 0x80000000u | b->rgb[i]);
    at = end;
  }
}

static char *hud_mb(char *p, u32 bytes) { return mx_hud_u1(p, bytes, 1u << 20); }

/* a line: up to four pieces of text, each in its colour */
#define HUD_PIECES 4
struct hud_line {
  char s[HUD_PIECES][40];
  u32 rgb[HUD_PIECES];
};
static u32 hud_line(u32 x, u32 y, const struct hud_line *l, int draw) {
  for (int i = 0; i < HUD_PIECES; i++) {
    if (!l->s[i][0])
      continue;
    if (draw)
      mx_hud_text(x, y, l->s[i], l->rgb[i], HUD_SCALE);
    x += mx_hud_width(l->s[i], HUD_SCALE) + 12 * HUD_SCALE;
  }
  return x;
}

static void draw_stats(void) {
  flush_out();
  /* the second: frames shown (hires: made), and the sums of the counters */
  static u32 v_last, f_last, n_frames, tris, verts, draws, up_kb, wraps;
  static u32 swap_in, swap_out, ee_last, ee_max, n_long;
  static struct hud_line line[3];
  static struct hud_bar ram, vram;
  u32 shown = hires ? mx_gs.frames : mx_gs.shown;
  u32 now = mx_count(), ee_us = (now - ee_last) / (MX_EE_HZ / 1000000);
  ee_last = now;
  if (ee_us > ee_max)
    ee_max = ee_us;
  n_long += ee_us > 1000000 / mx_gs.hz + 500;
  n_frames++;
  tris += st_last.tris, verts += st_last.verts, draws += st_last.draws;
  up_kb += st_last.upload_kb, wraps += st_last.wraps;
  if (shown < f_last) /* the mode changed: counted again from 0 */
    f_last = shown, v_last = mx_gs.vsync;
  u32 dv = mx_gs.vsync - v_last;
  if (dv >= mx_gs.hz) {
    memset(line, 0, sizeof line);
    /* frames: FPS green at the refresh rate, red below 20 */
    u32 fps10 = (shown - f_last) * 10 * mx_gs.hz / dv;
    struct hud_line *l = &line[0];
    mx_hud_cat(mx_hud_u1(l->s[0], fps10, 10), " FPS");
    l->rgb[0] = fps10 * 100 >= mx_gs.hz * 950 ? HUD_OK : fps10 < 200 ? HUD_BAD : HUD_WARN;
    char *t = mx_hud_cat(mx_hud_u(l->s[1], tris / n_frames), " TRIS  ");
    t = mx_hud_cat(mx_hud_u(t, verts / n_frames), " VERTS  ");
    mx_hud_cat(mx_hud_u(t, draws / n_frames), " DRAWS");
    l->rgb[1] = HUD_TEXT;
    /* RAM: the program (with the kernel and stacks), the core heap, VM
       pages; without VM, malloc's part of the heap */
    struct mx_core_stats cs;
    mx_core_stats(&cs);
    ram = (struct hud_bar){32u << 20, {0, cs.used, cs.total - cs.used, 0, 0},
                           {BAR_PROG, BAR_CORE, BAR_CORE_FREE, BAR_VM, BAR_PIN}};
    l = &line[1];
    if (mx_vm_active()) {
      struct mx_vm_stats vs;
      mx_vm_stats(&vs);
      ram.amt[0] = ram.total - cs.total - vs.frames * MX_VM_PAGE;
      ram.amt[3] = (vs.frames - vs.free_frames - vs.pinned) * MX_VM_PAGE;
      ram.amt[4] = vs.pinned * MX_VM_PAGE;
      u32 out = vs.swap_outs + vs.writer_outs;
      u32 si = (vs.swap_ins - swap_in) * (MX_VM_PAGE / 1024) * mx_gs.hz / dv;
      u32 so = (out - swap_out) * (MX_VM_PAGE / 1024) * mx_gs.hz / dv;
      swap_in = vs.swap_ins, swap_out = out;
      mx_hud_cat(hud_mb(mx_hud_cat(l->s[0], "VM "), (u32)((u8 *)mx_sbrk(0) - (u8 *)mx_vm_base())),
                 " MB");
      l->rgb[0] = HUD_TEXT;
      t = mx_hud_cat(mx_hud_u(mx_hud_cat(l->s[1], "SWAP IN "), si), " OUT ");
      mx_hud_cat(mx_hud_u(t, so), " KB/S");
      l->rgb[1] = si || so ? HUD_WARN : HUD_TEXT;
    } else {
      u8 *lo = (u8 *)mx_heap_start();
      ram.amt[0] = ram.total - (u32)((u8 *)mx_heap_end() - lo);
      ram.amt[2] = 0; /* free: the bar's end */
      ram.amt[3] = (u32)((u8 *)mx_sbrk(0) - lo);
      mx_hud_cat(hud_mb(mx_hud_cat(l->s[0], "MALLOC "), ram.amt[3]), " MB");
      l->rgb[0] = HUD_TEXT;
    }
    t = mx_hud_cat(hud_mb(mx_hud_cat(l->s[2], "CORE "), cs.used), "/");
    mx_hud_cat(hud_mb(t, cs.total), " MB");
    l->rgb[2] = cs.used * 100 >= cs.total * 95 ? HUD_BAD : HUD_TEXT;
    /* GS memory (KiB): the screen, Z, font and CLUTs; textures since the
       last refill, the end of them uploaded a frame */
    u32 room = (VRAM_END - vram_lo) / 4, tex = (vram_pos - vram_lo) / 4, up = up_kb / n_frames;
    vram = (struct hud_bar){VRAM_END / 4, {vram_lo / 4, tex > up ? tex - up : 0, 0, 0, 0},
                            {BAR_PROG, wraps ? BAR_REFILL : BAR_VM, BAR_UP, 0, 0}};
    vram.amt[2] = tex - vram.amt[1];
    l = &line[2];
    mx_hud_cat(mx_hud_u(mx_hud_cat(mx_hud_u(mx_hud_cat(l->s[0], "TEX "), tex), "/"), room), " KB");
    l->rgb[0] = tex * 100 >= room * 95 ? HUD_BAD : tex * 100 >= room * 80 ? HUD_WARN : HUD_OK;
    mx_hud_cat(mx_hud_u(mx_hud_cat(l->s[1], "UP "), up), " KB/F");
    l->rgb[1] = HUD_TEXT;
    mx_hud_cat(mx_hud_u1(mx_hud_cat(l->s[2], "REFILL "), wraps * 10 / n_frames, 10), "/F");
    l->rgb[2] = wraps ? HUD_BAD : HUD_OK;
    /* the log: the lines, and more */
    u32 c = MX_EE_HZ / 10000;
    for (int i = 0; i < 3; i++)
      mx_printf("stats: %s  %s  %s\n", line[i].s[0], line[i].s[1], line[i].s[2]);
    mx_printf("stats: longest frame %u us, %u frames over a field, arena %u/%u KB\n", ee_max,
              n_long, arena_pos / 1024, ARENA_BYTES / 1024);
    mx_printf("stats: ram: program %u KB, core %u/%u KB, %s %u KB, pinned %u KB\n",
              ram.amt[0] / 1024, cs.used / 1024, cs.total / 1024,
              mx_vm_active() ? "VM pages" : "malloc", ram.amt[3] / 1024, ram.amt[4] / 1024);
    static u32 ring0, buf0;
    mx_printf("stats: xf %u.%u tri %u.%u (incl flush %u.%u) ms, wait ring %u ms buf %u ms "
              "(since last)\n",
              st_last.xf_cycles / c / 10, st_last.xf_cycles / c % 10, st_last.tri_cycles / c / 10,
              st_last.tri_cycles / c % 10, st_last.flush_cycles / c / 10,
              st_last.flush_cycles / c % 10,
              (mx_frame_stats.wait_ring - ring0) / (MX_EE_HZ / 1000),
              (mx_frame_stats.wait_buffer - buf0) / (MX_EE_HZ / 1000));
    ring0 = mx_frame_stats.wait_ring, buf0 = mx_frame_stats.wait_buffer;
    mx_printf("stats: lists %u: bounds %u us, state %u us, call %u us\n", st_last.lists,
              st_last.c_bounds / 295, st_last.c_state / 295, st_last.c_call / 295);
    mx_printf("stats: calls %u, batches %u; matrix %u us, lod %u us, set_state %u us; "
              "clipped %u in %u us, refused %u\n",
              st_last.calls, st_last.draws, st_last.c_mvp / 295, st_last.c_lod / 295,
              st_last.c_set / 295, st_last.clipped, st_last.c_clip / 295, st_last.refused);
    if (hires) {
    mx_printf("stats: hires: fields %u, passes %u, late %u, records shown %u, held %u, largest %u KB of %u; "
              "longest pass per strip %u %u %u us\n",
              mx_hires_stats.fields, mx_hires_stats.passes, mx_hires_stats.late,
              mx_hires_stats.shown, mx_hires_stats.held, mx_hires_stats.rec_max / 1024, REC_BYTES / MX_HIRES_SLOTS / 1024,
              mx_hires_stats.pass_us[0], mx_hires_stats.pass_us[1], mx_hires_stats.pass_us[2]);
    mx_hires_stats.pass_us[0] = mx_hires_stats.pass_us[1] = mx_hires_stats.pass_us[2] = 0;
    }
    v_last = mx_gs.vsync, f_last = shown;
    n_frames = tris = verts = draws = up_kb = wraps = 0;
    ee_max = n_long = 0;
  }
  if (!line[0].s[0][0])
    return; /* the first second: nothing yet */
  /* full screen, no tests */
  u64 *c = mx_frame_reserve(5);
  c = mx_e_cnt(c, 3, mx_vif_flush(), mx_vif_direct(3));
  c = mx_e_ad_tag(c, 2, 1);
  if (hires)
    c = mx_e_ad(c, MX_GS_REG_ALPHA_1, mx_gs_alpha(0, 1, 0, 1, 0)); /* the box: over */
  else
    c = mx_e_ad(c, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, fb_w - 1, 0, fb_h - 1));
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  mx_frame_commit(c);
  gs.valid = 0, gs_serial++;
  u32 x0 = 8 * HUD_SCALE, x1 = HUD_TEXT_X * HUD_SCALE, w = hud_line(x0, 0, &line[0], 0);
  for (int i = 1; i < 3; i++) {
    u32 e = hud_line(x1, 0, &line[i], 0);
    if (e > w)
      w = e;
  }
  mx_hud_box(4 * HUD_SCALE, 2, w - 12 * HUD_SCALE, 31, 0x60000000);
  hud_line(x0, 4, &line[0], 1);
  mx_hud_text(x0, 13, "RAM", HUD_LABEL, HUD_SCALE);
  hud_bar(14, &ram);
  hud_line(x1, 13, &line[1], 1);
  mx_hud_text(x0, 22, "VRAM", HUD_LABEL, HUD_SCALE);
  hud_bar(23, &vram);
  hud_line(x1, 22, &line[2], 1);
}

void mxgl_end_frame(void) {
  pq_flush();
  if (!in_frame)
    frame_start();
  flush_out();
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
  /* the triangles gathered so far are drawn with the registers before */
  flush_out();
  /* hires: the same in context 2 (the draws without Z writes) */
  u32 reg2[24];
  u64 val2[24];
  if (hires) {
    for (int i = 0; i < n; i++) {
      reg2[2 * i] = reg[i], val2[2 * i] = val[i];
      reg2[2 * i + 1] = reg[i] + 1, val2[2 * i + 1] = val[i];
    }
    reg = reg2, val = val2, n *= 2;
  }
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
  flush_out();
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
  flush_out();
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

/* the game's palette as a CLUT (mxgl_Palette), after the brightness CLUTs */
static u32 clut_pal[256] MX_ALIGNED(64);
static int pal_on;
static u8 pal_rgb[256][3];
/* exact RGB -> index (open addressing, key RGB + 1), and the nearest index
   of a 15-bit colour (for the averaged colours of mipmap levels) */
static u32 pal_key[1024];
static u8 pal_idx[1024];
static u8 *pal_near;

static int pal_find(u32 rgb) {
  u32 h = (rgb * 2654435761u) >> 22;
  for (u32 i = 0; i < 1024; i++, h = (h + 1) & 1023) {
    if (pal_key[h] == rgb + 1)
      return pal_idx[h];
    if (!pal_key[h])
      return -1;
  }
  return -1;
}

static u8 pal_nearest(const u8 *c) {
  if (!pal_near) {
    pal_near = malloc(32768);
    if (!pal_near)
      return 0;
    for (u32 k = 0; k < 32768; k++) {
      int r = (int)((k & 31) << 3 | 4), g = (int)((k >> 5 & 31) << 3 | 4), b = (int)((k >> 10) << 3 | 4);
      u32 best = ~0u;
      u8 bi = 0;
      for (int i = 0; i < 256; i++) {
        int dr = r - pal_rgb[i][0], dg = g - pal_rgb[i][1], db = b - pal_rgb[i][2];
        u32 d = (u32)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
        if (d < best)
          best = d, bi = (u8)i;
      }
      pal_near[k] = bi;
    }
  }
  int e = pal_find((u32)c[0] | (u32)c[1] << 8 | (u32)c[2] << 16);
  return e >= 0 ? (u8)e : pal_near[c[0] >> 3 | (c[1] >> 3) << 5 | (c[2] >> 3) << 10];
}

void mxgl_Palette(const unsigned char *rgb) {
  memset(pal_key, 0, sizeof pal_key);
  free(pal_near);
  pal_near = NULL;
  for (int i = 0; i < 256; i++) {
    const u8 *c = rgb + 3 * i;
    memcpy(pal_rgb[i], c, 3);
    u32 key = (u32)c[0] | (u32)c[1] << 8 | (u32)c[2] << 16;
    if (pal_find(key) < 0) {
      u32 h = (key * 2654435761u) >> 22;
      while (pal_key[h])
        h = (h + 1) & 1023;
      pal_key[h] = key + 1;
      pal_idx[h] = (u8)i;
    }
    /* CSM1's order: bits 3 and 4 of the index swapped */
    u32 at = ((u32)i & 0xE7) | ((u32)i & 8) << 1 | ((u32)i & 16) >> 1;
    clut_pal[at] = key | 0x80u << 24;
  }
  mx_dcache_writeback(clut_pal, sizeof clut_pal);
  pal_on = 1;
}

static void clut_make(void) {
  for (u32 i = 0; i < 256; i++) {
    u32 at = (i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1, g = i | i << 8 | i << 16;
    clut[CLUT_2X][at] = g | i << 24;
    clut[CLUT_1X][at] = g | ((i + 1) >> 1) << 24;
    clut[CLUT_INV][at] = g | ((256 - i) >> 1) << 24;
  }
  mx_dcache_writeback(clut, sizeof clut);
}

/* hires: texture uploads go into the frame's once record (played once,
   not in every pass), unless GS memory was refilled in this frame: then
   their order among the draws matters */
static void once_on(void) {
  if (!hires)
    return;
  flush_out();
  if (!st_cur.wraps)
    mx_hires_once(1);
}
static void once_off(void) {
  if (hires)
    mx_hires_once(0);
}

static void clut_send(void) {
  once_on();
  for (int k = 0; k < NCLUT; k++)
    image_xfer(clut_cbp + 4 * (u32)k, 1, MX_GS_PSM_CT32, 16, 16, 4, clut[k], k == 0);
  if (pal_on)
    image_xfer(clut_cbp + 4 * NCLUT, 1, MX_GS_PSM_CT32, 16, 16, 4, clut_pal, 0);
  texflush();
  once_off();
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
  u32 at;
  if (t->dirty && t->gen == vram_gen && blocks <= t->vram_blocks) {
    /* changed: again where it is (the draws before have read it: the
       upload's FLUSH), not to new room, which would fill GS memory with
       old copies (a lightmap changed every frame) */
    at = t->mt.tbp[0];
  } else {
    at = (vram_pos + align - 1) & ~(align - 1);
    if (at + blocks > VRAM_END) {
      vram_gen++;
      st_cur.wraps++;
      at = (vram_lo + align - 1) & ~(align - 1);
    }
    vram_pos = at + blocks;
    t->vram_blocks = blocks;
  }
  t->dirty = 0;
  mx_tex_init(&t->mt, at, t->psm, t->lw, t->lh, nl, MX_TEX_PACKED);
  t->gen = vram_gen;
  st_cur.uploads++;
  once_on();

  for (u32 lv = 0; lv < nl; lv++)
    image_xfer(t->mt.tbp[lv], t->mt.tbw[lv], t->psm, t->w >> lv, t->h >> lv, tex_bpp(t),
               (const u8 *)t->data + t->off[lv], lv == 0);
  texflush();
  once_off();
}

/* ---- draw state ---------------------------------------------------------- */

static int clut_pick; /* the CLUT of a brightness texture (blend_mode) */

/* the blend factors; the framebuffer keeps no alpha (GL_ALPHA_BITS 0):
   DST_ALPHA is 1 */
static void blend_factors(GLenum *s, GLenum *d) {
  *s = gl.sf == GL_DST_ALPHA ? GL_ONE : gl.sf == GL_ONE_MINUS_DST_ALPHA ? GL_ZERO : gl.sf;
  *d = gl.dfac == GL_DST_ALPHA ? GL_ONE : gl.dfac == GL_ONE_MINUS_DST_ALPHA ? GL_ZERO : gl.dfac;
}

/* a product with the destination colour: done with a brightness texture
   (blend_mode), a colour texture's brightness copy (tex_twin) */
static int blend_product(void) {
  GLenum s, d;
  blend_factors(&s, &d);
  return (s == GL_DST_COLOR && (d == GL_ZERO || d == GL_ONE || d == GL_SRC_COLOR)) ||
         (s == GL_ZERO && (d == GL_SRC_COLOR || d == GL_ONE_MINUS_SRC_COLOR));
}

static struct tex *tex_twin(struct tex *t);

static int blend_mode(u64 *alpha, const struct tex *t) {
  GLenum s, d;
  blend_factors(&s, &d);
  if (s == GL_ONE && d == GL_ZERO)
    return 0;
  if (s == GL_SRC_ALPHA && d == GL_ONE_MINUS_SRC_ALPHA) {
    *alpha = mx_gs_alpha(0, 1, 0, 1, 0);
    return 1;
  }
  /* DST_COLOR ONE (Cd * (1 + Cs), a dynamic light) of a brightness
     texture: Cd * (1 + As), As its brightness (1x) times the vertex alpha */
  if (s == GL_DST_COLOR && d == GL_ONE && t && t->bright) {
    *alpha = mx_gs_alpha(1, 2, 0, 1, 0);
    clut_pick = CLUT_1X;
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
      t->bright) {
    *alpha = mx_gs_alpha(1, 2, 0, 2, 0);
    clut_pick = gl.cshift ? CLUT_2X : CLUT_1X;
    return 1;
  }
  /* DST_COLOR SRC_COLOR: 2 Cd Cs, Cd * As up to 2x */
  if (s == GL_DST_COLOR && d == GL_SRC_COLOR && t && t->bright) {
    *alpha = mx_gs_alpha(1, 2, 0, 2, 0);
    clut_pick = CLUT_2X;
    return 1;
  }
  if (s == GL_ZERO && d == GL_ONE_MINUS_SRC_COLOR && t && t->bright) {
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
  /* a colour texture multiplied with the destination: its brightness */
  if (t && !t->bright && gl.blend && blend_product() && !(t = tex_twin(t)))
    return -1;

  u64 alpha = 0;
  clut_pick = CLUT_1X;
  int abe = gl.blend ? blend_mode(&alpha, t) : 0;
  if (abe < 0)
    return -1;
  /* the registers were written by others (the frame's start, glClear, the
     HUD): none of the values kept holds, also of those this draw does not
     write (ALPHA when it does not blend: the next draw that does must
     write it) */
  if (!gs.valid) {
    int was = gs.valid;
    memset(&gs, 0xFF, sizeof gs);
    gs.valid = was;
  }

  u32 reg[10];
  u64 val[10];
  int n = 0;
  if (t) {
    if (t->gen != vram_gen || t->dirty) {
      tex_upload(t);
      if (t->gen != vram_gen)
        return -1;
    }
    u32 tfx = gl.texenv == GL_REPLACE || gl.texenv == GL_DECAL ? 1 : 0;
    if (t->bright)
      t->mt.cbp = clut_cbp + 4 * (u32)clut_pick;
    else if (t->pal)
      t->mt.cbp = clut_cbp + 4 * NCLUT;
    u64 tex0 = mx_tex_tex0(&t->mt, 1, tfx);
    /* MMIN: 0/1 nearest/linear; with levels 2/3 (nearest texel) or 4/5
       (linear), the nearest level or both (trilinear): both unless the
       engine asked for the nearest (r_textureMode *_MIPMAP_NEAREST) */
    /* MIN_FILTER without MIPMAP: level 0 only (GLQuake's lightmaps: a
       level below mixes the surfaces packed next to each other) */
    int mipped = t->mt.levels > 1 && t->mip;
    u32 mmin = (u32)t->lin_min;
    if (mipped)
      mmin = (t->lin_min ? 4 : 2) + (t->mip != 1);
    u64 tex1 = mx_gs_tex1(0, mipped ? t->mt.levels - 1u : 0, (u32)t->lin_mag, mmin, 0, 0,
                          mipped ? (u32)lod_k : 0);
    if (mipped) {
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
  if (!hires) {
    const struct mx_rt *target = &rt[buf];
    u64 zbuf = mx_gs_zbuf(target->zbp, target->zpsm, (u32)zmsk);
    if (!gs.valid || zbuf != gs.zbuf)
      reg[n] = MX_GS_REG_ZBUF_1, val[n++] = gs.zbuf = zbuf;
  }

  int x0 = 0, x1 = fb_w - 1, y0 = 0, y1 = fb_h - 1;
  if (gl.scissor) {
    x0 = gl.sc[0] * fb_w / gl_w, x1 = (gl.sc[0] + gl.sc[2]) * fb_w / gl_w - 1;
    y0 = (gl_h - (gl.sc[1] + gl.sc[3])) * fb_h / gl_h, y1 = (gl_h - gl.sc[1]) * fb_h / gl_h - 1;
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > fb_w - 1 ? fb_w - 1 : x1, y1 = y1 > fb_h - 1 ? fb_h - 1 : y1;
    if (x1 < x0 || y1 < y0)
      return -1;
  }
  if (!hires) {
    u64 sc = mx_gs_scissor((u32)x0, (u32)x1, (u32)y0, (u32)y1);
    if (!gs.valid || sc != gs.scissor)
      reg[n] = MX_GS_REG_SCISSOR_1, val[n++] = gs.scissor = sc;
  }

  write_regs(reg, val, n);
  gs.valid = 1;

  /* hires: context 2 for no Z writes (SD: ZBUF's ZMSK) */
  *prim = (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, t != NULL, 0, abe, 0, 0, hires && zmsk, 0);
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
  float r = (float)fb_h / (float)gl_h, rx = (float)fb_w / (float)gl_w;
  vpm[0] = vw / 2 * rx;
  vpm[12] = 2048.0f - fb_w / 2 + (vx + vw / 2) * rx;
  vpm[5] = -vh / 2 * r;
  vpm[13] = 2048.0f - fb_h / 2 + (gl_h - vy - vh / 2) * r;
  vpm[10] = -zmax * (gl.df - gl.dn) / 2;
  vpm[14] = zmax * (1 - gl.dn - (gl.df - gl.dn) / 2);
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

/* ---- output ----------------------------------------------------------------- */

/* The triangles are gathered into a batch (the draws with the same state
   and matrix one after the other) written straight into the frame's arena,
   each vertex stream into its own part of it (a batch is appended to each:
   only one is gathered at a time), through the uncached accelerated segment
   (UCAB): the stores do not take cache lines (the EE's 8 KiB data cache
   would lose the vertex arrays to them), and nothing is written back.
   flush_out ends the batch: its VU1 draw goes into the stream. */
/* the parts in 64-byte units: each starts aligned (the DMA reads whole
   quadwords: a colour part 10 bytes off showed every colour shifted) */
#define POS_PART (ARENA_BYTES / 7 * 4 & ~63u) /* 16 bytes a vertex */
#define ST_PART (ARENA_BYTES / 7 * 2 & ~63u)  /* 8 */
#define COL_PART (ARENA_BYTES / 7 & ~63u)     /* 4 */
#define UCAB(p) ((void *)((uintptr_t)(p) | 0x30000000u))
static float (*o_pos)[4]; /* x, y, z, 1: no triangle ends here (ADC) */
static float (*o_st)[2];
static u32 *o_col; /* R, G, B, A bytes: stored as one word (UCAB takes whole words) */
static u32 o_n, o_cap;     /* vertices in the batch, and room for them */
/* A vertex's position (x, y, z at x, 4-byte aligned; w's bits) and texture
   coordinates into the batch in 64-bit stores: on UCAB a store costs about
   the same whatever its width (PS2: the seven words of a vertex 45 cycles
   as words, 28 as 128, 64 and 32 bits). Not 128: that needs the value in a
   GPR's upper half, which an interrupt does not keep. */
static inline __attribute__((always_inline)) void o_put_pos(u32 n, const void *x, u32 w) {
  u64 a;
  u32 z;
  memcpy(&a, x, 8);
  memcpy(&z, (const u8 *)x + 8, 4);
  u64 *d = (u64 *)o_pos[n];
  d[0] = a, d[1] = (u64)z | (u64)w << 32;
}
/* A polygon's texture coordinates less whole repeats (st_poly): the GS
   takes S times the texture's width as a fixed-point texel coordinate of
   limited range (about 2048 texels), so a floor far from the map's origin
   (Quake 2: S of 20 to 30 repeats of a 128-texel texture) was drawn in
   streaks on the PS2 (PCSX2 computes in float). */
static float st_off[2];
static int st_shift;
static inline __attribute__((always_inline)) void o_put_st(u32 n, const void *t) {
  u64 a;
  memcpy(&a, t, 8);
  if (st_shift) {
    float f[2];
    memcpy(f, &a, 8);
    f[0] -= st_off[0], f[1] -= st_off[1];
    memcpy(&a, f, 8);
  }
  *(u64 *)o_st[n] = a;
}
/* the next vertices are a polygon's whose vertex 0 has texture coordinates
   st0: the whole repeats of them are taken off (on the axes that repeat) */
static void st_poly(const float *st0) {
  const struct tex *t = gl.tex2d && gl.bound && gl.bound < MAX_TEX ? &texs[gl.bound] : NULL;
  st_off[0] = t && !t->clamp_s ? (float)(s32)st0[0] : 0.0f;
  st_off[1] = t && !t->clamp_t ? (float)(s32)st0[1] : 0.0f;
  st_shift = st_off[0] != 0.0f || st_off[1] != 0.0f;
}

/* s_at: the parts' used bytes */
static u32 o_prim;
static float o_uni[VU_MXGLV_UNI_QW][4] MX_ALIGNED(16);

/* the next batch's place: after the batches of this frame */
static void batch_place(void) {
  u8 *a = arena[frame_par];
  o_pos = UCAB(a + s_at[0]);
  o_st = UCAB(a + POS_PART + s_at[1]);
  o_col = UCAB(a + POS_PART + ST_PART + s_at[2]);
  u32 c0 = (POS_PART - s_at[0]) / 16, c1 = (ST_PART - s_at[1]) / 8, c2 = (COL_PART - s_at[2]) / 4;
  u32 c = c0 < c1 ? c0 : c1;
  c = c < c2 ? c : c2;
  /* the batch's padding (VU_MXGLV_ALIGN) and rounding to quadwords */
  o_cap = c > 8 ? c - 8 : 0;
}

/* room for k more vertices in the batch (it is drawn first if needed); 0:
   the arena is full (the triangle is dropped) */
static int batch_room(u32 k) {
  if (o_n + k <= o_cap)
    return 1;
  flush_out();
  if (k <= o_cap)
    return 1;
  dropped_draws++;
  return 0;
}

static void flush_out(void) {
  if (!o_n)
    return;
  u32 pad = VU_MXGLV_ALIGN;
  u8 *a = arena[frame_par];
  float(*pos)[4] = (void *)(a + s_at[0]);
  float(*st)[2] = (void *)(a + POS_PART + s_at[1]);
  u32 *col = (void *)(a + POS_PART + ST_PART + s_at[2]);
  s_at[0] += (o_n + pad) * 16;
  s_at[1] += ((o_n + pad) * 8 + 15) & ~15u;
  s_at[2] += ((o_n + pad) * 4 + 15) & ~15u;
  arena_pos = s_at[0] + s_at[1] + s_at[2];
  u32 tf0 = mx_count();
  /* the UCAB stores are done before the DMA reads them */
  mx_sync();
  const void *streams[3] = {pos, st, col};
  mx_frame_shader_draw(&shader, o_prim, o_uni, streams, o_n);
  st_cur.flush_cycles += mx_count() - tf0;
  st_cur.draws++;
  o_n = 0;
  batch_place();
}

static void out_vertex(const struct cv *v, int adc) {
  o_put_pos(o_n, v->x, adc ? 0x3F800000u : 0);
  o_put_st(o_n, v->st);
  u32 c = 0;
  for (int i = 0; i < 4; i++)
    c |= (u32)(s32)__builtin_fminf(__builtin_fmaxf(v->c[i], 0.0f), 255.0f) << (8 * i);
  o_col[o_n] = c;
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

/* 1: a convex polygon (fan from p[0]) faces away: by its whole area (the
   sum of its fan triangles' determinants), not one triangle's (the first
   ones can be about collinear: a tiny area of either sign) */
static int culled_poly(const float *const *p, int n) {
  if (!gl.cull)
    return 0;
  if (gl.cullface == GL_FRONT_AND_BACK)
    return 1;
  const float *a = p[0];
  /* the first triangle, when its area is clearly not 0 against its terms
     (rounding); else the sum over the fan */
  const float *b = p[1], *c = p[2];
  float t0 = a[0] * (b[1] * c[3] - b[3] * c[1]), t1 = a[1] * (b[0] * c[3] - b[3] * c[0]),
        t2 = a[3] * (b[0] * c[1] - b[1] * c[0]);
  float det = t0 - t1 + t2;
  if (__builtin_fabsf(det) < 1e-3f * (__builtin_fabsf(t0) + __builtin_fabsf(t1) + __builtin_fabsf(t2)))
    for (int i = 2; i + 1 < n; i++) {
      b = p[i], c = p[i + 1];
      det += a[0] * (b[1] * c[3] - b[3] * c[1]) - a[1] * (b[0] * c[3] - b[3] * c[0]) +
             a[3] * (b[0] * c[1] - b[1] * c[0]);
    }
  int front = det < 0;
  return gl.cullface == GL_FRONT ? front : !front;
}

/* a convex polygon of n vertices (at most CLIP_MAX - 6) cut by the planes,
   culled as a whole (its first three vertices after the cut) and drawn as
   a fan */
#define CLIP_MAX 64
static void clip_poly(struct cv *in, int n, u32 planes);
static void clip_tri(const struct cv *a, const struct cv *b, const struct cv *c, u32 planes) {
  struct cv v[CLIP_MAX];
  v[0] = *a, v[1] = *b, v[2] = *c;
  clip_poly(v, 3, planes);
}

static void clip_poly(struct cv *buf0, int n, u32 planes) {
  static struct cv buf1[CLIP_MAX];
  struct cv *in = buf0, *out = buf1;
  for (int k = 0; k < 6 && n >= 3; k++) {
    if (!(planes & (1u << k)))
      continue;
    int m = 0;
    for (int i = 0; i < n; i++) {
      const struct cv *p = &in[i], *q = &in[i + 1 == n ? 0 : i + 1];
      float dp = plane_d(p->p, k), dq = plane_d(q->p, k);
      if (dp >= 0 && m < CLIP_MAX)
        out[m++] = *p;
      if ((dp >= 0) != (dq >= 0) && m < CLIP_MAX) {
        /* from the inside vertex, whichever way round the edge is: two
           triangles sharing it (a quad's diagonal, cut one by one) get
           the same point to the bit, else a pixel line between them */
        const struct cv *a = p, *b = q;
        float da = dp, db = dq;
        if (da < 0)
          a = q, b = p, da = dq, db = dp;
        lerp(&out[m++], a, b, da / (da - db));
      }
    }
    struct cv *t = in;
    in = out, out = t, n = m;
  }
  if (n < 3)
    return;
  if (gl.cull) {
    const float *pp[CLIP_MAX];
    for (int i = 0; i < n; i++)
      pp[i] = in[i].p;
    if (culled_poly(pp, n))
      return;
  }
  if (!batch_room((u32)n))
    return;
  /* a strip of its vertices: 0, 1, n-1, 2, n-2, ... (the first two end no
     triangle) */
  for (int k = 0, lo = 1, hi = n - 1; k < n; k++) {
    int v = k == 0 ? 0 : (k & 1) ? lo++ : hi--;
    out_vertex(&in[v], k < 2);
  }
  st_cur.tris += (u32)n - 2;
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
  float a = pr[0] * (float)gl.vp[2] * 0.5f * fb_w / gl_w * pr[5] * (float)gl.vp[3] * 0.5f * fb_h / gl_h;
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

struct dl_state {
  int tex2d, blend, atest, dtest, dmask, cull;
  GLuint bound;
  GLenum texenv, sf, dfac, afunc, dfunc, cullface;
  float aref;
  int cshift;
};

static void dl_get_state(struct dl_state *s) {
  s->tex2d = gl.tex2d, s->blend = gl.blend, s->atest = gl.atest, s->dtest = gl.dtest;
  s->dmask = gl.dmask, s->cull = gl.cull, s->bound = gl.bound, s->texenv = gl.texenv;
  s->sf = gl.sf, s->dfac = gl.dfac, s->afunc = gl.afunc, s->dfunc = gl.dfunc;
  s->cullface = gl.cullface, s->aref = gl.aref, s->cshift = gl.cshift;
}

static void dl_set_state(const struct dl_state *s) {
  pq_flush();
  st_serial++;
  gl.tex2d = s->tex2d, gl.blend = s->blend, gl.atest = s->atest, gl.dtest = s->dtest;
  gl.dmask = s->dmask, gl.cull = s->cull, gl.bound = s->bound, gl.texenv = s->texenv;
  gl.sf = s->sf, gl.dfac = s->dfac, gl.afunc = s->afunc, gl.dfunc = s->dfunc;
  gl.cullface = s->cullface, gl.aref = s->aref, gl.cshift = s->cshift;
}

/* The vertices transformed and their outcodes on VU0 (macro mode): each
   vertex (x, y, z at src, 4-byte aligned) by M into GS coordinates (times
   w, into vp, 16-byte aligned) and by Mc, M with the guard band and the
   depth range scaled to +/-w (x and y negated), whose CLIP flags are the
   outcode bits in plane_d's order; a vertex behind the eye (w <= 0), where
   CLIP's |w| does not give them, gets bit 7 (the caller works its bits out
   with plane_d). mats: M's columns, then Mc's. Returns the AND of
   the outcodes, *oor their OR. (The FPU did this at about 75 EE cycles a
   vertex: its values went between the FPU, the GPRs and VU0.)
   With interrupts off: the loop keeps 128-bit values in GPRs (pcpyld to
   qmtc2, qmfc2 to pcpyud), whose upper halves an interrupt does not keep
   (on the PS2, models came out torn and the program crashed); at most
   XFORM_RUN vertices at a time, so the hires pass timer is not held up. */
#define XFORM_RUN 64
static u32 vu0_xform_run(const u8 *src, int stride, int n, float (*vp)[4], u8 *vo,
                         const float *mats, u32 *oor) {
  u32 oand, o_or;
  int was = mx_di();
  __asm__ volatile(
      ".set push\n\t.set noreorder\n\t"
      "lqc2 $vf4, 0(%[m])\n\t"
      "lqc2 $vf5, 16(%[m])\n\t"
      "lqc2 $vf6, 32(%[m])\n\t"
      "lqc2 $vf7, 48(%[m])\n\t"
      "lqc2 $vf8, 64(%[m])\n\t"
      "lqc2 $vf9, 80(%[m])\n\t"
      "lqc2 $vf10, 96(%[m])\n\t"
      "lqc2 $vf11, 112(%[m])\n\t"
      "addiu %[and], $zero, 0x3F\n\t"
      "move %[or], $zero\n"
      "1:\n\t"
      "ldl $8, 7(%[src])\n\t"
      "ldr $8, 0(%[src])\n\t"
      "lw $9, 8(%[src])\n\t"
      "pcpyld $8, $9, $8\n\t"
      "qmtc2 $8, $vf1\n\t"
      "vmulaw.xyzw $ACC, $vf7, $vf0w\n\t"
      "vmaddax.xyzw $ACC, $vf4, $vf1x\n\t"
      "vmadday.xyzw $ACC, $vf5, $vf1y\n\t"
      "vmaddz.xyzw $vf2, $vf6, $vf1z\n\t"
      "vmulaw.xyzw $ACC, $vf11, $vf0w\n\t"
      "vmaddax.xyzw $ACC, $vf8, $vf1x\n\t"
      "vmadday.xyzw $ACC, $vf9, $vf1y\n\t"
      "vmaddz.xyzw $vf3, $vf10, $vf1z\n\t"
      "sqc2 $vf2, 0(%[vp])\n\t"
      "vclipw.xyz $vf3, $vf3w\n\t"
      "qmfc2 $10, $vf2\n\t"
      "pcpyud $10, $10, $10\n\t"
      "dsra32 $10, $10, 0\n\t"
      "addu %[src], %[src], %[stride]\n\t"
      "addiu %[vp], %[vp], 16\n\t"
      "slti $10, $10, 1\n\t"
      "sll $10, $10, 7\n\t"
      "vnop\n\t"
      "cfc2 $11, $vi18\n\t"
      "andi $11, $11, 0x3F\n\t"
      "or $11, $11, $10\n\t"
      "sb $11, 0(%[vo])\n\t"
      "addiu %[vo], %[vo], 1\n\t"
      "and %[and], %[and], $11\n\t"
      "addiu %[n], %[n], -1\n\t"
      "bgtz %[n], 1b\n\t"
      "or %[or], %[or], $11\n\t"
      ".set pop\n\t"
      : [and] "=&r"(oand), [or] "=&r"(o_or), [src] "+r"(src), [vp] "+r"(vp), [vo] "+r"(vo),
        [n] "+r"(n)
      : [m] "r"(mats), [stride] "r"(stride)
      : "$8", "$9", "$10", "$11", "$vf1", "$vf2", "$vf3", "$vf4", "$vf5", "$vf6", "$vf7", "$vf8",
        "$vf9", "$vf10", "$vf11", "memory");
  mx_irq_restore(was);
  *oor = o_or;
  return oand;
}
static u32 vu0_xform(const u8 *src, int stride, int n, float (*vp)[4], u8 *vo,
                     const float *mats, u32 *oor) {
  u32 oand = 0x3F, o_or = 0;
  while (n > 0) {
    int k = n < XFORM_RUN ? n : XFORM_RUN;
    u32 o;
    /* the run's vertices resident first (with virtual memory, the game's
       arrays may be swapped out: interrupts are off in the run) */
    const u8 *end = src + (k - 1) * stride;
    for (const u8 *p = src; p <= end; p += MX_VM_PAGE)
      (void)*(const volatile u8 *)p;
    (void)*(const volatile u8 *)end;
    oand &= vu0_xform_run(src, stride, k, vp, vo, mats, &o);
    o_or |= o;
    src += k * stride, vp += k, vo += k, n -= k;
  }
  *oor = o_or;
  return oand;
}

/* where a draw's vertices are (the GL arrays), for put_vertex */
struct vsrc {
  const u8 *vb, *tb, *cb;
  int vs, ts, cs, has_z;
  u32 col;
};
static inline __attribute__((always_inline)) struct vsrc vsrc_get(void) {
  struct vsrc r;
  r.vb = (const u8 *)gl.vptr, r.vs = gl.vstride, r.has_z = gl.vsize > 2;
  r.tb = gl.ta && gl.tptr ? (const u8 *)gl.tptr : NULL, r.ts = gl.tstride;
  r.cb = gl.ca && gl.cptr ? gl.cptr : NULL, r.cs = gl.cstride;
  r.col = gl.col8;
  return r;
}
/* vertex v into the batch as it is (position, texture coordinates, colour
   bytes); room is there (batch_room) */
static inline __attribute__((always_inline)) void put_vertex(const struct vsrc *s, u32 v, int adc) {
  const float *x = (const float *)(s->vb + v * s->vs);
  if (s->has_z) {
    o_put_pos(o_n, x, adc ? 0x3F800000u : 0);
  } else {
    float *d = o_pos[o_n];
    d[0] = x[0], d[1] = x[1], d[2] = 0, d[3] = adc ? 1.0f : 0.0f;
  }
  if (s->tb) {
    o_put_st(o_n, s->tb + v * s->ts);
  } else {
    o_st[o_n][0] = o_st[o_n][1] = 0;
  }
  u32 c = s->col;
  if (s->cb)
    memcpy(&c, s->cb + v * s->cs, 4);
  o_col[o_n] = c;
  o_n++;
}

/* the interleaved case (texture coordinates and colour in the vertex's
   record: the immediate arrays, Quake's polygons and models): a vertex is
   six words from one place */
struct fastv {
  const u8 *vb;
  int vs, toff, coff; /* coff < 0: the colour col */
  u32 col;
  int cmode; /* facing: 0 none culled, 1 det < 0 culled, 2 det >= 0 culled, 3 all */
};
static inline int fastv_get(struct fastv *f) {
  const u8 *vb = (const u8 *)gl.vptr;
  int vs = gl.vstride;
  if (gl.vsize < 3 || !gl.ta || !gl.tptr || gl.tstride != vs)
    return 0;
  int toff = (int)((const u8 *)gl.tptr - vb);
  if (toff < 12 || toff + 8 > vs || (toff & 3))
    return 0;
  int coff = -1;
  if (gl.ca && gl.cptr) {
    coff = (int)(gl.cptr - vb);
    if (gl.cstride != vs || coff < 12 || coff + 4 > vs || (coff & 3))
      return 0;
  }
  f->vb = vb, f->vs = vs, f->toff = toff, f->coff = coff, f->col = gl.col8;
  /* culled(): GL_FRONT culls front (det < 0), GL_BACK the others */
  f->cmode = !gl.cull ? 0 : gl.cullface == GL_FRONT_AND_BACK ? 3 : gl.cullface == GL_FRONT ? 1 : 2;
  return 1;
}
static inline __attribute__((always_inline)) void fast_put(const struct fastv *f, u32 v, int adc) {
  const u8 *r = f->vb + v * (u32)f->vs;
  const u32 *x = (const u32 *)r, *t = (const u32 *)(r + f->toff);
  o_put_pos(o_n, x, adc ? 0x3F800000u : 0);
  o_put_st(o_n, t);
  o_col[o_n] = f->coff >= 0 ? *(const u32 *)(r + f->coff) : f->col;
  o_n++;
}
static inline __attribute__((always_inline)) int fast_culled(const struct fastv *f, const float *a,
                                                             const float *b, const float *c) {
  if (!f->cmode)
    return 0;
  if (f->cmode == 3)
    return 1;
  float det = a[0] * (b[1] * c[3] - b[3] * c[1]) - a[1] * (b[0] * c[3] - b[3] * c[0]) +
              a[3] * (b[0] * c[1] - b[1] * c[0]);
  return f->cmode == 1 ? det < 0 : !(det < 0);
}

static void draw_tris_(const u32 *idx, int nidx, int nverts);
static void draw_tris(const u32 *idx, int nidx, int nverts) {
  u32 t0 = mx_count();
  draw_tris_(idx, nidx, nverts);
  st_cur.gl_cycles += mx_count() - t0;
}

static int dl_compiling;
static void dl_record(const u32 *idx, int nidx, int nverts);
/* set by glEnd: the draw is a GL_POLYGON (convex and planar), a fan from
   vertex 0 */
static int draw_polygon;
/* set by glEnd, glDrawArrays: the draw's mode (GL_TRIANGLE_STRIP, _FAN: as
   strips, the triangles in the order of the index list) */
static GLenum draw_mode;
/* mxgl_MultiDraw: the primitives of the draw (each a strip, fan or
   triangles from vertex first[i], count[i] vertices) */
static struct {
  const GLenum *mode;
  const int *first, *count;
  int n;
} multi;

/* the draws' matrix (GS coordinates times w), M and Mc for vu0_xform, made
   again when the matrices may have changed */
static float m[16] MX_ALIGNED(16);
static u32 m_serial;
static float mats[32] MX_ALIGNED(16);
static int mats_ok;
static void update_matrix(void) {
  /* the matrix, made again when the matrices may have changed */
  u32 tm0 = mx_count();
  /* M and Mc for vu0_xform (Mc: x' = (2048 w - x) / 1900, the same for
     y, z' = (z - c w) / h with the depth planes at c +/- h) */
  if (m_serial != mat_serial) {
    make_mvp(m);
    zn = zmax * (1 - gl.dn);
    zf = zmax * (1 - gl.df);
    m_serial = mat_serial;
    float c = (zn + zf) * 0.5f, hz = (zn - zf) * 0.5f;
    const float gc = 2048.0f, gh = 1.0f / ((GB_HI - GB_LO) * 0.5f);
    mats_ok = hz > 0;
    for (int col = 0; col < 4; col++) {
      const float *mc = m + 4 * col;
      float *o = mats + 16 + 4 * col;
      mats[4 * col + 0] = mc[0], mats[4 * col + 1] = mc[1];
      mats[4 * col + 2] = mc[2], mats[4 * col + 3] = mc[3];
      o[0] = (gc * mc[3] - mc[0]) * gh;
      o[1] = (gc * mc[3] - mc[1]) * gh;
      o[2] = mats_ok ? (mc[2] - c * mc[3]) / hz : 0;
      o[3] = mc[3];
    }
  }
  st_cur.c_mvp += mx_count() - tm0;
}

/* the GS state of a draw (set_state, unless nothing it depends on has
   changed), and the batch it goes into; 0: it cannot be drawn */
static u32 k_gen, k_vgen, k_tser, k_mser, k_gser, k_sser, k_prim;
static s32 k_lod;
static int k_ok, k_valid;
static float k_cs[4];
static u32 o_mser;
static int prepare_state(const u32 *idx, int nidx, int nverts) {
  /* the GS state: as the draw before's if nothing it depends on has
     changed (the serials of the GL state, textures, matrices) and no
     register was written since; then K too */
  struct tex *bt = gl.bound && gl.bound < MAX_TEX ? &texs[gl.bound] : NULL;
  u32 bgen = bt ? bt->gen + (u32)bt->dirty * 0x80000000u : 0;
  int same = k_valid && k_gser == gs_serial && k_sser == st_serial && k_tser == tex_serial &&
             k_vgen == vram_gen && k_gen == bgen;
  if (!(same && k_mser == mat_serial)) {
    u32 tl0 = mx_count();
    s32 k = lod_k(idx, nidx, nverts);
    u32 ts0 = mx_count();
    st_cur.c_lod += ts0 - tl0;
    /* only the matrix changed (a model's): the state is the same unless
       its LOD is not */
    if (same && k == k_lod) {
      k_mser = mat_serial;
      goto have;
    }
    k_lod = k;
    k_ok = set_state(&k_prim, k_cs, k) == 0;
    if (!k_ok) {
      static int told;
      if (told++ < 5)
        mx_printf("mxgl: draw refused: tex %u psm %u used %d data %p blend %d sf %x df %x\n",
                  gl.bound, bt ? bt->psm : 0, bt ? bt->used : 0, bt ? bt->data : 0, gl.blend,
                  gl.sf, gl.dfac);
    }
    st_cur.c_set += mx_count() - ts0;
    k_valid = 1;
    k_gen = bt ? bt->gen + (u32)bt->dirty * 0x80000000u : 0;
    k_vgen = vram_gen, k_tser = tex_serial, k_mser = mat_serial, k_gser = gs_serial;
    k_sser = st_serial;
  }
have:
  if (!k_ok) {
    st_cur.refused++;
    return 0;
  }
  /* gathered with the triangles before if they are drawn the same way */
  float *ocs = o_uni[VU_MXGLV_CSCALE];
  if (o_n && (k_prim != o_prim || o_mser != m_serial || k_cs[0] != ocs[0] || k_cs[1] != ocs[1] ||
              k_cs[2] != ocs[2] || k_cs[3] != ocs[3]))
    flush_out();
  o_prim = k_prim;
  ocs[0] = k_cs[0], ocs[1] = k_cs[1], ocs[2] = k_cs[2], ocs[3] = k_cs[3];
  if (o_mser != m_serial) {
    memcpy(o_uni[VU_MXGLV_MVP], m, 64);
    o_mser = m_serial;
  }
  return 1;
}

static void draw_tris_(const u32 *idx, int nidx, int nverts) {
  pq_flush();
  if (nidx < 3 || !gl.va || !gl.vptr)
    return;
  if (dl_compiling) {
    dl_record(idx, nidx, nverts);
    return;
  }
  frame_start();
  st_cur.calls++;
  update_matrix();

  /* the vertices in GS coordinates, and their outcodes */
  if (nverts <= SPR_VERTS) {
    vp_cache = SPR_VP, vo_cache = SPR_VO;
  } else {
    if ((u32)nverts > v_cache_n) {
      mx_core_free(vp_ram);
      mx_core_free(vo_ram);
      v_cache_n = (u32)nverts + 256;
      /* core memory: VU0 writes them with interrupts off */
      vp_ram = mx_core_alloc(v_cache_n * 16, 16);
      vo_ram = mx_core_alloc((v_cache_n + 15) & ~15u, 16);
    }
    vp_cache = vp_ram, vo_cache = vo_ram;
  }
  u32 tx0 = mx_count();
  st_cur.verts += (u32)nverts;
  const u8 *vb = (const u8 *)gl.vptr;
  const int vs = gl.vstride, has_z = gl.vsize > 2;
  const float zn_ = zn, zf_ = zf;
  const float m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3], m4 = m[4], m5 = m[5], m6 = m[6],
              m7 = m[7], m8 = m[8], m9 = m[9], m10 = m[10], m11 = m[11], m12 = m[12],
              m13 = m[13], m14 = m[14], m15 = m[15];
  u32 oand = 0x3F, oor = 0;
  /* the transform and the outcodes (bit k: outside plane k, plane_d < 0):
     on VU0 (vu0_xform), else in FPU registers, the outcodes as the sign
     bits of the distances */
  int nxf = nverts;
  if (nxf && has_z && mats_ok) {
    oand = vu0_xform(vb, vs, nxf, vp_cache, vo_cache, mats, &oor);
    /* vertices behind the eye: their outcodes from the signed distances */
    if (oor & 0x80) {
      oand = 0x3F, oor = 0;
      for (int i = 0; i < nxf; i++) {
        u32 o = vo_cache[i];
        if (o & 0x80) {
          o = 0;
          for (int k = 0; k < 6; k++)
            if (plane_d(vp_cache[i], k) < 0)
              o |= 1u << k;
          vo_cache[i] = (u8)o;
        }
        oand &= o, oor |= o;
      }
    }
    nxf = 0;
  }
  for (int i = 0; i < nxf; i++) {
    const float *x = (const float *)(vb + i * vs);
    float x0 = x[0], x1 = x[1], x2 = has_z ? x[2] : 0;
    float px = m0 * x0 + m4 * x1 + m8 * x2 + m12;
    float py = m1 * x0 + m5 * x1 + m9 * x2 + m13;
    float pz = m2 * x0 + m6 * x1 + m10 * x2 + m14;
    float pw = m3 * x0 + m7 * x1 + m11 * x2 + m15;
    float *d = vp_cache[i];
    d[0] = px, d[1] = py, d[2] = pz, d[3] = pw;
    union {
      float f;
      u32 u;
    } d0 = {px - GB_LO * pw}, d1 = {GB_HI * pw - px}, d2 = {py - GB_LO * pw},
      d3 = {GB_HI * pw - py}, d4 = {zn_ * pw - pz}, d5 = {pz - zf_ * pw};
    u32 o = d0.u >> 31 | (d1.u >> 31) << 1 | (d2.u >> 31) << 2 | (d3.u >> 31) << 3 |
            (d4.u >> 31) << 4 | (d5.u >> 31) << 5;
    vo_cache[i] = (u8)o;
    oand &= o;
    oor |= o;
  }
  st_cur.xf_cycles += mx_count() - tx0;
  /* all of it outside one plane: nothing to draw */
  if (oand)
    return;
  if (!prepare_state(idx, nidx, nverts))
    return;


  u32 tt0 = mx_count();
  if (draw_polygon && nverts == nidx / 3 + 2 && nverts <= CLIP_MAX - 6) {
    if (gl.ta && gl.tptr)
      st_poly((const float *)gl.tptr);
    if (!oor && has_z) {
      /* inside: one facing test for the whole polygon */
      if (gl.cull) {
        const float *pp[CLIP_MAX];
        for (int i = 0; i < nverts; i++)
          pp[i] = vp_cache[i];
        if (culled_poly(pp, nverts))
          goto done;
      }
      if (!batch_room((u32)nverts))
        goto done;
      /* a strip of its vertices: 0, 1, n-1, 2, n-2, ... */
      struct fastv fv;
      if (fastv_get(&fv)) {
        fast_put(&fv, 0, 1);
        for (int k = 1, lo = 1, hi = nverts - 1; k < nverts; k++)
          fast_put(&fv, (u32)((k & 1) ? lo++ : hi--), k < 2);
      } else {
        const struct vsrc src = vsrc_get();
        for (int k = 0, lo = 1, hi = nverts - 1; k < nverts; k++) {
          int v = k == 0 ? 0 : (k & 1) ? lo++ : hi--;
          put_vertex(&src, (u32)v, k < 2);
        }
      }
      st_cur.tris += (u32)nverts - 2;
      st_shift = 0;
      goto done;
    }
    if (oor) {
      /* across planes: cut once as a polygon */
      u32 tc0 = mx_count();
      struct cv poly[CLIP_MAX];
      for (int i = 0; i < nverts; i++)
        fetch(&poly[i], i, m);
      clip_poly(poly, nverts, oor);
      st_cur.clipped++;
      st_cur.c_clip += mx_count() - tc0;
      st_shift = 0;
      goto done;
    }
    st_shift = 0;
  }
  /* a strip or fan inside all planes: one strip (ADC on the vertex that
     ends a culled triangle; a fan's centre again, with ADC, every second
     triangle: 0 1 2 0* 3 4 0* 5 ... draws (0 1 2) (2 0 3) (0 3 4) ...) */
  if (multi.n) {
    /* each primitive: as a strip when inside all planes, else its
       triangles one by one (cut when across a plane) */
    const struct vsrc src = vsrc_get();
    struct fastv fv;
    const int fast = fastv_get(&fv);
    u32 drawn = 0;
    for (int p = 0; p < multi.n; p++) {
      int f = multi.first[p], c = multi.count[p];
      GLenum md = multi.mode[p];
      if (c < 3 || f < 0 || f + c > nverts)
        continue;
      u32 pand = 0x3F, por = 0;
      for (int i = f; i < f + c; i++)
        pand &= vo_cache[i], por |= vo_cache[i];
      if (pand)
        continue;
      int fan = md == GL_TRIANGLE_FAN, strip = md == GL_TRIANGLE_STRIP;
      int ntri = fan || strip ? c - 2 : c / 3;
      if (!por && (fan || strip) && fast) {
        if (!batch_room((u32)(fan ? c + (c - 2) / 2 + 1 : c)))
          break;
        fast_put(&fv, (u32)f, 1);
        fast_put(&fv, (u32)f + 1, 1);
        const float(*vq)[4] = (const float(*)[4])vp_cache;
        for (int t = 0; t < ntri; t++) {
          int ia = fan ? f : f + t + (strip && (t & 1)), ib = f + t + 1 - (strip && (t & 1)),
              ic = f + t + 2;
          int cl = fast_culled(&fv, vq[ia], vq[ib], vq[ic]);
          drawn += !cl;
          if (fan && (t & 1))
            fast_put(&fv, (u32)f, 1);
          fast_put(&fv, (u32)ic, cl);
        }
        continue;
      }
      if (!por && (fan || strip)) {
        if (!batch_room((u32)(fan ? c + (c - 2) / 2 + 1 : c)))
          break;
        put_vertex(&src, (u32)f, 1);
        put_vertex(&src, (u32)f + 1, 1);
        for (int t = 0; t < ntri; t++) {
          u32 a = fan ? (u32)f : (u32)(f + t), b = (u32)(f + t + 1), e = (u32)(f + t + 2);
          if (strip && (t & 1))
            a = (u32)(f + t + 1), b = (u32)(f + t);
          int cl = gl.cull && culled(vp_cache[a], vp_cache[b], vp_cache[e]);
          drawn += !cl;
          if (fan && (t & 1))
            put_vertex(&src, (u32)f, 1);
          put_vertex(&src, (u32)f + (u32)t + 2, cl);
        }
        continue;
      }
      for (int t = 0; t < ntri; t++) {
        u32 a, b, e;
        if (fan)
          a = (u32)f, b = (u32)(f + t + 1), e = (u32)(f + t + 2);
        else if (strip)
          a = (u32)(f + t + (t & 1)), b = (u32)(f + t + !(t & 1)), e = (u32)(f + t + 2);
        else
          a = (u32)(f + 3 * t), b = a + 1, e = a + 2;
        u32 o0 = vo_cache[a], o1 = vo_cache[b], o2 = vo_cache[e];
        if (o0 & o1 & o2)
          continue;
        if (!(o0 | o1 | o2)) {
          if (gl.cull && culled(vp_cache[a], vp_cache[b], vp_cache[e]))
            continue;
          if (!batch_room(3))
            break;
          put_vertex(&src, a, 1);
          put_vertex(&src, b, 1);
          put_vertex(&src, e, 0);
          drawn++;
        } else {
          struct cv v0, v1, v2;
          fetch(&v0, (int)a, m), fetch(&v1, (int)b, m), fetch(&v2, (int)e, m);
          clip_tri(&v0, &v1, &v2, o0 | o1 | o2);
          st_cur.clipped++;
        }
      }
    }
    st_cur.tris += drawn;
    goto done;
  }
  if ((draw_mode == GL_TRIANGLE_STRIP || draw_mode == GL_TRIANGLE_FAN) && !oor &&
      nidx == 3 * (nverts - 2)) {
    int fan = draw_mode == GL_TRIANGLE_FAN;
    if (!batch_room((u32)(fan ? nverts + (nverts - 2) / 2 + 1 : nverts)))
      goto done;
    const struct vsrc src = vsrc_get();
    u32 drawn = 0;
#define TRI_CULLED(t)                                                                             \
  (gl.cull && culled(vp_cache[idx[3 * (t)]], vp_cache[idx[3 * (t) + 1]], vp_cache[idx[3 * (t) + 2]]))
    put_vertex(&src, 0, 1);
    put_vertex(&src, 1, 1);
    for (int t = 0; t + 2 < nverts; t++) {
      int c = TRI_CULLED(t);
      drawn += !c;
      if (fan && (t & 1))
        put_vertex(&src, 0, 1);
      put_vertex(&src, (u32)t + 2, c);
    }
#undef TRI_CULLED
    st_cur.tris += drawn;
    goto done;
  }
  const struct vsrc src = vsrc_get();
  for (int i = 0; i + 2 < nidx; i += 3) {
    u32 i0 = idx[i], i1 = idx[i + 1], i2 = idx[i + 2];
    if ((int)i0 >= nverts || (int)i1 >= nverts || (int)i2 >= nverts)
      continue;
    u32 o0 = vo_cache[i0], o1 = vo_cache[i1], o2 = vo_cache[i2];
    if (o0 & o1 & o2)
      continue;
    if (!(o0 | o1 | o2)) {
      /* inside: the vertex as it is (position, texture coordinates, colour
         bytes) */
      if (culled(vp_cache[i0], vp_cache[i1], vp_cache[i2]))
        continue;
      if (!batch_room(3))
        continue;
      /* a strip of three */
      put_vertex(&src, i0, 1);
      put_vertex(&src, i1, 1);
      put_vertex(&src, i2, 0);
      st_cur.tris++;
    } else {
      u32 tc0 = mx_count();
      struct cv v0, v1, v2;
      fetch(&v0, (int)i0, m), fetch(&v1, (int)i1, m), fetch(&v2, (int)i2, m);
      clip_tri(&v0, &v1, &v2, o0 | o1 | o2);
      st_cur.clipped++;
      st_cur.c_clip += mx_count() - tc0;
    }
  }
done:
  st_cur.tri_cycles += mx_count() - tt0;
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

struct dl_entry {
  struct dl_entry *next;
  struct dl_state st;
  float (*pos)[4];
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
  u32 prim = MX_GS_PRIM_TRISTRIP;
  struct dl_entry *e = dl_take(sizeof *e);
  float(*pos)[4] = dl_take((n + pad) * 16);
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
      /* a strip of three a triangle (the first two end none) */
      pos[o][0] = x[0], pos[o][1] = x[1], pos[o][2] = gl.vsize > 2 ? x[2] : 0;
      pos[o][3] = j < 2 ? 1.0f : 0.0f;
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
  mx_dcache_writeback(pos, (n + pad) * 16);
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
    dl_pool = mx_core_alloc(DL_POOL_BYTES, 64); /* the DMAC reads it */
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
  pq_flush();
  if (id <= 0 || id >= DL_MAX || !dlists[id].used)
    return;
  u32 t0 = mx_count();
  frame_start();
  flush_out();
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
    float sx = pr[0] * (float)gl.vp[2] * 0.5f * fb_w / gl_w, sy = pr[5] * (float)gl.vp[3] * 0.5f * fb_h / gl_h;
    float a = sx * sy;
    lpp = 0.5f * fast_log2(a < 0 ? -a : a);
    zn = zmax * (1 - gl.dn);
    zf = zmax * (1 - gl.df);
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
  zn = zmax * (1 - gl.dn);
  zf = zmax * (1 - gl.df);
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
      gl.vptr = &e->pos[0][0], gl.vsize = 3, gl.vstride = 16, gl.va = 1;
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
    flush_out();
    mx_frame_shader_call(&shader, prim, uni, &e->ch);
    st_cur.c_call += mx_count() - c2;
    st_cur.draws++;
    st_cur.lists++;
    st_cur.tris += e->n / 3;
    st_cur.verts += e->n;
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

#define IMM_MAX 4096 /* vertices of a glBegin .. glEnd, glDrawArrays */

/* the index lists of the immediate modes: fixed patterns, made once */
static u32 ix_tri[3 * IMM_MAX], ix_strip[3 * IMM_MAX], ix_fan[3 * IMM_MAX], ix_quad[3 * IMM_MAX];
static void ix_make(void) {
  for (int i = 0; i < 3 * IMM_MAX; i++)
    ix_tri[i] = (u32)i;
  for (int i = 0; i + 2 < IMM_MAX; i++) {
    u32 *s = &ix_strip[3 * i], *f = &ix_fan[3 * i];
    if (i & 1)
      s[0] = (u32)i + 1, s[1] = (u32)i, s[2] = (u32)i + 2;
    else
      s[0] = (u32)i, s[1] = (u32)i + 1, s[2] = (u32)i + 2;
    f[0] = 0, f[1] = (u32)i + 1, f[2] = (u32)i + 2;
  }
  for (int q = 0; 4 * q + 3 < IMM_MAX; q++) {
    u32 *d = &ix_quad[6 * q], b = 4 * (u32)q;
    d[0] = b, d[1] = b + 1, d[2] = b + 2, d[3] = b, d[4] = b + 2, d[5] = b + 3;
  }
}


/* the index list of a mode (the fixed patterns of glEnd) and its length for
   n vertices; NULL for modes not drawn */
static inline const u32 *mode_ix(GLenum mode, int n, int *k) {
  static int made;
  if (!made) {
    ix_make();
    made = 1;
  }
  switch (mode) {
  case GL_TRIANGLES: *k = n / 3 * 3; return ix_tri;
  case GL_TRIANGLE_STRIP:
  case GL_QUAD_STRIP: *k = 3 * (n - 2); return ix_strip;
  case GL_TRIANGLE_FAN:
  case GL_POLYGON: *k = 3 * (n - 2); return ix_fan;
  case GL_QUADS: *k = n / 4 * 6; return ix_quad;
  default: *k = 0; return NULL;
  }
}

void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  int k;
  const u32 *ix = mode_ix(mode, count, &k);
  if (!ix || count < 3 || count > IMM_MAX || !gl.va || !gl.vptr)
    return;
  /* the arrays from vertex first on */
  const float *vp = gl.vptr, *tp = gl.tptr;
  const u8 *cp = gl.cptr;
  gl.vptr = (const float *)((const u8 *)vp + first * gl.vstride);
  if (tp)
    gl.tptr = (const float *)((const u8 *)tp + first * gl.tstride);
  if (cp)
    gl.cptr = cp + first * gl.cstride;
  draw_polygon = mode == GL_POLYGON;
  draw_mode = mode;
  draw_tris(ix, k, count);
  draw_polygon = 0;
  draw_mode = 0;
  gl.vptr = vp, gl.tptr = tp, gl.cptr = cp;
}

void mxgl_MultiDraw(const GLenum *mode, const int *first, const int *count, int n) {
  int nv = 0;
  for (int i = 0; i < n; i++)
    if (first[i] + count[i] > nv)
      nv = first[i] + count[i];
  if (n <= 0 || nv < 3 || nv > IMM_MAX || !gl.va || !gl.vptr)
    return;
  multi.mode = mode, multi.first = first, multi.count = count, multi.n = n;
  int k;
  const u32 *ix = mode_ix(GL_TRIANGLES, nv, &k); /* for the LOD estimate */
  draw_tris(ix, k, nv);
  multi.n = 0;
}

static void draw_polygon_now(const float *v, int n, int stride, int st) {
  if (n < 3 || n > IMM_MAX)
    return;
  const float *vp = gl.vptr, *tp = gl.tptr;
  const u8 *cp = gl.cptr;
  int vs = gl.vsize, vst = gl.vstride, tst = gl.tstride;
  int va = gl.va, ta = gl.ta, ca = gl.ca;
  gl.vptr = v, gl.vsize = 3, gl.vstride = stride * 4, gl.va = 1;
  gl.tptr = v + st, gl.tstride = stride * 4, gl.ta = 1;
  gl.ca = 0;
  int k;
  const u32 *ix = mode_ix(GL_POLYGON, n, &k);
  draw_polygon = 1;
  draw_tris(ix, k, n);
  draw_polygon = 0;
  gl.vptr = vp, gl.tptr = tp, gl.cptr = cp;
  gl.vsize = vs, gl.vstride = vst, gl.tstride = tst;
  gl.va = va, gl.ta = ta, gl.ca = ca;
}

/* the queued polygons, with one matrix and state check (all queued with
   the same GL state): each transformed on VU0, culled by its area, as a
   strip, or cut when across a plane */
static void draw_polys(void) {
  int np = pq_n;
  pq_n = 0;
  if (!np)
    return;
  const float *vp0 = gl.vptr, *tp0 = gl.tptr;
  const u8 *cp0 = gl.cptr;
  int vs0 = gl.vsize, vst0 = gl.vstride, tst0 = gl.tstride;
  int va0 = gl.va, ta0 = gl.ta, ca0 = gl.ca;
  const int vs = pq_stride * 4;
  gl.vptr = pq[0].v, gl.vsize = 3, gl.vstride = vs, gl.va = 1;
  gl.tptr = pq[0].v + pq_st, gl.tstride = vs, gl.ta = 1;
  gl.ca = 0;
  u32 t0 = mx_count();
  frame_start();
  update_matrix();
  vp_cache = SPR_VP, vo_cache = SPR_VO;
  int k;
  const u32 *ix = mode_ix(GL_POLYGON, pq[0].n, &k);
  struct fastv fv;
  if (prepare_state(ix, k, pq[0].n) && fastv_get(&fv)) {
    for (int i = 0; i < np; i++) {
      const u8 *vb = (const u8 *)pq[i].v;
      int n = pq[i].n;
      /* the next polygon's vertices into the cache meanwhile (each is a
         block of its own: a miss a polygon) */
      if (i + 1 < np) {
        const u8 *nb = (const u8 *)pq[i + 1].v;
        u32 nbytes = (u32)(pq[i + 1].n * vs);
        for (u32 o = 0; o < nbytes + 63; o += 64)
          __asm__ volatile("pref 0, 0(%0)" ::"r"(nb + o));
      }
      u32 oand, oor = 0;
      st_cur.calls++;
      st_cur.verts += (u32)n;
      if (mats_ok) {
        oand = vu0_xform(vb, vs, n, vp_cache, vo_cache, mats, &oor);
        if (oor & 0x80) {
          oand = 0x3F, oor = 0;
          for (int j = 0; j < n; j++) {
            u32 o = vo_cache[j];
            if (o & 0x80) {
              o = 0;
              for (int q = 0; q < 6; q++)
                if (plane_d(vp_cache[j], q) < 0)
                  o |= 1u << q;
              vo_cache[j] = (u8)o;
            }
            oand &= o, oor |= o;
          }
        }
      } else {
        v4f mc[4];
        memcpy(mc, m, 64);
        oand = 0x3F;
        for (int j = 0; j < n; j++) {
          const float *x = (const float *)(vb + j * vs);
          *(v4f *)vp_cache[j] = xform(mc, x[0], x[1], x[2]);
          u32 o = 0;
          for (int q = 0; q < 6; q++)
            if (plane_d(vp_cache[j], q) < 0)
              o |= 1u << q;
          vo_cache[j] = (u8)o;
          oand &= o, oor |= o;
        }
      }
      if (oand)
        continue;
      st_poly((const float *)vb + pq_st);
      if (!oor) {
        if (gl.cull) {
          const float *pp[CLIP_MAX];
          for (int j = 0; j < n; j++)
            pp[j] = vp_cache[j];
          if (culled_poly(pp, n))
            continue;
        }
        if (!batch_room((u32)n))
          break;
        /* a strip of its vertices: 0, 1, n-1, 2, n-2, ... */
        fv.vb = vb;
        fast_put(&fv, 0, 1);
        for (int q = 1, lo = 1, hi = n - 1; q < n; q++)
          fast_put(&fv, (u32)((q & 1) ? lo++ : hi--), q < 2);
        st_cur.tris += (u32)n - 2;
      } else {
        u32 tc0 = mx_count();
        gl.vptr = (const float *)vb, gl.tptr = (const float *)vb + pq_st;
        struct cv poly[CLIP_MAX];
        for (int j = 0; j < n; j++)
          fetch(&poly[j], j, m);
        clip_poly(poly, n, oor);
        st_cur.clipped++;
        st_cur.c_clip += mx_count() - tc0;
      }
    }
  }
  st_shift = 0;
  st_cur.gl_cycles += mx_count() - t0;
  gl.vptr = vp0, gl.tptr = tp0, gl.cptr = cp0;
  gl.vsize = vs0, gl.vstride = vst0, gl.tstride = tst0;
  gl.va = va0, gl.ta = ta0, gl.ca = ca0;
}

/* the vertices' GS coordinates and outcodes, in vp_cache / vo_cache (n
   vertices from pos, stride bytes apart); returns the AND, *por the OR */
static u32 xform_all(const u8 *pos, int stride, int n, u32 *por) {
  u32 oand = 0x3F, oor = 0;
  if (mats_ok) {
    oand = vu0_xform(pos, stride, n, vp_cache, vo_cache, mats, &oor);
    if (oor & 0x80) {
      oand = 0x3F, oor = 0;
      for (int j = 0; j < n; j++) {
        u32 o = vo_cache[j];
        if (o & 0x80) {
          o = 0;
          for (int q = 0; q < 6; q++)
            if (plane_d(vp_cache[j], q) < 0)
              o |= 1u << q;
          vo_cache[j] = (u8)o;
        }
        oand &= o, oor |= o;
      }
    }
  } else {
    v4f mc[4];
    memcpy(mc, m, 64);
    for (int j = 0; j < n; j++) {
      const float *x = (const float *)(pos + j * stride);
      *(v4f *)vp_cache[j] = xform(mc, x[0], x[1], x[2]);
      u32 o = 0;
      for (int q = 0; q < 6; q++)
        if (plane_d(vp_cache[j], q) < 0)
          o |= 1u << q;
      vo_cache[j] = (u8)o;
      oand &= o, oor |= o;
    }
  }
  *por = oor;
  return oand;
}

void mxgl_DrawGlCmds(const int *cmds, const float *pos, int stride, const unsigned int *col, int nv) {
  pq_flush();
  if (nv < 3 || nv > IMM_MAX || dl_compiling)
    return;
  u32 t0 = mx_count();
  frame_start();
  update_matrix();
  st_cur.calls++;
  st_cur.verts += (u32)nv;
  /* the state, its LOD from the first primitive (as a draw of it alone) */
  static struct {
    float xyz[3], st[2];
    u32 c;
  } first[64];
  int c0 = cmds[0] < 0 ? -cmds[0] : cmds[0];
  if (c0 < 3)
    return;
  if (c0 > 64)
    c0 = 64;
  for (int i = 0; i < c0; i++) {
    const int *e = cmds + 1 + 3 * i;
    const float *x = (const float *)((const u8 *)pos + e[2] * stride);
    first[i].xyz[0] = x[0], first[i].xyz[1] = x[1], first[i].xyz[2] = x[2];
    memcpy(first[i].st, e, 8);
  }
  const float *vp0 = gl.vptr, *tp0 = gl.tptr;
  const u8 *cp0 = gl.cptr;
  int vs0 = gl.vsize, vst0 = gl.vstride, tst0 = gl.tstride, cst0 = gl.cstride;
  int va0 = gl.va, ta0 = gl.ta, ca0 = gl.ca;
  gl.vptr = first[0].xyz, gl.vsize = 3, gl.vstride = sizeof first[0], gl.va = 1;
  gl.tptr = first[0].st, gl.tstride = sizeof first[0], gl.ta = 1, gl.ca = 0;
  int k;
  const u32 *ix = mode_ix(cmds[0] < 0 ? GL_TRIANGLE_FAN : GL_TRIANGLE_STRIP, c0, &k);
  int ok = prepare_state(ix, k, c0);
  gl.vptr = vp0, gl.tptr = tp0, gl.cptr = cp0;
  gl.vsize = vs0, gl.vstride = vst0, gl.tstride = tst0, gl.cstride = cst0;
  gl.va = va0, gl.ta = ta0, gl.ca = ca0;
  if (!ok)
    return;
  /* the vertices once */
  if (nv <= SPR_VERTS) {
    vp_cache = SPR_VP, vo_cache = SPR_VO;
  } else {
    if ((u32)nv > v_cache_n) {
      mx_core_free(vp_ram);
      mx_core_free(vo_ram);
      v_cache_n = (u32)nv + 256;
      /* core memory: VU0 writes them with interrupts off */
      vp_ram = mx_core_alloc(v_cache_n * 16, 16);
      vo_ram = mx_core_alloc((v_cache_n + 15) & ~15u, 16);
    }
    vp_cache = vp_ram, vo_cache = vo_ram;
  }
  u32 oor;
  if (xform_all((const u8 *)pos, stride, nv, &oor))
    goto out;
  const int cmode = !gl.cull ? 0 : gl.cullface == GL_FRONT_AND_BACK ? 3 : gl.cullface == GL_FRONT ? 1 : 2;
  u32 drawn = 0;
  for (const int *c = cmds;;) {
    int cnt = *c++;
    if (!cnt)
      break;
    int fan = cnt < 0;
    if (fan)
      cnt = -cnt;
    const int *e = c;
    c += 3 * cnt;
    if (cnt < 3)
      continue;
    u32 pa = 0x3F, po = 0;
    for (int i = 0; i < cnt; i++)
      pa &= vo_cache[e[3 * i + 2]], po |= vo_cache[e[3 * i + 2]];
    if (pa)
      continue;
#define GV(i) ((u32)e[3 * (i) + 2])
#define PUT(i, adc)                                                                               \
  do {                                                                                            \
    o_put_pos(o_n, (const u8 *)pos + GV(i) * (u32)stride, (adc) ? 0x3F800000u : 0);             \
    o_put_st(o_n, &e[3 * (i)]);                                                                   \
    o_col[o_n] = col[GV(i)];                                                                      \
    o_n++;                                                                                        \
  } while (0)
    if (!po) {
      if (!batch_room((u32)(fan ? cnt + (cnt - 2) / 2 + 1 : cnt)))
        break;
      PUT(0, 1);
      PUT(1, 1);
      for (int t = 0; t + 2 < cnt; t++) {
        int ia = fan ? 0 : t + (t & 1), ib = fan ? t + 1 : t + 1 - (t & 1), ic = t + 2;
        int cl = 0;
        if (cmode) {
          const float *a = vp_cache[GV(ia)], *b = vp_cache[GV(ib)], *d = vp_cache[GV(ic)];
          float det = a[0] * (b[1] * d[3] - b[3] * d[1]) - a[1] * (b[0] * d[3] - b[3] * d[0]) +
                      a[3] * (b[0] * d[1] - b[1] * d[0]);
          cl = cmode == 3 || (cmode == 1 ? det < 0 : !(det < 0));
        }
        drawn += !cl;
        if (fan && (t & 1))
          PUT(0, 1);
        PUT(ic, cl);
      }
      continue;
    }
    /* across a plane: the triangles one by one */
    for (int t = 0; t + 2 < cnt; t++) {
      int ia = fan ? 0 : t + (t & 1), ib = fan ? t + 1 : t + 1 - (t & 1), ic = t + 2;
      u32 o0 = vo_cache[GV(ia)], o1 = vo_cache[GV(ib)], o2 = vo_cache[GV(ic)];
      if (o0 & o1 & o2)
        continue;
      if (gl.cull && !(o0 | o1 | o2) && culled(vp_cache[GV(ia)], vp_cache[GV(ib)], vp_cache[GV(ic)]))
        continue;
      struct cv v[3];
      const int tri[3] = {ia, ib, ic};
      for (int j = 0; j < 3; j++) {
        u32 g = GV(tri[j]);
        const float *x = (const float *)((const u8 *)pos + g * (u32)stride);
        memcpy(v[j].p, vp_cache[g], 16);
        v[j].x[0] = x[0], v[j].x[1] = x[1], v[j].x[2] = x[2];
        memcpy(v[j].st, &e[3 * tri[j]], 8);
        const u8 *cb = (const u8 *)&col[g];
        for (int q = 0; q < 4; q++)
          v[j].c[q] = cb[q];
      }
      if (!(o0 | o1 | o2)) {
        if (!batch_room(3))
          break;
        out_vertex(&v[0], 1), out_vertex(&v[1], 1), out_vertex(&v[2], 0);
        drawn++;
      } else {
        clip_tri(&v[0], &v[1], &v[2], o0 | o1 | o2);
        st_cur.clipped++;
      }
    }
#undef PUT
#undef GV
  }
  st_cur.tris += drawn;
out:
  st_cur.gl_cycles += mx_count() - t0;
}

void mxgl_DrawPolygon(const float *v, int n, int stride, int st) {
  if (n < 3)
    return;
  /* large ones (beyond the scratchpad's cache), and lists being made: now */
  if (n > CLIP_MAX - 6 || n > SPR_VERTS || dl_compiling) {
    pq_flush();
    draw_polygon_now(v, n, stride, st);
    return;
  }
  if (pq_n && (pq_stride != stride || pq_st != st || pq_n == PQ_MAX))
    draw_polys();
  pq[pq_n].v = v, pq[pq_n].n = n;
  pq_stride = stride, pq_st = st;
  pq_n++;
}

/* immediate mode */
static struct {
  GLenum mode;
  int n;
  /* a vertex's position, texture coordinates and colour together: three
     arrays of power-of-two sizes put the first vertices of each in the same
     cache sets (2 ways), and they took each other's lines */
  struct {
    float xyz[3];
    float st[2];
    u32 col;
  } v[IMM_MAX] MX_ALIGNED(64);
  float cur_st[2];
} imm;

void APIENTRY glBegin(GLenum mode) {
  imm.mode = mode;
  imm.n = 0;
}

static inline void imm_vertex(float x, float y, float z) {
  if (imm.n >= IMM_MAX)
    return;
  int i = imm.n++;
  imm.v[i].xyz[0] = x, imm.v[i].xyz[1] = y, imm.v[i].xyz[2] = z;
  imm.v[i].st[0] = imm.cur_st[0], imm.v[i].st[1] = imm.cur_st[1];
  imm.v[i].col = gl.col8;
}

void APIENTRY glEnd(void) {
  int n = imm.n;
  if (n < 3)
    return;
  int k;
  const u32 *ix = mode_ix(imm.mode, n, &k);
  if (!ix)
    return;
  /* draw from the immediate arrays */
  const float *vp = gl.vptr, *tp = gl.tptr;
  const u8 *cp = gl.cptr;
  int vs = gl.vsize, vst = gl.vstride, tst = gl.tstride, cst = gl.cstride;
  int va = gl.va, ta = gl.ta, ca = gl.ca;
  gl.vptr = imm.v[0].xyz, gl.vsize = 3, gl.vstride = sizeof imm.v[0], gl.va = 1;
  gl.tptr = imm.v[0].st, gl.tstride = sizeof imm.v[0], gl.ta = 1;
  gl.cptr = (const u8 *)&imm.v[0].col, gl.cstride = sizeof imm.v[0], gl.ca = 1;
  draw_polygon = imm.mode == GL_POLYGON;
  draw_mode = imm.mode;
  draw_tris(ix, k, n);
  draw_polygon = 0;
  draw_mode = 0;
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
/* 0..1 to 0..255 without branches (min.s, max.s) */
static inline u32 to_u8(float c) {
  return (u32)(s32)__builtin_fminf(__builtin_fmaxf(c * 255.0f, 0.0f), 255.0f);
}
void APIENTRY glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  /* the same as before (a model's vertices): nothing to convert */
  if (r == gl.color[0] && g == gl.color[1] && b == gl.color[2] && a == gl.color[3])
    return;
  pq_flush();
  gl.color[0] = r, gl.color[1] = g, gl.color[2] = b, gl.color[3] = a;
  gl.col8 = to_u8(r) | to_u8(g) << 8 | to_u8(b) << 16 | to_u8(a) << 24;
}
void APIENTRY glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(r, g, b, 1); }
void APIENTRY glColor4fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], v[3]); }
void APIENTRY glColor3fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], 1); }
void APIENTRY glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
  pq_flush();
  gl.color[0] = r * (1 / 255.0f), gl.color[1] = g * (1 / 255.0f);
  gl.color[2] = b * (1 / 255.0f), gl.color[3] = a * (1 / 255.0f);
  gl.col8 = (u32)r | (u32)g << 8 | (u32)b << 16 | (u32)a << 24;
}
void APIENTRY glColor3ub(GLubyte r, GLubyte g, GLubyte b) { glColor4ub(r, g, b, 255); }
void APIENTRY glColor3ubv(const GLubyte *v) { glColor4ub(v[0], v[1], v[2], 255); }
void APIENTRY glColor4ubv(const GLubyte *v) { glColor4ub(v[0], v[1], v[2], v[3]); }
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
  pq_flush();
  st_serial++;
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
  pq_flush();
  mat_serial++;
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
  pq_flush();
  gl.vp[0] = x, gl.vp[1] = y, gl.vp[2] = w, gl.vp[3] = h;
  mat_serial++;
}
void APIENTRY glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  pq_flush();
  st_serial++;
  gl.sc[0] = x, gl.sc[1] = y, gl.sc[2] = w, gl.sc[3] = h;
}
void APIENTRY glDepthRange(GLclampd n, GLclampd f) {
  pq_flush();
  gl.dn = (float)n, gl.df = (float)f;
  mat_serial++;
}

void APIENTRY glBlendFunc(GLenum s, GLenum d) { pq_flush(), gl.sf = s, gl.dfac = d, st_serial++; }
void APIENTRY glAlphaFunc(GLenum f, GLclampf r) { pq_flush(), gl.afunc = f, gl.aref = r, st_serial++; }
void APIENTRY glDepthFunc(GLenum f) { pq_flush(), gl.dfunc = f, st_serial++; }
void APIENTRY glDepthMask(GLboolean m) { pq_flush(), gl.dmask = m, st_serial++; }
void APIENTRY glCullFace(GLenum m) { pq_flush(), gl.cullface = m, st_serial++; }
void APIENTRY glTexEnvf(GLenum t, GLenum p, GLfloat v) {
  pq_flush();
  st_serial++;
  if (p == GL_TEXTURE_ENV_MODE && gl.cunit == 0)
    gl.texenv = (GLenum)v;
}
void APIENTRY glTexEnvi(GLenum t, GLenum p, GLint v) { glTexEnvf(t, p, (GLfloat)v); }
void APIENTRY glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
  gl.clear[0] = r, gl.clear[1] = g, gl.clear[2] = b, gl.clear[3] = a;
}

void APIENTRY glClear(GLbitfield mask) {
  pq_flush();
  int was = in_frame;
  frame_start();
  flush_out();
  if (!was && (mask & GL_COLOR_BUFFER_BIT))
    return; /* mx_frame_begin cleared colour and Z */
  if (!(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)))
    return;
  if (hires) {
  /* a sprite over the scissor area (or all) in the passes' window: Z 0
     with context 1, without (ZMSK) with context 2; colour, or the
     destination's colour again (blend Cd) when only Z is cleared */
  int x0 = 0, x1 = fb_w, y0 = 0, y1 = fb_h;
  if (gl.scissor) {
    x0 = gl.sc[0] * fb_w / gl_w, x1 = (gl.sc[0] + gl.sc[2]) * fb_w / gl_w;
    y0 = (gl_h - (gl.sc[1] + gl.sc[3])) * fb_h / gl_h, y1 = (gl_h - gl.sc[1]) * fb_h / gl_h;
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
  } else {
  /* a sprite over the scissor area (or all): Z 0, colour if asked */
  const struct mx_rt *t = &rt[buf];
  int x0 = 0, x1 = fb_w, y0 = 0, y1 = fb_h;
  if (gl.scissor) {
    x0 = gl.sc[0] * fb_w / gl_w, x1 = (gl.sc[0] + gl.sc[2]) * fb_w / gl_w;
    y0 = (gl_h - (gl.sc[1] + gl.sc[3])) * fb_h / gl_h, y1 = (gl_h - gl.sc[1]) * fb_h / gl_h;
  }
  u64 *c = mx_frame_reserve(2 + 9);
  c = mx_e_cnt(c, 9, mx_vif_flush(), mx_vif_direct(9));
  c = mx_e_ad_tag(c, 8, 1);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_ZBUF_1, mx_gs_zbuf(t->zbp, t->zpsm, !(mask & GL_DEPTH_BUFFER_BIT)));
  c = mx_e_ad(c, MX_GS_REG_FRAME_1,
              mx_gs_frame(t->fbp, t->fbw, t->psm, (mask & GL_COLOR_BUFFER_BIT) ? 0 : ~0u));
  c = mx_e_ad(c, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, fb_w - 1, 0, fb_h - 1));
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
  }
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
        texs[k].mip = 2; /* GL's default NEAREST_MIPMAP_LINEAR (linear here) */
        t[i] = (GLuint)k;
        break;
      }
  }
}

/* data still read by frames in flight is freed two frames later */
#define DEFER_MAX 256
/* hires: a record is played until a later one replaces it, which may be
   MX_HIRES_SLOTS frames later */
#define NDEFER (MX_HIRES_SLOTS + 1)
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
    mx_core_free(deferred[k][i]);
  ndeferred[k] = 0;
}

/* the lists name textures: when textures go, so do the lists (the renderer
   asks, mxgl_ListsDirty, and makes them again) */
int mxgl_ListsDirty(void) { return dl_dirty; }

void APIENTRY glDeleteTextures(GLsizei n, const GLuint *t) {
  pq_flush();
  tex_serial++;
  dl_dirty = 1;
  for (int i = 0; i < n; i++)
    if (t[i] && t[i] < MAX_TEX) {
      defer_free(texs[t[i]].data);
      texs[t[i]].data = NULL;
      tex_drop_twin(&texs[t[i]]);
      texs[t[i]].used = 0;
    }
}
void APIENTRY glBindTexture(GLenum target, GLuint t) {
  if (gl.cunit != 0)
    return;
  if (gl.bound != t) {
    pq_flush();
    st_serial++;
  }
  gl.bound = t;
  /* a name not from glGenTextures is made by binding it (GL 1.1; GLQuake
     numbers its textures itself) */
  if (t && t < MAX_TEX && !texs[t].used) {
    memset(&texs[t], 0, sizeof texs[t]);
    texs[t].used = 1;
    texs[t].lin_mag = texs[t].lin_min = 1;
    texs[t].mip = 2;
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
      if (t->pal) {
        dst[at] = pal_nearest(s);
      } else if (t->psm == MX_GS_PSM_T8) {
        u8 m = s[0] > s[1] ? s[0] : s[1];
        dst[at] = m > s[2] ? m : s[2];
      } else if (t->psm == MX_GS_PSM_CT32) {
        u32 a = (s[3] + 1u) >> 1;
        ((u32 *)dst)[at] = (u32)s[0] | (u32)s[1] << 8 | (u32)s[2] << 16 | a << 24;
      } else {
        ((u16 *)dst)[at] = (u16)(s[0] >> 3 | (s[1] >> 3) << 5 | (s[2] >> 3) << 10 | (s[3] >= 128 ? 0x8000 : 0));
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
void mxgl_ColorShift(int s) {
  if (gl.cshift != s) {
    pq_flush();
    st_serial++;
  }
  gl.cshift = s;
}

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

static void tex_image_2d(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                         GLint border, GLenum format, GLenum type, const GLvoid *px);
static void tex_set_image(struct tex *t, GLint ifmt, GLsizei w, GLsizei h, const GLvoid *px);

static void tex_drop_twin(struct tex *t) {
  if (!t->twin)
    return;
  struct tex *c = &twins[t->twin - 1];
  defer_free(c->data);
  memset(c, 0, sizeof *c);
  t->twin = 0;
}

/* t's brightness copy (T8, the largest of R, G and B; for blendFunc
   products, blend_product), made from its level 0 when first asked for;
   NULL if there is no room */
static struct tex *tex_twin(struct tex *t) {
  if (!t->twin) {
    int k = 0;
    while (k < MAX_TWIN && twins[k].used)
      k++;
    if (k == MAX_TWIN)
      return NULL;
    u8 *rgba = malloc(t->w * t->h * 4);
    if (!rgba)
      return NULL;
    const u8 *src = (const u8 *)t->data + t->off[0];
    for (u32 i = 0; i < t->w * t->h; i++) {
      u8 *o = rgba + i * 4;
      if (t->pal) {
        o[0] = pal_rgb[src[i]][0], o[1] = pal_rgb[src[i]][1], o[2] = pal_rgb[src[i]][2];
      } else if (t->psm == MX_GS_PSM_CT32) {
        o[0] = src[i * 4], o[1] = src[i * 4 + 1], o[2] = src[i * 4 + 2];
      } else {
        u32 c = ((const u16 *)src)[i];
        o[0] = (u8)((c & 31) << 3), o[1] = (u8)((c >> 5 & 31) << 3), o[2] = (u8)((c >> 10 & 31) << 3);
      }
      o[3] = 255;
    }
    struct tex *c = &twins[k];
    memset(c, 0, sizeof *c);
    c->used = 1;
    tex_brightness = 1;
    tex_set_image(c, GL_RGB, (GLsizei)t->w, (GLsizei)t->h, rgba);
    free(rgba);
    if (!c->data) {
      c->used = 0;
      return NULL;
    }
    t->twin = k + 1;
  }
  struct tex *c = &twins[t->twin - 1];
  c->clamp_s = t->clamp_s, c->clamp_t = t->clamp_t;
  c->lin_mag = t->lin_mag, c->lin_min = t->lin_min, c->mip = t->mip;
  return c;
}
void APIENTRY glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                           GLint border, GLenum format, GLenum type, const GLvoid *px) {
  pq_flush();
  tex_serial++;
  tex_image_2d(target, level, ifmt, w, h, border, format, type, px);
}
static void tex_image_2d(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
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
  tex_drop_twin(t);
  tex_set_image(t, ifmt, w, h, px);
}

/* t's image, w x h RGBA8 texels s (NULL: zeros), at least 8 x 8 */
static void tex_set_image(struct tex *t, GLint ifmt, GLsizei w, GLsizei h, const GLvoid *px) {
  /* alpha in the texels? Only 0 and 255 (a colour key: Descent 3's 1555)
     fits 16 bits (the alpha bit, TEXA); other alpha takes 32 */
  int alpha = 0, alpha8 = 0;
  const u8 *s = px;
  if (s && ifmt != GL_RGB && ifmt != GL_RGB8 && ifmt != GL_RGB5 && ifmt != 3)
    for (int i = 0; i < w * h; i++)
      if (s[i * 4 + 3] != 255) {
        alpha = 1;
        if (s[i * 4 + 3] != 0) {
          alpha8 = 1;
          break;
        }
      }
  defer_free(t->data);
  t->w = (u32)w, t->h = (u32)h;
  t->lw = ilog2((u32)w), t->lh = ilog2((u32)h);
  t->alpha = alpha;
  /* opaque texels all of the game's palette: its indices (T8, exact), else
     16 bits */
  int pal = 0;
  if (!tex_brightness && !alpha && pal_on && s) {
    pal = 1;
    for (int i = 0; i < w * h && pal; i++)
      if (pal_find((u32)s[i * 4] | (u32)s[i * 4 + 1] << 8 | (u32)s[i * 4 + 2] << 16) < 0)
        pal = 0;
  }
  t->bright = tex_brightness;
  t->pal = pal;
  t->psm = tex_brightness || pal ? MX_GS_PSM_T8 : alpha8 ? MX_GS_PSM_CT32 : MX_GS_PSM_CT16;
  tex_brightness = 0;
  t->max_levels = mx_tex_max_levels(t->lw, t->lh, 8);
  t->levels = 1;
  t->bytes = 0;
  for (u32 lv = 0; lv < t->max_levels; lv++) {
    t->off[lv] = t->bytes;
    t->bytes += (((t->w >> lv) * (t->h >> lv) * tex_bpp(t)) + 63) & ~63u;
  }
  t->data = mx_core_alloc((t->bytes + 63) & ~63u, 64); /* IMAGE reads it by DMA */
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
  pq_flush();
  if (level != 0 || !gl.bound || gl.bound >= MAX_TEX)
    return;
  struct tex *t = &texs[gl.bound];
  if (!t->data)
    return;
  tex_drop_twin(t);
  u8 *rgba = to_rgba(format, w, h, px);
  if (rgba) {
    glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, type, rgba);
    free(rgba);
    return;
  }
  /* changed in place, level 0 only: an upload of a frame in flight may take
     the new texels, a frame early (a copy each time: Quake's lightmaps
     change every frame) */
  t->levels = 1;
  convert(t, 0, x, y, w, h, px);
  mx_dcache_writeback(t->data, t->bytes);
  t->dirty = 1;
}

void APIENTRY glTexParameterf(GLenum target, GLenum p, GLfloat v) {
  pq_flush();
  tex_serial++;
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


static int mode_set = -1; /* mxgl_mode: none yet */

/* the frame pipeline in SD: NTSC or PAL interlaced FIELD mode, two CT16S
   buffers and a Z24 */
static int start_sd(int pal) {
  if (mx_gs_init_video(pal ? &mx_video_pal : &mx_video_ntsc) < 0)
    return -1;
  fb_w = 640, fb_h = pal ? 512 : 448;
  u32 zbp = mx_vram_alloc(mx_rt_pages(fb_w, fb_h, MX_GS_PSM_Z24));
  for (int j = 0; j < 2; j++) {
    u32 fbp = mx_vram_alloc(mx_rt_pages(fb_w, fb_h, MX_GS_PSM_CT16S));
    mx_rt_init(&rt[j], fbp, fb_w, fb_h, MX_GS_PSM_CT16S, zbp, MX_GS_PSM_Z24);
    mx_display_init(&disp[j], &rt[j]);
  }
  mx_gs_displays(disp, 2);
  mx_vu_reset(2);
  /* chain mode: the stream calls the display lists' chains */
  mx_frame_init_chain(ring, sizeof ring, (void *)0x70002000, 0x2000);
  /* frames as fast as they are made (the screen tears): the real rate */
  mx_frame_vsync(0);
  hires = 0;
  zmax = ZMAX_SD;
  return 0;
}

/* hires: the records of the frame stream (each played every field until
   the next replaces it) and arenas 2, 3, taken while it is on */
static int start_hires(void) {
  if (!hires_mem)
    hires_mem = mx_core_alloc(REC_BYTES + 2 * ARENA_BYTES, 64);
  if (!hires_mem)
    return -1;
  mx_vu_reset(2);
  if (mx_hires_init(&mx_video_1080i, 1920, 540, MX_GS_PSM_CT16S, hires_mem, REC_BYTES,
                    (void *)0x70002000, 0x2000) < 0)
    return -1;
  fb_w = 1920, fb_h = 540;
  for (int i = 2; i < NARENA; i++) {
    arena[i] = hires_mem + REC_BYTES + (u32)(i - 2) * ARENA_BYTES;
    mx_dcache_invalidate(arena[i], ARENA_BYTES);
  }
  hires = 1;
  zmax = ZMAX_HIRES;
  return 0;
}

static void hires_free(void) {
  mx_core_free(hires_mem);
  hires_mem = NULL;
  arena[2] = arena[3] = NULL;
}

int mxgl_set_mode(int mode) {
  if (mode == MXGL_SD)
    mode = mx_video_default() == &mx_video_pal ? MXGL_PAL : MXGL_NTSC;
  if (mode == mode_set)
    return 0;
  if (mode_set >= 0) {
    /* the frames sent are shown (SD) or the passes stopped (hires), then
       the pipeline starts again in the new mode */
    pq_flush();
    if (in_frame)
      mxgl_end_frame();
    u32 t0 = mx_count();
    while (!hires && mx_gs.shown != mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
      ;
    mx_frame_stop();
    /* no frame reads anything now: what was freed goes (the room for
       1080i, after a game freed its textures for a mode change) */
    for (int k = 0; k < NDEFER; k++) {
      for (int i = 0; i < ndeferred[k]; i++)
        mx_core_free(deferred[k][i]);
      ndeferred[k] = 0;
    }
  }
  int r = 0;
  if (mode == MXGL_1080I) {
    if (start_hires() < 0) {
      mx_printf("mxgl: no 1080i (memory %p)\n", hires_mem);
      hires_free();
      r = -1;
      mode = mode_set == MXGL_PAL ? MXGL_PAL : MXGL_NTSC;
    }
  }
  if (mode != MXGL_1080I) {
    hires_free();
    start_sd(mode == MXGL_PAL);
  }
  mode_set = mode;
  frame_par = 0;
  mx_printf("mxgl: %s %ux%u\n", mode == MXGL_1080I ? "1080i" : mode == MXGL_PAL ? "PAL" : "NTSC", fb_w,
            fb_h);
  /* the HUD's font (256 x 8) in the first page after the buffers, the
     CLUTs after it, then the textures: all copied in again */
  mx_gs.w = (u32)fb_w, mx_gs.h = (u32)fb_h; /* the HUD's coordinates */
  mx_hud_init(mx_gs.tbp, (u32)fb_h);
  clut_cbp = mx_gs.tbp + mx_tex_blocks(256, 8, MX_GS_PSM_CT32);
  vram_lo = vram_pos = clut_cbp + 4 * (NCLUT + 1); /* and the palette's */
  vram_gen++;
  gs.valid = 0, gs_serial++;
  /* the lists' draws have the state of the mode they were made in */
  dl_dirty = 1;
  mxgl_window(fb_w, hires ? 1080 : fb_h);
  return r;
}

int mxgl_mode(void) { return mode_set; }



void mxgl_screen(int *w, int *h) {
  *w = fb_w;
  *h = hires ? 1080 : fb_h;
}

void mxgl_init(void) {
  static int done;
  if (done)
    return;
  done = 1;
  shader = MX_VU_SHADER(MXGLV, mxglv_vu, mxglv_vu_end);
  for (int i = 0; i < 2; i++) {
    arena[i] = mx_core_alloc(ARENA_BYTES, 64);
    /* written through UCAB only: no line of them may be in the cache */
    if (arena[i])
      mx_dcache_invalidate(arena[i], ARENA_BYTES);
  }
  if (!arena[1])
    mx_printf("mxgl: no memory for the arenas (2 x %u KB)\n", ARENA_BYTES / 1024);
  clut_make();
  mxgl_set_mode(MXGL_SD);
  memset(&gl, 0, sizeof gl);
  for (int i = 0; i < 8; i++)
    mat_ident(gl.mv[i]);
  for (int i = 0; i < 4; i++)
    mat_ident(gl.pr[i]);
  gl.mode = GL_MODELVIEW;
  gl.vp[2] = gl_w, gl.vp[3] = gl_h; /* the window of mxgl_set_mode */
  gl.df = 1;
  gl.sf = GL_ONE, gl.dfac = GL_ZERO;
  gl.dfunc = GL_LESS, gl.dmask = 1, gl.cullface = GL_BACK, gl.afunc = GL_ALWAYS;
  gl.texenv = GL_MODULATE;
  gl.color[0] = gl.color[1] = gl.color[2] = gl.color[3] = 1;
  gl.col8 = 0xFFFFFFFFu;

}

void mxgl_window(int w, int h) {
  pq_flush();
  gl_w = w, gl_h = h;
  gl.vp[2] = w, gl.vp[3] = h;
  mat_serial++;
}

void mxgl_frame_done(void) {
  defer_collect();
  if (dropped_draws) {
    mx_printf("mxgl: %u draws dropped (arena full)\n", dropped_draws);
    dropped_draws = 0;
  }
}

int mxgl_screenshot(const char *path) {
  /* hires: both fields, 1920 x 1080 */
  if (hires)
    return mx_hires_save_tga(path);
  /* the last frame on the screen, then the pipeline stopped (as the
     tutorials' tut_screenshot); its CT16S buffer as a 24-bit TGA */
  u32 t0 = mx_count();
  while (mx_gs.shown != mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
    ;
  mx_frame_stop();
  const struct mx_rt *t = &rt[(mx_gs.frames - 1) & 1];
  static u16 line[640] MX_ALIGNED(64);
  static u8 out[640 * 3];
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0)
    return -1;
  u8 hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (u8)fb_w, (u8)(fb_w >> 8), (u8)fb_h, (u8)(fb_h >> 8),
                24, 0x20};
  mx_fio_write(fd, hdr, sizeof hdr);
  for (u32 y = 0; y < (u32)fb_h; y++) {
    if (mx_gs_download(line, MX_GS_PAGE(t->fbp), t->fbw, t->psm, 0, y, (u32)fb_w, 1) < 0)
      break;
    for (u32 x = 0; x < (u32)fb_w; x++) {
      u16 p = line[x];
      out[x * 3 + 0] = (u8)(((p >> 10) & 31) << 3);
      out[x * 3 + 1] = (u8)(((p >> 5) & 31) << 3);
      out[x * 3 + 2] = (u8)((p & 31) << 3);
    }
    mx_fio_write(fd, out, (u32)fb_w * 3);
  }
  mx_fio_close(fd);
  return 0;
}
