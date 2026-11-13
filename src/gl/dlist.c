/* gl/dlist.c, a part of mxgl.c (included there, not compiled alone): display lists: glNewList ..
   glCallLists */

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
  u32 ntri, n = dl_strips(idx, nidx, nverts, seq, trs, &ntri);
  /* and the same positions: the arrays may hold other vertices by now (the
     immediate mode's are one buffer) */
  share = share && n == dl_last->n && ntri == dl_last->ntri;
  for (u32 o = 0; share && o < n; o++) {
    const float *x = (const float *)((const u8 *)gl.vptr + (seq[o] & 0x7FFFFFFFu) * gl.vstride);
    const float *q = dl_last->pos[o];
    share = x[0] == q[0] && x[1] == q[1] && (gl.vsize > 2 ? x[2] : 0) == q[2];
  }
  struct dl_entry *e = dl_take(sizeof *e);
  u16 *tri = share ? dl_last->tri : dl_take(ntri_max * 6);
  if (!e || !tri) {
    dl_failed = 1;
    return;
  }
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
   per unit at w = 1, from the projection: lpp), for the entry's texture */
static s32 dl_lod_k(const struct dl_entry *e, float lpp) {
  struct tex *t = e->st.tex2d && e->st.bound && e->st.bound < MAX_TEX ? &texs[e->st.bound] : NULL;
  if (!t || !t->used || t->levels < 2 || e->log2_density < -1e8f)
    return 0;
  /* texels per unit: the texture's size now (a name may get another) */
  float k = (e->log2_density + 0.5f * ((float)t->lw + (float)t->lh + tm_log2_area()) - lpp) * 16.0f;
  s32 ki = (s32)(k + (k < 0 ? -0.5f : 0.5f));
  return ki < -2048 ? -2048 : ki > 2047 ? 2047 : ki;
}

static u32 *idx_room(int n);

/* The matrix of the calls and the planes (plane_d, the user clip plane last)
   as rows: plane k of p = M v is dl_pl[k] . v. Made again when the matrices,
   the viewport, the depth range or the clip plane change (they bump
   mat_serial). dl_lpp: log2 of pixels per unit at w = 1. */
static float dl_m[16], dl_lpp;
static v4f dl_pl[7], dl_pl_abs[7];
static u32 dl_mser;
static void dl_matrix(void) {
  zn = zmax * (1 - gl.dn);
  zf = zmax * (1 - gl.df);
  if (dl_mser == mat_serial)
    return;
  dl_mser = mat_serial;
  /* without the polygon offset: an entry with it is drawn as any draw */
  int po = gl.poffset;
  gl.poffset = 0;
  make_mvp(dl_m);
  gl.poffset = po;
  const float *pr = gl.pr[gl.pr_top];
  float sx = pr[0] * (float)gl.vp[2] * 0.5f * fb_w / gl_w, sy = pr[5] * (float)gl.vp[3] * 0.5f * fb_h / gl_h;
  float a = sx * sy;
  dl_lpp = 0.5f * fast_log2(a < 0 ? -a : a);
  for (int c = 0; c < 4; c++) {
    const float *col = dl_m + 4 * c;
    dl_pl[0][c] = col[0] - GB_LO * col[3];
    dl_pl[1][c] = GB_HI * col[3] - col[0];
    dl_pl[2][c] = col[1] - GB_LO * col[3];
    dl_pl[3][c] = GB_HI * col[3] - col[1];
    dl_pl[4][c] = zn * col[3] - col[2];
    dl_pl[5][c] = col[2] - zf * col[3];
    dl_pl[6][c] = ucp[0] * col[0] + ucp[1] * col[1] + ucp[2] * col[2] + ucp[3] * col[3];
  }
  for (int k = 0; k < 7; k++)
    for (int c = 0; c < 4; c++)
      dl_pl_abs[k][c] = c < 3 ? (dl_pl[k][c] < 0 ? -dl_pl[k][c] : dl_pl[k][c]) : 0;
}

/* The entries drawn one after the other with the same state are a run: one
   set of registers and one VU1 context (uniforms, GIF tag) for all, then a
   CALL of each entry's chain (each chain's first batch takes the context
   again). K of TEX1 is the run's first entry's while the next ones' is
   within DL_K_TOL (an eighth of a level). */
#define DL_RUN 128
#define DL_K_TOL 2
static struct {
  const struct mx_chain *ch[DL_RUN];
  u32 n, prim;
  int cl; /* a channel a pass (mxgl_color_light) */
  /* the run's state, for the next entry's: valid while the registers are as
     it left them */
  struct dl_state st;
  s32 k;
  u32 serial, gen;
  int ok;
} dl_run;
static float dl_uni[MXGL_UNI_QW][4] MX_ALIGNED(16) = {
    [VU_MXGLV_CMUL] = {1, 1, 1, 1}}; /* the colour as it is */

static void dl_run_send(void) {
  if (!dl_run.n)
    return;
  u32 c0 = mx_count();
  flush_out(); /* a batch of entries drawn as any draw comes first */
  /* libgl's own program: a list's chain calls it (the entry is in its
     batches); the colours are the recorded ones */
  const struct mx_shader *sh = vprog_use(NULL);
  for (int pass = 0; pass < (dl_run.cl ? 3 : 1); pass++) {
    if (pass)
      cl_regs(pass);
    mx_frame_shader_call(sh, dl_run.prim, dl_uni, dl_run.ch[0]);
    for (u32 i = 1; i < dl_run.n; i++)
      mx_frame_call(dl_run.ch[i]);
  }
  if (dl_run.cl)
    cl_regs(0);
  gs_drew = 1;
  st_cur.draws++;
  st_cur.c_call += mx_count() - c0;
  dl_run.n = 0;
}

/* an entry of a list being called, with the matrices of now */
static void dl_draw_entry(const struct dl_entry *e, int nplanes) {
  u32 c0 = mx_count();
  /* the bounds against each plane: the centre's distance, and how far
     the box reaches along the plane's normal */
  u32 oand = 0, oor = 0;
  for (int k = 0; k < nplanes; k++) {
    v4f dc = dl_pl[k] * e->center, rr = dl_pl_abs[k] * e->half;
    float d = dc[0] + dc[1] + dc[2] + dc[3], r = rr[0] + rr[1] + rr[2];
    if (d + r < 0)
      oand = 1;
    if (d - r < 0)
      oor = 1;
  }
  if (oand)
    return;
  if (oor || e->st.poffset) {
    /* across a plane: as any draw */
    dl_run_send();
    dl_run.ok = 0;
    dl_set_state(&e->st);
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
    return;
  }
  if (e->planar && e->st.cull && e->st.cullface != GL_FRONT_AND_BACK && !vu1_clips()) {
    /* one plane, one facing: the first triangle's on the screen (culled()) */
    v4f mc[4];
    memcpy(mc, dl_m, 64);
    v4f a = xform(mc, e->pos[0][0], e->pos[0][1], e->pos[0][2]);
    v4f b = xform(mc, e->pos[1][0], e->pos[1][1], e->pos[1][2]);
    v4f c = xform(mc, e->pos[2][0], e->pos[2][1], e->pos[2][2]);
    float det = a[0] * (b[1] * c[3] - b[3] * c[1]) - a[1] * (b[0] * c[3] - b[3] * c[0]) +
                a[3] * (b[0] * c[1] - b[1] * c[0]);
    int front = det < 0;
    if (e->st.cullface == GL_FRONT ? front : !front)
      return;
  } else if (e->st.cull && e->st.cullface == GL_FRONT_AND_BACK) {
    return;
  }
  u32 c1 = mx_count();
  st_cur.c_bounds += c1 - c0;
  s32 k = dl_lod_k(e, dl_lpp);
  /* the same state as the run's, nothing written since: the registers are
     right, and the entry joins it */
  s32 dk = k - dl_run.k;
  if (!(dl_run.ok && dl_run.n < DL_RUN && gs_serial == dl_run.serial && vram_gen == dl_run.gen &&
        dk >= -DL_K_TOL && dk <= DL_K_TOL && !memcmp(&dl_run.st, &e->st, sizeof dl_run.st))) {
    dl_run_send();
    dl_run.ok = 0;
    dl_set_state(&e->st);
    if (set_state(&dl_run.prim, dl_uni[VU_MXGLV_CSCALE], k) < 0)
      return;
    dl_run.prim &= ~(1u << MX_GS_PRIM_FGE_SHIFT); /* libgl's own program: no F */
    dl_run.cl = cl_batch;
    dl_run.st = e->st, dl_run.k = k, dl_run.gen = vram_gen, dl_run.serial = gs_serial;
    dl_run.ok = 1;
    memcpy(dl_uni[VU_MXGLV_MVP], dl_m, 64);
    tm_uniforms(dl_uni[VU_MXGLV_TMS], dl_uni[VU_MXGLV_TMT]);
  }
  /* its bounds' nearest point: the finest level its texture is sampled at
     (the texture is the run's: gl.bound) */
  if (tex_bias_auto) {
    const float *m = dl_m;
    float wc = m[3] * e->center[0] + m[7] * e->center[1] + m[11] * e->center[2] + m[15];
    float wr = fabsf(m[3]) * e->half[0] + fabsf(m[7]) * e->half[1] + fabsf(m[11]) * e->half[2];
    tex_need_note(k, wc - wr);
  }
  dl_run.ch[dl_run.n++] = &e->ch;
  st_cur.c_state += mx_count() - c1;
  st_cur.lists++;
  st_cur.tris += e->ntri;
  st_cur.verts += e->n;
}

/* A list of passes over one surface: the first draws it with depth writes
   and no blending, the others go over its pixels (the same vertices, depth
   EQUAL or LEQUAL, no depth writes). Such lists called together may be
   drawn pass by pass, all first passes before all second ones: every pixel
   ends up as the nearest surface's passes in order, as when each list is
   drawn whole. */
static int dl_is_passes(const struct dl_entry *e) {
  if (!e || !e->st.dtest || !e->st.dmask || e->st.blend || e->st.poffset)
    return 0;
  for (const struct dl_entry *p = e->next; p; p = p->next)
    if (p->pos != e->pos || p->st.dmask || !p->st.dtest ||
        (p->st.dfunc != GL_EQUAL && p->st.dfunc != GL_LEQUAL) || p->st.poffset)
      return 0;
  return 1;
}

/* glCallList, glCallLists: the lists ids[0 .. n - 1] */
#define DL_CALL_MAX 256
static void dl_call(const GLuint *ids, int n) {
  pq_flush();
  if (dl_compiling) {
    gl_error(GL_INVALID_OPERATION); /* lists in lists: not recorded */
    return;
  }
  u32 t0 = mx_count();
  dl_called_frame = mx_gs.frames;
  frame_start();
  flush_out();
  dl_matrix();
  const int nplanes = gl.ucp_on ? 7 : 6;
  struct dl_state keep;
  dl_get_state(&keep);
  const struct dl_state *end = &keep;
  dl_run.ok = 0;
  while (n > 0) {
    const struct dl_entry *cur[DL_CALL_MAX];
    int m = 0, passes = 1;
    for (; m < DL_CALL_MAX && m < n; m++) {
      GLuint id = ids[m];
      cur[m] = id && id < DL_MAX && dlists[id].used ? dlists[id].first : NULL;
      if (cur[m]) {
        passes = passes && dl_is_passes(cur[m]);
        /* the list's state calls had their effect */
        if (dlists[id].end)
          end = dlists[id].end;
      }
    }
    if (passes && m > 1) {
      for (int left = 1; left;) {
        left = 0;
        for (int i = 0; i < m; i++)
          if (cur[i]) {
            dl_draw_entry(cur[i], nplanes);
            cur[i] = cur[i]->next;
            left |= cur[i] != NULL;
          }
      }
    } else {
      for (int i = 0; i < m; i++)
        for (const struct dl_entry *e = cur[i]; e; e = e->next)
          dl_draw_entry(e, nplanes);
    }
    ids += m, n -= m;
  }
  dl_run_send();
  dl_run.ok = 0;
  dl_set_state(end);
  st_cur.gl_cycles += mx_count() - t0;
}

void APIENTRY glCallList(GLuint id) { dl_call(&id, 1); }

static GLuint dl_base;
void APIENTRY glListBase(GLuint base) { dl_base = base; }
/* The lists are drawn in order; lists that are passes over a surface each
   (dl_is_passes) pass by pass, so the lists of surfaces with one material
   take one set of registers a pass instead of one a list. */
void APIENTRY glCallLists(GLsizei n, GLenum type, const GLvoid *lists) {
  if (n < 0) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  GLuint ids[DL_CALL_MAX];
  for (GLsizei at = 0; at < n;) {
    int k = 0;
    for (; k < DL_CALL_MAX && at < n; k++, at++) {
      GLuint v;
      switch (type) {
      case GL_UNSIGNED_BYTE: v = ((const GLubyte *)lists)[at]; break;
      case GL_BYTE: v = (GLuint)((const GLbyte *)lists)[at]; break;
      case GL_UNSIGNED_SHORT: v = ((const GLushort *)lists)[at]; break;
      case GL_SHORT: v = (GLuint)((const GLshort *)lists)[at]; break;
      case GL_UNSIGNED_INT: case GL_INT: v = ((const GLuint *)lists)[at]; break;
      default: gl_error(GL_INVALID_ENUM); return;
      }
      ids[k] = dl_base + v;
    }
    dl_call(ids, k);
  }
}
void mxgl_CallList(int id) { glCallList((GLuint)id); }
