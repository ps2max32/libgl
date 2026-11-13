/* gl/attribs.c, a part of mxgl.c (included there, not compiled alone): vertex programs with
   inputs of their own: glVertexAttribPointer, and their draws (glDrawArrays) */

/* A program whose inputs are not libgl's three (POSITION, TEXCOORD,
   COLOR_V4_8U) is drawn from generic attribute arrays: input i of the HLSL
   program is attribute i (glGetAttribLocation by its semantic), an array in
   the input's own format in memory (its UNPACK format: a int4
   POSITION_V4_16 is four shorts a vertex, a int4 NORMAL_V4_8 four bytes),
   so a model's vertices go to VU1 as they are stored. glDrawArrays of
   GL_TRIANGLE_STRIP, GL_TRIANGLES or GL_TRIANGLE_FAN copies the arrays into
   the frame and VU1 does all the rest: libgl does not look at the vertices,
   so nothing is culled or clipped (the program sets ADC itself: a strip's
   first two vertices, as mxglv.hlsl by a flag in an input; the caller draws
   only what is inside the guard band and beyond the near plane). An input
   with the semantic ADC (uint4 ADC_V4_8U) is libgl's: 1 for a strip's first
   two vertices, else 0. glDrawElements gathers the vertices by the indices
   (16 or 32 bits), where an index that restarts the primitive
   (GL_PRIMITIVE_RESTART with glPrimitiveRestartIndex, or
   GL_PRIMITIVE_RESTART_FIXED_INDEX) ends a strip: a model's strips in one
   draw, from its vertices as they are stored. libgl sets
   the uniforms mvp, cscale, tms, tmt as for the other programs. K of the
   mipmap level is for one repeat of the texture a unit of the vertices'
   space; GL_TEXTURE_LOD_BIAS (glTexEnvf(GL_TEXTURE_FILTER_CONTROL, ...))
   adds the surface's own log2 of repeats a unit. Between glLockArraysEXT and
   glUnlockArraysEXT the copies are kept for the next draws of the same
   arrays (a surface's next pass). Not recorded in display lists. */
#define MAX_ATTR MX_SHADER_MAX_IN
static struct {
  const void *ptr;
  int stride;
  u8 on;
} attr[MAX_ATTR];

void APIENTRY glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                    GLsizei stride, const GLvoid *ptr) {
  (void)size, (void)type, (void)normalized; /* the program's input says the format */
  if (index >= MAX_ATTR) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  attr[index].ptr = buf_ptr(gl.abuf, (uintptr_t)ptr), attr[index].stride = stride;
}
void APIENTRY glEnableVertexAttribArray(GLuint index) {
  if (index < MAX_ATTR)
    attr[index].on = 1;
  else
    gl_error(GL_INVALID_VALUE);
}
void APIENTRY glDisableVertexAttribArray(GLuint index) {
  if (index < MAX_ATTR)
    attr[index].on = 0;
  else
    gl_error(GL_INVALID_VALUE);
}

/* the copies of the last draw's arrays, kept while the arrays are locked */
static struct {
  const void *ptr[MAX_ATTR], *at[MAX_ATTR];
  struct vprog *p;
  const void *idx;
  int first, count;
  u32 frame, lock, n;
  int valid;
} ac;

/* GL_PRIMITIVE_RESTART (glPrimitiveRestartIndex) and
   GL_PRIMITIVE_RESTART_FIXED_INDEX (the type's largest): an index that ends
   a strip; the two vertices after it close no triangle (ADC) */
static u32 prim_restart_index;
void APIENTRY glPrimitiveRestartIndex(GLuint index) { prim_restart_index = index; }

/* glDrawArrays (idx NULL: vertices first .. first + count - 1) and
   glDrawElements (count indices of itype) with a program of its own inputs */
static void draw_attribs(GLenum mode, GLint first, GLsizei count, GLenum itype, const void *idx) {
  struct vprog *p = cur_prog;
  const struct mx_shader *s = &p->s;
  u32 type = mode == GL_TRIANGLE_STRIP ? MX_GS_PRIM_TRISTRIP
             : mode == GL_TRIANGLES    ? MX_GS_PRIM_TRIANGLE
             : mode == GL_TRIANGLE_FAN ? MX_GS_PRIM_TRIFAN
                                       : 8;
  if (type == 8 || (idx && itype != GL_UNSIGNED_SHORT && itype != GL_UNSIGNED_INT)) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  pq_flush();
  if (count < 3 || first < 0 || dl_compiling)
    return;
  for (u32 i = 0; i < s->nin; i++)
    if ((int)i != p->in_adc && (!attr[i].on || !attr[i].ptr)) {
      gl_error(GL_INVALID_OPERATION);
      return;
    }
  u32 t0 = mx_count();
  frame_start();
  st_cur.calls++;
  update_matrix();
  flush_out();
  /* the GS state, unless nothing it depends on has changed (as prepare_state) */
  struct tex *bt = gl.tex2d && gl.bound && gl.bound < MAX_TEX ? &texs[gl.bound] : NULL;
  s32 k = 0;
  if (bt && bt->used && bt->levels > 1) {
    float kf = (0.5f * ((float)bt->lw + (float)bt->lh + tm_log2_area()) + gl.lod_bias - proj_lpp()) * 16.0f;
    k = (s32)(kf + (kf < 0 ? -0.5f : 0.5f));
    k = k < -2048 ? -2048 : k > 2047 ? 2047 : k;
  }
  u32 bgen = gl.bound && gl.bound < MAX_TEX ? texs[gl.bound].gen + (u32)texs[gl.bound].dirty * 0x80000000u : 0;
  if (!(k_valid && k_gser == gs_serial && k_sser == st_serial && k_tser == tex_serial &&
        k_vgen == vram_gen && k_gen == bgen && k == k_lod)) {
    k_lod = k;
    k_ok = set_state(&k_prim, k_cs, k) == 0;
    k_valid = 1;
    k_gen = gl.bound && gl.bound < MAX_TEX ? texs[gl.bound].gen + (u32)texs[gl.bound].dirty * 0x80000000u : 0;
    k_vgen = vram_gen, k_tser = tex_serial, k_mser = mat_serial, k_gser = gs_serial, k_sser = st_serial;
  }
  if (!k_ok) {
    st_cur.refused++;
    return;
  }
  /* the arrays into the frame's arena, each stream on a quadword, with the
     vertices a batch reads past the last as zeros */
  u32 frame = mx_frame_number();
  int kept = ac.valid && lk.on && ac.lock == lk_serial && ac.frame == frame && ac.p == p &&
             ac.first == first && ac.count == count && ac.idx == idx;
  for (u32 i = 0; kept && i < s->nin; i++)
    kept = ac.ptr[i] == attr[i].ptr;
  if (!kept) {
    /* an index that ends a strip is no vertex */
    int restart = idx && (prim_restart || prim_restart_fixed);
    u32 ri = prim_restart_fixed ? (itype == GL_UNSIGNED_SHORT ? 0xFFFFu : 0xFFFFFFFFu) : prim_restart_index;
    const u16 *i16 = itype == GL_UNSIGNED_SHORT ? idx : NULL;
    const u32 *i32 = itype == GL_UNSIGNED_INT ? idx : NULL;
#define IDX(j) (i16 ? (u32)i16[j] : i32[j])
    u32 n = (u32)count;
    if (restart)
      for (int j = 0; j < count; j++)
        n -= IDX(j) == ri;
    if (n < 3)
      return;
    /* each stream in the part of the arena with the largest share left (the
       fixed draws' positions, texture coordinates and colours: 4, 2 and 1
       sevenths), so these draws take from all three as those do */
    const u32 part_at[3] = {0, POS_PART, POS_PART + ST_PART}, part_size[3] = {POS_PART, ST_PART, COL_PART};
    u32 at0[3] = {s_at[0], s_at[1], s_at[2]};
    for (u32 i = 0; i < s->nin; i++) {
      u32 b = s->in[i].bytes, bw = (b + 3) / 4, size = ((n + s->align) * b + 15) & ~15u;
      u32 part = 0;
      /* by the share of it that is free (compared without dividing) */
      for (u32 q = 1; q < 3; q++)
        if ((u64)(part_size[q] - s_at[q]) * part_size[part] > (u64)(part_size[part] - s_at[part]) * part_size[q])
          part = q;
      if (s_at[part] + size > part_size[part]) {
        /* no room: what this draw took is given back */
        s_at[0] = at0[0], s_at[1] = at0[1], s_at[2] = at0[2];
        ac.valid = 0;
        dropped_draws++;
        return;
      }
      u8 *a = arena[frame_par] + part_at[part] + s_at[part];
      /* the same array as an input before (a model's two frames, the same): its copy */
      int same = -1;
      for (u32 j = 0; j < i && same < 0; j++)
        if ((int)i != p->in_adc && (int)j != p->in_adc && attr[j].ptr == attr[i].ptr &&
            attr[j].stride == attr[i].stride && s->in[j].bytes == b)
          same = (int)j;
      if (same >= 0) {
        ac.ptr[i] = attr[i].ptr, ac.at[i] = ac.at[same];
        continue;
      }
      s_at[part] += size;
      u32 *d = UCAB(a);
      u32 w = 0;
      if ((int)i == p->in_adc) {
        /* libgl's own input: 1 where no triangle ends (a strip's first two) */
        u32 pend = type == MX_GS_PRIM_TRISTRIP ? 2 : 0;
        for (int j = 0; j < count; j++) {
          if (restart && IDX(j) == ri) {
            pend = 2;
            continue;
          }
          d[w] = pend ? 1 : 0;
          for (u32 q = 1; q < bw; q++)
            d[w + q] = 0;
          w += bw;
          pend -= pend != 0;
        }
      } else {
        u32 stride = attr[i].stride ? (u32)attr[i].stride : b;
        const u8 *src = (const u8 *)attr[i].ptr;
        if (!idx && stride == b) {
          src += (u32)first * stride;
          for (; w < (n * b + 3) / 4; w++) {
            u32 v;
            memcpy(&v, src + 4 * w, 4);
            d[w] = v;
          }
        } else if (b == 8) { /* a record of two words (shorts x, y, z, w; floats s, t) */
          u64 *d8 = (u64 *)d;
          u32 o = 0;
          for (int j = 0; j < count; j++) {
            u32 v = idx ? IDX(j) : (u32)(first + j);
            if (restart && v == ri)
              continue;
            u64 r;
            memcpy(&r, src + v * stride, 8);
            d8[o++] = r;
          }
          w = o * 2;
        } else {
          for (int j = 0; j < count; j++) {
            u32 v = idx ? IDX(j) : (u32)(first + j);
            if (restart && v == ri)
              continue;
            for (u32 q = 0; q < bw; q++) {
              u32 r;
              memcpy(&r, src + v * stride + 4 * q, 4);
              d[w + q] = r;
            }
            w += bw;
          }
        }
      }
#undef IDX
      for (; w < size / 4; w++)
        d[w] = 0;
      ac.ptr[i] = attr[i].ptr, ac.at[i] = a;
    }
    arena_pos = s_at[0] + s_at[1] + s_at[2];
    batch_place();
    ac.p = p, ac.first = first, ac.count = count, ac.idx = idx, ac.frame = frame, ac.lock = lk_serial;
    ac.n = n;
    ac.valid = 1;
    mx_sync(); /* the UCAB stores are done before the DMA reads them */
  }
  u32 n = ac.n;
  static float fx[MXGL_UNI_QW][4] MX_ALIGNED(16);
  memcpy(fx[VU_MXGLV_MVP], m, 64);
  memcpy(fx[VU_MXGLV_CSCALE], k_cs, 16);
  tm_uniforms(fx[VU_MXGLV_TMS], fx[VU_MXGLV_TMT]);
  const struct mx_shader *sh = vprog_use(p);
  const void *uni = vprog_uni(p, (const float(*)[4])fx);
  u32 prim = (k_prim & ~7u) | type;
  mx_frame_shader_draw(sh, prim, uni, ac.at, n);
  if (cl_batch) { /* a channel a pass (mxgl_color_light) */
    cl_regs(1);
    mx_frame_shader_draw(sh, prim, uni, ac.at, n);
    cl_regs(2);
    mx_frame_shader_draw(sh, prim, uni, ac.at, n);
    cl_regs(0);
  }
  gs_drew = 1;
  st_cur.draws++;
  st_cur.verts += n;
  st_cur.tris += type == MX_GS_PRIM_TRIANGLE ? n / 3 : n - 2;
  st_cur.gl_cycles += mx_count() - t0;
}
