/* gl/native.c, a part of mxgl.c (included there, not compiled alone): for a
   renderer of the game's own beside libgl, which writes its VU1 batches into
   the frame stream itself (<mx/frame.h>) and takes from libgl what a GL
   state is in GS registers (the texture placed, its CLUT, the blend) and
   the matrix */

/* libgl's own batches drawn: the stream is the caller's until
   mxgl_native_end */
void mxgl_native_begin(void) {
  pq_flush();
  if (!in_frame)
    frame_start();
  flush_out();
}

/* the stream's VU1 program, layout and GS registers are unknown again */
void mxgl_native_end(void) {
  vu_cur = NULL;
  gs.valid = 0;
  gs_serial++;
  gs_drew = 1;
}

/* the GS registers of the current GL state for a draw (all of them, not
   only those that changed; the texture is placed, uploaded in the stream if
   need be), lod_k the K of TEX1 (1/16): n registers in reg, val (context 1;
   with beam racing context 2 too, after each), *prim the PRIM of its
   triangles (TRISTRIP; CTXT 1 for no Z writes with beam racing), cscale
   the colour scale (0x80 is 1.0); the count, or -1: it cannot be drawn */
int mxgl_native_state(unsigned *reg, unsigned long long *val, int max, unsigned *prim,
                      float *cscale, int lod_k) {
  mxgl_native_begin();
  gs.valid = 0;
  nat_capture = 1, nat_reg = reg, nat_val = val, nat_n = 0, nat_max = max;
  u32 p = 0;
  int r = set_state(&p, cscale, lod_k);
  nat_capture = 0;
  gs.valid = 0;
  k_valid = 0;
  *prim = p;
  return r < 0 ? -1 : nat_n;
}

/* the draws' matrix: object coordinates to GS coordinates times w (x, y in
   pixels of the GS's window, z in the Z buffer's range); z[0], z[1] the near
   and far planes' z (a point is between them when z[1] w <= z <= z[0] w),
   z[2], z[3] the guard band's lowest and highest x and y */
void mxgl_native_matrix(float *out, float *z) {
  update_matrix();
  memcpy(out, m, 64);
  z[0] = zn, z[1] = zf, z[2] = GB_LO, z[3] = GB_HI;
}

/* the K (1/16) of the bound texture for triangles whose texture coordinates
   cover 2^l2 times their area (as lod_k finds it from a draw's triangles) */
int mxgl_native_lod(float l2) {
  struct tex *t = gl.tex2d && gl.bound && gl.bound < MAX_TEX ? &texs[gl.bound] : NULL;
  if (!t || !t->used || t->levels < 2)
    return 0;
  float k = (0.5f * (l2 + fast_log2((float)t->w * (float)t->h)) + tm_log2_area() - proj_lpp() +
             gl.lod_bias) * 16.0f;
  s32 ki = (s32)(k + (k < 0 ? -0.5f : 0.5f));
  return ki < -2048 ? -2048 : ki > 2047 ? 2047 : ki;
}
