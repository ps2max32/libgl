# libgl

OpenGL on the PS2's GS, on `libs/maxsdk`'s frame pipeline: GL 1.1, with the GL 1.2 - 1.5 and
OpenGL ES 1.1 calls the GS can do as extensions. The games call it as they would any GL (the
GL renderers of the Quake engines: `apps/quake1`, `quake2`, `quake3`); only set-up, the end of a
frame and the screen are the PS2's own (`mxgl.h`).

| | |
|---|---|
| `include/GL/gl.h`, `glext.h`, `KHR/` | Mesa's and Khronos' headers (MIT licence), as they are |
| `include/mxgl.h` | the PS2's own calls: set-up, mode, window, end of frame, screenshot, the counters; `mxglGetProcAddress`; deprecated drawing calls (each says the GL calls that do the same) |
| `src/mxgl.c`, `src/gl/*.c` | the GL calls: state to GS registers, clipping on the EE, textures (CT32/CT16/T8, mipmaps made here) in GS memory as needed. `mxgl.c` includes its parts in `src/gl/` (one translation unit: the draws' paths inline across them): `state` (the shared state), `frame`, `vram`, `texquality`, `gsstate` (GL state as GS registers), `batch` (vertex batches, the clipper), `draw` (a draw: transform, passes, triangles), `lines`, `dlist`, `drawcalls`, `immediate`, `client`, `matrix`, `statecalls`, `teximage`, `get`, `pixels`, `misc`, `paltex`, `attrib`, `mode` (set-up and modes), `vprog`, `attribs` (programs with inputs of their own), `proc` |
| `src/mxglv.hlsl` | libgl's own VU1 vertex program: transform, divide, texture matrix, ST·Q, colour (`col * cmul + cadd`, scaled) |
| `mk/mxgl.mk` | for an app's Makefile (see its head); `MXGL_PROGS`: the app's vertex programs (`mk/vu1bin.awk` makes their binaries) |
| `tests/glconf.c` | each feature drawn and read back (`glReadPixels`): `make -C tests run` (PCSX2), or the ELF on the PS2 |

`glGetString(GL_EXTENSIONS)` lists the extensions, and `mxglGetProcAddress` gives every call
by name (core and ARB/EXT names):

| | |
|---|---|
| `GL_ARB_multitexture` | two units; unit 1 is a second pass over the first's pixels (depth EQUAL, no depth writes): MODULATE as the product with the destination, REPLACE as it is. An alpha-tested first pass gets no second (its holes) |
| `GL_ARB_texture_env_combine` | COMBINE as MODULATE or REPLACE (COMBINE_RGB), RGB_SCALE 1 or 2 (the GS's modulate takes the colour up to 2x) |
| `GL_EXT_compiled_vertex_array` | between `glLockArraysEXT` and `glUnlockArraysEXT` a draw's transformed vertices are kept for the next draws of the same arrays and matrices (Quake 3's stages) |
| `GL_EXT_draw_range_elements`, `GL_EXT_multi_draw_arrays`, `GL_IBM_multimode_draw_arrays` | `glDrawRangeElements` (no index searched), `glMultiDrawArrays`, `glMultiModeDrawArraysIBM`: a model's strips and fans in one draw |
| `GL_EXT_paletted_texture`, `GL_EXT_shared_texture_palette` | `glColorTableEXT` and `GL_COLOR_INDEX` texels; the textures whose texels are all of the shared palette are kept as its indices (T8 with one CLUT: exact, half the memory of 16 bits); an entry with alpha 0 (`GL_RGBA` table) is the transparent one: textures keyed with alpha 0 are kept so too; `GL_COLOR_INDEX` texels with the palette enabled are stored as they are, sides of powers of two from 8 up (no RGBA on the way: a game's 8-bit art loads several times faster) |
| `GL_OES_compressed_paletted_texture` | `glCompressedTexImage2D` of 16 or 256 colours: T8 with the texture's own CLUT, its levels as given |
| `GL_EXT_texture_lod_bias` | `glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, levels)`: added to every draw's level (K of TEX1) |
| `GL_SGIS_generate_mipmap`, `GL_EXT_texture_edge_clamp` | the levels are made here anyway; CLAMP_TO_EDGE is the GS's clamp |
| `GL_PS2_vertex_program_binary` | GL 2.0's vertex shaders as VU1 programs from HLSL (`glShaderBinary` with `GL_PS2_VU1_BINARY`, `glLinkProgram`, `glUseProgram`, `glUniform*`); below |

### Vertex programs

An app's HLSL vertex program (`NAME.hlsl` in its `HLSL_DIRS`, listed in `MXGL_PROGS`) gives
`NAME_vu1`, a binary for `glShaderBinary(1, &sh, GL_PS2_VU1_BINARY, &NAME_vu1, sizeof
NAME_vu1)`; then `glAttachShader`, `glLinkProgram`, `glUseProgram`, `glGetUniformLocation`,
`glUniform*` as in GL 2.0. Fragments stay fixed function. `tests/glprog.hlsl` is one.

- Its inputs are libgl's own (`float4 POSITION`, `float2 TEXCOORD`, `uint4 COLOR_V4_8U`: the
  fixed arrays), else the link fails: the draws go through libgl's batches as its own do,
  culled and clipped on the EE (a cut triangle's new vertices get interpolated inputs:
  exact for a program affine in them), so the program places a vertex where `mvp` does.
- A program with other inputs is drawn from generic attributes (`glVertexAttribPointer`,
  `glEnableVertexAttribArray`; attribute i is the HLSL input i, an array in the input's own
  format in memory: `int4 POSITION_V4_16` is four shorts a vertex, `uint4 COLOR_V4_8U` four
  bytes): `glDrawArrays` copies the arrays into the frame, `glDrawElements` gathers them by 16-
  or 32-bit indices, and VU1 does the rest. libgl does not look at these vertices: nothing is
  culled or clipped (draw only what is inside the guard band and beyond the near plane). An
  input `uint4 ADC_V4_8U` is libgl's: 1 for a strip's first two vertices; an index that restarts
  the primitive (`GL_PRIMITIVE_RESTART`, `GL_PRIMITIVE_RESTART_FIXED_INDEX`) ends a strip, so a
  model's strips are one draw from its vertices as they are stored (two frames of shorts,
  blended by the program). `GL_TEXTURE_LOD_BIAS` gives the mipmap level's offset (libgl
  cannot see the texture's scale on these vertices: it takes one repeat a unit). Between
  `glLockArraysEXT` and `glUnlockArraysEXT` a surface's next passes use the same copies.
  `tests/glattr.hlsl` is one.
- libgl sets the uniforms `mvp`, `cscale`, `tms`, `tmt` (as in `mxglv.hlsl`); `glUniform*`
  the others (a location is a float's index). A program whose uniforms are these four and two
  `float4` more (`cmul`, `cadd`) has libgl's own data layout: switching costs no FLUSH; each
  program stays in VU1's micro memory at an address of its own (no upload).
- Display lists draw as recorded, with libgl's own program.
- Quake 3's `code/ps2/q3stage.hlsl`: a stage's `rgbGen`/`alphaGen` as `col * cmul + cadd`
  (and the vertex light's hue), so the EE no longer fills a colour per vertex and stage.

Fog (`glEnable(GL_FOG)`, `glFog*`, `GL_LINEAR`, `GL_EXP`, `GL_EXP2`) is the GS's: `PRIM.FGE` and
`FOGCOL`, the fog value per vertex in `XYZF2` from libgl's programs `src/mxglvf.hlsl` (linear) and
`src/mxglve.hlsl` (exponential), which have `mxglv.hlsl`'s data layout (no FLUSH when fog goes on or
off; draws without fog are as they were). The GS interpolates the value across a triangle without
perspective, and it has 8 bits. The eye distance is the vertex's w, so it needs a perspective
projection (w is 1 under `glOrtho`: one value for all). Display lists, lines and points, and a
game's own vertex programs are drawn without fog.

Display lists (`glNewList` .. `glCallList`, `glCallLists`, `glListBase`) hold their draws as DMA
chains the frame calls, with the matrices of the call. The draws called one after the other with
the same state share one set of GS registers and one VU1 context; `glCallLists` of lists that are
each the passes over one surface (the first with depth writes and no blending, the others over the
same vertices with depth EQUAL or LEQUAL and no depth writes) draws them pass by pass, so the
surfaces of one material take one state a pass (Quake 3's world: 940 draws a frame became 210); `glReadPixels` reads colour and depth (the stream drawn first:
slow), and so does `glCopyTexSubImage2D` (into the bound texture); `glGetTexImage` gives level 0; `glFrontFace`; `glClipPlane` (one plane), `glPolygonOffset` (a Z bias), `glColorMask` (FBMSK), the
texture matrix (on VU1), lines and points (on the EE) are there.

What the GS cannot do is left out or done another way:
- The texture environments `GL_BLEND` (`Cf (1 - Ct) + Cc Ct`, `GL_TEXTURE_ENV_COLOR` Cc) and
  `GL_ADD` (`Cf + Ct`) of unit 0 are two passes (Daedalus's N64 combiners): the fragment
  colour with the texture's alpha (`Af At`: a white copy with its alpha; blending and the alpha
  test as set), then over its pixels (depth EQUAL, no depth writes) for BLEND Cc blended by the
  texture's brightness times its alpha (`(Cc - Cd) Bt + Cd`: exact for grey textures), for ADD
  the texture added. The copies are made at the first such draw.
- An RGBA texture whose texels are all grey and opaque (or black with alpha 0: a lightmap
  block's unused texels) is kept as its brightness (T8, 8 bits, exact; Quakespasm's lightmaps),
  with a CLUT of alpha 1 when drawn other than in a product (those black texels are then
  opaque). When all its black texels are transparent (a font: white or grey on clear), its
  CLUT keeps them transparent (entry 0, alpha 0).
- Blends with the destination colour (lightmaps) are `Cd * As`: a luminance texture
  (internal format `GL_LUMINANCE8`, `GL_LUMINANCE`, `GL_INTENSITY8`) is kept as its brightness
  (the largest of R, G and B; T8 with a CLUT that puts it in alpha). A colour texture in such
  a blend (`DST_COLOR` with `ZERO`, `ONE` or `SRC_COLOR`, `ZERO` with `SRC_COLOR` or
  `ONE_MINUS_SRC_COLOR`) is drawn from a brightness copy made at its first such draw.
  `DST_COLOR ONE` (a dynamic light) is `Cd * (1 + As)`, the vertex alpha the light's strength.
  Colour in these is lost; a game can tint the surface's own pass (Quake 3 uses the vertex
  light's hue).
- No destination alpha (`GL_ALPHA_BITS` 0): `DST_ALPHA` is 1. The depth tests
  are LESS, LEQUAL (EQUAL: the same vertices), ALWAYS, NEVER (GS Z is larger nearer).
- With beam racing `glScissor` and `glReadPixels` are not kept (the passes own SCISSOR and
  draw the buffer in strips).
- The stencil is one bit (`GL_STENCIL_BITS` 1, 0 in 24-bit colour): the frame buffer's alpha
  bit, tested with `TEST.DATE`, and two values, the clear value and the first other one
  written, for masks and shadows drawn once. No counting (shadow volumes). `glStencilOp`'s
  fail and depth-fail operations are not done. A write of the first value with colour on is
  left out. A colour clear keeps the bit, but the frame's first clear (mx_frame_begin) sets
  it: `glClear` the stencil before using it.
- Textures up to 128 x 128 (`GL_MAX_TEXTURE_SIZE`), sides of powers of two (others are
  scaled). A MIN_FILTER without MIPMAP draws level 0 only (GLQuake's lightmaps: a level below
  mixes the surfaces packed next to each other).
- `mxgl_window(w, h)` gives the game a window of another size, mapped onto the whole
  picture (its 2D larger; the 3D still at the picture's resolution).

## Memory

Three heaps of maxsdk's (`<mx/mem.h>`); with its virtual memory on, they take frames from
the one pool as needed and give them back when freed (there is no core heap to size):

- `gl` (pinned): what the DMAC or VU0 reads: the display lists' pool (256 KiB pieces, up to
  2 MiB), buffer objects, the transform caches, the textures' smaller levels and CLUTs;
- `gl.stream` (pinned): the two arenas, and beam racing's records and two more arenas
  while it is on;
- `gl.textures` (pageable, 32 MiB of the heap window): the textures' two largest levels
  (`struct mx_tex_mem`, `<mx/vcache.h>`). A texture missing from GS memory is uploaded whole
  when they are resident (locked until the frame is done), else from its smaller levels at
  once while the pager brings them in: a later frame uploads it whole. Brightness textures
  (lightmaps) and textures of fewer than 3 levels stay pinned whole.

libgl's static tables are about 740 KB, 500 KB of them the texture names' (2048):
`MXGL_MAX_TEXTURES` in the app's Makefile sets another number (64: 240 KB in all).
`mxgl_suspend` gives back what the frames need (the arenas, beam racing's records, the display
lists and their pool) while a program draws nothing, as a shell beside a game; `mxgl_resume`
takes it again and sets the mode that was set. The textures stay unless deleted first.

`mxgl_deflicker(1)` is the flicker filter of NTSC and PAL without beam racing: the PCRTC's two read
circuits show the buffer a line apart, blended 1:1, so each line shown is the mean of two drawn
(softer down, no drawing cost). Off by default: every line as drawn.

`mxgl_texture_quality` leaves the top levels out (1 or 2) of textures drawn with MIPMAP
filtering, or picks them by itself (`MXGL_TEX_AUTO`), with two controls apart:

- GS memory: W, the bytes of the textures a frame uses, against the room for them. Up to
  120% the frame streams what does not fit. Above it for 2 s the textures are placed from
  coarser levels until about 80%: first the finest level their draws sample (each draw's
  nearest vertex and its LOD: no visible change), then a level less each, the textures
  seen from farthest first, the largest first; back to full when W at full quality is at
  most halfway (100%) for 3 s.
  In Quake's rooms nearly every texture is also on a near surface (sampled at level 0), so
  the first step seldom has much to take.
- RAM: swap-ins at 30 pages a second or more for 3 s: textures whose draws do not sample
  their top level are drawn from their pinned tails, their pageable upper levels marked to
  go first (`mx_mem_cold`); back after 5 quiet seconds.

`mxgl_texture_limits(120, 80, 30)` sets the limits; the log's `stats: texture quality` line
shows W, the levels placed and the textures by the level their draws sample. Without virtual memory every heap is physical memory. The game's
vertex arrays are read with interrupts off by VU0 macro code: each run's pages are touched
first, so a page in the swap file is brought in with interrupts on.

## Modes

`mxgl_init` starts in the console's TV mode: NTSC (640 x 448, 60 Hz) or PAL (640 x 512,
50 Hz) by the BIOS's region (`mx_video_default`), interlaced, a 16-bit colour buffer
(dithered) each of two frames and a 24-bit Z buffer. `mxgl_set_mode` changes it between
frames (`MXGL_SD`, `MXGL_NTSC`, `MXGL_PAL`, `MXGL_1080I`, `MXGL_1080I50`, `MXGL_1080P30`,
`MXGL_720P`, `MXGL_720P50`, `MXGL_576P`, `MXGL_480P`, with the flags `MXGL_BEAM` and
`MXGL_RGB24` or-ed in): the pipeline starts again, GS memory is filled again, the display
lists are dirty, the window is the screen (`mxgl_screen`). `mxgl_mode_flags` says which
flags are in effect. 720p50, 1080i50 and 1080p30 are not in the BIOS: the kernel times them
(mx-kernel 22 or later). A mode that does not run (the console or the kernel lacks it: no
vblank) leaves the console's own NTSC or PAL set, and `mxgl_set_mode` returns -1
(`mxgl_mode` says which).

| Mode | Buffers | Screen | GS memory for textures |
|---|---|---|---|
| NTSC / PAL | 2 x CT16S 640 x 448 / 512, Z24 | 640 x 448 / 512 | 1.8 / 1.5 MiB, + 280 / 320 KiB of 8-bit textures in Z's top byte |
| NTSC / PAL + `MXGL_BEAM` | CT16S 640 x 224 / 256 a field, 4 strips of 64 lines, Z16 in them | 640 x 448 / 512 | 3.6 MiB |
| NTSC / PAL + `MXGL_BEAM` + `MXGL_RGB24` | CT24 640 x 224 / 256, 3 strips of 96 lines, Z24 in them | 640 x 448 / 512 | 3.3 MiB, + 180 KiB of 8-bit textures in the strips' top byte |
| 1080i | 2 x CT16S 640 x 540, Z16, stretched (FRAME mode: both fields the same picture) | 1920 x 1080 | 1.9 MiB |
| 1080i + `MXGL_BEAM` | CT16S 1920 x 540 a field, 3 strips of 192 lines, Z16 in them | 1920 x 1080 | 1.9 MiB |
| 1080p30 + `MXGL_BEAM` | CT16S 1920 x 540, each line shown twice (1920 x 1080 does not fit) | 1920 x 1080 | 1.9 MiB |
| 720p + `MXGL_BEAM` | CT16S 1280 x 720, strips, Z16 in them | 1280 x 720 | about 2.1 MiB |
| 480p / 576p + `MXGL_BEAM` + `MXGL_RGB24` | CT24 640 x 480 / 576, strips, Z24 in them | 640 x 480 / 576 | about 2.8 / 2.5 MiB |

Without `MXGL_BEAM` the progressive HD modes draw two smaller 16-bit buffers, stretched
(720p: 640 x 360; 1080p30: 640 x 540; 480p / 576p: 640 x 480 / 512).

16-bit colour is dithered with the GS manual's 4 x 4 matrix, or with `mxgl_dither`'s: up to
eight DIMX values used in turn, one a field with beam racing (each field is drawn anew:
maxsdk's `mx_hires_dither`), else one a frame. Bayer's order moved about a field at a time
gives every pixel each offset in turn and looks like fine grain.

### Beam racing

`MXGL_BEAM` draws on maxsdk's hires passes (`<mx/hires.h>`): one buffer of a field's
lines, drawn in strips just ahead of the beam, every field, from the frame's recorded
stream; Z in the next strip; the lower field half a line lower, so all the screen's lines
are drawn. It takes 2 MiB of pinned memory while it is on (the records and two more arenas;
`MXGL_REC_BYTES`: the records' size); `mxgl_set_mode` returns -1 if there is not that much,
and sets the mode without it.
`MXGL_RGB24` (32-bit colour, no dithering) only fits in SD.

- The passes own FRAME, ZBUF, XYOFFSET and SCISSOR: draws without Z writes use GS
  context 2 (ZMSK), so state goes to both contexts; `glScissor` and `glColorMask` are not
  kept, `glReadPixels` gives zeros.
- A frame is played until the next replaces it (four records, two arenas more): at most
  a frame a field is shown.
- Texture uploads go into the frame's once record, sent over PATH3 while the frames
  before are shown, not repeated in each pass; a frame that refills GS memory keeps
  them in its passes (in order), and the frame after uploads all it uses.
- The GS draws each frame once per strip, every field (VU1 runs it again too): its
  fill and the frame's batches are the limit (a pass has the beam time of one strip:
  1080i about 5.5 ms, SD about 4 ms).
- `mxgl_screenshot` saves both fields (1920 x 1080, 640 x 448 / 512).

## Speed (both modes)

- Draws with the same state and matrix are gathered into one VU1 batch (Quake: about
  1000 GL draws a frame become 40 to 60 batches); the state and matrix are only made
  again when a GL call changed them (serials).
- The batches are triangle strips (ADC on the vertices that end no triangle): a
  polygon (`GL_POLYGON`) is a strip of its n vertices, a `GL_TRIANGLE_STRIP` goes as it
  is, a fan with its centre again every second triangle; a triangle list goes on the
  strip so far when a triangle shares its last two vertices (one vertex) or its last one
  (two); back-facing triangles get ADC.
- The vertices are written straight into the frame's arena through the uncached
  accelerated segment (UCAB), each stream in its own part.
- The immediate mode's arrays are one interleaved array (three power-of-two arrays put
  their first vertices in the same cache sets of the EE's 8 KiB data cache).
- `glDrawArrays(GL_POLYGON)` of textured polygons without colours (Quake's world) copies
  them into a queue drawn with one state and matrix check (several hundred a batch).
- Locked arrays (`glLockArraysEXT`) are transformed once for all the draws of them.
- GS memory is maxsdk's cache (`<mx/vcache.h>`): what goes is what the frame needs last
  (textures done for the frame first, by their uses against the frame before). 8-bit
  textures also go into the top byte of 24-bit buffers (Z24, CT24 strips: nothing draws
  it) as T8H, when that room is cheaper; `TEX` in the counters includes it. `glTexSubImage2D` sends only the
  lines changed (Quake's dynamic lightmaps).
- A register write waits for the batches before (FLUSH) only if something was drawn
  since the last one.
- The shared palette's textures are kept as 8-bit indices with one CLUT (exact colours,
  half the GS memory, faster texturing); their mipmap levels take the nearest palette
  colour.
