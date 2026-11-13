/* gl/get.c, a part of mxgl.c (included there, not compiled alone): queries, the calls that do
   nothing here, OpenGL ES's float forms */

/* queries and the rest */
GLenum APIENTRY glGetError(void) {
  GLenum e = gl.err;
  gl.err = GL_NO_ERROR;
  return e;
}
void APIENTRY glGetIntegerv(GLenum p, GLint *v) {
  switch (p) {
  case GL_MAX_TEXTURE_SIZE: *v = MXGL_MAX_TEXTURE; break;
  case GL_VIEWPORT: v[0] = gl.vp[0], v[1] = gl.vp[1], v[2] = gl.vp[2], v[3] = gl.vp[3]; break;
  case GL_SCISSOR_BOX: v[0] = gl.sc[0], v[1] = gl.sc[1], v[2] = gl.sc[2], v[3] = gl.sc[3]; break;
  case GL_MATRIX_MODE: *v = (GLint)gl.mode; break;
  case GL_MAX_MODELVIEW_STACK_DEPTH: *v = MV_DEPTH; break;
  case GL_MAX_PROJECTION_STACK_DEPTH: *v = PR_DEPTH; break;
  case GL_MAX_TEXTURE_STACK_DEPTH: *v = TM_DEPTH; break;
  case GL_MODELVIEW_STACK_DEPTH: *v = gl.mv_top + 1; break;
  case GL_PROJECTION_STACK_DEPTH: *v = gl.pr_top + 1; break;
  case GL_TEXTURE_STACK_DEPTH: *v = gl.tm_top + 1; break;
  case GL_MAX_CLIP_PLANES: *v = 1; break;
  case GL_MAX_TEXTURE_UNITS: *v = 2; break;
  case GL_PACK_ALIGNMENT: *v = gl.pack_align; break;
  case GL_UNPACK_ALIGNMENT: *v = 4; break;
  case GL_TEXTURE_BINDING_2D: *v = (GLint)TBOUND; break;
  case GL_ACTIVE_TEXTURE: *v = (GLint)(GL_TEXTURE0 + gl.cunit); break;
  case GL_CLIENT_ACTIVE_TEXTURE: *v = (GLint)(GL_TEXTURE0 + gl.ccunit); break;
  case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: *v = rgb24 ? 8 : 5; break;
  case GL_ALPHA_BITS: *v = 0; break;
  /* the frame buffer's alpha bit: none in 24-bit colour */
  case GL_STENCIL_BITS: *v = st_there(); break;
  case GL_DEPTH_BITS: *v = zmax == ZMAX_Z16 ? 16 : 24; break;
  case GL_MAX_LIST_NESTING: *v = 1; break;
  case GL_MAX_VIEWPORT_DIMS: v[0] = 2048, v[1] = 2048; break;
  default: *v = 0; gl_error(GL_INVALID_ENUM); break;
  }
}
void APIENTRY glGetBooleanv(GLenum p, GLboolean *v) { *v = 0; }
void APIENTRY glGetFloatv(GLenum p, GLfloat *v) {
  switch (p) {
  case GL_MODELVIEW_MATRIX: memcpy(v, gl.mv[gl.mv_top], 64); break;
  case GL_PROJECTION_MATRIX: memcpy(v, gl.pr[gl.pr_top], 64); break;
  default: *v = 0; break;
  }
}
GLboolean APIENTRY glIsEnabled(GLenum cap) {
  switch (cap) {
  case GL_TEXTURE_2D: return (GLboolean)(gl.cunit ? gl.u1.tex2d : gl.tex2d);
  case GL_SHARED_TEXTURE_PALETTE_EXT: return (GLboolean)gl.shared_pal;
  case GL_BLEND: return (GLboolean)gl.blend;
  case GL_ALPHA_TEST: return (GLboolean)gl.atest;
  case GL_FOG: return (GLboolean)gl.fog;
  case GL_DEPTH_TEST: return (GLboolean)gl.dtest;
  case GL_CULL_FACE: return (GLboolean)gl.cull;
  case GL_SCISSOR_TEST: return (GLboolean)gl.scissor;
  case GL_POLYGON_OFFSET_FILL: return (GLboolean)gl.poffset;
  case GL_CLIP_PLANE0: return (GLboolean)gl.ucp_on;
  default: return 0;
  }
}
const GLubyte *APIENTRY glGetString(GLenum name) {
  switch (name) {
  case GL_VENDOR: return (const GLubyte *)"maxsdk";
  case GL_RENDERER: return (const GLubyte *)"PS2 GS + VU1 (HLSL)";
  case GL_VERSION: return (const GLubyte *)"1.1";
  case GL_EXTENSIONS:
    return (const GLubyte *)"GL_ARB_multitexture GL_ARB_texture_env_combine GL_EXT_texture_env_combine "
                            "GL_EXT_compiled_vertex_array GL_EXT_draw_range_elements "
                            "GL_EXT_multi_draw_arrays GL_IBM_multimode_draw_arrays "
                            "GL_EXT_paletted_texture GL_EXT_shared_texture_palette "
                            "GL_OES_compressed_paletted_texture GL_SGIS_generate_mipmap "
                            "GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp GL_EXT_texture_lod_bias "
                            "GL_ARB_vertex_buffer_object GL_PS2_vertex_program_binary";
  default: gl_error(GL_INVALID_ENUM); return (const GLubyte *)"";
  }
}
void APIENTRY glFinish(void) {}
/* state the GS does not have, or that does not change a picture here */
void APIENTRY glShadeModel(GLenum m) {}
void APIENTRY glDrawBuffer(GLenum m) {}
void APIENTRY glReadBuffer(GLenum m) {}
void APIENTRY glPolygonMode(GLenum f, GLenum m) {}
void APIENTRY glLineWidth(GLfloat w) {}
void APIENTRY glPointSize(GLfloat s) {}
void APIENTRY glHint(GLenum t, GLenum m) {}
void APIENTRY glPixelStorei(GLenum p, GLint v) {
  if (p == GL_PACK_ALIGNMENT && (v == 1 || v == 2 || v == 4 || v == 8))
    gl.pack_align = v;
}
void APIENTRY glTexParameteriv(GLenum t, GLenum p, const GLint *v) {
  if (p != GL_TEXTURE_BORDER_COLOR)
    glTexParameteri(t, p, v[0]);
}
void APIENTRY glTexParameterfv(GLenum t, GLenum p, const GLfloat *v) {
  if (p != GL_TEXTURE_BORDER_COLOR)
    glTexParameterf(t, p, v[0]);
}
void APIENTRY glTexEnvfv(GLenum t, GLenum p, const GLfloat *v) {
  if (p != GL_TEXTURE_ENV_COLOR)
    glTexEnvf(t, p, v[0]);
  else if (!gl.cunit)
    for (int i = 0; i < 4; i++)
      env_color[i] = v[i] < 0 ? 0 : v[i] > 1 ? 1 : v[i];
}
void APIENTRY glTexEnviv(GLenum t, GLenum p, const GLint *v) {
  if (p != GL_TEXTURE_ENV_COLOR)
    glTexEnvf(t, p, (GLfloat)v[0]);
  else {
    GLfloat f[4];
    for (int i = 0; i < 4; i++)
      f[i] = (GLfloat)v[i] / 2147483647.0f;
    glTexEnvfv(t, p, f);
  }
}
GLboolean APIENTRY glIsTexture(GLuint t) { return t && t < MAX_TEX && texs[t].used; }
/* OpenGL ES 1.1's float forms */
void APIENTRY glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
  glOrtho(l, r, b, t, n, f);
}
void APIENTRY glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
  glFrustum(l, r, b, t, n, f);
}
void APIENTRY glDepthRangef(GLclampf n, GLclampf f) { glDepthRange(n, f); }
void APIENTRY glClearDepthf(GLclampf d) {}
void APIENTRY glTexCoord2i(GLint s, GLint t) { glTexCoord2f((GLfloat)s, (GLfloat)t); }
void APIENTRY glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
  glVertex3f(x / w, y / w, z / w);
}
void APIENTRY glLoadMatrixd(const GLdouble *m) {
  float f[16];
  for (int i = 0; i < 16; i++)
    f[i] = (float)m[i];
  glLoadMatrixf(f);
}
void APIENTRY glMultMatrixd(const GLdouble *m) {
  float f[16];
  for (int i = 0; i < 16; i++)
    f[i] = (float)m[i];
  glMultMatrixf(f);
}
void APIENTRY glFlush(void) {}
