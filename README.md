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
