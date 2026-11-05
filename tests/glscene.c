/* glscene: a scene through the clipper, the picture of one fog mode (built with
   -DMODE=0..3: none, linear, exp, exp2; a screenshot ends a program): a floor from
   behind the eye to the far plane (cut by the near plane and the guard band), a culled
   cube, strips, fans and indexed triangles. The pictures (host:glscene<n>.tga) are for
   comparing libgl's EE clipping with VU1's (MXGL_VU1CLIP=1). */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "mxgl.h"
#include "t.h"

#define W 640
#define H 448
static u32 tex[16 * 16];

static void floor_(void) {
  glEnable(GL_TEXTURE_2D);
  glColor3ub(255, 255, 255);
  glBegin(GL_QUADS);
  for (int z = -30; z < 6; z++)
    for (int x = -12; x < 12; x++) {
      float x0 = (float)x * 2, x1 = x0 + 2, z0 = (float)z * 2, z1 = z0 + 2;
      glTexCoord2f(x0, z1), glVertex3f(x0, -1, z1);
      glTexCoord2f(x1, z1), glVertex3f(x1, -1, z1);
      glTexCoord2f(x1, z0), glVertex3f(x1, -1, z0);
      glTexCoord2f(x0, z0), glVertex3f(x0, -1, z0);
    }
  glEnd();
  glDisable(GL_TEXTURE_2D);
}

static void cube(float cx, float cy, float cz, float s) {
  static const signed char f[6][4][3] = {
      {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},     {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
      {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},     {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
      {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},     {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}}};
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glBegin(GL_QUADS);
  for (int i = 0; i < 6; i++) {
    glColor3ub((GLubyte)(60 + 30 * i), (GLubyte)(250 - 35 * i), (GLubyte)(40 * i));
    for (int j = 0; j < 4; j++)
      glVertex3f(cx + s * f[i][j][0], cy + s * f[i][j][1], cz + s * f[i][j][2]);
  }
  glEnd();
  glDisable(GL_CULL_FACE);
}

/* a strip and a fan as arrays, through the near plane; indexed triangles culled */
static void arrays(void) {
  static float v[64][3];
  static GLubyte c[64][4];
  static GLushort ix[60 * 3];
  for (int i = 0; i < 64; i++) {
    v[i][0] = -6 + (float)(i / 2) * 0.4f, v[i][1] = (i & 1) ? 1.5f : 0.2f, v[i][2] = 2 - (float)(i / 2) * 0.7f;
    c[i][0] = (GLubyte)(i * 4), c[i][1] = 200, c[i][2] = (GLubyte)(255 - i * 4), c[i][3] = 255;
  }
  glEnableClientState(GL_VERTEX_ARRAY), glEnableClientState(GL_COLOR_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, v);
  glColorPointer(4, GL_UNSIGNED_BYTE, 0, c);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 64);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_FRONT);
  for (int i = 0; i < 60; i++)
    ix[3 * i] = (GLushort)i, ix[3 * i + 1] = (GLushort)(i + 1 + (i & 1)), ix[3 * i + 2] = (GLushort)(i + 2 - (i & 1));
  glTranslatef(6, -0.5f, -2);
  glDrawElements(GL_TRIANGLES, 60 * 3, GL_UNSIGNED_SHORT, ix);
  glDisable(GL_CULL_FACE);
  glDisableClientState(GL_VERTEX_ARRAY), glDisableClientState(GL_COLOR_ARRAY);
}

int main(void) {
  mxgl_init();
  mxgl_show_stats = 0;
  mxgl_window(W, H);
  glViewport(0, 0, W, H);
  for (int i = 0; i < 256; i++)
    tex[i] = ((i ^ i >> 4) & 1) ? 0xFF2060E0u : 0xFFE0D040u;
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
  static const GLint modes[4] = {0, GL_LINEAR, GL_EXP, GL_EXP2};
  static const float col[4] = {0.5f, 0.6f, 0.7f, 1};
  static char name[] = "host:glscene0.tga";
  {
    const int m = MODE;
    for (int f = 0; f < 3; f++) {
      glClearColor(0.5f, 0.6f, 0.7f, 1);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glMatrixMode(GL_PROJECTION);
      glLoadIdentity();
      glFrustum(-0.6, 0.6, -0.42, 0.42, 0.5, 80);
      glMatrixMode(GL_MODELVIEW);
      glLoadIdentity();
      glEnable(GL_DEPTH_TEST);
      if (modes[m]) {
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, modes[m]);
        glFogfv(GL_FOG_COLOR, col);
        glFogf(GL_FOG_START, 2), glFogf(GL_FOG_END, 40), glFogf(GL_FOG_DENSITY, 0.06f);
      }
      floor_();
      cube(-2, 0, -6, 1), cube(2.5f, 1, -14, 2);
      arrays();
      glDisable(GL_FOG), glDisable(GL_DEPTH_TEST);
      if (f < 2) {
        mxgl_end_frame();
        mxgl_frame_done();
      }
    }
    mxgl_end_frame();
    mxgl_frame_done();
    name[12] = (char)('0' + m);
    int r = mxgl_screenshot(name);
    mx_printf("glscene: %s %d\n", name, r);
    CHECK_EQ(r, 0);
  }
  CHECK_EQ(glGetError(), GL_NO_ERROR);
  return t_done("glscene");
}
