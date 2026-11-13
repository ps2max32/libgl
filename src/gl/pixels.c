/* gl/pixels.c, a part of mxgl.c (included there, not compiled alone): glReadPixels,
   glCopyTexSubImage2D, glGetTexImage */

/* The pixels of the frame being drawn (or the last one, between frames):
   what the stream has so far is drawn first (mx_frame_sync; slow: the EE
   waits for the GS), then read back line by line. GL_RGB, GL_RGBA (bytes)
   and GL_DEPTH_COMPONENT (GL_FLOAT, GL_UNSIGNED_INT, _SHORT, _BYTE), in the
   window's coordinates (mxgl_window), lines GL_PACK_ALIGNMENT apart.
   Not in 1080i (the passes own the buffer): zeros. */
static u32 rp_line[1920] MX_ALIGNED(64) MX_CORE_BSS; /* the GS's download writes it */
void APIENTRY glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t,
                           GLvoid *px) {
  int depth = f == GL_DEPTH_COMPONENT;
  int nc = f == GL_RGB ? 3 : f == GL_RGBA ? 4 : 1;
  int bpc = !depth || t == GL_UNSIGNED_BYTE ? 1 : t == GL_UNSIGNED_SHORT ? 2 : 4;
  if ((!depth && f != GL_RGB && f != GL_RGBA) || (!depth && t != GL_UNSIGNED_BYTE) || w < 0 || h < 0) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  u32 line = ((u32)(w * nc * bpc) + (u32)gl.pack_align - 1) & ~((u32)gl.pack_align - 1);
  memset(px, 0, line * (u32)h);
  if (beam || !w || !h)
    return;
  pq_flush();
  if (in_frame)
    flush_out();
  if (mx_frame_sync() < 0)
    return;
  /* the frame's colour buffer (between frames: the last one's), the Z buffer */
  const struct mx_rt *rtb = &rt[buf];
  u32 bp = depth ? MX_GS_PAGE(rtb->zbp) : MX_GS_PAGE(rtb->fbp);
  /* Z24 read as Z32 (the same layout): a 24-bit transfer sends 3 bytes a pixel */
  u32 psm = depth ? (rtb->zpsm == MX_GS_PSM_Z24 ? MX_GS_PSM_Z32 : rtb->zpsm) : rtb->psm;
  int last_fy = -1;
  for (int j = 0; j < h; j++) {
    /* GL's y is up from the bottom of the window */
    int gy = y + j, fy = (gl_h - 1 - gy) * fb_h / gl_h;
    if (gy < 0 || gy >= gl_h || fy < 0 || fy >= fb_h)
      continue;
    if (fy != last_fy && mx_gs_download(rp_line, bp, rtb->fbw, psm, 0, (u32)fy, (u32)fb_w, 1) < 0)
      break;
    last_fy = fy;
    u8 *d = (u8 *)px + (u32)j * line;
    for (int i = 0; i < w; i++) {
      int gx = x + i, fx = gx * fb_w / gl_w;
      if (gx < 0 || gx >= gl_w || fx < 0 || fx >= fb_w)
        continue;
      if (depth) {
        /* GS Z is (1 - depth) of its range (make_mvp) */
        u32 z = rp_line[fx] & (psm == MX_GS_PSM_Z24 || psm == MX_GS_PSM_Z32 ? 0xFFFFFFu : 0xFFFFu);
        float zm = zmax * 16.0f + 15.0f, dv = 1.0f - (float)z / zm;
        if (t == GL_FLOAT)
          memcpy(d + 4 * i, &dv, 4);
        else if (t == GL_UNSIGNED_INT)
          ((u32 *)d)[i] = (u32)(dv * 4294967295.0f);
        else if (t == GL_UNSIGNED_SHORT)
          ((u16 *)d)[i] = (u16)(dv * 65535.0f);
        else
          d[i] = (u8)(dv * 255.0f);
      } else {
        u32 p = ((const u16 *)rp_line)[fx];
        u8 *o = d + nc * i;
        u32 r = p & 31, g = p >> 5 & 31, b = p >> 10 & 31;
        o[0] = (u8)(r << 3 | r >> 2), o[1] = (u8)(g << 3 | g >> 2), o[2] = (u8)(b << 3 | b >> 2);
        if (nc == 4)
          o[3] = 255;
      }
    }
  }
}

/* the window's pixels into the bound texture's level 0 (glReadPixels, then
   glTexSubImage2D): the frame drawn so far is finished first, so it is
   slow; with beam racing the pixels are zeros (glReadPixels) */
void APIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y,
                                  GLsizei w, GLsizei h) {
  if (w <= 0 || h <= 0)
    return;
  u8 *px = malloc((size_t)w * (size_t)h * 4);
  if (!px) {
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  int align = gl.pack_align;
  gl.pack_align = 1;
  glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  gl.pack_align = align;
  glTexSubImage2D(target, level, xoff, yoff, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  free(px);
}

/* the bound texture's level 0 as RGBA or RGB bytes, from its texels in
   GS format (16-bit ones as 5 bits a channel, a brightness texture as
   grey); level 0 only */
void APIENTRY glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *px) {
  GLuint bound = TBOUND;
  int nc = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 0;
  if (!nc || type != GL_UNSIGNED_BYTE) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  if (level != 0 || !bound || bound >= MAX_TEX || !texs[bound].data)
    return;
  pq_flush();
  const struct tex *t = &texs[bound];
  const u8 *src = TLV(t, 0);
  u32 line = ((t->w * (u32)nc) + (u32)gl.pack_align - 1) & ~((u32)gl.pack_align - 1);
  for (u32 y = 0; y < t->h; y++) {
    u8 *d = (u8 *)px + y * line;
    for (u32 x = 0; x < t->w; x++, d += nc) {
      u32 i = y * t->w + x;
      u8 c[4];
      if (t->psm == MX_GS_PSM_CT32) {
        u32 v = ((const u32 *)src)[i];
        u32 a = v >> 24;
        c[0] = (u8)v, c[1] = (u8)(v >> 8), c[2] = (u8)(v >> 16), c[3] = (u8)(a >= 128 ? 255 : a * 2);
      } else if (t->psm == MX_GS_PSM_T8) {
        u8 k = src[i];
        if (t->pal)
          memcpy(c, pal_rgb[k], 3), c[3] = k == pal_transp ? 0 : 255;
        else
          c[0] = c[1] = c[2] = k, c[3] = 255;
      } else {
        u16 v = ((const u16 *)src)[i];
        c[0] = (u8)((v & 31) << 3), c[1] = (u8)(((v >> 5) & 31) << 3), c[2] = (u8)(((v >> 10) & 31) << 3);
        c[3] = v & 0x8000 ? 255 : 0;
      }
      memcpy(d, c, (size_t)nc);
    }
  }
}
