/* gl/vprog.c, a part of mxgl.c (included there, not compiled alone): vertex programs: GL 2.0's
   shader and program calls for VU1 binaries, the fog programs */

/* ---- vertex programs (GL 2.0 shaders and programs, GL_PS2_VU1_BINARY) ----- */

/* VU1's micro memory (64-bit units): libgl's own program from 0, the
   programs' code after it, each where it stays (a switch is a call: no
   upload, and no FLUSH between programs of the same layout); when it is
   full, from after libgl's own again (maxsdk uploads what is not
   resident) */
#define VU1_CODE 2048
#define MAX_GLSL 64
struct vprog {
  struct mx_shader s; /* its entry where the code was placed */
  u32 at;             /* the code's address */
  const struct mxgl_vu1_binary *bin;
  int r_mvp, r_cscale, r_tms, r_tmt; /* libgl's uniforms: quadwords, -1 none */
  int r_clip;                        /* mxglc.i's clipping uniforms */
  float (*uni)[4];
};
static struct {
  u8 kind; /* 0 free, 1 shader, 2 program */
  u8 linked;
  const struct mxgl_vu1_binary *bin; /* a shader's; a program's attached */
  struct vprog *p;
  const char *log;
} glsl[MAX_GLSL];
static u32 vu1_next;

/* room in VU1's micro memory for n double words, after libgl's own program */
static u32 vu1_place(u32 n) {
  u32 own = (shader.code_bytes + 7) / 8;
  if (vu1_next < own || vu1_next + n > VU1_CODE)
    vu1_next = own;
  u32 at = vu1_next;
  vu1_next += n;
  return at;
}

/* a fog program: placed once, drawn with o_uni (no uni of its own) */
static void fog_program(int i, const struct mx_shader *s) {
  static struct vprog progs[2];
  struct vprog *p = fog_prog[i] = &progs[i];
  p->at = vu1_place((s->code_bytes + 7) / 8);
  p->s = *s;
  p->s.entry = s->entry + p->at;
}

/* the stream's program becomes p's (NULL: libgl's own); its shader */
static const struct mx_shader *vprog_use(struct vprog *p) {
  const struct mx_shader *s = p ? &p->s : &shader;
  if (s == vu_cur)
    return s;
  const struct mx_shader *o = vu_cur;
  if (!o || s->buf_qw != o->buf_qw || s->ctx_at != o->ctx_at || s->ctx_qw != o->ctx_qw ||
      s->nctx != o->nctx || s->in_at != o->in_at || s->out_at != o->out_at) {
    /* another layout: its inputs or contexts may be where the GIF still
       reads the last program's packets (as mx_frame_shader_setup) */
    u64 *c = mx_frame_reserve(2);
    c = mx_e_cnt(c, 1, mx_vif_flush(), mx_vif_base(0));
    c = mx_e_vif4(c, mx_vif_offset(s->buf_qw), mx_vif_stcycl(1, 1), 0, 0);
    mx_frame_commit(c);
  }
  mx_frame_mpg(s->code, s->code_bytes, p ? p->at : 0);
  vu_cur = s;
  return s;
}

/* p's uniforms with libgl's (mvp, cscale, tms, tmt) from those of its own
   program; fixed itself for libgl's */
static const void *vprog_uni(struct vprog *p, const float (*fixed)[4]) {
  if (!p || !p->uni) /* libgl's own, the fog programs: its layout */
    return fixed;
  if (p->r_mvp >= 0)
    memcpy(p->uni[p->r_mvp], fixed[VU_MXGLV_MVP], 64);
  if (p->r_cscale >= 0)
    memcpy(p->uni[p->r_cscale], fixed[VU_MXGLV_CSCALE], 16);
#if MXGL_VU1CLIP
  if (p->r_clip >= 0)
    memcpy(p->uni[p->r_clip], fixed[VU_MXGLV_UNI_QW], (MXGL_UNI_QW - VU_MXGLV_UNI_QW) * 16);
#endif
  if (p->r_tms >= 0)
    memcpy(p->uni[p->r_tms], fixed[VU_MXGLV_TMS], 16);
  if (p->r_tmt >= 0)
    memcpy(p->uni[p->r_tmt], fixed[VU_MXGLV_TMT], 16);
  return p->uni;
}

static int glsl_new(int kind) {
  for (int i = 1; i < MAX_GLSL; i++)
    if (!glsl[i].kind) {
      glsl[i].kind = (u8)kind, glsl[i].linked = 0, glsl[i].bin = NULL, glsl[i].p = NULL;
      glsl[i].log = "";
      return i;
    }
  gl_error(GL_OUT_OF_MEMORY);
  return 0;
}
static int glsl_is(GLuint id, int kind) { return id && id < MAX_GLSL && glsl[id].kind == kind; }

GLuint APIENTRY glCreateShader(GLenum type) {
  if (type != GL_VERTEX_SHADER) {
    gl_error(GL_INVALID_ENUM);
    return 0;
  }
  return (GLuint)glsl_new(1);
}
void APIENTRY glDeleteShader(GLuint sh) {
  if (glsl_is(sh, 1))
    glsl[sh].kind = 0;
}
/* binary: a struct mxgl_vu1_binary (<mxgl.h>) */
void APIENTRY glShaderBinary(GLsizei count, const GLuint *shaders, GLenum format, const void *binary,
                             GLsizei length) {
  const struct mxgl_vu1_binary *b = binary;
  if (format != GL_PS2_VU1_BINARY || !b || length < (GLsizei)sizeof *b || b->magic != MXGL_VU1_MAGIC) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  for (int i = 0; i < count; i++)
    if (glsl_is(shaders[i], 1))
      glsl[shaders[i]].bin = b;
    else
      gl_error(GL_INVALID_VALUE);
}
void APIENTRY glGetShaderiv(GLuint sh, GLenum pname, GLint *v) {
  if (!glsl_is(sh, 1)) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  *v = pname == GL_COMPILE_STATUS ? glsl[sh].bin != NULL : pname == GL_SHADER_TYPE ? GL_VERTEX_SHADER : 0;
}
GLuint APIENTRY glCreateProgram(void) { return (GLuint)glsl_new(2); }
void APIENTRY glAttachShader(GLuint prog, GLuint sh) {
  if (!glsl_is(prog, 2) || !glsl_is(sh, 1)) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  glsl[prog].bin = glsl[sh].bin;
}
void APIENTRY glDetachShader(GLuint prog, GLuint sh) {
  if (glsl_is(prog, 2))
    glsl[prog].bin = NULL;
}
void APIENTRY glDeleteProgram(GLuint prog) {
  if (!glsl_is(prog, 2))
    return;
  struct vprog *p = glsl[prog].p;
  if (p && (p == cur_prog || p == o_prog)) {
    /* still in use (the batch's, or glUseProgram's): its memory is kept
       (a few hundred bytes) */
    glsl[prog].kind = 0;
    return;
  }
  if (p) {
    gl_free(p->uni);
    free(p);
  }
  glsl[prog].kind = 0;
}

/* the uniform name (lower case in the binary) of a cbuffer member */
static int uni_find(const struct mxgl_vu1_binary *b, const char *name) {
  for (u32 i = 0; i < b->nuni; i++) {
    const char *a = b->uni[i].name, *n = name;
    while (*a && *n && (*n | 0x20) == *a)
      a++, n++;
    if (!*a && !*n)
      return (int)i;
  }
  return -1;
}

/* the binary's program placed in VU1's micro memory; its inputs must be
   libgl's own (the batches and the display lists' chains are made for
   them) */
void APIENTRY glLinkProgram(GLuint prog) {
  if (!glsl_is(prog, 2)) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  glsl[prog].linked = 0;
  const struct mxgl_vu1_binary *b = glsl[prog].bin;
  if (!b) {
    glsl[prog].log = "no shader binary attached";
    return;
  }
  struct mx_shader s;
  b->shader(&s);
  if (s.nin != shader.nin || memcmp(s.in, shader.in, sizeof s.in[0] * s.nin) ||
      s.in_at != shader.in_at || s.align != shader.align || s.max_i) {
    glsl[prog].log = "inputs not float4 POSITION, float2 TEXCOORD, uint4 COLOR_V4_8U (mxglv.hlsl)";
    return;
  }
  u32 own = (shader.code_bytes + 7) / 8, n = (s.code_bytes + 7) / 8;
  if (own + n > VU1_CODE) {
    glsl[prog].log = "code larger than VU1's micro memory";
    return;
  }
  struct vprog *p = glsl[prog].p;
  if (!p) {
    p = calloc(1, sizeof *p);
    if (!p) {
      gl_error(GL_OUT_OF_MEMORY);
      return;
    }
  } else {
    gl_free(p->uni);
    p->uni = NULL;
  }
  p->at = vu1_place(n);
  p->s = s;
  p->s.entry = s.entry + p->at;
  p->bin = b;
  p->uni = gl_alloc(s.uni_qw * 16 + 16, 16);
  if (!p->uni) {
    free(p);
    glsl[prog].p = NULL;
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  memset(p->uni, 0, s.uni_qw * 16);
  int k;
  p->r_mvp = (k = uni_find(b, "mvp")) >= 0 ? b->uni[k].at : -1;
  p->r_cscale = (k = uni_find(b, "cscale")) >= 0 ? b->uni[k].at : -1;
  p->r_clip = (k = uni_find(b, "clip")) >= 0 ? b->uni[k].at : -1;
  p->r_tms = (k = uni_find(b, "tms")) >= 0 ? b->uni[k].at : -1;
  p->r_tmt = (k = uni_find(b, "tmt")) >= 0 ? b->uni[k].at : -1;
  glsl[prog].p = p;
  glsl[prog].linked = 1;
  glsl[prog].log = "";
}
void APIENTRY glGetProgramiv(GLuint prog, GLenum pname, GLint *v) {
  if (!glsl_is(prog, 2)) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  switch (pname) {
  case GL_LINK_STATUS: *v = glsl[prog].linked; break;
  case GL_INFO_LOG_LENGTH: *v = (GLint)strlen(glsl[prog].log) + 1; break;
  case GL_ATTACHED_SHADERS: *v = glsl[prog].bin != NULL; break;
  case GL_ACTIVE_UNIFORMS: *v = glsl[prog].bin ? (GLint)glsl[prog].bin->nuni : 0; break;
  case GL_ACTIVE_ATTRIBUTES: *v = glsl[prog].bin ? (GLint)glsl[prog].bin->nin : 0; break;
  default: gl_error(GL_INVALID_ENUM);
  }
}
void APIENTRY glGetProgramInfoLog(GLuint prog, GLsizei size, GLsizei *len, GLchar *log) {
  const char *m = glsl_is(prog, 2) ? glsl[prog].log : "";
  GLsizei n = (GLsizei)strlen(m);
  if (size > 0) {
    n = n < size - 1 ? n : size - 1;
    memcpy(log, m, (size_t)n);
    log[n] = 0;
  }
  if (len)
    *len = n;
}
void APIENTRY glUseProgram(GLuint prog) {
  if (!prog) {
    cur_prog = NULL, cur_prog_clips = 0;
    return;
  }
  if (!glsl_is(prog, 2) || !glsl[prog].linked) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  cur_prog = glsl[prog].p;
  cur_prog_clips = cur_prog->r_clip >= 0;
}
/* a float's index in the uniforms (the member's first), or -1 */
GLint APIENTRY glGetUniformLocation(GLuint prog, const GLchar *name) {
  if (!glsl_is(prog, 2) || !glsl[prog].linked)
    return -1;
  int k = uni_find(glsl[prog].bin, name);
  return k < 0 ? -1 : (GLint)glsl[prog].bin->uni[k].at * 4;
}
/* the input's index (by its semantic: POSITION, TEXCOORD, COLOR), or -1 */
GLint APIENTRY glGetAttribLocation(GLuint prog, const GLchar *name) {
  if (!glsl_is(prog, 2) || !glsl[prog].bin)
    return -1;
  const struct mxgl_vu1_binary *b = glsl[prog].bin;
  for (u32 i = 0; i < b->nin; i++)
    if (!strcmp(b->in[i], name))
      return (GLint)i;
  return -1;
}

/* n floats of the program in use from location loc (the batch drawn with
   the old values first) */
static float *uni_at(GLint loc, int n) {
  struct vprog *p = cur_prog;
  if (!p) {
    gl_error(GL_INVALID_OPERATION);
    return NULL;
  }
  if (loc < 0 || (u32)loc + (u32)n > p->s.uni_qw * 4) {
    if (loc != -1)
      gl_error(GL_INVALID_OPERATION);
    return NULL;
  }
  if (o_n && o_prog == p)
    flush_out();
  return &p->uni[0][0] + loc;
}
void APIENTRY glUniform1f(GLint loc, GLfloat x) {
  float *u = uni_at(loc, 1);
  if (u)
    u[0] = x;
}
void APIENTRY glUniform2f(GLint loc, GLfloat x, GLfloat y) {
  float *u = uni_at(loc, 2);
  if (u)
    u[0] = x, u[1] = y;
}
void APIENTRY glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z) {
  float *u = uni_at(loc, 3);
  if (u)
    u[0] = x, u[1] = y, u[2] = z;
}
void APIENTRY glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
  float *u = uni_at(loc, 4);
  if (u)
    u[0] = x, u[1] = y, u[2] = z, u[3] = w;
}
/* arrays: one element a quadword (a cbuffer's float4 array) */
static void uni_v(GLint loc, GLsizei count, const GLfloat *v, int k) {
  float *u = uni_at(loc, count > 0 ? (count - 1) * 4 + k : 0);
  if (u)
    for (int i = 0; i < count; i++)
      memcpy(u + 4 * i, v + k * i, (size_t)k * 4);
}
void APIENTRY glUniform1fv(GLint loc, GLsizei count, const GLfloat *v) { uni_v(loc, count, v, 1); }
void APIENTRY glUniform2fv(GLint loc, GLsizei count, const GLfloat *v) { uni_v(loc, count, v, 2); }
void APIENTRY glUniform3fv(GLint loc, GLsizei count, const GLfloat *v) { uni_v(loc, count, v, 3); }
void APIENTRY glUniform4fv(GLint loc, GLsizei count, const GLfloat *v) { uni_v(loc, count, v, 4); }
/* column-major as GL's matrices (libgl's mvp is too) */
void APIENTRY glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v) {
  float *u = uni_at(loc, count * 16);
  if (!u)
    return;
  for (int i = 0; i < count; i++, u += 16, v += 16)
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++)
        u[4 * c + r] = transpose ? v[4 * r + c] : v[4 * c + r];
}
