/* gl/teximage.c, a part of mxgl.c (included there, not compiled alone): texture objects and
   images: glTexImage2D, the levels, brightness and channel copies, glTexSubImage2D,
   glTexParameter */

/* textures */
void APIENTRY glGenTextures(GLsizei n, GLuint *t) {
  for (int i = 0; i < n; i++) {
    t[i] = 0;
    for (int k = 1; k < MAX_TEX; k++)
      if (!texs[k].used) {
        memset(&texs[k], 0, sizeof texs[k]);
        texs[k].used = 1;
        texs[k].lin_mag = texs[k].lin_min = 1;
        texs[k].mip = 2; /* GL's default NEAREST_MIPMAP_LINEAR (linear here) */
        t[i] = (GLuint)k;
        break;
      }
  }
}

/* data still read by frames in flight is freed two frames later */
/* beam: a record is played until a later one replaces it, which may be
   MX_HIRES_SLOTS frames later */
#define NDEFER (MX_HIRES_SLOTS + 1)
static void **deferred[NDEFER];
static int ndeferred[NDEFER], cdeferred[NDEFER];
static u32 defer_frame;
static void defer_free(void *p) {
  if (!p)
    return;
  int k = (int)(mx_gs.frames % NDEFER);
  if (ndeferred[k] == cdeferred[k]) {
    /* a list that grows (a level's textures go in one frame) */
    int c = cdeferred[k] ? 2 * cdeferred[k] : 256;
    void **d = realloc(deferred[k], (size_t)c * sizeof *d);
    if (!d) {
      /* no room to keep it: free it once no frame reads anything */
      u32 t0 = mx_count();
      while (mx_frame_done() < mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
        ;
      if (!in_frame) {
        gl_free(p);
        return;
      }
      gl_error(GL_OUT_OF_MEMORY);
      return; /* leaked: the frame being made may read it */
    }
    deferred[k] = d, cdeferred[k] = c;
  }
  deferred[k][ndeferred[k]++] = p;
}
static void defer_collect(void) {
  int k = (int)((mx_gs.frames + 1) % NDEFER); /* filled NDEFER - 1 frames ago */
  if (defer_frame == mx_gs.frames)
    return;
  defer_frame = mx_gs.frames;
  for (int i = 0; i < ndeferred[k]; i++)
    gl_free(deferred[k][i]);
  ndeferred[k] = 0;
}

/* the lists name textures: when textures go, so do the lists (the renderer
   asks, mxgl_ListsDirty, and makes them again) */
int mxgl_ListsDirty(void) { return 0; }

void APIENTRY glDeleteTextures(GLsizei n, const GLuint *t) {
  pq_flush();
  tex_serial++;
  for (int i = 0; i < n; i++)
    if (t[i] && t[i] < MAX_TEX) {
      if (texs[t[i]].gen)
        tex_drop(&texs[t[i]]);
      tex_free_mem(&texs[t[i]]);
      free(texs[t[i]].ipal);
      texs[t[i]].data = NULL;
      texs[t[i]].oclut = NULL;
      texs[t[i]].ipal = NULL;
      tex_drop_twin(&texs[t[i]]);
      texs[t[i]].used = 0;
    }
}
void APIENTRY glBindTexture(GLenum target, GLuint t) {
  if (target != GL_TEXTURE_2D) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  GLuint *b = gl.cunit ? &gl.u1.bound : &gl.bound;
  if (*b != t) {
    pq_flush();
    st_serial++;
  }
  *b = t;
  /* a name not from glGenTextures is made by binding it (GL 1.1; GLQuake
     numbers its textures itself) */
  if (t && t < MAX_TEX && !texs[t].used) {
    memset(&texs[t], 0, sizeof texs[t]);
    texs[t].used = 1;
    texs[t].lin_mag = texs[t].lin_min = 1;
    texs[t].mip = 2;
  }
}

static u32 ilog2(u32 v) {
  u32 l = 0;
  while ((1u << l) < v)
    l++;
  return l;
}

/* RGBA8 (GL) into level lv of texture t at (x, y) */
static void convert(struct tex *t, u32 lv, int x0, int y0, int w, int h, const u8 *src) {
  u32 lw = t->w >> lv;
  u8 *dst = TLV(t, lv);
  /* the format chosen once, not per texel (lightmaps change every frame) */
  int f = t->pal ? 0 : t->psm == MX_GS_PSM_T8 ? 1 : t->psm == MX_GS_PSM_CT32 ? 2 : 3;
  for (int y = 0; y < h; y++) {
    const u8 *s = src + (u32)(y * w) * 4;
    u32 at = (u32)(y0 + y) * lw + (u32)x0;
    switch (f) {
    case 0:
      for (int x = 0; x < w; x++, s += 4)
        dst[at + (u32)x] = pal_transp >= 0 && s[3] < 128 ? (u8)pal_transp : pal_nearest(s);
      break;
    case 1:
      for (int x = 0; x < w; x++, s += 4) {
        u8 m = s[0] > s[1] ? s[0] : s[1];
        dst[at + (u32)x] = m > s[2] ? m : s[2];
      }
      break;
    case 2:
      for (int x = 0; x < w; x++, s += 4)
        ((u32 *)dst)[at + (u32)x] = (u32)s[0] | (u32)s[1] << 8 | (u32)s[2] << 16 | ((s[3] + 1u) >> 1) << 24;
      break;
    default:
      for (int x = 0; x < w; x++, s += 4)
        ((u16 *)dst)[at + (u32)x] =
            (u16)(s[0] >> 3 | (s[1] >> 3) << 5 | (s[2] >> 3) << 10 | (s[3] >= 128 ? 0x8000 : 0));
      break;
    }
  }
}

/* levels 1 .. max_levels - 1 from level 0's RGBA8 texels: each the mean of
   2 x 2 of the level above */
static void make_levels(struct tex *t, const u8 *src) {
  u32 w = t->w, h = t->h;
  if (t->max_levels < 2)
    return;
  u8 *a = malloc((w / 2) * (h / 2) * 4), *b = malloc((w / 4) * (h / 4) * 4 + 4);
  if (!a || !b) {
    free(a), free(b);
    return;
  }
  const u8 *up = src;
  u8 *bufs[2] = {a, b};
  for (u32 lv = 1; lv < t->max_levels; lv++) {
    u32 nw = w >> 1, nh = h >> 1;
    u8 *dn = bufs[(lv - 1) & 1];
    for (u32 y = 0; y < nh; y++)
      for (u32 x = 0; x < nw; x++)
        for (u32 k = 0; k < 4; k++) {
          const u8 *p = up + ((2 * y) * w + 2 * x) * 4 + k;
          dn[(y * nw + x) * 4 + k] = (u8)((p[0] + p[4] + p[w * 4] + p[w * 4 + 4] + 2) >> 2);
        }
    convert(t, lv, 0, 0, (int)nw, (int)nh, dn);
    up = dn, w = nw, h = nh;
  }
  t->levels = t->max_levels;
  free(a), free(b);
}

/* the next texture is kept as its brightness (the largest of R, G and B),
   T8 with a CLUT of that in alpha, for blendFunc filter (Cd * As) */
static int tex_brightness;
void mxgl_TexBrightness(void) { tex_brightness = 1; }

/* the colour of the draws after is scaled by 1 << s (s 0 or 1), the
   overbright range of hardware gamma, done by the GS's modulate */
void mxgl_ColorShift(int s) {
  pq_flush();
  st_serial++;
  env_legacy = s;
  env_shifts();
}

/* texels of format (GL_RGB, GL_LUMINANCE, ...) as RGBA8 (malloc'ed), or
   NULL if they are RGBA8 already (or of a format not known) */
static u8 *to_rgba(GLenum format, GLsizei w, GLsizei h, const GLvoid *px) {
  int n = format == GL_RGB ? 3 : format == GL_LUMINANCE || format == GL_ALPHA ? 1
          : format == GL_LUMINANCE_ALPHA ? 2 : 0;
  if (!n || !px)
    return NULL;
  u8 *o = malloc((size_t)w * (size_t)h * 4);
  if (!o)
    return NULL;
  const u8 *s = px;
  for (int i = 0; i < w * h; i++, s += n) {
    u8 *d = o + i * 4;
    if (format == GL_RGB)
      d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = 255;
    else if (format == GL_ALPHA)
      d[0] = d[1] = d[2] = 255, d[3] = s[0];
    else
      d[0] = d[1] = d[2] = s[0], d[3] = n == 2 ? s[1] : 255;
  }
  return o;
}

static void tex_image_2d(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                         GLint border, GLenum format, GLenum type, const GLvoid *px);

/* the texels of a GL_COLOR_INDEX texture (bytes) as RGBA8 (malloc'ed), by
   the shared palette when it is on or the texture has none, else by the
   texture's; NULL without texels */
static u8 *index_rgba(GLsizei w, GLsizei h, const GLvoid *px) {
  if (!px)
    return NULL;
  u8 *o = malloc((size_t)w * (size_t)h * 4);
  if (!o)
    return NULL;
  GLuint b = TBOUND;
  const u8(*pal)[4] = !gl.shared_pal && b < MAX_TEX && texs[b].ipal ? (const u8(*)[4])texs[b].ipal
                                                                    : (const u8(*)[4])pal_rgba;
  const u8 *s = px;
  for (int i = 0; i < w * h; i++)
    memcpy(o + 4 * i, pal[s[i]], 4);
  return o;
}
/* the next tex_set_image's texels are the shared palette's indices, a byte
   each (glTexImage2D with GL_COLOR_INDEX, the palette on) */
static int tex_indexed;

static void tex_set_image(struct tex *t, GLint ifmt, GLsizei w, GLsizei h, const GLvoid *px);

static void tex_drop_twin_ch(struct tex *t, int ch) {
  if (!t->twin[ch])
    return;
  struct tex *c = &twins[t->twin[ch] - 1];
  tex_free_mem(c);
  if (c->gen)
    tex_drop(c);
  memset(c, 0, sizeof *c);
  t->twin[ch] = 0;
}

static void tex_drop_twin(struct tex *t) {
  for (int ch = 0; ch < NTWIN; ch++) {
    if (!t->twin[ch])
      continue;
    struct tex *c = &twins[t->twin[ch] - 1];
    tex_free_mem(c);
    if (c->gen)
      tex_drop(c);
    memset(c, 0, sizeof *c);
    t->twin[ch] = 0;
  }
}

/* texel i of t's level 0 as RGB */
static void tex_rgb(const struct tex *t, u32 i, u8 *o) {
  const u8 *src = TLV(t, 0);
  if (t->oclut) {
    u32 c = t->oclut[csm1(src[i])];
    o[0] = (u8)c, o[1] = (u8)(c >> 8), o[2] = (u8)(c >> 16);
  } else if (t->pal) {
    o[0] = pal_rgb[src[i]][0], o[1] = pal_rgb[src[i]][1], o[2] = pal_rgb[src[i]][2];
  } else if (t->psm == MX_GS_PSM_CT32) {
    o[0] = src[i * 4], o[1] = src[i * 4 + 1], o[2] = src[i * 4 + 2];
  } else {
    u32 c = ((const u16 *)src)[i];
    o[0] = (u8)((c & 31) << 3), o[1] = (u8)((c >> 5 & 31) << 3), o[2] = (u8)((c >> 10 & 31) << 3);
  }
}

/* texel i of t's level 0's alpha, 0 .. 255 */
static u32 tex_alpha_of(const struct tex *t, u32 i) {
  const u8 *src = TLV(t, 0);
  u32 a;
  if (t->bright)
    return t->grey == 2 && !src[i] ? 0 : 255;
  if (t->oclut)
    a = t->oclut[csm1(src[i])] >> 24;
  else if (t->pal)
    return pal_rgba[src[i]][3];
  else if (t->psm == MX_GS_PSM_CT32)
    a = src[i * 4 + 3];
  else
    return ((const u16 *)src)[i] & 0x8000 ? 255 : 0;
  return a >= 0x80 ? 255 : a * 2; /* stored halved: 0x80 is 1.0 */
}

/* t's brightness copy (T8, the largest of R, G and B; for blendFunc
   products, blend_product), made from its level 0 when first asked for;
   NULL if there is no room */
static int tex_one_level; /* tex_set_image: level 0 only (planar textures, their copies) */

/* t's copy ch made from rgba (t's size, RGBA8): the brightness, or a
   channel's value (which the brightness, the max, then keeps) */
static struct tex *twin_make(struct tex *t, int ch, u8 *rgba) {
  int k = 0;
  while (k < MAX_TWIN && twins[k].used)
    k++;
  if (k == MAX_TWIN)
    return NULL;
  if (ch >= 1 && ch <= 3)
    for (u32 i = 0; i < t->w * t->h; i++)
      rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + ch - 1];
  struct tex *c = &twins[k];
  memset(c, 0, sizeof *c);
  c->used = 1;
  /* TWIN_ENV_A keeps its alpha (RGBA); the others are brightness textures */
  tex_brightness = ch != TWIN_ENV_A;
  tex_one_level = ch >= 1 && ch <= 3; /* the channels': drawn without MIPMAP filtering (cl_src) */
  tex_set_image(c, ch == TWIN_ENV_A ? GL_RGBA : GL_RGB, (GLsizei)t->w, (GLsizei)t->h, rgba);
  if (!c->data) {
    c->used = 0;
    return NULL;
  }
  t->twin[ch] = (u16)(k + 1);
  return c;
}

static struct tex *tex_twin(struct tex *t, int ch) {
  if (t->planar && ch == 1)
    return t; /* its own texels */
  if (!t->twin[ch]) {
    if (t->planar && ch > 1)
      return NULL; /* made with it (tex_set_image) */
    u8 *rgba = malloc(t->w * t->h * 4);
    if (!rgba)
      return NULL;
    for (u32 i = 0; i < t->w * t->h; i++) {
      u8 *o = rgba + i * 4;
      if (ch == TWIN_ENV_A) { /* white, its alpha */
        o[0] = o[1] = o[2] = 255, o[3] = (u8)tex_alpha_of(t, i);
        continue;
      }
      if (t->planar) { /* the brightness of its planes */
        const struct tex *g = &twins[t->twin[2] - 1], *b = &twins[t->twin[3] - 1];
        o[0] = TLV(t, 0)[i], o[1] = TLV(g, 0)[i], o[2] = TLV(b, 0)[i];
      } else
        tex_rgb(t, i, o);
      if (ch == TWIN_ENV_B) { /* the brightness times the alpha */
        u32 m = o[0] > o[1] ? o[0] : o[1], a = tex_alpha_of(t, i);
        m = o[2] > m ? o[2] : m;
        o[0] = o[1] = o[2] = (u8)((m * a + 127) / 255);
      }
      o[3] = 255;
    }
    struct tex *c = twin_make(t, ch, rgba);
    free(rgba);
    if (!c)
      return NULL;
  }
  struct tex *c = &twins[t->twin[ch] - 1];
  c->clamp_s = t->clamp_s, c->clamp_t = t->clamp_t;
  c->lin_mag = t->lin_mag, c->lin_min = t->lin_min, c->mip = t->mip;
  return c;
}
void APIENTRY glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                           GLint border, GLenum format, GLenum type, const GLvoid *px) {
  pq_flush();
  tex_serial++;
  tex_image_2d(target, level, ifmt, w, h, border, format, type, px);
}
static void tex_image_2d(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                                  GLint border, GLenum format, GLenum type, const GLvoid *px) {
  GLuint bound = TBOUND;
  if (!bound || bound >= MAX_TEX)
    return;
  if (target != GL_TEXTURE_2D || w < 0 || h < 0 || border) {
    gl_error(target != GL_TEXTURE_2D ? GL_INVALID_ENUM : GL_INVALID_VALUE);
    return;
  }
  /* luminance and intensity textures are kept as their brightness (T8, a
     CLUT that puts it in alpha too: the lightmaps, for the products with
     the destination colour), whatever the format of the texels given */
  if (ifmt == GL_LUMINANCE || ifmt == GL_LUMINANCE8 || ifmt == 1 || ifmt == GL_INTENSITY ||
      ifmt == GL_INTENSITY8 || (format == GL_LUMINANCE && (ifmt == 3 || ifmt == GL_RGB)))
    tex_brightness = 1;
  /* indices of the shared palette are kept as they are */
  if (level == 0 && format == GL_COLOR_INDEX && px && gl.shared_pal && pal_on && w >= 8 && h >= 8 &&
      !(w & (w - 1)) && !(h & (h - 1))) {
    tex_indexed = 1;
    format = GL_RGBA;
  }
  u8 *rgba = level == 0 && !tex_indexed
                 ? (format == GL_COLOR_INDEX ? index_rgba(w, h, px) : to_rgba(format, w, h, px))
                 : NULL;
  if (rgba) {
    tex_image_2d(target, level, ifmt == GL_COLOR_INDEX8_EXT ? GL_RGBA : ifmt, w, h, border, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    return;
  }
  if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
    tex_brightness = 0;
    if (px)
      gl_error(GL_INVALID_ENUM);
    if (px)
      return;
  }
  struct tex *t = &texs[bound];
  if (level > 0)
    return; /* made from level 0 */
  /* at least 8 x 8 (an IMAGE transfer of whole quadwords), sides of powers
     of two (the GS's): scaled */
  int pw = (int)pow2_ceil((u32)(w < 8 ? 8 : w)), ph = (int)pow2_ceil((u32)(h < 8 ? 8 : h));
  if (px && (w != pw || h != ph)) {
    int nw = pw, nh = ph;
    u8 *big = malloc((size_t)nw * nh * 4);
    if (!big)
      return;
    for (int y = 0; y < nh; y++)
      for (int x = 0; x < nw; x++)
        memcpy(big + (y * nw + x) * 4, (const u8 *)px + ((y * h / nh) * w + x * w / nw) * 4, 4);
    glTexImage2D(target, level, ifmt, nw, nh, border, format, type, big);
    free(big);
    return;
  }
  w = pw, h = ph;
  tex_drop_twin(t);
  tex_set_image(t, ifmt, w, h, px);
}

/* t's image, w x h RGBA8 texels s (NULL: zeros), at least 8 x 8 */
/* t's levels from its level 0 of indices: each texel the box of four, through
   the palette (a box of one index: that index, the usual case in a game's
   8-bit art); transparent when half of the box is */
static void index_levels(struct tex *t) {
  u32 w = t->w, h = t->h;
  if (t->max_levels < 2)
    return;
  u8 black[3] = {0, 0, 0};
  pal_nearest(black); /* pal_near made */
  if (!pal_near)
    return;
  for (u32 lv = 1; lv < t->max_levels; lv++) {
    u32 nw = w >> 1, nh = h >> 1;
    const u8 *up = TLV(t, lv - 1);
    u8 *dn = TLV(t, lv);
    for (u32 y = 0; y < nh; y++) {
      const u8 *p = up + 2 * y * w;
      for (u32 x = 0; x < nw; x++, p += 2) {
        u32 a = p[0], b = p[1], c = p[w], d = p[w + 1];
        if (a == b && a == c && a == d) {
          *dn++ = (u8)a;
          continue;
        }
        u32 r = 0, g = 0, bl = 0, n = 0, ix[4] = {a, b, c, d};
        for (int k = 0; k < 4; k++)
          if ((int)ix[k] != pal_transp) {
            const u8 *e = pal_rgb[ix[k]];
            r += e[0], g += e[1], bl += e[2], n++;
          }
        if (n < 2) {
          *dn++ = (u8)pal_transp;
          continue;
        }
        r /= n, g /= n, bl /= n;
        *dn++ = pal_near[r >> 3 | (g >> 3) << 5 | (bl >> 3) << 10];
      }
    }
    w = nw, h = nh;
  }
  t->levels = t->max_levels;
}

static void tex_set_image(struct tex *t, GLint ifmt, GLsizei w, GLsizei h, const GLvoid *px) {
  /* alpha in the texels? Only 0 and 255 (a colour key: Descent 3's 1555)
     fits 16 bits (the alpha bit, TEXA); other alpha takes 32 */
  int alpha = 0, alpha8 = 0, indexed = tex_indexed;
  const u8 *s = px;
  tex_indexed = 0;
  if (indexed) {
    for (int i = 0; i < w * h && !alpha; i++)
      alpha = s[i] == pal_transp;
  } else if (s && ifmt != GL_RGB && ifmt != GL_RGB8 && ifmt != GL_RGB5 && ifmt != 3)
    for (int i = 0; i < w * h; i++)
      if (s[i * 4 + 3] != 255) {
        alpha = 1;
        if (s[i * 4 + 3] != 0) {
          alpha8 = 1;
          break;
        }
      }
  tex_free_mem(t);
  t->w = (u32)w, t->h = (u32)h;
  t->lw = ilog2((u32)w), t->lh = ilog2((u32)h);
  t->alpha = alpha;
  /* mxgl_color_light: a GL_RGB8 texture without MIPMAP filtering (a
     lightmap) as its channels' brightness, each 8 bits: the red one its
     own texels, the green and blue ones copies (twin[2], twin[3]) */
  int planar = cl_enabled && (ifmt == GL_RGB8 || ifmt == GL_RGBA8) && !t->mip && s;
  if (planar)
    tex_brightness = 1, tex_one_level = 1;
  t->planar = (u8)planar;
  /* texels all of the game's palette (the transparent ones its transparent
     entry, if it has one): its indices (T8, exact), else 16 bits */
  int pal = indexed;
  if (!indexed && !tex_brightness && (!alpha || (pal_transp >= 0 && !alpha8)) && pal_on && s) {
    pal = 1;
    for (int i = 0; i < w * h && pal; i++)
      if (s[i * 4 + 3] && pal_find((u32)s[i * 4] | (u32)s[i * 4 + 1] << 8 | (u32)s[i * 4 + 2] << 16) < 0)
        pal = 0;
  }
  /* RGBA texels all grey (R = G = B) and opaque, or black with alpha 0
     (a lightmap block's unused texels, calloc'ed): the brightness texture
     (8 bits, not 16; one byte a texel to update, no brightness copy for
     the products with the destination: lightmaps sent as RGBA). Exact,
     but for those black texels outside a product: opaque there */
  int grey = 0;
  if (!pal && !indexed && !tex_brightness && !planar && !alpha8 && s) {
    /* its black texels all transparent (and some are): a font, kept with
       entry 0 transparent (CLUT_GREYK) */
    int clear = 0, black = 0;
    grey = 1;
    for (int i = 0; i < w * h && grey; i++) {
      const u8 *c = s + i * 4;
      grey = c[0] == c[1] && c[0] == c[2] && (c[3] == 255 || (c[3] == 0 && c[0] == 0));
      clear |= c[3] == 0, black |= c[3] == 255 && c[0] == 0;
    }
    if (grey && clear && !black)
      grey = 2;
    if (grey)
      tex_brightness = 1;
  }
  t->grey = (u8)grey;
  t->bright = tex_brightness;
  t->pal = pal;
  t->alpha_fmt = tex_brightness || ifmt == GL_RGBA || ifmt == 4 || ifmt == GL_RGBA8 ||
                 ifmt == GL_RGBA4 || ifmt == GL_RGB5_A1 || ifmt == GL_RGBA2 || ifmt == GL_ALPHA ||
                 ifmt == GL_ALPHA8 || ifmt == GL_LUMINANCE_ALPHA || ifmt == 2 ||
                 ifmt == GL_LUMINANCE8_ALPHA8 || ifmt == GL_INTENSITY || ifmt == GL_INTENSITY8;
  t->psm = tex_brightness || pal ? MX_GS_PSM_T8 : alpha8 ? MX_GS_PSM_CT32 : MX_GS_PSM_CT16;
  tex_brightness = 0;
  t->max_levels = tex_one_level ? 1 : mx_tex_max_levels(t->lw, t->lh, 8);
  tex_one_level = 0;
  t->levels = 1;
  tex_alloc_mem(t, 0); /* the DMAC reads it (IMAGE) */
  tex_new_gen(t); /* new texels: a new key */
  if (!t->data)
    return;
  if (planar) {
    /* its red plane, then the copies of green and blue */
    u8 *one = malloc((size_t)(w * h * 4));
    int ok = one != NULL;
    for (int ch = 1; ch <= 3 && ok; ch++) {
      memcpy(one, s, (size_t)(w * h * 4));
      if (ch == 1) {
        for (int i = 0; i < w * h; i++)
          one[i * 4 + 1] = one[i * 4 + 2] = one[i * 4];
        convert(t, 0, 0, 0, w, h, one);
      } else
        ok = twin_make(t, ch, one) != NULL;
    }
    free(one);
    if (!ok)
      gl_error(GL_OUT_OF_MEMORY);
  } else if (indexed) {
    memcpy(TLV(t, 0), s, (size_t)(w * h));
    index_levels(t);
  } else if (s) {
    convert(t, 0, 0, 0, w, h, s);
    make_levels(t, s);
  } else {
    for (u32 lv = 0; lv < t->max_levels; lv++)
      memset(TLV(t, lv), 0, mx_tex_level_bytes(t->psm, t->lw, t->lh, lv));
  }
  tex_writeback(t);
}

static void tex_sub(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const GLvoid *px);
static void tex_sub_l8(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const GLvoid *px);
static void tex_sub_ch(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const u8 *px);
void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                                     GLsizei h, GLenum format, GLenum type, const GLvoid *px) {
  pq_flush();
  GLuint bound = TBOUND;
  if (level != 0 || !bound || bound >= MAX_TEX)
    return;
  struct tex *t = &texs[bound];
  if (!t->data)
    return;
  /* GL_BLEND's and GL_ADD's copies: made again when drawn */
  tex_drop_twin_ch(t, TWIN_ENV_A);
  tex_drop_twin_ch(t, TWIN_ENV_B);
  /* bytes into a brightness texture (Quake's lightmaps, every frame): its
     texels as they are, without the RGBA8 copy */
  if ((format == GL_LUMINANCE || format == GL_INTENSITY) && px && t->psm == MX_GS_PSM_T8 &&
      !t->pal && !t->planar && !t->oclut && !t->twin[0] && !t->twin[1] && !t->twin[2] &&
      !t->twin[3] && x >= 0 && y >= 0 && w > 0 && h > 0 && (u32)(x + w) <= t->w &&
      (u32)(y + h) <= t->h) {
    tex_sub_l8(t, x, y, w, h, px);
    return;
  }
  u8 *rgba = to_rgba(format, w, h, px);
  if (rgba) {
    glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, type, rgba);
    free(rgba);
    return;
  }
  if (t->oclut) {
    gl_error(GL_INVALID_OPERATION); /* a compressed (paletted) texture */
    return;
  }
  if (x < 0 || y < 0 || w < 0 || h < 0 || (u32)(x + w) > t->w || (u32)(y + h) > t->h) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  if (!w || !h)
    return;
  /* changed in place, level 0 only: an upload of a frame in flight may take
     the new texels, a frame early (a copy each time: Quake's lightmaps
     change every frame). Only its lines go back from the cache, and to GS
     memory (tex_place) */
  /* its texels (planar: the red plane), its brightness and channel copies */
  u8 *one = NULL;
  for (int ch = -1; ch < 4; ch++) {
    if (ch < 0) {
      if (!t->planar)
        tex_sub(t, x, y, w, h, px);
      continue;
    }
    if (!t->twin[ch] && !(t->planar && ch == 1))
      continue;
    struct tex *c = t->planar && ch == 1 ? t : &twins[t->twin[ch] - 1];
    const u8 *src = px;
    /* a channel into a T8 copy (the planes of coloured light): its bytes
       as they are */
    if (ch && c->psm == MX_GS_PSM_T8 && !c->pal) {
      tex_sub_ch(c, x, y, w, h, (const u8 *)px + ch - 1);
      continue;
    }
    if (ch && (one || (one = malloc((size_t)(w * h * 4))))) {
      for (int i = 0; i < w * h; i++)
        one[i * 4] = one[i * 4 + 1] = one[i * 4 + 2] = ((const u8 *)px)[i * 4 + ch - 1];
      src = one;
    } else if (ch) {
      tex_drop_twin(t); /* no memory to convert: made again when drawn */
      break;
    }
    tex_sub(c, x, y, w, h, src);
  }
  free(one);
}

/* lines y .. y + h - 1 of t's level 0 changed: written back, marked for
   tex_place (which uploads them alone); level 0 only from now on */
static void tex_sub_mark(struct tex *t, GLint y, GLsizei h) {
  t->levels = 1;
  u32 rb = t->w * tex_bpp(t);
  mx_dcache_writeback(TLV(t, 0) + (u32)y * rb, (u32)h * rb);
  if (!t->dirty)
    t->dy0 = (u32)y, t->dy1 = (u32)(y + h);
  else
    t->dy0 = (u32)y < t->dy0 ? (u32)y : t->dy0, t->dy1 = (u32)(y + h) > t->dy1 ? (u32)(y + h) : t->dy1;
  t->dirty = 1;
}

/* RGBA8 texels px into t's level 0 at (x, y) */
static void tex_sub(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const GLvoid *px) {
  convert(t, 0, x, y, w, h, px);
  tex_sub_mark(t, y, h);
}

/* every fourth byte from px (a channel of RGBA8 texels) into the T8
   texture t's level 0 at (x, y) */
static void tex_sub_ch(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const u8 *px) {
  u8 *dst = TLV(t, 0) + (u32)y * t->w + (u32)x;
  for (int r = 0; r < h; r++, dst += t->w)
    for (int i = 0; i < w; i++, px += 4)
      dst[i] = *px;
  tex_sub_mark(t, y, h);
}

/* bytes px into the T8 texture t's level 0 at (x, y) */
static void tex_sub_l8(struct tex *t, GLint x, GLint y, GLsizei w, GLsizei h, const GLvoid *px) {
  u8 *dst = TLV(t, 0) + (u32)y * t->w + (u32)x;
  const u8 *src = px;
  for (int r = 0; r < h; r++, dst += t->w, src += w)
    memcpy(dst, src, (size_t)w);
  tex_sub_mark(t, y, h);
}

void APIENTRY glTexParameterf(GLenum target, GLenum p, GLfloat v) {
  pq_flush();
  tex_serial++;
  GLuint bound = TBOUND;
  if (!bound || bound >= MAX_TEX)
    return;
  struct tex *t = &texs[bound];
  GLenum e = (GLenum)v;
  switch (p) {
  case GL_TEXTURE_WRAP_S: t->clamp_s = e != GL_REPEAT; break;
  case GL_TEXTURE_WRAP_T: t->clamp_t = e != GL_REPEAT; break;
  case GL_TEXTURE_MAG_FILTER: t->lin_mag = e != GL_NEAREST; break;
  case GL_TEXTURE_MIN_FILTER: {
    int mip = e == GL_NEAREST || e == GL_LINEAR                                  ? 0
              : e == GL_NEAREST_MIPMAP_NEAREST || e == GL_LINEAR_MIPMAP_NEAREST ? 1
                                                                                 : 2;
    t->lin_min = !(e == GL_NEAREST || e == GL_NEAREST_MIPMAP_NEAREST || e == GL_NEAREST_MIPMAP_LINEAR);
    t->mip = mip;
    break;
  }
  /* the levels are made here from level 0 anyway */
  case GL_GENERATE_MIPMAP: break;
  case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_MAX_LOD: case GL_TEXTURE_BASE_LEVEL:
  case GL_TEXTURE_MAX_LEVEL: case GL_TEXTURE_PRIORITY: break;
  default: gl_error(GL_INVALID_ENUM); break;
  }
}
void APIENTRY glTexParameteri(GLenum target, GLenum p, GLint v) {
  glTexParameterf(target, p, (GLfloat)v);
}
