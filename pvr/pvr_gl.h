/*
 * pvr_gl: the OpenGL 1.1 subset the RtCW renderer uses, on the Dreamcast
 * PowerVR (KOS <dc/pvr.h>).  On PC the same code runs on tools/gpu_pvr.
 *
 * It only knows GL and the PVR, not the game.
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
/* Upload a pvrtex .dt file (twiddled or VQ, mipmapped or not) as the bound
 * texture, as it is. Gives the image's size; 0 if it can't. */
int  pvrgl_TexImageDT( const void *file, int len, int *width, int *height );
/* back faces culled by the PVR: 1, or on the CPU: 0 */
extern int pvrgl_hwCull;

/* A vertex as pvrglDrawPackedStrips takes it: the position and texture
 * coordinates as shorts from an origin, in steps (16 bytes) */
typedef struct {
	short		xyz[3];
	unsigned char	unused[2];
	short		st[2];
	unsigned char	rgba[4];
} pvrglPackedVert_t;
/* on the first index of each strip */
#define PVRGL_STRIP_START	0x8000
/* Draws triangle strips of packed vertexes (pvrglPackedVert_t) with the
 * texture, state and matrices as set: xyz = origin + step * v, st =
 * stOrigin + stStep * v, the colour's alpha 1. strips: the vertex indexes,
 * PVRGL_STRIP_START on each strip's first (as glDrawElements of
 * GL_TRIANGLE_STRIPs with a restart). */
void pvrglPackedBegin( void );
void pvrglPackedEnd( void );
void pvrglDrawPackedStrips( const void *verts, int numVerts, const float origin[3], float step,
							const float stOrigin[2], float stStep, const unsigned short *strips, int numIndexes,
							const float *mins, const float *maxs );

void APIENTRY pvrglLockArraysEXT( GLint first, GLsizei count );
void APIENTRY pvrglUnlockArraysEXT( void );

void pvrglFogArray( const unsigned char *amounts );
/* A fog for a packed batch's fast draws (pvrglPackedFog before
 * pvrglPackedBegin, till pvrglPackedEnd): the PVR blends pvrgl_FogColor's
 * colour in by vertex, the amount table[depth * invDepth] (256 amounts
 * 0..1 by the view depth, the last beyond) for a vertex in the fog:
 * dot(xyz, plane) >= plane[3] if hasPlane, as the game's R_FogFactor.
 * eyeT: the eye's dot - plane[3]; under 0 it's outside and the depth is
 * cut at the plane. A draw not on the fast path (pvrglPackedFast) has
 * no fog. */
#ifndef PVRGL_PACKED_FOG_T
#define PVRGL_PACKED_FOG_T
typedef struct {
	float		plane[4];
	int			hasPlane;
	float		eyeT;
	float		invDepth;
	const float	*table;
} pvrglPackedFog_t;
void pvrglPackedFog( const pvrglPackedFog_t *fog );
int  pvrglPackedFast( const float *mins, const float *maxs );
#endif
int  pvrgl_FogColor( unsigned int rgba );

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
