# maxsdk_gl

An OpenGL 1.1 subset on the PS2's GS, on `libs/maxsdk`'s frame pipeline: what the GL
renderers of the Quake engines use (`apps/maxsdk_quake1`, `maxsdk_quake2`, `maxsdk_quake3`).

| | |
|---|---|
| `include/GL/gl.h`, `glext.h`, `KHR/` | Mesa's and Khronos' headers (MIT licence), as they are |
| `include/mxgl.h` | the PS2's own calls: set-up, end of frame, screenshot, display lists, brightness textures, overbright |
| `src/mxgl.c` | the GL calls: state to GS registers, clipping on the EE, textures (CT32/CT16/T8, mipmaps made here) in GS memory as needed |
| `src/mxglv.hlsl` | the one VU1 vertex program: transform, divide, ST·Q, colour |
| `mk/mxgl.mk` | for an app's Makefile (see its head) |

What the GS cannot do is left out or done another way:
- Blends with the destination colour (lightmaps) are `Cd * As`: the lightmap is kept as its
  brightness (T8 with a CLUT that puts it in alpha); `mxgl_TexBrightness` before the
  upload, or `GL_LUMINANCE` texels. Colour in lightmaps is lost; a game can tint the
  surface's own pass (Quake 3 uses the vertex light's hue).
- No lines, points, stencil, clip planes, polygon offset, colour mask, `glReadPixels`.
- The window is 640 x 448 (`MXGL_WIDTH`, `MXGL_HEIGHT`); textures up to 128 x 128
  (`GL_MAX_TEXTURE_SIZE`); a 16-bit colour buffer (dithered) and a 24-bit Z buffer.
  `mxgl_window(w, h)` gives the game a window of another size, mapped onto the whole
  picture (its 2D larger; the 3D still at the picture's resolution).

## 1080i (`MXGL_HIRES=1`)

With `MXGL_HIRES=1` (make variable; the app adds `$(MXGL_DEFS)` to its CFLAGS) the
picture is 1920 x 1080 interlaced, on maxsdk's hires passes (`<mx/hires.h>`, branch
`hires` of maxsdk): one 1920 x 540 CT16S buffer, drawn in three strips just ahead of the
beam, every field, from the frame's recorded stream; Z (16 bits) in the next strip; the
lower field half a line lower, so all 1080 lines are drawn. The window is 1920 x 1080.

- The passes own FRAME, ZBUF, XYOFFSET and SCISSOR: draws without Z writes use GS
  context 2 (ZMSK), so state goes to both contexts; `glScissor` is not kept.
- A frame is played until the next replaces it (four records, three arenas more).
  Texture uploads go into the frame's once record, sent over PATH3 while the frames
  before are shown, not repeated in each pass; a frame that refills GS memory keeps
  them in its passes (in order), and the frame after uploads all it uses.
- The GS draws each frame three times a field: its fill is the limit (a pass has the
  beam time of one strip, about 5.5 ms). 8-bit textures (`mxgl_Palette`) and
  trilinear filtering's cost decide it.
- `mxgl_screenshot` saves both fields (1920 x 1080).

## Speed (both modes)

- Draws with the same state and matrix are gathered into one VU1 batch (Quake: about
  1000 GL draws a frame become 40 to 60 batches); the state and matrix are only made
  again when a GL call changed them (serials).
- The batches are triangle strips (ADC on the vertices that end no triangle): a
  polygon (`GL_POLYGON`, `mxgl_DrawPolygon`) is a strip of its n vertices, a
  `GL_TRIANGLE_STRIP` goes as it is, a fan with its centre again every second
  triangle; back-facing triangles get ADC.
- The vertices are written straight into the frame's arena through the uncached
  accelerated segment (UCAB), each stream in its own part.
- The immediate mode's arrays are one interleaved array (three power-of-two arrays put
  their first vertices in the same cache sets of the EE's 8 KiB data cache).
- `mxgl_DrawPolygon(v, n, stride, st)` draws a game's own polygon (Quake's `glpoly_t`)
  in one call; a per-frame cache keeps its verdict (culled, or inside all planes) for
  the next pass over the same vertices (the lightmap pass).
- `mxgl_Palette(rgb)`: textures whose texels are all of the game's palette are kept as
  8-bit indices with one CLUT (exact colours, half the GS memory, faster texturing);
  their mipmap levels take the nearest palette colour.
