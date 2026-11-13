/* gl/immediate.c, a part of mxgl.c (included there, not compiled alone): immediate mode: glBegin
   .. glEnd, glArrayElement */

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
static struct imm_v imm_small[IMM_SMALL] MX_ALIGNED(64) MX_CORE_BSS; /* VU0 reads it */

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
