/* gl/frame.c, a part of mxgl.c (included there, not compiled alone): the frame stream: a frame's
   start, the counters on the screen and in the log, its end, register writes */

/* ---- the stream ---------------------------------------------------------- */

static void clut_send(void);
static void flush_out(void);
static void batch_place(void);
static u32 s_at[3];

static void frame_start(void) {
  if (in_frame)
    return;
  u64 *e;
  if (beam) {
    /* the passes set the target and clear the strips; the arena is the
       record's (kept while the record is played) */
    mx_frame_begin_n();
    frame_par = (int)mx_hires_slot();
    mx_hires_clear((u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255), (u32)(gl.clear[2] * 255));
    /* TEXFLUSH: textures uploaded by PATH3 since (the once records) */
    e = mx_frame_direct(6);
    e = mx_e_ad_tag(e, 5, 1);
    e = mx_e_ad(e, MX_GS_REG_TEXFLUSH, 0);
  } else {
    buf = (int)(mx_frame_begin_n() & 1);
    mx_frame_target(&rt[buf], 0);
    frame_par ^= 1;
    /* TEST off for the clear, PRMODECONT, COLCLAMP, dithering into 16 bits */
    e = mx_frame_direct(6 + 5);
    e = mx_e_ad_tag(e, 5, 0);
    e = mx_e_ad(e, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  }
  e = mx_e_ad(e, MX_GS_REG_PRMODECONT, 1);
  e = mx_e_ad(e, MX_GS_REG_COLCLAMP, 1);
  /* the dither matrix: the manual's 4 x 4 or mxgl_dither's, offsets -4 to
     3, a whole step of 5 bits (8 of 255), so every shade between two
     dithers. The GS then keeps the top 5 bits: a write is 4 of 255 darker
     on average (8 bits would need offsets 0 to 7; DIMX has -4 to 3). With
     beam racing and mxgl_dither the passes write it, per field: none here */
  if (beam && dither_n)
    e = mx_e_ad(e, 0x7F, 0); /* SCE_GS_NOP */
  else
    e = mx_e_ad(e, MX_GS_REG_DIMX, dither_n ? dither_m[dither_frame++ % dither_n] : 0x6071243571603524ull);
  e = mx_e_ad(e, MX_GS_REG_DTHE, !rgb24); /* not defined into 32 bits */
  if (!beam)
    e = mx_rt_e_clear(e, &rt[buf], 0, (u32)(gl.clear[0] * 255), (u32)(gl.clear[1] * 255),
                      (u32)(gl.clear[2] * 255), 1);
  mx_frame_commit(e);
  in_frame = 1;
  gs_drew = 1;
  mx_vcache_frame(mx_frame_number(), mx_frame_done());
#ifdef MXGL_VCACHE_CHECK /* debugging: the cache's places checked every frame */
  {
    int mx__vcache_check(void);
    static int told;
    if (!told && mx__vcache_check())
      told = 1, mx_printf("mxgl: the cache's places are wrong at frame %u\n", mx_frame_number());
  }
#endif
  arena_pos = 0;
  s_at[0] = s_at[1] = s_at[2] = 0;
  batch_place();
  gs.valid = 0, gs_serial++;
  mx_frame_shader_setup(&shader);
  vu_cur = &shader;
  /* TEXA: CT16 alpha bit 1 is 0x80, 0 is 0 */
  u64 *c = mx_frame_direct(1 + 2);
  c = mx_e_ad_tag(c, 2, 1);
  c = mx_e_ad(c, MX_GS_REG_TEXA, 0x80ull << 32);
  c = mx_e_ad(c, MX_GS_REG_PABE, 0);
  mx_frame_commit(c);
  clut_send();
}

/* The counters, top left, in three lines: frames shown a second;
   triangles, vertices and draws a frame; RAM (32 MiB) as a bar of its
   parts, with the VM in use and swap a second; GS memory (4 MiB) as a bar,
   with the texture room used, uploads and refills a frame. The values are
   made once a second (averages over it), and logged with more. */
/* text: 0x80 is full (the font's texels times the colour / 128) */
#define HUD_LABEL 0x506050
#define HUD_TEXT 0x808080
#define HUD_OK 0x508050
#define HUD_WARN 0x808040
#define HUD_BAD 0x805050
#define HUD_BAR_X 38
#define HUD_BAR_W 100
#define HUD_TEXT_X 146
/* bar parts (boxes: 0xff is full) */
#define BAR_EMPTY 0x202020
#define BAR_PROG 0x606060  /* the program, stacks; GS: the screen and Z */
#define BAR_CORE 0x4070ff  /* the core heap in use */
#define BAR_CORE_FREE 0x203878
#define BAR_VM 0x40c040    /* VM pages (malloc without VM); GS: textures */
#define BAR_PIN 0xff9020   /* pinned VM pages */
#define BAR_UP 0xffff60    /* GS: uploaded a frame */
#define BAR_REFILL 0xd03030 /* GS: textures while they refill */
#define BAR_PARTS 5

struct hud_bar {
  u32 total, amt[BAR_PARTS], rgb[BAR_PARTS];
};
static void hud_bar(u32 y, const struct hud_bar *b) {
  u32 x = HUD_BAR_X * HUD_SCALE, w = HUD_BAR_W * HUD_SCALE;
  mx_hud_box(x, y, w, 5, 0x80000000u | BAR_EMPTY);
  /* the ends of the parts from their sums: no gaps between them */
  u32 sum = 0, at = 0;
  for (int i = 0; i < BAR_PARTS; i++) {
    sum += b->amt[i];
    u32 end = (u32)((u64)(sum < b->total ? sum : b->total) * w / b->total);
    if (end > at)
      mx_hud_box(x + at, y, end - at, 5, 0x80000000u | b->rgb[i]);
    at = end;
  }
}

static char *hud_mb(char *p, u32 bytes) { return mx_hud_u1(p, bytes, 1u << 20); }

/* a line: up to four pieces of text, each in its colour */
#define HUD_PIECES 4
struct hud_line {
  char s[HUD_PIECES][40];
  u32 rgb[HUD_PIECES];
};
static u32 hud_line(u32 x, u32 y, const struct hud_line *l, int draw) {
  for (int i = 0; i < HUD_PIECES; i++) {
    if (!l->s[i][0])
      continue;
    if (draw)
      mx_hud_text(x, y, l->s[i], l->rgb[i], HUD_SCALE);
    x += mx_hud_width(l->s[i], HUD_SCALE) + 12 * HUD_SCALE;
  }
  return x;
}

static void draw_stats(void) {
  flush_out();
  gs_drew = 1; /* the HUD's draws */
  /* the second: frames shown (beam racing: made), and the sums of the counters */
  static u32 v_last, f_last, n_frames, tris, verts, draws, up_kb, wraps;
  static u32 swap_in, swap_out, ee_last, ee_max, n_long;
  static struct hud_line line[3];
  static struct hud_bar ram, vram;
  u32 shown = beam ? mx_gs.frames : mx_gs.shown;
  u32 now = mx_count(), ee_us = (now - ee_last) / (MX_EE_HZ / 1000000);
  ee_last = now;
  if (ee_us > ee_max)
    ee_max = ee_us;
  n_long += ee_us > 1000000 / mx_gs.hz + 500;
  n_frames++;
  tris += st_last.tris, verts += st_last.verts, draws += st_last.draws;
  up_kb += st_last.upload_kb, wraps += st_last.wraps;
  if (shown < f_last) /* the mode changed: counted again from 0 */
    f_last = shown, v_last = mx_gs.vsync;
  u32 dv = mx_gs.vsync - v_last;
  if (dv >= mx_gs.hz) {
    memset(line, 0, sizeof line);
    /* frames: FPS green at the refresh rate, red below 20 */
    u32 fps10 = (shown - f_last) * 10 * mx_gs.hz / dv;
    struct hud_line *l = &line[0];
    mx_hud_cat(mx_hud_u1(l->s[0], fps10, 10), " FPS");
    l->rgb[0] = fps10 * 100 >= mx_gs.hz * 950 ? HUD_OK : fps10 < 200 ? HUD_BAD : HUD_WARN;
    char *t = mx_hud_cat(mx_hud_u(l->s[1], tris / n_frames), " TRIS  ");
    t = mx_hud_cat(mx_hud_u(t, verts / n_frames), " VERTS  ");
    mx_hud_cat(mx_hud_u(t, draws / n_frames), " DRAWS");
    l->rgb[1] = HUD_TEXT;
    /* RAM: the program (with the kernel, the VM's tables and the stack),
       pinned memory, free frames, VM pages in use, pinned VM pages;
       without VM, libgl's heaps and malloc's part of the heap */
    u32 gu, gh, su, sh;
    mx_heap_stats(gl_heap, &gu, &gh);
    mx_heap_stats(gl_stream, &su, &sh);
    ram = (struct hud_bar){32u << 20, {0, gh + sh, 0, 0, 0},
                           {BAR_PROG, BAR_CORE, BAR_CORE_FREE, BAR_VM, BAR_PIN}};
    u32 pinned = gh + sh;
    l = &line[1];
    if (mx_vm_active()) {
      struct mx_vm_stats vs;
      mx_vm_stats(&vs);
      pinned = vs.pinned_frames * MX_VM_PAGE;
      ram.amt[0] = ram.total - vs.frames * MX_VM_PAGE;
      ram.amt[1] = pinned;
      ram.amt[2] = vs.free_frames * MX_VM_PAGE;
      ram.amt[3] = (vs.frames - vs.free_frames - vs.pinned_frames - vs.pinned) * MX_VM_PAGE;
      ram.amt[4] = vs.pinned * MX_VM_PAGE;
      u32 out = vs.swap_outs + vs.writer_outs;
      u32 si = (vs.swap_ins - swap_in) * (MX_VM_PAGE / 1024) * mx_gs.hz / dv;
      u32 so = (out - swap_out) * (MX_VM_PAGE / 1024) * mx_gs.hz / dv;
      swap_in = vs.swap_ins, swap_out = out;
      mx_hud_cat(hud_mb(mx_hud_cat(l->s[0], "VM "), (u32)((u8 *)mx_sbrk(0) - (u8 *)mx_vm_base())),
                 " MB");
      l->rgb[0] = HUD_TEXT;
      t = mx_hud_cat(mx_hud_u(mx_hud_cat(l->s[1], "SWAP IN "), si), " OUT ");
      mx_hud_cat(mx_hud_u(t, so), " KB/S");
      l->rgb[1] = si || so ? HUD_WARN : HUD_TEXT;
    } else {
      u8 *lo = (u8 *)mx_heap_start();
      ram.amt[0] = ram.total - (u32)((u8 *)mx_heap_end() - lo);
      ram.amt[3] = (u32)((u8 *)mx_sbrk(0) - lo);
      mx_hud_cat(hud_mb(mx_hud_cat(l->s[0], "MALLOC "), ram.amt[3]), " MB");
      l->rgb[0] = HUD_TEXT;
    }
    /* libgl's part of the pinned memory, of all of it */
    t = mx_hud_cat(hud_mb(mx_hud_cat(l->s[2], "PIN GL "), gu + su), "/");
    mx_hud_cat(hud_mb(t, pinned), " MB");
    l->rgb[2] = HUD_TEXT;
    /* GS memory (KiB): the screen, Z, font and CLUTs; the textures in the
       cache, the end of them uploaded a frame */
    struct mx_vcache_stats vs;
    mx_vcache_stats(&vs);
    /* the top bytes' blocks hold a byte a word: a quarter */
    u32 room = tex_room() / 1024, tex = vs.used_blocks / 4 + vs.hbyte_used / 16,
        up = up_kb / n_frames;
    vram = (struct hud_bar){MX_VRAM_BLOCKS / 4, {vram_lo / 4, tex > up ? tex - up : 0, 0, 0, 0},
                            {BAR_PROG, wraps ? BAR_REFILL : BAR_VM, BAR_UP, 0, 0}};
    vram.amt[2] = tex - vram.amt[1];
    l = &line[2];
    mx_hud_cat(mx_hud_u(mx_hud_cat(mx_hud_u(mx_hud_cat(l->s[0], "TEX "), tex), "/"), room), " KB");
    l->rgb[0] = tex * 100 >= room * 95 ? HUD_BAD : tex * 100 >= room * 80 ? HUD_WARN : HUD_OK;
    mx_hud_cat(mx_hud_u(mx_hud_cat(l->s[1], "UP "), up), " KB/F");
    l->rgb[1] = HUD_TEXT;
    mx_hud_cat(mx_hud_u1(mx_hud_cat(l->s[2], "EVICT "), wraps * 10 / n_frames, 10), "/F");
    l->rgb[2] = wraps ? HUD_BAD : HUD_OK;
    /* the log: the lines, and more */
    u32 c = MX_EE_HZ / 10000;
    for (int i = 0; i < 3; i++)
      mx_printf("stats: %s  %s  %s\n", line[i].s[0], line[i].s[1], line[i].s[2]);
    struct mx_vcache_stats cv;
    mx_vcache_stats(&cv);
    mx_printf("stats: longest frame %u us, %u frames over a field, arena %u/%u KB; textures: %d "
              "levels left out%s, %u drawn from their tails (upper levels paged out), %u whole again\n",
              ee_max, n_long, arena_pos / 1024, ARENA_BYTES / 1024, tex_bias,
              tex_bias_auto ? " (auto)" : "", cv.proxies, cv.upgrades);
    if (tex_bias_auto) {
      u32 room = tex_room();
      mx_printf("stats: texture quality: W %u KB = %u%% of GS memory%s (placed about %u%%), RAM %s; "
                "mipped textures sampled from level 0/1/2+: %u/%u/%u\n",
                ws_last_w0 / 1024, room ? (u32)((u64)ws_last_w0 * 100 / room) : 0,
                v_low ? ", lowered" : "", v_low && room ? (u32)((u64)ws_last_e * 100 / room) : 0,
                ram_cold ? "cold (heads not used)" : "normal", ws_far[0], ws_far[1], ws_far[2]);
    }
    {
      /* where the cache's GS memory goes (KB), and the room made since the
         last line: the blocks in use in the runs taken, the blocks evicted */
      static u32 run0, ev0;
      struct mx_vcache_space sp;
      mx_vcache_space(&sp);
      u32 run = cv.evict_run_blocks - run0, ev = cv.evicted_blocks - ev0;
      run0 = cv.evict_run_blocks, ev0 = cv.evicted_blocks;
      mx_printf("stats: gs memory: room %u KB, held %u (pages %u); texels %u, between blocks %u, "
                "whole pages %u, free in shared pages %u; top bytes %u of %u KB (texels %u); "
                "evicted %u KB for runs with %u KB in use\n",
                (MX_VRAM_BLOCKS - vram_lo) / 4, cv.pages * 8, cv.pages, sp.need_blocks / 4,
                sp.span_waste / 4, sp.round_waste / 4, sp.small_free / 4, cv.hbyte_used / 16,
                cv.hbyte_blocks / 16, sp.hbyte_need / 16, ev / 4, run / 4);
    }
    mx_printf("stats: ram: program %u KB, pinned %u KB (libgl %u in %u, stream %u in %u), free %u KB, "
              "%s %u KB, pinned pages %u KB\n",
              ram.amt[0] / 1024, ram.amt[1] / 1024, gu / 1024, gh / 1024, su / 1024, sh / 1024,
              ram.amt[2] / 1024, mx_vm_active() ? "VM pages" : "malloc", ram.amt[3] / 1024,
              ram.amt[4] / 1024);
    static u32 ring0, buf0;
    mx_printf("stats: xf %u.%u tri %u.%u (incl flush %u.%u) ms, wait ring %u ms buf %u ms "
              "(since last)\n",
              st_last.xf_cycles / c / 10, st_last.xf_cycles / c % 10, st_last.tri_cycles / c / 10,
              st_last.tri_cycles / c % 10, st_last.flush_cycles / c / 10,
              st_last.flush_cycles / c % 10,
              (mx_frame_stats.wait_ring - ring0) / (MX_EE_HZ / 1000),
              (mx_frame_stats.wait_buffer - buf0) / (MX_EE_HZ / 1000));
    ring0 = mx_frame_stats.wait_ring, buf0 = mx_frame_stats.wait_buffer;
    mx_printf("stats: lists %u (made %u, %u did not fit; pool %u KB): bounds %u us, state %u us, "
              "call %u us\n", st_last.lists, dl_nused, dl_nfailed, dl_pool_pos / 1024,
              st_last.c_bounds / 295, st_last.c_state / 295, st_last.c_call / 295);
    mx_printf("stats: calls %u, batches %u; matrix %u us, lod %u us, set_state %u us; "
              "clipped %u in %u us, refused %u\n",
              st_last.calls, st_last.draws, st_last.c_mvp / 295, st_last.c_lod / 295,
              st_last.c_set / 295, st_last.clipped, st_last.c_clip / 295, st_last.refused);
    if (beam) {
      /* the longest pass of each strip */
      u32 n = mx_hires_passes(), us = 0;
      for (u32 k = 0; k < n; k++) {
        us = mx_hires_stats.pass_us[k] > us ? mx_hires_stats.pass_us[k] : us;
        mx_hires_stats.pass_us[k] = 0;
      }
      mx_printf("stats: beam: fields %u, passes %u (%u a field), late %u, records shown %u, held %u, "
                "largest %u KB of %u; longest pass %u us\n",
                mx_hires_stats.fields, mx_hires_stats.passes, n, mx_hires_stats.late, mx_hires_stats.shown,
                mx_hires_stats.held, mx_hires_stats.rec_max / 1024, REC_BYTES / MX_HIRES_SLOTS / 4 * 3 / 1024, us);
    }
    v_last = mx_gs.vsync, f_last = shown;
    n_frames = tris = verts = draws = up_kb = wraps = 0;
    ee_max = n_long = 0;
  }
  if (!line[0].s[0][0])
    return; /* the first second: nothing yet */
  /* full screen, no tests */
  u64 *c = mx_frame_reserve(5);
  c = mx_e_cnt(c, 3, mx_vif_flush(), mx_vif_direct(3));
  c = mx_e_ad_tag(c, 2, 1);
  if (beam)
    c = mx_e_ad(c, MX_GS_REG_ALPHA_1, mx_gs_alpha(0, 1, 0, 1, 0)); /* the box: over */
  else
    c = mx_e_ad(c, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, fb_w - 1, 0, fb_h - 1));
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  mx_frame_commit(c);
  gs.valid = 0, gs_serial++;
  u32 x0 = 8 * HUD_SCALE, x1 = HUD_TEXT_X * HUD_SCALE, w = hud_line(x0, 0, &line[0], 0);
  for (int i = 1; i < 3; i++) {
    u32 e = hud_line(x1, 0, &line[i], 0);
    if (e > w)
      w = e;
  }
  mx_hud_box(4 * HUD_SCALE, 2, w - 12 * HUD_SCALE, 31, 0x60000000);
  hud_line(x0, 4, &line[0], 1);
  mx_hud_text(x0, 13, "RAM", HUD_LABEL, HUD_SCALE);
  hud_bar(14, &ram);
  hud_line(x1, 13, &line[1], 1);
  mx_hud_text(x0, 22, "VRAM", HUD_LABEL, HUD_SCALE);
  hud_bar(23, &vram);
  hud_line(x1, 22, &line[2], 1);
}

static void tex_auto(u32 up_kb);
void mxgl_end_frame(void) {
  /* L3 + R3 (both sticks pressed): the counters on and off, in every program */
  static int sticks_held;
  int sticks = (mx_pad_buttons(0) & (MX_PAD_L3 | MX_PAD_R3)) == (MX_PAD_L3 | MX_PAD_R3);
  if (sticks && !sticks_held)
    mxgl_show_stats = !mxgl_show_stats;
  sticks_held = sticks;
  pq_flush();
  if (!in_frame)
    frame_start();
  flush_out();
  if (mxgl_show_stats)
    draw_stats();
  /* the textures the cache evicted this frame */
  {
    static u32 evicted;
    struct mx_vcache_stats vs;
    mx_vcache_stats(&vs);
    st_cur.wraps = vs.evicted - evicted;
    evicted = vs.evicted;
  }
  tex_auto(st_cur.upload_kb);
#ifdef MXGL_STATS_WINDOW
  stats_window();
#endif
  st_last = st_cur;
  memset(&st_cur, 0, sizeof st_cur);
  mx_frame_end();
  in_frame = 0;
}

/* register writes after a FLUSH (the VU1 batches before are drawn first) */
static void write_regs(const u32 *reg, const u64 *val, int n) {
  gs_serial++;
  if (!n)
    return;
  /* the triangles gathered so far are drawn with the registers before */
  flush_out();
  /* beam: the same in context 2 (the draws without Z writes) */
  u32 reg2[24];
  u64 val2[24];
  if (beam) {
    for (int i = 0; i < n; i++) {
      reg2[2 * i] = reg[i], val2[2 * i] = val[i];
      reg2[2 * i + 1] = reg[i] + 1, val2[2 * i + 1] = val[i];
    }
    reg = reg2, val = val2, n *= 2;
  }
  u64 *c = mx_frame_reserve((u32)n + 2);
  c = mx_e_cnt(c, (u32)n + 1, GS_DREW ? mx_vif_flush() : mx_vif_nop(), mx_vif_direct((u32)n + 1));
  gs_drew = 0;
  c = mx_e_ad_tag(c, (u32)n, 1);
  for (int i = 0; i < n; i++)
    c = mx_e_ad(c, reg[i], val[i]);
  mx_frame_commit(c);
}
