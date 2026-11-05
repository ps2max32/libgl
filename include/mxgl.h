/* mxgl.h: OpenGL on the PS2's GS (src/mxgl.c). The GL calls are the usual
   ones (<GL/gl.h>, <GL/glext.h>): GL 1.1 with GL 1.2 - 1.5 and OpenGL ES 1.1
   calls as extensions (glGetString(GL_EXTENSIONS), mxglGetProcAddress).
   These are the PS2's own: set-up, the end of a frame, the screen. */
#ifndef MXGL_H
#define MXGL_H

/* the largest texture side the games should make (they scale larger ones
   down: GL_MAX_TEXTURE_SIZE) */
#ifndef MXGL_MAX_TEXTURE
#define MXGL_MAX_TEXTURE 128
#endif
/* pinned memory besides the textures (<mx/mem.h>, taken as needed): two
   arenas, beam racing's 2 MiB while it is on, the display lists' pool (up
   to 2 MiB). For sizing; nothing is reserved. */
#define MXGL_CORE_BYTES (6u << 20)
/* GS, VU1 and the frame pipeline, in the console's TV mode (MXGL_SD).
   What the DMAC or VU0 reads (the arenas, textures, display lists, beam
   racing's records) is pinned memory (<mx/mem.h>): with maxsdk's virtual
   memory on (<mx/vm.h>) it is frames taken from the pool as needed, given
   back when freed. The game's own arrays may be virtual memory. */
void mxgl_init(void);
/* the picture, after mxgl_init: a mode, with flags or-ed in
   - MXGL_SD: NTSC or PAL, the console's own (its BIOS's region)
   - MXGL_NTSC: 640 x 448, 60 Hz; MXGL_PAL: 640 x 512, 50 Hz; interlaced
   - MXGL_1080I: 1920 x 1080 interlaced
   - MXGL_BEAM: beam racing (maxsdk's passes, <mx/hires.h>): one buffer of
     a field's lines (640 x 224 / 256, 1920 x 540), drawn a strip at a time
     just ahead of the beam, every field, from the frame recorded; each
     field's lines half a line apart, so all of the screen's lines are
     drawn. 16-bit Z. It leaves GS memory for more textures (SD: 3.6 MiB,
     1080i: 1.9 MiB; without: 1.8 / 1.5 MiB for NTSC / PAL), shows at most
     a frame a field, and takes 2 MiB of core memory more while it is on
     (the records and two more arenas; MXGL_REC_BYTES, MXGL_ARENA_BYTES:
     mk/mxgl.mk). glReadPixels gives zeros, the
     colour mask and the scissor are not kept.
   - MXGL_RGB24: with MXGL_BEAM in SD, 32-bit colour (and Z): 3.3 MiB left
     for textures; ignored otherwise
   Without MXGL_BEAM: two 16-bit buffers, 24-bit Z (SD), or in 1080i 640 x
   540 with 16-bit Z, stretched to the screen (1.9 MiB for textures).
   Between frames (a frame begun is ended first). GS memory is filled
   again (the textures are copied in as they are drawn), and the window is
   the whole screen (mxgl_screen). Returns 0, or -1 if beam racing could
   not be set (memory): the mode is set without it. */
enum { MXGL_SD, MXGL_NTSC, MXGL_PAL, MXGL_1080I };
#define MXGL_BEAM 0x100
#define MXGL_RGB24 0x200
int mxgl_set_mode(int mode);
/* the mode set: MXGL_NTSC, MXGL_PAL or MXGL_1080I */
int mxgl_mode(void);
/* the flags in effect: MXGL_BEAM, MXGL_RGB24 */
int mxgl_mode_flags(void);
/* the screen's size: 640 x 448, 640 x 512, 1920 x 1080 */
void mxgl_screen(int *w, int *h);
/* the window the game sees, w x h (the screen after mxgl_set_mode): its
   coordinates (viewport, scissor, 2D, glReadPixels) are mapped onto the
   whole picture, which is drawn at its own resolution; a smaller window
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
/* the counters on the screen, top left (1, the default) or not (0), and
   once a second in the log: frames shown a second; triangles, vertices and
   draws a frame; RAM as a bar (grey: the program, blue: the core heap in
   use, dark blue: free, green: VM pages or malloc, orange: pinned), with the
   VM in use, swap a second and the core heap; GS memory as a bar (grey: the
   screen, green: textures, red while they refill, yellow: uploaded a
   frame), with the texture room used, uploads and refills a frame */
extern int mxgl_show_stats;

/* Texture quality under memory pressure (maxsdk's texture cache,
   <mx/vcache.h>): the top levels left out of textures drawn with MIPMAP
   filtering: 0 (all, the default), 1 or 2 (blurrier; a quarter or a
   sixteenth of the GS memory and of the pages read), or MXGL_TEX_AUTO, two
   controls apart:
   - GS memory: W, the textures a frame uses, against the room for them.
     Up to 120% the frame streams what does not fit (they are in RAM).
     Above it for 2 s the textures are placed from coarser levels until W
     is about 80%: first the finest level their draws sample (no visible
     change), then the largest ones a level less; back once W at full
     quality is at most halfway (100%) for 3 s.
   - RAM: while the swap file is read (more than 30 pages a second, 3 s)
     the textures whose top level is not sampled are drawn from their
     smaller, pinned levels, their upper ones left to be paged out first;
     back after 5 quiet seconds.
   With virtual memory a texture's upper levels are pageable either way:
   while they are swapped out its smaller levels are drawn and they come
   back in the background. */
#define MXGL_TEX_AUTO (-1)
void mxgl_texture_quality(int q);
/* MXGL_TEX_AUTO's limits: W above vram_hi_pct % of the room lowers the
   quality to vram_lo_pct %, swap-ins above swap_ins_s pages a second make
   RAM cold (defaults 120, 80, 30) */
void mxgl_texture_limits(int vram_hi_pct, int vram_lo_pct, int swap_ins_s);
/* the levels left out now */
int mxgl_texture_bias(void);

/* Products with the destination colour of colour textures (blendFunc
   DST_COLOR ZERO, ZERO SRC_COLOR, ...: Quake 2's lightmaps) per channel: 1
   draws each such batch three times, one channel written in each (FBMSK)
   with that channel of the texture (coloured light; the batch's vertices
   once, its fill three times); 0 (the default) with its brightness (one
   pass, monochrome). Not with beam racing (MXGL_BEAM: the passes own
   FRAME): the brightness then. With it on, a texture of internal format
   GL_RGB8 / GL_RGBA8 without MIPMAP filtering when its image comes (a
   lightmap) is kept as its three channels, 8 bits each, one level (drawn
   alone, without a product, it shows its red channel as grey). */
void mxgl_color_light(int on);

/* a GL call by name (core and extension names, e.g. "glActiveTextureARB"),
   or NULL: the platform's GetProcAddress */
void (*mxglGetProcAddress(const char *name))(void);

/* ---- vertex programs (GL_PS2_vertex_program_binary) ----

   GL 2.0's vertex shaders as VU1 programs, compiled from HLSL by the build:
   glCreateShader(GL_VERTEX_SHADER), glShaderBinary(1, &sh,
   GL_PS2_VU1_BINARY, &NAME_vu1, sizeof NAME_vu1), glCreateProgram,
   glAttachShader, glLinkProgram, glUseProgram (0: fixed function again),
   glGetUniformLocation, glUniform*, glGetAttribLocation. Fragments stay
   fixed function (texture environment, blending, tests).

   A program's inputs are the fixed arrays' (glVertexPointer,
   glTexCoordPointer, glColorPointer), as libgl's own program (src/mxglv.hlsl)
   takes them: float4 POSITION (x, y, z, and w: 1 for a vertex that ends no
   triangle: the program's XYZ2 gets vu::adc(uint(pos.w))), float2 TEXCOORD,
   uint4 COLOR_V4_8U (0..255); a link fails for others. libgl transforms the
   positions on the EE for culling and clipping as for its own draws (the
   uniform mvp), so the program must place a vertex where mvp does (no
   deforms); a triangle cut by a plane gets vertices whose inputs are
   interpolated (exact for programs affine in their inputs). Uniforms libgl
   sets by name: mvp (float4x4: to GS coordinates times w), cscale (the
   texture environment's colour scale: GS colour = colour * cscale), tms,
   tmt (the texture matrix, as in mxglv.hlsl); the others glUniform* sets
   (a location is a float's index in the uniforms). A program whose
   uniforms are these four and two float4 more (mxglv.hlsl's cmul, cadd)
   has libgl's own program's data layout: switching between them costs no
   FLUSH (others: one a switch). Display lists draw as recorded, with
   libgl's own program, whatever program is in use.

   The build (mk/mxgl.mk): MXGL_PROGS lists the app's programs (NAME.hlsl
   in HLSL_DIRS); each gives `extern const struct mxgl_vu1_binary
   NAME_vu1;`. */
#define GL_PS2_VU1_BINARY 0x6A50
struct mxgl_vu1_uniform {
  const char *name;           /* the cbuffer member's, lower case */
  unsigned short at, qw;      /* quadwords */
};
struct mxgl_vu1_binary {
  unsigned magic;             /* MXGL_VU1_MAGIC */
  void (*shader)(void *s);    /* fills a struct mx_shader (<mx/frame.h>) */
  unsigned nuni;
  const struct mxgl_vu1_uniform *uni;
  unsigned nin;
  const char *const *in;      /* the inputs' semantics (POSITION, ...) */
};
#define MXGL_VU1_MAGIC 0x31555658u /* "XVU1" */

/* OpenGL ES 1.1's float forms (not in <GL/gl.h>), after it */
#ifdef GL_VERSION_1_1
void APIENTRY glClipPlanef(GLenum plane, const GLfloat *equation);
void APIENTRY glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
void APIENTRY glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
void APIENTRY glDepthRangef(GLclampf n, GLclampf f);
void APIENTRY glClearDepthf(GLclampf d);
#endif

/* ---- deprecated: the GL calls in the comments do the same ---- */

/* glNewList(id, GL_COMPILE), glEndList (glGetError: GL_OUT_OF_MEMORY when
   not all draws fitted), glCallList, glDeleteLists */
int mxgl_NewList(int id);
int mxgl_EndList(void);
void mxgl_CallList(int id);
void mxgl_DeleteAllLists(void);
/* 0: the lists name textures by their names, looked up at the call */
int mxgl_ListsDirty(void);
/* glVertexPointer, glTexCoordPointer, glDrawArrays(GL_POLYGON) */
void mxgl_DrawPolygon(const float *v, int n, int stride, int st);
/* glMultiModeDrawArraysIBM(mode, first, count, n, sizeof(GLenum)) */
void mxgl_MultiDraw(const unsigned int *mode, const int *first, const int *count, int n); /* GLenum */
/* Quake 2's command lists: glDrawRangeElements of index lists made at load */
void mxgl_DrawGlCmds(const int *cmds, const float *pos, int stride, const unsigned int *col, int nv);
/* glColorTableEXT(GL_SHARED_TEXTURE_PALETTE_EXT, GL_RGB, 256, GL_RGB,
   GL_UNSIGNED_BYTE, rgb): the shared palette; textures whose texels are
   all of it are kept as its indices (8 bits, exact) */
void mxgl_Palette(const unsigned char *rgb);
/* glTexImage2D with the internal format GL_LUMINANCE8: the texture is kept
   as its brightness (the largest of R, G, B), for the blends with the
   destination colour (lightmaps: blendFunc DST_COLOR ZERO, ZERO SRC_COLOR,
   ZERO ONE_MINUS_SRC_COLOR), which the GS draws as Cd * As */
void mxgl_TexBrightness(void);
/* glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1 << s) on texture unit 0 */
void mxgl_ColorShift(int s);

#endif
