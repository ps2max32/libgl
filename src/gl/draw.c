/* gl/draw.c, a part of mxgl.c (included there, not compiled alone): a draw: level of detail, the
   transform on VU0, the passes of unit 1 and the texture environments, draw_tris_ */

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
      "pref 0, 128(%[src])\n\t"
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
    /* w as o_put_pos has it: a vertex that closes a triangle in the reverse
       order (adc 2) closes one */
    const float xy0[3] = {x[0], x[1], 0};
    o_put_pos(o_n, xy0, MXGL_W(adc));
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
  if (t && t->alpha && (!t->bright || t->grey == 2)) /* Af At: the texture's alpha, white */
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
