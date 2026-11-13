/* gl/drawcalls.c, a part of mxgl.c (included there, not compiled alone): glDrawElements,
   glDrawArrays, the multi-draws, the polygon queue, Quake 2's command lists */

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
