/* gl/client.c, a part of mxgl.c (included there, not compiled alone): glEnable, the client
   state, buffer objects, the array pointers */

static void set_cap(GLenum cap, int on) {
  /* the indexed draws' restart: no GS state */
  if (cap == GL_PRIMITIVE_RESTART || cap == GL_PRIMITIVE_RESTART_FIXED_INDEX) {
    *(cap == GL_PRIMITIVE_RESTART ? &prim_restart : &prim_restart_fixed) = on;
    return;
  }
  pq_flush();
  st_serial++;
  switch (cap) {
  case GL_TEXTURE_2D:
    if (gl.cunit)
      gl.u1.tex2d = on;
    else
      gl.tex2d = on;
    break;
  case GL_SHARED_TEXTURE_PALETTE_EXT: gl.shared_pal = on; break;
  case GL_BLEND: gl.blend = on; break;
  case GL_ALPHA_TEST: gl.atest = on; break;
  case GL_DEPTH_TEST: gl.dtest = on; break;
  case GL_CULL_FACE: gl.cull = on; break;
  case GL_SCISSOR_TEST: gl.scissor = on; break;
  case GL_POLYGON_OFFSET_FILL:
    if (gl.poffset != on)
      mat_serial++;
    gl.poffset = on;
    break;
  case GL_CLIP_PLANE0:
    if (gl.ucp_on != on)
      mat_serial++;
    gl.ucp_on = on;
    break;
  /* state the GS has not, or that changes no picture here */
  case GL_FOG: gl.fog = on; break;
  case GL_STENCIL_TEST: gl.st_on = on; break;
  case GL_DITHER: case GL_LIGHTING: case GL_NORMALIZE: case GL_LINE_SMOOTH:
  case GL_POINT_SMOOTH: case GL_POLYGON_SMOOTH: case GL_COLOR_MATERIAL:
  case GL_TEXTURE_GEN_S: case GL_TEXTURE_GEN_T: case GL_POLYGON_OFFSET_LINE:
  case GL_POLYGON_OFFSET_POINT: case GL_TEXTURE_1D: case GL_LIGHT0:
    break;
  default: gl_error(GL_INVALID_ENUM); break;
  }
}
void APIENTRY glEnable(GLenum cap) { set_cap(cap, 1); }
void APIENTRY glDisable(GLenum cap) { set_cap(cap, 0); }

static void set_client(GLenum cap, int on) {
  if (cap == GL_VERTEX_ARRAY)
    gl.va = on;
  else if (cap == GL_TEXTURE_COORD_ARRAY && gl.ccunit)
    gl.u1.ta = on;
  else if (cap == GL_TEXTURE_COORD_ARRAY)
    gl.ta = on;
  else if (cap == GL_COLOR_ARRAY)
    gl.ca = on;
}
void APIENTRY glEnableClientState(GLenum cap) { set_client(cap, 1); }
void APIENTRY glDisableClientState(GLenum cap) { set_client(cap, 0); }

/* ---- buffer objects (GL 1.5, OpenGL ES 1.1) ------------------------------- */

/* A buffer's data is core memory (VU0 reads the vertices with interrupts
   off). It changes only by glBufferData and glBufferSubData, which draw
   the polygons queued first: draws of buffer data are queued as pointers
   into it, not copied (Quake's world). */
#define MAX_BUF 256
static struct {
  u8 used;
  u8 *data;
  u32 size;
} bufs[MAX_BUF];

/* where an array's pointer points: its buffer's data + offset, or the pointer */
static const void *buf_ptr(GLuint b, uintptr_t off) {
  return b && b < MAX_BUF && bufs[b].data ? (const void *)(bufs[b].data + off) : (const void *)off;
}
/* the arrays' pointers again (a buffer's data moved) */
static void buf_arrays(void) {
  if (gl.vbuf)
    gl.vptr = buf_ptr(gl.vbuf, gl.voff);
  if (gl.tbuf)
    gl.tptr = buf_ptr(gl.tbuf, gl.toff);
  if (gl.cbuf)
    gl.cptr = buf_ptr(gl.cbuf, gl.coff);
  if (gl.u1buf)
    gl.u1.tptr = buf_ptr(gl.u1buf, gl.u1off);
}

void APIENTRY glGenBuffers(GLsizei n, GLuint *b) {
  for (int i = 0; i < n; i++) {
    b[i] = 0;
    for (GLuint k = 1; k < MAX_BUF; k++)
      if (!bufs[k].used) {
        bufs[k].used = 1, bufs[k].data = NULL, bufs[k].size = 0;
        b[i] = k;
        break;
      }
    if (!b[i])
      gl_error(GL_OUT_OF_MEMORY);
  }
}
void APIENTRY glBindBuffer(GLenum target, GLuint b) {
  if (b >= MAX_BUF) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  if (b && !bufs[b].used)
    bufs[b].used = 1; /* a name not from glGenBuffers is made by binding it */
  if (target == GL_ARRAY_BUFFER)
    gl.abuf = b;
  else if (target == GL_ELEMENT_ARRAY_BUFFER)
    gl.ebuf = b;
  else
    gl_error(GL_INVALID_ENUM);
}
static GLuint *buf_target(GLenum target) {
  return target == GL_ARRAY_BUFFER ? &gl.abuf : target == GL_ELEMENT_ARRAY_BUFFER ? &gl.ebuf : NULL;
}
void APIENTRY glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage) {
  GLuint *bp = buf_target(target);
  if (!bp || !*bp || size < 0) {
    gl_error(!bp ? GL_INVALID_ENUM : *bp ? GL_INVALID_VALUE : GL_INVALID_OPERATION);
    return;
  }
  pq_flush(); /* queued polygons may point into it */
  GLuint b = *bp;
  u8 *d = size ? gl_alloc(((u32)size + 63) & ~63u, 64) : NULL;
  if (size && !d) {
    gl_error(GL_OUT_OF_MEMORY);
    return;
  }
  if (d && data)
    memcpy(d, data, (size_t)size);
  defer_free(bufs[b].data);
  bufs[b].data = d, bufs[b].size = (u32)size;
  buf_arrays();
}
void APIENTRY glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const GLvoid *data) {
  GLuint *bp = buf_target(target);
  if (!bp || !*bp || off < 0 || size < 0 || (u32)(off + size) > bufs[*bp].size) {
    gl_error(!bp ? GL_INVALID_ENUM : GL_INVALID_VALUE);
    return;
  }
  pq_flush();
  memcpy(bufs[*bp].data + off, data, (size_t)size);
}
void APIENTRY glDeleteBuffers(GLsizei n, const GLuint *b) {
  pq_flush();
  for (int i = 0; i < n; i++) {
    GLuint k = b[i];
    if (!k || k >= MAX_BUF)
      continue;
    defer_free(bufs[k].data);
    bufs[k].used = 0, bufs[k].data = NULL, bufs[k].size = 0;
    if (gl.abuf == k)
      gl.abuf = 0;
    if (gl.ebuf == k)
      gl.ebuf = 0;
  }
}
GLboolean APIENTRY glIsBuffer(GLuint b) { return b && b < MAX_BUF && bufs[b].used; }
void APIENTRY glGenBuffersARB(GLsizei n, GLuint *b) { glGenBuffers(n, b); }
void APIENTRY glBindBufferARB(GLenum t, GLuint b) { glBindBuffer(t, b); }
void APIENTRY glBufferDataARB(GLenum t, GLsizeiptr s, const GLvoid *d, GLenum u) {
  glBufferData(t, s, d, u);
}
void APIENTRY glBufferSubDataARB(GLenum t, GLintptr o, GLsizeiptr s, const GLvoid *d) {
  glBufferSubData(t, o, s, d);
}
void APIENTRY glDeleteBuffersARB(GLsizei n, const GLuint *b) { glDeleteBuffers(n, b); }
GLboolean APIENTRY glIsBufferARB(GLuint b) { return glIsBuffer(b); }

/* the array pointers: offsets into the bound GL_ARRAY_BUFFER, if one is */
void APIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
  gl.vbuf = gl.abuf, gl.voff = (uintptr_t)ptr;
  gl.vptr = buf_ptr(gl.vbuf, gl.voff), gl.vsize = size, gl.vstride = stride ? stride : size * 4;
}
void APIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
  if (gl.ccunit) {
    gl.u1buf = gl.abuf, gl.u1off = (uintptr_t)ptr;
    gl.u1.tptr = buf_ptr(gl.u1buf, gl.u1off), gl.u1.tstride = stride ? stride : size * 4;
  } else {
    gl.tbuf = gl.abuf, gl.toff = (uintptr_t)ptr;
    gl.tptr = buf_ptr(gl.tbuf, gl.toff), gl.tstride = stride ? stride : size * 4;
  }
}
void APIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
  gl.cbuf = gl.abuf, gl.coff = (uintptr_t)ptr;
  gl.cptr = buf_ptr(gl.cbuf, gl.coff), gl.cstride = stride ? stride : size;
}
