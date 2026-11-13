/* gl/misc.c, a part of mxgl.c (included there, not compiled alone): the colour mask, the stencil
   calls, polygon offset, the clip plane, locked arrays */

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
  lk_serial++;
  lk.valid = 0;
  lk.count = first + count;
}
void APIENTRY glUnlockArraysEXT(void) { lk.on = lk.valid = 0; }
