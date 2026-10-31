/* mxgl.h: an OpenGL 1.1 subset on the PS2's GS (src/mxgl.c). The GL calls
   are the usual ones (<GL/gl.h>); these are the PS2's own. */
#ifndef MXGL_H
#define MXGL_H

/* the largest texture side the games should make (they scale larger ones
   down: GL_MAX_TEXTURE_SIZE) */
#define MXGL_MAX_TEXTURE 128
/* the window the game sees: the GS draws 640 x 448; with MXGL_HIRES
   1920 x 1080 interlaced (1920 x 540 a field) */
#ifdef MXGL_HIRES
#define MXGL_WIDTH 1920
#define MXGL_HEIGHT 1080
#else
#define MXGL_WIDTH 640
#define MXGL_HEIGHT 448
#endif

/* GS, VU1 and the frame pipeline */
void mxgl_init(void);
/* the window the game sees, w x h (MXGL_WIDTH x MXGL_HEIGHT by default),
   after mxgl_init: its coordinates (viewport, scissor, 2D) are mapped onto
   the whole picture, which is drawn at its own resolution; a smaller window
   makes the game's 2D (in pixels of its window) larger */
void mxgl_window(int w, int h);
/* the end of a frame (the game's swap buffers): sends it */
void mxgl_end_frame(void);
/* after mxgl_end_frame, once the game is done with the frame's data:
   frees what the frames before used */
void mxgl_frame_done(void);
/* the frame shown last as a 24-bit TGA file ("host:..."); stops the
   pipeline (for tests: the program ends after it) */
int mxgl_screenshot(const char *path);
/* the counters on the screen (frames a second, EE and GL time, draws ...),
   and once a second in the log */
extern int mxgl_show_stats;

/* display lists: the draws between NewList and EndList are recorded, not
   drawn; CallList draws them with the matrices of then. NewList returns 0
   if it cannot start the list, EndList 0 if not all draws fitted in (the
   list is then empty). */
int mxgl_NewList(int id);
int mxgl_EndList(void);
void mxgl_CallList(int id);
void mxgl_DeleteAllLists(void);
/* 1 once textures have gone (the lists name them): make the lists again */
int mxgl_ListsDirty(void);

/* a convex polygon of n vertices at v (x, y, z at v[0..2], s, t at
   v[st..st+1]; stride floats from one vertex to the next), in the current
   colour: glBegin(GL_POLYGON) .. glEnd in one call, from the game's own
   vertex array (Quake's glpoly_t) */
void mxgl_DrawPolygon(const float *v, int n, int stride, int st);

/* n primitives from the current arrays (glVertexPointer, ...): primitive i
   is mode[i] (GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_TRIANGLES) of count[i]
   vertices from first[i] (glMultiDrawArrays with a mode each): a model's
   strips and fans in one call */
void mxgl_MultiDraw(const unsigned int *mode, const int *first, const int *count, int n); /* GLenum */

/* strips and fans from a command list (Quake 2's models): a count (< 0: a
   fan), then count times s, t (floats) and a vertex index; 0 ends it. The
   vertices: positions (x, y, z at pos, stride bytes apart) and colours
   (bytes R, G, B, A in col), nv of them, each transformed once */
void mxgl_DrawGlCmds(const int *cmds, const float *pos, int stride, const unsigned int *col, int nv);

/* the game's palette (256 RGB): textures whose texels are all of it are
   kept as its indices (8 bits, exact; half the GS memory of 16-bit and
   faster to draw), their mipmap levels as the nearest of its colours */
void mxgl_Palette(const unsigned char *rgb);

/* the next glTexImage2D keeps the texture as its brightness (the largest
   of R, G, B), for the blends with the destination colour (lightmaps:
   blendFunc DST_COLOR ZERO, ZERO SRC_COLOR, ZERO ONE_MINUS_SRC_COLOR), which
   the GS draws as Cd * As. GL_LUMINANCE textures are kept so anyway. */
void mxgl_TexBrightness(void);
/* colours of the draws after scaled by 1 << s (s 0 or 1): the overbright
   range of hardware gamma, by the GS's modulate (0x80 is 1.0); with s 1 a
   brightness texture's product is up to 2x */
void mxgl_ColorShift(int s);

#endif
