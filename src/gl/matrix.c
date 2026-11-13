/* gl/matrix.c, a part of mxgl.c (included there, not compiled alone): the matrix calls,
   viewport, scissor, depth range */

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
