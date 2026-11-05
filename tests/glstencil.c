/* glstencil: the stencil's bit (the frame buffer's alpha bit), by the sequences games
   use: a mask, a planar shadow drawn once where it overlaps itself (Half-Life, Hexen 2),
   a decal's mask with the alpha test (Half-Life), depth, and the colour of the rest. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "mxgl.h"
#include "t.h"

#define W 640
#define H 448
#define TOL 24

static u32 pix(int x, int y) {
  u8 p[4];
  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
  return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16;
}
static int near(u32 a, u32 b) {
  for (int i = 0; i < 3; i++) {
    int d = (int)(a >> (8 * i) & 255) - (int)(b >> (8 * i) & 255);
    if (d < -TOL || d > TOL)
      return 0;
  }
  return 1;
}
#define CHECK_PIX(x, y, rgb)                                                                      \
  do {                                                                                            \
    u32 p_ = pix(x, y);                                                                           \
    if (!near(p_, rgb)) {                                                                         \
      mx_printf("FAIL %s:%d: pixel (%d, %d) = %06x, expected %06x\n", __FILE__, __LINE__, (int)(x), \
                (int)(y), p_, (u32)(rgb));                                                        \
      t_fail++;                                                                                   \
    }                                                                                             \
  } while (0)

static void quad(float x0, float y0, float x1, float y1, float z) {
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0), glVertex3f(x0, y0, z);
  glTexCoord2f(1, 0), glVertex3f(x1, y0, z);
  glTexCoord2f(1, 1), glVertex3f(x1, y1, z);
  glTexCoord2f(0, 1), glVertex3f(x0, y1, z);
  glEnd();
}
static void frame(void) {
  mxgl_end_frame();
  mxgl_frame_done();
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, W, 0, H, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0, 0, 0, 1);
  glClearStencil(0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

int main(void) {
  mxgl_init();
  mxgl_show_stats = 0;
  mxgl_window(W, H);
  glViewport(0, 0, W, H);
  GLint bits = 0;
  glGetIntegerv(GL_STENCIL_BITS, &bits);
  CHECK_EQ(bits, 1);

  /* a mask: colour off, REPLACE in a quad; EQUAL draws inside, NOTEQUAL outside */
  frame();
  glEnable(GL_STENCIL_TEST);
  glColorMask(0, 0, 0, 0);
  glStencilFunc(GL_ALWAYS, 1, 255);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  quad(100, 100, 200, 200, 0);
  glColorMask(1, 1, 1, 1);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  glStencilFunc(GL_EQUAL, 1, 255);
  glColor3f(1, 0, 0);
  quad(50, 50, 300, 300, 0);
  CHECK_PIX(150, 150, 0x0000FF);
  CHECK_PIX(250, 250, 0x000000);
  glStencilFunc(GL_NOTEQUAL, 1, 255);
  glColor3f(0, 1, 0);
  quad(50, 50, 300, 300, 0);
  CHECK_PIX(150, 150, 0x0000FF);
  CHECK_PIX(250, 250, 0x00FF00);
  /* the mask stays when the stencil test is off and colour is drawn over it */
  glDisable(GL_STENCIL_TEST);
  glColor3f(0, 0, 1);
  quad(50, 50, 300, 300, 0);
  glEnable(GL_STENCIL_TEST);
  glStencilFunc(GL_EQUAL, 1, 255);
  glColor3f(1, 1, 1);
  quad(50, 50, 300, 300, 0);
  CHECK_PIX(150, 150, 0xFFFFFF);
  CHECK_PIX(250, 250, 0xFF0000);

  /* a planar shadow (Hexen 2): stencil cleared to 1, EQUAL 1 with INCR, blended: where
     two triangles overlap it is dark once */
  frame();
  glDisable(GL_STENCIL_TEST);
  glColor3f(1, 1, 1);
  quad(0, 0, W, H, 0);
  glClearStencil(1);
  glClear(GL_STENCIL_BUFFER_BIT);
  glEnable(GL_STENCIL_TEST);
  glStencilFunc(GL_EQUAL, 1, 255);
  glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(0, 0, 0, 0.5f);
  quad(100, 100, 300, 300, 0);
  quad(200, 200, 400, 400, 0);
  glDisable(GL_BLEND);
  CHECK_PIX(150, 150, 0x808080);
  CHECK_PIX(250, 250, 0x808080); /* the overlap: once */
  CHECK_PIX(350, 350, 0x808080);
  CHECK_PIX(500, 100, 0xFFFFFF);

  /* a decal's mask (Half-Life): REPLACE 1, DECR where the texture's alpha passes,
     EQUAL 0 draws there and outside */
  frame();
  static const u32 tx[4] = {0xFFFFFFFFu, 0x00FFFFFFu, 0x00FFFFFFu, 0xFFFFFFFFu};
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, tx);
  glStencilFunc(GL_ALWAYS, 1, 0xFFFFFFFFu);
  glColorMask(0, 0, 0, 0);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glColor4f(1, 1, 1, 1);
  quad(100, 100, 300, 300, 0);
  glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
  glEnable(GL_TEXTURE_2D);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.5f);
  quad(100, 100, 300, 300, 0);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_TEXTURE_2D);
  glColorMask(1, 1, 1, 1);
  glStencilFunc(GL_EQUAL, 0, 0xFFFFFFFFu);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  glColor3f(0, 1, 0);
  quad(50, 50, 350, 350, 0);
  CHECK_PIX(60, 60, 0x00FF00);    /* outside: 0 */
  CHECK_PIX(150, 150, 0x00FF00);  /* alpha 1: back to 0 */
  CHECK_PIX(250, 150, 0x000000);  /* alpha 0: stays 1 */
  CHECK_PIX(150, 250, 0x000000);
  CHECK_PIX(250, 250, 0x00FF00);

  /* depth: a pixel that fails the depth test does not write the stencil */
  frame();
  glDisable(GL_STENCIL_TEST);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glColor3f(0.2f, 0.2f, 0.2f);
  quad(100, 100, 200, 200, 0.5f); /* in front (glOrtho: z -1 is far from -near) */
  glEnable(GL_STENCIL_TEST);
  glStencilFunc(GL_ALWAYS, 1, 255);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glColorMask(0, 0, 0, 0);
  quad(50, 50, 300, 300, -0.5f);
  glColorMask(1, 1, 1, 1);
  glDisable(GL_DEPTH_TEST);
  glStencilFunc(GL_EQUAL, 1, 255);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  glColor3f(1, 1, 0);
  quad(50, 50, 300, 300, 0);
  glDisable(GL_STENCIL_TEST);
  CHECK_PIX(250, 250, 0x00FFFF);
  CHECK_EQ(glGetError(), GL_NO_ERROR);
  return t_done("glstencil");
}
