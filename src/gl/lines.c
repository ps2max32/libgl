/* gl/lines.c, a part of mxgl.c (included there, not compiled alone): lines and points */

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
