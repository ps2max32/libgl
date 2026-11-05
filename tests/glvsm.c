/* glvsm: a hand-written VU1 program (glvsm.vsm) through glShaderBinary, drawn
   next to libgl's own: the same scene with each, read back, must be the same
   pixels. The scene has what a batch can hold: colours, a texture, perspective,
   strips restarted by ADC, and more vertices than a batch. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <string.h>

#include "mxgl.h"
#include "t.h"

#define W 640
#define H 448
#define RW 256
#define RH 192
extern const struct mxgl_vu1_binary glvsm_vu1;
static u8 a[RW * RH * 4], b[RW * RH * 4];
static const u32 texels[4] = {0xFF2040E0u, 0xFFE04020u, 0xFF20E040u, 0xFFE0E0E0u};

static void scene(void) {
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glFrustum(-1, 1, -0.75, 0.75, 1, 100);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_TEXTURE_2D);
  /* a strip of 300 vertices, tilted: more than one batch */
  glBegin(GL_TRIANGLE_STRIP);
  for (int i = 0; i < 300; i++) {
    float x = -3 + 6 * (float)(i / 2) / 149, y = (i & 1) ? 2.0f : -2.0f, z = -3 - x;
    glColor3ub((GLubyte)(i * 3), (GLubyte)(255 - i), (GLubyte)(i & 1 ? 200 : 40));
    glTexCoord2f(x, y);
    glVertex3f(x, y, z);
  }
  glEnd();
  glDisable(GL_TEXTURE_2D);
  /* separate triangles and quads: strips restarted by ADC */
  glBegin(GL_TRIANGLES);
  for (int i = 0; i < 90; i++) {
    float x = -1.5f + 0.3f * (float)(i % 10), y = -1 + 0.6f * (float)(i / 30), z = -2.5f;
    glColor3ub(255, (GLubyte)(i * 2), 0);
    glVertex3f(x, y, z);
    glColor3ub(0, 255, (GLubyte)(i * 2));
    glVertex3f(x + 0.2f, y, z);
    glColor3ub((GLubyte)(i * 2), 0, 255);
    glVertex3f(x, y + 0.2f, z);
  }
  glEnd();
  glDisable(GL_DEPTH_TEST);
}

static void shot(u8 *to) {
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  scene();
  glReadPixels((W - RW) / 2, (H - RH) / 2, RW, RH, GL_RGBA, GL_UNSIGNED_BYTE, to);
  mxgl_end_frame();
  mxgl_frame_done();
}

int main(void) {
  mxgl_init();
  mxgl_show_stats = 0;
  mxgl_window(W, H);
  glViewport(0, 0, W, H);
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
  GLuint sh = glCreateShader(GL_VERTEX_SHADER), prog = glCreateProgram();
  glShaderBinary(1, &sh, GL_PS2_VU1_BINARY, &glvsm_vu1, sizeof glvsm_vu1);
  glAttachShader(prog, sh);
  glLinkProgram(prog);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  CHECK_EQ(ok, 1);
  GLint cmul = glGetUniformLocation(prog, "cmul"), cadd = glGetUniformLocation(prog, "cadd");
  CHECK(cmul >= 0 && cadd >= 0);

  shot(a);
  glUseProgram(prog);
  glUniform4f(cmul, 1, 1, 1, 1);
  glUniform4f(cadd, 0, 0, 0, 0);
  shot(b);
  glUseProgram(0);

  u32 lit = 0, diff = 0;
  for (u32 i = 0; i < RW * RH; i++) {
    lit += a[4 * i] | a[4 * i + 1] | a[4 * i + 2] ? 1 : 0;
    diff += memcmp(a + 4 * i, b + 4 * i, 3) != 0;
  }
  mx_printf("glvsm: %u of %u pixels drawn, %u differ\n", lit, RW * RH, diff);
  CHECK(lit > RW * RH / 2);
  CHECK_EQ(diff, 0);
  CHECK_EQ(glGetError(), GL_NO_ERROR);
  return t_done("glvsm");
}
