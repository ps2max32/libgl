/* gl/texquality.c, a part of mxgl.c (included there, not compiled alone): texture quality under
   pressure (MXGL_TEX_AUTO), and a texture's place in GS memory (tex_place) */

/* ---- texture quality under pressure (MXGL_TEX_AUTO) ----

   VRAM: W, the bytes of the textures a frame uses (at full quality), against
   V, GS memory's room for textures. Up to 120% of V the frame streams what
   does not fit (the textures are in RAM: cheap). Above it for 2 seconds the
   textures are placed from coarser levels until about 80% of V: first the
   finest level their draws sample (LOD: no visible change), then the
   largest ones a level less each, never below MX_VCACHE_TAIL_MIN a side;
   back to full once W at full quality is at most halfway (100%) for 3
   seconds.
   RAM: swap-ins above 30 pages a second for 3 seconds: textures whose
   draws do not sample their top level are drawn from their tails (their
   pageable upper levels not touched, and marked to go first: mx_mem_cold);
   back after 5 quiet seconds. */

void mxgl_texture_limits(int vram_hi_pct, int vram_lo_pct, int swap_ins_s) {
  tq_hi = vram_hi_pct, tq_lo = vram_lo_pct, tq_swap = swap_ins_s;
}

/* t's bytes in GS memory placed from level f (and its CLUT) */
static u32 tex_bytes_from(const struct tex *t, u32 f) {
  u32 n = t->oclut ? 1024 : 0;
  for (u32 lv = f; lv < t->levels; lv++)
    n += mx_tex_level_bytes(t->psm, t->lw, t->lh, lv);
  return n;
}
/* the coarsest level t may be placed from (MX_VCACHE_TAIL_MIN a side) */
static u32 tex_max_from(const struct tex *t) {
  u32 f = 0;
  while (f + 1 < t->levels && (t->w >> (f + 1)) >= MX_VCACHE_TAIL_MIN &&
         (t->h >> (f + 1)) >= MX_VCACHE_TAIL_MIN)
    f++;
  return f;
}
static int tex_mipped(const struct tex *t) { return t->mip && t->levels > 1; }

/* a draw of the bound texture with LOD constant k (1/16) whose nearest
   vertex is at w: the finest level the GS samples there */
static void tex_need_note(s32 k, float wmin) {
  if (!gl.tex2d || !gl.bound || gl.bound >= MAX_TEX)
    return;
  struct tex *t = &texs[gl.bound];
  if (!t->used || !tex_mipped(t))
    return;
  float l = wmin > 1e-6f ? fast_log2(wmin) + (float)k / 16.0f : 0.0f;
  u32 lvl = l <= 0 ? 0 : (u32)l;
  if (lvl >= t->levels)
    lvl = t->levels - 1u;
  u32 f = mx_frame_number();
  if (lvl <= t->need_lvl || (s32)(f - t->need_until) > 0)
    t->need_lvl = (u8)lvl, t->need_until = f + NEED_HOLD;
}

static u32 tex_need(const struct tex *t) {
  if (!tex_mipped(t))
    return 0;
  u32 n = (u32)tex_bias;
  if (v_low) {
    u32 v = t->vfrom_epoch == v_epoch ? t->vfrom : t->need_lvl;
    n = v > n ? v : n;
  }
  /* RAM: drawn from its tail while its top level is not sampled */
  if (ram_cold && t->m.head && t->need_lvl >= 1 && t->m.levels > t->m.tail && t->m.tail > n)
    n = t->m.tail;
  return n;
}

void mxgl_texture_quality(int q) {
  tex_bias_auto = q == MXGL_TEX_AUTO;
  int b = tex_bias_auto ? 0 : q < 0 ? 0 : q > 2 ? 2 : q;
  if (b != tex_bias || !tex_bias_auto)
    vram_gen++; /* every texture looked up again */
  tex_bias = b;
  if (!tex_bias_auto)
    v_low = ram_cold = 0;
}
int mxgl_texture_bias(void) { return tex_bias; }

/* the frame used t (its first lookup in the frame) */
static void ws_note(struct tex *t) {
  u32 f = mx_frame_number();
  if (t->ws_frame == f + 1)
    return;
  t->ws_frame = f + 1;
  ws_w0 += tex_bytes_from(t, 0);
  if (ws_n[ws_cur] < WS_MAX)
    ws[ws_cur][ws_n[ws_cur]++] = t;
}

/* the VRAM control's levels for the textures of the frame before: from
   the finest level sampled, then the largest a level less until the
   estimate is at most tq_lo % of V */
static int vram_assign(u32 room) {
  v_epoch++;
  struct tex **l = ws[ws_cur ^ 1];
  u32 n = ws_n[ws_cur ^ 1], e = 0;
  static u8 before[WS_MAX];
  for (u32 i = 0; i < n; i++) {
    struct tex *t = l[i];
    before[i] = t->vfrom;
    t->vfrom = tex_mipped(t) ? t->need_lvl : 0;
    t->vfrom_epoch = v_epoch;
    e += tex_bytes_from(t, t->vfrom);
  }
  u64 target = (u64)room * (u32)tq_lo / 100;
  /* a level less each, the textures seen from farthest first (their draws
     sample coarse levels already: least visible), within those the largest
     first; the ones sampled at level 0 (near) last */
  for (int cls = 2; cls >= 0 && e > target; cls--)
    for (int pass = 0; pass < 3 && e > target; pass++) {
      u32 big = 0;
      for (u32 i = 0; i < n; i++)
        if (tex_mipped(l[i]) && l[i]->vfrom < tex_max_from(l[i]) &&
            (cls == 2 ? l[i]->need_lvl >= 2 : l[i]->need_lvl == (u32)cls)) {
          u32 b = tex_bytes_from(l[i], l[i]->vfrom);
          big = b > big ? b : big;
        }
      if (!big)
        break;
      for (u32 i = 0; i < n && e > target; i++) {
        struct tex *t = l[i];
        if (!tex_mipped(t) || t->vfrom >= tex_max_from(t) ||
            !(cls == 2 ? t->need_lvl >= 2 : t->need_lvl == (u32)cls))
          continue;
        u32 b = tex_bytes_from(t, t->vfrom);
        if (b * 2 < big && pass < 2)
          continue; /* the larger ones first */
        e -= b - tex_bytes_from(t, t->vfrom + 1u);
        t->vfrom++;
      }
    }
  ws_last_e = e;
  /* changed: looked up again (the others keep their places) */
  int changed = 0;
  for (u32 i = 0; i < n; i++)
    changed |= l[i]->vfrom != before[i];
  return changed;
}

static u32 cold_hot, cold_cool, v_hot, v_cool;
static void tex_auto(u32 up_kb) {
  static u32 v0;
  (void)up_kb;
  ws_sum += ws_w0, ws_frames++;
  ws_w0 = 0;
  ws_cur ^= 1;
  ws_n[ws_cur] = 0;
  if (!tex_bias_auto || mx_gs.vsync - v0 < mx_gs.hz)
    return;
  v0 = mx_gs.vsync;
  u32 room = tex_room();
  u32 w = ws_frames ? ws_sum / ws_frames : 0;
  ws_last_w0 = w;
  ws_sum = ws_frames = 0;
  /* VRAM */
  int was = v_low;
  if (!v_low) {
    v_hot = (u64)w * 100 > (u64)room * (u32)tq_hi ? v_hot + 1 : 0;
    if (v_hot >= 2)
      v_low = 1, v_hot = 0, v_cool = 0;
  } else {
    /* back at full quality once W at full quality is halfway between the
       limits (100%): streaming that much is fine */
    v_cool = (u64)w * 200 <= (u64)room * (u32)(tq_lo + tq_hi) ? v_cool + 1 : 0;
    if (v_cool >= 3)
      v_low = 0, v_cool = 0;
  }
  ws_far[0] = ws_far[1] = ws_far[2] = 0;
  for (u32 i = 0; i < ws_n[ws_cur ^ 1]; i++)
    if (tex_mipped(ws[ws_cur ^ 1][i]))
      ws_far[ws[ws_cur ^ 1][i]->need_lvl > 2 ? 2 : ws[ws_cur ^ 1][i]->need_lvl]++;
  int changed = v_low ? vram_assign(room) : 0;
  if (changed || v_low != was)
    vram_gen++; /* the textures looked up again, from their new levels */
  /* RAM */
  struct mx_mem_pressure p;
  mx_mem_pressure(&p);
  int busy = p.swap_ins_s >= (u32)tq_swap; /* 0: always (tests) */
  cold_hot = busy ? cold_hot + 1 : 0;
  cold_cool = busy ? 0 : cold_cool + 1;
  if (!ram_cold && cold_hot >= 3) {
    ram_cold = 1;
    vram_gen++;
    struct tex **l = ws[ws_cur ^ 1];
    for (u32 i = 0; i < ws_n[ws_cur ^ 1]; i++)
      if (l[i]->m.head && l[i]->need_lvl >= 1)
        mx_mem_cold(l[i]->m.head, l[i]->m.head_bytes);
  } else if (ram_cold && cold_cool >= 5) {
    ram_cold = 0;
    vram_gen++;
  }
}

static int tex_place(struct tex *t) {
  u32 frame = mx_frame_number();
  if (t->vc_frame == frame && t->vc_gen == vram_gen && t->vc_gen && !t->dirty) {
    /* a use all the same: the cache tells a texture done for the frame by its uses */
    mx_vcache_use(mx_vcache_tex_key(tex_key(t), t->m.from));
    return 0;
  }
  ws_note(t);
  u32 cpsm = t->psm == MX_GS_PSM_T8 ? (t->oclut ? MX_GS_PSM_CT32 : MX_VCACHE_NO_CLUT) : 0;
  t->m.levels = (u8)tex_levels(t);
  /* without MIPMAP filtering level 0 is drawn: wait for it rather than a proxy */
  u32 from;
#ifdef MXGL_FORCE_TAIL /* debugging: split textures drawn from their tails */
  u32 need_ = t->m.tail ? t->m.tail : tex_need(t);
#else
  u32 need_ = tex_need(t);
#endif
  int r = mx_vcache_tex_mem(tex_key(t), &t->m, t->psm, t->lw, t->lh, cpsm, need_, !t->mip,
                            &t->mt, &from);
  u32 nl = t->m.levels - from;
  if (r == MX_VCACHE_FULL)
    return -1;
  if (r == MX_VCACHE_HIT && !t->dirty) {
    t->vc_frame = frame, t->vc_gen = vram_gen;
    return 0;
  }
  st_cur.uploads++;
  once_on();
  if (r == MX_VCACHE_HIT && nl == 1 && !from) {
    /* changed lines alone (whole quadwords of lines), read from its head */
    mx_tex_mem_lock(&t->m);
    u32 rb = t->w * tex_bpp(t), k = 1;
    while ((k * rb) & 15)
      k <<= 1;
    u32 y0 = t->dy0 & ~(k - 1), y1 = (t->dy1 + k - 1) & ~(k - 1);
    y1 = y1 > t->h ? t->h : y1;
    const u8 *src = TLV(t, 0) + y0 * rb;
    image_xfer_at(t->mt.tbp[0], t->mt.tbw[0], t->mt.psm, y0, t->w, y1 - y0, tex_bpp(t), src, 1);
  } else {
    if (r == MX_VCACHE_MISS) {
      /* other textures moved only if the miss evicted some */
      static u32 evicted;
      struct mx_vcache_stats vs;
      mx_vcache_stats(&vs);
      if (vs.evicted != evicted)
        vram_gen++;
      evicted = vs.evicted;
    }
    for (u32 lv = 0; lv < nl; lv++)
      image_xfer(t->mt.tbp[lv], t->mt.tbw[lv], t->mt.psm, t->w >> (lv + from), t->h >> (lv + from),
                 tex_bpp(t), TLV(t, lv + from), lv == 0);
    if (t->oclut) {
      image_xfer(t->mt.cbp, 1, MX_GS_PSM_CT32, 16, 16, 4, t->oclut, 0);
      /* the GS loads a CLUT when TEX0 is written (CLD 1): written again even
         if the same (another texture of this shape had these places this
         frame, its CLUT is the one loaded) */
      gs.tex0 = ~0ull;
    }
  }
  t->dirty = 0;
  texflush();
  once_off();
  t->vc_frame = frame, t->vc_gen = vram_gen;
  return 0;
}
