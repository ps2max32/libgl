/* gl/mode.c, a part of mxgl.c (included there, not compiled alone): set-up: the buffers and
   modes, mxgl_init, the window, a frame's memory, the screenshot */

/* ---- set-up ----------------------------------------------------------- */


static int mode_set = -1; /* mxgl_mode: none yet */
static int mode_want;     /* the flags asked for with it */

/* without beam racing: video mode m, two CT16S buffers of w x h and a Z
   buffer of zpsm */
static int start_buffers(const struct mx_video_mode *m, int w, int h, u32 zpsm) {
  if (mx_gs_init_video(m) < 0)
    return -1;
  fb_w = w, fb_h = h;
  u32 zbp = mx_vram_alloc(mx_rt_pages((u32)w, (u32)h, zpsm));
  for (int j = 0; j < 2; j++) {
    u32 fbp = mx_vram_alloc(mx_rt_pages((u32)w, (u32)h, MX_GS_PSM_CT16S));
    mx_rt_init(&rt[j], fbp, (u32)w, (u32)h, MX_GS_PSM_CT16S, zbp, zpsm);
    mx_display_init(&disp[j], &rt[j]);
  }
  mx_gs_displays(disp, 2);
  mx_vu_reset(2);
  /* chain mode: the stream calls the display lists' chains */
  mx_frame_init_chain(ring, sizeof ring, (void *)0x70002000, 0x2000);
  /* frames as fast as they are made (the screen tears): the real rate */
  mx_frame_vsync(0);
  beam = rgb24 = 0;
  zmax = zpsm == MX_GS_PSM_Z16S || zpsm == MX_GS_PSM_Z16 ? ZMAX_Z16 : ZMAX_Z24;
  return 0;
}

/* beam racing (maxsdk's passes, <mx/hires.h>): video mode m in interlaced
   FRAME mode, one buffer of w x h (a field: both fields' lines, half a line
   apart) in psm, Z (16 bits; Z32 for 32-bit colour) in the passes' strips;
   the records of the frame stream (each played every field until the next
   replaces it) and arenas 2, 3 are taken while it is on */
static int start_beam(const struct mx_video_mode *m, int w, int h, u32 psm) {
  if (!hires_mem)
    hires_mem = mx_heap_alloc(gl_stream, REC_BYTES + 2 * ARENA_BYTES, 64);
  if (!hires_mem)
    return -1;
  mx_vu_reset(2);
  if (mx_hires_init(m, (u32)w, (u32)h, psm, hires_mem, REC_BYTES, (void *)0x70002000, 0x2000) < 0)
    return -1;
  fb_w = w, fb_h = h;
  for (int i = 2; i < NARENA; i++) {
    arena[i] = hires_mem + REC_BYTES + (u32)(i - 2) * ARENA_BYTES;
    mx_dcache_invalidate(arena[i], ARENA_BYTES);
  }
  beam = 1;
  rgb24 = psm == MX_GS_PSM_CT32 || psm == MX_GS_PSM_CT24;
  zmax = rgb24 ? ZMAX_Z24 : ZMAX_Z16;
  return 0;
}

static void hires_free(void) {
  mx_heap_free(gl_stream, hires_mem);
  hires_mem = NULL;
  arena[2] = arena[3] = NULL;
}


/* the modes: the video mode, the picture (beam racing's buffer: a field's
   lines when interlaced), whether 32-bit colour fits with beam racing, and
   the buffers without it (shown stretched when smaller than the picture) */
static const struct {
  const struct mx_video_mode *m;
  short w, h, bw, bh;    /* the picture, beam racing's buffer */
  short rgb24;           /* CT24 fits */
  short fw, fh;          /* two buffers without beam racing */
  const char *name;
} modes[] = {
    [MXGL_NTSC] = {&mx_video_ntsc, 640, 448, 640, 224, 1, MXGL_FB_W, 448, "NTSC"},
    [MXGL_PAL] = {&mx_video_pal, 640, 512, 640, 256, 1, MXGL_FB_W, 512, "PAL"},
    [MXGL_1080I] = {&mx_video_1080i, 1920, 1080, 1920, 540, 0, 640, 540, "1080i"},
    [MXGL_480P] = {&mx_video_480p, 640, 480, 640, 480, 1, 640, 480, "480p"},
    [MXGL_576P] = {&mx_video_576p, 640, 576, 640, 576, 1, 640, 512, "576p"},
    [MXGL_720P] = {&mx_video_720p, 1280, 720, 1280, 720, 0, 640, 360, "720p"},
    [MXGL_720P50] = {&mx_video_720p50, 1280, 720, 1280, 720, 0, 640, 360, "720p50"},
    [MXGL_1080I50] = {&mx_video_1080i50, 1920, 1080, 1920, 540, 0, 640, 540, "1080i50"},
    [MXGL_1080P30] = {&mx_video_1080p30, 1920, 1080, 1920, 540, 0, 640, 540, "1080p30"},
};
#define NMODES ((int)(sizeof modes / sizeof modes[0]))

/* a mode's video mode for a buffer shown by both fields (interlaced FRAME
   mode) or stretched */
static struct mx_video_mode video_frame;

static int mode_flags; /* MXGL_BEAM, MXGL_RGB24 in effect */

/* the mode's picture: with beam racing if asked (and memory allows), else
   two buffers; returns 0, or -1 for beam racing not set */
static int start_mode(int mode, int flags) {
  const struct mx_video_mode *vm = modes[mode].m;
  int sd = mode == MXGL_NTSC || mode == MXGL_PAL;
  if (flags & MXGL_BEAM) {
    /* 32-bit colour where it fits (SD, 480p, 576p) */
    u32 psm = flags & MXGL_RGB24 && modes[mode].rgb24 ? MX_GS_PSM_CT24 : MX_GS_PSM_CT16S;
    if (start_beam(vm, modes[mode].bw, modes[mode].bh, psm) == 0) {
      mode_flags = MXGL_BEAM | (rgb24 ? MXGL_RGB24 : 0);
      scr_w = modes[mode].w, scr_h = modes[mode].h;
      return 0;
    }
    mx_printf("mxgl: no beam racing (memory %p)\n", hires_mem);
  }
  hires_free();
  mode_flags = 0;
  int ok;
  if (sd) {
    ok = start_buffers(vm, modes[mode].fw, modes[mode].fh, MXGL_FB_Z) == 0;
    scr_w = fb_w, scr_h = fb_h;
  } else {
    video_frame = *vm;
    if (video_frame.interlace)
      video_frame.ffmd = 1;
    /* Z16: room for textures */
    ok = start_buffers(&video_frame, modes[mode].fw, modes[mode].fh, MX_GS_PSM_Z16S) == 0;
    scr_w = modes[mode].w, scr_h = modes[mode].h;
  }
  if (!ok)
    return -2; /* the mode does not run here */
  return flags & MXGL_BEAM ? -1 : 0;
}

void mxgl_dither(const unsigned long long *dimx, int n) {
  n = n < 0 ? 0 : n > 16 ? 16 : n;
  for (int i = 0; i < n; i++)
    dither_m[i] = dimx[i];
  dither_n = (u32)n;
  mx_hires_dither(dither_m, dither_n);
}

int mxgl_set_mode(int mode) {
  int flags = mode & (MXGL_BEAM | MXGL_RGB24);
  mode &= 0xFF;
  if (mode == MXGL_SD || mode >= NMODES)
    mode = mx_video_default() == &mx_video_pal ? MXGL_PAL : MXGL_NTSC;
  if (mode == mode_set && flags == mode_want)
    return 0;
  if (mode_set >= 0) {
    /* the frames sent are shown (two buffers) or the passes stopped (beam
       racing), then the pipeline starts again in the new mode */
    pq_flush();
    if (in_frame)
      mxgl_end_frame();
    u32 t0 = mx_count();
    while (!beam && mx_gs.shown != mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
      ;
    mx_frame_stop();
    /* no frame reads anything now: what was freed goes (the room for
       beam racing, after a game freed its textures for a mode change) */
    for (int k = 0; k < NDEFER; k++) {
      for (int i = 0; i < ndeferred[k]; i++)
        gl_free(deferred[k][i]);
      ndeferred[k] = 0;
    }
  }
  int r = start_mode(mode, flags);
  if (r == -2) {
    /* the mode does not run (on this console, or this kernel): the
       console's own, as for MXGL_SD */
    int sd = mx_video_default() == &mx_video_pal ? MXGL_PAL : MXGL_NTSC;
    mx_printf("mxgl: %s does not run here; %s\n", modes[mode].name, modes[sd].name);
    mode = sd;
    start_mode(mode, flags);
    r = -1;
  }
  mode_set = mode, mode_want = flags;
  frame_par = 0;
  mx_printf("mxgl: %s %ux%u%s%s, buffer %ux%u\n", modes[mode].name, scr_w, scr_h,
            beam ? ", beam racing" : "", rgb24 ? ", 24-bit colour" : "", fb_w, fb_h);
  /* the HUD's font (256 x 8) in the first page after the buffers, the
     CLUTs after it (pages of libgl's own), then the textures (the cache,
     in the pages left): all copied in again */
  mx_gs.w = (u32)fb_w, mx_gs.h = (u32)fb_h; /* the HUD's coordinates */
  mx_hud_init(mx_gs.tbp, (u32)fb_h);
  clut_cbp = mx_gs.tbp + mx_tex_blocks(256, 8, MX_GS_PSM_CT32);
  vram_lo = clut_cbp + 4 * (NCLUT + 1); /* and the palette's */
  u32 p0 = mx_gs.tbp / 32, p1 = (vram_lo + 31) / 32;
  if (mx_vram_take(p0, p1 - p0, MX_VRAM_APP) < 0)
    mx_printf("mxgl: GS memory for the font and CLUTs taken\n");
  /* the pages below the buffers count too (beam racing's start high: mx_hires_init) */
  vram_lo = (p1 - (beam ? mx_gs.fbp[0] : 0)) * 32;
  /* beam racing: uploads go in the once records (PATH3), ahead of the
     passes of the frames before: only what the GS is done with goes */
#ifndef MXGL_VCACHE_POLICY
#define MXGL_VCACHE_POLICY MX_VCACHE_AUTO
#endif
  mx_vcache_init(0, beam ? MX_VCACHE_ASYNC : MX_VCACHE_STREAM, MXGL_VCACHE_POLICY);
  /* the top bytes no drawing writes (24-bit colour strips, a Z24 buffer):
     8-bit textures as T8H (the cache gives mt.psm) */
  u32 hp = 0, hn = 0;
#ifndef MXGL_NO_HBYTE /* debugging: no textures in top bytes */
  if (beam)
    hn = mx_hires_hbyte(&hp);
  else if (rt[0].zpsm == MX_GS_PSM_Z24)
    hp = rt[0].zbp, hn = mx_rt_pages((u32)fb_w, (u32)fb_h, MX_GS_PSM_Z24);
#endif
  mx_vcache_hpages(hp, hn);
  h_blocks = hn * 32;
  vram_gen++;
  gs.valid = 0, gs_serial++;
  mxgl_window(scr_w, scr_h);
  return r;
}

int mxgl_mode_flags(void) { return mode_flags; }

int mxgl_mode(void) { return mode_set; }



void mxgl_screen(int *w, int *h) {
  *w = scr_w;
  *h = scr_h;
}

void mxgl_init(void) {
  static int done;
  if (done)
    return;
  done = 1;
  gl_heap = mx_heap_create("gl", MX_MEM_PINNED, 0);
  gl_stream = mx_heap_create("gl.stream", MX_MEM_STREAM, 0);
  if (mx_vm_active())
    gl_assets = mx_heap_create("gl.textures", MX_MEM_ASSET, GL_ASSET_BYTES);
#if MXGL_VU1CLIP
  shader = MX_VU_SHADER(MXGLC, mxglc_vu, mxglc_vu_end);
#else
  shader = MX_VU_SHADER(MXGLV, mxglv_vu, mxglv_vu_end);
#endif
#if MXGL_VU1CLIP
  struct mx_shader fs = MX_VU_SHADER(MXGLCF, mxglcf_vu, mxglcf_vu_end);
  fog_program(0, &fs);
  fs = MX_VU_SHADER(MXGLCE, mxglce_vu, mxglce_vu_end);
  fog_program(1, &fs);
#else
  struct mx_shader fs = MX_VU_SHADER(MXGLVF, mxglvf_vu, mxglvf_vu_end);
  fog_program(0, &fs);
  fs = MX_VU_SHADER(MXGLVE, mxglve_vu, mxglve_vu_end);
  fog_program(1, &fs);
#endif
  for (int i = 0; i < 2; i++) {
    arena[i] = mx_heap_alloc(gl_stream, ARENA_BYTES, 64);
    /* written through UCAB only: no line of them may be in the cache */
    if (arena[i])
      mx_dcache_invalidate(arena[i], ARENA_BYTES);
  }
  if (!arena[1])
    mx_printf("mxgl: no memory for the arenas (2 x %u KB)\n", ARENA_BYTES / 1024);
  clut_make();
  mxgl_set_mode(MXGL_SD);
  memset(&gl, 0, sizeof gl);
  for (int i = 0; i < MV_DEPTH; i++)
    mat_ident(gl.mv[i]);
  for (int i = 0; i < PR_DEPTH; i++)
    mat_ident(gl.pr[i]);
  for (int i = 0; i < TM_DEPTH; i++)
    mat_ident(gl.tm[i]);
  gl.pack_align = 4;
  gl.mode = GL_MODELVIEW;
  gl.vp[2] = gl_w, gl.vp[3] = gl_h; /* the window of mxgl_set_mode */
  gl.df = 1;
  gl.fog_mode = GL_EXP, gl.fog_density = 1, gl.fog_end = 1; /* GL's defaults */
  gl.fog_a = 1;
  gl.sf = GL_ONE, gl.dfac = GL_ZERO;
  gl.dfunc = GL_LESS, gl.dmask = 1, gl.cullface = GL_BACK, gl.afunc = GL_ALWAYS;
  gl.st_func = GL_ALWAYS, gl.st_op = GL_KEEP, gl.st_mask = ~0u, gl.st_wmask = ~0u, gl.st_vb = -1;
  gl.texenv = GL_MODULATE;
  gl.color[0] = gl.color[1] = gl.color[2] = gl.color[3] = 1;
  gl.col8 = 0xFFFFFFFFu;

}

void mxgl_window(int w, int h) {
  pq_flush();
  gl_w = w, gl_h = h;
  gl.vp[2] = w, gl.vp[3] = h;
  mat_serial++;
}

void mxgl_frame_done(void) {
  defer_collect();
  if (dropped_draws) {
    mx_printf("mxgl: %u draws dropped (arena full)\n", dropped_draws);
    dropped_draws = 0;
  }
}

#ifdef MXGL_CHECK_TEX
/* debugging (the pipeline stopped): every resident texture's level 0 in
   GS memory against its texels in RAM; and the CLUTs at clut_cbp */
static void check_textures(void) {
  static u8 buf[128 * 128 * 4] MX_ALIGNED(64);
  u32 nt = 0, nbad = 0;
  for (u32 i = 1; i < MAX_TEX; i++) {
    struct tex *t = &texs[i];
    if (!t->used || !t->data)
      continue;
    struct mx_tex m;
    u32 f = t->m.from;
    if (!mx_vcache_peek(mx_vcache_tex_key(tex_key(t), f), &m))
      continue;
    u32 bytes = (t->w >> f) * (t->h >> f) * tex_bpp(t);
    if (f)
      mx_printf("check: tex %u %ux%u from level %u (levels %u, tail %u, mip %d, pal %d): %ux%u at %u, "
                "TEX0 tw %u th %u levels %u\n", i, t->w, t->h, f, t->m.levels, t->m.tail, t->mip,
                t->pal, t->w >> f, t->h >> f, m.tbp[0], m.tw, m.th, m.levels);
    if (bytes % 128 || bytes > sizeof buf)
      continue;
    nt++;
    if (mx_gs_download(buf, m.tbp[0], m.tbw[0], m.psm, 0, 0, t->w >> f, t->h >> f) < 0)
      continue;
    u32 bad = 0;
    for (u32 k = 0; k < bytes; k++)
      bad += buf[k] != TLV(t, f)[k];
    if (bad && nbad++ < 20)
      mx_printf("check: tex %u %ux%u psm %u bright %d pal %d at %u: %u of %u bytes wrong\n", i, t->w,
                t->h, m.psm, t->bright, t->pal, m.tbp[0], bad, bytes);
  }
  mx_printf("check: %u textures, %u wrong\n", nt, nbad);
  static u32 cl[256] MX_ALIGNED(64);
  for (int k = 0; k <= NCLUT; k++) {
    if (mx_gs_download(cl, clut_cbp + 4 * (u32)k, 1, MX_GS_PSM_CT32, 0, 0, 16, 16) < 0)
      continue;
    const u32 *want = k < NCLUT ? clut[k] : clut_pal;
    u32 bad = 0;
    for (int j = 0; j < 256; j++)
      bad += cl[j] != want[j];
    mx_printf("check: CLUT %d: %u of 256 entries wrong (first %08x, want %08x)\n", k, bad, cl[0], want[0]);
  }
}
#endif

int mxgl_screenshot(const char *path) {
  /* beam racing: both fields (1920 x 1080, 640 x 448 / 512) */
  if (beam) {
    int r = mx_hires_save_tga(path);
#ifdef MXGL_CHECK_TEX
    check_textures();
#endif
    return r;
  }
  /* the last frame on the screen, then the pipeline stopped (as the
     tutorials' tut_screenshot); its CT16S buffer as a 24-bit TGA */
  u32 t0 = mx_count();
  while (mx_gs.shown != mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
    ;
  mx_frame_stop();
  const struct mx_rt *t = &rt[(mx_gs.frames - 1) & 1];
  static u16 line[640] MX_ALIGNED(64);
  static u8 out[640 * 3];
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0)
    return -1;
  u8 hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (u8)fb_w, (u8)(fb_w >> 8), (u8)fb_h, (u8)(fb_h >> 8),
                24, 0x20};
  mx_fio_write(fd, hdr, sizeof hdr);
  for (u32 y = 0; y < (u32)fb_h; y++) {
    if (mx_gs_download(line, MX_GS_PAGE(t->fbp), t->fbw, t->psm, 0, y, (u32)fb_w, 1) < 0)
      break;
    for (u32 x = 0; x < (u32)fb_w; x++) {
      u16 p = line[x];
      out[x * 3 + 0] = (u8)(((p >> 10) & 31) << 3);
      out[x * 3 + 1] = (u8)(((p >> 5) & 31) << 3);
      out[x * 3 + 2] = (u8)((p & 31) << 3);
    }
    mx_fio_write(fd, out, (u32)fb_w * 3);
  }
  mx_fio_close(fd);
  return 0;
}
