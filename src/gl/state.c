/* gl/state.c, a part of mxgl.c (included there, not compiled alone): the library's state: the
   frame's arenas and counters, the textures, the GL state, vectors and matrices */

/* ---- frame stream, arenas ---------------------------------------------------- */

static int tex_bias, tex_bias_auto; /* mxgl_texture_quality */
/* MXGL_TEX_AUTO's state (tex_auto, below) */
#define NEED_HOLD 60
static int tq_hi = 120, tq_lo = 80, tq_swap = 30;
static int v_low, ram_cold;
static u32 v_epoch = 1;
#define WS_MAX 1024
static struct tex *ws[2][WS_MAX];
static u32 ws_n[2], ws_cur;
static u32 ws_w0;              /* W of the frame being made, at full quality */
static u32 ws_sum, ws_frames;  /* this second's */
static u32 ws_last_w0, ws_last_e; /* the last second's average W, and its estimate placed */
static u32 ws_far[3];             /* the last frame's mipped textures by need (0, 1, 2+) */

#define RING_BYTES (64 * 1024) /* the stream's slots (chain mode) */
/* the frame chain's ring (DMA): core also when libgl is VM (mx OS plan 4b) */
static u8 ring[RING_BYTES] MX_ALIGNED(64) MX_CORE_BSS;
/* the batches' vertices of a frame (Quake: up to ~230 KB); a game may set
   another size (MXGL_ARENA_BYTES: Descent 3 takes 256 KB) */
#ifdef MXGL_ARENA_BYTES
#define ARENA_BYTES (MXGL_ARENA_BYTES)
#else
#define ARENA_BYTES (512 * 1024)
#endif
/* beam racing: the records of the frame stream, each its main part (three
   quarters) and its uploads' (Quake 1, 2: up to ~60 KB a frame; Quake 3
   ~200 KB: MXGL_REC_BYTES 512 KB) */
#ifdef MXGL_REC_BYTES
#define REC_BYTES (MX_HIRES_SLOTS * (MXGL_REC_BYTES))
#else
#define REC_BYTES (MX_HIRES_SLOTS * 256 * 1024)
#endif
/* arenas 0 and 1 are always there; beam racing takes the records and arenas 2,
   3 in one block (hires_mem) while it is on */
static u8 *arena[NARENA];
static u8 *hires_mem;
/* mxgl_dither's matrices */
static u64 dither_m[16];
static u32 dither_n, dither_frame;
/* libgl's memory (<mx/mem.h>): pinned (the DMAC or VU0 reads it, VU0 with
   interrupts off): gl_heap its objects (textures, lists, buffers, tables),
   gl_stream the arenas and beam racing's records */
static struct mx_heap *gl_heap, *gl_stream;
/* with VM: the textures' upper levels, pageable (<mx/vcache.h>,
   struct mx_tex_mem); the tails stay in gl_heap */
static struct mx_heap *gl_assets;
#define GL_ASSET_BYTES (32u << 20) /* of the heap window (mx_vm_config.heap_size) */
static void *gl_alloc(u32 bytes, u32 align) { return mx_heap_alloc(gl_heap, bytes, align); }
static void gl_free(void *p) { mx_heap_free(mx_is_vm(p) ? gl_assets : gl_heap, p); }
static u32 arena_pos;
static int in_frame, buf, frame_par;
static struct mx_shader shader;
/* vertex programs (glUseProgram; NULL: libgl's own, shader): the one the
   batch is drawn with, the one in use; the program the stream has set up */
struct vprog;
static struct vprog *o_prog, *cur_prog;
static int cur_prog_clips; /* cur_prog has mxglc.i's clipping */
static struct vprog *fog_prog[2]; /* GL_LINEAR; GL_EXP and GL_EXP2 */
static void fog_program(int i, const struct mx_shader *s);
static const struct mx_shader *vu_cur;
static const struct mx_shader *vprog_use(struct vprog *p);
static const void *vprog_uni(struct vprog *p, const float (*fixed)[4]);
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
/* MXGL_STATS_WINDOW: every so many frames, one line of the units' busy shares
   (per 1000, of mx_trace_sample) and the cycles and counts of a frame */
#ifdef MXGL_STATS_WINDOW
#include <mx/trace.h>
static void stats_window(void) {
  static u64 sum[sizeof(struct mxgl_stats) / 4], cyc;
  static u32 n, last, started, b0[MX_TS_N], n0, wb0, wr0, qw0, win;
  u32 now = mx_count();
  if (!started) {
    started = 1, last = now;
    mx_trace_sample(1000);
    return;
  }
  cyc += now - last, last = now;
  const u32 *a = (const u32 *)&st_cur;
  for (u32 i = 0; i < sizeof st_cur / 4; i++)
    sum[i] += a[i];
  if (++n < MXGL_STATS_WINDOW)
    return;
  u32 b[MX_TS_N], ns = mx_trace_busy(b), d = ns - n0 ? ns - n0 : 1;
#define PM(i) ((b[i] - b0[i]) * 1000 / d)
#define AV(f) (u32)(sum[__builtin_offsetof(struct mxgl_stats, f) / 4] / n)
  u32 wb = mx_frame_stats.wait_buffer, wr = mx_frame_stats.wait_ring, q = mx_frame_stats.qw;
  mx_printf("EXP %u cyc %u VIF1 %u VU1 %u GW %u PATH %u GS %u VU0 %u wb %u wr %u qw %u | calls %u "
            "draws %u tris %u verts %u clip %u up %u gl %u xf %u tri %u fl %u set %u lod %u mvp %u\n",
            win++, (u32)(cyc / n), PM(1), PM(6), PM(9), PM(7), PM(8), PM(5), (wb - wb0) / n,
            (wr - wr0) / n, (q - qw0) / n, AV(calls), AV(draws), AV(tris), AV(verts), AV(clipped),
            AV(upload_kb), AV(gl_cycles), AV(xf_cycles), AV(tri_cycles), AV(flush_cycles),
            AV(c_set), AV(c_lod), AV(c_mvp));
#undef PM
#undef AV
  memcpy(b0, b, sizeof b), n0 = ns, wb0 = wb, wr0 = wr, qw0 = q;
  memset(sum, 0, sizeof sum), cyc = 0, n = 0;
}
#endif
int mxgl_show_stats = 0;

/* ---- textures ---------------------------------------------------------------- */

/* the texture names (glGenTextures gives 1 .. MAX_TEX - 1): a program with
   few textures takes less static memory (MXGL_MAX_TEXTURES in mk/mxgl.mk;
   a struct tex is about 200 bytes) */
#ifdef MXGL_MAX_TEXTURES
#define MAX_TEX (MXGL_MAX_TEXTURES)
#else
#define MAX_TEX 2048
#endif
/* A texture and its MIPMAP levels, down to 8 x 8 (bilinear filters are
   wrong below; also whole quadwords for IMAGE), MX_TEX_LEVELS at most. The
   engine sends the levels (glTexImage2D level n); levels has those that
   made here from level 0 (box filter) for every texture, whether the engine
   asked for levels or not (its level n calls are ignored). */
struct tex {
  u32 w, h;              /* level 0's size */
  u8 lw, lh;             /* its log2 */
  u8 used;
  u8 psm;
  u8 max_levels;         /* the levels it can have */
  u8 levels;             /* the levels that came (1 .. max_levels) */
  u8 alpha;
  u8 alpha_fmt;          /* its format has alpha (REPLACE takes the texture's) */
  u8 bright;             /* T8 brightness texture (CLUT_2X/1X/INV) */
  u8 grey;               /* bright, from RGBA texels all grey and opaque (Quakespasm's
                            lightmaps): CLUT_GREY when not in a product; 2: its black
                            texels all transparent (a font), CLUT_GREYK */
  u8 planar;             /* mxgl_color_light's GL_RGB8 texture without MIPMAP (a
                            lightmap): its texels the red channel's brightness,
                            twin[2], twin[3] the green and blue ones; no levels */
  u8 pal;                /* T8 indices of the shared palette */
  u8 clamp_s, clamp_t;
  u8 lin_mag, lin_min;   /* filters: 1 linear */
  u8 mip;                /* MIN_FILTER: 0 no levels, 1 nearest level, 2 both levels */
  u8 dirty;              /* changed (glTexSubImage2D) while in GS memory: upload it again */
  u16 twin[6];           /* its brightness copy (0) and channel copies (1 R, 2 G, 3 B),
                            and for GL_BLEND and GL_ADD (draw_env) its alpha with white
                            (TWIN_ENV_A) and its brightness times its alpha (TWIN_ENV_B):
                            twins[twin - 1], or 0 */
  u16 dy0, dy1;          /* the lines of level 0 changed since: dy0 .. dy1 - 1 */
  struct mx_tex_mem m;   /* its levels in GS format (m.level[], 64-byte aligned): the
                            upper ones pageable (m.head), the others pinned */
  u32 bytes;             /* all levels */
  void *data;            /* its texels are there (m.tail_mem), or NULL */
  struct mx_tex mt;      /* the levels in GS memory (mx_vcache: valid until a lookup misses) */
  u32 gen;               /* its texels' number: with the struct's address the cache's key */
  u8 need_lvl;           /* the finest level its draws sampled lately (LOD), held
                            NEED_HOLD frames before it may get coarser */
  u8 vfrom;              /* the level the VRAM control places it from (vfrom_epoch) */
  u32 need_until, ws_frame, vfrom_epoch;
  u32 vc_frame, vc_gen;  /* the frame and vram_gen of its last lookup (once a frame
                            is enough while nothing was evicted since) */
  u32 *oclut;            /* T8 with its own palette (glCompressedTexImage2D): 256
                            CT32 in CSM1's order (m.clut, pinned); after the levels
                            in GS memory */
  u8 (*ipal)[4];         /* glColorTableEXT(GL_TEXTURE_2D): the palette of its
                            GL_COLOR_INDEX texels (RGBA, malloc'ed) */
};
static struct tex texs[MAX_TEX];
/* brightness copies of colour textures, made when a draw multiplies with
   the destination (tex_twin) */
#define MAX_TWIN (MAX_TEX / 4 > 8 ? MAX_TEX / 4 : 8) /* a lightmap page has 3 with mxgl_color_light */
enum { TWIN_ENV_A = 4, TWIN_ENV_B = 5, NTWIN = 6 };
static struct tex twins[MAX_TWIN];
static void tex_drop_twin(struct tex *t);
static void defer_free(void *p);
static u32 dl_pool_pos;
static int dl_nused, dl_nfailed; /* lists made; of the glNewLists since, those that did not fit */
static const void *buf_ptr(GLuint b, uintptr_t off);
/* GS memory: the screen, the HUD's font and the CLUTs below vram_lo (64-word
   blocks), the textures in the pages after, as maxsdk's cache
   (<mx/vcache.h>) places them. vram_gen is made again when a lookup
   misses (places of other textures may have gone): the states kept
   (prepare_state, the lists' calls) are made again then. */
static u32 vram_lo, vram_gen = 1, tex_gens;
static u32 h_blocks; /* blocks of top bytes the cache has (mx_vcache_hpages) */
/* GS memory's room for textures, bytes of texels: a top byte's block
   holds 64 */
static u32 tex_room(void) { return (MX_VRAM_BLOCKS - vram_lo) * 256 + h_blocks * 64; }
static u64 tex_key(const struct tex *t) { return (u64)t->gen << 32 | (u32)(uintptr_t)t; }
/* a level's texels */
#define TLV(t, lv) ((u8 *)(t)->m.level[lv])
/* t's entries in the cache go: whole and from its tail (if the GS may
   still read one, it goes as the cache needs room) */
static void tex_drop(struct tex *t) {
  for (u32 f = 0; f < 3; f++)
    mx_vcache_drop(mx_vcache_tex_key(tex_key(t), f));
}
/* its texels freed once no frame reads them */
static void defer_free(void *p);
static void tex_free_mem(struct tex *t) {
  defer_free(t->m.head);
  defer_free(t->m.tail_mem);
  t->m = (struct mx_tex_mem){0};
  t->data = NULL, t->oclut = NULL;
}
/* memory for its levels (max_levels, of psm, lw x lh), and a CLUT: the
   upper levels pageable when it has 3 or more (not brightness textures:
   lightmaps change every frame) */
static int tex_alloc_mem(struct tex *t, u32 clut_bytes) {
#ifdef MXGL_NO_HEADS /* debugging: every texture pinned whole */
  int split = 0;
#else
  int split = gl_assets && !t->bright && t->max_levels >= 3;
#endif
  t->bytes = 0;
  for (u32 lv = 0; lv < t->max_levels; lv++)
    t->bytes += mx_tex_level_bytes(t->psm, t->lw, t->lh, lv);
  if (mx_tex_mem_alloc(&t->m, t->psm, t->lw, t->lh, t->max_levels, clut_bytes, gl_heap,
                       split ? gl_assets : 0) < 0) {
    t->data = NULL;
    return -1;
  }
  t->data = t->m.tail_mem;
  t->oclut = clut_bytes ? t->m.clut : NULL;
  return 0;
}
/* its texels written: out of the D-cache for the DMAC */
static void tex_writeback(struct tex *t) {
  if (t->m.head)
    mx_dcache_writeback(t->m.head, t->m.head_bytes);
  mx_dcache_writeback(t->m.tail_mem, t->m.tail_bytes + (t->oclut ? 1024u : 0));
}
/* t gets new texels: the cache's entry of the old ones goes */
static void tex_new_gen(struct tex *t) {
  if (t->gen)
    tex_drop(t);
  t->gen = ++tex_gens;
  t->vc_gen = 0; /* looked up again */
}

/* ---- GL state ------------------------------------------------------------- */

#define MV_DEPTH 32 /* GL's minimum */
#define PR_DEPTH 4
#define TM_DEPTH 4
static struct {
  float mv[MV_DEPTH][16], pr[PR_DEPTH][16], tm[TM_DEPTH][16];
  int mv_top, pr_top, tm_top;
  GLenum mode;
  /* glPolygonOffset (GL_POLYGON_OFFSET_FILL) */
  int poffset;
  float pofactor, pounits;
  /* glColorMask as FRAME's FBMSK (bits of the colour before it is made 16 bits) */
  u32 cmask;
  /* the stencil: one bit, the frame buffer's alpha bit (CT16S), tested with TEST's
     DATE and DATM. Bit 0 is the value st_va (the last clear), bit 1 st_vb, the first
     other value a draw wrote (-1: none yet); a third value is taken for st_vb.
     GL's stencil and depth-fail operations are not done (the GS writes no pixel
     that fails) */
  int st_on, st_ref, st_va, st_vb;
  u32 st_mask, st_wmask, st_clear;
  GLenum st_func, st_op;
  /* GL_CLIP_PLANE0: the plane in eye space (glClipPlane) */
  int ucp_on;
  float ucp_eye[4];
  int pack_align;
  GLenum err; /* glGetError */
  int vp[4], sc[4];
  float dn, df;
  int tex2d, blend, atest, dtest, cull, scissor;
  /* glFog's state; a, b of the fog programs (fog_update); the GS's FOGCOL */
  int fog;
  GLenum fog_mode;
  float fog_density, fog_start, fog_end, fog_a, fog_b;
  u32 fog_col;
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
  int cunit, ccunit; /* the active texture unit (glActiveTexture), the client's */
  /* texture unit 1 (GL_ARB_multitexture): drawn as a second pass that
     multiplies the first (draw_tris) */
  struct {
    int tex2d, cshift;
    GLuint bound;
    GLenum texenv;
    const float *tptr;
    int tstride, ta;
  } u1;
  int shared_pal; /* GL_SHARED_TEXTURE_PALETTE_EXT */
  /* buffer objects (GL 1.5): the bound ones, and each array's (0: client
     memory) with its offset; the pointers above are made from them */
  GLuint abuf, ebuf;
  GLuint vbuf, tbuf, cbuf, u1buf;
  uintptr_t voff, toff, coff, u1off;
} gl;
/* the texture of texture calls (glTexImage2D, glTexParameter): the active unit's */
#define TBOUND (gl.cunit ? gl.u1.bound : gl.bound)

/* bumped by each register write (write_regs), and when the copy below is
   dropped: the list calls' state is right while it stays */
static u32 gs_serial;
/* something was drawn through VU1 since the last FLUSH: a register write
   or an upload must wait for it (FLUSH); else it need not */
static int gs_drew = 1;
/* beam: the frame stream goes into the frame's once record (once_on): an
   upload there does not wait for the main record's batches */
static int in_once;
#ifdef MXGL_NO_FLUSHSKIP
#define GS_DREW 1
#else
#define GS_DREW gs_drew
#endif
/* GS registers last written in this frame */
static struct {
  u64 tex0, tex1, clamp, alpha, test, zbuf, scissor, mip1, mip2, frame, fogcol, fba;
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
/* glDrawArrays(GL_POLYGON)'s vertices, copied for the queue (GL reads
   the arrays at the call): x, y, z, s, t */
#define PQ_BUF_FLOATS (5 * 1280) /* 5 vertices a polygon of PQ_MAX */
static float pq_buf[PQ_BUF_FLOATS];
static int pq_buf_n;
static void draw_polys(void);
static inline void pq_flush(void) {
  if (pq_n)
    draw_polys();
}

static inline float fast_log2(float x);

/* the first error since glGetError */
static void gl_error(GLenum e) {
  if (gl.err == GL_NO_ERROR)
    gl.err = e;
}

static float *cur_mat(void) {
  pq_flush();
  mat_serial++;
  return gl.mode == GL_PROJECTION ? gl.pr[gl.pr_top]
         : gl.mode == GL_TEXTURE  ? gl.tm[gl.tm_top]
                                  : gl.mv[gl.mv_top];
}

/* the texture matrix is the identity, or moves S and T by whole repeats
   at most (st_poly's offsets may then be taken off before it) */
static int tm_ident(void) {
  /* made again when a matrix may have changed (mat_serial) */
  static u32 ser;
  static int id;
  if (ser != mat_serial) {
    const float *t = gl.tm[gl.tm_top];
    id = t[0] == 1 && t[1] == 0 && t[4] == 0 && t[5] == 1 && t[12] == 0 && t[13] == 0;
    ser = mat_serial;
  }
  return id;
}
/* log2 of how much the texture matrix scales an area of S, T (lod_k) */
static float tm_log2_area(void) {
  const float *t = gl.tm[gl.tm_top];
  float d = t[0] * t[5] - t[1] * t[4];
  d = d < 0 ? -d : d;
  return d > 1e-12f ? fast_log2(d) : 0.0f;
}
/* the texture matrix as the program's tms, tmt */
static void tm_uniforms(float *tms, float *tmt) {
  const float *t = gl.tm[gl.tm_top];
  tms[0] = t[0], tms[1] = t[1], tms[2] = t[12], tms[3] = 0;
  tmt[0] = t[4], tmt[1] = t[5], tmt[2] = t[13], tmt[3] = 0;
}
