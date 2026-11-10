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
   - The screen (mxgl_set_mode) is SD, the console's TV mode, or 1080i,
     with or without beam racing:
     - SD: 640 x 448 (NTSC) or 640 x 512 (PAL), interlaced FIELD mode, two
       16-bit colour buffers (CT16S, dithered) and one 24-bit Z buffer,
       which leaves about 1.8 MiB (PAL 1.5) of GS memory for textures.
     - 1080i: 640 x 540 shown by both fields (FRAME mode), stretched to
       1920 x 1080, two CT16S buffers and a 16-bit Z: 1.9 MiB left.
     - Beam racing (maxsdk's passes, <mx/hires.h>): one buffer of a field's
       lines (SD 640 x 224 / 256, 1080i 1920 x 540; CT16S, or CT32 in SD)
       drawn in strips just ahead of the beam, every field, from the
       frame's stream recorded; Z (16 bits, or 32 with CT32) in the next
       strip; each field's lines half a line apart, so all the screen's
       lines are drawn. The passes own FRAME, ZBUF, XYOFFSET and SCISSOR: a
       draw without Z writes uses GS context 2 (ZMSK set), so the state
       registers are written in both contexts, and the GL scissor is not
       kept. GS memory left for textures: SD 3.6 MiB (CT32: 3.3), 1080i
       1.9 MiB.
     The mode can change between frames: the pipeline is started again,
     GS memory filled again.

   - Lines and points are GS primitives made on the EE; glReadPixels reads
     the GS's buffers back (mx_frame_sync first); one user clip plane is a
     seventh plane of the clipping; polygon offset is a Z bias in the
     matrix; the colour mask is FRAME's FBMSK; the texture matrix is on VU1.
   Not done: a counting stencil, destination alpha, the other blends with the
   destination colour (the draw is skipped). */

#include <stdlib.h>
#include <string.h>

#include <malloc.h>
#include <math.h>
#include <mx/mx.h>
#include <mx/hud.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <mx/tex.h>
#include <mx/vcache.h>
#include <mx/mem.h>
#include <mx/vm.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include "mxglv.h"
/* MXGL_VU1CLIP: clipping and culling on VU1 (src/mxglc.vsm): the EE sends a draw's
   vertices without looking at them. A game's own programs without mxglc.i's clipping
   and user clip planes keep the EE's clipper (vu1_clips). */
/* the buffers in the TV's mode without beam racing: 512 wide is the video clock's 2560 / 5,
   as exact as 640; smaller buffers leave more GS memory for textures, 16-bit Z most
   (but no top bytes for 8-bit textures) */
#ifndef MXGL_FB_W
#define MXGL_FB_W 640
#endif
#ifndef MXGL_FB_Z
#define MXGL_FB_Z MX_GS_PSM_Z24
#endif
#ifndef MXGL_VU1CLIP
#define MXGL_VU1CLIP 0
#endif
#if MXGL_VU1CLIP
#include "mxglc.h"
#include "mxglcf.h"
#include "mxglce.h"
extern const u8 mxglc_vu[], mxglc_vu_end[], mxglcf_vu[], mxglcf_vu_end[], mxglce_vu[], mxglce_vu_end[];
#define MXGL_UNI_QW VU_MXGLC_UNI_QW
#define MXGL_ADC_W 0x3F808000u /* mxglc.vsm reads the low half, XYZ2's ADC; about 1.0 */
#define MXGL_REV_W 0x00004000u /* closes a triangle in the reverse of its order; about 0 */
#else
#define MXGL_UNI_QW VU_MXGLV_UNI_QW
#define MXGL_ADC_W 0x3F800000u
#define MXGL_REV_W 0u
#endif
/* a vertex's pos.w: 0 it closes a triangle, 1 it closes none (ADC), 2 it closes one
   and the last three vertices have the triangle's reverse order (for mxglc.vsm's
   culling) */
#define MXGL_W(a) ((a) == 1 ? MXGL_ADC_W : (a) == 2 ? MXGL_REV_W : 0)
#include "mxglvf.h"
#include "mxglve.h"

#include "mxgl.h"

extern const u8 mxglv_vu[], mxglv_vu_end[];
extern const u8 mxglvf_vu[], mxglvf_vu_end[], mxglve_vu[], mxglve_vu_end[];
/* the fog programs have mxglv's data layout: o_uni is theirs too */
_Static_assert(VU_MXGLVF_UNI_QW == VU_MXGLV_UNI_QW && VU_MXGLVE_UNI_QW == VU_MXGLV_UNI_QW &&
               VU_MXGLVF_TMS == VU_MXGLV_TMS && VU_MXGLVE_TMS == VU_MXGLV_TMS &&
               VU_MXGLVF_TMT == VU_MXGLV_TMT && VU_MXGLVE_TMT == VU_MXGLV_TMT, "fog layout");

#include <mx/hires.h>
/* the picture (mxgl_set_mode): the buffer drawn into, fb_w x fb_h (beam
   racing: a field's), and the screen it shows, scr_w x scr_h; beam: by
   maxsdk's passes; rgb24: 24-bit colour (CT24 strips with Z24) */
static int beam, rgb24;
static int fb_w = 640, fb_h = 448, scr_w = 640, scr_h = 448;
#define ZMAX_Z24 1048575.0f /* GS Z (24 bits) / 16: ftoi4 (Z32: the same range) */
#define ZMAX_Z16 4095.0f    /* 16 bits */
static float zmax = ZMAX_Z24;
#define NARENA MX_HIRES_SLOTS /* beam racing: an arena per record; else two */
#define HUD_SCALE 1
/* the window the game sees (mxgl_window), mapped onto the whole buffer */
static int gl_w = 640, gl_h = 448;
#define GB_LO (2048.0f - 1900.0f) /* the guard band, GS pixels */
#define GB_HI (2048.0f + 1900.0f)

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
static u8 ring[RING_BYTES] MX_ALIGNED(64);
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

#define MAX_TEX 2048
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
                            lightmaps): CLUT_GREY when not in a product */
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
#define MAX_TWIN 512 /* a lightmap page has 3 with mxgl_color_light */
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

/* ---- the stream ---------------------------------------------------------- */

static void clut_send(void);
static void flush_out(void);
static void batch_place(void);
static u32 s_at[3];

static void frame_start(void) {
  if (in_frame)
    return;
  u64 *e;
  if (beam) {
    /* the passes set the target and clear the strips; the arena is the
       record's (kept while the record is played) */
    mx_frame_begin_n();
    frame_par = (int)mx_hires_slot();
    mx_hires_clear((u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255), (u32)(gl.clear[2] * 255));
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
  /* the dither matrix: the manual's 4 x 4, offsets -4 to 3, a whole step
     of 5 bits (8 of 255), so every shade between two dithers. The GS then
     keeps the top 5 bits: a write is 4 of 255 darker on average (8 bits
     would need offsets 0 to 7; DIMX has -4 to 3) */
  e = mx_e_ad(e, MX_GS_REG_DIMX, 0x6071243571603524ull);
  e = mx_e_ad(e, MX_GS_REG_DTHE, !rgb24); /* not defined into 32 bits */
  if (!beam)
    e = mx_rt_e_clear(e, &rt[buf], 0, (u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255),
                      (u32)(gl.clear[2] * 255), 1);
  mx_frame_commit(e);
  in_frame = 1;
  gs_drew = 1;
  mx_vcache_frame(mx_frame_number(), mx_frame_done());
#ifdef MXGL_VCACHE_CHECK /* debugging: the cache's places checked every frame */
  {
    int mx__vcache_check(void);
    static int told;
    if (!told && mx__vcache_check())
      told = 1, mx_printf("mxgl: the cache's places are wrong at frame %u\n", mx_frame_number());
  }
#endif
  arena_pos = 0;
  s_at[0] = s_at[1] = s_at[2] = 0;
  batch_place();
  gs.valid = 0, gs_serial++;
  mx_frame_shader_setup(&shader);
  vu_cur = &shader;
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
  gs_drew = 1; /* the HUD's draws */
  /* the second: frames shown (beam racing: made), and the sums of the counters */
  static u32 v_last, f_last, n_frames, tris, verts, draws, up_kb, wraps;
  static u32 swap_in, swap_out, ee_last, ee_max, n_long;
  static struct hud_line line[3];
  static struct hud_bar ram, vram;
  u32 shown = beam ? mx_gs.frames : mx_gs.shown;
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
    /* RAM: the program (with the kernel, the VM's tables and the stack),
       pinned memory, free frames, VM pages in use, pinned VM pages;
       without VM, libgl's heaps and malloc's part of the heap */
    u32 gu, gh, su, sh;
    mx_heap_stats(gl_heap, &gu, &gh);
    mx_heap_stats(gl_stream, &su, &sh);
    ram = (struct hud_bar){32u << 20, {0, gh + sh, 0, 0, 0},
                           {BAR_PROG, BAR_CORE, BAR_CORE_FREE, BAR_VM, BAR_PIN}};
    u32 pinned = gh + sh;
    l = &line[1];
    if (mx_vm_active()) {
      struct mx_vm_stats vs;
      mx_vm_stats(&vs);
      pinned = vs.pinned_frames * MX_VM_PAGE;
      ram.amt[0] = ram.total - vs.frames * MX_VM_PAGE;
      ram.amt[1] = pinned;
      ram.amt[2] = vs.free_frames * MX_VM_PAGE;
      ram.amt[3] = (vs.frames - vs.free_frames - vs.pinned_frames - vs.pinned) * MX_VM_PAGE;
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
      ram.amt[3] = (u32)((u8 *)mx_sbrk(0) - lo);
      mx_hud_cat(hud_mb(mx_hud_cat(l->s[0], "MALLOC "), ram.amt[3]), " MB");
      l->rgb[0] = HUD_TEXT;
    }
    /* libgl's part of the pinned memory, of all of it */
    t = mx_hud_cat(hud_mb(mx_hud_cat(l->s[2], "PIN GL "), gu + su), "/");
    mx_hud_cat(hud_mb(t, pinned), " MB");
    l->rgb[2] = HUD_TEXT;
    /* GS memory (KiB): the screen, Z, font and CLUTs; the textures in the
       cache, the end of them uploaded a frame */
    struct mx_vcache_stats vs;
    mx_vcache_stats(&vs);
    /* the top bytes' blocks hold a byte a word: a quarter */
    u32 room = tex_room() / 1024, tex = vs.used_blocks / 4 + vs.hbyte_used / 16,
        up = up_kb / n_frames;
    vram = (struct hud_bar){MX_VRAM_BLOCKS / 4, {vram_lo / 4, tex > up ? tex - up : 0, 0, 0, 0},
                            {BAR_PROG, wraps ? BAR_REFILL : BAR_VM, BAR_UP, 0, 0}};
    vram.amt[2] = tex - vram.amt[1];
    l = &line[2];
    mx_hud_cat(mx_hud_u(mx_hud_cat(mx_hud_u(mx_hud_cat(l->s[0], "TEX "), tex), "/"), room), " KB");
    l->rgb[0] = tex * 100 >= room * 95 ? HUD_BAD : tex * 100 >= room * 80 ? HUD_WARN : HUD_OK;
    mx_hud_cat(mx_hud_u(mx_hud_cat(l->s[1], "UP "), up), " KB/F");
    l->rgb[1] = HUD_TEXT;
    mx_hud_cat(mx_hud_u1(mx_hud_cat(l->s[2], "EVICT "), wraps * 10 / n_frames, 10), "/F");
    l->rgb[2] = wraps ? HUD_BAD : HUD_OK;
    /* the log: the lines, and more */
    u32 c = MX_EE_HZ / 10000;
    for (int i = 0; i < 3; i++)
      mx_printf("stats: %s  %s  %s\n", line[i].s[0], line[i].s[1], line[i].s[2]);
    struct mx_vcache_stats cv;
    mx_vcache_stats(&cv);
    mx_printf("stats: longest frame %u us, %u frames over a field, arena %u/%u KB; textures: %d "
              "levels left out%s, %u drawn from their tails (upper levels paged out), %u whole again\n",
              ee_max, n_long, arena_pos / 1024, ARENA_BYTES / 1024, tex_bias,
              tex_bias_auto ? " (auto)" : "", cv.proxies, cv.upgrades);
    if (tex_bias_auto) {
      u32 room = tex_room();
      mx_printf("stats: texture quality: W %u KB = %u%% of GS memory%s (placed about %u%%), RAM %s; "
                "mipped textures sampled from level 0/1/2+: %u/%u/%u\n",
                ws_last_w0 / 1024, room ? (u32)((u64)ws_last_w0 * 100 / room) : 0,
                v_low ? ", lowered" : "", v_low && room ? (u32)((u64)ws_last_e * 100 / room) : 0,
                ram_cold ? "cold (heads not used)" : "normal", ws_far[0], ws_far[1], ws_far[2]);
    }
    {
      /* where the cache's GS memory goes (KB), and the room made since the
         last line: the blocks in use in the runs taken, the blocks evicted */
      static u32 run0, ev0;
      struct mx_vcache_space sp;
      mx_vcache_space(&sp);
      u32 run = cv.evict_run_blocks - run0, ev = cv.evicted_blocks - ev0;
      run0 = cv.evict_run_blocks, ev0 = cv.evicted_blocks;
      mx_printf("stats: gs memory: room %u KB, held %u (pages %u); texels %u, between blocks %u, "
                "whole pages %u, free in shared pages %u; top bytes %u of %u KB (texels %u); "
                "evicted %u KB for runs with %u KB in use\n",
                (MX_VRAM_BLOCKS - vram_lo) / 4, cv.pages * 8, cv.pages, sp.need_blocks / 4,
                sp.span_waste / 4, sp.round_waste / 4, sp.small_free / 4, cv.hbyte_used / 16,
                cv.hbyte_blocks / 16, sp.hbyte_need / 16, ev / 4, run / 4);
    }
    mx_printf("stats: ram: program %u KB, pinned %u KB (libgl %u in %u, stream %u in %u), free %u KB, "
              "%s %u KB, pinned pages %u KB\n",
              ram.amt[0] / 1024, ram.amt[1] / 1024, gu / 1024, gh / 1024, su / 1024, sh / 1024,
              ram.amt[2] / 1024, mx_vm_active() ? "VM pages" : "malloc", ram.amt[3] / 1024,
              ram.amt[4] / 1024);
    static u32 ring0, buf0;
    mx_printf("stats: xf %u.%u tri %u.%u (incl flush %u.%u) ms, wait ring %u ms buf %u ms "
              "(since last)\n",
              st_last.xf_cycles / c / 10, st_last.xf_cycles / c % 10, st_last.tri_cycles / c / 10,
              st_last.tri_cycles / c % 10, st_last.flush_cycles / c / 10,
              st_last.flush_cycles / c % 10,
              (mx_frame_stats.wait_ring - ring0) / (MX_EE_HZ / 1000),
              (mx_frame_stats.wait_buffer - buf0) / (MX_EE_HZ / 1000));
    ring0 = mx_frame_stats.wait_ring, buf0 = mx_frame_stats.wait_buffer;
    mx_printf("stats: lists %u (made %u, %u did not fit; pool %u KB): bounds %u us, state %u us, "
              "call %u us\n", st_last.lists, dl_nused, dl_nfailed, dl_pool_pos / 1024,
              st_last.c_bounds / 295, st_last.c_state / 295, st_last.c_call / 295);
    mx_printf("stats: calls %u, batches %u; matrix %u us, lod %u us, set_state %u us; "
              "clipped %u in %u us, refused %u\n",
              st_last.calls, st_last.draws, st_last.c_mvp / 295, st_last.c_lod / 295,
              st_last.c_set / 295, st_last.clipped, st_last.c_clip / 295, st_last.refused);
    if (beam) {
      /* the longest pass of each strip */
      u32 n = mx_hires_passes(), us = 0;
      for (u32 k = 0; k < n; k++) {
        us = mx_hires_stats.pass_us[k] > us ? mx_hires_stats.pass_us[k] : us;
        mx_hires_stats.pass_us[k] = 0;
      }
      mx_printf("stats: beam: fields %u, passes %u (%u a field), late %u, records shown %u, held %u, "
                "largest %u KB of %u; longest pass %u us\n",
                mx_hires_stats.fields, mx_hires_stats.passes, n, mx_hires_stats.late, mx_hires_stats.shown,
                mx_hires_stats.held, mx_hires_stats.rec_max / 1024, REC_BYTES / MX_HIRES_SLOTS / 4 * 3 / 1024, us);
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
  if (beam)
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

static void tex_auto(u32 up_kb);
void mxgl_end_frame(void) {
  /* L3 + R3 (both sticks pressed): the counters on and off, in every program */
  static int sticks_held;
  int sticks = (mx_pad_buttons(0) & (MX_PAD_L3 | MX_PAD_R3)) == (MX_PAD_L3 | MX_PAD_R3);
  if (sticks && !sticks_held)
    mxgl_show_stats = !mxgl_show_stats;
  sticks_held = sticks;
  pq_flush();
  if (!in_frame)
    frame_start();
  flush_out();
  if (mxgl_show_stats)
    draw_stats();
  /* the textures the cache evicted this frame */
  {
    static u32 evicted;
    struct mx_vcache_stats vs;
    mx_vcache_stats(&vs);
    st_cur.wraps = vs.evicted - evicted;
    evicted = vs.evicted;
  }
  tex_auto(st_cur.upload_kb);
#ifdef MXGL_STATS_WINDOW
  stats_window();
#endif
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
  /* beam: the same in context 2 (the draws without Z writes) */
  u32 reg2[24];
  u64 val2[24];
  if (beam) {
    for (int i = 0; i < n; i++) {
      reg2[2 * i] = reg[i], val2[2 * i] = val[i];
      reg2[2 * i + 1] = reg[i] + 1, val2[2 * i + 1] = val[i];
    }
    reg = reg2, val = val2, n *= 2;
  }
  u64 *c = mx_frame_reserve((u32)n + 2);
  c = mx_e_cnt(c, (u32)n + 1, GS_DREW ? mx_vif_flush() : mx_vif_nop(), mx_vif_direct((u32)n + 1));
  gs_drew = 0;
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
static void image_xfer_at(u32 dbp, u32 dbw, u32 psm, u32 y, u32 w, u32 h, u32 bpp,
                          const void *data, int flush);
static void image_xfer(u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, u32 bpp, const void *data,
                       int flush) {
  image_xfer_at(dbp, dbw, psm, 0, w, h, bpp, data, flush);
}
/* the same into lines y .. y + h - 1 */
static void image_xfer_at(u32 dbp, u32 dbw, u32 psm, u32 y, u32 w, u32 h, u32 bpp,
                          const void *data, int flush) {
  flush_out();
  u32 qwc = w * h * bpp / 16;
  st_cur.upload_kb += qwc / 64;
  u64 *c = mx_frame_reserve(6 + 3 * (qwc / 0x7FFF + 1));
  const u8 *d = data;
  c = mx_e_cnt(c, 5, flush && GS_DREW ? mx_vif_flush() : mx_vif_nop(), mx_vif_direct(5));
  /* the batches before are drawn: no FLUSH needed before the next
     register writes, unless this went into the once record, apart from
     the main record's batches */
  if (flush && !in_once)
    gs_drew = 0;
  c = mx_e_ad_tag(c, 4, 0);
  c = mx_e_ad(c, MX_GS_REG_BITBLTBUF, mx_gs_bitbltbuf(0, 0, 0, dbp, dbw, psm));
  c = mx_e_ad(c, MX_GS_REG_TRXPOS, mx_gs_trxpos(0, 0, 0, y, 0));
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
   ONE_MINUS_SRC_COLOR), 1.0 (CLUT_GREY: a grey RGBA texture drawn other
   than in a product, as GL draws it); CLUT_ENV: white (the vertex colour
   as it is in a modulate) with i / 2 in alpha, for GL_BLEND's second pass
   (draw_env); in
   CSM1's order (bits 3 and 4 of the index swapped), each 16 x 16 CT32 (4
   blocks) */
enum { CLUT_2X, CLUT_1X, CLUT_INV, CLUT_GREY, CLUT_ENV, NCLUT };
static u32 clut_cbp;
static u32 clut[NCLUT][256] MX_ALIGNED(64);

/* the game's palette as a CLUT (mxgl_Palette), after the brightness CLUTs */
static u32 clut_pal[256] MX_ALIGNED(64);
static int pal_on;
static u8 pal_rgb[256][3];
/* the shared palette with alpha (GL_COLOR_INDEX textures) */
static u8 pal_rgba[256][4];
/* the entry of a shared palette that is transparent (glColorTableEXT: its
   alpha 0), or -1: textures keyed with it are 8 bits too */
static int pal_transp = -1;
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
        if (i == pal_transp)
          continue;
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
    memcpy(pal_rgba[i], c, 3);
    pal_rgba[i][3] = 255;
    u32 key = (u32)c[0] | (u32)c[1] << 8 | (u32)c[2] << 16;
    if (i != pal_transp && pal_find(key) < 0) {
      u32 h = (key * 2654435761u) >> 22;
      while (pal_key[h])
        h = (h + 1) & 1023;
      pal_key[h] = key + 1;
      pal_idx[h] = (u8)i;
    }
    /* CSM1's order: bits 3 and 4 of the index swapped */
    u32 at = ((u32)i & 0xE7) | ((u32)i & 8) << 1 | ((u32)i & 16) >> 1;
    clut_pal[at] = key | (i == pal_transp ? 0 : 0x80u << 24);
  }
  mx_dcache_writeback(clut_pal, sizeof clut_pal);
  pal_on = 1;
  /* a frame being made: its draws from now on take the new one (the CLUTs
     are sent at each frame's start) */
  if (in_frame) {
    flush_out();
    clut_send();
    gs.valid = 0, gs_serial++;
  }
}

/* CSM1's order of a CLUT's entry i: bits 3 and 4 swapped */
static inline u32 csm1(u32 i) { return (i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1; }

static void clut_make(void) {
  for (u32 i = 0; i < 256; i++) {
    u32 at = (i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1, g = i | i << 8 | i << 16;
    clut[CLUT_2X][at] = g | i << 24;
    clut[CLUT_1X][at] = g | ((i + 1) >> 1) << 24;
    clut[CLUT_INV][at] = g | ((256 - i) >> 1) << 24;
    clut[CLUT_GREY][at] = g | 0x80u << 24;
    clut[CLUT_ENV][at] = 0xFFFFFFu | ((i + 1) >> 1) << 24;
  }
  mx_dcache_writeback(clut, sizeof clut);
}

/* beam: texture uploads go into the frame's once record (played once,
   not in every pass), unless GS memory was refilled in this frame: then
   their order among the draws matters */
static void once_on(void) {
#ifdef MXGL_NO_ONCE /* debugging: beam racing's uploads in the main record */
  return;
#endif
  if (!beam)
    return;
  flush_out();
  mx_hires_once(1);
  in_once = 1;
}
static void once_off(void) {
#ifdef MXGL_NO_ONCE
  return;
#endif
  if (beam)
    mx_hires_once(0);
  in_once = 0;
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

/* the texture quality (mxgl_texture_quality): the levels left out above
   (0 .. 2) of textures drawn with MIPMAP filtering */
static s32 k_lod; /* the LOD constant of the draw being made (prepare_state) */

/* ---- texture quality under pressure (MXGL_TEX_AUTO) ----

   VRAM: W, the bytes of the textures a frame uses (at full quality), against
   V, GS memory's room for textures. Up to 120% of V the frame streams what
   does not fit (the textures are in RAM: cheap). Above it for 2 seconds the
   textures are placed from coarser levels until about 80% of V: first the
   finest level their draws sample (LOD: no visible change), then the
   largest ones a level less each, never below MX_VCACHE_TAIL_MIN a side;
   back to full once W at full quality is at most halfway (100%) for 3
   seconds.
   RAM: swap-ins above 30 pages a second for 3 seconds: textures whose
   draws do not sample their top level are drawn from their tails (their
   pageable upper levels not touched, and marked to go first: mx_mem_cold);
   back after 5 quiet seconds. */

void mxgl_texture_limits(int vram_hi_pct, int vram_lo_pct, int swap_ins_s) {
  tq_hi = vram_hi_pct, tq_lo = vram_lo_pct, tq_swap = swap_ins_s;
}

/* t's bytes in GS memory placed from level f (and its CLUT) */
static u32 tex_bytes_from(const struct tex *t, u32 f) {
  u32 n = t->oclut ? 1024 : 0;
  for (u32 lv = f; lv < t->levels; lv++)
    n += mx_tex_level_bytes(t->psm, t->lw, t->lh, lv);
  return n;
}
/* the coarsest level t may be placed from (MX_VCACHE_TAIL_MIN a side) */
static u32 tex_max_from(const struct tex *t) {
  u32 f = 0;
  while (f + 1 < t->levels && (t->w >> (f + 1)) >= MX_VCACHE_TAIL_MIN &&
         (t->h >> (f + 1)) >= MX_VCACHE_TAIL_MIN)
    f++;
  return f;
}
static int tex_mipped(const struct tex *t) { return t->mip && t->levels > 1; }

/* a draw of the bound texture with LOD constant k (1/16) whose nearest
   vertex is at w: the finest level the GS samples there */
static void tex_need_note(s32 k, float wmin) {
  if (!gl.tex2d || !gl.bound || gl.bound >= MAX_TEX)
    return;
  struct tex *t = &texs[gl.bound];
  if (!t->used || !tex_mipped(t))
    return;
  float l = wmin > 1e-6f ? fast_log2(wmin) + (float)k / 16.0f : 0.0f;
  u32 lvl = l <= 0 ? 0 : (u32)l;
  if (lvl >= t->levels)
    lvl = t->levels - 1u;
  u32 f = mx_frame_number();
  if (lvl <= t->need_lvl || (s32)(f - t->need_until) > 0)
    t->need_lvl = (u8)lvl, t->need_until = f + NEED_HOLD;
}

static u32 tex_need(const struct tex *t) {
  if (!tex_mipped(t))
    return 0;
  u32 n = (u32)tex_bias;
  if (v_low) {
    u32 v = t->vfrom_epoch == v_epoch ? t->vfrom : t->need_lvl;
    n = v > n ? v : n;
  }
  /* RAM: drawn from its tail while its top level is not sampled */
  if (ram_cold && t->m.head && t->need_lvl >= 1 && t->m.levels > t->m.tail && t->m.tail > n)
    n = t->m.tail;
  return n;
}

void mxgl_texture_quality(int q) {
  tex_bias_auto = q == MXGL_TEX_AUTO;
  int b = tex_bias_auto ? 0 : q < 0 ? 0 : q > 2 ? 2 : q;
  if (b != tex_bias || !tex_bias_auto)
    vram_gen++; /* every texture looked up again */
  tex_bias = b;
  if (!tex_bias_auto)
    v_low = ram_cold = 0;
}
int mxgl_texture_bias(void) { return tex_bias; }

/* the frame used t (its first lookup in the frame) */
static void ws_note(struct tex *t) {
  u32 f = mx_frame_number();
  if (t->ws_frame == f + 1)
    return;
  t->ws_frame = f + 1;
  ws_w0 += tex_bytes_from(t, 0);
  if (ws_n[ws_cur] < WS_MAX)
    ws[ws_cur][ws_n[ws_cur]++] = t;
}

/* the VRAM control's levels for the textures of the frame before: from
   the finest level sampled, then the largest a level less until the
   estimate is at most tq_lo % of V */
static int vram_assign(u32 room) {
  v_epoch++;
  struct tex **l = ws[ws_cur ^ 1];
  u32 n = ws_n[ws_cur ^ 1], e = 0;
  static u8 before[WS_MAX];
  for (u32 i = 0; i < n; i++) {
    struct tex *t = l[i];
    before[i] = t->vfrom;
    t->vfrom = tex_mipped(t) ? t->need_lvl : 0;
    t->vfrom_epoch = v_epoch;
    e += tex_bytes_from(t, t->vfrom);
  }
  u64 target = (u64)room * (u32)tq_lo / 100;
  /* a level less each, the textures seen from farthest first (their draws
     sample coarse levels already: least visible), within those the largest
     first; the ones sampled at level 0 (near) last */
  for (int cls = 2; cls >= 0 && e > target; cls--)
    for (int pass = 0; pass < 3 && e > target; pass++) {
      u32 big = 0;
      for (u32 i = 0; i < n; i++)
        if (tex_mipped(l[i]) && l[i]->vfrom < tex_max_from(l[i]) &&
            (cls == 2 ? l[i]->need_lvl >= 2 : l[i]->need_lvl == (u32)cls)) {
          u32 b = tex_bytes_from(l[i], l[i]->vfrom);
          big = b > big ? b : big;
        }
      if (!big)
        break;
      for (u32 i = 0; i < n && e > target; i++) {
        struct tex *t = l[i];
        if (!tex_mipped(t) || t->vfrom >= tex_max_from(t) ||
            !(cls == 2 ? t->need_lvl >= 2 : t->need_lvl == (u32)cls))
          continue;
        u32 b = tex_bytes_from(t, t->vfrom);
        if (b * 2 < big && pass < 2)
          continue; /* the larger ones first */
        e -= b - tex_bytes_from(t, t->vfrom + 1u);
        t->vfrom++;
      }
    }
  ws_last_e = e;
  /* changed: looked up again (the others keep their places) */
  int changed = 0;
  for (u32 i = 0; i < n; i++)
    changed |= l[i]->vfrom != before[i];
  return changed;
}

static u32 cold_hot, cold_cool, v_hot, v_cool;
static void tex_auto(u32 up_kb) {
  static u32 v0;
  (void)up_kb;
  ws_sum += ws_w0, ws_frames++;
  ws_w0 = 0;
  ws_cur ^= 1;
  ws_n[ws_cur] = 0;
  if (!tex_bias_auto || mx_gs.vsync - v0 < mx_gs.hz)
    return;
  v0 = mx_gs.vsync;
  u32 room = tex_room();
  u32 w = ws_frames ? ws_sum / ws_frames : 0;
  ws_last_w0 = w;
  ws_sum = ws_frames = 0;
  /* VRAM */
  int was = v_low;
  if (!v_low) {
    v_hot = (u64)w * 100 > (u64)room * (u32)tq_hi ? v_hot + 1 : 0;
    if (v_hot >= 2)
      v_low = 1, v_hot = 0, v_cool = 0;
  } else {
    /* back at full quality once W at full quality is halfway between the
       limits (100%): streaming that much is fine */
    v_cool = (u64)w * 200 <= (u64)room * (u32)(tq_lo + tq_hi) ? v_cool + 1 : 0;
    if (v_cool >= 3)
      v_low = 0, v_cool = 0;
  }
  ws_far[0] = ws_far[1] = ws_far[2] = 0;
  for (u32 i = 0; i < ws_n[ws_cur ^ 1]; i++)
    if (tex_mipped(ws[ws_cur ^ 1][i]))
      ws_far[ws[ws_cur ^ 1][i]->need_lvl > 2 ? 2 : ws[ws_cur ^ 1][i]->need_lvl]++;
  int changed = v_low ? vram_assign(room) : 0;
  if (changed || v_low != was)
    vram_gen++; /* the textures looked up again, from their new levels */
  /* RAM */
  struct mx_mem_pressure p;
  mx_mem_pressure(&p);
  int busy = p.swap_ins_s >= (u32)tq_swap; /* 0: always (tests) */
  cold_hot = busy ? cold_hot + 1 : 0;
  cold_cool = busy ? 0 : cold_cool + 1;
  if (!ram_cold && cold_hot >= 3) {
    ram_cold = 1;
    vram_gen++;
    struct tex **l = ws[ws_cur ^ 1];
    for (u32 i = 0; i < ws_n[ws_cur ^ 1]; i++)
      if (l[i]->m.head && l[i]->need_lvl >= 1)
        mx_mem_cold(l[i]->m.head, l[i]->m.head_bytes);
  } else if (ram_cold && cold_cool >= 5) {
    ram_cold = 0;
    vram_gen++;
  }
}

static int tex_place(struct tex *t) {
  u32 frame = mx_frame_number();
  if (t->vc_frame == frame && t->vc_gen == vram_gen && t->vc_gen && !t->dirty) {
    /* a use all the same: the cache tells a texture done for the frame by its uses */
    mx_vcache_use(mx_vcache_tex_key(tex_key(t), t->m.from));
    return 0;
  }
  ws_note(t);
  u32 cpsm = t->psm == MX_GS_PSM_T8 ? (t->oclut ? MX_GS_PSM_CT32 : MX_VCACHE_NO_CLUT) : 0;
  t->m.levels = (u8)tex_levels(t);
  /* without MIPMAP filtering level 0 is drawn: wait for it rather than a proxy */
  u32 from;
#ifdef MXGL_FORCE_TAIL /* debugging: split textures drawn from their tails */
  u32 need_ = t->m.tail ? t->m.tail : tex_need(t);
#else
  u32 need_ = tex_need(t);
#endif
  int r = mx_vcache_tex_mem(tex_key(t), &t->m, t->psm, t->lw, t->lh, cpsm, need_, !t->mip,
                            &t->mt, &from);
  u32 nl = t->m.levels - from;
  if (r == MX_VCACHE_FULL)
    return -1;
  if (r == MX_VCACHE_HIT && !t->dirty) {
    t->vc_frame = frame, t->vc_gen = vram_gen;
    return 0;
  }
  st_cur.uploads++;
  once_on();
  if (r == MX_VCACHE_HIT && nl == 1 && !from) {
    /* changed lines alone (whole quadwords of lines), read from its head */
    mx_tex_mem_lock(&t->m);
    u32 rb = t->w * tex_bpp(t), k = 1;
    while ((k * rb) & 15)
      k <<= 1;
    u32 y0 = t->dy0 & ~(k - 1), y1 = (t->dy1 + k - 1) & ~(k - 1);
    y1 = y1 > t->h ? t->h : y1;
    const u8 *src = TLV(t, 0) + y0 * rb;
    image_xfer_at(t->mt.tbp[0], t->mt.tbw[0], t->mt.psm, y0, t->w, y1 - y0, tex_bpp(t), src, 1);
  } else {
    if (r == MX_VCACHE_MISS) {
      /* other textures moved only if the miss evicted some */
      static u32 evicted;
      struct mx_vcache_stats vs;
      mx_vcache_stats(&vs);
      if (vs.evicted != evicted)
        vram_gen++;
      evicted = vs.evicted;
    }
    for (u32 lv = 0; lv < nl; lv++)
      image_xfer(t->mt.tbp[lv], t->mt.tbw[lv], t->mt.psm, t->w >> (lv + from), t->h >> (lv + from),
                 tex_bpp(t), TLV(t, lv + from), lv == 0);
    if (t->oclut) {
      image_xfer(t->mt.cbp, 1, MX_GS_PSM_CT32, 16, 16, 4, t->oclut, 0);
      /* the GS loads a CLUT when TEX0 is written (CLD 1): written again even
         if the same (another texture of this shape had these places this
         frame, its CLUT is the one loaded) */
      gs.tex0 = ~0ull;
    }
  }
  t->dirty = 0;
  texflush();
  once_off();
  t->vc_frame = frame, t->vc_gen = vram_gen;
  return 0;
}

/* ---- draw state ---------------------------------------------------------- */

static int clut_pick; /* the CLUT of a brightness texture (blend_mode) */
/* draw_env: 1 while the second pass of a GL_BLEND draw is being drawn
   (set_state uses the texture's brightness times its alpha, with CLUT_ENV),
   2 while the first pass of a texture with alpha (its alpha copy: white) */
static int env_pass;
/* GL_TEXTURE_ENV_COLOR of unit 0 (GL_BLEND's Cc) */
static float env_color[4];

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

static struct tex *tex_twin(struct tex *t, int ch);

/* mxgl_color_light: products with the destination of colour textures (the
   lightmaps of Quake 2) per channel: three passes of each batch, one
   channel written in each (FRAME's FBMSK), its texture the channel's copy;
   also with beam racing (FRAME is in the stream) */
static int cl_enabled;
static int cl_batch;          /* the batch being gathered is drawn so */
static u64 cl_tex0[3], cl_frame[3];
void mxgl_color_light(int on) { cl_enabled = on != 0; }

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
/* the stencil: the alpha bit of 16-bit colour; CT24 has none */
static inline int st_there(void) { return !(beam && rgb24); }
/* FRAME of the buffer being drawn into, with FBMSK mask */
static inline u64 frame_reg(u32 mask) {
  return beam ? mx_hires_frame(mask) : mx_gs_frame(rt[buf].fbp, rt[buf].fbw, rt[buf].psm, mask);
}

/* the stencil's test and operations on a value */
static int st_pass(int v) {
  u32 a = (u32)gl.st_ref & gl.st_mask, b = (u32)v & gl.st_mask;
  switch (gl.st_func) {
  case GL_NEVER: return 0;
  case GL_LESS: return a < b;
  case GL_LEQUAL: return a <= b;
  case GL_GREATER: return a > b;
  case GL_GEQUAL: return a >= b;
  case GL_EQUAL: return a == b;
  case GL_NOTEQUAL: return a != b;
  default: return 1;
  }
}
static int st_new(int v) {
  int n = v;
  switch (gl.st_op) {
  case GL_ZERO: n = 0; break;
  case GL_REPLACE: n = gl.st_ref; break;
  case GL_INCR: n = v < 255 ? v + 1 : 255; break;
  case GL_DECR: n = v > 0 ? v - 1 : 0; break;
  case GL_INVERT: n = ~v & 255; break;
  case GL_INCR_WRAP: n = (v + 1) & 255; break;
  case GL_DECR_WRAP: n = (v - 1) & 255; break;
  default: break;
  }
  return (int)(((u32)v & ~gl.st_wmask) | ((u32)n & gl.st_wmask));
}

static int set_state(u32 *prim, float *cscale, s32 lod_k) {
  struct tex *t = NULL;
  /* the per channel passes' TEX0 and FRAME (mxgl_color_light): kept for
     the batch before until write_regs has drawn it */
  u64 ntex0[3] = {0, 0, 0}, nframe[3] = {0, 0, 0};
  if (gl.tex2d && gl.bound && gl.bound < MAX_TEX && texs[gl.bound].used && texs[gl.bound].data)
    t = &texs[gl.bound];
  /* a colour texture multiplied with the destination: its brightness, or
     per channel (mxgl_color_light) */
  struct tex *cl_src = NULL;
  if (t && env_pass == 2) { /* draw_env's first pass: the fragment colour, the texture's alpha */
    if (!(t = tex_twin(t, TWIN_ENV_A)))
      return -1;
  } else if (t && env_pass) { /* GL_BLEND's second: its brightness (times its alpha) */
    if ((!t->bright || t->planar) && !(t = tex_twin(t, t->alpha ? TWIN_ENV_B : 0)))
      return -1;
  } else if (t && (!t->bright || t->planar) && gl.blend && blend_product()) {
    if (cl_enabled && !(t->mip && t->levels > 1))
      cl_src = t;
    if (!(t = tex_twin(t, cl_src ? 1 : 0)))
      return -1;
  }

  u64 alpha = 0;
  clut_pick = t && t->grey ? CLUT_GREY : CLUT_1X;
  int abe = gl.blend ? blend_mode(&alpha, t) : 0;
  if (abe < 0)
    return -1;
  if (env_pass == 1)
    clut_pick = CLUT_ENV;
  /* the registers were written by others (the frame's start, glClear, the
     HUD): none of the values kept holds, also of those this draw does not
     write (ALPHA when it does not blend: the next draw that does must
     write it) */
  if (!gs.valid) {
    int was = gs.valid;
    memset(&gs, 0xFF, sizeof gs);
    gs.valid = was;
  }

  u32 reg[12];
  u64 val[12];
  int n = 0;
  if (t) {
    if (tex_place(t) < 0)
      return -1;
    /* REPLACE, DECAL: the GS's DECAL (the texture's colour); the alpha is
       the texture's if its format has one, else the fragment's (TCC 0).
       GL's DECAL of an RGBA texture (a blend by its alpha) is drawn so too */
    int repl = gl.texenv == GL_REPLACE || gl.texenv == GL_DECAL;
    u32 tfx = repl ? 1 : 0, tcc = repl ? (u32)t->alpha_fmt : 1;
    if (t->bright)
      t->mt.cbp = clut_cbp + 4 * (u32)clut_pick;
    else if (t->pal)
      t->mt.cbp = clut_cbp + 4 * NCLUT;
    u64 tex0 = mx_tex_tex0(&t->mt, tcc, tfx);
    /* per channel: the green and blue copies in GS memory too (the red one
       kept while they are placed) */
    if (cl_src) {
      struct tex *g = tex_twin(cl_src, 2), *b = tex_twin(cl_src, 3);
      u64 kr = mx_vcache_tex_key(tex_key(t), t->m.from);
      mx_vcache_pin(kr);
      int ok = g && tex_place(g) == 0;
      u64 kg = ok ? mx_vcache_tex_key(tex_key(g), g->m.from) : 0;
      if (ok)
        mx_vcache_pin(kg);
      ok = ok && b && tex_place(b) == 0;
      if (kg)
        mx_vcache_unpin(kg);
      mx_vcache_unpin(kr);
      if (!ok)
        return -1;
      g->mt.cbp = b->mt.cbp = t->mt.cbp;
      ntex0[0] = tex0;
      ntex0[1] = mx_tex_tex0(&g->mt, tcc, tfx);
      ntex0[2] = mx_tex_tex0(&b->mt, tcc, tfx);
    }
    /* MMIN: 0/1 nearest/linear; with levels 2/3 (nearest texel) or 4/5
       (linear), the nearest level or both (trilinear): both unless the
       engine asked for the nearest (r_textureMode *_MIPMAP_NEAREST) */
    /* MIN_FILTER without MIPMAP: level 0 only (GLQuake's lightmaps: a
       level below mixes the surfaces packed next to each other) */
    int mipped = t->mt.levels > 1 && t->mip;
    u32 mmin = (u32)t->lin_min;
    if (mipped)
      mmin = (t->lin_min ? 4 : 2) + (t->mip != 1);
    /* placed from level m.from (its tail, or a lower quality): that level is
       level 0, so the level asked for is m.from less (K in 1/16) */
    u64 tex1 = mx_gs_tex1(0, mipped ? t->mt.levels - 1u : 0, (u32)t->lin_mag, mmin, 0, 0,
                          mipped ? (u32)(lod_k - 16 * (s32)t->m.from) : 0);
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

  /* the stencil: the alpha bit is not written unless a draw does */
  int st_date = 0, st_datm = 0, st_never = 0, st_half = 0;
  u32 st_fba = 0, amask = 0x80000000u;
  if (gl.st_on && st_there()) {
    /* the other value, before the test: this draw's pixels may overlap, and the
       draws after it have this state without passing here */
    int na = st_new(gl.st_va);
    if (gl.st_vb < 0 && st_pass(gl.st_va) && na != gl.st_va)
      gl.st_vb = na;
    int b = gl.st_vb < 0 ? gl.st_va : gl.st_vb;
    int pa = st_pass(gl.st_va), pb = st_pass(b);
    st_date = pa != pb, st_datm = pb, st_never = !pa && !pb;
    int nb = st_new(b);
    if ((pa && na != gl.st_va) || (pb && nb != b)) {
      /* the bit of what the pixels that pass become */
      int w = (pa && na != gl.st_va) || (pb && nb != gl.st_va);
      if (w) {
        st_fba = 1, amask = 0;
      } else if ((gl.cmask & 0x00FFFFFFu) == 0x00FFFFFFu) {
        /* bit 7 of the source alpha, with FBA 0: half the alpha, and the alpha
           test's reference with it; only without colour, which it would change */
        st_half = 1, amask = 0;
      }
    }
  }
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
  /* GS Z is larger nearer (GL's depth reversed); it tests NEVER, ALWAYS,
     GEQUAL, GREATER: GL's LESS, LEQUAL; EQUAL is LEQUAL (a pass over the
     same vertices), the tests the other way (GREATER, GEQUAL, NOTEQUAL)
     pass always */
  u32 ztst = 1;
  if (gl.dtest)
    ztst = gl.dfunc == GL_NEVER                         ? 0
           : gl.dfunc == GL_LESS                        ? 3
           : gl.dfunc == GL_LEQUAL || gl.dfunc == GL_EQUAL ? 2
                                                        : 1;
  if (st_half)
    aref /= 2;
  if (st_never)
    ate = 1, atst = 0, aref = 0;
  u64 test = mx_gs_test((int)ate, atst, aref, 0, st_date, st_datm, 1, ztst);
  if (!gs.valid || test != gs.test)
    reg[n] = MX_GS_REG_TEST_1, val[n++] = gs.test = test;

  int zmsk = !(gl.dtest && gl.dmask);
  if (!beam) {
    const struct mx_rt *target = &rt[buf];
    u64 zbuf = mx_gs_zbuf(target->zbp, target->zpsm, (u32)zmsk);
    if (!gs.valid || zbuf != gs.zbuf)
      reg[n] = MX_GS_REG_ZBUF_1, val[n++] = gs.zbuf = zbuf;
  }
  /* glColorMask; per channel: red first (flush_out draws green and blue) */
  for (int k = 0; k < 3; k++)
    nframe[k] = frame_reg((gl.cmask & 0x00FFFFFFu) | amask | (0x00FFFFFFu & ~(0xFFu << (8 * k))));
  u64 frame = cl_src ? nframe[0] : frame_reg((gl.cmask & 0x00FFFFFFu) | amask);
  if (!gs.valid || frame != gs.frame)
    reg[n] = MX_GS_REG_FRAME_1, val[n++] = gs.frame = frame;
  if (!gs.valid || st_fba != gs.fba)
    reg[n] = MX_GS_REG_FBA_1, val[n++] = gs.fba = st_fba;

  int x0 = 0, x1 = fb_w - 1, y0 = 0, y1 = fb_h - 1;
  if (gl.scissor) {
    x0 = gl.sc[0] * fb_w / gl_w, x1 = (gl.sc[0] + gl.sc[2]) * fb_w / gl_w - 1;
    y0 = (gl_h - (gl.sc[1] + gl.sc[3])) * fb_h / gl_h, y1 = (gl_h - gl.sc[1]) * fb_h / gl_h - 1;
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > fb_w - 1 ? fb_w - 1 : x1, y1 = y1 > fb_h - 1 ? fb_h - 1 : y1;
    if (x1 < x0 || y1 < y0)
      return -1;
  }
  if (!beam) {
    u64 sc = mx_gs_scissor((u32)x0, (u32)x1, (u32)y0, (u32)y1);
    if (!gs.valid || sc != gs.scissor)
      reg[n] = MX_GS_REG_SCISSOR_1, val[n++] = gs.scissor = sc;
  }

  if (gl.fog && (!gs.valid || gl.fog_col != gs.fogcol))
    reg[n] = MX_GS_REG_FOGCOL, val[n++] = gs.fogcol = gl.fog_col;

  write_regs(reg, val, n); /* the batch before drawn first */
  gs.valid = 1;
  cl_batch = cl_src != NULL;
  if (cl_batch) {
    memcpy(cl_tex0, ntex0, sizeof cl_tex0);
    memcpy(cl_frame, nframe, sizeof cl_frame);
  }

  /* beam: context 2 for no Z writes (SD: ZBUF's ZMSK); fog only with the
     fog programs (a program of the game's writes no F) */
  *prim = (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, t != NULL, gl.fog && !cur_prog, abe, 0, 0,
                          beam && zmsk, 0);
  if (t) {
    /* 0x80 is 1.0 when modulated: up to 2x with a colour shift */
    cscale[0] = cscale[1] = cscale[2] = gl.cshift ? 1.0f : 0.5f;
  } else {
    cscale[0] = cscale[1] = cscale[2] = 1.0f;
  }
  cscale[3] = st_half ? 64.0f / 255.0f : 128.0f / 255.0f;
  return 0;
}

/* glPolygonOffset in Z buffer steps: factor stands for the slope GL would
   take per polygon (one value for all: the GS has none), units for the
   smallest step */
#define PO_FACTOR 32.0f
#define PO_UNITS 4.0f

/* r = the inverse of a (column-major); 0 if a is singular */
static int mat_inverse(float *r, const float *a) {
  float t[16];
  t[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
         a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  t[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
         a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  t[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
         a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  t[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
          a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  t[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
         a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  t[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
         a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  t[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
         a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  t[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
          a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  t[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
         a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  t[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
         a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  t[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
          a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  t[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
          a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  t[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
         a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  t[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
         a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  t[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
          a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  t[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
          a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
  float det = a[0] * t[0] + a[1] * t[4] + a[2] * t[8] + a[3] * t[12];
  if (det == 0)
    return 0;
  det = 1.0f / det;
  for (int i = 0; i < 16; i++)
    r[i] = t[i] * det;
  return 1;
}

/* the user clip plane in GS coordinates (times w): plane_d's plane 6 */
static float ucp[4];

/* GS coordinates from the GL matrices, viewport and depth range (and the
   polygon offset, a Z bias); with the user clip plane on, its plane in GS
   coordinates into ucp */
static void make_mvp(float *m) {
  float vpm[16];
  memset(vpm, 0, 64);
  float vx = (float)gl.vp[0], vy = (float)gl.vp[1], vw = (float)gl.vp[2], vh = (float)gl.vp[3];
  float r = (float)fb_h / (float)gl_h, rx = (float)fb_w / (float)gl_w;
  /* GL samples a pixel at its centre (x + 0.5), the GS at its corner (x):
     half a pixel of the picture less, or the picture lay half a pixel right
     and down of GL's (measured against Mesa: GLQuake at +0.5, +0.5) and
     pixel-aligned 2D sampled its texels between two. With beam racing the
     buffer is a field (its line two of the picture's, the fields half a
     line apart): half a picture line is a quarter of a buffer line. */
  vpm[0] = vw / 2 * rx;
  vpm[12] = 2048.0f - fb_w / 2 + (vx + vw / 2) * rx - 0.5f;
  vpm[5] = -vh / 2 * r;
  vpm[13] = 2048.0f - fb_h / 2 + (gl_h - vy - vh / 2) * r - (beam ? 0.25f : 0.5f);
  vpm[10] = -zmax * (gl.df - gl.dn) / 2;
  vpm[14] = zmax * (1 - gl.dn - (gl.df - gl.dn) / 2);
  vpm[15] = 1;
  if (gl.poffset) {
    /* + offset is farther: a smaller GS Z; Z is 16 times the coordinate (ftoi4) */
    float bias = -(gl.pofactor * PO_FACTOR + gl.pounits * PO_UNITS) / 16.0f;
    vpm[14] += bias;
  }
  float vpp[16];
  mat_mul(vpp, vpm, gl.pr[gl.pr_top]);
  mat_mul(m, vpp, gl.mv[gl.mv_top]);
  if (gl.ucp_on) {
    /* eye . e >= 0 with eye = vpp^-1 p: the plane is e (vpp^-1), a row */
    float inv[16];
    if (mat_inverse(inv, vpp)) {
      for (int c = 0; c < 4; c++)
        ucp[c] = gl.ucp_eye[0] * inv[4 * c] + gl.ucp_eye[1] * inv[4 * c + 1] +
                 gl.ucp_eye[2] * inv[4 * c + 2] + gl.ucp_eye[3] * inv[4 * c + 3];
    } else {
      ucp[0] = ucp[1] = ucp[2] = 0, ucp[3] = 1;
    }
  }
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
  case 5: return p[2] - zf * p[3]; /* far: Z >= zf */
  default: return ucp[0] * p[0] + ucp[1] * p[1] + ucp[2] * p[2] + ucp[3] * p[3]; /* GL_CLIP_PLANE0 */
  }
}

/* GL_CLIP_PLANE0 is outcode bit 6 (bit 7 marks a vertex behind the eye
   while vu0_xform's outcodes are made): sets it in vo[0..n-1] and makes
   *oand, *oor again */
#define UCP_BIT 0x40u
static void ucp_outcodes(const float (*vp)[4], u8 *vo, int n, u32 *oand, u32 *oor) {
  u32 a = 0x7F, o = 0;
  for (int i = 0; i < n; i++) {
    u32 c = vo[i] & 0x3F;
    if (plane_d(vp[i], 6) < 0)
      c |= UCP_BIT;
    vo[i] = (u8)c;
    a &= c, o |= c;
  }
  *oand = a, *oor = o;
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
#define UCAB(p) ((void *)MX_UCAB((u8 *)(p)))
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
  /* not under a texture matrix that scales or turns S, T */
  const struct tex *t = gl.tex2d && gl.bound && gl.bound < MAX_TEX && tm_ident() ? &texs[gl.bound] : NULL;
  st_off[0] = t && !t->clamp_s ? (float)(s32)st0[0] : 0.0f;
  st_off[1] = t && !t->clamp_t ? (float)(s32)st0[1] : 0.0f;
  st_shift = st_off[0] != 0.0f || st_off[1] != 0.0f;
}

/* s_at: the parts' used bytes */
static u32 o_prim;
static float o_uni[MXGL_UNI_QW][4] MX_ALIGNED(16) = {
    [VU_MXGLV_CMUL] = {1, 1, 1, 1}}; /* the colour as it is (cadd 0) */

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
/* the draw's clipping and culling are VU1's (mxglc.vsm) */
static inline int vu1_clips(void) {
  return MXGL_VU1CLIP && (!cur_prog || cur_prog_clips) && !gl.ucp_on && zn > zf &&
         gl.cullface != GL_FRONT_AND_BACK;
}

static int batch_room(u32 k) {
  if (o_n + k <= o_cap)
    return 1;
  flush_out();
  if (k <= o_cap)
    return 1;
  dropped_draws++;
  return 0;
}

/* mxgl_color_light: channel j's texture and colour mask (after a FLUSH:
   the batch before drawn with the channel before) */
static void cl_regs(int j) {
  /* beam: the draws without Z writes are context 2's */
  u32 n = beam ? 4 : 2;
  u64 *c = mx_frame_reserve(2 + n);
  c = mx_e_cnt(c, n + 1, mx_vif_flush(), mx_vif_direct(n + 1));
  c = mx_e_ad_tag(c, n, 1);
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, cl_tex0[j]);
  c = mx_e_ad(c, MX_GS_REG_FRAME_1, cl_frame[j]);
  if (beam) {
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_TEX0_1, 1), cl_tex0[j]);
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_FRAME_1, 1), cl_frame[j]);
  }
  mx_frame_commit(c);
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
  const struct mx_shader *sh = vprog_use(o_prog);
  const void *uni = vprog_uni(o_prog, (const float(*)[4])o_uni);
  mx_frame_shader_draw(sh, o_prim, uni, streams, o_n);
  gs_drew = 1;
  if (cl_batch) {
    /* green and blue: the same batch again, then red's registers back */
    cl_regs(1);
    mx_frame_shader_draw(sh, o_prim, uni, streams, o_n);
    cl_regs(2);
    mx_frame_shader_draw(sh, o_prim, uni, streams, o_n);
    cl_regs(0);
  }
  st_cur.flush_cycles += mx_count() - tf0;
  st_cur.draws++;
  o_n = 0;
  batch_place();
}

static void out_vertex(const struct cv *v, int adc) {
  o_put_pos(o_n, v->x, MXGL_W(adc));
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
  if (!gl.cull || vu1_clips())
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
  if (!gl.cull || vu1_clips())
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

/* plane k of plane_d as coefficients of (x, y, z, w) */
static void plane_coef(int k, float *c) {
  static const float base[6][4] = {{1, 0, 0, -GB_LO}, {-1, 0, 0, GB_HI}, {0, 1, 0, -GB_LO},
                                   {0, -1, 0, GB_HI}, {0, 0, -1, 0},     {0, 0, 1, 0}};
  if (k == 6) {
    memcpy(c, ucp, 16);
    return;
  }
  memcpy(c, base[k], 16);
  if (k == 4)
    c[3] = zn;
  else if (k == 5)
    c[3] = -zf;
}

static void clip_poly(struct cv *buf0, int n, u32 planes) {
  /* the polygon as pointers to its vertices: the cut ones in pool (a
     convex polygon gains at most two a plane) */
  static struct cv pool[2 * 7 + 2];
  const struct cv *b0[CLIP_MAX], *b1[CLIP_MAX];
  const struct cv **in = b0, **out = b1;
  float d[CLIP_MAX];
  int np = 0;
  for (int i = 0; i < n; i++)
    in[i] = &buf0[i];
  for (int k = 0; k < 7 && n >= 3; k++) {
    if (!(planes & (1u << k)))
      continue;
    float c[4];
    plane_coef(k, c);
    for (int i = 0; i < n; i++) {
      const float *q = in[i]->p;
      d[i] = c[0] * q[0] + c[1] * q[1] + c[2] * q[2] + c[3] * q[3];
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
      int j = i + 1 == n ? 0 : i + 1;
      float dp = d[i], dq = d[j];
      if (dp >= 0 && m < CLIP_MAX)
        out[m++] = in[i];
      if ((dp >= 0) != (dq >= 0) && m < CLIP_MAX && np < (int)(sizeof pool / sizeof pool[0])) {
        /* from the inside vertex, whichever way round the edge is: two
           triangles sharing it (a quad's diagonal, cut one by one) get
           the same point to the bit, else a pixel line between them */
        const struct cv *a = in[i], *b = in[j];
        float da = dp, db = dq;
        if (da < 0)
          a = in[j], b = in[i], da = dq, db = dp;
        lerp(&pool[np], a, b, da / (da - db));
        out[m++] = &pool[np++];
      }
    }
    const struct cv **t = in;
    in = out, out = t, n = m;
  }
  if (n < 3)
    return;
  if (gl.cull) {
    const float *pp[CLIP_MAX];
    for (int i = 0; i < n; i++)
      pp[i] = in[i]->p;
    if (culled_poly(pp, n))
      return;
  }
  if (!batch_room((u32)n))
    return;
  /* a strip of its vertices: 0, 1, n-1, 2, n-2, ... (the first two end no
     triangle) */
  for (int k = 0, lo = 1, hi = n - 1; k < n; k++) {
    int v = k == 0 ? 0 : (k & 1) ? lo++ : hi--;
    out_vertex(in[v], k < 2);
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
  float k = (0.5f * (fast_log2(at * (float)t->w * (float)t->h / ao) + tm_log2_area()) - proj_lpp()) *
            16.0f;
  s32 ki = (s32)(k + (k < 0 ? -0.5f : 0.5f));
  return ki < -2048 ? -2048 : ki > 2047 ? 2047 : ki;
}

struct dl_state {
  int tex2d, blend, atest, dtest, dmask, cull;
  GLuint bound;
  GLenum texenv, sf, dfac, afunc, dfunc, cullface;
  float aref;
  int cshift;
  int poffset;
  float pofactor, pounits;
  u32 cmask;
};

static void dl_get_state(struct dl_state *s) {
  s->tex2d = gl.tex2d, s->blend = gl.blend, s->atest = gl.atest, s->dtest = gl.dtest;
  s->dmask = gl.dmask, s->cull = gl.cull, s->bound = gl.bound, s->texenv = gl.texenv;
  s->sf = gl.sf, s->dfac = gl.dfac, s->afunc = gl.afunc, s->dfunc = gl.dfunc;
  s->cullface = gl.cullface, s->aref = gl.aref, s->cshift = gl.cshift;
  s->poffset = gl.poffset, s->pofactor = gl.pofactor, s->pounits = gl.pounits;
  s->cmask = gl.cmask;
}

static void dl_set_state(const struct dl_state *s) {
  pq_flush();
  st_serial++;
  gl.tex2d = s->tex2d, gl.blend = s->blend, gl.atest = s->atest, gl.dtest = s->dtest;
  gl.dmask = s->dmask, gl.cull = s->cull, gl.bound = s->bound, gl.texenv = s->texenv;
  gl.sf = s->sf, gl.dfac = s->dfac, gl.afunc = s->afunc, gl.dfunc = s->dfunc;
  gl.cullface = s->cullface, gl.aref = s->aref, gl.cshift = s->cshift;
  if (gl.poffset != s->poffset || gl.pofactor != s->pofactor || gl.pounits != s->pounits)
    mat_serial++;
  gl.poffset = s->poffset, gl.pofactor = s->pofactor, gl.pounits = s->pounits;
  gl.cmask = s->cmask;
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
   XFORM_RUN vertices at a time, so the beam racing pass timer is not held up. */
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
/* the vertices' GS coordinates and outcodes; wmin: NULL, or the smallest
   w (the nearest vertex) is taken into it */
static u32 vu0_xform_w(const u8 *src, int stride, int n, float (*vp)[4], u8 *vo,
                       const float *mats, u32 *oor, float *wmin) {
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
    if (wmin) {
      float wm = *wmin;
      for (int i = 0; i < k; i++)
        wm = vp[i][3] < wm ? vp[i][3] : wm;
      *wmin = wm;
    }
    src += k * stride, vp += k, vo += k, n -= k;
  }
  *oor = o_or;
  return oand;
}
static u32 vu0_xform(const u8 *src, int stride, int n, float (*vp)[4], u8 *vo,
                     const float *mats, u32 *oor) {
  if (!tex_bias_auto)
    return vu0_xform_w(src, stride, n, vp, vo, mats, oor, NULL);
  /* the nearest vertex: the finest level the draw's texture is sampled at */
  float wm = 3.4e38f;
  u32 oand = vu0_xform_w(src, stride, n, vp, vo, mats, oor, &wm);
  tex_need_note(k_lod, wm);
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
    o_put_pos(o_n, x, MXGL_W(adc));
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
  o_put_pos(o_n, x, MXGL_W(adc));
  o_put_st(o_n, t);
  o_col[o_n] = f->coff >= 0 ? *(const u32 *)(r + f->coff) : f->col;
  o_n++;
}
static inline __attribute__((always_inline)) int fast_culled(const struct fastv *f, const float *a,
                                                             const float *b, const float *c) {
  if (!f->cmode || vu1_clips())
    return 0;
  if (f->cmode == 3)
    return 1;
  float det = a[0] * (b[1] * c[3] - b[3] * c[1]) - a[1] * (b[0] * c[3] - b[3] * c[0]) +
              a[3] * (b[0] * c[1] - b[1] * c[0]);
  return f->cmode == 1 ? det < 0 : !(det < 0);
}

static void draw_tris_(const u32 *idx, int nidx, int nverts);
/* texture unit 1 is on (GL_ARB_multitexture): a texture, and its
   coordinates */
static int u1_on(void) {
  return gl.u1.tex2d && gl.u1.bound && gl.u1.bound < MAX_TEX && texs[gl.u1.bound].used &&
         gl.u1.ta && gl.u1.tptr;
}

/* A draw, and with texture unit 1 on, its second pass: unit 1's texture
   with its coordinates over the first pass's pixels (depth EQUAL, no depth
   writes), MODULATE as the product with the destination colour
   (blendFunc DST_COLOR ZERO: the GS's Cd * As of the texture's brightness),
   REPLACE as it is. Exact when the first pass does not blend; an alpha
   tested first pass has its holes, which the second would fill: it is
   left out then. */
/* texture unit 1's pass of a draw (draw_tris) */
static __attribute__((noinline)) void draw_unit1(const u32 *idx, int nidx, int nverts) {
  pq_flush();
  struct dl_state keep;
  dl_get_state(&keep);
  const float *tp = gl.tptr;
  int tst = gl.tstride, ta = gl.ta, ca = gl.ca;
  u32 col8 = gl.col8;
  float col[4];
  memcpy(col, gl.color, sizeof col);
  gl.tex2d = 1, gl.bound = gl.u1.bound, gl.cshift = gl.u1.cshift;
  gl.tptr = gl.u1.tptr, gl.tstride = gl.u1.tstride, gl.ta = 1;
  gl.ca = 0, gl.col8 = 0xFFFFFFFFu;
  gl.color[0] = gl.color[1] = gl.color[2] = gl.color[3] = 1;
  if (gl.u1.texenv == GL_REPLACE || gl.u1.texenv == GL_DECAL) {
    gl.blend = 0, gl.texenv = GL_REPLACE;
  } else {
    gl.blend = 1, gl.sf = GL_DST_COLOR, gl.dfac = GL_ZERO, gl.texenv = GL_MODULATE;
  }
  if (gl.dtest)
    gl.dfunc = GL_EQUAL;
  gl.dmask = 0;
  st_serial++;
  draw_tris_(idx, nidx, nverts);
  gl.tptr = tp, gl.tstride = tst, gl.ta = ta, gl.ca = ca, gl.col8 = col8;
  memcpy(gl.color, col, sizeof col);
  dl_set_state(&keep);
}

/* GL_BLEND (Cf (1 - Ct) + Cc Ct) and GL_ADD (Cf + Ct) of texture unit 0,
   which the GS's texture functions do not have: two passes. The first
   draws the fragment colour Cf without the texture; the second goes over
   its pixels (depth EQUAL, no depth writes) with the vertex colour
   constant: for BLEND the environment colour Cc, its alpha the texture's
   brightness (a brightness texture, or the brightness copy of a colour
   one, with CLUT_ENV), blended (Cc - Cd) * At + Cd: exact for grey
   textures, the brightness of a colour one's; for ADD the texture, added
   (Cd + Ct). The fragment's alpha (blending, alpha test) is the first
   pass's, without the texture's. */
static inline u32 to_u8(float c);
static __attribute__((noinline)) void draw_env(const u32 *idx, int nidx, int nverts) {
  pq_flush();
  struct dl_state keep;
  dl_get_state(&keep);
  GLenum env = gl.texenv;
  const struct tex *t = gl.bound && gl.bound < MAX_TEX && texs[gl.bound].used ? &texs[gl.bound] : NULL;
  if (t && t->alpha && !t->bright) /* Af At: the texture's alpha, white */
    gl.texenv = GL_MODULATE, env_pass = 2;
  else
    gl.tex2d = 0;
  st_serial++;
  draw_tris_(idx, nidx, nverts);
  env_pass = 0;

  pq_flush();
  int ca = gl.ca;
  u32 col8 = gl.col8;
  float col[4];
  memcpy(col, gl.color, sizeof col);
  gl.tex2d = 1, gl.texenv = GL_MODULATE, gl.atest = 0, gl.blend = 1, gl.ca = 0;
  if (env == GL_BLEND) {
    gl.sf = GL_SRC_ALPHA, gl.dfac = GL_ONE_MINUS_SRC_ALPHA;
    gl.color[0] = env_color[0], gl.color[1] = env_color[1], gl.color[2] = env_color[2];
    env_pass = 1;
  } else { /* Cd + Ct At (a brightness texture has no alpha: its CLUT's is its brightness) */
    gl.sf = t && t->alpha && !t->bright ? GL_SRC_ALPHA : GL_ONE, gl.dfac = GL_ONE;
    gl.color[0] = gl.color[1] = gl.color[2] = 1;
  }
  gl.color[3] = 1;
  gl.col8 = to_u8(gl.color[0]) | to_u8(gl.color[1]) << 8 | to_u8(gl.color[2]) << 16 | 0xFFu << 24;
  if (gl.dtest)
    gl.dfunc = GL_EQUAL;
  gl.dmask = 0;
  st_serial++;
  draw_tris_(idx, nidx, nverts);
  pq_flush();
  env_pass = 0;
  gl.ca = ca, gl.col8 = col8;
  memcpy(gl.color, col, sizeof col);
  dl_set_state(&keep);
}

static inline __attribute__((always_inline)) void draw_tris(const u32 *idx, int nidx, int nverts) {
  u32 t0 = mx_count();
  if (gl.tex2d && (gl.texenv == GL_BLEND || gl.texenv == GL_ADD))
    draw_env(idx, nidx, nverts);
  else
    draw_tris_(idx, nidx, nverts);
  if (gl.u1.tex2d && !gl.atest && u1_on())
    draw_unit1(idx, nidx, nverts);
  st_cur.gl_cycles += mx_count() - t0;
}

static int dl_compiling, dl_execute; /* glNewList .. glEndList; GL_COMPILE_AND_EXECUTE */
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
  int fog = gl.fog && !cur_prog;
  struct vprog *want = fog ? fog_prog[gl.fog_mode != GL_LINEAR] : cur_prog;
  float fa = fog ? gl.fog_a : 0, fb = fog ? gl.fog_b : 0;
#if MXGL_VU1CLIP
  /* mxglc.vsm's uniforms, when what they depend on has changed */
  static float cz[2];
  static u32 ck = ~0u;
  int vc = vu1_clips() && (k_prim & 7) == MX_GS_PRIM_TRISTRIP;
  u32 key = (u32)vc | (u32)(vc && gl.cull) << 1 | (u32)(gl.cullface == GL_FRONT) << 2;
  int cchange = key != ck || (vc && (cz[0] != zn || cz[1] != zf));
#endif
  if (o_n && (k_prim != o_prim || o_mser != m_serial || k_cs[0] != ocs[0] || k_cs[1] != ocs[1] ||
              k_cs[2] != ocs[2] || k_cs[3] != ocs[3] || o_prog != want ||
              fa != o_uni[VU_MXGLV_TMS][3] || fb != o_uni[VU_MXGLV_TMT][3]
#if MXGL_VU1CLIP
              || cchange
#endif
                  ))
    flush_out();
#if MXGL_VU1CLIP
  if (cchange) {
    union {
      float f;
      u32 u;
    } on = {.u = key & 2 ? 0x20 : 0}, sense = {.u = key & 4 ? 0 : 0x20};
    float hz = (zn - zf) * 0.5f, cc = (zn + zf) * 0.5f;
    float ih = vc ? 1.0f / hz : 0, g = vc ? 2.0f / (GB_HI - GB_LO) : 0;
    const float gc = (GB_LO + GB_HI) * 0.5f;
    const float pl[6][4] = {{0, 0, -1, zn},    {0, 0, 1, -zf},   {-1, 0, 0, GB_HI},
                            {1, 0, 0, -GB_LO}, {0, -1, 0, GB_HI}, {0, 1, 0, -GB_LO}};
    float(*cu)[4] = &o_uni[VU_MXGLV_UNI_QW];
    cu[0][0] = g, cu[0][1] = g, cu[0][2] = ih, cu[0][3] = on.f;
    cu[1][0] = -gc * g, cu[1][1] = -gc * g, cu[1][2] = -cc * ih, cu[1][3] = sense.f;
    memcpy(cu[2], pl, sizeof pl);
    ck = key, cz[0] = zn, cz[1] = zf;
  }
#endif
  o_prog = want;
  o_prim = k_prim;
  ocs[0] = k_cs[0], ocs[1] = k_cs[1], ocs[2] = k_cs[2], ocs[3] = k_cs[3];
  if (o_mser != m_serial) {
    memcpy(o_uni[VU_MXGLV_MVP], m, 64);
    tm_uniforms(o_uni[VU_MXGLV_TMS], o_uni[VU_MXGLV_TMT]);
    o_mser = m_serial;
  }
  o_uni[VU_MXGLV_TMS][3] = fa, o_uni[VU_MXGLV_TMT][3] = fb;
  return 1;
}

/* glLockArraysEXT (GL_EXT_compiled_vertex_array): the arrays do not change
   until glUnlockArraysEXT, so the vertices' GS coordinates and outcodes of
   a draw are kept for the next draws of the same arrays and matrices
   (Quake III draws a surface once a stage) */
static struct {
  int on, valid, n, vstride, vsize;
  const void *vptr, *vp;
  int count; /* the locked range's end: the draws transform it all, once */
  u32 mser, oand, oor;
} lk;

static void draw_tris_(const u32 *idx, int nidx, int nverts) {
  pq_flush();
  if (nidx < 3 || !gl.va || !gl.vptr)
    return;
  /* draws of a locked range that each use part of it: one transform for all */
  if (lk.on && lk.count > nverts)
    nverts = lk.count;
  if (dl_compiling) {
    dl_record(idx, nidx, nverts);
    if (!dl_execute)
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
      gl_free(vp_ram);
      gl_free(vo_ram);
      v_cache_n = (u32)nverts + 256;
      /* core memory: VU0 writes them with interrupts off */
      vp_ram = gl_alloc(v_cache_n * 16, 16);
      vo_ram = gl_alloc((v_cache_n + 15) & ~15u, 16);
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
  if (vu1_clips()) {
    /* not looked at: nothing outside any plane for the code below */
    memset(vo_cache, 0, (u32)nverts);
    oand = 0;
    lk.valid = 0;
    goto vu1_clipped;
  }
  /* the transform and the outcodes (bit k: outside plane k, plane_d < 0):
     on VU0 (vu0_xform), else in FPU registers, the outcodes as the sign
     bits of the distances; none while the locked arrays' are kept */
  int nxf = nverts;
#ifdef MXGL_NO_LOCK
  lk.valid = 0;
#endif
  int kept = lk.on && lk.valid && lk.vptr == (const void *)gl.vptr && lk.vstride == gl.vstride &&
             lk.vsize == gl.vsize && lk.mser == mat_serial && lk.n == nverts &&
             lk.vp == (const void *)vp_cache;
  if (kept) {
    oand = lk.oand, oor = lk.oor;
    nxf = 0;
  } else if (nxf && has_z && mats_ok) {
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
  if (gl.ucp_on && !kept)
    ucp_outcodes((const float(*)[4])vp_cache, vo_cache, nverts, &oand, &oor);
  if (!kept) {
    /* kept for the next draws if the arrays are locked; else what was kept
       is overwritten */
    lk.valid = lk.on;
    lk.vptr = gl.vptr, lk.vstride = gl.vstride, lk.vsize = gl.vsize, lk.n = nverts;
    lk.vp = vp_cache, lk.mser = mat_serial, lk.oand = oand, lk.oor = oor;
  }
vu1_clipped:
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
          fast_put(&fv, (u32)((k & 1) ? lo++ : hi--), k < 2 ? 1 : (k & 1) * 2);
      } else {
        const struct vsrc src = vsrc_get();
        for (int k = 0, lo = 1, hi = nverts - 1; k < nverts; k++) {
          int v = k == 0 ? 0 : (k & 1) ? lo++ : hi--;
          put_vertex(&src, (u32)v, k < 2 ? 1 : (k & 1) * 2);
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
      u32 pand = 0x7F, por = 0;
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
          fast_put(&fv, (u32)ic, cl ? 1 : (t & 1) * 2);
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
          put_vertex(&src, (u32)f + (u32)t + 2, cl ? 1 : (t & 1) * 2);
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
      put_vertex(&src, (u32)t + 2, c ? 1 : (t & 1) * 2);
    }
#undef TRI_CULLED
    st_cur.tris += drawn;
    goto done;
  }
  const struct vsrc src = vsrc_get();
  /* interleaved arrays (position, texture coordinates, colour in one
     record): the vertex's words from one place */
  struct fastv fv;
  const int fast = fastv_get(&fv);
#define GPUT(v, adc) (fast ? fast_put(&fv, (v), (adc)) : put_vertex(&src, (v), (adc)))
  /* room for all of the draw's triangles (three vertices each at most): no
     check a triangle */
  int room = o_n + 3u * (u32)(nidx / 3) <= o_cap;
  /* Each triangle goes on the strip so far when it can: the GS draws a
     triangle of the last three vertices whichever their order (the facing
     is tested here). With the last two vertices in it, its third; with the
     last one, the other end of an edge of it at the last (ADC: no
     triangle) and its third (a fan's next); else a strip of its own, three.
     l1, l2: the vertices before the last, and the last. */
  u32 l1 = ~0u, l2 = ~0u;
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
      if (!room && !batch_room(3))
        continue;
      if (!o_n)
        l1 = l2 = ~0u; /* a new batch: no strip */
#ifdef MXGL_NO_CHAIN
      l1 = l2 = ~0u;
#endif
      int h1 = i0 == l1 || i1 == l1 || i2 == l1, h2 = i0 == l2 || i1 == l2 || i2 == l2;
      if (h1 && h2) {
        u32 x = i0 != l1 && i0 != l2 ? i0 : i1 != l1 && i1 != l2 ? i1 : i2;
        /* the window (l1, l2, x) in the triangle's order, or the reverse */
        GPUT(x, (i0 == l1 && i1 == l2) || (i1 == l1 && i2 == l2) || (i2 == l1 && i0 == l2) ? 0 : 2);
        l1 = l2, l2 = x;
      } else if (h2) {
        /* y: after l2 in the triangle's order, z the other */
        u32 y = i0 == l2 ? i1 : i1 == l2 ? i2 : i0, z = i0 == l2 ? i2 : i1 == l2 ? i0 : i1;
        GPUT(y, 1);
        GPUT(z, 0);
        l1 = y, l2 = z;
      } else {
        GPUT(i0, 1);
        GPUT(i1, 1);
        GPUT(i2, 0);
        l1 = i1, l2 = i2;
      }
      st_cur.tris++;
    } else {
      l1 = l2 = ~0u; /* the clipped one's own strip comes between */
      room = 0;      /* its vertices were not counted */
      u32 tc0 = mx_count();
      struct cv v0, v1, v2;
      fetch(&v0, (int)i0, m), fetch(&v1, (int)i1, m), fetch(&v2, (int)i2, m);
      clip_tri(&v0, &v1, &v2, o0 | o1 | o2);
      st_cur.clipped++;
      st_cur.c_clip += mx_count() - tc0;
    }
  }
#undef GPUT
done:
  st_cur.tri_cycles += mx_count() - tt0;
}

/* ---- lines and points --------------------------------------------------------- */

/* GL_LINES, _LINE_STRIP, _LINE_LOOP, GL_POINTS: GS LINE and POINT
   primitives built on the EE (they are few: debug lines, beams), clipped
   to the planes there, and sent by DIRECT after the batches before
   (FLUSH). Vertex k is idx[k] of the GL arrays, or k itself (idx NULL). */
static int is_lp(GLenum mode) {
  return mode == GL_LINES || mode == GL_LINE_STRIP || mode == GL_LINE_LOOP || mode == GL_POINTS;
}

/* vertex i of the arrays, in GS coordinates by m */
static void lp_fetch(struct cv *v, u32 i) {
  const float *x = (const float *)((const u8 *)gl.vptr + i * (u32)gl.vstride);
  v->x[0] = x[0], v->x[1] = x[1], v->x[2] = gl.vsize > 2 ? x[2] : 0;
  for (int r = 0; r < 4; r++)
    v->p[r] = m[r] * v->x[0] + m[4 + r] * v->x[1] + m[8 + r] * v->x[2] + m[12 + r];
  if (gl.ta && gl.tptr) {
    const float *t = (const float *)((const u8 *)gl.tptr + i * (u32)gl.tstride);
    v->st[0] = t[0], v->st[1] = t[1];
  } else {
    v->st[0] = v->st[1] = 0;
  }
  if (gl.ca && gl.cptr) {
    const u8 *c = gl.cptr + i * (u32)gl.cstride;
    for (int k = 0; k < 4; k++)
      v->c[k] = c[k];
  } else {
    for (int k = 0; k < 4; k++)
      v->c[k] = gl.color[k] * 255.0f;
  }
}

#define LP_RUN 80 /* vertices a packet (3 quadwords each): it must fit a half of the frame stream (beam racing: 255 quadwords) */
static struct {
  u32 prim, n;
  float cs[4], tms[4], tmt[4];
  struct cv v[LP_RUN];
} lp;

static void lp_send(void) {
  if (!lp.n)
    return;
  u32 qwc = 1 + 3 * lp.n;
  u64 *c = mx_frame_reserve(qwc + 1);
  c = mx_e_cnt(c, qwc, mx_vif_flush(), mx_vif_direct(qwc));
  c = mx_e_giftag(c, lp.n, 1, (s32)lp.prim, MX_GIF_FLG_PACKED, 3,
                  mx_gif_regs3(MX_GIF_REG_ST, MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2));
  for (u32 k = 0; k < lp.n; k++) {
    const struct cv *v = &lp.v[k];
    float q = 1.0f / v->p[3];
    float s = lp.tms[0] * v->st[0] + lp.tmt[0] * v->st[1] + lp.tms[2];
    float t = lp.tms[1] * v->st[0] + lp.tmt[1] * v->st[1] + lp.tmt[2];
    c = mx_e_st(c, s * q, t * q, q);
    u32 col[4];
    for (int i = 0; i < 4; i++)
      col[i] = (u32)(s32)__builtin_fminf(__builtin_fmaxf(v->c[i] * lp.cs[i] + 0.5f, 0.0f), 255.0f);
    c = mx_e_rgbaq(c, col[0], col[1], col[2], col[3]);
    c = mx_e_xyz2(c, (u32)(s32)(v->p[0] * q * 16.0f), (u32)(s32)(v->p[1] * q * 16.0f),
                  (u32)(v->p[2] * q * 16.0f), 0);
  }
  mx_frame_commit(c);
  lp.n = 0;
}

static void lp_put(const struct cv *v) {
  if (lp.n == LP_RUN)
    lp_send();
  lp.v[lp.n++] = *v;
}

/* a and b cut to the planes; 0 if nothing is left */
static int lp_clip(struct cv *a, struct cv *b) {
  for (int k = 0; k < (gl.ucp_on ? 7 : 6); k++) {
    float da = plane_d(a->p, k), db = plane_d(b->p, k);
    if (da < 0 && db < 0)
      return 0;
    if (da < 0)
      lerp(a, b, a, db / (db - da));
    else if (db < 0)
      lerp(b, a, b, da / (da - db));
  }
  return 1;
}

static void draw_lp(GLenum mode, const u32 *idx, int n) {
  pq_flush();
  if (n < 1 || !gl.va || !gl.vptr || dl_compiling)
    return;
  frame_start();
  update_matrix();
  flush_out();
  if (set_state(&lp.prim, lp.cs, 0) < 0)
    return;
  int points = mode == GL_POINTS;
  /* built here with XYZ2: not fogged */
  lp.prim = (lp.prim & ~(7u | 1u << MX_GS_PRIM_FGE_SHIFT)) |
            (points ? MX_GS_PRIM_POINT : MX_GS_PRIM_LINE);
  tm_uniforms(lp.tms, lp.tmt);
  lp.n = 0;
#define LP_IX(k) (idx ? idx[k] : (u32)(k))
  if (points) {
    for (int k = 0; k < n; k++) {
      struct cv v;
      lp_fetch(&v, LP_IX(k));
      int in = 1;
      for (int p = 0; p < (gl.ucp_on ? 7 : 6) && in; p++)
        in = plane_d(v.p, p) >= 0;
      if (in)
        lp_put(&v);
    }
  } else {
    int step = mode == GL_LINES ? 2 : 1;
    int nseg = mode == GL_LINES ? n / 2 : mode == GL_LINE_LOOP && n > 2 ? n : n - 1;
    for (int sg = 0; sg < nseg; sg++) {
      int ka = sg * step, kb = ka + 1 == n ? 0 : ka + 1;
      struct cv a, b;
      lp_fetch(&a, LP_IX(ka));
      lp_fetch(&b, LP_IX(kb));
      if (!lp_clip(&a, &b))
        continue;
      if (lp.n + 2 > LP_RUN)
        lp_send();
      lp_put(&a);
      lp_put(&b);
    }
  }
#undef LP_IX
  lp_send();
  st_cur.calls++;
  /* the PRIM of the next batch comes with its GIF tag; the registers held */
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
  float (*pos)[4];  /* x, y, z, w 1: no triangle ends here (ADC); maybe the
                       entry before's (the same vertices and triangles) */
  float (*st2)[2];
  u8 (*col)[4];
  u32 n;            /* vertices: strips of the triangles */
  u16 *tri;         /* the triangles as three of these vertices each (a call
                       across a plane draws these, clipped) */
  u32 ntri;
  struct mx_chain ch;
  float bmin[3], bmax[3];
  v4f center, half; /* the bounds as centre (w 1) and half sizes */
  int planar;       /* the first triangle stands for all (culling) */
  float log2_density; /* log2 of S, T units per unit (both ways), for K; -1e9: none */
};

#define DL_MAX 4096
#define DL_POOL_BYTES (2 * 1024 * 1024)
static struct {
  struct dl_entry *first;
  struct dl_state *end; /* the GL state at its glEndList (its state calls'
                           effect), in the pool; NULL: none */
  u8 used;              /* it has been made (glEndList) */
  u8 named;             /* glGenLists gave its name */
} dlists[DL_MAX];
static int dl_cur, dl_failed;
static u32 dl_called_frame = ~0u; /* the frame (mx_gs.frames) that last called a list */
static struct dl_state dl_before; /* GL_COMPILE: the state is as before glNewList */
static struct dl_entry **dl_tail;

/* the pool in chunks, taken as the lists need them: a core heap that
   textures cut up (beam racing's records in it too) still has room for
   pieces this size where it has none for 2 MiB in one */
#define DL_CHUNK (256 * 1024)
#define DL_NCHUNK (DL_POOL_BYTES / DL_CHUNK)
static u8 *dl_chunk[DL_NCHUNK];
static u32 dl_ci, dl_coff; /* the chunk being filled, and the place in it */

static void *dl_take(u32 bytes) {
  bytes = (bytes + 63) & ~63u;
  if (bytes > DL_CHUNK)
    return NULL;
  if (dl_coff + bytes > DL_CHUNK) {
    if (dl_ci + 1 >= DL_NCHUNK)
      return NULL;
    if (!dl_chunk[dl_ci + 1] && !(dl_chunk[dl_ci + 1] = gl_alloc(DL_CHUNK, 64)))
      return NULL;
    dl_ci++, dl_coff = 0;
  }
  void *p = dl_chunk[dl_ci] + dl_coff;
  dl_coff += bytes;
  dl_pool_pos += bytes;
  return p;
}

/* all the pool's lists gone: it is filled again from its start */
static void dl_pool_reset(void) { dl_pool_pos = 0, dl_ci = 0, dl_coff = 0; }

/* the entry recorded last in the list being made, and what its vertices
   came from: the next draw of the same arrays and triangles (Quake III's
   next stage of a surface) takes its positions */
static struct dl_entry *dl_last;
static const void *dl_last_vptr;
static const u32 *dl_last_idx;
static int dl_last_vstride, dl_last_nidx, dl_last_nverts;

/* the triangles idx[0 .. nidx - 1] (of vertices below nverts) as strips:
   seq[] the vertices in order (bit 31: no triangle ends there), tri[] each
   triangle's three places in seq; returns how many in seq (at most 3 a
   triangle), *ntri the triangles */
static u32 dl_strips(const u32 *idx, int nidx, int nverts, u32 *seq, u16 *tri, u32 *ntri) {
  u32 n = 0, t = 0, l1 = ~0u, l2 = ~0u, p1 = 0, p2 = 0;
  for (int i = 0; i + 2 < nidx; i += 3) {
    u32 a = idx[i], b = idx[i + 1], c = idx[i + 2];
    if ((int)a >= nverts || (int)b >= nverts || (int)c >= nverts)
      continue;
    int h1 = a == l1 || b == l1 || c == l1, h2 = a == l2 || b == l2 || c == l2;
#ifdef MXGL_NO_LCHAIN
    h1 = h2 = 0;
#endif
    /* tri: each corner's place, in the triangle's own order (its facing:
       the EE clips and culls from these) */
#define PLACE(v, va, pa, vb, pb, pc) ((v) == (va) ? (pa) : (v) == (vb) ? (pb) : (pc))
    if (h1 && h2 && n) {
      u32 x = a != l1 && a != l2 ? a : b != l1 && b != l2 ? b : c;
      tri[3 * t] = (u16)PLACE(a, l1, p1, l2, p2, n);
      tri[3 * t + 1] = (u16)PLACE(b, l1, p1, l2, p2, n);
      tri[3 * t + 2] = (u16)PLACE(c, l1, p1, l2, p2, n);
      seq[n] = x;
      l1 = l2, p1 = p2, l2 = x, p2 = n++;
    } else if (h2 && n) {
      u32 y = a == l2 ? b : b == l2 ? c : a, z = a == l2 ? c : b == l2 ? a : b;
      tri[3 * t] = (u16)PLACE(a, l2, p2, y, n, n + 1);
      tri[3 * t + 1] = (u16)PLACE(b, l2, p2, y, n, n + 1);
      tri[3 * t + 2] = (u16)PLACE(c, l2, p2, y, n, n + 1);
      seq[n] = y | 0x80000000u, seq[n + 1] = z;
      l1 = y, p1 = n, l2 = z, p2 = n + 1;
      n += 2;
    } else {
      tri[3 * t] = (u16)n, tri[3 * t + 1] = (u16)(n + 1), tri[3 * t + 2] = (u16)(n + 2);
      seq[n] = a | 0x80000000u, seq[n + 1] = b | 0x80000000u, seq[n + 2] = c;
      l1 = b, p1 = n + 1, l2 = c, p2 = n + 2;
      n += 3;
    }
#undef PLACE
    t++;
  }
  *ntri = t;
  return n;
}

static void dl_record(const u32 *idx, int nidx, int nverts) {
  if (dl_failed)
    return;
  u32 ntri_max = 0;
  for (int i = 0; i + 2 < nidx; i += 3)
    if ((int)idx[i] < nverts && (int)idx[i + 1] < nverts && (int)idx[i + 2] < nverts)
      ntri_max++;
  if (!ntri_max)
    return;
  if (3 * ntri_max > 65535) {
    dl_failed = 1;
    return;
  }
  /* the strips (a scratch list of the vertices in order) */
  static u32 *seq;
  static u16 *trs;
  static u32 seq_cap;
  if (3 * ntri_max > seq_cap) {
    free(seq), free(trs);
    seq_cap = 3 * ntri_max + 1024;
    seq = malloc(seq_cap * 4), trs = malloc(seq_cap * 2);
    if (!seq || !trs) {
      free(seq), free(trs);
      seq = NULL, trs = NULL, seq_cap = 0;
      dl_failed = 1;
      return;
    }
  }
  u32 pad = VU_MXGLV_ALIGN;
  u32 prim = MX_GS_PRIM_TRISTRIP;
  /* the same vertices and triangles as the entry before: its positions */
#ifdef MXGL_NO_SHARE
  int share = 0 &&
#else
  int share = dl_last && dl_last_vptr == (const void *)gl.vptr &&
#endif
              dl_last_vstride == gl.vstride && dl_last_idx == idx && dl_last_nidx == nidx &&
              dl_last_nverts == nverts;
  struct dl_entry *e = dl_take(sizeof *e);
  u16 *tri = share ? dl_last->tri : dl_take(ntri_max * 6);
  if (!e || !tri) {
    dl_failed = 1;
    return;
  }
  u32 ntri, n = dl_strips(idx, nidx, nverts, seq, trs, &ntri);
  if (!share)
    memcpy(tri, trs, ntri * 6);
  float(*pos)[4] = share ? dl_last->pos : dl_take((n + pad) * 16);
  float(*st2)[2] = dl_take((n + pad) * 8);
  u8(*col)[4] = dl_take((n + pad) * 4);
  u32 chain_bytes = mx_shader_mesh_qw(&shader, prim, n) * 16;
  void *chain_mem = dl_take(chain_bytes);
  if (!pos || !st2 || !col || !chain_mem) {
    dl_failed = 1;
    return;
  }
  memset(e, 0, sizeof *e);
  dl_get_state(&e->st);
  e->pos = pos, e->st2 = st2, e->col = col, e->n = n, e->tri = tri, e->ntri = ntri;
  for (u32 o = 0; o < n; o++) {
    u32 v = seq[o] & 0x7FFFFFFFu;
    if (!share) {
      const float *x = (const float *)((const u8 *)gl.vptr + v * gl.vstride);
      pos[o][0] = x[0], pos[o][1] = x[1], pos[o][2] = gl.vsize > 2 ? x[2] : 0;
      {
        u32 w = seq[o] >> 31 ? MXGL_ADC_W : 0;
        memcpy(&pos[o][3], &w, 4);
      }
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
  if (share) {
    memcpy(e->bmin, dl_last->bmin, sizeof e->bmin), memcpy(e->bmax, dl_last->bmax, sizeof e->bmax);
    e->center = dl_last->center, e->half = dl_last->half, e->planar = dl_last->planar;
  } else {
    for (int k = 0; k < 3; k++)
      e->bmin[k] = 1e30f, e->bmax[k] = -1e30f;
    for (u32 o = 0; o < n; o++)
      for (int k = 0; k < 3; k++) {
        if (pos[o][k] < e->bmin[k])
          e->bmin[k] = pos[o][k];
        if (pos[o][k] > e->bmax[k])
          e->bmax[k] = pos[o][k];
      }
    /* one plane, one facing: the triangles' normals all along the first's,
       their corners on its plane */
    float nx = 0, ny = 0, nz = 0, d = 0;
    {
      const float *a = pos[tri[0]], *b = pos[tri[1]], *c = pos[tri[2]];
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
    for (u32 t = 0; t < ntri && e->planar; t++) {
      const float *a = pos[tri[3 * t]], *b = pos[tri[3 * t + 1]], *c = pos[tri[3 * t + 2]];
      float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
      float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
      float mx = uy * vz - uz * vy, my = uz * vx - ux * vz, mz = ux * vy - uy * vx;
      if (mx * nx + my * ny + mz * nz < 0)
        e->planar = 0;
      const float *q[3] = {a, b, c};
      for (int j = 0; j < 3; j++) {
        float dist = nx * q[j][0] + ny * q[j][1] + nz * q[j][2] - d;
        if (dist > 0.5f || dist < -0.5f)
          e->planar = 0;
      }
    }
    for (int k = 0; k < 3; k++) {
      e->center[k] = (e->bmin[k] + e->bmax[k]) * 0.5f;
      e->half[k] = (e->bmax[k] - e->bmin[k]) * 0.5f;
    }
    e->center[3] = 1, e->half[3] = 0;
  }
  /* S, T units per unit: sqrt of the texture area over the surface area */
  e->log2_density = -1e9f;
  {
    struct tex *tx = gl.tex2d && gl.bound && gl.bound < MAX_TEX ? &texs[gl.bound] : NULL;
    float at = 0, ao = 0;
    for (u32 t = 0; tx && t < ntri; t++) {
      const float *a = pos[tri[3 * t]], *b = pos[tri[3 * t + 1]], *c = pos[tri[3 * t + 2]];
      float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
      float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
      float mx = uy * vz - uz * vy, my = uz * vx - ux * vz, mz = ux * vy - uy * vx;
      ao += sqrtf(mx * mx + my * my + mz * mz);
      const float *ta = st2[tri[3 * t]], *tb = st2[tri[3 * t + 1]], *tc = st2[tri[3 * t + 2]];
      float x = (tb[0] - ta[0]) * (tc[1] - ta[1]) - (tc[0] - ta[0]) * (tb[1] - ta[1]);
      at += x < 0 ? -x : x;
    }
    if (ao > 1e-6f && at > 1e-9f)
      e->log2_density = 0.5f * fast_log2(at / ao);
  }
  if (!share)
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
  dl_last = e, dl_last_vptr = gl.vptr, dl_last_vstride = gl.vstride;
  dl_last_idx = idx, dl_last_nidx = nidx, dl_last_nverts = nverts;
}

/* Display lists (GL 1.0): glNewList .. glEndList record the draws (each
   with the GL state it is drawn in) and the state at the end; glCallList
   draws them with the matrices of then and leaves the list's end state.
   State calls between glNewList and glEndList are done at once (the draws
   recorded take the state from them); with GL_COMPILE the state is put
   back at glEndList, as GL's lists hold the calls and do not do them.
   Matrix calls in a list are not recorded. Names 1 .. DL_MAX - 1. A list
   whose draws do not all fit in the pool is empty (GL_OUT_OF_MEMORY). */
void APIENTRY glNewList(GLuint id, GLenum mode) {
  static int warned;
  if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  if (!id || id >= DL_MAX) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  if (dl_compiling) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  pq_flush();
  if (!dl_chunk[0]) /* taken by the first list (a game without lists does not pay) */
    dl_chunk[0] = gl_alloc(DL_CHUNK, 64); /* the DMAC reads it */
  if (!dl_chunk[0] && !warned++)
    mx_printf("mxgl: no memory for the display lists (%u KB): drawn without\n", DL_CHUNK / 1024);
  if (dlists[id].used)
    dl_nused--;
  dlists[id].first = NULL;
  dlists[id].end = NULL;
  dlists[id].used = 0;
  dl_tail = &dlists[id].first;
  dl_last = NULL;
  dl_cur = (int)id;
  dl_failed = !dl_chunk[0];
  dl_execute = mode == GL_COMPILE_AND_EXECUTE;
  dl_get_state(&dl_before);
  dl_compiling = 1;
}

void APIENTRY glEndList(void) {
  if (!dl_compiling) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  pq_flush();
  dl_compiling = 0;
  dlists[dl_cur].end = dl_failed ? NULL : dl_take(sizeof(struct dl_state));
  if (dlists[dl_cur].end)
    dl_get_state(dlists[dl_cur].end);
  dlists[dl_cur].used = 1;
  dl_nused++;
  if (dl_failed) {
    dlists[dl_cur].first = NULL;
    dl_nfailed++;
    gl_error(GL_OUT_OF_MEMORY);
  }
  if (!dl_execute)
    dl_set_state(&dl_before);
}

GLuint APIENTRY glGenLists(GLsizei range) {
  if (range <= 0)
    return 0;
  for (GLuint b = 1; b + (GLuint)range <= DL_MAX; b++) {
    GLuint k = 0;
    while (k < (GLuint)range && !dlists[b + k].named && !dlists[b + k].used)
      k++;
    if (k == (GLuint)range) {
      for (k = 0; k < (GLuint)range; k++)
        dlists[b + k].named = 1;
      return b;
    }
    b += k;
  }
  return 0;
}

GLboolean APIENTRY glIsList(GLuint id) {
  return id && id < DL_MAX && (dlists[id].used || dlists[id].named);
}

/* The lists go; when none is left, the pool is empty again (after the
   frames in flight, which may call them). */
void APIENTRY glDeleteLists(GLuint first, GLsizei range) {
  if (range < 0) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  for (GLuint id = first; id < first + (GLuint)range && id < DL_MAX; id++) {
    if (!id)
      continue;
    if (dlists[id].used)
      dl_nused--;
    dlists[id].first = NULL;
    dlists[id].end = NULL;
    dlists[id].used = dlists[id].named = 0;
  }
  if (!dl_nused && dl_pool_pos && !dl_compiling) {
    pq_flush();
    if (in_frame && dl_called_frame == mx_gs.frames)
      return; /* the frame being made calls them: the pool stays */
    u32 t0 = mx_count();
    while (mx_frame_done() < mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
      ;
    dl_pool_reset();
  }
}

/* deprecated: the standard calls above */
int mxgl_NewList(int id) {
  glNewList((GLuint)id, GL_COMPILE);
  return dl_compiling;
}
int mxgl_EndList(void) {
  int ok = dl_compiling && !dl_failed;
  glEndList();
  (void)glGetError();
  return ok;
}
void mxgl_DeleteAllLists(void) {
  glDeleteLists(1, DL_MAX - 1);
  if (!dl_nused) {
    u32 t0 = mx_count();
    while (mx_frame_done() < mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
      ;
    dl_pool_reset();
  }
}

/* K: log2(texels a pixel at w = 1) = log2(texels per unit) - log2(pixels
   per unit at w = 1, from the projection: lpp) */
static s32 dl_lod_k(const struct dl_entry *e, float lpp) {
  struct tex *t = gl.tex2d && gl.bound && gl.bound < MAX_TEX ? &texs[gl.bound] : NULL;
  if (!t || !t->used || t->levels < 2 || e->log2_density < -1e8f)
    return 0;
  /* texels per unit: the texture's size now (a name may get another) */
  float k = (e->log2_density + 0.5f * ((float)t->lw + (float)t->lh + tm_log2_area()) - lpp) * 16.0f;
  s32 ki = (s32)(k + (k < 0 ? -0.5f : 0.5f));
  return ki < -2048 ? -2048 : ki > 2047 ? 2047 : ki;
}

static u32 *idx_room(int n);

void APIENTRY glCallList(GLuint id) {
  pq_flush();
  if (!id || id >= DL_MAX || !dlists[id].used)
    return;
  dl_called_frame = mx_gs.frames;
  if (dl_compiling) {
    gl_error(GL_INVALID_OPERATION); /* lists in lists: not recorded */
    return;
  }
  u32 t0 = mx_count();
  frame_start();
  flush_out();
  /* the matrix of the calls, made again when the matrices, the viewport or
     the depth range change; lpp: log2 of pixels per unit at w = 1 */
  static float m[16], lpp, key[16 + 16 + 6 + 5];
  /* the planes (plane_d, the user clip plane last) as rows: plane k of
     p = M v is pl[k] . v */
  static v4f pl[7], pl_abs[7];
  float nk[16 + 16 + 6 + 5];
  memcpy(nk, gl.mv[gl.mv_top], 64);
  memcpy(nk + 16, gl.pr[gl.pr_top], 64);
  nk[32] = (float)gl.vp[0], nk[33] = (float)gl.vp[1], nk[34] = (float)gl.vp[2];
  nk[35] = (float)gl.vp[3], nk[36] = gl.dn, nk[37] = gl.df;
  nk[38] = (float)gl.ucp_on;
  memcpy(nk + 39, gl.ucp_eye, 16);
  const int nplanes = gl.ucp_on ? 7 : 6;
  if (memcmp(nk, key, sizeof key)) {
    memcpy(key, nk, sizeof key);
    /* without the polygon offset: an entry with it is drawn as any draw */
    int po = gl.poffset;
    gl.poffset = 0;
    make_mvp(m);
    gl.poffset = po;
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
      pl[6][c] = ucp[0] * col[0] + ucp[1] * col[1] + ucp[2] * col[2] + ucp[3] * col[3];
    }
    for (int k = 0; k < 7; k++)
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
    for (int k = 0; k < nplanes; k++) {
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
    if (oor || e->st.poffset) {
      /* across a plane: as any draw */
      const float *vp = gl.vptr, *tp = gl.tptr;
      const u8 *cp = gl.cptr;
      int vs = gl.vsize, vst = gl.vstride, tst = gl.tstride, cst = gl.cstride;
      int va = gl.va, ta = gl.ta, ca = gl.ca;
      gl.vptr = &e->pos[0][0], gl.vsize = 3, gl.vstride = 16, gl.va = 1;
      gl.tptr = &e->st2[0][0], gl.tstride = 8, gl.ta = 1;
      gl.cptr = &e->col[0][0], gl.cstride = 4, gl.ca = 1;
      u32 *ix = idx_room((int)(3 * e->ntri));
      for (u32 i = 0; i < 3 * e->ntri; i++)
        ix[i] = e->tri[i];
      draw_tris_(ix, (int)(3 * e->ntri), (int)e->n);
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
    static float uni[MXGL_UNI_QW][4] MX_ALIGNED(16) = {
        [VU_MXGLV_CMUL] = {1, 1, 1, 1}}; /* the colour as it is */
    static struct dl_state last_st;
    static s32 last_k;
    static u32 last_serial, last_gen;
    u32 c1 = mx_count();
    st_cur.c_bounds += c1 - c0;
    s32 k = dl_lod_k(e, lpp);
    /* its bounds' nearest point: the finest level its texture is sampled at */
    if (tex_bias_auto) {
      float wc = m[3] * e->center[0] + m[7] * e->center[1] + m[11] * e->center[2] + m[15];
      float wr = fabsf(m[3]) * e->half[0] + fabsf(m[7]) * e->half[1] + fabsf(m[11]) * e->half[2];
      tex_need_note(k, wc - wr);
    }
    /* the same state as the call before, nothing written since: the
       registers are right */
    if (!(gs_serial == last_serial && vram_gen == last_gen && k == last_k &&
          !memcmp(&last_st, &e->st, sizeof last_st))) {
      if (set_state(&prim, uni[VU_MXGLV_CSCALE], k) < 0)
        continue;
      last_st = e->st, last_k = k, last_gen = vram_gen, last_serial = gs_serial;
      prim &= ~(1u << MX_GS_PRIM_FGE_SHIFT); /* libgl's own program: no F */
    }
    memcpy(uni[VU_MXGLV_MVP], m, 64);
    tm_uniforms(uni[VU_MXGLV_TMS], uni[VU_MXGLV_TMT]);
    u32 c2 = mx_count();
    st_cur.c_state += c2 - c1;
    flush_out();
    /* libgl's own program: a list's chain calls it (the entry is in its
       batches); the colours are the recorded ones */
    const struct mx_shader *sh = vprog_use(NULL);
    const void *pu = uni;
    mx_frame_shader_call(sh, prim, pu, &e->ch);
    if (cl_batch) { /* a channel a pass (mxgl_color_light) */
      cl_regs(1);
      mx_frame_shader_call(sh, prim, pu, &e->ch);
      cl_regs(2);
      mx_frame_shader_call(sh, prim, pu, &e->ch);
      cl_regs(0);
    }
    gs_drew = 1;
    st_cur.c_call += mx_count() - c2;
    st_cur.draws++;
    st_cur.lists++;
    st_cur.tris += e->ntri;
    st_cur.verts += e->n;
  }
  /* the list's state calls had their effect */
  dl_set_state(dlists[id].end ? dlists[id].end : &keep);
  st_cur.gl_cycles += mx_count() - t0;
}
void mxgl_CallList(int id) { glCallList((GLuint)id); }

/* the triangles of n vertices of mode (vertex k is in[k], or k if in is
   NULL) as an index list in its own room; *k its length; NULL for modes
   without triangles */
static u32 *tri_buf;
static int tri_buf_n;
static const u32 *mode_tris(GLenum mode, const u32 *in, int n, int *k) {
  int need = mode == GL_QUADS ? n / 4 * 6 : n < 3 ? 0 : mode == GL_TRIANGLES ? n / 3 * 3 : 3 * (n - 2);
  *k = 0;
  if (mode != GL_TRIANGLES && mode != GL_TRIANGLE_STRIP && mode != GL_TRIANGLE_FAN &&
      mode != GL_QUADS && mode != GL_QUAD_STRIP && mode != GL_POLYGON)
    return NULL;
  if (need > tri_buf_n) {
    free(tri_buf);
    tri_buf_n = need + 1024;
    tri_buf = malloc((size_t)tri_buf_n * 4);
    if (!tri_buf) {
      tri_buf_n = 0;
      gl_error(GL_OUT_OF_MEMORY);
      return NULL;
    }
  }
#define V(j) (in ? in[j] : (u32)(j))
  u32 *o = tri_buf;
  switch (mode) {
  case GL_TRIANGLES:
    for (int j = 0; j < need; j++)
      *o++ = V(j);
    break;
  case GL_TRIANGLE_STRIP:
  case GL_QUAD_STRIP:
    for (int j = 0; j + 2 < n; j++) {
      int odd = j & 1;
      *o++ = V(j + odd), *o++ = V(j + !odd), *o++ = V(j + 2);
    }
    break;
  case GL_TRIANGLE_FAN:
  case GL_POLYGON:
    for (int j = 0; j + 2 < n; j++)
      *o++ = V(0), *o++ = V(j + 1), *o++ = V(j + 2);
    break;
  default: /* GL_QUADS */
    for (int q = 0; 4 * q + 3 < n; q++) {
      u32 a = V(4 * q), b = V(4 * q + 1), c = V(4 * q + 2), d = V(4 * q + 3);
      *o++ = a, *o++ = b, *o++ = c, *o++ = a, *o++ = c, *o++ = d;
    }
    break;
  }
#undef V
  *k = need;
  return tri_buf;
}

/* maxv: the largest index, or -1 (found here) */
static void draw_elements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices, int maxv) {
  if (gl.ebuf)
    indices = buf_ptr(gl.ebuf, (uintptr_t)indices);
  if (count < 0 || (type != GL_UNSIGNED_INT && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_BYTE)) {
    gl_error(count < 0 ? GL_INVALID_VALUE : GL_INVALID_ENUM);
    return;
  }
  /* 32-bit triangles: the game's list as it is */
  if (type == GL_UNSIGNED_INT && mode == GL_TRIANGLES) {
    const u32 *gi = indices;
    if (maxv < 0) {
      maxv = 0;
      for (int i = 0; i < count; i++)
        if ((int)gi[i] > maxv)
          maxv = (int)gi[i];
    }
    draw_tris(gi, count, maxv + 1);
    return;
  }
  u32 *ix = idx_room(count);
  if (!ix)
    return;
  if (maxv < 0) {
    maxv = 0;
    for (int i = 0; i < count; i++) {
      u32 v = type == GL_UNSIGNED_SHORT ? ((const u16 *)indices)[i]
              : type == GL_UNSIGNED_BYTE ? ((const u8 *)indices)[i]
                                        : ((const u32 *)indices)[i];
      ix[i] = v;
      if ((int)v > maxv)
        maxv = (int)v;
    }
  } else if (type == GL_UNSIGNED_INT) {
    memcpy(ix, indices, (size_t)count * 4);
  } else {
    for (int i = 0; i < count; i++)
      ix[i] = type == GL_UNSIGNED_SHORT ? ((const u16 *)indices)[i] : ((const u8 *)indices)[i];
  }
  if (is_lp(mode)) {
    draw_lp(mode, ix, count);
    return;
  }
  if (mode == GL_TRIANGLES) {
    draw_tris(ix, count, maxv + 1);
    return;
  }
  int k;
  const u32 *t = mode_tris(mode, ix, count, &k);
  if (!t) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  draw_tris(t, k, maxv + 1);
}

void APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
  draw_elements(mode, count, type, indices, -1);
}
/* GL 1.2: the indices are in start .. end: none is looked for */
void APIENTRY glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                  const GLvoid *indices) {
  if (end < start) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  draw_elements(mode, count, type, indices, (int)end);
}
void APIENTRY glDrawRangeElementsEXT(GLenum mode, GLuint start, GLuint end, GLsizei count,
                                     GLenum type, const GLvoid *indices) {
  glDrawRangeElements(mode, start, end, count, type, indices);
}

#define IMM_MAX 4096 /* vertices of a glBegin .. glEnd, glDrawArrays */

/* the index lists of the immediate modes: fixed patterns, made as large as
   the largest draw so far */
static u32 *ix_tri, *ix_strip, *ix_fan, *ix_quad;
static int ix_cap; /* vertices */
static int ix_make(int n) {
  int cap = ix_cap ? ix_cap : 256;
  while (cap < n)
    cap *= 2;
  /* core memory: the draws read them (in virtual memory each read of a
     page not in the TLB is a refill) */
  u32 *t = gl_alloc((u32)cap * 12, 16), *s = gl_alloc((u32)cap * 12, 16),
      *f = gl_alloc((u32)cap * 12, 16), *q = gl_alloc((u32)cap * 12, 16);
  if (!t || !s || !f || !q) {
    gl_free(t), gl_free(s), gl_free(f), gl_free(q);
    gl_error(GL_OUT_OF_MEMORY);
    return 0;
  }
  gl_free(ix_tri), gl_free(ix_strip), gl_free(ix_fan), gl_free(ix_quad);
  ix_tri = t, ix_strip = s, ix_fan = f, ix_quad = q, ix_cap = cap;
  for (int i = 0; i < 3 * cap; i++)
    t[i] = (u32)i;
  for (int i = 0; i + 2 < cap; i++) {
    u32 *a = &s[3 * i], *b = &f[3 * i];
    if (i & 1)
      a[0] = (u32)i + 1, a[1] = (u32)i, a[2] = (u32)i + 2;
    else
      a[0] = (u32)i, a[1] = (u32)i + 1, a[2] = (u32)i + 2;
    b[0] = 0, b[1] = (u32)i + 1, b[2] = (u32)i + 2;
  }
  for (int k = 0; 4 * k + 3 < cap; k++) {
    u32 *d = &q[6 * k], b = 4 * (u32)k;
    d[0] = b, d[1] = b + 1, d[2] = b + 2, d[3] = b, d[4] = b + 2, d[5] = b + 3;
  }
  return 1;
}

/* the index list of a mode (the fixed patterns of glEnd) and its length for
   n vertices; NULL for modes not drawn */
static inline __attribute__((always_inline)) const u32 *mode_ix(GLenum mode, int n, int *k) {
  *k = 0;
  if (n > ix_cap && !ix_make(n))
    return NULL;
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

/* a polygon's vertices (x, y, z at xyz, s, t at st) copied into the
   polygon queue (pq_buf) */
static void pq_copy(const void *xyz, int xs, const void *st, int ss, int n) {
  if (pq_n && (pq_stride != 5 || pq_st != 3 || pq_n == PQ_MAX || pq_buf_n + 5 * n > PQ_BUF_FLOATS))
    draw_polys();
  float *d = pq_buf + pq_buf_n;
  for (int i = 0; i < n; i++, d += 5) {
    const float *x = (const float *)((const u8 *)xyz + i * xs);
    const float *t = (const float *)((const u8 *)st + i * ss);
    d[0] = x[0], d[1] = x[1], d[2] = x[2], d[3] = t[0], d[4] = t[1];
  }
  pq[pq_n].v = pq_buf + pq_buf_n, pq[pq_n].n = n;
  pq_stride = 5, pq_st = 3;
  pq_n++;
  pq_buf_n += 5 * n;
}

void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  /* a polygon of one buffer object's records (Quake's world): straight
     into the polygon queue, as a pointer into the buffer */
  if (mode == GL_POLYGON && gl.vbuf && gl.vbuf == gl.tbuf && gl.va && gl.ta && !gl.ca &&
      gl.vsize == 3 && gl.vstride == gl.tstride && count >= 3 && count <= CLIP_MAX - 6 &&
      first >= 0 && !dl_compiling && !(gl.u1.tex2d && gl.u1.ta)) {
    intptr_t pst = (const u8 *)gl.tptr - (const u8 *)gl.vptr;
    if (!(gl.vstride & 3) && !(pst & 3) && pst >= 12 && pst + 8 <= gl.vstride) {
      int stride = gl.vstride / 4, st = (int)(pst / 4);
      if (pq_n && (pq_stride != stride || pq_st != st || pq_n == PQ_MAX))
        draw_polys();
      pq[pq_n].v = (const float *)((const u8 *)gl.vptr + first * gl.vstride), pq[pq_n].n = count;
      pq_stride = stride, pq_st = st;
      pq_n++;
      return;
    }
  }
  if (count < 0 || first < 0) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  int k;
  const u32 *ix = is_lp(mode) ? NULL : mode_ix(mode, count, &k);
  if (!is_lp(mode) && !ix) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  if (!gl.va || !gl.vptr || count < (mode == GL_POINTS ? 1 : is_lp(mode) ? 2 : 3))
    return;
  /* the arrays from vertex first on */
  const float *vp = gl.vptr, *tp = gl.tptr;
  const u8 *cp = gl.cptr;
  gl.vptr = (const float *)((const u8 *)vp + first * gl.vstride);
  if (tp)
    gl.tptr = (const float *)((const u8 *)tp + first * gl.tstride);
  if (cp)
    gl.cptr = cp + first * gl.cstride;
  if (is_lp(mode)) {
    draw_lp(mode, NULL, count);
    gl.vptr = vp, gl.tptr = tp, gl.cptr = cp;
    return;
  }
  /* more vertices than the fixed patterns: an index list of its own */
  if (count > IMM_MAX) {
    ix = mode_tris(mode, NULL, count, &k);
    if (ix)
      draw_tris(ix, k, count);
    gl.vptr = vp, gl.tptr = tp, gl.cptr = cp;
    return;
  }
  /* a textured polygon without colours (Quake's world) goes into the
     polygon queue, drawn with the next ones of the same state (draw_polys):
     of one buffer object (position and texture coordinates in one record),
     as a pointer into it; else copied */
  intptr_t pst = (const u8 *)gl.tptr - (const u8 *)gl.vptr;
  if (mode == GL_POLYGON && gl.vsize == 3 && gl.ta && gl.tptr && !gl.ca && !u1_on() &&
      !dl_compiling && count <= CLIP_MAX - 6 && count <= SPR_VERTS && gl.vbuf &&
      gl.vbuf == gl.tbuf && gl.vstride == gl.tstride && !(gl.vstride & 3) && !(pst & 3) &&
      pst >= 12 && pst + 8 <= gl.vstride) {
    int stride = gl.vstride / 4, st = (int)(pst / 4);
    if (pq_n && (pq_stride != stride || pq_st != st || pq_n == PQ_MAX))
      draw_polys();
    pq[pq_n].v = gl.vptr, pq[pq_n].n = count;
    pq_stride = stride, pq_st = st;
    pq_n++;
    gl.vptr = vp, gl.tptr = tp, gl.cptr = cp;
    return;
  }
  if (mode == GL_POLYGON && gl.vsize == 3 && gl.ta && gl.tptr && !gl.ca && !u1_on() &&
      !dl_compiling && count <= CLIP_MAX - 6 && count <= SPR_VERTS) {
    pq_copy(gl.vptr, gl.vstride, gl.tptr, gl.tstride, count);
    gl.vptr = vp, gl.tptr = tp, gl.cptr = cp;
    return;
  }
  draw_polygon = mode == GL_POLYGON;
  draw_mode = mode;
  draw_tris(ix, k, count);
  draw_polygon = 0;
  draw_mode = 0;
  gl.vptr = vp, gl.tptr = tp, gl.cptr = cp;
}

/* n primitives of the arrays, primitive i mode[i] (strips, fans, polygons
   as fans, triangles) of count[i] vertices from first[i]: one draw (the
   vertices transformed once); other modes, or more vertices than
   IMM_MAX, one by one */
static void multi_draw(const GLenum *mode, const GLint *first, const GLsizei *count, int n) {
  if (n <= 0 || !gl.va || !gl.vptr)
    return;
  int nv = 0, ok = 1;
  for (int i = 0; i < n; i++) {
    GLenum md = mode[i];
    if (first[i] < 0 || count[i] < 0)
      ok = 0;
    if (md != GL_TRIANGLE_STRIP && md != GL_TRIANGLE_FAN && md != GL_TRIANGLES && md != GL_POLYGON)
      ok = 0;
    if (first[i] + count[i] > nv)
      nv = first[i] + count[i];
  }
  if (!ok || nv > IMM_MAX) {
    for (int i = 0; i < n; i++)
      glDrawArrays(mode[i], first[i], count[i]);
    return;
  }
  if (nv < 3)
    return;
  /* polygons are fans (convex: GL's) */
  static GLenum md[512];
  const GLenum *mp = mode;
  for (int i = 0; i < n; i++)
    if (mode[i] == GL_POLYGON) {
      for (int b = 0; b < n; b += 512) {
        int c = n - b < 512 ? n - b : 512;
        for (int j = 0; j < c; j++)
          md[j] = mode[b + j] == GL_POLYGON ? GL_TRIANGLE_FAN : mode[b + j];
        multi_draw(md, first + b, count + b, c);
      }
      return;
    }
  multi.mode = mp, multi.first = first, multi.count = count, multi.n = n;
  int k;
  const u32 *ix = mode_ix(GL_TRIANGLES, nv, &k); /* for the LOD estimate */
  draw_tris(ix, k, nv);
  multi.n = 0;
}

void APIENTRY glMultiDrawArrays(GLenum mode, const GLint *first, const GLsizei *count, GLsizei n) {
  /* polygons: each its own (the polygon queue takes them, many to a draw) */
  if (mode == GL_POLYGON) {
    for (int i = 0; i < n; i++)
      glDrawArrays(mode, first[i], count[i]);
    return;
  }
  static GLenum md[512];
  for (int b = 0; b < n; b += 512) {
    int c = n - b < 512 ? n - b : 512;
    for (int i = 0; i < c; i++)
      md[i] = mode;
    multi_draw(md, first + b, count + b, c);
  }
}
void APIENTRY glMultiDrawArraysEXT(GLenum mode, const GLint *first, const GLsizei *count,
                                   GLsizei n) {
  glMultiDrawArrays(mode, first, count, n);
}
/* GL_IBM_multimode_draw_arrays: a mode each (modestride bytes apart) */
void APIENTRY glMultiModeDrawArraysIBM(const GLenum *mode, const GLint *first, const GLsizei *count,
                                       GLsizei n, GLint modestride) {
  if (modestride == (GLint)sizeof(GLenum) || modestride == 0) {
    multi_draw(mode, first, count, n);
    return;
  }
  static GLenum md[512];
  for (int b = 0; b < n; b += 512) {
    int c = n - b < 512 ? n - b : 512;
    for (int i = 0; i < c; i++)
      md[i] = *(const GLenum *)((const u8 *)mode + (b + i) * modestride);
    multi_draw(md, first + b, count + b, c);
  }
}
void APIENTRY glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type,
                                  const void *const *indices, GLsizei n) {
  for (int i = 0; i < n; i++)
    glDrawElements(mode, count[i], type, indices[i]);
}

/* deprecated: glMultiModeDrawArraysIBM */
void mxgl_MultiDraw(const GLenum *mode, const int *first, const int *count, int n) {
  multi_draw(mode, first, count, n);
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
  pq_buf_n = 0;
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
  lk.valid = 0;
  int k;
  const u32 *ix = mode_ix(GL_POLYGON, pq[0].n, &k);
  struct fastv fv;
  /* the nearest vertex of the polygons drawn, noted once (they share the
     texture and its LOD) */
  float wmin = 3.4e38f, wpoly;
  const int vc = vu1_clips();
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
      wpoly = 3.4e38f;
      if (vc) {
        oand = 0;
      } else if (mats_ok) {
        oand = vu0_xform_w(vb, vs, n, vp_cache, vo_cache, mats, &oor, tex_bias_auto ? &wpoly : NULL);
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
      if (gl.ucp_on)
        ucp_outcodes((const float(*)[4])vp_cache, vo_cache, n, &oand, &oor);
      if (oand)
        continue;
      wmin = wpoly < wmin ? wpoly : wmin;
      st_poly((const float *)vb + pq_st);
      if (!oor) {
        if (gl.cull && !vc) {
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
          fast_put(&fv, (u32)((q & 1) ? lo++ : hi--), q < 2 ? 1 : (q & 1) * 2);
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
    if (tex_bias_auto && wmin < 3.4e38f)
      tex_need_note(k_lod, wmin);
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
  lk.valid = 0;
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
  if (gl.ucp_on)
    ucp_outcodes((const float(*)[4])vp_cache, vo_cache, n, &oand, &oor);
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
      gl_free(vp_ram);
      gl_free(vo_ram);
      v_cache_n = (u32)nv + 256;
      /* core memory: VU0 writes them with interrupts off */
      vp_ram = gl_alloc(v_cache_n * 16, 16);
      vo_ram = gl_alloc((v_cache_n + 15) & ~15u, 16);
    }
    vp_cache = vp_ram, vo_cache = vo_ram;
  }
  u32 oor;
  const int vc = vu1_clips();
  if (vc)
    memset(vo_cache, 0, (u32)nv);
  else if (xform_all((const u8 *)pos, stride, nv, &oor))
    goto out;
  const int cmode = !gl.cull || vc ? 0 : gl.cullface == GL_FRONT_AND_BACK ? 3 : gl.cullface == GL_FRONT ? 1 : 2;
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
    u32 pa = 0x7F, po = 0;
    for (int i = 0; i < cnt; i++)
      pa &= vo_cache[e[3 * i + 2]], po |= vo_cache[e[3 * i + 2]];
    if (pa)
      continue;
#define GV(i) ((u32)e[3 * (i) + 2])
#define PUT(i, adc)                                                                               \
  do {                                                                                            \
    o_put_pos(o_n, (const u8 *)pos + GV(i) * (u32)stride, MXGL_W(adc));             \
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
        PUT(ic, cl ? 1 : (t & 1) * 2);
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
  struct imm_v {
    float xyz[3];
    float st[2];
    u32 col;
  } *v; /* imm_small, or a larger one taken (core memory: VU0 reads it) */
  int cap;
  float cur_st[2], cur_st1[2];
  float (*st1)[2]; /* texture unit 1's (malloc'ed by its first glMultiTexCoord) */
} imm;

#define IMM_SMALL 1024
static struct imm_v imm_small[IMM_SMALL] MX_ALIGNED(64);

void APIENTRY glBegin(GLenum mode) {
  imm.mode = mode;
  imm.n = 0;
  if (!imm.v)
    imm.v = imm_small, imm.cap = IMM_SMALL;
}

/* more vertices than the room: a room twice as large (IMM_MAX at most) */
static int imm_grow(void) {
  int cap = imm.cap * 2;
  if (cap > IMM_MAX)
    return 0;
  struct imm_v *v = gl_alloc((u32)cap * sizeof *v, 64);
  if (!v) {
    gl_error(GL_OUT_OF_MEMORY);
    return 0;
  }
  memcpy(v, imm.v, (size_t)imm.n * sizeof *v);
  if (imm.v != imm_small)
    gl_free(imm.v);
  imm.v = v, imm.cap = cap;
  return 1;
}

static inline void imm_vertex(float x, float y, float z) {
  if (imm.n >= imm.cap && !imm_grow())
    return;
  int i = imm.n++;
  imm.v[i].xyz[0] = x, imm.v[i].xyz[1] = y, imm.v[i].xyz[2] = z;
  imm.v[i].st[0] = imm.cur_st[0], imm.v[i].st[1] = imm.cur_st[1];
  imm.v[i].col = gl.col8;
  if (imm.st1)
    imm.st1[i][0] = imm.cur_st1[0], imm.st1[i][1] = imm.cur_st1[1];
}

void APIENTRY glEnd(void) {
  int n = imm.n;
  int lp_mode = is_lp(imm.mode);
  if (n < (lp_mode ? 1 : 3))
    return;
  int k = 0;
  /* a polygon of the current colour (Quake's water and sky, subdivided
     into many): into the polygon queue, drawn with the next ones */
  if (imm.mode == GL_POLYGON && !imm.st1 && !dl_compiling && n <= CLIP_MAX - 6 &&
      n <= SPR_VERTS && !u1_on()) {
    int same = 1;
    for (int i = 0; i < n && same; i++)
      same = imm.v[i].col == gl.col8;
    if (same) {
      pq_copy(imm.v[0].xyz, sizeof imm.v[0], imm.v[0].st, sizeof imm.v[0], n);
      return;
    }
  }
  const u32 *ix = lp_mode ? NULL : mode_ix(imm.mode, n, &k);
  if (!ix && !lp_mode)
    return;
  /* draw from the immediate arrays */
  const float *vp = gl.vptr, *tp = gl.tptr;
  const u8 *cp = gl.cptr;
  int vs = gl.vsize, vst = gl.vstride, tst = gl.tstride, cst = gl.cstride;
  int va = gl.va, ta = gl.ta, ca = gl.ca;
  gl.vptr = imm.v[0].xyz, gl.vsize = 3, gl.vstride = sizeof imm.v[0], gl.va = 1;
  gl.tptr = imm.v[0].st, gl.tstride = sizeof imm.v[0], gl.ta = 1;
  gl.cptr = (const u8 *)&imm.v[0].col, gl.cstride = sizeof imm.v[0], gl.ca = 1;
  const float *u1p = gl.u1.tptr;
  int u1s = gl.u1.tstride, u1a = gl.u1.ta;
  if (imm.st1)
    gl.u1.tptr = imm.st1[0], gl.u1.tstride = 8, gl.u1.ta = 1;
  else
    gl.u1.ta = 0;
  if (lp_mode) {
    draw_lp(imm.mode, NULL, n);
  } else {
    draw_polygon = imm.mode == GL_POLYGON;
    draw_mode = imm.mode;
    draw_tris(ix, k, n);
    draw_polygon = 0;
    draw_mode = 0;
  }
  gl.vptr = vp, gl.tptr = tp, gl.cptr = cp;
  gl.vsize = vs, gl.vstride = vst, gl.tstride = tst, gl.cstride = cst;
  gl.va = va, gl.ta = ta, gl.ca = ca;
  gl.u1.tptr = u1p, gl.u1.tstride = u1s, gl.u1.ta = u1a;
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
    if (gl.cunit)
      gl.u1.tex2d = on;
    else
      gl.tex2d = on;
    break;
  case GL_SHARED_TEXTURE_PALETTE_EXT: gl.shared_pal = on; break;
  case GL_BLEND: gl.blend = on; break;
  case GL_ALPHA_TEST: gl.atest = on; break;
  case GL_DEPTH_TEST: gl.dtest = on; break;
  case GL_CULL_FACE: gl.cull = on; break;
  case GL_SCISSOR_TEST: gl.scissor = on; break;
  case GL_POLYGON_OFFSET_FILL:
    if (gl.poffset != on)
      mat_serial++;
    gl.poffset = on;
    break;
  case GL_CLIP_PLANE0:
    if (gl.ucp_on != on)
      mat_serial++;
    gl.ucp_on = on;
    break;
  /* state the GS has not, or that changes no picture here */
  case GL_FOG: gl.fog = on; break;
  case GL_DITHER: case GL_LIGHTING: case GL_NORMALIZE: case GL_LINE_SMOOTH:
  case GL_STENCIL_TEST: gl.st_on = on; break;
  case GL_POINT_SMOOTH: case GL_POLYGON_SMOOTH: case GL_COLOR_MATERIAL:
  case GL_TEXTURE_GEN_S: case GL_TEXTURE_GEN_T: case GL_POLYGON_OFFSET_LINE:
  case GL_POLYGON_OFFSET_POINT: case GL_TEXTURE_1D: case GL_LIGHT0:
    break;
  default: gl_error(GL_INVALID_ENUM); break;
  }
}
void APIENTRY glEnable(GLenum cap) { set_cap(cap, 1); }
void APIENTRY glDisable(GLenum cap) { set_cap(cap, 0); }

static void set_client(GLenum cap, int on) {
  if (cap == GL_VERTEX_ARRAY)
    gl.va = on;
  else if (cap == GL_TEXTURE_COORD_ARRAY && gl.ccunit)
    gl.u1.ta = on;
  else if (cap == GL_TEXTURE_COORD_ARRAY)
    gl.ta = on;
  else if (cap == GL_COLOR_ARRAY)
    gl.ca = on;
}
void APIENTRY glEnableClientState(GLenum cap) { set_client(cap, 1); }
void APIENTRY glDisableClientState(GLenum cap) { set_client(cap, 0); }

/* ---- buffer objects (GL 1.5, OpenGL ES 1.1) ------------------------------- */

/* A buffer's data is core memory (VU0 reads the vertices with interrupts
   off). It changes only by glBufferData and glBufferSubData, which draw
   the polygons queued first: draws of buffer data are queued as pointers
   into it, not copied (Quake's world). */
#define MAX_BUF 256
static struct {
  u8 used;
  u8 *data;
  u32 size;
} bufs[MAX_BUF];

/* where an array's pointer points: its buffer's data + offset, or the pointer */
static const void *buf_ptr(GLuint b, uintptr_t off) {
  return b && b < MAX_BUF && bufs[b].data ? (const void *)(bufs[b].data + off) : (const void *)off;
}
/* the arrays' pointers again (a buffer's data moved) */
static void buf_arrays(void) {
  if (gl.vbuf)
    gl.vptr = buf_ptr(gl.vbuf, gl.voff);
  if (gl.tbuf)
    gl.tptr = buf_ptr(gl.tbuf, gl.toff);
  if (gl.cbuf)
    gl.cptr = buf_ptr(gl.cbuf, gl.coff);
  if (gl.u1buf)
    gl.u1.tptr = buf_ptr(gl.u1buf, gl.u1off);
}

void APIENTRY glGenBuffers(GLsizei n, GLuint *b) {
  for (int i = 0; i < n; i++) {
    b[i] = 0;
    for (GLuint k = 1; k < MAX_BUF; k++)
      if (!bufs[k].used) {
        bufs[k].used = 1, bufs[k].data = NULL, bufs[k].size = 0;
        b[i] = k;
        break;
      }
    if (!b[i])
      gl_error(GL_OUT_OF_MEMORY);
  }
}
void APIENTRY glBindBuffer(GLenum target, GLuint b) {
  if (b >= MAX_BUF) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  if (b && !bufs[b].used)
    bufs[b].used = 1; /* a name not from glGenBuffers is made by binding it */
  if (target == GL_ARRAY_BUFFER)
    gl.abuf = b;
  else if (target == GL_ELEMENT_ARRAY_BUFFER)
    gl.ebuf = b;
  else
    gl_error(GL_INVALID_ENUM);
}
static GLuint *buf_target(GLenum target) {
  return target == GL_ARRAY_BUFFER ? &gl.abuf : target == GL_ELEMENT_ARRAY_BUFFER ? &gl.ebuf : NULL;
}
void APIENTRY glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage) {
  GLuint *bp = buf_target(target);
  if (!bp || !*bp || size < 0) {
    gl_error(!bp ? GL_INVALID_ENUM : *bp ? GL_INVALID_VALUE : GL_INVALID_OPERATION);
    return;
  }
  pq_flush(); /* queued polygons may point into it */
  GLuint b = *bp;
  u8 *d = size ? gl_alloc(((u32)size + 63) & ~63u, 64) : NULL;
  if (size && !d) {
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  if (d && data)
    memcpy(d, data, (size_t)size);
  defer_free(bufs[b].data);
  bufs[b].data = d, bufs[b].size = (u32)size;
  buf_arrays();
}
void APIENTRY glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const GLvoid *data) {
  GLuint *bp = buf_target(target);
  if (!bp || !*bp || off < 0 || size < 0 || (u32)(off + size) > bufs[*bp].size) {
    gl_error(!bp ? GL_INVALID_ENUM : GL_INVALID_VALUE);
    return;
  }
  pq_flush();
  memcpy(bufs[*bp].data + off, data, (size_t)size);
}
void APIENTRY glDeleteBuffers(GLsizei n, const GLuint *b) {
  pq_flush();
  for (int i = 0; i < n; i++) {
    GLuint k = b[i];
    if (!k || k >= MAX_BUF)
      continue;
    defer_free(bufs[k].data);
    bufs[k].used = 0, bufs[k].data = NULL, bufs[k].size = 0;
    if (gl.abuf == k)
      gl.abuf = 0;
    if (gl.ebuf == k)
      gl.ebuf = 0;
  }
}
GLboolean APIENTRY glIsBuffer(GLuint b) { return b && b < MAX_BUF && bufs[b].used; }
void APIENTRY glGenBuffersARB(GLsizei n, GLuint *b) { glGenBuffers(n, b); }
void APIENTRY glBindBufferARB(GLenum t, GLuint b) { glBindBuffer(t, b); }
void APIENTRY glBufferDataARB(GLenum t, GLsizeiptr s, const GLvoid *d, GLenum u) {
  glBufferData(t, s, d, u);
}
void APIENTRY glBufferSubDataARB(GLenum t, GLintptr o, GLsizeiptr s, const GLvoid *d) {
  glBufferSubData(t, o, s, d);
}
void APIENTRY glDeleteBuffersARB(GLsizei n, const GLuint *b) { glDeleteBuffers(n, b); }
GLboolean APIENTRY glIsBufferARB(GLuint b) { return glIsBuffer(b); }

/* the array pointers: offsets into the bound GL_ARRAY_BUFFER, if one is */
void APIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
  gl.vbuf = gl.abuf, gl.voff = (uintptr_t)ptr;
  gl.vptr = buf_ptr(gl.vbuf, gl.voff), gl.vsize = size, gl.vstride = stride ? stride : size * 4;
}
void APIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
  if (gl.ccunit) {
    gl.u1buf = gl.abuf, gl.u1off = (uintptr_t)ptr;
    gl.u1.tptr = buf_ptr(gl.u1buf, gl.u1off), gl.u1.tstride = stride ? stride : size * 4;
  } else {
    gl.tbuf = gl.abuf, gl.toff = (uintptr_t)ptr;
    gl.tptr = buf_ptr(gl.tbuf, gl.toff), gl.tstride = stride ? stride : size * 4;
  }
}
void APIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
  gl.cbuf = gl.abuf, gl.coff = (uintptr_t)ptr;
  gl.cptr = buf_ptr(gl.cbuf, gl.coff), gl.cstride = stride ? stride : size;
}

void APIENTRY glMatrixMode(GLenum mode) {
  if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  gl.mode = mode;
}
void APIENTRY glLoadIdentity(void) { mat_ident(cur_mat()); }
void APIENTRY glLoadMatrixf(const GLfloat *m) { memcpy(cur_mat(), m, 64); }
/* the current mode's stack: its matrices, top and depth */
static float (*mat_stack(int **top, int *depth))[16] {
  if (gl.mode == GL_PROJECTION)
    return *top = &gl.pr_top, *depth = PR_DEPTH, gl.pr;
  if (gl.mode == GL_TEXTURE)
    return *top = &gl.tm_top, *depth = TM_DEPTH, gl.tm;
  return *top = &gl.mv_top, *depth = MV_DEPTH, gl.mv;
}
void APIENTRY glPushMatrix(void) {
  int *top, depth;
  float(*st)[16] = mat_stack(&top, &depth);
  if (*top + 1 >= depth) {
    gl_error(GL_STACK_OVERFLOW);
    return;
  }
  memcpy(st[*top + 1], st[*top], 64);
  (*top)++;
}
void APIENTRY glPopMatrix(void) {
  int *top, depth;
  mat_stack(&top, &depth);
  if (!*top) {
    gl_error(GL_STACK_UNDERFLOW);
    return;
  }
  pq_flush();
  mat_serial++;
  (*top)--;
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
/* sine and cosine of r on the FPU (libm's are software floating point):
   r to [-pi, pi], then [-pi/2, pi/2], Taylor series (error < 1e-7) */
static void fsincos(float r, float *ps, float *pc) {
  float q = r * 0.159154943f;
  q = (float)(int)(q + (q >= 0 ? 0.5f : -0.5f));
  r = r - q * 6.28318548f + q * 1.74845553e-7f;
  float cs = 1;
  if (r > 1.57079633f)
    r = 3.14159265f - r, cs = -1;
  else if (r < -1.57079633f)
    r = -3.14159265f - r, cs = -1;
  float r2 = r * r;
  *ps = r * (1 + r2 * (-1.0f / 6 + r2 * (1.0f / 120 + r2 * (-1.0f / 5040 + r2 * (1.0f / 362880 +
                                                                                r2 * (-1.0f / 39916800))))));
  *pc = cs * (1 + r2 * (-0.5f + r2 * (1.0f / 24 + r2 * (-1.0f / 720 + r2 * (1.0f / 40320 +
                                                                          r2 * (-1.0f / 3628800 +
                                                                                r2 * (1.0f / 479001600)))))));
}

void APIENTRY glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) {
  float l = sqrtf(x * x + y * y + z * z);
  if (l == 0)
    return;
  x /= l, y /= l, z /= l;
  float c, s;
  fsincos(a * (3.14159265f / 180.0f), &s, &c);
  float k = 1 - c;
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
/* the fog's a and b (mxglvf.hlsl, mxglve.hlsl) */
static void fog_update(void) {
  if (gl.fog_mode == GL_LINEAR) {
    float d = gl.fog_end - gl.fog_start;
    gl.fog_a = d != 0 ? -255.0f / d : 0;
    gl.fog_b = d != 0 ? 255.0f * gl.fog_end / d : 255.0f;
  } else {
    gl.fog_a = gl.fog_density;
    gl.fog_b = gl.fog_mode == GL_EXP2;
  }
}
static u32 fog_byte(float c) { return (u32)((c < 0 ? 0 : c > 1 ? 1 : c) * 255.0f + 0.5f); }
void APIENTRY glFogfv(GLenum p, const GLfloat *v) {
  pq_flush();
  st_serial++;
  switch (p) {
  case GL_FOG_MODE:
    if (v[0] != GL_LINEAR && v[0] != GL_EXP && v[0] != GL_EXP2) {
      gl_error(GL_INVALID_ENUM);
      return;
    }
    gl.fog_mode = (GLenum)v[0];
    break;
  case GL_FOG_DENSITY: gl.fog_density = v[0]; break;
  case GL_FOG_START: gl.fog_start = v[0]; break;
  case GL_FOG_END: gl.fog_end = v[0]; break;
  case GL_FOG_COLOR:
    gl.fog_col = fog_byte(v[0]) | fog_byte(v[1]) << 8 | fog_byte(v[2]) << 16;
    break;
  case GL_FOG_INDEX: break;
  default: gl_error(GL_INVALID_ENUM); return;
  }
  fog_update();
}
void APIENTRY glFogf(GLenum p, GLfloat v) { glFogfv(p, &v); }
void APIENTRY glFogi(GLenum p, GLint v) {
  GLfloat f = (GLfloat)v;
  glFogfv(p, &f);
}
void APIENTRY glFogiv(GLenum p, const GLint *v) {
  GLfloat f[4] = {(GLfloat)v[0], 0, 0, 0};
  if (p == GL_FOG_COLOR)
    for (int i = 0; i < 3; i++) /* 0 .. INT_MAX is 0 .. 1 */
      f[i] = (GLfloat)v[i] * (1.0f / 2147483647.0f);
  glFogfv(p, f);
}
void APIENTRY glAlphaFunc(GLenum f, GLclampf r) { pq_flush(), gl.afunc = f, gl.aref = r, st_serial++; }
void APIENTRY glDepthFunc(GLenum f) { pq_flush(), gl.dfunc = f, st_serial++; }
void APIENTRY glDepthMask(GLboolean m) { pq_flush(), gl.dmask = m, st_serial++; }
/* gl.cullface is the face culled in counter-clockwise terms (culled()):
   glCullFace's, the other one under glFrontFace(GL_CW) */
static GLenum cull_mode = GL_BACK;
static int front_cw;
static void cull_set(void) {
  pq_flush();
  gl.cullface = !front_cw || cull_mode == GL_FRONT_AND_BACK ? cull_mode
                : cull_mode == GL_FRONT                    ? GL_BACK
                                                           : GL_FRONT;
  st_serial++;
}
void APIENTRY glCullFace(GLenum m) { cull_mode = m, cull_set(); }
void APIENTRY glFrontFace(GLenum m) { front_cw = m == GL_CW, cull_set(); }
/* each unit's GL_COMBINE, COMBINE_RGB, RGB_SCALE (0: 1, 1: 2), and unit 0's
   mxgl_ColorShift */
static int env_combine[2], env_scale[2], env_legacy;
static GLenum env_combine_rgb[2] = {GL_MODULATE, GL_MODULATE};
/* the colour shift of each unit's draws: RGB_SCALE in COMBINE mode (GL
   takes it only there), else mxgl_ColorShift's */
static void env_shifts(void) {
  gl.cshift = env_combine[0] ? env_scale[0] : env_legacy;
  gl.u1.cshift = env_combine[1] ? env_scale[1] : 0;
}

/* The texture environment of the active unit: MODULATE, REPLACE, DECAL,
   and COMBINE as one of them (COMBINE_RGB MODULATE or REPLACE; the GS has
   no others); RGB_SCALE 1 or 2 (4 is 2): the GS's modulate takes the
   colour up to 2x (0x80 is 1.0). */
void APIENTRY glTexEnvf(GLenum t, GLenum p, GLfloat v) {
  pq_flush();
  st_serial++;
  GLenum *env = gl.cunit ? &gl.u1.texenv : &gl.texenv;
  int u = gl.cunit;
  switch (p) {
  case GL_TEXTURE_ENV_MODE:
    env_combine[u] = (GLenum)v == GL_COMBINE;
    *env = env_combine[u] ? env_combine_rgb[u] : (GLenum)v;
    break;
  case GL_COMBINE_RGB:
    env_combine_rgb[u] = (GLenum)v == GL_REPLACE ? GL_REPLACE : GL_MODULATE;
    if (env_combine[u])
      *env = env_combine_rgb[u];
    break;
  case GL_RGB_SCALE: env_scale[u] = v > 1.5f; break;
  case GL_COMBINE_ALPHA: case GL_ALPHA_SCALE: case GL_SOURCE0_RGB: case GL_SOURCE1_RGB:
  case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_TEXTURE_ENV_COLOR: break;
  default: gl_error(GL_INVALID_ENUM); break;
  }
  env_shifts();
}
void APIENTRY glTexEnvi(GLenum t, GLenum p, GLint v) { glTexEnvf(t, p, (GLfloat)v); }
void APIENTRY glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
  gl.clear[0] = r, gl.clear[1] = g, gl.clear[2] = b, gl.clear[3] = a;
}

static void clear_cz(GLbitfield mask);
/* the alpha bit to 0 where a sprite covers: all of the buffer but for the scissor */
static void clear_stencil(void) {
  int x0 = 0, x1 = fb_w, y0 = 0, y1 = fb_h;
  if (gl.scissor) {
    x0 = gl.sc[0] * fb_w / gl_w, x1 = (gl.sc[0] + gl.sc[2]) * fb_w / gl_w;
    y0 = (gl_h - (gl.sc[1] + gl.sc[3])) * fb_h / gl_h, y1 = (gl_h - gl.sc[1]) * fb_h / gl_h;
  }
  /* beam: context 2, without Z writes (ZBUF and SCISSOR are the passes') */
  int ctx = beam != 0;
  u32 n = beam ? 7 : 9;
  u64 *c = mx_frame_reserve(2 + n);
  c = mx_e_cnt(c, n + 1, mx_vif_flush(), mx_vif_direct(n + 1));
  c = mx_e_ad_tag(c, n, 1);
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_TEST_1, ctx), mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_FRAME_1, ctx), frame_reg(0x00FFFFFFu));
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_FBA_1, ctx), 0);
  if (!beam) {
    c = mx_e_ad(c, MX_GS_REG_ZBUF_1, mx_gs_zbuf(rt[buf].zbp, rt[buf].zpsm, 1));
    c = mx_e_ad(c, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, fb_w - 1, 0, fb_h - 1));
  }
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, ctx, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0, 0, 0, 0, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x0), mx_gs_py((u32)y0), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x1), mx_gs_py((u32)y1), 0));
  mx_frame_commit(c);
  gs.valid = 0, gs_serial++;
}
void APIENTRY glClear(GLbitfield mask) {
  clear_cz(mask);
  if ((mask & GL_STENCIL_BUFFER_BIT) && st_there()) {
    frame_start();
    flush_out();
    clear_stencil();
    gl.st_va = (int)(gl.st_clear & 255), gl.st_vb = -1;
    st_serial++;
  }
}
static void clear_cz(GLbitfield mask) {
  pq_flush();
  int was = in_frame;
  frame_start();
  flush_out();
  if (!was && (mask & GL_COLOR_BUFFER_BIT))
    return; /* mx_frame_begin cleared colour and Z */
  if (!(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)))
    return;
  if (beam) {
  /* a sprite over the scissor area (or all) in the passes' window: Z 0
     with context 1, without (ZMSK) with context 2; colour, or the
     destination's colour again (blend Cd) when only Z is cleared */
  int x0 = 0, x1 = fb_w, y0 = 0, y1 = fb_h;
  if (gl.scissor) {
    x0 = gl.sc[0] * fb_w / gl_w, x1 = (gl.sc[0] + gl.sc[2]) * fb_w / gl_w;
    y0 = (gl_h - (gl.sc[1] + gl.sc[3])) * fb_h / gl_h, y1 = (gl_h - gl.sc[1]) * fb_h / gl_h;
  }
  int z = (mask & GL_DEPTH_BUFFER_BIT) != 0, col = (mask & GL_COLOR_BUFFER_BIT) != 0;
  u64 *c = mx_frame_reserve(2 + 9);
  c = mx_e_cnt(c, 9, mx_vif_flush(), mx_vif_direct(9));
  c = mx_e_ad_tag(c, 8, 1);
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_FRAME_1, !z),
              frame_reg(col ? (gl.cmask & 0x00FFFFFFu) | 0x80000000u : 0x80000000u));
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
  /* the write masks hold for glClear too (glColorMask, glDepthMask) */
  c = mx_e_ad(c, MX_GS_REG_ZBUF_1,
              mx_gs_zbuf(t->zbp, t->zpsm, !(mask & GL_DEPTH_BUFFER_BIT) || !gl.dmask));
  c = mx_e_ad(c, MX_GS_REG_FRAME_1,
              mx_gs_frame(t->fbp, t->fbw, t->psm,
                          (mask & GL_COLOR_BUFFER_BIT) ? gl.cmask | 0x80000000u : ~0u));
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
/* beam: a record is played until a later one replaces it, which may be
   MX_HIRES_SLOTS frames later */
#define NDEFER (MX_HIRES_SLOTS + 1)
static void **deferred[NDEFER];
static int ndeferred[NDEFER], cdeferred[NDEFER];
static u32 defer_frame;
static void defer_free(void *p) {
  if (!p)
    return;
  int k = (int)(mx_gs.frames % NDEFER);
  if (ndeferred[k] == cdeferred[k]) {
    /* a list that grows (a level's textures go in one frame) */
    int c = cdeferred[k] ? 2 * cdeferred[k] : 256;
    void **d = realloc(deferred[k], (size_t)c * sizeof *d);
    if (!d) {
      /* no room to keep it: free it once no frame reads anything */
      u32 t0 = mx_count();
      while (mx_frame_done() < mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
        ;
      if (!in_frame) {
        gl_free(p);
        return;
      }
      gl_error(GL_OUT_OF_MEMORY);
      return; /* leaked: the frame being made may read it */
    }
    deferred[k] = d, cdeferred[k] = c;
  }
  deferred[k][ndeferred[k]++] = p;
}
static void defer_collect(void) {
  int k = (int)((mx_gs.frames + 1) % NDEFER); /* filled NDEFER - 1 frames ago */
  if (defer_frame == mx_gs.frames)
    return;
  defer_frame = mx_gs.frames;
  for (int i = 0; i < ndeferred[k]; i++)
    gl_free(deferred[k][i]);
  ndeferred[k] = 0;
}

/* the lists name textures: when textures go, so do the lists (the renderer
   asks, mxgl_ListsDirty, and makes them again) */
int mxgl_ListsDirty(void) { return 0; }

void APIENTRY glDeleteTextures(GLsizei n, const GLuint *t) {
  pq_flush();
  tex_serial++;
  for (int i = 0; i < n; i++)
    if (t[i] && t[i] < MAX_TEX) {
      if (texs[t[i]].gen)
        tex_drop(&texs[t[i]]);
      tex_free_mem(&texs[t[i]]);
      free(texs[t[i]].ipal);
      texs[t[i]].data = NULL;
      texs[t[i]].oclut = NULL;
      texs[t[i]].ipal = NULL;
      tex_drop_twin(&texs[t[i]]);
      texs[t[i]].used = 0;
    }
}
void APIENTRY glBindTexture(GLenum target, GLuint t) {
  if (target != GL_TEXTURE_2D) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  GLuint *b = gl.cunit ? &gl.u1.bound : &gl.bound;
  if (*b != t) {
    pq_flush();
    st_serial++;
  }
  *b = t;
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
  u8 *dst = TLV(t, lv);
  /* the format chosen once, not per texel (lightmaps change every frame) */
  int f = t->pal ? 0 : t->psm == MX_GS_PSM_T8 ? 1 : t->psm == MX_GS_PSM_CT32 ? 2 : 3;
  for (int y = 0; y < h; y++) {
    const u8 *s = src + (u32)(y * w) * 4;
    u32 at = (u32)(y0 + y) * lw + (u32)x0;
    switch (f) {
    case 0:
      for (int x = 0; x < w; x++, s += 4)
        dst[at + (u32)x] = pal_transp >= 0 && s[3] < 128 ? (u8)pal_transp : pal_nearest(s);
      break;
    case 1:
      for (int x = 0; x < w; x++, s += 4) {
        u8 m = s[0] > s[1] ? s[0] : s[1];
        dst[at + (u32)x] = m > s[2] ? m : s[2];
      }
      break;
    case 2:
      for (int x = 0; x < w; x++, s += 4)
        ((u32 *)dst)[at + (u32)x] = (u32)s[0] | (u32)s[1] << 8 | (u32)s[2] << 16 | ((s[3] + 1u) >> 1) << 24;
      break;
    default:
      for (int x = 0; x < w; x++, s += 4)
        ((u16 *)dst)[at + (u32)x] =
            (u16)(s[0] >> 3 | (s[1] >> 3) << 5 | (s[2] >> 3) << 10 | (s[3] >= 128 ? 0x8000 : 0));
      break;
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
  pq_flush();
  st_serial++;
  env_legacy = s;
  env_shifts();
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

/* the texels of a GL_COLOR_INDEX texture (bytes) as RGBA8 (malloc'ed), by
   the shared palette when it is on or the texture has none, else by the
   texture's; NULL without texels */
static u8 *index_rgba(GLsizei w, GLsizei h, const GLvoid *px) {
  if (!px)
    return NULL;
  u8 *o = malloc((size_t)w * (size_t)h * 4);
  if (!o)
    return NULL;
  GLuint b = TBOUND;
  const u8(*pal)[4] = !gl.shared_pal && b < MAX_TEX && texs[b].ipal ? (const u8(*)[4])texs[b].ipal
                                                                    : (const u8(*)[4])pal_rgba;
  const u8 *s = px;
  for (int i = 0; i < w * h; i++)
    memcpy(o + 4 * i, pal[s[i]], 4);
  return o;
}
/* the next tex_set_image's texels are the shared palette's indices, a byte
   each (glTexImage2D with GL_COLOR_INDEX, the palette on) */
static int tex_indexed;

static void tex_set_image(struct tex *t, GLint ifmt, GLsizei w, GLsizei h, const GLvoid *px);

static void tex_drop_twin_ch(struct tex *t, int ch) {
  if (!t->twin[ch])
    return;
  struct tex *c = &twins[t->twin[ch] - 1];
  tex_free_mem(c);
  if (c->gen)
    tex_drop(c);
  memset(c, 0, sizeof *c);
  t->twin[ch] = 0;
}

static void tex_drop_twin(struct tex *t) {
  for (int ch = 0; ch < NTWIN; ch++) {
    if (!t->twin[ch])
      continue;
    struct tex *c = &twins[t->twin[ch] - 1];
    tex_free_mem(c);
    if (c->gen)
      tex_drop(c);
    memset(c, 0, sizeof *c);
    t->twin[ch] = 0;
  }
}

/* texel i of t's level 0 as RGB */
static void tex_rgb(const struct tex *t, u32 i, u8 *o) {
  const u8 *src = TLV(t, 0);
  if (t->oclut) {
    u32 c = t->oclut[csm1(src[i])];
    o[0] = (u8)c, o[1] = (u8)(c >> 8), o[2] = (u8)(c >> 16);
  } else if (t->pal) {
    o[0] = pal_rgb[src[i]][0], o[1] = pal_rgb[src[i]][1], o[2] = pal_rgb[src[i]][2];
  } else if (t->psm == MX_GS_PSM_CT32) {
    o[0] = src[i * 4], o[1] = src[i * 4 + 1], o[2] = src[i * 4 + 2];
  } else {
    u32 c = ((const u16 *)src)[i];
    o[0] = (u8)((c & 31) << 3), o[1] = (u8)((c >> 5 & 31) << 3), o[2] = (u8)((c >> 10 & 31) << 3);
  }
}

/* texel i of t's level 0's alpha, 0 .. 255 */
static u32 tex_alpha_of(const struct tex *t, u32 i) {
  const u8 *src = TLV(t, 0);
  u32 a;
  if (t->bright)
    return 255;
  if (t->oclut)
    a = t->oclut[csm1(src[i])] >> 24;
  else if (t->pal)
    return pal_rgba[src[i]][3];
  else if (t->psm == MX_GS_PSM_CT32)
    a = src[i * 4 + 3];
  else
    return ((const u16 *)src)[i] & 0x8000 ? 255 : 0;
  return a >= 0x80 ? 255 : a * 2; /* stored halved: 0x80 is 1.0 */
}

/* t's brightness copy (T8, the largest of R, G and B; for blendFunc
   products, blend_product), made from its level 0 when first asked for;
   NULL if there is no room */
static int tex_one_level; /* tex_set_image: level 0 only (planar textures, their copies) */

/* t's copy ch made from rgba (t's size, RGBA8): the brightness, or a
   channel's value (which the brightness, the max, then keeps) */
static struct tex *twin_make(struct tex *t, int ch, u8 *rgba) {
  int k = 0;
  while (k < MAX_TWIN && twins[k].used)
    k++;
  if (k == MAX_TWIN)
    return NULL;
  if (ch >= 1 && ch <= 3)
    for (u32 i = 0; i < t->w * t->h; i++)
      rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + ch - 1];
  struct tex *c = &twins[k];
  memset(c, 0, sizeof *c);
  c->used = 1;
  /* TWIN_ENV_A keeps its alpha (RGBA); the others are brightness textures */
  tex_brightness = ch != TWIN_ENV_A;
  tex_one_level = ch >= 1 && ch <= 3; /* the channels': drawn without MIPMAP filtering (cl_src) */
  tex_set_image(c, ch == TWIN_ENV_A ? GL_RGBA : GL_RGB, (GLsizei)t->w, (GLsizei)t->h, rgba);
  if (!c->data) {
    c->used = 0;
    return NULL;
  }
  t->twin[ch] = (u16)(k + 1);
  return c;
}

static struct tex *tex_twin(struct tex *t, int ch) {
  if (t->planar && ch == 1)
    return t; /* its own texels */
  if (!t->twin[ch]) {
    if (t->planar && ch > 1)
      return NULL; /* made with it (tex_set_image) */
    u8 *rgba = malloc(t->w * t->h * 4);
    if (!rgba)
      return NULL;
    for (u32 i = 0; i < t->w * t->h; i++) {
      u8 *o = rgba + i * 4;
      if (ch == TWIN_ENV_A) { /* white, its alpha */
        o[0] = o[1] = o[2] = 255, o[3] = (u8)tex_alpha_of(t, i);
        continue;
      }
      if (t->planar) { /* the brightness of its planes */
        const struct tex *g = &twins[t->twin[2] - 1], *b = &twins[t->twin[3] - 1];
        o[0] = TLV(t, 0)[i], o[1] = TLV(g, 0)[i], o[2] = TLV(b, 0)[i];
      } else
        tex_rgb(t, i, o);
      if (ch == TWIN_ENV_B) { /* the brightness times the alpha */
        u32 m = o[0] > o[1] ? o[0] : o[1], a = tex_alpha_of(t, i);
        m = o[2] > m ? o[2] : m;
        o[0] = o[1] = o[2] = (u8)((m * a + 127) / 255);
      }
      o[3] = 255;
    }
    struct tex *c = twin_make(t, ch, rgba);
    free(rgba);
    if (!c)
      return NULL;
  }
  struct tex *c = &twins[t->twin[ch] - 1];
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
  GLuint bound = TBOUND;
  if (!bound || bound >= MAX_TEX)
    return;
  if (target != GL_TEXTURE_2D || w < 0 || h < 0 || border) {
    gl_error(target != GL_TEXTURE_2D ? GL_INVALID_ENUM : GL_INVALID_VALUE);
    return;
  }
  /* luminance and intensity textures are kept as their brightness (T8, a
     CLUT that puts it in alpha too: the lightmaps, for the products with
     the destination colour), whatever the format of the texels given */
  if (ifmt == GL_LUMINANCE || ifmt == GL_LUMINANCE8 || ifmt == 1 || ifmt == GL_INTENSITY ||
      ifmt == GL_INTENSITY8 || (format == GL_LUMINANCE && (ifmt == 3 || ifmt == GL_RGB)))
    tex_brightness = 1;
  /* indices of the shared palette are kept as they are */
  if (level == 0 && format == GL_COLOR_INDEX && px && gl.shared_pal && pal_on && w >= 8 && h >= 8 &&
      !(w & (w - 1)) && !(h & (h - 1))) {
    tex_indexed = 1;
    format = GL_RGBA;
  }
  u8 *rgba = level == 0 && !tex_indexed
                 ? (format == GL_COLOR_INDEX ? index_rgba(w, h, px) : to_rgba(format, w, h, px))
                 : NULL;
  if (rgba) {
    tex_image_2d(target, level, ifmt == GL_COLOR_INDEX8_EXT ? GL_RGBA : ifmt, w, h, border, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    return;
  }
  if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
    tex_brightness = 0;
    if (px)
      gl_error(GL_INVALID_ENUM);
    if (px)
      return;
  }
  struct tex *t = &texs[bound];
  if (level > 0)
    return; /* made from level 0 */
  /* at least 8 x 8 (an IMAGE transfer of whole quadwords), sides of powers
     of two (the GS's): scaled */
  int pw = (int)pow2_ceil((u32)(w < 8 ? 8 : w)), ph = (int)pow2_ceil((u32)(h < 8 ? 8 : h));
  if (px && (w != pw || h != ph)) {
    int nw = pw, nh = ph;
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
  w = pw, h = ph;
  tex_drop_twin(t);
  tex_set_image(t, ifmt, w, h, px);
}

/* t's image, w x h RGBA8 texels s (NULL: zeros), at least 8 x 8 */
/* t's levels from its level 0 of indices: each texel the box of four, through
   the palette (a box of one index: that index, the usual case in a game's
   8-bit art); transparent when half of the box is */
static void index_levels(struct tex *t) {
  u32 w = t->w, h = t->h;
  if (t->max_levels < 2)
    return;
  u8 black[3] = {0, 0, 0};
  pal_nearest(black); /* pal_near made */
  if (!pal_near)
    return;
  for (u32 lv = 1; lv < t->max_levels; lv++) {
    u32 nw = w >> 1, nh = h >> 1;
    const u8 *up = TLV(t, lv - 1);
    u8 *dn = TLV(t, lv);
    for (u32 y = 0; y < nh; y++) {
      const u8 *p = up + 2 * y * w;
      for (u32 x = 0; x < nw; x++, p += 2) {
        u32 a = p[0], b = p[1], c = p[w], d = p[w + 1];
        if (a == b && a == c && a == d) {
          *dn++ = (u8)a;
          continue;
        }
        u32 r = 0, g = 0, bl = 0, n = 0, ix[4] = {a, b, c, d};
        for (int k = 0; k < 4; k++)
          if ((int)ix[k] != pal_transp) {
            const u8 *e = pal_rgb[ix[k]];
            r += e[0], g += e[1], bl += e[2], n++;
          }
        if (n < 2) {
          *dn++ = (u8)pal_transp;
          continue;
        }
        r /= n, g /= n, bl /= n;
        *dn++ = pal_near[r >> 3 | (g >> 3) << 5 | (bl >> 3) << 10];
      }
    }
    w = nw, h = nh;
  }
  t->levels = t->max_levels;
}

static void tex_set_image(struct tex *t, GLint ifmt, GLsizei w, GLsizei h, const GLvoid *px) {
  /* alpha in the texels? Only 0 and 255 (a colour key: Descent 3's 1555)
     fits 16 bits (the alpha bit, TEXA); other alpha takes 32 */
  int alpha = 0, alpha8 = 0, indexed = tex_indexed;
  const u8 *s = px;
  tex_indexed = 0;
  if (indexed) {
    for (int i = 0; i < w * h && !alpha; i++)
      alpha = s[i] == pal_transp;
  } else if (s && ifmt != GL_RGB && ifmt != GL_RGB8 && ifmt != GL_RGB5 && ifmt != 3)
    for (int i = 0; i < w * h; i++)
      if (s[i * 4 + 3] != 255) {
        alpha = 1;
        if (s[i * 4 + 3] != 0) {
          alpha8 = 1;
          break;
        }
      }
  tex_free_mem(t);
  t->w = (u32)w, t->h = (u32)h;
  t->lw = ilog2((u32)w), t->lh = ilog2((u32)h);
  t->alpha = alpha;
  /* mxgl_color_light: a GL_RGB8 texture without MIPMAP filtering (a
     lightmap) as its channels' brightness, each 8 bits: the red one its
     own texels, the green and blue ones copies (twin[2], twin[3]) */
  int planar = cl_enabled && (ifmt == GL_RGB8 || ifmt == GL_RGBA8) && !t->mip && s;
  if (planar)
    tex_brightness = 1, tex_one_level = 1;
  t->planar = (u8)planar;
  /* texels all of the game's palette (the transparent ones its transparent
     entry, if it has one): its indices (T8, exact), else 16 bits */
  int pal = indexed;
  if (!indexed && !tex_brightness && (!alpha || (pal_transp >= 0 && !alpha8)) && pal_on && s) {
    pal = 1;
    for (int i = 0; i < w * h && pal; i++)
      if (s[i * 4 + 3] && pal_find((u32)s[i * 4] | (u32)s[i * 4 + 1] << 8 | (u32)s[i * 4 + 2] << 16) < 0)
        pal = 0;
  }
  /* RGBA texels all grey (R = G = B) and opaque, or black with alpha 0
     (a lightmap block's unused texels, calloc'ed): the brightness texture
     (8 bits, not 16; one byte a texel to update, no brightness copy for
     the products with the destination: lightmaps sent as RGBA). Exact,
     but for those black texels outside a product: opaque there */
  int grey = 0;
  if (!pal && !indexed && !tex_brightness && !planar && !alpha8 && s) {
    grey = 1;
    for (int i = 0; i < w * h && grey; i++) {
      const u8 *c = s + i * 4;
      grey = c[0] == c[1] && c[0] == c[2] && (c[3] == 255 || (c[3] == 0 && c[0] == 0));
    }
    if (grey)
      tex_brightness = 1;
  }
  t->grey = (u8)grey;
  t->bright = tex_brightness;
  t->pal = pal;
  t->alpha_fmt = tex_brightness || ifmt == GL_RGBA || ifmt == 4 || ifmt == GL_RGBA8 ||
                 ifmt == GL_RGBA4 || ifmt == GL_RGB5_A1 || ifmt == GL_RGBA2 || ifmt == GL_ALPHA ||
                 ifmt == GL_ALPHA8 || ifmt == GL_LUMINANCE_ALPHA || ifmt == 2 ||
                 ifmt == GL_LUMINANCE8_ALPHA8 || ifmt == GL_INTENSITY || ifmt == GL_INTENSITY8;
  t->psm = tex_brightness || pal ? MX_GS_PSM_T8 : alpha8 ? MX_GS_PSM_CT32 : MX_GS_PSM_CT16;
  tex_brightness = 0;
  t->max_levels = tex_one_level ? 1 : mx_tex_max_levels(t->lw, t->lh, 8);
  tex_one_level = 0;
  t->levels = 1;
  tex_alloc_mem(t, 0); /* the DMAC reads it (IMAGE) */
  tex_new_gen(t); /* new texels: a new key */
  if (!t->data)
    return;
  if (planar) {
    /* its red plane, then the copies of green and blue */
    u8 *one = malloc((size_t)(w * h * 4));
    int ok = one != NULL;
    for (int ch = 1; ch <= 3 && ok; ch++) {
      memcpy(one, s, (size_t)(w * h * 4));
      if (ch == 1) {
        for (int i = 0; i < w * h; i++)
          one[i * 4 + 1] = one[i * 4 + 2] = one[i * 4];
        convert(t, 0, 0, 0, w, h, one);
      } else
        ok = twin_make(t, ch, one) != NULL;
    }
    free(one);
    if (!ok)
      gl_error(GL_OUT_OF_MEMORY);
  } else if (indexed) {
    memcpy(TLV(t, 0), s, (size_t)(w * h));
    index_levels(t);
  } else if (s) {
    convert(t, 0, 0, 0, w, h, s);
    make_levels(t, s);
  } else {
    for (u32 lv = 0; lv < t->max_levels; lv++)
      memset(TLV(t, lv), 0, mx_tex_level_bytes(t->psm, t->lw, t->lh, lv));
  }
  tex_writeback(t);
}

static void tex_sub(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const GLvoid *px);
static void tex_sub_l8(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const GLvoid *px);
static void tex_sub_ch(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const u8 *px);
void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                                     GLsizei h, GLenum format, GLenum type, const GLvoid *px) {
  pq_flush();
  GLuint bound = TBOUND;
  if (level != 0 || !bound || bound >= MAX_TEX)
    return;
  struct tex *t = &texs[bound];
  if (!t->data)
    return;
  /* GL_BLEND's and GL_ADD's copies: made again when drawn */
  tex_drop_twin_ch(t, TWIN_ENV_A);
  tex_drop_twin_ch(t, TWIN_ENV_B);
  /* bytes into a brightness texture (Quake's lightmaps, every frame): its
     texels as they are, without the RGBA8 copy */
  if ((format == GL_LUMINANCE || format == GL_INTENSITY) && px && t->psm == MX_GS_PSM_T8 &&
      !t->pal && !t->planar && !t->oclut && !t->twin[0] && !t->twin[1] && !t->twin[2] &&
      !t->twin[3] && x >= 0 && y >= 0 && w > 0 && h > 0 && (u32)(x + w) <= t->w &&
      (u32)(y + h) <= t->h) {
    tex_sub_l8(t, x, y, w, h, px);
    return;
  }
  u8 *rgba = to_rgba(format, w, h, px);
  if (rgba) {
    glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, type, rgba);
    free(rgba);
    return;
  }
  if (t->oclut) {
    gl_error(GL_INVALID_OPERATION); /* a compressed (paletted) texture */
    return;
  }
  if (x < 0 || y < 0 || w < 0 || h < 0 || (u32)(x + w) > t->w || (u32)(y + h) > t->h) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  if (!w || !h)
    return;
  /* changed in place, level 0 only: an upload of a frame in flight may take
     the new texels, a frame early (a copy each time: Quake's lightmaps
     change every frame). Only its lines go back from the cache, and to GS
     memory (tex_place) */
  /* its texels (planar: the red plane), its brightness and channel copies */
  u8 *one = NULL;
  for (int ch = -1; ch < 4; ch++) {
    if (ch < 0) {
      if (!t->planar)
        tex_sub(t, x, y, w, h, px);
      continue;
    }
    if (!t->twin[ch] && !(t->planar && ch == 1))
      continue;
    struct tex *c = t->planar && ch == 1 ? t : &twins[t->twin[ch] - 1];
    const u8 *src = px;
    /* a channel into a T8 copy (the planes of coloured light): its bytes
       as they are */
    if (ch && c->psm == MX_GS_PSM_T8 && !c->pal) {
      tex_sub_ch(c, x, y, w, h, (const u8 *)px + ch - 1);
      continue;
    }
    if (ch && (one || (one = malloc((size_t)(w * h * 4))))) {
      for (int i = 0; i < w * h; i++)
        one[i * 4] = one[i * 4 + 1] = one[i * 4 + 2] = ((const u8 *)px)[i * 4 + ch - 1];
      src = one;
    } else if (ch) {
      tex_drop_twin(t); /* no memory to convert: made again when drawn */
      break;
    }
    tex_sub(c, x, y, w, h, src);
  }
  free(one);
}

/* lines y .. y + h - 1 of t's level 0 changed: written back, marked for
   tex_place (which uploads them alone); level 0 only from now on */
static void tex_sub_mark(struct tex *t, GLint y, GLsizei h) {
  t->levels = 1;
  u32 rb = t->w * tex_bpp(t);
  mx_dcache_writeback(TLV(t, 0) + (u32)y * rb, (u32)h * rb);
  if (!t->dirty)
    t->dy0 = (u32)y, t->dy1 = (u32)(y + h);
  else
    t->dy0 = (u32)y < t->dy0 ? (u32)y : t->dy0, t->dy1 = (u32)(y + h) > t->dy1 ? (u32)(y + h) : t->dy1;
  t->dirty = 1;
}

/* RGBA8 texels px into t's level 0 at (x, y) */
static void tex_sub(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const GLvoid *px) {
  convert(t, 0, x, y, w, h, px);
  tex_sub_mark(t, y, h);
}

/* every fourth byte from px (a channel of RGBA8 texels) into the T8
   texture t's level 0 at (x, y) */
static void tex_sub_ch(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const u8 *px) {
  u8 *dst = TLV(t, 0) + (u32)y * t->w + (u32)x;
  for (int r = 0; r < h; r++, dst += t->w)
    for (int i = 0; i < w; i++, px += 4)
      dst[i] = *px;
  tex_sub_mark(t, y, h);
}

/* bytes px into the T8 texture t's level 0 at (x, y) */
static void tex_sub_l8(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const GLvoid *px) {
  u8 *dst = TLV(t, 0) + (u32)y * t->w + (u32)x;
  const u8 *src = px;
  for (int r = 0; r < h; r++, dst += t->w, src += w)
    memcpy(dst, src, (size_t)w);
  tex_sub_mark(t, y, h);
}

void APIENTRY glTexParameterf(GLenum target, GLenum p, GLfloat v) {
  pq_flush();
  tex_serial++;
  GLuint bound = TBOUND;
  if (!bound || bound >= MAX_TEX)
    return;
  struct tex *t = &texs[bound];
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
  /* the levels are made here from level 0 anyway */
  case GL_GENERATE_MIPMAP: break;
  case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_MAX_LOD: case GL_TEXTURE_BASE_LEVEL:
  case GL_TEXTURE_MAX_LEVEL: case GL_TEXTURE_PRIORITY: break;
  default: gl_error(GL_INVALID_ENUM); break;
  }
}
void APIENTRY glTexParameteri(GLenum target, GLenum p, GLint v) {
  glTexParameterf(target, p, (GLfloat)v);
}

/* queries and the rest */
GLenum APIENTRY glGetError(void) {
  GLenum e = gl.err;
  gl.err = GL_NO_ERROR;
  return e;
}
void APIENTRY glGetIntegerv(GLenum p, GLint *v) {
  switch (p) {
  case GL_MAX_TEXTURE_SIZE: *v = MXGL_MAX_TEXTURE; break;
  case GL_VIEWPORT: v[0] = gl.vp[0], v[1] = gl.vp[1], v[2] = gl.vp[2], v[3] = gl.vp[3]; break;
  case GL_SCISSOR_BOX: v[0] = gl.sc[0], v[1] = gl.sc[1], v[2] = gl.sc[2], v[3] = gl.sc[3]; break;
  case GL_MATRIX_MODE: *v = (GLint)gl.mode; break;
  case GL_MAX_MODELVIEW_STACK_DEPTH: *v = MV_DEPTH; break;
  case GL_MAX_PROJECTION_STACK_DEPTH: *v = PR_DEPTH; break;
  case GL_MAX_TEXTURE_STACK_DEPTH: *v = TM_DEPTH; break;
  case GL_MODELVIEW_STACK_DEPTH: *v = gl.mv_top + 1; break;
  case GL_PROJECTION_STACK_DEPTH: *v = gl.pr_top + 1; break;
  case GL_TEXTURE_STACK_DEPTH: *v = gl.tm_top + 1; break;
  case GL_MAX_CLIP_PLANES: *v = 1; break;
  case GL_MAX_TEXTURE_UNITS: *v = 2; break;
  case GL_PACK_ALIGNMENT: *v = gl.pack_align; break;
  case GL_UNPACK_ALIGNMENT: *v = 4; break;
  case GL_TEXTURE_BINDING_2D: *v = (GLint)TBOUND; break;
  case GL_ACTIVE_TEXTURE: *v = (GLint)(GL_TEXTURE0 + gl.cunit); break;
  case GL_CLIENT_ACTIVE_TEXTURE: *v = (GLint)(GL_TEXTURE0 + gl.ccunit); break;
  case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: *v = rgb24 ? 8 : 5; break;
  case GL_ALPHA_BITS: *v = 0; break;
  /* the frame buffer's alpha bit: none in 24-bit colour */
  case GL_STENCIL_BITS: *v = st_there(); break;
  case GL_DEPTH_BITS: *v = zmax == ZMAX_Z16 ? 16 : 24; break;
  case GL_MAX_LIST_NESTING: *v = 1; break;
  case GL_MAX_VIEWPORT_DIMS: v[0] = 2048, v[1] = 2048; break;
  default: *v = 0; gl_error(GL_INVALID_ENUM); break;
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
  case GL_TEXTURE_2D: return (GLboolean)(gl.cunit ? gl.u1.tex2d : gl.tex2d);
  case GL_SHARED_TEXTURE_PALETTE_EXT: return (GLboolean)gl.shared_pal;
  case GL_BLEND: return (GLboolean)gl.blend;
  case GL_ALPHA_TEST: return (GLboolean)gl.atest;
  case GL_FOG: return (GLboolean)gl.fog;
  case GL_DEPTH_TEST: return (GLboolean)gl.dtest;
  case GL_CULL_FACE: return (GLboolean)gl.cull;
  case GL_SCISSOR_TEST: return (GLboolean)gl.scissor;
  case GL_POLYGON_OFFSET_FILL: return (GLboolean)gl.poffset;
  case GL_CLIP_PLANE0: return (GLboolean)gl.ucp_on;
  default: return 0;
  }
}
const GLubyte *APIENTRY glGetString(GLenum name) {
  switch (name) {
  case GL_VENDOR: return (const GLubyte *)"maxsdk";
  case GL_RENDERER: return (const GLubyte *)"PS2 GS + VU1 (HLSL)";
  case GL_VERSION: return (const GLubyte *)"1.1";
  case GL_EXTENSIONS:
    return (const GLubyte *)"GL_ARB_multitexture GL_ARB_texture_env_combine GL_EXT_texture_env_combine "
                            "GL_EXT_compiled_vertex_array GL_EXT_draw_range_elements "
                            "GL_EXT_multi_draw_arrays GL_IBM_multimode_draw_arrays "
                            "GL_EXT_paletted_texture GL_EXT_shared_texture_palette "
                            "GL_OES_compressed_paletted_texture GL_SGIS_generate_mipmap "
                            "GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp "
                            "GL_ARB_vertex_buffer_object GL_PS2_vertex_program_binary";
  default: gl_error(GL_INVALID_ENUM); return (const GLubyte *)"";
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
void APIENTRY glPixelStorei(GLenum p, GLint v) {
  if (p == GL_PACK_ALIGNMENT && (v == 1 || v == 2 || v == 4 || v == 8))
    gl.pack_align = v;
}
void APIENTRY glTexParameteriv(GLenum t, GLenum p, const GLint *v) {
  if (p != GL_TEXTURE_BORDER_COLOR)
    glTexParameteri(t, p, v[0]);
}
void APIENTRY glTexParameterfv(GLenum t, GLenum p, const GLfloat *v) {
  if (p != GL_TEXTURE_BORDER_COLOR)
    glTexParameterf(t, p, v[0]);
}
void APIENTRY glTexEnvfv(GLenum t, GLenum p, const GLfloat *v) {
  if (p != GL_TEXTURE_ENV_COLOR)
    glTexEnvf(t, p, v[0]);
  else if (!gl.cunit)
    for (int i = 0; i < 4; i++)
      env_color[i] = v[i] < 0 ? 0 : v[i] > 1 ? 1 : v[i];
}
void APIENTRY glTexEnviv(GLenum t, GLenum p, const GLint *v) {
  if (p != GL_TEXTURE_ENV_COLOR)
    glTexEnvf(t, p, (GLfloat)v[0]);
  else {
    GLfloat f[4];
    for (int i = 0; i < 4; i++)
      f[i] = (GLfloat)v[i] / 2147483647.0f;
    glTexEnvfv(t, p, f);
  }
}
GLboolean APIENTRY glIsTexture(GLuint t) { return t && t < MAX_TEX && texs[t].used; }
/* OpenGL ES 1.1's float forms */
void APIENTRY glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
  glOrtho(l, r, b, t, n, f);
}
void APIENTRY glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
  glFrustum(l, r, b, t, n, f);
}
void APIENTRY glDepthRangef(GLclampf n, GLclampf f) { glDepthRange(n, f); }
void APIENTRY glClearDepthf(GLclampf d) {}
void APIENTRY glTexCoord2i(GLint s, GLint t) { glTexCoord2f((GLfloat)s, (GLfloat)t); }
void APIENTRY glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
  glVertex3f(x / w, y / w, z / w);
}
void APIENTRY glLoadMatrixd(const GLdouble *m) {
  float f[16];
  for (int i = 0; i < 16; i++)
    f[i] = (float)m[i];
  glLoadMatrixf(f);
}
void APIENTRY glMultMatrixd(const GLdouble *m) {
  float f[16];
  for (int i = 0; i < 16; i++)
    f[i] = (float)m[i];
  glMultMatrixf(f);
}
void APIENTRY glFlush(void) {}
/* The pixels of the frame being drawn (or the last one, between frames):
   what the stream has so far is drawn first (mx_frame_sync; slow: the EE
   waits for the GS), then read back line by line. GL_RGB, GL_RGBA (bytes)
   and GL_DEPTH_COMPONENT (GL_FLOAT, GL_UNSIGNED_INT, _SHORT, _BYTE), in the
   window's coordinates (mxgl_window), lines GL_PACK_ALIGNMENT apart.
   Not in 1080i (the passes own the buffer): zeros. */
static u32 rp_line[1920] MX_ALIGNED(64);
void APIENTRY glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t,
                           GLvoid *px) {
  int depth = f == GL_DEPTH_COMPONENT;
  int nc = f == GL_RGB ? 3 : f == GL_RGBA ? 4 : 1;
  int bpc = !depth || t == GL_UNSIGNED_BYTE ? 1 : t == GL_UNSIGNED_SHORT ? 2 : 4;
  if ((!depth && f != GL_RGB && f != GL_RGBA) || (!depth && t != GL_UNSIGNED_BYTE) || w < 0 || h < 0) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  u32 line = ((u32)(w * nc * bpc) + (u32)gl.pack_align - 1) & ~((u32)gl.pack_align - 1);
  memset(px, 0, line * (u32)h);
  if (beam || !w || !h)
    return;
  pq_flush();
  if (in_frame)
    flush_out();
  if (mx_frame_sync() < 0)
    return;
  /* the frame's colour buffer (between frames: the last one's), the Z buffer */
  const struct mx_rt *rtb = &rt[buf];
  u32 bp = depth ? MX_GS_PAGE(rtb->zbp) : MX_GS_PAGE(rtb->fbp);
  /* Z24 read as Z32 (the same layout): a 24-bit transfer sends 3 bytes a pixel */
  u32 psm = depth ? (rtb->zpsm == MX_GS_PSM_Z24 ? MX_GS_PSM_Z32 : rtb->zpsm) : rtb->psm;
  int last_fy = -1;
  for (int j = 0; j < h; j++) {
    /* GL's y is up from the bottom of the window */
    int gy = y + j, fy = (gl_h - 1 - gy) * fb_h / gl_h;
    if (gy < 0 || gy >= gl_h || fy < 0 || fy >= fb_h)
      continue;
    if (fy != last_fy && mx_gs_download(rp_line, bp, rtb->fbw, psm, 0, (u32)fy, (u32)fb_w, 1) < 0)
      break;
    last_fy = fy;
    u8 *d = (u8 *)px + (u32)j * line;
    for (int i = 0; i < w; i++) {
      int gx = x + i, fx = gx * fb_w / gl_w;
      if (gx < 0 || gx >= gl_w || fx < 0 || fx >= fb_w)
        continue;
      if (depth) {
        /* GS Z is (1 - depth) of its range (make_mvp) */
        u32 z = rp_line[fx] & (psm == MX_GS_PSM_Z24 || psm == MX_GS_PSM_Z32 ? 0xFFFFFFu : 0xFFFFu);
        float zm = zmax * 16.0f + 15.0f, dv = 1.0f - (float)z / zm;
        if (t == GL_FLOAT)
          memcpy(d + 4 * i, &dv, 4);
        else if (t == GL_UNSIGNED_INT)
          ((u32 *)d)[i] = (u32)(dv * 4294967295.0f);
        else if (t == GL_UNSIGNED_SHORT)
          ((u16 *)d)[i] = (u16)(dv * 65535.0f);
        else
          d[i] = (u8)(dv * 255.0f);
      } else {
        u32 p = ((const u16 *)rp_line)[fx];
        u8 *o = d + nc * i;
        u32 r = p & 31, g = p >> 5 & 31, b = p >> 10 & 31;
        o[0] = (u8)(r << 3 | r >> 2), o[1] = (u8)(g << 3 | g >> 2), o[2] = (u8)(b << 3 | b >> 2);
        if (nc == 4)
          o[3] = 255;
      }
    }
  }
}

/* the window's pixels into the bound texture's level 0 (glReadPixels, then
   glTexSubImage2D): the frame drawn so far is finished first, so it is
   slow; with beam racing the pixels are zeros (glReadPixels) */
void APIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y,
                                  GLsizei w, GLsizei h) {
  if (w <= 0 || h <= 0)
    return;
  u8 *px = malloc((size_t)w * (size_t)h * 4);
  if (!px) {
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  int align = gl.pack_align;
  gl.pack_align = 1;
  glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  gl.pack_align = align;
  glTexSubImage2D(target, level, xoff, yoff, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  free(px);
}

/* the bound texture's level 0 as RGBA or RGB bytes, from its texels in
   GS format (16-bit ones as 5 bits a channel, a brightness texture as
   grey); level 0 only */
void APIENTRY glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *px) {
  GLuint bound = TBOUND;
  int nc = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 0;
  if (!nc || type != GL_UNSIGNED_BYTE) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  if (level != 0 || !bound || bound >= MAX_TEX || !texs[bound].data)
    return;
  pq_flush();
  const struct tex *t = &texs[bound];
  const u8 *src = TLV(t, 0);
  u32 line = ((t->w * (u32)nc) + (u32)gl.pack_align - 1) & ~((u32)gl.pack_align - 1);
  for (u32 y = 0; y < t->h; y++) {
    u8 *d = (u8 *)px + y * line;
    for (u32 x = 0; x < t->w; x++, d += nc) {
      u32 i = y * t->w + x;
      u8 c[4];
      if (t->psm == MX_GS_PSM_CT32) {
        u32 v = ((const u32 *)src)[i];
        u32 a = v >> 24;
        c[0] = (u8)v, c[1] = (u8)(v >> 8), c[2] = (u8)(v >> 16), c[3] = (u8)(a >= 128 ? 255 : a * 2);
      } else if (t->psm == MX_GS_PSM_T8) {
        u8 k = src[i];
        if (t->pal)
          memcpy(c, pal_rgb[k], 3), c[3] = k == pal_transp ? 0 : 255;
        else
          c[0] = c[1] = c[2] = k, c[3] = 255;
      } else {
        u16 v = ((const u16 *)src)[i];
        c[0] = (u8)((v & 31) << 3), c[1] = (u8)(((v >> 5) & 31) << 3), c[2] = (u8)(((v >> 10) & 31) << 3);
        c[3] = v & 0x8000 ? 255 : 0;
      }
      memcpy(d, c, (size_t)nc);
    }
  }
}
void APIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
  /* FBMSK: the bits not written, of the colour before it is made 16 bits */
  u32 m = (r ? 0 : 0xFFu) | (g ? 0 : 0xFF00u) | (b ? 0 : 0xFF0000u) | (a ? 0 : 0xFF000000u);
  if (m != gl.cmask) {
    pq_flush();
    st_serial++;
  }
  gl.cmask = m;
}
void APIENTRY glStencilFunc(GLenum f, GLint r, GLuint m) {
  pq_flush();
  gl.st_func = f, gl.st_ref = r, gl.st_mask = m;
  st_serial++;
}
void APIENTRY glStencilOp(GLenum a, GLenum b, GLenum c) {
  (void)a, (void)b; /* what fails is not written: see gl.st_on */
  pq_flush();
  gl.st_op = c;
  st_serial++;
}
void APIENTRY glStencilMask(GLuint m) {
  pq_flush();
  gl.st_wmask = m;
  st_serial++;
}
void APIENTRY glClearStencil(GLint s) { gl.st_clear = (u32)s; }
void APIENTRY glClearDepth(GLclampd d) {}
void APIENTRY glPolygonOffset(GLfloat factor, GLfloat units) {
  if (factor != gl.pofactor || units != gl.pounits) {
    pq_flush();
    mat_serial++;
  }
  gl.pofactor = factor, gl.pounits = units;
}
/* the plane in eye space: e times the inverse of the modelview of now */
static void clip_plane(GLenum p, const float *e) {
  if (p != GL_CLIP_PLANE0) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  pq_flush();
  mat_serial++;
  float inv[16];
  if (!mat_inverse(inv, gl.mv[gl.mv_top]))
    return;
  for (int c = 0; c < 4; c++)
    gl.ucp_eye[c] = e[0] * inv[4 * c] + e[1] * inv[4 * c + 1] + e[2] * inv[4 * c + 2] + e[3] * inv[4 * c + 3];
}
void APIENTRY glClipPlane(GLenum p, const GLdouble *e) {
  float f[4] = {(float)e[0], (float)e[1], (float)e[2], (float)e[3]};
  clip_plane(p, f);
}
void APIENTRY glClipPlanef(GLenum p, const GLfloat *e) { clip_plane(p, e); }
void APIENTRY glLockArraysEXT(GLint first, GLsizei count) {
  lk.on = 1;
  lk.valid = 0;
  lk.count = first + count;
}
void APIENTRY glUnlockArraysEXT(void) { lk.on = lk.valid = 0; }
/* GL_EXT_paletted_texture, GL_EXT_shared_texture_palette: a palette of up
   to 256 colours (GL_RGB, GL_RGBA bytes) for GL_COLOR_INDEX textures: the
   shared one (whose RGB textures are also kept as its indices: 8 bits,
   exact), or the bound texture's (for its next GL_COLOR_INDEX texels) */
void APIENTRY glColorTableEXT(GLenum target, GLenum ifmt, GLsizei width, GLenum format, GLenum type,
                              const GLvoid *table) {
  int nc = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 0;
  if (!nc || type != GL_UNSIGNED_BYTE || width <= 0 || width > 256 || !table) {
    gl_error(!nc || type != GL_UNSIGNED_BYTE ? GL_INVALID_ENUM : GL_INVALID_VALUE);
    return;
  }
  const u8 *c = table;
  u8 rgba[256][4];
  memset(rgba, 0, sizeof rgba);
  for (int i = 0; i < width; i++, c += nc)
    rgba[i][0] = c[0], rgba[i][1] = c[1], rgba[i][2] = c[2], rgba[i][3] = nc == 4 ? c[3] : 255;
  if (target == GL_SHARED_TEXTURE_PALETTE_EXT) {
    u8 rgb[256 * 3];
    for (int i = 0; i < 256; i++)
      memcpy(rgb + 3 * i, rgba[i], 3);
    pq_flush();
    pal_transp = -1;
    for (int i = 0; i < 256 && pal_transp < 0; i++)
      if (!rgba[i][3])
        pal_transp = i;
    mxgl_Palette(rgb);
    memcpy(pal_rgba, rgba, sizeof rgba);
    return;
  }
  GLuint b = TBOUND;
  if (target != GL_TEXTURE_2D || !b || b >= MAX_TEX) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  if (!texs[b].ipal && !(texs[b].ipal = malloc(1024))) {
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  memcpy(texs[b].ipal, rgba, sizeof rgba);
}
void APIENTRY glColorTable(GLenum target, GLenum ifmt, GLsizei width, GLenum format, GLenum type,
                           const GLvoid *table) {
  glColorTableEXT(target, ifmt, width, format, type, table);
}

/* GL_OES_compressed_paletted_texture: a palette (16 or 256 colours) and the
   texels' indices (4 or 8 bits; two a byte, the first in the high bits),
   level 0 and, for a level of -n, n levels after it, each half the size.
   Kept as they are: T8 with the texture's own CLUT (as GS memory takes it,
   a 4-bit texture's indices made bytes), the levels given (none made). */
static int oes_pal(GLenum f, int *bits, int *eb) {
  if (f < GL_PALETTE4_RGB8_OES || f > GL_PALETTE8_RGB5_A1_OES)
    return 0;
  int k = (int)(f - GL_PALETTE4_RGB8_OES);
  *bits = k < 5 ? 4 : 8;
  static const int ebytes[5] = {3, 4, 2, 2, 2};
  *eb = ebytes[k % 5];
  return 1;
}
static void oes_entry(GLenum f, const u8 *e, u8 *o) {
  u32 v = (u32)e[0] | (u32)e[1] << 8;
  switch ((f - GL_PALETTE4_RGB8_OES) % 5) {
  case 0: o[0] = e[0], o[1] = e[1], o[2] = e[2], o[3] = 255; break;
  case 1: o[0] = e[0], o[1] = e[1], o[2] = e[2], o[3] = e[3]; break;
  case 2: /* R5 G6 B5 */
    o[0] = (u8)((v >> 11) * 255 / 31), o[1] = (u8)((v >> 5 & 63) * 255 / 63), o[2] = (u8)((v & 31) * 255 / 31);
    o[3] = 255;
    break;
  case 3: /* R4 G4 B4 A4 */
    o[0] = (u8)((v >> 12) * 17), o[1] = (u8)((v >> 8 & 15) * 17), o[2] = (u8)((v >> 4 & 15) * 17);
    o[3] = (u8)((v & 15) * 17);
    break;
  default: /* R5 G5 B5 A1 */
    o[0] = (u8)((v >> 11) * 255 / 31), o[1] = (u8)((v >> 6 & 31) * 255 / 31), o[2] = (u8)((v >> 1 & 31) * 255 / 31);
    o[3] = v & 1 ? 255 : 0;
    break;
  }
}

void APIENTRY glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                     GLint border, GLsizei bytes, const GLvoid *data) {
  pq_flush();
  tex_serial++;
  int bits, eb;
  GLuint b = TBOUND;
  if (target != GL_TEXTURE_2D || !oes_pal(ifmt, &bits, &eb)) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  if (level > 0 || border || w <= 0 || h <= 0 || !data) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  if (!b || b >= MAX_TEX)
    return;
  int npal = bits == 4 ? 16 : 256, nlev = 1 - level;
  const u8 *p = data, *end = p + bytes;
  u8 pal[256][4];
  memset(pal, 0, sizeof pal);
  if (p + npal * eb > end) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  int alpha = 0;
  for (int i = 0; i < npal; i++, p += eb) {
    oes_entry(ifmt, p, pal[i]);
    alpha |= pal[i][3] != 255;
  }
  struct tex *t = &texs[b];
  /* sides that are not powers of two, or below 8: as RGBA (scaled) */
  if (w < 8 || h < 8 || (w & (w - 1)) || (h & (h - 1))) {
    u8 *rgba = malloc((size_t)w * (size_t)h * 4);
    if (!rgba)
      return;
    for (int i = 0; i < w * h; i++) {
      int ix = bits == 8 ? p[i] : (p[i / 2] >> (i & 1 ? 0 : 4)) & 15;
      memcpy(rgba + 4 * i, pal[ix], 4);
    }
    tex_image_2d(GL_TEXTURE_2D, 0, alpha ? GL_RGBA : GL_RGB, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    return;
  }
  tex_drop_twin(t);
  tex_brightness = 0;
  tex_set_image(t, GL_COLOR_INDEX8_EXT, w, h, NULL); /* frees the old texels and CLUT */
  tex_free_mem(t);
  t->psm = MX_GS_PSM_T8;
  t->pal = 0, t->bright = 0;
  t->alpha = alpha;
  t->alpha_fmt = ifmt == GL_PALETTE4_RGBA8_OES || ifmt == GL_PALETTE8_RGBA8_OES ||
                 ifmt == GL_PALETTE4_RGBA4_OES || ifmt == GL_PALETTE8_RGBA4_OES ||
                 ifmt == GL_PALETTE4_RGB5_A1_OES || ifmt == GL_PALETTE8_RGB5_A1_OES;
  /* the levels given, down to 8 x 8 */
  u32 nl = 0;
  while ((int)nl < nlev && nl < t->max_levels)
    nl++;
  u32 max = t->max_levels;
  t->max_levels = (u8)(nl ? nl : 1);
  int bad = tex_alloc_mem(t, 1024) < 0;
  t->max_levels = (u8)max;
  if (bad) {
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  for (u32 lv = 0; lv < nl; lv++) {
    u32 n = (t->w >> lv) * (t->h >> lv), nb = bits == 8 ? n : (n + 1) / 2;
    if (p + nb > end) {
      nl = lv;
      break;
    }
    u8 *d = TLV(t, lv);
    for (u32 i = 0; i < n; i++)
      d[i] = bits == 8 ? p[i] : (u8)((p[i / 2] >> (i & 1 ? 0 : 4)) & 15);
    p += nb;
  }
  t->levels = nl ? nl : 1;
  for (u32 i = 0; i < 256; i++)
    t->oclut[csm1(i)] = (u32)pal[i][0] | (u32)pal[i][1] << 8 | (u32)pal[i][2] << 16 |
                        (u32)((pal[i][3] + 1u) >> 1) << 24;
  tex_writeback(t);
  tex_new_gen(t); /* new texels: a new key */
}
void APIENTRY glCompressedTexImage2DARB(GLenum target, GLint level, GLenum ifmt, GLsizei w,
                                        GLsizei h, GLint border, GLsizei bytes, const GLvoid *data) {
  glCompressedTexImage2D(target, level, ifmt, w, h, border, bytes, data);
}

/* glPushAttrib .. glPopAttrib: the state of the draws (what display lists
   keep: enables, blend, alpha and depth tests, culling, the bound texture
   and its environment, colour mask, polygon offset), the scissor, the clip
   plane's enable, unit 1's texture; the mask is not looked at (all of it) */
#define ATTRIB_DEPTH 8
static struct {
  struct dl_state st;
  int scissor, sc[4], ucp_on, env_combine[2], env_scale[2], env_legacy;
  GLenum env_combine_rgb[2];
  int u1_tex2d;
  GLuint u1_bound;
  GLenum u1_texenv;
} attrib[ATTRIB_DEPTH];
static int attrib_n;
void APIENTRY glPushAttrib(GLbitfield mask) {
  (void)mask;
  if (attrib_n == ATTRIB_DEPTH) {
    gl_error(GL_STACK_OVERFLOW);
    return;
  }
  typeof(attrib[0]) *a = &attrib[attrib_n++];
  dl_get_state(&a->st);
  a->scissor = gl.scissor, memcpy(a->sc, gl.sc, sizeof a->sc), a->ucp_on = gl.ucp_on;
  memcpy(a->env_combine, env_combine, sizeof env_combine);
  memcpy(a->env_scale, env_scale, sizeof env_scale);
  memcpy(a->env_combine_rgb, env_combine_rgb, sizeof env_combine_rgb);
  a->env_legacy = env_legacy;
  a->u1_tex2d = gl.u1.tex2d, a->u1_bound = gl.u1.bound, a->u1_texenv = gl.u1.texenv;
}
void APIENTRY glPopAttrib(void) {
  if (!attrib_n) {
    gl_error(GL_STACK_UNDERFLOW);
    return;
  }
  typeof(attrib[0]) *a = &attrib[--attrib_n];
  dl_set_state(&a->st);
  if (gl.scissor != a->scissor || memcmp(gl.sc, a->sc, sizeof a->sc))
    st_serial++;
  gl.scissor = a->scissor, memcpy(gl.sc, a->sc, sizeof a->sc);
  if (gl.ucp_on != a->ucp_on)
    mat_serial++;
  gl.ucp_on = a->ucp_on;
  memcpy(env_combine, a->env_combine, sizeof env_combine);
  memcpy(env_scale, a->env_scale, sizeof env_scale);
  memcpy(env_combine_rgb, a->env_combine_rgb, sizeof env_combine_rgb);
  env_legacy = a->env_legacy;
  gl.u1.tex2d = a->u1_tex2d, gl.u1.bound = a->u1_bound, gl.u1.texenv = a->u1_texenv;
  /* the shift as it was (dl_set_state's), not made again from the env */
}

/* GL_ARB_multitexture: two units */
void APIENTRY glActiveTexture(GLenum t) {
  if (t != GL_TEXTURE0 && t != GL_TEXTURE1) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  gl.cunit = t == GL_TEXTURE1;
}
void APIENTRY glActiveTextureARB(GLenum t) { glActiveTexture(t); }
void APIENTRY glClientActiveTexture(GLenum t) {
  if (t != GL_TEXTURE0 && t != GL_TEXTURE1) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  gl.ccunit = t == GL_TEXTURE1;
}
void APIENTRY glClientActiveTextureARB(GLenum t) { glClientActiveTexture(t); }
void APIENTRY glMultiTexCoord2f(GLenum t, GLfloat s, GLfloat tt) {
  if (t == GL_TEXTURE1 && !imm.st1 && !(imm.st1 = malloc(IMM_MAX * sizeof *imm.st1))) {
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  float *c = t == GL_TEXTURE1 ? imm.cur_st1 : imm.cur_st;
  c[0] = s, c[1] = tt;
}
void APIENTRY glMultiTexCoord2fARB(GLenum t, GLfloat s, GLfloat tt) { glMultiTexCoord2f(t, s, tt); }
void APIENTRY glMultiTexCoord2fv(GLenum t, const GLfloat *v) { glMultiTexCoord2f(t, v[0], v[1]); }
void APIENTRY glMultiTexCoord2fvARB(GLenum t, const GLfloat *v) { glMultiTexCoord2f(t, v[0], v[1]); }

/* ---- set-up ----------------------------------------------------------- */


static int mode_set = -1; /* mxgl_mode: none yet */
static int mode_want;     /* the flags asked for with it */

/* without beam racing: video mode m, two CT16S buffers of w x h and a Z
   buffer of zpsm */
static int start_buffers(const struct mx_video_mode *m, int w, int h, u32 zpsm) {
  if (mx_gs_init_video(m) < 0)
    return -1;
  fb_w = w, fb_h = h;
  u32 zbp = mx_vram_alloc(mx_rt_pages((u32)w, (u32)h, zpsm));
  for (int j = 0; j < 2; j++) {
    u32 fbp = mx_vram_alloc(mx_rt_pages((u32)w, (u32)h, MX_GS_PSM_CT16S));
    mx_rt_init(&rt[j], fbp, (u32)w, (u32)h, MX_GS_PSM_CT16S, zbp, zpsm);
    mx_display_init(&disp[j], &rt[j]);
  }
  mx_gs_displays(disp, 2);
  mx_vu_reset(2);
  /* chain mode: the stream calls the display lists' chains */
  mx_frame_init_chain(ring, sizeof ring, (void *)0x70002000, 0x2000);
  /* frames as fast as they are made (the screen tears): the real rate */
  mx_frame_vsync(0);
  beam = rgb24 = 0;
  zmax = zpsm == MX_GS_PSM_Z16S || zpsm == MX_GS_PSM_Z16 ? ZMAX_Z16 : ZMAX_Z24;
  return 0;
}

/* beam racing (maxsdk's passes, <mx/hires.h>): video mode m in interlaced
   FRAME mode, one buffer of w x h (a field: both fields' lines, half a line
   apart) in psm, Z (16 bits; Z32 for 32-bit colour) in the passes' strips;
   the records of the frame stream (each played every field until the next
   replaces it) and arenas 2, 3 are taken while it is on */
static int start_beam(const struct mx_video_mode *m, int w, int h, u32 psm) {
  if (!hires_mem)
    hires_mem = mx_heap_alloc(gl_stream, REC_BYTES + 2 * ARENA_BYTES, 64);
  if (!hires_mem)
    return -1;
  mx_vu_reset(2);
  if (mx_hires_init(m, (u32)w, (u32)h, psm, hires_mem, REC_BYTES, (void *)0x70002000, 0x2000) < 0)
    return -1;
  fb_w = w, fb_h = h;
  for (int i = 2; i < NARENA; i++) {
    arena[i] = hires_mem + REC_BYTES + (u32)(i - 2) * ARENA_BYTES;
    mx_dcache_invalidate(arena[i], ARENA_BYTES);
  }
  beam = 1;
  rgb24 = psm == MX_GS_PSM_CT32 || psm == MX_GS_PSM_CT24;
  zmax = rgb24 ? ZMAX_Z24 : ZMAX_Z16;
  return 0;
}

static void hires_free(void) {
  mx_heap_free(gl_stream, hires_mem);
  hires_mem = NULL;
  arena[2] = arena[3] = NULL;
}

/* 1080i without beam racing: 640 x 540 shown by both fields (FRAME mode),
   stretched to 1920 x 1080 */
static struct mx_video_mode video_1080i_frame;

static int mode_flags; /* MXGL_BEAM, MXGL_RGB24 in effect */

/* the mode's picture: with beam racing if asked (and memory allows), else
   two buffers; returns 0, or -1 for beam racing not set */
static int start_mode(int mode, int flags) {
  int pal = mode == MXGL_PAL, hd = mode == MXGL_1080I;
  if (flags & MXGL_BEAM) {
    /* SD: 640 x 224 / 256 a field; 32-bit colour fits only there */
    int ok = hd ? start_beam(&mx_video_1080i, 1920, 540, MX_GS_PSM_CT16S)
                : start_beam(pal ? &mx_video_pal : &mx_video_ntsc, 640, pal ? 256 : 224,
                             flags & MXGL_RGB24 ? MX_GS_PSM_CT24 : MX_GS_PSM_CT16S);
    if (ok == 0) {
      mode_flags = MXGL_BEAM | (rgb24 ? MXGL_RGB24 : 0);
      scr_w = hd ? 1920 : 640, scr_h = 2 * fb_h;
      return 0;
    }
    mx_printf("mxgl: no beam racing (memory %p)\n", hires_mem);
  }
  hires_free();
  mode_flags = 0;
  if (hd) {
    video_1080i_frame = mx_video_1080i;
    video_1080i_frame.ffmd = 1;
    /* Z16: the room for textures as with beam racing (Z24: 1.3 MiB) */
    start_buffers(&video_1080i_frame, 640, 540, MX_GS_PSM_Z16S);
    scr_w = 1920, scr_h = 1080;
  } else {
    start_buffers(pal ? &mx_video_pal : &mx_video_ntsc, MXGL_FB_W, pal ? 512 : 448, MXGL_FB_Z);
    scr_w = fb_w, scr_h = fb_h;
  }
  return flags & MXGL_BEAM ? -1 : 0;
}

int mxgl_set_mode(int mode) {
  int flags = mode & (MXGL_BEAM | MXGL_RGB24);
  mode &= 0xFF;
  if (mode == MXGL_SD)
    mode = mx_video_default() == &mx_video_pal ? MXGL_PAL : MXGL_NTSC;
  if (mode == mode_set && flags == mode_want)
    return 0;
  if (mode_set >= 0) {
    /* the frames sent are shown (two buffers) or the passes stopped (beam
       racing), then the pipeline starts again in the new mode */
    pq_flush();
    if (in_frame)
      mxgl_end_frame();
    u32 t0 = mx_count();
    while (!beam && mx_gs.shown != mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
      ;
    mx_frame_stop();
    /* no frame reads anything now: what was freed goes (the room for
       beam racing, after a game freed its textures for a mode change) */
    for (int k = 0; k < NDEFER; k++) {
      for (int i = 0; i < ndeferred[k]; i++)
        gl_free(deferred[k][i]);
      ndeferred[k] = 0;
    }
  }
  int r = start_mode(mode, flags);
  mode_set = mode, mode_want = flags;
  frame_par = 0;
  mx_printf("mxgl: %s %ux%u%s%s, buffer %ux%u\n",
            mode == MXGL_1080I ? "1080i" : mode == MXGL_PAL ? "PAL" : "NTSC", scr_w, scr_h,
            beam ? ", beam racing" : "", rgb24 ? ", 24-bit colour" : "", fb_w, fb_h);
  /* the HUD's font (256 x 8) in the first page after the buffers, the
     CLUTs after it (pages of libgl's own), then the textures (the cache,
     in the pages left): all copied in again */
  mx_gs.w = (u32)fb_w, mx_gs.h = (u32)fb_h; /* the HUD's coordinates */
  mx_hud_init(mx_gs.tbp, (u32)fb_h);
  clut_cbp = mx_gs.tbp + mx_tex_blocks(256, 8, MX_GS_PSM_CT32);
  vram_lo = clut_cbp + 4 * (NCLUT + 1); /* and the palette's */
  u32 p0 = mx_gs.tbp / 32, p1 = (vram_lo + 31) / 32;
  if (mx_vram_take(p0, p1 - p0, MX_VRAM_APP) < 0)
    mx_printf("mxgl: GS memory for the font and CLUTs taken\n");
  /* the pages below the buffers count too (beam racing's start high: mx_hires_init) */
  vram_lo = (p1 - (beam ? mx_gs.fbp[0] : 0)) * 32;
  /* beam racing: uploads go in the once records (PATH3), ahead of the
     passes of the frames before: only what the GS is done with goes */
#ifndef MXGL_VCACHE_POLICY
#define MXGL_VCACHE_POLICY MX_VCACHE_AUTO
#endif
  mx_vcache_init(0, beam ? MX_VCACHE_ASYNC : MX_VCACHE_STREAM, MXGL_VCACHE_POLICY);
  /* the top bytes no drawing writes (24-bit colour strips, a Z24 buffer):
     8-bit textures as T8H (the cache gives mt.psm) */
  u32 hp = 0, hn = 0;
#ifndef MXGL_NO_HBYTE /* debugging: no textures in top bytes */
  if (beam)
    hn = mx_hires_hbyte(&hp);
  else if (rt[0].zpsm == MX_GS_PSM_Z24)
    hp = rt[0].zbp, hn = mx_rt_pages((u32)fb_w, (u32)fb_h, MX_GS_PSM_Z24);
#endif
  mx_vcache_hpages(hp, hn);
  h_blocks = hn * 32;
  vram_gen++;
  gs.valid = 0, gs_serial++;
  mxgl_window(scr_w, scr_h);
  return r;
}

int mxgl_mode_flags(void) { return mode_flags; }

int mxgl_mode(void) { return mode_set; }



void mxgl_screen(int *w, int *h) {
  *w = scr_w;
  *h = scr_h;
}

void mxgl_init(void) {
  static int done;
  if (done)
    return;
  done = 1;
  gl_heap = mx_heap_create("gl", MX_MEM_PINNED, 0);
  gl_stream = mx_heap_create("gl.stream", MX_MEM_STREAM, 0);
  if (mx_vm_active())
    gl_assets = mx_heap_create("gl.textures", MX_MEM_ASSET, GL_ASSET_BYTES);
#if MXGL_VU1CLIP
  shader = MX_VU_SHADER(MXGLC, mxglc_vu, mxglc_vu_end);
#else
  shader = MX_VU_SHADER(MXGLV, mxglv_vu, mxglv_vu_end);
#endif
#if MXGL_VU1CLIP
  struct mx_shader fs = MX_VU_SHADER(MXGLCF, mxglcf_vu, mxglcf_vu_end);
  fog_program(0, &fs);
  fs = MX_VU_SHADER(MXGLCE, mxglce_vu, mxglce_vu_end);
  fog_program(1, &fs);
#else
  struct mx_shader fs = MX_VU_SHADER(MXGLVF, mxglvf_vu, mxglvf_vu_end);
  fog_program(0, &fs);
  fs = MX_VU_SHADER(MXGLVE, mxglve_vu, mxglve_vu_end);
  fog_program(1, &fs);
#endif
  for (int i = 0; i < 2; i++) {
    arena[i] = mx_heap_alloc(gl_stream, ARENA_BYTES, 64);
    /* written through UCAB only: no line of them may be in the cache */
    if (arena[i])
      mx_dcache_invalidate(arena[i], ARENA_BYTES);
  }
  if (!arena[1])
    mx_printf("mxgl: no memory for the arenas (2 x %u KB)\n", ARENA_BYTES / 1024);
  clut_make();
  mxgl_set_mode(MXGL_SD);
  memset(&gl, 0, sizeof gl);
  for (int i = 0; i < MV_DEPTH; i++)
    mat_ident(gl.mv[i]);
  for (int i = 0; i < PR_DEPTH; i++)
    mat_ident(gl.pr[i]);
  for (int i = 0; i < TM_DEPTH; i++)
    mat_ident(gl.tm[i]);
  gl.pack_align = 4;
  gl.mode = GL_MODELVIEW;
  gl.vp[2] = gl_w, gl.vp[3] = gl_h; /* the window of mxgl_set_mode */
  gl.df = 1;
  gl.fog_mode = GL_EXP, gl.fog_density = 1, gl.fog_end = 1; /* GL's defaults */
  gl.fog_a = 1;
  gl.sf = GL_ONE, gl.dfac = GL_ZERO;
  gl.dfunc = GL_LESS, gl.dmask = 1, gl.cullface = GL_BACK, gl.afunc = GL_ALWAYS;
  gl.st_func = GL_ALWAYS, gl.st_op = GL_KEEP, gl.st_mask = ~0u, gl.st_wmask = ~0u, gl.st_vb = -1;
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

#ifdef MXGL_CHECK_TEX
/* debugging (the pipeline stopped): every resident texture's level 0 in
   GS memory against its texels in RAM; and the CLUTs at clut_cbp */
static void check_textures(void) {
  static u8 buf[128 * 128 * 4] MX_ALIGNED(64);
  u32 nt = 0, nbad = 0;
  for (u32 i = 1; i < MAX_TEX; i++) {
    struct tex *t = &texs[i];
    if (!t->used || !t->data)
      continue;
    struct mx_tex m;
    u32 f = t->m.from;
    if (!mx_vcache_peek(mx_vcache_tex_key(tex_key(t), f), &m))
      continue;
    u32 bytes = (t->w >> f) * (t->h >> f) * tex_bpp(t);
    if (f)
      mx_printf("check: tex %u %ux%u from level %u (levels %u, tail %u, mip %d, pal %d): %ux%u at %u, "
                "TEX0 tw %u th %u levels %u\n", i, t->w, t->h, f, t->m.levels, t->m.tail, t->mip,
                t->pal, t->w >> f, t->h >> f, m.tbp[0], m.tw, m.th, m.levels);
    if (bytes % 128 || bytes > sizeof buf)
      continue;
    nt++;
    if (mx_gs_download(buf, m.tbp[0], m.tbw[0], m.psm, 0, 0, t->w >> f, t->h >> f) < 0)
      continue;
    u32 bad = 0;
    for (u32 k = 0; k < bytes; k++)
      bad += buf[k] != TLV(t, f)[k];
    if (bad && nbad++ < 20)
      mx_printf("check: tex %u %ux%u psm %u bright %d pal %d at %u: %u of %u bytes wrong\n", i, t->w,
                t->h, m.psm, t->bright, t->pal, m.tbp[0], bad, bytes);
  }
  mx_printf("check: %u textures, %u wrong\n", nt, nbad);
  static u32 cl[256] MX_ALIGNED(64);
  for (int k = 0; k <= NCLUT; k++) {
    if (mx_gs_download(cl, clut_cbp + 4 * (u32)k, 1, MX_GS_PSM_CT32, 0, 0, 16, 16) < 0)
      continue;
    const u32 *want = k < NCLUT ? clut[k] : clut_pal;
    u32 bad = 0;
    for (int j = 0; j < 256; j++)
      bad += cl[j] != want[j];
    mx_printf("check: CLUT %d: %u of 256 entries wrong (first %08x, want %08x)\n", k, bad, cl[0], want[0]);
  }
}
#endif

int mxgl_screenshot(const char *path) {
  /* beam racing: both fields (1920 x 1080, 640 x 448 / 512) */
  if (beam) {
    int r = mx_hires_save_tga(path);
#ifdef MXGL_CHECK_TEX
    check_textures();
#endif
    return r;
  }
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

/* ---- vertex programs (GL 2.0 shaders and programs, GL_PS2_VU1_BINARY) ----- */

/* VU1's micro memory (64-bit units): libgl's own program from 0, the
   programs' code after it, each where it stays (a switch is a call: no
   upload, and no FLUSH between programs of the same layout); when it is
   full, from after libgl's own again (maxsdk uploads what is not
   resident) */
#define VU1_CODE 2048
#define MAX_GLSL 64
struct vprog {
  struct mx_shader s; /* its entry where the code was placed */
  u32 at;             /* the code's address */
  const struct mxgl_vu1_binary *bin;
  int r_mvp, r_cscale, r_tms, r_tmt; /* libgl's uniforms: quadwords, -1 none */
  int r_clip;                        /* mxglc.i's clipping uniforms */
  float (*uni)[4];
};
static struct {
  u8 kind; /* 0 free, 1 shader, 2 program */
  u8 linked;
  const struct mxgl_vu1_binary *bin; /* a shader's; a program's attached */
  struct vprog *p;
  const char *log;
} glsl[MAX_GLSL];
static u32 vu1_next;

/* room in VU1's micro memory for n double words, after libgl's own program */
static u32 vu1_place(u32 n) {
  u32 own = (shader.code_bytes + 7) / 8;
  if (vu1_next < own || vu1_next + n > VU1_CODE)
    vu1_next = own;
  u32 at = vu1_next;
  vu1_next += n;
  return at;
}

/* a fog program: placed once, drawn with o_uni (no uni of its own) */
static void fog_program(int i, const struct mx_shader *s) {
  static struct vprog progs[2];
  struct vprog *p = fog_prog[i] = &progs[i];
  p->at = vu1_place((s->code_bytes + 7) / 8);
  p->s = *s;
  p->s.entry = s->entry + p->at;
}

/* the stream's program becomes p's (NULL: libgl's own); its shader */
static const struct mx_shader *vprog_use(struct vprog *p) {
  const struct mx_shader *s = p ? &p->s : &shader;
  if (s == vu_cur)
    return s;
  const struct mx_shader *o = vu_cur;
  if (!o || s->buf_qw != o->buf_qw || s->ctx_at != o->ctx_at || s->ctx_qw != o->ctx_qw ||
      s->nctx != o->nctx || s->in_at != o->in_at || s->out_at != o->out_at) {
    /* another layout: its inputs or contexts may be where the GIF still
       reads the last program's packets (as mx_frame_shader_setup) */
    u64 *c = mx_frame_reserve(2);
    c = mx_e_cnt(c, 1, mx_vif_flush(), mx_vif_base(0));
    c = mx_e_vif4(c, mx_vif_offset(s->buf_qw), mx_vif_stcycl(1, 1), 0, 0);
    mx_frame_commit(c);
  }
  mx_frame_mpg(s->code, s->code_bytes, p ? p->at : 0);
  vu_cur = s;
  return s;
}

/* p's uniforms with libgl's (mvp, cscale, tms, tmt) from those of its own
   program; fixed itself for libgl's */
static const void *vprog_uni(struct vprog *p, const float (*fixed)[4]) {
  if (!p || !p->uni) /* libgl's own, the fog programs: its layout */
    return fixed;
  if (p->r_mvp >= 0)
    memcpy(p->uni[p->r_mvp], fixed[VU_MXGLV_MVP], 64);
  if (p->r_cscale >= 0)
    memcpy(p->uni[p->r_cscale], fixed[VU_MXGLV_CSCALE], 16);
#if MXGL_VU1CLIP
  if (p->r_clip >= 0)
    memcpy(p->uni[p->r_clip], fixed[VU_MXGLV_UNI_QW], (MXGL_UNI_QW - VU_MXGLV_UNI_QW) * 16);
#endif
  if (p->r_tms >= 0)
    memcpy(p->uni[p->r_tms], fixed[VU_MXGLV_TMS], 16);
  if (p->r_tmt >= 0)
    memcpy(p->uni[p->r_tmt], fixed[VU_MXGLV_TMT], 16);
  return p->uni;
}

static int glsl_new(int kind) {
  for (int i = 1; i < MAX_GLSL; i++)
    if (!glsl[i].kind) {
      glsl[i].kind = (u8)kind, glsl[i].linked = 0, glsl[i].bin = NULL, glsl[i].p = NULL;
      glsl[i].log = "";
      return i;
    }
  gl_error(GL_OUT_OF_MEMORY);
  return 0;
}
static int glsl_is(GLuint id, int kind) { return id && id < MAX_GLSL && glsl[id].kind == kind; }

GLuint APIENTRY glCreateShader(GLenum type) {
  if (type != GL_VERTEX_SHADER) {
    gl_error(GL_INVALID_ENUM);
    return 0;
  }
  return (GLuint)glsl_new(1);
}
void APIENTRY glDeleteShader(GLuint sh) {
  if (glsl_is(sh, 1))
    glsl[sh].kind = 0;
}
/* binary: a struct mxgl_vu1_binary (<mxgl.h>) */
void APIENTRY glShaderBinary(GLsizei count, const GLuint *shaders, GLenum format, const void *binary,
                             GLsizei length) {
  const struct mxgl_vu1_binary *b = binary;
  if (format != GL_PS2_VU1_BINARY || !b || length < (GLsizei)sizeof *b || b->magic != MXGL_VU1_MAGIC) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  for (int i = 0; i < count; i++)
    if (glsl_is(shaders[i], 1))
      glsl[shaders[i]].bin = b;
    else
      gl_error(GL_INVALID_VALUE);
}
void APIENTRY glGetShaderiv(GLuint sh, GLenum pname, GLint *v) {
  if (!glsl_is(sh, 1)) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  *v = pname == GL_COMPILE_STATUS ? glsl[sh].bin != NULL : pname == GL_SHADER_TYPE ? GL_VERTEX_SHADER : 0;
}
GLuint APIENTRY glCreateProgram(void) { return (GLuint)glsl_new(2); }
void APIENTRY glAttachShader(GLuint prog, GLuint sh) {
  if (!glsl_is(prog, 2) || !glsl_is(sh, 1)) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  glsl[prog].bin = glsl[sh].bin;
}
void APIENTRY glDetachShader(GLuint prog, GLuint sh) {
  if (glsl_is(prog, 2))
    glsl[prog].bin = NULL;
}
void APIENTRY glDeleteProgram(GLuint prog) {
  if (!glsl_is(prog, 2))
    return;
  struct vprog *p = glsl[prog].p;
  if (p && (p == cur_prog || p == o_prog)) {
    /* still in use (the batch's, or glUseProgram's): its memory is kept
       (a few hundred bytes) */
    glsl[prog].kind = 0;
    return;
  }
  if (p) {
    gl_free(p->uni);
    free(p);
  }
  glsl[prog].kind = 0;
}

/* the uniform name (lower case in the binary) of a cbuffer member */
static int uni_find(const struct mxgl_vu1_binary *b, const char *name) {
  for (u32 i = 0; i < b->nuni; i++) {
    const char *a = b->uni[i].name, *n = name;
    while (*a && *n && (*n | 0x20) == *a)
      a++, n++;
    if (!*a && !*n)
      return (int)i;
  }
  return -1;
}

/* the binary's program placed in VU1's micro memory; its inputs must be
   libgl's own (the batches and the display lists' chains are made for
   them) */
void APIENTRY glLinkProgram(GLuint prog) {
  if (!glsl_is(prog, 2)) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  glsl[prog].linked = 0;
  const struct mxgl_vu1_binary *b = glsl[prog].bin;
  if (!b) {
    glsl[prog].log = "no shader binary attached";
    return;
  }
  struct mx_shader s;
  b->shader(&s);
  if (s.nin != shader.nin || memcmp(s.in, shader.in, sizeof s.in[0] * s.nin) ||
      s.in_at != shader.in_at || s.align != shader.align || s.max_i) {
    glsl[prog].log = "inputs not float4 POSITION, float2 TEXCOORD, uint4 COLOR_V4_8U (mxglv.hlsl)";
    return;
  }
  u32 own = (shader.code_bytes + 7) / 8, n = (s.code_bytes + 7) / 8;
  if (own + n > VU1_CODE) {
    glsl[prog].log = "code larger than VU1's micro memory";
    return;
  }
  struct vprog *p = glsl[prog].p;
  if (!p) {
    p = calloc(1, sizeof *p);
    if (!p) {
      gl_error(GL_OUT_OF_MEMORY);
      return;
    }
  } else {
    gl_free(p->uni);
    p->uni = NULL;
  }
  p->at = vu1_place(n);
  p->s = s;
  p->s.entry = s.entry + p->at;
  p->bin = b;
  p->uni = gl_alloc(s.uni_qw * 16 + 16, 16);
  if (!p->uni) {
    free(p);
    glsl[prog].p = NULL;
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  memset(p->uni, 0, s.uni_qw * 16);
  int k;
  p->r_mvp = (k = uni_find(b, "mvp")) >= 0 ? b->uni[k].at : -1;
  p->r_cscale = (k = uni_find(b, "cscale")) >= 0 ? b->uni[k].at : -1;
  p->r_clip = (k = uni_find(b, "clip")) >= 0 ? b->uni[k].at : -1;
  p->r_tms = (k = uni_find(b, "tms")) >= 0 ? b->uni[k].at : -1;
  p->r_tmt = (k = uni_find(b, "tmt")) >= 0 ? b->uni[k].at : -1;
  glsl[prog].p = p;
  glsl[prog].linked = 1;
  glsl[prog].log = "";
}
void APIENTRY glGetProgramiv(GLuint prog, GLenum pname, GLint *v) {
  if (!glsl_is(prog, 2)) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  switch (pname) {
  case GL_LINK_STATUS: *v = glsl[prog].linked; break;
  case GL_INFO_LOG_LENGTH: *v = (GLint)strlen(glsl[prog].log) + 1; break;
  case GL_ATTACHED_SHADERS: *v = glsl[prog].bin != NULL; break;
  case GL_ACTIVE_UNIFORMS: *v = glsl[prog].bin ? (GLint)glsl[prog].bin->nuni : 0; break;
  case GL_ACTIVE_ATTRIBUTES: *v = glsl[prog].bin ? (GLint)glsl[prog].bin->nin : 0; break;
  default: gl_error(GL_INVALID_ENUM);
  }
}
void APIENTRY glGetProgramInfoLog(GLuint prog, GLsizei size, GLsizei *len, GLchar *log) {
  const char *m = glsl_is(prog, 2) ? glsl[prog].log : "";
  GLsizei n = (GLsizei)strlen(m);
  if (size > 0) {
    n = n < size - 1 ? n : size - 1;
    memcpy(log, m, (size_t)n);
    log[n] = 0;
  }
  if (len)
    *len = n;
}
void APIENTRY glUseProgram(GLuint prog) {
  if (!prog) {
    cur_prog = NULL, cur_prog_clips = 0;
    return;
  }
  if (!glsl_is(prog, 2) || !glsl[prog].linked) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  cur_prog = glsl[prog].p;
  cur_prog_clips = cur_prog->r_clip >= 0;
}
/* a float's index in the uniforms (the member's first), or -1 */
GLint APIENTRY glGetUniformLocation(GLuint prog, const GLchar *name) {
  if (!glsl_is(prog, 2) || !glsl[prog].linked)
    return -1;
  int k = uni_find(glsl[prog].bin, name);
  return k < 0 ? -1 : (GLint)glsl[prog].bin->uni[k].at * 4;
}
/* the input's index (by its semantic: POSITION, TEXCOORD, COLOR), or -1 */
GLint APIENTRY glGetAttribLocation(GLuint prog, const GLchar *name) {
  if (!glsl_is(prog, 2) || !glsl[prog].bin)
    return -1;
  const struct mxgl_vu1_binary *b = glsl[prog].bin;
  for (u32 i = 0; i < b->nin; i++)
    if (!strcmp(b->in[i], name))
      return (GLint)i;
  return -1;
}

/* n floats of the program in use from location loc (the batch drawn with
   the old values first) */
static float *uni_at(GLint loc, int n) {
  struct vprog *p = cur_prog;
  if (!p) {
    gl_error(GL_INVALID_OPERATION);
    return NULL;
  }
  if (loc < 0 || (u32)loc + (u32)n > p->s.uni_qw * 4) {
    if (loc != -1)
      gl_error(GL_INVALID_OPERATION);
    return NULL;
  }
  if (o_n && o_prog == p)
    flush_out();
  return &p->uni[0][0] + loc;
}
void APIENTRY glUniform1f(GLint loc, GLfloat x) {
  float *u = uni_at(loc, 1);
  if (u)
    u[0] = x;
}
void APIENTRY glUniform2f(GLint loc, GLfloat x, GLfloat y) {
  float *u = uni_at(loc, 2);
  if (u)
    u[0] = x, u[1] = y;
}
void APIENTRY glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z) {
  float *u = uni_at(loc, 3);
  if (u)
    u[0] = x, u[1] = y, u[2] = z;
}
void APIENTRY glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
  float *u = uni_at(loc, 4);
  if (u)
    u[0] = x, u[1] = y, u[2] = z, u[3] = w;
}
/* arrays: one element a quadword (a cbuffer's float4 array) */
static void uni_v(GLint loc, GLsizei count, const GLfloat *v, int k) {
  float *u = uni_at(loc, count > 0 ? (count - 1) * 4 + k : 0);
  if (u)
    for (int i = 0; i < count; i++)
      memcpy(u + 4 * i, v + k * i, (size_t)k * 4);
}
void APIENTRY glUniform1fv(GLint loc, GLsizei count, const GLfloat *v) { uni_v(loc, count, v, 1); }
void APIENTRY glUniform2fv(GLint loc, GLsizei count, const GLfloat *v) { uni_v(loc, count, v, 2); }
void APIENTRY glUniform3fv(GLint loc, GLsizei count, const GLfloat *v) { uni_v(loc, count, v, 3); }
void APIENTRY glUniform4fv(GLint loc, GLsizei count, const GLfloat *v) { uni_v(loc, count, v, 4); }
/* column-major as GL's matrices (libgl's mvp is too) */
void APIENTRY glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v) {
  float *u = uni_at(loc, count * 16);
  if (!u)
    return;
  for (int i = 0; i < count; i++, u += 16, v += 16)
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++)
        u[4 * c + r] = transpose ? v[4 * r + c] : v[4 * c + r];
}

/* ---- mxglGetProcAddress ---------------------------------------------------- */

/* every GL call of libgl by name (the extensions' too), in strcmp order */
static const struct {
  const char *name;
  void (*fn)(void);
} procs[] = {
    {"glActiveTexture", (void (*)(void))glActiveTexture},
    {"glActiveTextureARB", (void (*)(void))glActiveTextureARB},
    {"glAlphaFunc", (void (*)(void))glAlphaFunc},
    {"glArrayElement", (void (*)(void))glArrayElement},
    {"glAttachShader", (void (*)(void))glAttachShader},
    {"glBegin", (void (*)(void))glBegin},
    {"glBindBuffer", (void (*)(void))glBindBuffer},
    {"glBindBufferARB", (void (*)(void))glBindBufferARB},
    {"glBindTexture", (void (*)(void))glBindTexture},
    {"glBlendFunc", (void (*)(void))glBlendFunc},
    {"glBufferData", (void (*)(void))glBufferData},
    {"glBufferDataARB", (void (*)(void))glBufferDataARB},
    {"glBufferSubData", (void (*)(void))glBufferSubData},
    {"glBufferSubDataARB", (void (*)(void))glBufferSubDataARB},
    {"glCallList", (void (*)(void))glCallList},
    {"glClear", (void (*)(void))glClear},
    {"glClearColor", (void (*)(void))glClearColor},
    {"glClearDepth", (void (*)(void))glClearDepth},
    {"glClearDepthf", (void (*)(void))glClearDepthf},
    {"glClearStencil", (void (*)(void))glClearStencil},
    {"glClientActiveTexture", (void (*)(void))glClientActiveTexture},
    {"glClientActiveTextureARB", (void (*)(void))glClientActiveTextureARB},
    {"glClipPlane", (void (*)(void))glClipPlane},
    {"glClipPlanef", (void (*)(void))glClipPlanef},
    {"glColor3f", (void (*)(void))glColor3f},
    {"glColor3fv", (void (*)(void))glColor3fv},
    {"glColor3ub", (void (*)(void))glColor3ub},
    {"glColor3ubv", (void (*)(void))glColor3ubv},
    {"glColor4f", (void (*)(void))glColor4f},
    {"glColor4fv", (void (*)(void))glColor4fv},
    {"glColor4ub", (void (*)(void))glColor4ub},
    {"glColor4ubv", (void (*)(void))glColor4ubv},
    {"glColorMask", (void (*)(void))glColorMask},
    {"glColorPointer", (void (*)(void))glColorPointer},
    {"glColorTable", (void (*)(void))glColorTable},
    {"glColorTableEXT", (void (*)(void))glColorTableEXT},
    {"glCompressedTexImage2D", (void (*)(void))glCompressedTexImage2D},
    {"glCompressedTexImage2DARB", (void (*)(void))glCompressedTexImage2DARB},
    {"glCopyTexSubImage2D", (void (*)(void))glCopyTexSubImage2D},
    {"glCreateProgram", (void (*)(void))glCreateProgram},
    {"glCreateShader", (void (*)(void))glCreateShader},
    {"glCullFace", (void (*)(void))glCullFace},
    {"glDeleteBuffers", (void (*)(void))glDeleteBuffers},
    {"glDeleteBuffersARB", (void (*)(void))glDeleteBuffersARB},
    {"glDeleteLists", (void (*)(void))glDeleteLists},
    {"glDeleteProgram", (void (*)(void))glDeleteProgram},
    {"glDeleteShader", (void (*)(void))glDeleteShader},
    {"glDeleteTextures", (void (*)(void))glDeleteTextures},
    {"glDepthFunc", (void (*)(void))glDepthFunc},
    {"glDepthMask", (void (*)(void))glDepthMask},
    {"glDepthRange", (void (*)(void))glDepthRange},
    {"glDepthRangef", (void (*)(void))glDepthRangef},
    {"glDetachShader", (void (*)(void))glDetachShader},
    {"glDisable", (void (*)(void))glDisable},
    {"glDisableClientState", (void (*)(void))glDisableClientState},
    {"glDrawArrays", (void (*)(void))glDrawArrays},
    {"glDrawBuffer", (void (*)(void))glDrawBuffer},
    {"glDrawElements", (void (*)(void))glDrawElements},
    {"glDrawRangeElements", (void (*)(void))glDrawRangeElements},
    {"glDrawRangeElementsEXT", (void (*)(void))glDrawRangeElementsEXT},
    {"glEnable", (void (*)(void))glEnable},
    {"glEnableClientState", (void (*)(void))glEnableClientState},
    {"glEnd", (void (*)(void))glEnd},
    {"glEndList", (void (*)(void))glEndList},
    {"glFinish", (void (*)(void))glFinish},
    {"glFlush", (void (*)(void))glFlush},
    {"glFogf", (void (*)(void))glFogf},
    {"glFogfv", (void (*)(void))glFogfv},
    {"glFogi", (void (*)(void))glFogi},
    {"glFogiv", (void (*)(void))glFogiv},
    {"glFrontFace", (void (*)(void))glFrontFace},
    {"glFrustum", (void (*)(void))glFrustum},
    {"glFrustumf", (void (*)(void))glFrustumf},
    {"glGenBuffers", (void (*)(void))glGenBuffers},
    {"glGenBuffersARB", (void (*)(void))glGenBuffersARB},
    {"glGenLists", (void (*)(void))glGenLists},
    {"glGenTextures", (void (*)(void))glGenTextures},
    {"glGetAttribLocation", (void (*)(void))glGetAttribLocation},
    {"glGetBooleanv", (void (*)(void))glGetBooleanv},
    {"glGetError", (void (*)(void))glGetError},
    {"glGetFloatv", (void (*)(void))glGetFloatv},
    {"glGetIntegerv", (void (*)(void))glGetIntegerv},
    {"glGetProgramInfoLog", (void (*)(void))glGetProgramInfoLog},
    {"glGetProgramiv", (void (*)(void))glGetProgramiv},
    {"glGetShaderiv", (void (*)(void))glGetShaderiv},
    {"glGetString", (void (*)(void))glGetString},
    {"glGetTexImage", (void (*)(void))glGetTexImage},
    {"glGetUniformLocation", (void (*)(void))glGetUniformLocation},
    {"glHint", (void (*)(void))glHint},
    {"glIsBuffer", (void (*)(void))glIsBuffer},
    {"glIsBufferARB", (void (*)(void))glIsBufferARB},
    {"glIsEnabled", (void (*)(void))glIsEnabled},
    {"glIsList", (void (*)(void))glIsList},
    {"glIsTexture", (void (*)(void))glIsTexture},
    {"glLineWidth", (void (*)(void))glLineWidth},
    {"glLinkProgram", (void (*)(void))glLinkProgram},
    {"glLoadIdentity", (void (*)(void))glLoadIdentity},
    {"glLoadMatrixd", (void (*)(void))glLoadMatrixd},
    {"glLoadMatrixf", (void (*)(void))glLoadMatrixf},
    {"glLockArraysEXT", (void (*)(void))glLockArraysEXT},
    {"glMatrixMode", (void (*)(void))glMatrixMode},
    {"glMultMatrixd", (void (*)(void))glMultMatrixd},
    {"glMultMatrixf", (void (*)(void))glMultMatrixf},
    {"glMultiDrawArrays", (void (*)(void))glMultiDrawArrays},
    {"glMultiDrawArraysEXT", (void (*)(void))glMultiDrawArraysEXT},
    {"glMultiDrawElements", (void (*)(void))glMultiDrawElements},
    {"glMultiModeDrawArraysIBM", (void (*)(void))glMultiModeDrawArraysIBM},
    {"glMultiTexCoord2f", (void (*)(void))glMultiTexCoord2f},
    {"glMultiTexCoord2fARB", (void (*)(void))glMultiTexCoord2fARB},
    {"glMultiTexCoord2fv", (void (*)(void))glMultiTexCoord2fv},
    {"glMultiTexCoord2fvARB", (void (*)(void))glMultiTexCoord2fvARB},
    {"glNewList", (void (*)(void))glNewList},
    {"glOrtho", (void (*)(void))glOrtho},
    {"glOrthof", (void (*)(void))glOrthof},
    {"glPixelStorei", (void (*)(void))glPixelStorei},
    {"glPointSize", (void (*)(void))glPointSize},
    {"glPolygonMode", (void (*)(void))glPolygonMode},
    {"glPolygonOffset", (void (*)(void))glPolygonOffset},
    {"glPopAttrib", (void (*)(void))glPopAttrib},
    {"glPopMatrix", (void (*)(void))glPopMatrix},
    {"glPushAttrib", (void (*)(void))glPushAttrib},
    {"glPushMatrix", (void (*)(void))glPushMatrix},
    {"glReadBuffer", (void (*)(void))glReadBuffer},
    {"glReadPixels", (void (*)(void))glReadPixels},
    {"glRotatef", (void (*)(void))glRotatef},
    {"glScalef", (void (*)(void))glScalef},
    {"glScissor", (void (*)(void))glScissor},
    {"glShadeModel", (void (*)(void))glShadeModel},
    {"glShaderBinary", (void (*)(void))glShaderBinary},
    {"glStencilFunc", (void (*)(void))glStencilFunc},
    {"glStencilMask", (void (*)(void))glStencilMask},
    {"glStencilOp", (void (*)(void))glStencilOp},
    {"glTexCoord2f", (void (*)(void))glTexCoord2f},
    {"glTexCoord2fv", (void (*)(void))glTexCoord2fv},
    {"glTexCoord2i", (void (*)(void))glTexCoord2i},
    {"glTexCoordPointer", (void (*)(void))glTexCoordPointer},
    {"glTexEnvf", (void (*)(void))glTexEnvf},
    {"glTexEnvfv", (void (*)(void))glTexEnvfv},
    {"glTexEnvi", (void (*)(void))glTexEnvi},
    {"glTexEnviv", (void (*)(void))glTexEnviv},
    {"glTexImage2D", (void (*)(void))glTexImage2D},
    {"glTexParameterf", (void (*)(void))glTexParameterf},
    {"glTexParameterfv", (void (*)(void))glTexParameterfv},
    {"glTexParameteri", (void (*)(void))glTexParameteri},
    {"glTexParameteriv", (void (*)(void))glTexParameteriv},
    {"glTexSubImage2D", (void (*)(void))glTexSubImage2D},
    {"glTranslatef", (void (*)(void))glTranslatef},
    {"glUniform1f", (void (*)(void))glUniform1f},
    {"glUniform1fv", (void (*)(void))glUniform1fv},
    {"glUniform2f", (void (*)(void))glUniform2f},
    {"glUniform2fv", (void (*)(void))glUniform2fv},
    {"glUniform3f", (void (*)(void))glUniform3f},
    {"glUniform3fv", (void (*)(void))glUniform3fv},
    {"glUniform4f", (void (*)(void))glUniform4f},
    {"glUniform4fv", (void (*)(void))glUniform4fv},
    {"glUniformMatrix4fv", (void (*)(void))glUniformMatrix4fv},
    {"glUnlockArraysEXT", (void (*)(void))glUnlockArraysEXT},
    {"glUseProgram", (void (*)(void))glUseProgram},
    {"glVertex2f", (void (*)(void))glVertex2f},
    {"glVertex2fv", (void (*)(void))glVertex2fv},
    {"glVertex2i", (void (*)(void))glVertex2i},
    {"glVertex3f", (void (*)(void))glVertex3f},
    {"glVertex3fv", (void (*)(void))glVertex3fv},
    {"glVertex3i", (void (*)(void))glVertex3i},
    {"glVertex4f", (void (*)(void))glVertex4f},
    {"glVertexPointer", (void (*)(void))glVertexPointer},
    {"glViewport", (void (*)(void))glViewport},
};

void (*mxglGetProcAddress(const char *name))(void) {
  int lo = 0, hi = (int)(sizeof procs / sizeof procs[0]) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2, c = strcmp(name, procs[mid].name);
    if (!c)
      return procs[mid].fn;
    if (c < 0)
      hi = mid - 1;
    else
      lo = mid + 1;
  }
  return NULL;
}
