/* glstencilp: the stencil and the colour mask, in one picture (host:glstencilp.tga), built
   for a mode of mxgl_set_mode (-DMODE=n): with beam racing glReadPixels has no pixels, so
   the pictures of the modes are compared. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "mxgl.h"
#include "t.h"

#define W 640
#define H 448

static void quad(float x0, float y0, float x1, float y1, float z) {
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0), glVertex3f(x0, y0, z);
  glTexCoord2f(1, 0), glVertex3f(x1, y0, z);
  glTexCoord2f(1, 1), glVertex3f(x1, y1, z);
  glTexCoord2f(0, 1), glVertex3f(x0, y1, z);
  glEnd();
}

int main(void) {
  mxgl_init();
  CHECK_EQ(mxgl_set_mode(MODE), 0);
  CHECK_EQ(mxgl_mode_flags() & MODE & 0x300, MODE & 0x300);
  mxgl_show_stats = 0;
  mxgl_window(W, H);
  glViewport(0, 0, W, H);
  static const u32 tx[4] = {0xFFFFFFFFu, 0x00FFFFFFu, 0x00FFFFFFu, 0xFFFFFFFFu};
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, tx);
  GLint bits = 0;
  glGetIntegerv(GL_STENCIL_BITS, &bits);
  mx_printf("glstencilp: mode %d, %d stencil bits\n", MODE, bits);
  for (int f = 0; f < 8; f++) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.1f, 0.1f, 0.2f, 1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    /* a mask over strips of the screen: tall, so that it crosses them */
    glEnable(GL_STENCIL_TEST);
    glColorMask(0, 0, 0, 0);
    glStencilFunc(GL_ALWAYS, 1, 255);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    quad(20, 20, 120, 420, 0);
    glColorMask(1, 1, 1, 1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_EQUAL, 1, 255);
    glColor3f(1, 0, 0);
    quad(10, 60, 200, 380, 0);
    glStencilFunc(GL_NOTEQUAL, 1, 255);
    glColor3f(0, 1, 0);
    quad(10, 60, 200, 380, 0);
    glDisable(GL_STENCIL_TEST);

    /* overlapping shadows, each pixel once */
    glClearStencil(1);
    glClear(GL_STENCIL_BUFFER_BIT);
    glColor3f(1, 1, 1);
    quad(220, 20, 420, 420, 0);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 1, 255);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0, 0, 0, 0.5f);
    quad(240, 40, 340, 300, 0);
    quad(300, 150, 400, 400, 0);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);

    /* a decal's mask, with the alpha test */
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 255);
    glColorMask(0, 0, 0, 0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    quad(440, 30, 620, 210, 0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    quad(440, 30, 620, 210, 0);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
    glColorMask(1, 1, 1, 1);
    glStencilFunc(GL_EQUAL, 0, 255);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColor3f(0, 0.8f, 1);
    quad(430, 20, 630, 220, 0);
    glDisable(GL_STENCIL_TEST);

    /* the colour mask: green off, over a yellow quad */
    glColor3f(1, 1, 0);
    quad(440, 240, 620, 420, 0);
    glColorMask(1, 0, 1, 1);
    glColor3f(0, 0, 1);
    quad(470, 270, 590, 390, 0);
    glColorMask(1, 1, 1, 1);

    mxgl_end_frame();
    mxgl_frame_done();
  }
  int r = mxgl_screenshot("host:glstencilp.tga");
  mx_printf("glstencilp: picture %d\n", r);
  return t_done("glstencilp");
}
