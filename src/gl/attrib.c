/* gl/attrib.c, a part of mxgl.c (included there, not compiled alone): glPushAttrib, glPopAttrib;
   texture unit 1's calls */

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
