/* gl/vram.c, a part of mxgl.c (included there, not compiled alone): textures in GS memory: image
   transfers, the CLUTs, the shared palette */

/* ---- textures in GS memory --------------------------------------------- */

static u32 pow2_ceil(u32 v) {
  u32 p = 1;
  while (p < v)
    p <<= 1;
  return p;
}

static u32 tex_bpp(const struct tex *t) {
  return t->psm == MX_GS_PSM_CT32 ? 4 : t->psm == MX_GS_PSM_T8 ? 1 : 2;
}

/* the levels a draw reads: all that came, if the filter uses levels */
static u32 tex_levels(const struct tex *t) { return t->levels; }

/* an IMAGE transfer of w x h texels of psm at dbp (FLUSH first if flush):
   a DIRECT of BITBLTBUF, TRXPOS, TRXREG, TRXDIR; then per piece a DIRECT
   of its IMAGE tag and a ref tag whose own DIRECT sends the texels (a
   DIRECT must not go on past a tag: the next tag's upper half is VIFcodes) */
static void image_xfer_at(u32 dbp, u32 dbw, u32 psm, u32 y, u32 w, u32 h, u32 bpp,
                          const void *data, int flush);
static void image_xfer(u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, u32 bpp, const void *data,
                       int flush) {
  image_xfer_at(dbp, dbw, psm, 0, w, h, bpp, data, flush);
}
/* the same into lines y .. y + h - 1 */
static void image_xfer_at(u32 dbp, u32 dbw, u32 psm, u32 y, u32 w, u32 h, u32 bpp,
                          const void *data, int flush) {
  flush_out();
  u32 qwc = w * h * bpp / 16;
  st_cur.upload_kb += qwc / 64;
  u64 *c = mx_frame_reserve(6 + 3 * (qwc / 0x7FFF + 1));
  const u8 *d = data;
  c = mx_e_cnt(c, 5, flush && GS_DREW ? mx_vif_flush() : mx_vif_nop(), mx_vif_direct(5));
  /* the batches before are drawn: no FLUSH needed before the next
     register writes, unless this went into the once record, apart from
     the main record's batches */
  if (flush && !in_once)
    gs_drew = 0;
  c = mx_e_ad_tag(c, 4, 0);
  c = mx_e_ad(c, MX_GS_REG_BITBLTBUF, mx_gs_bitbltbuf(0, 0, 0, dbp, dbw, psm));
  c = mx_e_ad(c, MX_GS_REG_TRXPOS, mx_gs_trxpos(0, 0, 0, y, 0));
  c = mx_e_ad(c, MX_GS_REG_TRXREG, mx_gs_trxreg(w, h));
  c = mx_e_ad(c, MX_GS_REG_TRXDIR, 0);
  while (qwc) {
    u32 n = qwc > 0x7FFF ? 0x7FFF : qwc;
    c = mx_e_cnt(c, 1, mx_vif_nop(), mx_vif_direct(1));
    c = mx_e_giftag(c, n, n == qwc, -1, MX_GIF_FLG_IMAGE, 0, 0);
    c = mx_e_ref(c, d, n, mx_vif_nop(), mx_vif_direct(n));
    d += n * 16;
    qwc -= n;
  }
  mx_frame_commit(c);
}

static void texflush(void) {
  flush_out();
  u64 *c = mx_frame_reserve(3);
  c = mx_e_cnt(c, 2, 0, mx_vif_direct(2));
  c = mx_e_ad_tag(c, 1, 1);
  c = mx_e_ad(c, MX_GS_REG_TEXFLUSH, 0);
  mx_frame_commit(c);
}

/* the CLUTs of the brightness textures (T8): entry i is grey i, and in
   alpha (0x80 is 1.0 in the blend Cd * As): i (up to 2x, for overbright),
   i / 2 (1x), (255 - i) / 2 (1x of the inverse, blendFunc ZERO
   ONE_MINUS_SRC_COLOR), 1.0 (CLUT_GREY: a grey RGBA texture drawn other
   than in a product, as GL draws it); CLUT_ENV: white (the vertex colour
   as it is in a modulate) with i / 2 in alpha, for GL_BLEND's second pass
   (draw_env); in
   CSM1's order (bits 3 and 4 of the index swapped), each 16 x 16 CT32 (4
   blocks) */
/* CLUT_GREYK: CLUT_GREY with entry 0 transparent (a grey texture whose
   black texels are all transparent: a font) */
enum { CLUT_2X, CLUT_1X, CLUT_INV, CLUT_GREY, CLUT_ENV, CLUT_GREYK, NCLUT };
static u32 clut_cbp;
static u32 clut[NCLUT][256] MX_ALIGNED(64) MX_CORE_BSS; /* sent to the GS */

/* the game's palette as a CLUT (mxgl_Palette), after the brightness CLUTs */
static u32 clut_pal[256] MX_ALIGNED(64);
static int pal_on;
static u8 pal_rgb[256][3];
/* the shared palette with alpha (GL_COLOR_INDEX textures) */
static u8 pal_rgba[256][4];
/* the entry of a shared palette that is transparent (glColorTableEXT: its
   alpha 0), or -1: textures keyed with it are 8 bits too */
static int pal_transp = -1;
/* exact RGB -> index (open addressing, key RGB + 1), and the nearest index
   of a 15-bit colour (for the averaged colours of mipmap levels) */
static u32 pal_key[1024];
static u8 pal_idx[1024];
static u8 *pal_near;

static int pal_find(u32 rgb) {
  u32 h = (rgb * 2654435761u) >> 22;
  for (u32 i = 0; i < 1024; i++, h = (h + 1) & 1023) {
    if (pal_key[h] == rgb + 1)
      return pal_idx[h];
    if (!pal_key[h])
      return -1;
  }
  return -1;
}

static u8 pal_nearest(const u8 *c) {
  if (!pal_near) {
    pal_near = malloc(32768);
    if (!pal_near)
      return 0;
    for (u32 k = 0; k < 32768; k++) {
      int r = (int)((k & 31) << 3 | 4), g = (int)((k >> 5 & 31) << 3 | 4), b = (int)((k >> 10) << 3 | 4);
      u32 best = ~0u;
      u8 bi = 0;
      for (int i = 0; i < 256; i++) {
        if (i == pal_transp)
          continue;
        int dr = r - pal_rgb[i][0], dg = g - pal_rgb[i][1], db = b - pal_rgb[i][2];
        u32 d = (u32)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
        if (d < best)
          best = d, bi = (u8)i;
      }
      pal_near[k] = bi;
    }
  }
  int e = pal_find((u32)c[0] | (u32)c[1] << 8 | (u32)c[2] << 16);
  return e >= 0 ? (u8)e : pal_near[c[0] >> 3 | (c[1] >> 3) << 5 | (c[2] >> 3) << 10];
}

void mxgl_Palette(const unsigned char *rgb) {
  memset(pal_key, 0, sizeof pal_key);
  free(pal_near);
  pal_near = NULL;
  for (int i = 0; i < 256; i++) {
    const u8 *c = rgb + 3 * i;
    memcpy(pal_rgb[i], c, 3);
    memcpy(pal_rgba[i], c, 3);
    pal_rgba[i][3] = 255;
    u32 key = (u32)c[0] | (u32)c[1] << 8 | (u32)c[2] << 16;
    if (i != pal_transp && pal_find(key) < 0) {
      u32 h = (key * 2654435761u) >> 22;
      while (pal_key[h])
        h = (h + 1) & 1023;
      pal_key[h] = key + 1;
      pal_idx[h] = (u8)i;
    }
    /* CSM1's order: bits 3 and 4 of the index swapped */
    u32 at = ((u32)i & 0xE7) | ((u32)i & 8) << 1 | ((u32)i & 16) >> 1;
    clut_pal[at] = key | (i == pal_transp ? 0 : 0x80u << 24);
  }
  mx_dcache_writeback(clut_pal, sizeof clut_pal);
  pal_on = 1;
  /* a frame being made: its draws from now on take the new one (the CLUTs
     are sent at each frame's start) */
  if (in_frame) {
    flush_out();
    clut_send();
    gs.valid = 0, gs_serial++;
  }
}

/* CSM1's order of a CLUT's entry i: bits 3 and 4 swapped */
static inline u32 csm1(u32 i) { return (i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1; }

static void clut_make(void) {
  for (u32 i = 0; i < 256; i++) {
    u32 at = (i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1, g = i | i << 8 | i << 16;
    clut[CLUT_2X][at] = g | i << 24;
    clut[CLUT_1X][at] = g | ((i + 1) >> 1) << 24;
    clut[CLUT_INV][at] = g | ((256 - i) >> 1) << 24;
    clut[CLUT_GREY][at] = g | 0x80u << 24;
    clut[CLUT_ENV][at] = 0xFFFFFFu | ((i + 1) >> 1) << 24;
    clut[CLUT_GREYK][at] = g | (i ? 0x80u : 0u) << 24;
  }
  mx_dcache_writeback(clut, sizeof clut);
}

/* beam: texture uploads go into the frame's once record (played once,
   not in every pass), unless GS memory was refilled in this frame: then
   their order among the draws matters */
static void once_on(void) {
#ifdef MXGL_NO_ONCE /* debugging: beam racing's uploads in the main record */
  return;
#endif
  if (!beam)
    return;
  flush_out();
  mx_hires_once(1);
  in_once = 1;
}
static void once_off(void) {
#ifdef MXGL_NO_ONCE
  return;
#endif
  if (beam)
    mx_hires_once(0);
  in_once = 0;
}

static void clut_send(void) {
  once_on();
  for (int k = 0; k < NCLUT; k++)
    image_xfer(clut_cbp + 4 * (u32)k, 1, MX_GS_PSM_CT32, 16, 16, 4, clut[k], k == 0);
  if (pal_on)
    image_xfer(clut_cbp + 4 * NCLUT, 1, MX_GS_PSM_CT32, 16, 16, 4, clut_pal, 0);
  texflush();
  once_off();
}

/* the texture quality (mxgl_texture_quality): the levels left out above
   (0 .. 2) of textures drawn with MIPMAP filtering */
static s32 k_lod; /* the LOD constant of the draw being made (prepare_state) */
