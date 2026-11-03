/* glconf: libgl's GL calls drawn on the GS and read back (glReadPixels),
   one check a feature. The colour buffer is 16 bits and dithered: colours
   are compared within a tolerance. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <string.h>

#include "mxgl.h"
#include "t.h"

#define W 640
#define H 448
#define TOL 20

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

#define CHECK_PIX(x, y, rgb)                                                                \
  do {                                                                                     \
    u32 p_ = pix(x, y);                                                                    \
    if (!near(p_, rgb)) {                                                                  \
      mx_printf("FAIL %s:%d: pixel (%d, %d) = %06x, expected %06x\n", __FILE__, __LINE__,  \
                (int)(x), (int)(y), p_, (u32)(rgb));                                       \
      t_fail++;                                                                            \
    }                                                                                      \
  } while (0)

static void quad(float x0, float y0, float x1, float y1, float z) {
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0), glVertex3f(x0, y0, z);
  glTexCoord2f(1, 0), glVertex3f(x1, y0, z);
  glTexCoord2f(1, 1), glVertex3f(x1, y1, z);
  glTexCoord2f(0, 1), glVertex3f(x0, y1, z);
  glEnd();
}

/* a new frame, cleared to black, 2D in window pixels, all off */
static void frame(void) {
  mxgl_end_frame();
  mxgl_frame_done();
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, W, 0, H, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glMatrixMode(GL_TEXTURE);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glDisable(GL_TEXTURE_2D), glDisable(GL_BLEND), glDisable(GL_ALPHA_TEST);
  glDisable(GL_DEPTH_TEST), glDisable(GL_CULL_FACE), glDisable(GL_SCISSOR_TEST);
  glDisable(GL_POLYGON_OFFSET_FILL), glDisable(GL_CLIP_PLANE0);
  glColorMask(1, 1, 1, 1);
  glDepthMask(1);
  glDepthFunc(GL_LESS);
  glColor4f(1, 1, 1, 1);
}

int main(void) {
  mxgl_init();
  mxgl_show_stats = 0;
  /* the window: the screen of NTSC, mapped onto PAL's too */
  mxgl_window(W, H);
  glViewport(0, 0, W, H);
  (void)glGetError();

  /* the clear colour */
  frame();
  glClearColor(0.25f, 0.5f, 0.75f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  CHECK_PIX(320, 224, 0xBF8040);

  /* a flat quad */
  frame();
  glColor3f(1, 0, 0);
  quad(100, 100, 200, 200, 0);
  CHECK_PIX(150, 150, 0x0000FF);
  CHECK_PIX(250, 150, 0x000000);

  /* blend: half white over black */
  frame();
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(1, 1, 1, 0.5f);
  quad(100, 100, 200, 200, 0);
  CHECK_PIX(150, 150, 0x808080);

  /* alpha test */
  frame();
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.5f);
  glColor4f(1, 1, 1, 0.25f);
  quad(100, 100, 200, 200, 0);
  CHECK_PIX(150, 150, 0x000000);
  glColor4f(1, 1, 1, 0.75f);
  quad(100, 100, 200, 200, 0);
  CHECK_PIX(150, 150, 0xFFFFFF);

  /* depth: nearer (z 0.5: depth 0.25) stays */
  frame();
  glEnable(GL_DEPTH_TEST);
  glColor3f(1, 0, 0);
  quad(100, 100, 200, 200, 0.5f);
  glColor3f(0, 1, 0);
  quad(100, 100, 200, 200, -0.5f);
  CHECK_PIX(150, 150, 0x0000FF);
  glDepthFunc(GL_ALWAYS);
  quad(100, 100, 200, 200, -0.5f);
  CHECK_PIX(150, 150, 0x00FF00);
  glDepthFunc(GL_NEVER);
  glColor3f(0, 0, 1);
  quad(100, 100, 200, 200, 0.9f);
  CHECK_PIX(150, 150, 0x00FF00);
  /* the depth read back (z -0.5: depth 0.75) */
  {
    float d = -1;
    glReadPixels(150, 150, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d);
    if (!(d > 0.74f && d < 0.76f))
      mx_printf("depth %d/1000\n", (int)(d * 1000));
    CHECK(d > 0.74f && d < 0.76f);
  }

  /* scissor */
  frame();
  glEnable(GL_SCISSOR_TEST);
  glScissor(300, 300, 20, 20);
  glColor3f(0, 0, 1);
  quad(0, 0, W, H, 0);
  CHECK_PIX(310, 310, 0xFF0000);
  CHECK_PIX(280, 310, 0x000000);

  /* colour mask: no red */
  frame();
  glColorMask(0, 1, 1, 1);
  quad(100, 100, 200, 200, 0);
  CHECK_PIX(150, 150, 0xFFFF00);
  glColorMask(1, 1, 1, 1);
  quad(300, 100, 400, 200, 0);
  CHECK_PIX(350, 150, 0xFFFFFF);

  /* polygon offset: a second quad in the same place passes LESS with it */
  frame();
  glEnable(GL_DEPTH_TEST);
  glColor3f(1, 0, 0);
  quad(100, 100, 200, 200, 0);
  glColor3f(0, 1, 0);
  quad(100, 100, 200, 200, 0);
  CHECK_PIX(150, 150, 0x0000FF);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(-1, -2);
  quad(100, 100, 200, 200, 0);
  CHECK_PIX(150, 150, 0x00FF00);

  /* a clip plane: x >= 320 */
  frame();
  {
    const double eq[4] = {1, 0, 0, -320};
    glClipPlane(GL_CLIP_PLANE0, eq);
  }
  glEnable(GL_CLIP_PLANE0);
  glColor3f(1, 1, 0);
  quad(0, 0, W, H, 0);
  CHECK_PIX(100, 224, 0x000000);
  CHECK_PIX(500, 224, 0x00FFFF);

  /* lines and points */
  frame();
  glBegin(GL_LINES);
  glVertex2f(10, 50.5f), glVertex2f(600, 50.5f);
  glEnd();
  {
    int hit = 0;
    for (int y = 49; y <= 52; y++)
      hit |= near(pix(300, y), 0xFFFFFF);
    CHECK(hit);
  }
  glBegin(GL_POINTS);
  glVertex2f(400.5f, 300.5f);
  glEnd();
  {
    int hit = 0;
    for (int y = 299; y <= 302; y++)
      for (int x = 399; x <= 402; x++)
        hit |= near(pix(x, y), 0xFFFFFF);
    CHECK(hit);
  }

  /* a texture (left red, right green) and the texture matrix */
  frame();
  {
    static u8 tex[8 * 8 * 4];
    for (int i = 0; i < 64; i++) {
      u8 *p = tex + 4 * i;
      int right = (i & 7) >= 4;
      p[0] = right ? 0 : 255, p[1] = right ? 255 : 0, p[2] = 0, p[3] = 255;
    }
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad(100, 100, 200, 200, 0);
    CHECK_PIX(120, 150, 0x0000FF);
    CHECK_PIX(180, 150, 0x00FF00);
    /* S + 0.5: the halves change places */
    glMatrixMode(GL_TEXTURE);
    glTranslatef(0.5f, 0, 0);
    glMatrixMode(GL_MODELVIEW);
    quad(300, 100, 400, 200, 0);
    CHECK_PIX(320, 150, 0x00FF00);
    CHECK_PIX(380, 150, 0x0000FF);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    /* a side that is not a power of two (12 x 12): made 16 x 16 */
    static u8 t12[12 * 12 * 4];
    memset(t12, 255, sizeof t12);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 12, 12, 0, GL_RGBA, GL_UNSIGNED_BYTE, t12);
    quad(100, 250, 200, 350, 0);
    CHECK_PIX(150, 300, 0xFFFFFF);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &id);
  }

  /* glDrawElements of a strip */
  frame();
  {
    static const float v[4][3] = {{100, 100, 0}, {200, 100, 0}, {100, 200, 0}, {200, 200, 0}};
    static const unsigned short ix[4] = {0, 1, 2, 3};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, v);
    glColor3f(0, 1, 1);
    glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, ix);
    glDisableClientState(GL_VERTEX_ARRAY);
    CHECK_PIX(150, 150, 0xFFFF00);
  }

  /* a display list: the draw recorded (GL_COMPILE draws nothing now, and
     the state is back after it), drawn by glCallList in another place */
  frame();
  {
    GLuint l = glGenLists(1);
    CHECK(l != 0);
    glNewList(l, GL_COMPILE);
    glColor3f(1, 0, 1);
    quad(100, 100, 200, 200, 0);
    glEnable(GL_BLEND);
    glEndList();
    CHECK(!glIsEnabled(GL_BLEND));
    CHECK_PIX(150, 150, 0x000000);
    glCallList(l);
    CHECK_PIX(150, 150, 0xFF00FF);
    CHECK(glIsEnabled(GL_BLEND)); /* the list's state */
    glDisable(GL_BLEND);
    glMatrixMode(GL_MODELVIEW);
    glTranslatef(200, 0, 0);
    glCallList(l);
    glLoadIdentity();
    CHECK_PIX(350, 150, 0xFF00FF);
    glDeleteLists(l, 1);
    CHECK(!glIsList(l));
  }

  /* GL_COLOR_INDEX texels by the shared palette */
  frame();
  {
    static u8 pal[256 * 3], ix[8 * 8];
    for (int i = 0; i < 256; i++)
      pal[3 * i] = (u8)i, pal[3 * i + 1] = (u8)(255 - i), pal[3 * i + 2] = 0;
    glColorTableEXT(GL_SHARED_TEXTURE_PALETTE_EXT, GL_RGB, 256, GL_RGB, GL_UNSIGNED_BYTE, pal);
    glEnable(GL_SHARED_TEXTURE_PALETTE_EXT);
    memset(ix, 255, sizeof ix);
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX8_EXT, 8, 8, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, ix);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad(100, 100, 200, 200, 0);
    CHECK_PIX(150, 150, 0x0000FF);
    glDisable(GL_SHARED_TEXTURE_PALETTE_EXT);
    glDeleteTextures(1, &id);

    /* GL_OES_compressed_paletted_texture: 16 colours, 4-bit texels, and its
       level 1 (index 2: blue) */
    static u8 blob[16 * 3 + 32 + 8];
    memset(blob, 0, sizeof blob);
    blob[3 * 1 + 1] = 255;               /* 1: green */
    blob[3 * 2 + 2] = 255;               /* 2: blue */
    memset(blob + 48, 0x11, 32);         /* level 0, 8 x 8: all 1 */
    memset(blob + 48 + 32, 0x22, 8);     /* level 1, 4 x 4 (below 8: not kept) */
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glCompressedTexImage2D(GL_TEXTURE_2D, -1, GL_PALETTE4_RGB8_OES, 8, 8, 0, sizeof blob, blob);
    CHECK_EQ(glGetError(), GL_NO_ERROR);
    quad(300, 100, 400, 200, 0);
    CHECK_PIX(350, 150, 0x00FF00);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &id);
  }

  /* multitexture: unit 1's luminance texture (half) multiplies unit 0's */
  frame();
  {
    static u8 white[8 * 8 * 4], half[8 * 8];
    memset(white, 255, sizeof white);
    memset(half, 128, sizeof half);
    GLuint id[2];
    glGenTextures(2, id);
    glBindTexture(GL_TEXTURE_2D, id[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, id[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8, 8, 8, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, half);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glBegin(GL_QUADS);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 0), glMultiTexCoord2f(GL_TEXTURE1, 0, 0), glVertex2f(100, 100);
    glMultiTexCoord2f(GL_TEXTURE0, 1, 0), glMultiTexCoord2f(GL_TEXTURE1, 1, 0), glVertex2f(200, 100);
    glMultiTexCoord2f(GL_TEXTURE0, 1, 1), glMultiTexCoord2f(GL_TEXTURE1, 1, 1), glVertex2f(200, 200);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 1), glMultiTexCoord2f(GL_TEXTURE1, 0, 1), glVertex2f(100, 200);
    glEnd();
    CHECK_PIX(150, 150, 0x808080);
    /* RGB_SCALE 2 on unit 1: the lightmap up to 2x */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 2);
    glMatrixMode(GL_MODELVIEW);
    glTranslatef(200, 0, 0);
    glBegin(GL_QUADS);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 0), glMultiTexCoord2f(GL_TEXTURE1, 0, 0), glVertex2f(100, 100);
    glMultiTexCoord2f(GL_TEXTURE0, 1, 0), glMultiTexCoord2f(GL_TEXTURE1, 1, 0), glVertex2f(200, 100);
    glMultiTexCoord2f(GL_TEXTURE0, 1, 1), glMultiTexCoord2f(GL_TEXTURE1, 1, 1), glVertex2f(200, 200);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 1), glMultiTexCoord2f(GL_TEXTURE1, 0, 1), glVertex2f(100, 200);
    glEnd();
    glLoadIdentity();
    CHECK_PIX(350, 150, 0xFFFFFF);
    glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(2, id);
  }

  /* glMultiDrawArrays, glMultiModeDrawArraysIBM, glDrawRangeElements and
     glDrawArrays(GL_POLYGON) (queued) */
  frame();
  {
    static const float v[8][3] = {{100, 100, 0}, {150, 100, 0}, {100, 150, 0}, {150, 150, 0},
                                  {300, 100, 0}, {350, 100, 0}, {350, 150, 0}, {300, 150, 0}};
    static const GLint first[2] = {0, 4};
    static const GLsizei count[2] = {4, 4};
    static const GLenum modes[2] = {GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, v);
    glColor3f(1, 1, 1);
    glMultiModeDrawArraysIBM(modes, first, count, 2, sizeof(GLenum));
    CHECK_PIX(125, 125, 0xFFFFFF);
    CHECK_PIX(325, 125, 0xFFFFFF);
    glColor3f(1, 0, 0);
    glMultiDrawArrays(GL_TRIANGLE_FAN, first + 1, count + 1, 1);
    CHECK_PIX(325, 125, 0x0000FF);
    static const GLuint ix[4] = {0, 1, 2, 3};
    glColor3f(0, 0, 1);
    glDrawRangeElements(GL_TRIANGLE_STRIP, 0, 3, 4, GL_UNSIGNED_INT, ix);
    CHECK_PIX(125, 125, 0xFF0000);
    /* polygons with texture coordinates: the queue */
    static float poly[2][4][5];
    for (int k = 0; k < 2; k++)
      for (int i = 0; i < 4; i++) {
        float *p = poly[k][i];
        p[0] = (float)(100 + 200 * k + (i == 1 || i == 2 ? 50 : 0));
        p[1] = (float)(300 + (i >= 2 ? 50 : 0));
        p[2] = 0, p[3] = p[4] = 0;
      }
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor3f(0, 1, 0);
    for (int k = 0; k < 2; k++) {
      glVertexPointer(3, GL_FLOAT, 20, poly[k][0]);
      glTexCoordPointer(2, GL_FLOAT, 20, &poly[k][0][3]);
      glDrawArrays(GL_POLYGON, 0, 4);
    }
    /* changed after the call: GL took them at the call */
    poly[1][0][0] = poly[1][3][0] = 600;
    CHECK_PIX(125, 325, 0x00FF00);
    CHECK_PIX(325, 325, 0x00FF00);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
  }

  /* locked arrays (the transform kept): a fan as a triangle list (strips
     from the index list), drawn twice, the second with another matrix */
  frame();
  {
    static float v[6][3];
    static GLuint tri[12];
    for (int i = 0; i < 6; i++) {
      float a = (float)i * 6.2831853f / 6;
      v[i][0] = 150 + 40 * __builtin_cosf(a), v[i][1] = 150 + 40 * __builtin_sinf(a), v[i][2] = 0;
    }
    for (int t = 0; t < 4; t++)
      tri[3 * t] = 0, tri[3 * t + 1] = (GLuint)t + 1, tri[3 * t + 2] = (GLuint)t + 2;
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, v);
    glLockArraysEXT(0, 6);
    glColor3f(1, 1, 0);
    glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_INT, tri);
    glColor3f(0, 1, 1);
    glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_INT, tri);
    glMatrixMode(GL_MODELVIEW);
    glTranslatef(200, 0, 0);
    glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_INT, tri);
    glLoadIdentity();
    glUnlockArraysEXT();
    glDisableClientState(GL_VERTEX_ARRAY);
    CHECK_PIX(160, 160, 0xFFFF00);
    CHECK_PIX(360, 160, 0xFFFF00);
    CHECK_PIX(140, 150, 0xFFFF00);
  }

  /* glTexSubImage2D of a resident texture: two lines changed */
  frame();
  {
    static u8 red[8 * 8 * 4], green[8 * 2 * 4];
    for (int i = 0; i < 64; i++)
      red[4 * i] = 255, red[4 * i + 3] = 255;
    for (int i = 0; i < 16; i++)
      green[4 * i + 1] = 255, green[4 * i + 3] = 255;
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad(100, 100, 180, 180, 0);
    CHECK_PIX(140, 105, 0x0000FF);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 4, 8, 2, GL_RGBA, GL_UNSIGNED_BYTE, green);
    quad(300, 100, 380, 180, 0);
    CHECK_PIX(340, 145, 0x00FF00); /* lines 4, 5: y 140 .. 160 */
    CHECK_PIX(340, 105, 0x0000FF);
    CHECK_PIX(340, 175, 0x0000FF);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &id);
  }

  /* more textures than GS memory holds (30 x 128 x 128 x 32 bits, with
     their levels): the ring wraps, the textures in the way go and come
     back when drawn again */
  frame();
  {
    static u8 big[128 * 128 * 4];
    static GLuint id[30];
    glGenTextures(30, id);
    for (int k = 0; k < 30; k++) {
      for (int i = 0; i < 128 * 128; i++) {
        u8 *p = big + 4 * i;
        p[0] = (u8)(k * 8), p[1] = (u8)(255 - k * 8), p[2] = (u8)(k & 1 ? 255 : 0);
        p[3] = 200; /* not opaque: 32 bits */
      }
      glBindTexture(GL_TEXTURE_2D, id[k]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
    }
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    for (int pass = 0; pass < 3; pass++)
      for (int k = 0; k < 30; k++) {
        int x = 20 + (k % 10) * 60, y = 20 + (k / 10) * 60;
        glBindTexture(GL_TEXTURE_2D, id[(k * 7 + pass * 11) % 30]);
        quad((float)x, (float)y, (float)x + 50, (float)y + 50, 0);
      }
    for (int k = 0; k < 30; k++) {
      int x = 20 + (k % 10) * 60, y = 20 + (k / 10) * 60, t = (k * 7 + 2 * 11) % 30;
      u32 want = (u32)(t * 8) | (u32)(255 - t * 8) << 8 | (u32)(t & 1 ? 255 : 0) << 16;
      CHECK_PIX(x + 25, y + 25, want);
    }
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(30, id);
  }

  /* buffer objects: a strip from one, its indices from another; polygons
     from one (queued as pointers into it) */
  frame();
  {
    static const float v[4][5] = {{100, 100, 0, 0, 0}, {200, 100, 0, 1, 0}, {100, 200, 0, 0, 1},
                                  {200, 200, 0, 1, 1}};
    static const unsigned short ix[4] = {0, 1, 2, 3};
    GLuint b[2];
    glGenBuffers(2, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof ix, ix, GL_STATIC_DRAW);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 20, (const void *)0);
    glColor3f(1, 0, 1);
    glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, (const void *)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    CHECK_PIX(150, 150, 0xFF00FF);
    /* two polygons (quads) of one buffer, textured: the queue's pointers */
    static float poly[8][5];
    for (int k = 0; k < 2; k++)
      for (int i = 0; i < 4; i++) {
        float *p = poly[4 * k + i];
        p[0] = (float)(300 + 150 * k + (i == 1 || i == 2 ? 50 : 0));
        p[1] = (float)(300 + (i >= 2 ? 50 : 0));
        p[2] = 0, p[3] = p[4] = 0.5f;
      }
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof poly, poly, GL_STATIC_DRAW);
    static u8 white[8 * 8 * 4];
    memset(white, 255, sizeof white);
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 20, (const void *)0);
    glTexCoordPointer(2, GL_FLOAT, 20, (const void *)12);
    glColor3f(0, 1, 0);
    static const GLint first[2] = {0, 4};
    static const GLsizei count[2] = {4, 4};
    glMultiDrawArrays(GL_POLYGON, first, count, 2);
    CHECK_PIX(325, 325, 0x00FF00);
    CHECK_PIX(475, 325, 0x00FF00);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &id);
    glDeleteBuffers(2, b);
    CHECK(!glIsBuffer(b[0]));
  }

  /* a list of two draws of the same arrays and triangles (a fan as a
     list: strips; the second takes the first's positions), and one
     across the guard band (its call clipped on the EE, from its triangles) */
  frame();
  {
    static const float v[4][3] = {{100, 100, 0}, {200, 100, 0}, {200, 200, 0}, {100, 200, 0}};
    static const GLuint ix[6] = {0, 1, 2, 0, 2, 3};
    static const float big[4][3] = {{300, 100, 0}, {5000, 100, 0}, {5000, 200, 0}, {300, 200, 0}};
    GLuint l = glGenLists(2);
    glEnableClientState(GL_VERTEX_ARRAY);
    glNewList(l, GL_COMPILE);
    glVertexPointer(3, GL_FLOAT, 0, v);
    glColor3f(1, 0, 0);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, ix);
    glColor3f(0, 0, 1);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, ix);
    glEndList();
    glNewList(l + 1, GL_COMPILE);
    glVertexPointer(3, GL_FLOAT, 0, big);
    glColor3f(1, 1, 0);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, ix);
    glEndList();
    glDisableClientState(GL_VERTEX_ARRAY);
    CHECK_EQ(glGetError(), GL_NO_ERROR);
    glCallList(l);
    glCallList(l + 1);
    CHECK_PIX(120, 120, 0xFF0000);
    CHECK_PIX(180, 180, 0xFF0000);
    CHECK_PIX(400, 150, 0x00FFFF);
    CHECK_PIX(630, 150, 0x00FFFF);
    glDeleteLists(l, 2);
  }

  /* a grid of 20 x 20 quads (800 triangles: several batches) as a list
     (strips) and drawn as it is: the same pixels */
  frame();
  {
    static float gv[21 * 21][3];
    static u8 gc[21 * 21][4];
    static GLuint gi[20 * 20 * 6];
    for (int y = 0; y <= 20; y++)
      for (int x = 0; x <= 20; x++) {
        float *p = gv[y * 21 + x];
        p[0] = (float)(x * 12), p[1] = (float)(y * 12), p[2] = 0;
        u8 *c = gc[y * 21 + x];
        c[0] = (u8)(x * 12), c[1] = (u8)(y * 12), c[2] = (u8)((x + y) * 6), c[3] = 255;
      }
    int n = 0;
    for (int y = 0; y < 20; y++)
      for (int x = 0; x < 20; x++) {
        GLuint a = (GLuint)(y * 21 + x), b = a + 1, c = a + 21, d = c + 1;
        gi[n++] = a, gi[n++] = b, gi[n++] = d, gi[n++] = a, gi[n++] = d, gi[n++] = c;
      }
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, gv);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, gc);
    GLuint l = glGenLists(1);
    glNewList(l, GL_COMPILE);
    glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_INT, gi);
    glEndList();
    glMatrixMode(GL_MODELVIEW);
    glTranslatef(20, 20, 0);
    glCallList(l);
    glLoadIdentity();
    glTranslatef(320, 20, 0);
    glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_INT, gi);
    glLoadIdentity();
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    int bad = 0;
    for (int y = 3; y < 240; y += 7)
      for (int x = 3; x < 240; x += 7) {
        u32 a = pix(20 + x, 20 + y), b = pix(320 + x, 20 + y);
        if (!near(a, b) && bad++ < 4)
          mx_printf("grid: (%d, %d) list %06x drawn %06x\n", x, y, a, b);
      }
    CHECK_EQ(bad, 0);
    /* the list across the guard band (clipped and culled on the EE from its
       triangles): front faces stay */
    frame();
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glMatrixMode(GL_MODELVIEW);
    glTranslatef(20, 20, 0);
    glScalef(30, 1, 1); /* to x 7200: past the guard band */
    glCallList(l);
    glLoadIdentity();
    glDisable(GL_CULL_FACE);
    CHECK(!near(pix(400, 100), 0));
    CHECK(!near(pix(600, 200), 0));
    glDeleteLists(l, 1);
  }

  /* the calls by name, and the extensions */
  CHECK(mxglGetProcAddress("glActiveTextureARB") == (void (*)(void))glActiveTextureARB);
  CHECK(mxglGetProcAddress("glMultiModeDrawArraysIBM") != NULL);
  CHECK(mxglGetProcAddress("glNoSuchCall") == NULL);
  CHECK(strstr((const char *)glGetString(GL_EXTENSIONS), "GL_OES_compressed_paletted_texture") != NULL);

  /* errors */
  (void)glGetError();
  glEnable(0x1234);
  CHECK_EQ(glGetError(), GL_INVALID_ENUM);
  CHECK_EQ(glGetError(), GL_NO_ERROR);
  {
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    CHECK(vp[2] == W && vp[3] == H);
  }

  mxgl_end_frame();
  /* mxgl_color_light: white times a GL_RGB8 texture without MIPMAP (a
     lightmap) by blendFunc ZERO SRC_COLOR, a channel a pass; then the
     texture changed (glTexSubImage2D): its channels follow */
  frame();
  {
    static u8 lm[8 * 8 * 4];
    for (int i = 0; i < 64; i++)
      lm[i * 4] = 200, lm[i * 4 + 1] = 100, lm[i * 4 + 2] = 50, lm[i * 4 + 3] = 255;
    mxgl_color_light(1);
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, lm);
    quad(100, 100, 200, 200, 0);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    quad(100, 100, 200, 200, 0);
    CHECK_PIX(150, 150, 0x3264C8);
    for (int i = 0; i < 64; i++)
      lm[i * 4] = 50, lm[i * 4 + 1] = 200, lm[i * 4 + 2] = 100;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, lm);
    glDisable(GL_TEXTURE_2D), glDisable(GL_BLEND);
    quad(300, 100, 400, 200, 0);
    glEnable(GL_TEXTURE_2D), glEnable(GL_BLEND);
    quad(300, 100, 400, 200, 0);
    CHECK_PIX(350, 150, 0x64C832);
    glDisable(GL_TEXTURE_2D), glDisable(GL_BLEND);
    /* in a display list (called in a later frame), then a plain grey quad:
       not tinted (the colour mask and texture back) */
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBindTexture(GL_TEXTURE_2D, id);
    glEnable(GL_TEXTURE_2D), glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    quad(100, 250, 200, 350, 0);
    glDisable(GL_TEXTURE_2D), glDisable(GL_BLEND);
    glEndList();
    frame();
    quad(100, 250, 200, 350, 0);
    glCallList(list);
    glColor4f(0.5f, 0.5f, 0.5f, 1);
    quad(300, 250, 400, 350, 0);
    glColor4f(1, 1, 1, 1);
    CHECK_PIX(150, 300, 0x64C832);
    CHECK_PIX(350, 300, 0x808080);
    glDeleteLists(list, 1);
    /* texture unit 1 (a texture and a lightmap): white times the lightmap */
    static u8 white[8 * 8 * 4];
    memset(white, 255, sizeof white);
    GLuint base;
    glGenTextures(1, &base);
    glBindTexture(GL_TEXTURE_2D, base);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, id);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glBegin(GL_QUADS);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 0), glMultiTexCoord2f(GL_TEXTURE1, 0, 0), glVertex2f(500, 250);
    glMultiTexCoord2f(GL_TEXTURE0, 1, 0), glMultiTexCoord2f(GL_TEXTURE1, 1, 0), glVertex2f(600, 250);
    glMultiTexCoord2f(GL_TEXTURE0, 1, 1), glMultiTexCoord2f(GL_TEXTURE1, 1, 1), glVertex2f(600, 350);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 1), glMultiTexCoord2f(GL_TEXTURE1, 0, 1), glVertex2f(500, 350);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_TEXTURE_2D);
    CHECK_PIX(550, 300, 0x64C832);
    /* a list of both: the texture, then the lightmap's product (Quake III's
       world surfaces), called in a later frame */
    list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBindTexture(GL_TEXTURE_2D, base);
    glEnable(GL_TEXTURE_2D);
    quad(100, 100, 200, 200, 0);
    glBindTexture(GL_TEXTURE_2D, id);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    glDepthFunc(GL_LEQUAL);
    quad(100, 100, 200, 200, 0);
    glDisable(GL_TEXTURE_2D), glDisable(GL_BLEND);
    glEndList();
    frame();
    glCallList(list);
    CHECK_PIX(150, 150, 0x64C832);
    frame();
    glCallList(list);
    CHECK_PIX(150, 150, 0x64C832);
    glDeleteLists(list, 1);
    glDeleteTextures(1, &base);
    glDeleteTextures(1, &id);
    mxgl_color_light(0);
  }

  /* a vertex program (GL_PS2_vertex_program_binary): the colour as
     col * cmul + cadd (inverted here); glUseProgram(0) draws as before;
     a display list draws as recorded with a program in use */
  {
    extern const struct mxgl_vu1_binary glprog_vu1;
    frame();
    CHECK(strstr((const char *)glGetString(GL_EXTENSIONS), "GL_PS2_vertex_program_binary") != NULL);
    GLuint sh = glCreateShader(GL_VERTEX_SHADER), prog = glCreateProgram();
    glShaderBinary(1, &sh, GL_PS2_VU1_BINARY, &glprog_vu1, sizeof glprog_vu1);
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    CHECK_EQ(ok, 1);
    GLint cmul = glGetUniformLocation(prog, "cmul"), cadd = glGetUniformLocation(prog, "cadd");
    CHECK(cmul >= 0 && cadd >= 0);
    CHECK_EQ(glGetUniformLocation(prog, "nothing"), -1);
    CHECK_EQ(glGetAttribLocation(prog, "COLOR"), 2);
    glUseProgram(prog);
    glUniform4f(cmul, -1, -1, -1, 1);
    glUniform4f(cadd, 255, 255, 255, 0);
    glColor3ub(51, 102, 153);
    quad(100, 100, 200, 200, 0);
    /* the uniforms changed between two draws: each has its own */
    glUniform4f(cmul, 0, 0, 0, 1);
    glUniform4f(cadd, 0, 255, 0, 0);
    quad(300, 100, 400, 200, 0);
    glUseProgram(0);
    quad(500, 100, 600, 200, 0);
    CHECK_PIX(150, 150, 0x6699CC);
    CHECK_PIX(350, 150, 0x00FF00);
    CHECK_PIX(550, 150, 0x996633);
    /* a list, called with the program in use and without */
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glColor3ub(51, 102, 153);
    quad(100, 250, 200, 350, 0);
    glEndList();
    frame();
    glUseProgram(prog);
    glUniform4f(cmul, -1, -1, -1, 1);
    glUniform4f(cadd, 255, 255, 255, 0);
    glCallList(list);
    glUseProgram(0);
    glTranslatef(200, 0, 0);
    glCallList(list);
    glLoadIdentity();
    CHECK_PIX(150, 300, 0x996633);
    CHECK_PIX(350, 300, 0x996633);
    /* and a draw after the list: the program again */
    glUseProgram(prog);
    glColor3ub(51, 102, 153);
    quad(500, 250, 600, 350, 0);
    glUseProgram(0);
    CHECK_PIX(550, 300, 0x6699CC);
    glDeleteLists(list, 1);
    glDeleteProgram(prog);
    glDeleteShader(sh);
    CHECK_EQ(glGetError(), GL_NO_ERROR);
  }

  return t_done("glconf");
}
