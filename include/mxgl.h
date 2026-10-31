/* mxgl.h: an OpenGL 1.1 subset on the PS2's GS (src/mxgl.c). The GL calls
   are the usual ones (<GL/gl.h>); these are the PS2's own. */
#ifndef MXGL_H
#define MXGL_H

/* the largest texture side the games should make (they scale larger ones
   down: GL_MAX_TEXTURE_SIZE) */
#define MXGL_MAX_TEXTURE 128
/* the window the game sees: the GS draws 640 x 448 */
#define MXGL_WIDTH 640
#define MXGL_HEIGHT 448

/* GS, VU1 and the frame pipeline */
void mxgl_init(void);
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
