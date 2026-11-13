/* gl/statecalls.c, a part of mxgl.c (included there, not compiled alone): blending, fog, the
   tests, culling, the texture environment, glClear */

void APIENTRY glBlendFunc(GLenum s, GLenum d) { pq_flush(), gl.sf = s, gl.dfac = d, st_serial++; }
/* the fog's a and b (mxglvf.hlsl, mxglve.hlsl) */
static void fog_update(void) {
  if (gl.fog_mode == GL_LINEAR) {
    float d = gl.fog_end - gl.fog_start;
    gl.fog_a = d != 0 ? -255.0f / d : 0;
    gl.fog_b = d != 0 ? 255.0f * gl.fog_end / d : 255.0f;
  } else {
    gl.fog_a = gl.fog_density;
    gl.fog_b = gl.fog_mode == GL_EXP2;
  }
}
static u32 fog_byte(float c) { return (u32)((c < 0 ? 0 : c > 1 ? 1 : c) * 255.0f + 0.5f); }
void APIENTRY glFogfv(GLenum p, const GLfloat *v) {
  pq_flush();
  st_serial++;
  switch (p) {
  case GL_FOG_MODE:
    if (v[0] != GL_LINEAR && v[0] != GL_EXP && v[0] != GL_EXP2) {
      gl_error(GL_INVALID_ENUM);
      return;
    }
    gl.fog_mode = (GLenum)v[0];
    break;
  case GL_FOG_DENSITY: gl.fog_density = v[0]; break;
  case GL_FOG_START: gl.fog_start = v[0]; break;
  case GL_FOG_END: gl.fog_end = v[0]; break;
  case GL_FOG_COLOR:
    gl.fog_col = fog_byte(v[0]) | fog_byte(v[1]) << 8 | fog_byte(v[2]) << 16;
    break;
  case GL_FOG_INDEX: break;
  default: gl_error(GL_INVALID_ENUM); return;
  }
  fog_update();
}
void APIENTRY glFogf(GLenum p, GLfloat v) { glFogfv(p, &v); }
void APIENTRY glFogi(GLenum p, GLint v) {
  GLfloat f = (GLfloat)v;
  glFogfv(p, &f);
}
void APIENTRY glFogiv(GLenum p, const GLint *v) {
  GLfloat f[4] = {(GLfloat)v[0], 0, 0, 0};
  if (p == GL_FOG_COLOR)
    for (int i = 0; i < 3; i++) /* 0 .. INT_MAX is 0 .. 1 */
      f[i] = (GLfloat)v[i] * (1.0f / 2147483647.0f);
  glFogfv(p, f);
}
void APIENTRY glAlphaFunc(GLenum f, GLclampf r) { pq_flush(), gl.afunc = f, gl.aref = r, st_serial++; }
void APIENTRY glDepthFunc(GLenum f) { pq_flush(), gl.dfunc = f, st_serial++; }
void APIENTRY glDepthMask(GLboolean m) { pq_flush(), gl.dmask = m, st_serial++; }
/* gl.cullface is the face culled in counter-clockwise terms (culled()):
   glCullFace's, the other one under glFrontFace(GL_CW) */
static GLenum cull_mode = GL_BACK;
static int front_cw;
static void cull_set(void) {
  pq_flush();
  gl.cullface = !front_cw || cull_mode == GL_FRONT_AND_BACK ? cull_mode
                : cull_mode == GL_FRONT                    ? GL_BACK
                                                           : GL_FRONT;
  st_serial++;
}
void APIENTRY glCullFace(GLenum m) { cull_mode = m, cull_set(); }
void APIENTRY glFrontFace(GLenum m) { front_cw = m == GL_CW, cull_set(); }
/* each unit's GL_COMBINE, COMBINE_RGB, RGB_SCALE (0: 1, 1: 2), and unit 0's
   mxgl_ColorShift */
static int env_combine[2], env_scale[2], env_legacy;
static GLenum env_combine_rgb[2] = {GL_MODULATE, GL_MODULATE};
/* the colour shift of each unit's draws: RGB_SCALE in COMBINE mode (GL
   takes it only there), else mxgl_ColorShift's */
static void env_shifts(void) {
  gl.cshift = env_combine[0] ? env_scale[0] : env_legacy;
  gl.u1.cshift = env_combine[1] ? env_scale[1] : 0;
}

/* The texture environment of the active unit: MODULATE, REPLACE, DECAL,
   and COMBINE as one of them (COMBINE_RGB MODULATE or REPLACE; the GS has
   no others); RGB_SCALE 1 or 2 (4 is 2): the GS's modulate takes the
   colour up to 2x (0x80 is 1.0). */
void APIENTRY glTexEnvf(GLenum t, GLenum p, GLfloat v) {
  pq_flush();
  if (t == GL_TEXTURE_FILTER_CONTROL) {
    /* GL_EXT_texture_lod_bias: levels added to every draw's (its K is made
       for each draw: no state serial) */
    if (p == GL_TEXTURE_LOD_BIAS)
      gl.lod_bias = v;
    else
      gl_error(GL_INVALID_ENUM);
    return;
  }
  st_serial++;
  GLenum *env = gl.cunit ? &gl.u1.texenv : &gl.texenv;
  int u = gl.cunit;
  switch (p) {
  case GL_TEXTURE_ENV_MODE:
    env_combine[u] = (GLenum)v == GL_COMBINE;
    *env = env_combine[u] ? env_combine_rgb[u] : (GLenum)v;
    break;
  case GL_COMBINE_RGB:
    env_combine_rgb[u] = (GLenum)v == GL_REPLACE ? GL_REPLACE : GL_MODULATE;
    if (env_combine[u])
      *env = env_combine_rgb[u];
    break;
  case GL_RGB_SCALE: env_scale[u] = v > 1.5f; break;
  case GL_COMBINE_ALPHA: case GL_ALPHA_SCALE: case GL_SOURCE0_RGB: case GL_SOURCE1_RGB:
  case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_TEXTURE_ENV_COLOR: break;
  default: gl_error(GL_INVALID_ENUM); break;
  }
  env_shifts();
}
void APIENTRY glTexEnvi(GLenum t, GLenum p, GLint v) { glTexEnvf(t, p, (GLfloat)v); }
void APIENTRY glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
  gl.clear[0] = r, gl.clear[1] = g, gl.clear[2] = b, gl.clear[3] = a;
}

static void clear_cz(GLbitfield mask);
/* the alpha bit to 0 where a sprite covers: all of the buffer but for the scissor */
static void clear_stencil(void) {
  int x0 = 0, x1 = fb_w, y0 = 0, y1 = fb_h;
  if (gl.scissor) {
    x0 = gl.sc[0] * fb_w / gl_w, x1 = (gl.sc[0] + gl.sc[2]) * fb_w / gl_w;
    y0 = (gl_h - (gl.sc[1] + gl.sc[3])) * fb_h / gl_h, y1 = (gl_h - gl.sc[1]) * fb_h / gl_h;
  }
  /* beam: context 2, without Z writes (ZBUF and SCISSOR are the passes') */
  int ctx = beam != 0;
  u32 n = beam ? 7 : 9;
  u64 *c = mx_frame_reserve(2 + n);
  c = mx_e_cnt(c, n + 1, mx_vif_flush(), mx_vif_direct(n + 1));
  c = mx_e_ad_tag(c, n, 1);
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_TEST_1, ctx), mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_FRAME_1, ctx), frame_reg(0x00FFFFFFu));
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_FBA_1, ctx), 0);
  if (!beam) {
    c = mx_e_ad(c, MX_GS_REG_ZBUF_1, mx_gs_zbuf(rt[buf].zbp, rt[buf].zpsm, 1));
    c = mx_e_ad(c, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, fb_w - 1, 0, fb_h - 1));
  }
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, ctx, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0, 0, 0, 0, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x0), mx_gs_py((u32)y0), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x1), mx_gs_py((u32)y1), 0));
  mx_frame_commit(c);
  gs.valid = 0, gs_serial++;
}
void APIENTRY glClear(GLbitfield mask) {
  clear_cz(mask);
  if ((mask & GL_STENCIL_BUFFER_BIT) && st_there()) {
    frame_start();
    flush_out();
    clear_stencil();
    gl.st_va = (int)(gl.st_clear & 255), gl.st_vb = -1;
    st_serial++;
  }
}
static void clear_cz(GLbitfield mask) {
  pq_flush();
  int was = in_frame;
  frame_start();
  flush_out();
  if (!was && (mask & GL_COLOR_BUFFER_BIT))
    return; /* mx_frame_begin cleared colour and Z */
  if (!(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)))
    return;
  if (beam) {
  /* a sprite over the scissor area (or all) in the passes' window: Z 0
     with context 1, without (ZMSK) with context 2; colour, or the
     destination's colour again (blend Cd) when only Z is cleared */
  int x0 = 0, x1 = fb_w, y0 = 0, y1 = fb_h;
  if (gl.scissor) {
    x0 = gl.sc[0] * fb_w / gl_w, x1 = (gl.sc[0] + gl.sc[2]) * fb_w / gl_w;
    y0 = (gl_h - (gl.sc[1] + gl.sc[3])) * fb_h / gl_h, y1 = (gl_h - gl.sc[1]) * fb_h / gl_h;
  }
  int z = (mask & GL_DEPTH_BUFFER_BIT) != 0, col = (mask & GL_COLOR_BUFFER_BIT) != 0;
  u64 *c = mx_frame_reserve(2 + 9);
  c = mx_e_cnt(c, 9, mx_vif_flush(), mx_vif_direct(9));
  c = mx_e_ad_tag(c, 8, 1);
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_FRAME_1, !z),
              frame_reg(col ? (gl.cmask & 0x00FFFFFFu) | 0x80000000u : 0x80000000u));
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_TEST_1, !z), mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_ALPHA_1, !z), mx_gs_alpha(1, 1, 2, 1, 0));
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, !col, 0, 0, !z, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ,
              mx_gs_rgbaq((u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255),
                          (u32)(gl.clear[2] * 255), 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x0), mx_gs_py((u32)y0), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x1), mx_gs_py((u32)y1), 0));
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  mx_frame_commit(c);
  } else {
  /* a sprite over the scissor area (or all): Z 0, colour if asked */
  const struct mx_rt *t = &rt[buf];
  int x0 = 0, x1 = fb_w, y0 = 0, y1 = fb_h;
  if (gl.scissor) {
    x0 = gl.sc[0] * fb_w / gl_w, x1 = (gl.sc[0] + gl.sc[2]) * fb_w / gl_w;
    y0 = (gl_h - (gl.sc[1] + gl.sc[3])) * fb_h / gl_h, y1 = (gl_h - gl.sc[1]) * fb_h / gl_h;
  }
  u64 *c = mx_frame_reserve(2 + 9);
  c = mx_e_cnt(c, 9, mx_vif_flush(), mx_vif_direct(9));
  c = mx_e_ad_tag(c, 8, 1);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  /* the write masks hold for glClear too (glColorMask, glDepthMask) */
  c = mx_e_ad(c, MX_GS_REG_ZBUF_1,
              mx_gs_zbuf(t->zbp, t->zpsm, !(mask & GL_DEPTH_BUFFER_BIT) || !gl.dmask));
  c = mx_e_ad(c, MX_GS_REG_FRAME_1,
              mx_gs_frame(t->fbp, t->fbw, t->psm,
                          (mask & GL_COLOR_BUFFER_BIT) ? gl.cmask | 0x80000000u : ~0u));
  c = mx_e_ad(c, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, fb_w - 1, 0, fb_h - 1));
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ,
              mx_gs_rgbaq((u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255),
                          (u32)(gl.clear[2] * 255), 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x0), mx_gs_py((u32)y0), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x1), mx_gs_py((u32)y1), 0));
  mx_frame_commit(c);
  c = mx_frame_direct(2);
  c = mx_e_ad_tag(c, 1, 1);
  c = mx_e_ad(c, MX_GS_REG_FRAME_1, mx_gs_frame(t->fbp, t->fbw, t->psm, 0));
  mx_frame_commit(c);
  }
  gs.valid = 0, gs_serial++;
}
