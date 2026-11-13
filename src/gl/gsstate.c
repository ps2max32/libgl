/* gl/gsstate.c, a part of mxgl.c (included there, not compiled alone): the GL state as GS
   registers: blending, the stencil bit, set_state; the draws' matrix (make_mvp) */

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
  clut_pick = t && t->grey ? (t->grey == 2 ? CLUT_GREYK : CLUT_GREY) : CLUT_1X;
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
