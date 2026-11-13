/* gl/batch.c, a part of mxgl.c (included there, not compiled alone): the planes, the batch of
   vertices being gathered (flush_out), facing, the clipper */

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
