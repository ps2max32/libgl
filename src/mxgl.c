/* mxgl.c: an OpenGL 1.1 subset on the GS through maxsdk's frame pipeline
   (<mx/frame.h>), for the GL renderers of games (the Quake engines).

   - A draw (glDrawElements, glDrawArrays, glBegin/glEnd) is clipped on the
     EE: each vertex is taken to GS coordinates (times w) by the matrix mvp
     below, and the triangles that cross the near or far plane or leave the
     GS guard band are cut (Sutherland-Hodgman, all attributes linear). The
     triangles are written as three vertex streams (position, texture
     coordinates, colour) into the frame's arena, and VU1 runs the HLSL
     program mxglv.hlsl on them (mx_frame_shader_draw): transform, divide,
     ST * Q, colour scale.
   - GL state becomes GS registers (TEX0/1, CLAMP, ALPHA, TEST, ZBUF,
     SCISSOR), written by DIRECT after a FLUSH when they change.
   - Textures are kept in main memory in the GS's format (CT32, or CT16 when
     they have no alpha, or T8 for brightness textures: mxgl_TexBrightness,
     GL_LUMINANCE) with mipmap levels made here, and copied into GS memory
     in the stream (IMAGE by ref tags) when a draw needs one that is not
     there; when GS memory is full, all are dropped and it is filled again
     from the start.
   - The GS cannot multiply by the destination colour, only by the source
     alpha: the blends that do (lightmaps) are drawn as Cd * As with a
     brightness texture, whose CLUT puts its texels in alpha.
   - Display lists (mxgl_NewList ...) keep a draw's streams as a DMA chain
     the frame stream calls.
   - The screen (mxgl_set_mode) is SD, the console's TV mode, or 1080i,
     with or without beam racing:
     - SD: 640 x 448 (NTSC) or 640 x 512 (PAL), interlaced FIELD mode, two
       16-bit colour buffers (CT16S, dithered) and one 24-bit Z buffer,
       which leaves about 1.8 MiB (PAL 1.5) of GS memory for textures.
     - 1080i: 640 x 540 shown by both fields (FRAME mode), stretched to
       1920 x 1080, two CT16S buffers and a 16-bit Z: 1.9 MiB left.
     - Beam racing (maxsdk's passes, <mx/hires.h>): one buffer of a field's
       lines (SD 640 x 224 / 256, 1080i 1920 x 540; CT16S, or CT32 in SD)
       drawn in strips just ahead of the beam, every field, from the
       frame's stream recorded; Z (16 bits, or 32 with CT32) in the next
       strip; each field's lines half a line apart, so all the screen's
       lines are drawn. The passes own FRAME, ZBUF, XYOFFSET and SCISSOR: a
       draw without Z writes uses GS context 2 (ZMSK set), so the state
       registers are written in both contexts, and the GL scissor is not
       kept. GS memory left for textures: SD 3.6 MiB (CT32: 3.3), 1080i
       1.9 MiB.
     The mode can change between frames: the pipeline is started again,
     GS memory filled again.

   - Lines and points are GS primitives made on the EE; glReadPixels reads
     the GS's buffers back (mx_frame_sync first); one user clip plane is a
     seventh plane of the clipping; polygon offset is a Z bias in the
     matrix; the colour mask is FRAME's FBMSK; the texture matrix is on VU1.
   Not done: a counting stencil, destination alpha, the other blends with the
   destination colour (the draw is skipped). */

#include <stdlib.h>
#include <string.h>

#include <malloc.h>
#include <math.h>
#include <mx/mx.h>
#include <mx/hud.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <mx/tex.h>
#include <mx/vcache.h>
#include <mx/mem.h>
#include <mx/vm.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include "mxglv.h"
/* MXGL_VU1CLIP: clipping and culling on VU1 (src/mxglc.vsm): the EE sends a draw's
   vertices without looking at them. A game's own programs without mxglc.i's clipping
   and user clip planes keep the EE's clipper (vu1_clips). */
/* the buffers in the TV's mode without beam racing: 512 wide is the video clock's 2560 / 5,
   as exact as 640; smaller buffers leave more GS memory for textures, 16-bit Z most
   (but no top bytes for 8-bit textures) */
#ifndef MXGL_FB_W
#define MXGL_FB_W 640
#endif
#ifndef MXGL_FB_Z
#define MXGL_FB_Z MX_GS_PSM_Z24
#endif
#ifndef MXGL_VU1CLIP
#define MXGL_VU1CLIP 0
#endif
#if MXGL_VU1CLIP
#include "mxglc.h"
#include "mxglcf.h"
#include "mxglce.h"
extern const u8 mxglc_vu[], mxglc_vu_end[], mxglcf_vu[], mxglcf_vu_end[], mxglce_vu[], mxglce_vu_end[];
#define MXGL_UNI_QW VU_MXGLC_UNI_QW
#define MXGL_ADC_W 0x3F808000u /* mxglc.vsm reads the low half, XYZ2's ADC; about 1.0 */
#define MXGL_REV_W 0x00004000u /* closes a triangle in the reverse of its order; about 0 */
#else
#define MXGL_UNI_QW VU_MXGLV_UNI_QW
#define MXGL_ADC_W 0x3F800000u
#define MXGL_REV_W 0u
#endif
/* a vertex's pos.w: 0 it closes a triangle, 1 it closes none (ADC), 2 it closes one
   and the last three vertices have the triangle's reverse order (for mxglc.vsm's
   culling) */
#define MXGL_W(a) ((a) == 1 ? MXGL_ADC_W : (a) == 2 ? MXGL_REV_W : 0)
#include "mxglvf.h"
#include "mxglve.h"

#include "mxgl.h"

extern const u8 mxglv_vu[], mxglv_vu_end[];
extern const u8 mxglvf_vu[], mxglvf_vu_end[], mxglve_vu[], mxglve_vu_end[];
/* the fog programs have mxglv's data layout: o_uni is theirs too */
_Static_assert(VU_MXGLVF_UNI_QW == VU_MXGLV_UNI_QW && VU_MXGLVE_UNI_QW == VU_MXGLV_UNI_QW &&
               VU_MXGLVF_TMS == VU_MXGLV_TMS && VU_MXGLVE_TMS == VU_MXGLV_TMS &&
               VU_MXGLVF_TMT == VU_MXGLV_TMT && VU_MXGLVE_TMT == VU_MXGLV_TMT, "fog layout");

#include <mx/hires.h>
/* the picture (mxgl_set_mode): the buffer drawn into, fb_w x fb_h (beam
   racing: a field's), and the screen it shows, scr_w x scr_h; beam: by
   maxsdk's passes; rgb24: 24-bit colour (CT24 strips with Z24) */
static int beam, rgb24;
static int fb_w = 640, fb_h = 448, scr_w = 640, scr_h = 448;
#define ZMAX_Z24 1048575.0f /* GS Z (24 bits) / 16: ftoi4 (Z32: the same range) */
#define ZMAX_Z16 4095.0f    /* 16 bits */
static float zmax = ZMAX_Z24;
#define NARENA MX_HIRES_SLOTS /* beam racing: an arena per record; else two */
#define HUD_SCALE 1
/* the window the game sees (mxgl_window), mapped onto the whole buffer */
static int gl_w = 640, gl_h = 448;
#define GB_LO (2048.0f - 1900.0f) /* the guard band, GS pixels */
#define GB_HI (2048.0f + 1900.0f)

/* The parts, in this order (each uses what the ones before define; all one
   translation unit: the draws' paths inline across them): */
#include "gl/state.c"
#include "gl/frame.c"
#include "gl/vram.c"
#include "gl/texquality.c"
#include "gl/gsstate.c"
#include "gl/batch.c"
#include "gl/draw.c"
#include "gl/lines.c"
#include "gl/dlist.c"
#include "gl/drawcalls.c"
#include "gl/immediate.c"
#include "gl/client.c"
#include "gl/matrix.c"
#include "gl/statecalls.c"
#include "gl/teximage.c"
#include "gl/get.c"
#include "gl/pixels.c"
#include "gl/misc.c"
#include "gl/paltex.c"
#include "gl/attrib.c"
#include "gl/mode.c"
#include "gl/vprog.c"
#include "gl/proc.c"
