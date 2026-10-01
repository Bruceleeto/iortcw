/*
 * pvr_gl: the OpenGL 1.1 subset the RtCW renderer uses, on the Dreamcast
 * PowerVR (KOS <dc/pvr.h>).  On PC the same code runs on tools/gpu_pvr.
 *
 * Shared by SP and MP: it only knows GL and the PVR, not the game.
 */
#ifndef PVR_GL_H
#define PVR_GL_H

/* only the GL types and constants; SDL's copy is bundled with the game */
#include "SDL_opengl.h"

#ifndef APIENTRY
#define APIENTRY
#endif

/* screen the PVR renders to */
#define PVRGL_WIDTH		640
#define PVRGL_HEIGHT	480

/* largest texture the renderer is told it may upload; bigger images are
 * scaled down by the renderer itself */
#define PVRGL_MAX_TEXTURE_SIZE	256

int  pvrgl_Init( void );
void pvrgl_Shutdown( void );
/* submit the frame to the PVR and start the next one */
void pvrgl_EndFrame( void );

/* one entry per GL function the renderer can call, as pvrgl<Name> */
#define PVRGL_PROCS \
	GLE(void, BindTexture, GLenum target, GLuint texture) \
	GLE(void, BlendFunc, GLenum sfactor, GLenum dfactor) \
	GLE(void, ClearColor, GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) \
	GLE(void, Clear, GLbitfield mask) \
	GLE(void, ClearStencil, GLint s) \
	GLE(void, ColorMask, GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) \
	GLE(void, CopyTexSubImage2D, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) \
	GLE(void, CullFace, GLenum mode) \
	GLE(void, DeleteTextures, GLsizei n, const GLuint *textures) \
	GLE(void, DepthFunc, GLenum func) \
	GLE(void, DepthMask, GLboolean flag) \
	GLE(void, Disable, GLenum cap) \
	GLE(void, DrawArrays, GLenum mode, GLint first, GLsizei count) \
	GLE(void, DrawElements, GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) \
	GLE(void, Enable, GLenum cap) \
	GLE(void, Finish, void) \
	GLE(void, Flush, void) \
	GLE(void, GenTextures, GLsizei n, GLuint *textures ) \
	GLE(void, GetBooleanv, GLenum pname, GLboolean *params) \
	GLE(GLenum, GetError, void) \
	GLE(void, GetIntegerv, GLenum pname, GLint *params) \
	GLE(const GLubyte *, GetString, GLenum name) \
	GLE(void, LineWidth, GLfloat width) \
	GLE(void, PolygonOffset, GLfloat factor, GLfloat units) \
	GLE(void, ReadPixels, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels) \
	GLE(void, Scissor, GLint x, GLint y, GLsizei width, GLsizei height) \
	GLE(void, StencilFunc, GLenum func, GLint ref, GLuint mask) \
	GLE(void, StencilMask, GLuint mask) \
	GLE(void, StencilOp, GLenum fail, GLenum zfail, GLenum zpass) \
	GLE(void, TexImage2D, GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels) \
	GLE(void, TexParameterf, GLenum target, GLenum pname, GLfloat param) \
	GLE(void, TexParameteri, GLenum target, GLenum pname, GLint param) \
	GLE(void, TexSubImage2D, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) \
	GLE(void, Translatef, GLfloat x, GLfloat y, GLfloat z) \
	GLE(void, Viewport, GLint x, GLint y, GLsizei width, GLsizei height) \
	GLE(void, Hint, GLenum target, GLenum mode) \
	GLE(GLboolean, IsEnabled, GLenum cap) \
	GLE(void, PixelStorei, GLenum pname, GLint param) \
	GLE(void, AlphaFunc, GLenum func, GLclampf ref) \
	GLE(void, Color4f, GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) \
	GLE(void, ColorPointer, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) \
	GLE(void, DisableClientState, GLenum cap) \
	GLE(void, EnableClientState, GLenum cap) \
	GLE(void, LoadIdentity, void) \
	GLE(void, LoadMatrixf, const GLfloat *m) \
	GLE(void, MatrixMode, GLenum mode) \
	GLE(void, PopMatrix, void) \
	GLE(void, PushMatrix, void) \
	GLE(void, ShadeModel, GLenum mode) \
	GLE(void, TexCoordPointer, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) \
	GLE(void, TexEnvf, GLenum target, GLenum pname, GLfloat param) \
	GLE(void, VertexPointer, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) \
	GLE(void, Color4ub, GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) \
	GLE(void, Fogf, GLenum pname, GLfloat param) \
	GLE(void, Fogfv, GLenum pname, const GLfloat *params) \
	GLE(void, MultiTexCoord4f, GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) \
	GLE(void, NormalPointer, GLenum type, GLsizei stride, const GLvoid *ptr) \
	GLE(void, ClearDepth, GLclampd depth) \
	GLE(void, DepthRange, GLclampd near_val, GLclampd far_val) \
	GLE(void, DrawBuffer, GLenum mode) \
	GLE(void, PolygonMode, GLenum face, GLenum mode) \
	GLE(void, ArrayElement, GLint i) \
	GLE(void, Begin, GLenum mode) \
	GLE(void, ClipPlane, GLenum plane, const GLdouble *equation) \
	GLE(void, Color3f, GLfloat red, GLfloat green, GLfloat blue) \
	GLE(void, Color4ubv, const GLubyte *v) \
	GLE(void, End, void) \
	GLE(void, Frustum, GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val) \
	GLE(void, Ortho, GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val) \
	GLE(void, TexCoord2f, GLfloat s, GLfloat t) \
	GLE(void, TexCoord2fv, const GLfloat *v) \
	GLE(void, Vertex2f, GLfloat x, GLfloat y) \
	GLE(void, Vertex3f, GLfloat x, GLfloat y, GLfloat z) \
	GLE(void, Vertex3fv, const GLfloat *v) \
	GLE(void, Color3fv, const GLfloat *v) \
	GLE(void, Fogi, GLenum pname, GLint param)

#define GLE( ret, name, ... ) ret APIENTRY pvrgl##name( __VA_ARGS__ );
PVRGL_PROCS
#undef GLE

#endif
