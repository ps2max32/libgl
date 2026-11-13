/* gl/paltex.c, a part of mxgl.c (included there, not compiled alone): paletted textures:
   glColorTable, glCompressedTexImage2D */

/* GL_EXT_paletted_texture, GL_EXT_shared_texture_palette: a palette of up
   to 256 colours (GL_RGB, GL_RGBA bytes) for GL_COLOR_INDEX textures: the
   shared one (whose RGB textures are also kept as its indices: 8 bits,
   exact), or the bound texture's (for its next GL_COLOR_INDEX texels) */
void APIENTRY glColorTableEXT(GLenum target, GLenum ifmt, GLsizei width, GLenum format, GLenum type,
                              const GLvoid *table) {
  int nc = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 0;
  if (!nc || type != GL_UNSIGNED_BYTE || width <= 0 || width > 256 || !table) {
    gl_error(!nc || type != GL_UNSIGNED_BYTE ? GL_INVALID_ENUM : GL_INVALID_VALUE);
    return;
  }
  const u8 *c = table;
  u8 rgba[256][4];
  memset(rgba, 0, sizeof rgba);
  for (int i = 0; i < width; i++, c += nc)
    rgba[i][0] = c[0], rgba[i][1] = c[1], rgba[i][2] = c[2], rgba[i][3] = nc == 4 ? c[3] : 255;
  if (target == GL_SHARED_TEXTURE_PALETTE_EXT) {
    u8 rgb[256 * 3];
    for (int i = 0; i < 256; i++)
      memcpy(rgb + 3 * i, rgba[i], 3);
    pq_flush();
    pal_transp = -1;
    for (int i = 0; i < 256 && pal_transp < 0; i++)
      if (!rgba[i][3])
        pal_transp = i;
    mxgl_Palette(rgb);
    memcpy(pal_rgba, rgba, sizeof rgba);
    return;
  }
  GLuint b = TBOUND;
  if (target != GL_TEXTURE_2D || !b || b >= MAX_TEX) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  if (!texs[b].ipal && !(texs[b].ipal = malloc(1024))) {
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  memcpy(texs[b].ipal, rgba, sizeof rgba);
}
void APIENTRY glColorTable(GLenum target, GLenum ifmt, GLsizei width, GLenum format, GLenum type,
                           const GLvoid *table) {
  glColorTableEXT(target, ifmt, width, format, type, table);
}

/* GL_OES_compressed_paletted_texture: a palette (16 or 256 colours) and the
   texels' indices (4 or 8 bits; two a byte, the first in the high bits),
   level 0 and, for a level of -n, n levels after it, each half the size.
   Kept as they are: T8 with the texture's own CLUT (as GS memory takes it,
   a 4-bit texture's indices made bytes), the levels given (none made). */
static int oes_pal(GLenum f, int *bits, int *eb) {
  if (f < GL_PALETTE4_RGB8_OES || f > GL_PALETTE8_RGB5_A1_OES)
    return 0;
  int k = (int)(f - GL_PALETTE4_RGB8_OES);
  *bits = k < 5 ? 4 : 8;
  static const int ebytes[5] = {3, 4, 2, 2, 2};
  *eb = ebytes[k % 5];
  return 1;
}
static void oes_entry(GLenum f, const u8 *e, u8 *o) {
  u32 v = (u32)e[0] | (u32)e[1] << 8;
  switch ((f - GL_PALETTE4_RGB8_OES) % 5) {
  case 0: o[0] = e[0], o[1] = e[1], o[2] = e[2], o[3] = 255; break;
  case 1: o[0] = e[0], o[1] = e[1], o[2] = e[2], o[3] = e[3]; break;
  case 2: /* R5 G6 B5 */
    o[0] = (u8)((v >> 11) * 255 / 31), o[1] = (u8)((v >> 5 & 63) * 255 / 63), o[2] = (u8)((v & 31) * 255 / 31);
    o[3] = 255;
    break;
  case 3: /* R4 G4 B4 A4 */
    o[0] = (u8)((v >> 12) * 17), o[1] = (u8)((v >> 8 & 15) * 17), o[2] = (u8)((v >> 4 & 15) * 17);
    o[3] = (u8)((v & 15) * 17);
    break;
  default: /* R5 G5 B5 A1 */
    o[0] = (u8)((v >> 11) * 255 / 31), o[1] = (u8)((v >> 6 & 31) * 255 / 31), o[2] = (u8)((v >> 1 & 31) * 255 / 31);
    o[3] = v & 1 ? 255 : 0;
    break;
  }
}

void APIENTRY glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                     GLint border, GLsizei bytes, const GLvoid *data) {
  pq_flush();
  tex_serial++;
  int bits, eb;
  GLuint b = TBOUND;
  if (target != GL_TEXTURE_2D || !oes_pal(ifmt, &bits, &eb)) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  if (level > 0 || border || w <= 0 || h <= 0 || !data) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  if (!b || b >= MAX_TEX)
    return;
  int npal = bits == 4 ? 16 : 256, nlev = 1 - level;
  const u8 *p = data, *end = p + bytes;
  u8 pal[256][4];
  memset(pal, 0, sizeof pal);
  if (p + npal * eb > end) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  int alpha = 0;
  for (int i = 0; i < npal; i++, p += eb) {
    oes_entry(ifmt, p, pal[i]);
    alpha |= pal[i][3] != 255;
  }
  struct tex *t = &texs[b];
  /* sides that are not powers of two, or below 8: as RGBA (scaled) */
  if (w < 8 || h < 8 || (w & (w - 1)) || (h & (h - 1))) {
    u8 *rgba = malloc((size_t)w * (size_t)h * 4);
    if (!rgba)
      return;
    for (int i = 0; i < w * h; i++) {
      int ix = bits == 8 ? p[i] : (p[i / 2] >> (i & 1 ? 0 : 4)) & 15;
      memcpy(rgba + 4 * i, pal[ix], 4);
    }
    tex_image_2d(GL_TEXTURE_2D, 0, alpha ? GL_RGBA : GL_RGB, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    return;
  }
  tex_drop_twin(t);
  tex_brightness = 0;
  tex_set_image(t, GL_COLOR_INDEX8_EXT, w, h, NULL); /* frees the old texels and CLUT */
  tex_free_mem(t);
  t->psm = MX_GS_PSM_T8;
  t->pal = 0, t->bright = 0;
  t->alpha = alpha;
  t->alpha_fmt = ifmt == GL_PALETTE4_RGBA8_OES || ifmt == GL_PALETTE8_RGBA8_OES ||
                 ifmt == GL_PALETTE4_RGBA4_OES || ifmt == GL_PALETTE8_RGBA4_OES ||
                 ifmt == GL_PALETTE4_RGB5_A1_OES || ifmt == GL_PALETTE8_RGB5_A1_OES;
  /* the levels given, down to 8 x 8 */
  u32 nl = 0;
  while ((int)nl < nlev && nl < t->max_levels)
    nl++;
  u32 max = t->max_levels;
  t->max_levels = (u8)(nl ? nl : 1);
  int bad = tex_alloc_mem(t, 1024) < 0;
  t->max_levels = (u8)max;
  if (bad) {
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  for (u32 lv = 0; lv < nl; lv++) {
    u32 n = (t->w >> lv) * (t->h >> lv), nb = bits == 8 ? n : (n + 1) / 2;
    if (p + nb > end) {
      nl = lv;
      break;
    }
    u8 *d = TLV(t, lv);
    for (u32 i = 0; i < n; i++)
      d[i] = bits == 8 ? p[i] : (u8)((p[i / 2] >> (i & 1 ? 0 : 4)) & 15);
    p += nb;
  }
  t->levels = nl ? nl : 1;
  for (u32 i = 0; i < 256; i++)
    t->oclut[csm1(i)] = (u32)pal[i][0] | (u32)pal[i][1] << 8 | (u32)pal[i][2] << 16 |
                        (u32)((pal[i][3] + 1u) >> 1) << 24;
  tex_writeback(t);
  tex_new_gen(t); /* new texels: a new key */
}
void APIENTRY glCompressedTexImage2DARB(GLenum target, GLint level, GLenum ifmt, GLsizei w,
                                        GLsizei h, GLint border, GLsizei bytes, const GLvoid *data) {
  glCompressedTexImage2D(target, level, ifmt, w, h, border, bytes, data);
}
