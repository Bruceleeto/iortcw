/*
 * pvr_gl: the OpenGL 1.1 subset the RtCW renderer uses, on the PowerVR.
 *
 * Geometry is transformed and near-clipped on the CPU, projected to screen
 * space with z = 1/w (what the PVR wants for depth and perspective-correct
 * texturing) and written as PVR vertices.  The PVR draws its lists in a
 * fixed order (opaque, punch-through, translucent) and each list has to be
 * sent in one go, while GL interleaves them freely; so primitives are
 * collected per list in RAM during the frame and sent at pvrgl_EndFrame().
 *
 * Translucent polys are drawn in submission order (autosort disabled),
 * which is what Quake 3 style multi-pass shaders rely on.  All translucent
 * passes land after all opaque ones, but they depth test against the final
 * opaque depth, so lightmap passes (depth EQUAL) still only hit visible
 * surfaces.
 *
 * Depth: GL depth is replaced by 1/w, so the comparisons are flipped
 * (LEQUAL -> GEQUAL).  glDepthRange tricks become scale factors on 1/w,
 * which keeps texturing perspective correct:
 *   (0,1)      normal
 *   (0,<1)     weapon/view model hack: always in front of the world
 *   (0,0)      never occluded
 *   (1,1)      sky: behind everything
 * A depth clear in mid-frame (portals, 3D UI models) cannot clear the PVR
 * depth buffer, so everything drawn after it gets a larger scale instead.
 *
 * Not done (yet): mipmaps, fog, stencil, lines/points, scissor, user clip
 * planes, render to texture, multitexture.
 */

#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dc/pvr.h>
#ifdef USE_SH4ZAM
#include <sh4zam/shz_sh4zam.h>
#endif

#include "pvr_gl.h"
#include "../SP/code/qcommon/dc_prof.h"

#ifndef PVR_PT_ALPHA_REF
#define PVR_PT_ALPHA_REF	0x011c
#endif

#define MAX_MATRIX_DEPTH	8	/* the renderer pushes 2 deep at most (tr_flares.c) */
#define MAX_TEXTURES		( 2048 + 1 )	/* the renderer's MAX_DRAWIMAGES, from name 1, reused */
/* the lists are kept in blocks, from one pool for all of them, made as
   frames need them, up to the vertex buffer pvrgl_Init gives the PVR: no
   frame can send it more than that, whichever lists it is in */
#define VERTEX_BUFFER		( 768 * 1024 )
#define LIST_BLOCK			( 16 * 1024 )
#define LIST_BLOCKS			( VERTEX_BUFFER / LIST_BLOCK )

/* scale factors on 1/w, see the comment at the top */
#define DEPTH_SCALE_SKY		0.01f
#define DEPTH_SCALE_HACK	64.0f
#define DEPTH_SCALE_TOP		4096.0f
#define DEPTH_SCALE_CLEAR	256.0f
#define DEPTH_MAX_CLEARS	8
#define DEPTH_OFFSET_SCALE	1.0005f

/* --------------------------------------------------------------------- */

typedef struct {
	pvr_ptr_t	data;
	int			width, height;	/* PVR size (power of two, >= 8) */
	int			srcWidth, srcHeight;	/* size the renderer uploaded */
	int			format;			/* PVR_TXRFMT_* pixel format */
	uint32_t	txrFormat;		/* what the poly header gets: + twiddled / VQ / stride */
	pvr_ptr_t	base;			/* what the PVR is pointed at (a VQ codebook may be short) */
	int			mipmap;
	int			alpha;			/* has an alpha channel */
	int			bytes;
	int			clampS, clampT;
	int			linear;
	int			palBank;		/* 8 bit palette bank + 1, 0 for none */
} pvrTexture_t;

typedef struct {
	GLint		size;
	GLenum		type;
	GLsizei		stride;
	const GLvoid	*ptr;
	int			enabled;
} glArray_t;

/* a vertex on its way to the PVR: a cache line */
typedef struct {
	union {
		struct { float sx, sy, sz; };	/* PVR screen space: in front of the near plane */
		struct { float x, y, w; };		/* clip space (the viewport in): behind it */
	};
	float		d;				/* clip z + w: < 0 behind the near plane */
	float		u, v;
	uint32_t	argb;
	int			code;			/* 1 behind the near plane, 2 4 8 16 off the viewport's edges */
} __attribute__( ( aligned( 32 ) ) ) clipVert_t;

typedef struct {
	uint8_t		*blocks[LIST_BLOCKS];
	int			numBlocks;
	int			used;			/* bytes, in all its blocks */
	int			whole;			/* used, to the end of its last whole primitive */
	int			overflowed;
	pvr_poly_hdr_t	last;		/* last header written to this list */
	int			hasLast;
} listBuffer_t;

typedef struct {
	float		xyz[3];
	float		st[2];
	uint8_t		rgba[4];
} immVert_t;

static struct {
	int			inited;

	/* matrices */
	GLenum		matrixMode;
	float		modelview[MAX_MATRIX_DEPTH][16];
	float		projection[MAX_MATRIX_DEPTH][16];
	int			mvDepth, projDepth;
	float		mvp[16];
	int			mvpDirty;

	/* viewport, in GL window coordinates (y up) */
	int			vpX, vpY, vpW, vpH;
	float		depthNear, depthFar;

	/* capabilities */
	int			texture2D, blend, alphaTest, depthTest, cullFace;
	int			fog, scissor, stencil, polygonOffset, clipPlane0;

	GLenum		blendSrc, blendDst;
	GLenum		alphaFunc;
	float		alphaRef;
	GLenum		depthFunc;
	int			depthMask;
	int			colorMask;
	GLenum		cullMode;
	GLenum		texEnv;
	GLenum		shadeModel;
	float		clearColor[4];
	int			clearedColor;

	/* current immediate values */
	uint32_t	color;
	float		texCoord[2];

	/* client arrays */
	glArray_t	vertexArray, colorArray, normalArray;
	glArray_t	texCoordArray[2];
	int			clientUnit;

	/* textures */
	pvrTexture_t	*textures[MAX_TEXTURES];
	unsigned char	named[( MAX_TEXTURES + 7 ) / 8];	/* names given out, not yet deleted */
	GLuint		bound;
	int			textureBytes;
	int			outOfVram;
	int			palBanks;		/* bit per 8 bit palette bank in use */

	/* immediate mode: glArrayElement's indices, drawn from the arrays bound,
	   or glVertex's vertices; each grows to the most a glBegin has had */
	GLenum		immMode;
	int			immCount, immSize;
	immVert_t	*immVerts;
	int			numImmIndexes, immIndexSize, immMaxIndex;
	GLuint		*immIndexes;

	/* frame */
	listBuffer_t	lists[5];	/* indexed by pvr_list_t */
	uint8_t		*freeBlocks[LIST_BLOCKS];	/* made, in no list this frame */
	int			numFreeBlocks, numBlocks;
	int			clears;			/* depth clears so far this frame */
	int			drawnSinceClear;
	GLenum		error;
} gl;

/* per draw call vertex cache */
static clipVert_t	*vcache;
static uint32_t		*vstamp;
static int			vcacheSize;
static uint32_t		vgen;

/* per draw call: the mvp with the viewport in it (x y as PVR screen
   space times w), the viewport's edges for OutCode, and the float arrays' strides */
static struct {
	float		m[16];
	float		xLo, xHi, yLo, yHi, depthScale;
	const uint8_t	*pos, *st;
	int			posStride, stStride, posZ;
} xf;

/* ===================================================================== */
/* matrices                                                              */
/* ===================================================================== */

static void Mat_Identity( float *m ) {
	memset( m, 0, 16 * sizeof( float ) );
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* out = a * b, column major */
static void Mat_Mul( float *out, const float *a, const float *b ) {
	float r[16];
	int i, j;

	for ( i = 0; i < 4; i++ ) {
		for ( j = 0; j < 4; j++ ) {
			r[j * 4 + i] = a[0 * 4 + i] * b[j * 4 + 0] + a[1 * 4 + i] * b[j * 4 + 1]
						 + a[2 * 4 + i] * b[j * 4 + 2] + a[3 * 4 + i] * b[j * 4 + 3];
		}
	}
	memcpy( out, r, sizeof( r ) );
}

static float *CurrentMatrix( void ) {
	gl.mvpDirty = 1;
	if ( gl.matrixMode == GL_PROJECTION ) {
		return gl.projection[gl.projDepth];
	}
	return gl.modelview[gl.mvDepth];
}

static void UpdateMVP( void ) {
	if ( gl.mvpDirty ) {
		Mat_Mul( gl.mvp, gl.projection[gl.projDepth], gl.modelview[gl.mvDepth] );
		gl.mvpDirty = 0;
	}
}

void APIENTRY pvrglMatrixMode( GLenum mode ) {
	gl.matrixMode = mode;
}

void APIENTRY pvrglLoadIdentity( void ) {
	Mat_Identity( CurrentMatrix() );
}

void APIENTRY pvrglLoadMatrixf( const GLfloat *m ) {
	memcpy( CurrentMatrix(), m, 16 * sizeof( float ) );
}

void APIENTRY pvrglPushMatrix( void ) {
	if ( gl.matrixMode == GL_PROJECTION ) {
		if ( gl.projDepth < MAX_MATRIX_DEPTH - 1 ) {
			memcpy( gl.projection[gl.projDepth + 1], gl.projection[gl.projDepth], 16 * sizeof( float ) );
			gl.projDepth++;
		}
	} else if ( gl.matrixMode == GL_MODELVIEW ) {
		if ( gl.mvDepth < MAX_MATRIX_DEPTH - 1 ) {
			memcpy( gl.modelview[gl.mvDepth + 1], gl.modelview[gl.mvDepth], 16 * sizeof( float ) );
			gl.mvDepth++;
		}
	}
}

void APIENTRY pvrglPopMatrix( void ) {
	if ( gl.matrixMode == GL_PROJECTION ) {
		if ( gl.projDepth > 0 ) {
			gl.projDepth--;
		}
	} else if ( gl.matrixMode == GL_MODELVIEW ) {
		if ( gl.mvDepth > 0 ) {
			gl.mvDepth--;
		}
	}
	gl.mvpDirty = 1;
}

static void MultCurrent( const float *m ) {
	float *c = CurrentMatrix();
	Mat_Mul( c, c, m );
}

void APIENTRY pvrglTranslatef( GLfloat x, GLfloat y, GLfloat z ) {
	float m[16];
	Mat_Identity( m );
	m[12] = x; m[13] = y; m[14] = z;
	MultCurrent( m );
}

void APIENTRY pvrglOrtho( GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f ) {
	float m[16];
	Mat_Identity( m );
	m[0] = 2.0f / ( r - l );
	m[5] = 2.0f / ( t - b );
	m[10] = -2.0f / ( f - n );
	m[12] = -( r + l ) / ( r - l );
	m[13] = -( t + b ) / ( t - b );
	m[14] = -( f + n ) / ( f - n );
	MultCurrent( m );
}

void APIENTRY pvrglFrustum( GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f ) {
	float m[16];
	memset( m, 0, sizeof( m ) );
	m[0] = 2.0f * n / ( r - l );
	m[5] = 2.0f * n / ( t - b );
	m[8] = ( r + l ) / ( r - l );
	m[9] = ( t + b ) / ( t - b );
	m[10] = -( f + n ) / ( f - n );
	m[11] = -1.0f;
	m[14] = -2.0f * f * n / ( f - n );
	MultCurrent( m );
}

void APIENTRY pvrglViewport( GLint x, GLint y, GLsizei w, GLsizei h ) {
	gl.vpX = x; gl.vpY = y; gl.vpW = w; gl.vpH = h;
}

void APIENTRY pvrglDepthRange( GLclampd n, GLclampd f ) {
	gl.depthNear = n;
	gl.depthFar = f;
}

/* ===================================================================== */
/* state                                                                 */
/* ===================================================================== */

static int *CapFlag( GLenum cap ) {
	switch ( cap ) {
	case GL_TEXTURE_2D:				return &gl.texture2D;
	case GL_BLEND:					return &gl.blend;
	case GL_ALPHA_TEST:				return &gl.alphaTest;
	case GL_DEPTH_TEST:				return &gl.depthTest;
	case GL_CULL_FACE:				return &gl.cullFace;
	case GL_FOG:					return &gl.fog;
	case GL_SCISSOR_TEST:			return &gl.scissor;
	case GL_STENCIL_TEST:			return &gl.stencil;
	case GL_POLYGON_OFFSET_FILL:	return &gl.polygonOffset;
	case GL_CLIP_PLANE0:			return &gl.clipPlane0;
	default:						return NULL;
	}
}

static glArray_t *ClientArray( GLenum cap ) {
	switch ( cap ) {
	case GL_VERTEX_ARRAY:			return &gl.vertexArray;
	case GL_COLOR_ARRAY:			return &gl.colorArray;
	case GL_NORMAL_ARRAY:			return &gl.normalArray;
	case GL_TEXTURE_COORD_ARRAY:	return &gl.texCoordArray[gl.clientUnit];
	default:						return NULL;
	}
}

void APIENTRY pvrglEnable( GLenum cap ) {
	int *f = CapFlag( cap );
	if ( f ) {
		*f = 1;
	}
}

void APIENTRY pvrglDisable( GLenum cap ) {
	int *f = CapFlag( cap );
	if ( f ) {
		*f = 0;
	}
}

GLboolean APIENTRY pvrglIsEnabled( GLenum cap ) {
	int *f = CapFlag( cap );
	glArray_t *a;

	if ( f ) {
		return *f ? GL_TRUE : GL_FALSE;
	}
	a = ClientArray( cap );
	return a && a->enabled ? GL_TRUE : GL_FALSE;
}

void APIENTRY pvrglEnableClientState( GLenum cap ) {
	glArray_t *a = ClientArray( cap );
	if ( a ) {
		a->enabled = 1;
	}
}

void APIENTRY pvrglDisableClientState( GLenum cap ) {
	glArray_t *a = ClientArray( cap );
	if ( a ) {
		a->enabled = 0;
	}
}

static void SetArray( glArray_t *a, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	a->size = size;
	a->type = type;
	a->stride = stride;
	a->ptr = ptr;
}

void APIENTRY pvrglVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	SetArray( &gl.vertexArray, size, type, stride, ptr );
}

void APIENTRY pvrglColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	SetArray( &gl.colorArray, size, type, stride, ptr );
}

void APIENTRY pvrglTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	SetArray( &gl.texCoordArray[gl.clientUnit], size, type, stride, ptr );
}

void APIENTRY pvrglNormalPointer( GLenum type, GLsizei stride, const GLvoid *ptr ) {
	SetArray( &gl.normalArray, 3, type, stride, ptr );
}

void APIENTRY pvrglBlendFunc( GLenum s, GLenum d ) {
	gl.blendSrc = s;
	gl.blendDst = d;
}

void APIENTRY pvrglAlphaFunc( GLenum func, GLclampf ref ) {
	gl.alphaFunc = func;
	gl.alphaRef = ref;
}

void APIENTRY pvrglDepthFunc( GLenum func ) {
	gl.depthFunc = func;
}

void APIENTRY pvrglDepthMask( GLboolean flag ) {
	gl.depthMask = flag != 0;
}

void APIENTRY pvrglColorMask( GLboolean r, GLboolean g, GLboolean b, GLboolean a ) {
	(void)a;	/* the frame buffer has no alpha */
	gl.colorMask = r || g || b;
}

void APIENTRY pvrglCullFace( GLenum mode ) {
	gl.cullMode = mode;
}

void APIENTRY pvrglTexEnvf( GLenum target, GLenum pname, GLfloat param ) {
	if ( target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE ) {
		gl.texEnv = (GLenum)param;
	}
}

void APIENTRY pvrglShadeModel( GLenum mode ) {
	gl.shadeModel = mode;
}

void APIENTRY pvrglClearColor( GLclampf r, GLclampf g, GLclampf b, GLclampf a ) {
	gl.clearColor[0] = r;
	gl.clearColor[1] = g;
	gl.clearColor[2] = b;
	gl.clearColor[3] = a;
}

void APIENTRY pvrglClear( GLbitfield mask ) {
	if ( mask & GL_COLOR_BUFFER_BIT ) {
		/* the PVR clears with its background plane once per frame */
		gl.clearedColor = 1;
	}
	if ( ( mask & GL_DEPTH_BUFFER_BIT ) && gl.drawnSinceClear ) {
		if ( gl.clears < DEPTH_MAX_CLEARS ) {
			gl.clears++;
		}
		gl.drawnSinceClear = 0;
	}
}

static uint32_t PackColor( float r, float g, float b, float a ) {
	int ir = (int)( r * 255.0f ), ig = (int)( g * 255.0f );
	int ib = (int)( b * 255.0f ), ia = (int)( a * 255.0f );

	ir = ir < 0 ? 0 : ir > 255 ? 255 : ir;
	ig = ig < 0 ? 0 : ig > 255 ? 255 : ig;
	ib = ib < 0 ? 0 : ib > 255 ? 255 : ib;
	ia = ia < 0 ? 0 : ia > 255 ? 255 : ia;
	return ( (uint32_t)ia << 24 ) | ( ir << 16 ) | ( ig << 8 ) | ib;
}

void APIENTRY pvrglColor4f( GLfloat r, GLfloat g, GLfloat b, GLfloat a ) {
	gl.color = PackColor( r, g, b, a );
}

void APIENTRY pvrglColor3f( GLfloat r, GLfloat g, GLfloat b ) {
	gl.color = PackColor( r, g, b, 1.0f );
}

void APIENTRY pvrglColor3fv( const GLfloat *v ) {
	gl.color = PackColor( v[0], v[1], v[2], 1.0f );
}

void APIENTRY pvrglColor4ub( GLubyte r, GLubyte g, GLubyte b, GLubyte a ) {
	gl.color = ( (uint32_t)a << 24 ) | ( r << 16 ) | ( g << 8 ) | b;
}

void APIENTRY pvrglColor4ubv( const GLubyte *v ) {
	pvrglColor4ub( v[0], v[1], v[2], v[3] );
}

void APIENTRY pvrglTexCoord2f( GLfloat s, GLfloat t ) {
	gl.texCoord[0] = s;
	gl.texCoord[1] = t;
}

void APIENTRY pvrglTexCoord2fv( const GLfloat *v ) {
	pvrglTexCoord2f( v[0], v[1] );
}

void APIENTRY pvrglMultiTexCoord4f( GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q ) {
	(void)r; (void)q;
	if ( target == GL_TEXTURE0 ) {
		pvrglTexCoord2f( s, t );
	}
}

/* unsupported or meaningless here */
void APIENTRY pvrglClearStencil( GLint s ) { (void)s; }
void APIENTRY pvrglStencilFunc( GLenum func, GLint ref, GLuint mask ) { (void)func; (void)ref; (void)mask; }
void APIENTRY pvrglStencilMask( GLuint mask ) { (void)mask; }
void APIENTRY pvrglStencilOp( GLenum fail, GLenum zfail, GLenum zpass ) { (void)fail; (void)zfail; (void)zpass; }
void APIENTRY pvrglLineWidth( GLfloat width ) { (void)width; }
void APIENTRY pvrglPolygonOffset( GLfloat factor, GLfloat units ) { (void)factor; (void)units; }
void APIENTRY pvrglScissor( GLint x, GLint y, GLsizei w, GLsizei h ) { (void)x; (void)y; (void)w; (void)h; }
void APIENTRY pvrglHint( GLenum target, GLenum mode ) { (void)target; (void)mode; }
void APIENTRY pvrglPixelStorei( GLenum pname, GLint param ) { (void)pname; (void)param; }
void APIENTRY pvrglFogf( GLenum pname, GLfloat param ) { (void)pname; (void)param; }
void APIENTRY pvrglFogi( GLenum pname, GLint param ) { (void)pname; (void)param; }
void APIENTRY pvrglFogfv( GLenum pname, const GLfloat *params ) { (void)pname; (void)params; }
void APIENTRY pvrglClearDepth( GLclampd depth ) { (void)depth; }
void APIENTRY pvrglDrawBuffer( GLenum mode ) { (void)mode; }
void APIENTRY pvrglPolygonMode( GLenum face, GLenum mode ) { (void)face; (void)mode; }
void APIENTRY pvrglClipPlane( GLenum plane, const GLdouble *eq ) { (void)plane; (void)eq; }
void APIENTRY pvrglFinish( void ) {}
void APIENTRY pvrglFlush( void ) {}

void APIENTRY pvrglCopyTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset,
									 GLint x, GLint y, GLsizei width, GLsizei height ) {
	(void)target; (void)level; (void)xoffset; (void)yoffset;
	(void)x; (void)y; (void)width; (void)height;
}

GLenum APIENTRY pvrglGetError( void ) {
	GLenum e = gl.error;
	gl.error = GL_NO_ERROR;
	return e;
}

void APIENTRY pvrglGetIntegerv( GLenum pname, GLint *params ) {
	switch ( pname ) {
	case GL_MAX_TEXTURE_SIZE:
		params[0] = PVRGL_MAX_TEXTURE_SIZE;
		break;
	case GL_MAX_TEXTURE_UNITS:
		params[0] = 1;
		break;
	case GL_VIEWPORT:
		params[0] = gl.vpX; params[1] = gl.vpY;
		params[2] = gl.vpW; params[3] = gl.vpH;
		break;
	case GL_STENCIL_BITS:
	case GL_DEPTH_BITS:
	case GL_ALPHA_BITS:
	default:
		params[0] = 0;
		break;
	case GL_RED_BITS:
	case GL_BLUE_BITS:
		params[0] = 5;
		break;
	case GL_GREEN_BITS:
		params[0] = 6;
		break;
	}
}

void APIENTRY pvrglGetBooleanv( GLenum pname, GLboolean *params ) {
	(void)pname;
	params[0] = GL_FALSE;
}

const GLubyte * APIENTRY pvrglGetString( GLenum name ) {
	switch ( name ) {
	case GL_VENDOR:		return (const GLubyte *)"KallistiOS";
	case GL_RENDERER:	return (const GLubyte *)"PowerVR CLX2 (pvr_gl)";
	case GL_VERSION:	return (const GLubyte *)"1.1";
	default:			return (const GLubyte *)"";
	}
}

void APIENTRY pvrglReadPixels( GLint x, GLint y, GLsizei width, GLsizei height,
							  GLenum format, GLenum type, GLvoid *pixels ) {
	int bpp = format == GL_RGBA ? 4 : 3;
	(void)x; (void)y; (void)type;
	/* TODO: read the front buffer back for screenshots */
	memset( pixels, 0, (size_t)width * height * bpp );
}

/* ===================================================================== */
/* textures                                                              */
/* ===================================================================== */

/* PVR twiddled texel index, as libpvr's TextureSampler::twiddle_address */
static uint32_t SpreadBits( uint32_t x ) {
	x &= 0x3ff;
	x = ( x | ( x << 8 ) ) & 0x00ff00ff;
	x = ( x | ( x << 4 ) ) & 0x0f0f0f0f;
	x = ( x | ( x << 2 ) ) & 0x33333333;
	x = ( x | ( x << 1 ) ) & 0x55555555;
	return x;
}

static uint32_t TwiddleIndex( int u, int v, int w, int h ) {
	int minSize = w < h ? w : h;
	int minBits = 0, s;
	uint32_t mask, idx;

	for ( s = minSize; s > 1; s >>= 1 ) {
		minBits++;
	}
	mask = minSize - 1;
	idx = SpreadBits( v & mask ) | ( SpreadBits( u & mask ) << 1 );
	if ( w != h ) {
		uint32_t extra = (uint32_t)( w > h ? u : v ) >> minBits;
		idx |= extra << ( 2 * minBits );
	}
	return idx;
}

static int PowerOfTwoAtLeast8( int n ) {
	int p = 8;
	while ( p < n && p < 1024 ) {
		p <<= 1;
	}
	return p;
}

static int FormatHasAlpha( GLint internalFormat ) {
	switch ( internalFormat ) {
	case 3:
	case GL_RGB:
	case GL_RGB5:
	case GL_RGB8:
	case GL_LUMINANCE:
	case GL_LUMINANCE8:
		return 0;
	default:
		return 1;
	}
}

/* read one source pixel as RGBA8 */
static void SourcePixel( const uint8_t *src, GLenum format, int index, uint8_t *rgba ) {
	switch ( format ) {
	case GL_RGB:
		src += index * 3;
		rgba[0] = src[0]; rgba[1] = src[1]; rgba[2] = src[2]; rgba[3] = 255;
		break;
	case GL_LUMINANCE:
		src += index;
		rgba[0] = rgba[1] = rgba[2] = src[0]; rgba[3] = 255;
		break;
	case GL_LUMINANCE_ALPHA:
		src += index * 2;
		rgba[0] = rgba[1] = rgba[2] = src[0]; rgba[3] = src[1];
		break;
	case GL_ALPHA:
		src += index;
		rgba[0] = rgba[1] = rgba[2] = 255; rgba[3] = src[0];
		break;
	case GL_BGRA:
		src += index * 4;
		rgba[0] = src[2]; rgba[1] = src[1]; rgba[2] = src[0]; rgba[3] = src[3];
		break;
	case GL_RGBA:
	default:
		src += index * 4;
		rgba[0] = src[0]; rgba[1] = src[1]; rgba[2] = src[2]; rgba[3] = src[3];
		break;
	}
}

static uint16_t PackTexel( const uint8_t *rgba, int format ) {
	if ( format == PVR_TXRFMT_ARGB4444 ) {
		return ( ( rgba[3] >> 4 ) << 12 ) | ( ( rgba[0] >> 4 ) << 8 )
			 | ( ( rgba[1] >> 4 ) << 4 ) | ( rgba[2] >> 4 );
	}
	return ( ( rgba[0] >> 3 ) << 11 ) | ( ( rgba[1] >> 2 ) << 5 ) | ( rgba[2] >> 3 );
}

/* Write the source rectangle (sx,sy,sw,sh of a srcW x srcH image) into the
 * twiddled PVR texture.  Images smaller than the PVR minimum of 8 are
 * stretched (nearest) to fill it. */
static void WriteTexels( pvrTexture_t *t, int dx, int dy, int sw, int sh,
						 GLenum format, const uint8_t *src ) {
	int sx = t->width / t->srcWidth, sy = t->height / t->srcHeight;
	uint16_t *dst = (uint16_t *)t->data;
	uint8_t rgba[4];
	int x, y, i, j;

	if ( sx < 1 ) sx = 1;
	if ( sy < 1 ) sy = 1;

	for ( y = 0; y < sh; y++ ) {
		for ( x = 0; x < sw; x++ ) {
			uint16_t texel;

			SourcePixel( src, format, y * sw + x, rgba );
			texel = PackTexel( rgba, t->format );
			for ( j = 0; j < sy; j++ ) {
				for ( i = 0; i < sx; i++ ) {
					int u = ( dx + x ) * sx + i, v = ( dy + y ) * sy + j;
					if ( u < t->width && v < t->height ) {
						dst[TwiddleIndex( u, v, t->width, t->height )] = texel;
					}
				}
			}
		}
	}
}

static pvrTexture_t *BoundTexture( int create ) {
	pvrTexture_t *t;

	if ( gl.bound >= MAX_TEXTURES ) {
		return NULL;
	}
	t = gl.textures[gl.bound];
	if ( !t && create ) {
		t = calloc( 1, sizeof( *t ) );
		t->linear = 1;
		gl.textures[gl.bound] = t;
	}
	return t;
}

/* print resident textures grouped by size, to see where VRAM went */
static void TextureStats( void ) {
	int counts[8][8][2];	/* log2(w) - 3, log2(h) - 3, has alpha */
	int bytes[8][8][2];
	int i, x, y, a;

	memset( counts, 0, sizeof( counts ) );
	memset( bytes, 0, sizeof( bytes ) );
	for ( i = 0; i < MAX_TEXTURES; i++ ) {
		pvrTexture_t *t = gl.textures[i];
		if ( t && t->data ) {
			for ( x = 0; ( 8 << x ) < t->width; x++ );
			for ( y = 0; ( 8 << y ) < t->height; y++ );
			counts[x][y][t->alpha]++;
			bytes[x][y][t->alpha] += t->bytes;
		}
	}
	fprintf( stderr, "pvr_gl: resident textures (%d KB):\n", gl.textureBytes / 1024 );
	for ( x = 0; x < 8; x++ ) {
		for ( y = 0; y < 8; y++ ) {
			for ( a = 0; a < 2; a++ ) {
				if ( counts[x][y][a] ) {
					fprintf( stderr, "  %4dx%-4d %s %4d  %6d KB\n", 8 << x, 8 << y, a ? "alpha" : "     ",
							 counts[x][y][a], bytes[x][y][a] / 1024 );
				}
			}
		}
	}
}

static void FreeTextureData( pvrTexture_t *t ) {
	if ( t->data ) {
		pvr_mem_free( t->data );
		gl.textureBytes -= t->bytes;
		t->data = NULL;
		t->bytes = 0;
	}
	if ( t->palBank ) {
		gl.palBanks &= ~( 1 << ( t->palBank - 1 ) );
		t->palBank = 0;
	}
}

void APIENTRY pvrglGenTextures( GLsizei n, GLuint *textures ) {
	GLuint i, name;

	/* the lowest free, as deleted names come back: 0 is no texture */
	for ( i = 0, name = 1; i < (GLuint)n; i++ ) {
		while ( name < MAX_TEXTURES && ( gl.named[name >> 3] & ( 1 << ( name & 7 ) ) ) ) {
			name++;
		}
		if ( name == MAX_TEXTURES ) {
			fprintf( stderr, "pvr_gl: out of texture names (%d)\n", MAX_TEXTURES );
			textures[i] = MAX_TEXTURES;	/* binds to nothing */
			continue;
		}
		gl.named[name >> 3] |= 1 << ( name & 7 );
		textures[i] = name;
	}
}

void APIENTRY pvrglBindTexture( GLenum target, GLuint texture ) {
	(void)target;
	gl.bound = texture;
}

void APIENTRY pvrglDeleteTextures( GLsizei n, const GLuint *textures ) {
	int i;
	for ( i = 0; i < n; i++ ) {
		if ( textures[i] < MAX_TEXTURES ) {
			if ( gl.textures[textures[i]] ) {
				FreeTextureData( gl.textures[textures[i]] );
				free( gl.textures[textures[i]] );
				gl.textures[textures[i]] = NULL;
			}
			gl.named[textures[i] >> 3] &= ~( 1 << ( textures[i] & 7 ) );
		}
	}
}

void APIENTRY pvrglTexImage2D( GLenum target, GLint level, GLint internalFormat,
							  GLsizei width, GLsizei height, GLint border,
							  GLenum format, GLenum type, const GLvoid *pixels ) {
	pvrTexture_t *t;
	int w, h, fmt, bytes;

	(void)target; (void)border; (void)type;

	if ( level != 0 ) {
		return;		/* TODO: PVR mipmaps (square textures only) */
	}
	t = BoundTexture( 1 );
	if ( !t ) {
		return;
	}

	w = PowerOfTwoAtLeast8( width );
	h = PowerOfTwoAtLeast8( height );
	fmt = FormatHasAlpha( internalFormat ) ? PVR_TXRFMT_ARGB4444 : PVR_TXRFMT_RGB565;
	bytes = w * h * 2;

	if ( !t->data || t->bytes != bytes ) {
		FreeTextureData( t );
		t->data = pvr_mem_malloc( bytes );
		if ( !t->data ) {
			if ( !gl.outOfVram ) {
				fprintf( stderr, "pvr_gl: out of texture memory\n" );
				TextureStats();
				gl.outOfVram = 1;
			}
			t->bytes = 0;
			return;
		}
		t->bytes = bytes;
		gl.textureBytes += bytes;
	}
	if ( getenv( "PVRGL_STATS" ) && bytes >= 16384 ) {
		fprintf( stderr, "pvrgl tex %d: %dx%d raw %d KB\n", gl.bound, w, h, bytes / 1024 );
	}

	t->width = w;
	t->height = h;
	t->srcWidth = width;
	t->srcHeight = height;
	t->format = fmt;
	t->txrFormat = fmt | PVR_TXRFMT_TWIDDLED;
	t->base = t->data;
	t->mipmap = 0;
	t->alpha = fmt == PVR_TXRFMT_ARGB4444;

	if ( pixels ) {
		WriteTexels( t, 0, 0, width, height, format, pixels );
	} else {
		memset( t->data, 0, bytes );
	}
}

void APIENTRY pvrglTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset,
								 GLsizei width, GLsizei height, GLenum format, GLenum type,
								 const GLvoid *pixels ) {
	pvrTexture_t *t = BoundTexture( 0 );

	(void)target; (void)type;
	if ( level != 0 || !t || !t->data || !pixels || t->txrFormat != ( t->format | PVR_TXRFMT_TWIDDLED ) ) {
		return;
	}
	WriteTexels( t, xoffset, yoffset, width, height, format, pixels );
}

/* .dt (pvrtex) header, little endian like the SH-4 */
typedef struct {
	char		fourcc[4];		/* "DcTx" */
	uint32_t	chunkSize;		/* header + data, a multiple of 32 */
	uint8_t		version;
	uint8_t		headerSize;		/* in 32 bytes, minus one */
	uint8_t		codebookSize;	/* VQ codebook entries - 1 */
	uint8_t		colorsUsed;
	uint16_t	width, height;	/* the image's, in pixels */
	uint32_t	pvrType;		/* poly mode3 format bits (31-25) | log2 sizes - 3 (5-0) */
	uint8_t		pad[12];
} dtHeader_t;

#define DT_MIPMAP			( 1u << 31 )
#define DT_VQ				( 1u << 30 )
#define DT_FORMAT_BITS		0x7e000000u		/* VQ, pixel format, not twiddled, stride */
#define DT_PIXEL_FORMAT( t )	( ( ( t ) >> 27 ) & 7 )
#define DT_CODEBOOK_BYTES	2048

int pvrgl_TexImageDT( const void *file, int len, int *width, int *height ) {
	const dtHeader_t *h = file;
	pvrTexture_t *t;
	int dataOfs, bytes, pf, colors = 0, bank = 0, i;

	if ( len < (int)sizeof( *h ) || memcmp( h->fourcc, "DcTx", 4 ) || h->version != 0 ) {
		return 0;
	}
	dataOfs = ( h->headerSize + 1 ) * 32;
	bytes = (int)h->chunkSize - dataOfs;
	pf = DT_PIXEL_FORMAT( h->pvrType );
	if ( pf == 6 ) {
		colors = ( len - (int)h->chunkSize ) / 4;	/* rtcwconv puts the ARGB8888 palette after the texels */
	}
	if ( bytes <= 0 || dataOfs + bytes > len || ( pf > 3 && ( pf != 6 || colors < 1 || colors > 256 ) ) ) {
		return 0;	/* 4 bit palettes and normal maps aren't made */
	}
	t = BoundTexture( 1 );
	if ( !t ) {
		return 0;
	}

	FreeTextureData( t );
	if ( colors ) {
		while ( bank < 4 && ( gl.palBanks & ( 1 << bank ) ) ) {
			bank++;
		}
		if ( bank == 4 ) {
			fprintf( stderr, "pvr_gl: out of palette banks\n" );
			return 0;
		}
	}
	t->data = pvr_mem_malloc( bytes );
	if ( !t->data ) {
		if ( !gl.outOfVram ) {
			fprintf( stderr, "pvr_gl: out of texture memory\n" );
			TextureStats();
			gl.outOfVram = 1;
		}
		return 0;
	}
	t->bytes = bytes;
	gl.textureBytes += bytes;
	pvr_txr_load( (const uint8_t *)file + dataOfs, t->data, bytes );
	if ( getenv( "PVRGL_STATS" ) && bytes >= 16384 ) {
		fprintf( stderr, "pvrgl tex %d: %dx%d dt %08x %d KB\n", gl.bound, h->width, h->height, h->pvrType, bytes / 1024 );
	}

	t->width = 8 << ( ( h->pvrType >> 3 ) & 7 );
	t->height = 8 << ( h->pvrType & 7 );
	t->srcWidth = h->width;
	t->srcHeight = h->height;
	t->format = pf << 27;
	t->txrFormat = h->pvrType & DT_FORMAT_BITS;
	if ( colors ) {
		const uint32_t *pal = (const uint32_t *)( (const uint8_t *)file + h->chunkSize );
		for ( i = 0; i < colors; i++ ) {
			pvr_set_pal_entry( bank * 256 + i, pal[i] );
		}
		gl.palBanks |= 1 << bank;
		t->palBank = bank + 1;
		t->txrFormat |= PVR_TXRFMT_8BPP_PAL( bank );
	}
	t->mipmap = ( h->pvrType & DT_MIPMAP ) != 0;
	t->alpha = t->format != PVR_TXRFMT_RGB565 && t->format != PVR_TXRFMT_YUV422;
	/* a VQ codebook of fewer than 256 entries is stored as the end of a full one */
	t->base = t->data;
	if ( h->pvrType & DT_VQ ) {
		t->base = (uint8_t *)t->data - DT_CODEBOOK_BYTES + ( h->codebookSize + 1 ) * 8;
	}

	*width = h->width;
	*height = h->height;
	return 1;
}

static void TexParameter( GLenum pname, GLint param ) {
	pvrTexture_t *t = BoundTexture( 1 );

	if ( !t ) {
		return;
	}
	switch ( pname ) {
	case GL_TEXTURE_WRAP_S:
		t->clampS = param != GL_REPEAT;
		break;
	case GL_TEXTURE_WRAP_T:
		t->clampT = param != GL_REPEAT;
		break;
	case GL_TEXTURE_MAG_FILTER:
		t->linear = param != GL_NEAREST;
		break;
	default:
		break;
	}
}

void APIENTRY pvrglTexParameterf( GLenum target, GLenum pname, GLfloat param ) {
	(void)target;
	TexParameter( pname, (GLint)param );
}

void APIENTRY pvrglTexParameteri( GLenum target, GLenum pname, GLint param ) {
	(void)target;
	TexParameter( pname, param );
}

/* ===================================================================== */
/* primitive output                                                      */
/* ===================================================================== */

/* room for 32 bytes at the list's end, or NULL when it's full; its cache
   line taken without a read from RAM, so all 32 are to be written */
static uint32_t *ListAlloc( listBuffer_t *l ) {
	uint32_t *d;

	if ( l->overflowed ) {
		return NULL;
	}
	if ( l->used == l->numBlocks * LIST_BLOCK ) {
		uint8_t *block = NULL;

		if ( gl.numFreeBlocks ) {
			block = gl.freeBlocks[--gl.numFreeBlocks];
		} else if ( gl.numBlocks < LIST_BLOCKS && ( block = memalign( 32, LIST_BLOCK ) ) ) {
			gl.numBlocks++;
		}
		if ( !block ) {
			// no half a primitive for the PVR
			l->overflowed = 1;
			l->used = l->whole;
			return NULL;
		}
		l->blocks[l->numBlocks++] = block;
	}
	d = (uint32_t *)( l->blocks[l->used / LIST_BLOCK] + l->used % LIST_BLOCK );
	l->used += 32;
#ifdef USE_SH4ZAM
	shz_dcache_alloc_line( d );
#endif
	return d;
}

static void ListWrite( listBuffer_t *l, const void *data ) {
	const uint32_t *s = data;
	uint32_t *d = ListAlloc( l );

	if ( d ) {
		d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
		d[4] = s[4]; d[5] = s[5]; d[6] = s[6]; d[7] = s[7];
		if ( s[0] != PVR_CMD_VERTEX ) {
			l->whole = l->used;
		}
	}
}

static int BlendFactor( GLenum f, int isDst ) {
	switch ( f ) {
	case GL_ZERO:					return PVR_BLEND_ZERO;
	case GL_ONE:					return PVR_BLEND_ONE;
	/* the PVR's "other colour": dest colour for src, src colour for dst */
	case GL_DST_COLOR:				return isDst ? PVR_BLEND_ONE : PVR_BLEND_DESTCOLOR;
	case GL_ONE_MINUS_DST_COLOR:	return isDst ? PVR_BLEND_ZERO : PVR_BLEND_INVDESTCOLOR;
	case GL_SRC_COLOR:				return isDst ? PVR_BLEND_DESTCOLOR : PVR_BLEND_ONE;
	case GL_ONE_MINUS_SRC_COLOR:	return isDst ? PVR_BLEND_INVDESTCOLOR : PVR_BLEND_ZERO;
	case GL_SRC_ALPHA:				return PVR_BLEND_SRCALPHA;
	case GL_ONE_MINUS_SRC_ALPHA:	return PVR_BLEND_INVSRCALPHA;
	/* no destination alpha: treat it as 1 */
	case GL_DST_ALPHA:				return PVR_BLEND_ONE;
	case GL_ONE_MINUS_DST_ALPHA:	return PVR_BLEND_ZERO;
	case GL_SRC_ALPHA_SATURATE:		return PVR_BLEND_ONE;
	default:						return PVR_BLEND_ONE;
	}
}

static int DepthCompare( void ) {
	if ( !gl.depthTest ) {
		return PVR_DEPTHCMP_ALWAYS;
	}
	/* depth is 1/w: bigger is nearer, so GL comparisons flip */
	switch ( gl.depthFunc ) {
	case GL_NEVER:		return PVR_DEPTHCMP_NEVER;
	case GL_LESS:		return PVR_DEPTHCMP_GREATER;
	case GL_EQUAL:		return PVR_DEPTHCMP_EQUAL;
	case GL_LEQUAL:		return PVR_DEPTHCMP_GEQUAL;
	case GL_GREATER:	return PVR_DEPTHCMP_LESS;
	case GL_NOTEQUAL:	return PVR_DEPTHCMP_NOTEQUAL;
	case GL_GEQUAL:		return PVR_DEPTHCMP_LEQUAL;
	case GL_ALWAYS:
	default:			return PVR_DEPTHCMP_ALWAYS;
	}
}

/* Pick the list and write the poly header for the current state.
 * Returns the list, or -1 if nothing should be drawn. */
static int BeginPrimitives( void ) {
	pvr_poly_cxt_t cxt;
	pvr_poly_hdr_t hdr;
	pvrTexture_t *t = NULL;
	listBuffer_t *l;
	int list, blended;

	if ( !gl.colorMask ) {
		return -1;		/* depth-only pass: no way to write depth alone */
	}

	blended = gl.blend && !( gl.blendSrc == GL_ONE && gl.blendDst == GL_ZERO );
	if ( blended ) {
		list = PVR_LIST_TR_POLY;
	} else if ( gl.alphaTest ) {
		list = PVR_LIST_PT_POLY;
	} else {
		list = PVR_LIST_OP_POLY;
	}

	if ( gl.texture2D ) {
		t = BoundTexture( 0 );
		if ( t && !t->data ) {
			t = NULL;
		}
	}

	if ( t ) {
		pvr_poly_cxt_txr( &cxt, list, t->txrFormat, t->width, t->height, t->base,
						  t->linear ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST );
		cxt.txr.mipmap = t->mipmap ? PVR_MIPMAP_ENABLE : PVR_MIPMAP_DISABLE;
		cxt.txr.uv_clamp = ( t->clampS ? PVR_UVCLAMP_U : 0 ) | ( t->clampT ? PVR_UVCLAMP_V : 0 );
		switch ( gl.texEnv ) {
		case GL_REPLACE:	cxt.txr.env = PVR_TXRENV_REPLACE; break;
		case GL_DECAL:		cxt.txr.env = PVR_TXRENV_DECAL; break;
		case GL_MODULATE:
		default:			cxt.txr.env = PVR_TXRENV_MODULATEALPHA; break;
		}
		cxt.txr.alpha = t->alpha ? PVR_TXRALPHA_ENABLE : PVR_TXRALPHA_DISABLE;
	} else {
		pvr_poly_cxt_col( &cxt, list );
	}

	cxt.gen.culling = PVR_CULLING_NONE;		/* culled on the CPU */
	cxt.gen.shading = gl.shadeModel == GL_FLAT ? PVR_SHADE_FLAT : PVR_SHADE_GOURAUD;
	cxt.gen.alpha = list != PVR_LIST_OP_POLY ? PVR_ALPHA_ENABLE : PVR_ALPHA_DISABLE;
	cxt.depth.comparison = DepthCompare();
	cxt.depth.write = gl.depthMask && gl.depthTest ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;

	if ( list == PVR_LIST_TR_POLY ) {
		cxt.blend.src = BlendFactor( gl.blendSrc, 0 );
		cxt.blend.dst = BlendFactor( gl.blendDst, 1 );
	} else {
		cxt.blend.src = PVR_BLEND_ONE;
		cxt.blend.dst = PVR_BLEND_ZERO;
	}

	pvr_poly_compile( &hdr, &cxt );

	l = &gl.lists[list];
	if ( !l->hasLast || memcmp( &l->last, &hdr, sizeof( hdr ) ) ) {
		ListWrite( l, &hdr );
		l->last = hdr;
		l->hasLast = 1;
	}
	gl.drawnSinceClear = 1;
	return list;
}

static float DepthScale( void ) {
	float s;
	int i;

	if ( gl.depthNear >= 1.0f ) {
		s = DEPTH_SCALE_SKY;
	} else if ( gl.depthFar <= 0.0f ) {
		s = DEPTH_SCALE_TOP;
	} else if ( gl.depthFar < 1.0f ) {
		s = DEPTH_SCALE_HACK;
	} else {
		s = 1.0f;
	}
	if ( gl.polygonOffset ) {
		s *= DEPTH_OFFSET_SCALE;
	}
	for ( i = 0; i < gl.clears; i++ ) {
		s *= DEPTH_SCALE_CLEAR;
	}
	return s;
}

/* clip space to PVR screen space, w > 0 */
static void ToScreen( clipVert_t *c, float x, float y, float w ) {
#ifdef USE_SH4ZAM
	float invw = shz_invf_fsrra( w );
#else
	float invw = 1.0f / w;
#endif

	c->sx = x * invw;
	c->sy = y * invw;
	c->sz = invw * xf.depthScale;
}

/* and back, for the near clip */
static void ClipSpace( const clipVert_t *c, float *x, float *y, float *w ) {
	if ( c->code & 1 ) {
		*x = c->x;
		*y = c->y;
		*w = c->w;
	} else {
		*w = xf.depthScale / c->sz;
		*x = c->sx * *w;
		*y = c->sy * *w;
	}
}

static void EmitVertex( listBuffer_t *l, const clipVert_t *c, uint32_t flags ) {
	pvr_vertex_t *v = (pvr_vertex_t *)ListAlloc( l );

	if ( !v ) {
		return;
	}
	PROF_COUNT( STAT_EMITTED, 1 );
	v->flags = flags;
	v->x = c->sx;
	v->y = c->sy;
	v->z = c->sz;
	v->u = c->u;
	v->v = c->v;
	v->argb = c->argb;
	v->oargb = 0;
	if ( flags != PVR_CMD_VERTEX ) {
		l->whole = l->used;
	}
}

static int Culled( const clipVert_t *a, const clipVert_t *b, const clipVert_t *c ) {
	float area;

	if ( !gl.cullFace ) {
		return 0;
	}
	/* > 0 is counter-clockwise in GL window space (front facing) */
	area = ( b->sx - a->sx ) * ( a->sy - c->sy ) - ( c->sx - a->sx ) * ( a->sy - b->sy );
	if ( gl.cullMode == GL_FRONT ) {
		return area > 0.0f;
	}
	if ( gl.cullMode == GL_BACK ) {
		return area < 0.0f;
	}
	return 1;	/* GL_FRONT_AND_BACK */
}

static void LerpVert( clipVert_t *out, const clipVert_t *a, const clipVert_t *b, float t ) {
	int i;

	float ax, ay, aw, bx, by, bw;

	ClipSpace( a, &ax, &ay, &aw );
	ClipSpace( b, &bx, &by, &bw );
	out->u = a->u + ( b->u - a->u ) * t;
	out->v = a->v + ( b->v - a->v ) * t;
	out->argb = 0;
	for ( i = 0; i < 32; i += 8 ) {
		int ca = ( a->argb >> i ) & 0xff, cb = ( b->argb >> i ) & 0xff;
		out->argb |= (uint32_t)( ca + ( cb - ca ) * t + 0.5f ) << i;
	}
	out->d = 0.0f;
	out->code = 0;
	ToScreen( out, ax + ( bx - ax ) * t, ay + ( by - ay ) * t, aw + ( bw - aw ) * t );
}

static int OutCode( float x, float y, float d, float w ) {
	int c = 0;
	if ( d < 0.0f ) c |= 1;
	if ( x < xf.xLo * w ) c |= 2;
	if ( x > xf.xHi * w ) c |= 4;
	if ( y < xf.yLo * w ) c |= 8;
	if ( y > xf.yHi * w ) c |= 16;
	return c;
}

static void EmitTriangle( listBuffer_t *l, const clipVert_t *a, const clipVert_t *b,
						  const clipVert_t *c ) {
	int ca = a->code, cb = b->code, cc = c->code;
	const clipVert_t *in[3] = { a, b, c };
	clipVert_t out[4];
	int i, n;

	PROF_COUNT( STAT_TRIS, 1 );
	if ( ca & cb & cc ) {
		PROF_COUNT( STAT_CULLED, 1 );
		return;		/* all outside one plane */
	}

	if ( !( ( ca | cb | cc ) & 1 ) ) {
		if ( Culled( a, b, c ) ) {
			PROF_COUNT( STAT_CULLED, 1 );
			return;
		}
		EmitVertex( l, a, PVR_CMD_VERTEX );
		EmitVertex( l, b, PVR_CMD_VERTEX );
		EmitVertex( l, c, PVR_CMD_VERTEX_EOL );
		return;
	}

	/* clip against the near plane z = -w */
	PROF_COUNT( STAT_CLIPPED, 1 );
	n = 0;
	for ( i = 0; i < 3; i++ ) {
		const clipVert_t *p = in[i], *q = in[( i + 1 ) % 3];
		float dp = p->d, dq = q->d;

		if ( dp >= 0.0f ) {
			out[n++] = *p;
		}
		if ( ( dp >= 0.0f ) != ( dq >= 0.0f ) ) {
			LerpVert( &out[n++], p, q, dp / ( dp - dq ) );
		}
	}
	if ( n < 3 || Culled( &out[0], &out[1], &out[2] ) ) {
		return;
	}
	if ( n == 3 ) {
		EmitVertex( l, &out[0], PVR_CMD_VERTEX );
		EmitVertex( l, &out[1], PVR_CMD_VERTEX );
		EmitVertex( l, &out[2], PVR_CMD_VERTEX_EOL );
	} else {
		/* quad 0 1 2 3 as the strip 0 1 3 2 */
		EmitVertex( l, &out[0], PVR_CMD_VERTEX );
		EmitVertex( l, &out[1], PVR_CMD_VERTEX );
		EmitVertex( l, &out[3], PVR_CMD_VERTEX );
		EmitVertex( l, &out[2], PVR_CMD_VERTEX_EOL );
	}
}

/* a strip being sent (its vertexes so far not ended): its last made the end */
static void EndStrip( listBuffer_t *l ) {
	if ( !l->overflowed && l->used > l->whole ) {
		*(uint32_t *)( l->blocks[( l->used - 32 ) / LIST_BLOCK] + ( l->used - 32 ) % LIST_BLOCK ) = PVR_CMD_VERTEX_EOL;
		l->whole = l->used;
	}
}

/* ===================================================================== */
/* vertex fetch                                                          */
/* ===================================================================== */

static float ArrayFloat( const glArray_t *a, int index, int comp ) {
	const uint8_t *p = a->ptr;
	int stride;

	switch ( a->type ) {
	case GL_FLOAT:
		stride = a->stride ? a->stride : a->size * 4;
		return ( (const float *)( p + index * stride ) )[comp];
	case GL_SHORT:
		stride = a->stride ? a->stride : a->size * 2;
		return ( (const short *)( p + index * stride ) )[comp];
	case GL_INT:
		stride = a->stride ? a->stride : a->size * 4;
		return (float)( (const int *)( p + index * stride ) )[comp];
	case GL_DOUBLE:
		stride = a->stride ? a->stride : a->size * 8;
		return (float)( (const double *)( p + index * stride ) )[comp];
	default:
		return 0.0f;
	}
}

static uint32_t ArrayColor( const glArray_t *a, int index ) {
	if ( a->type == GL_UNSIGNED_BYTE ) {
		int stride = a->stride ? a->stride : a->size;
		const uint8_t *c = (const uint8_t *)a->ptr + index * stride;
		uint32_t alpha = a->size > 3 ? c[3] : 255;
		return ( alpha << 24 ) | ( c[0] << 16 ) | ( c[1] << 8 ) | c[2];
	}
	return PackColor( ArrayFloat( a, index, 0 ), ArrayFloat( a, index, 1 ), ArrayFloat( a, index, 2 ),
					  a->size > 3 ? ArrayFloat( a, index, 3 ) : 1.0f );
}

static void GrowCache( int count ) {
	if ( count <= vcacheSize ) {
		return;
	}
	vcacheSize = ( count + 255 ) & ~255;
	free( vcache );
	vcache = memalign( 32, vcacheSize * sizeof( *vcache ) );
	vstamp = realloc( vstamp, vcacheSize * sizeof( *vstamp ) );
	memset( vstamp, 0, vcacheSize * sizeof( *vstamp ) );
	vgen = 0;
}

static const clipVert_t *FetchVertex( int i ) {
	clipVert_t *c = &vcache[i];
	float x, y, z;

	if ( vstamp[i] == vgen ) {
		return c;
	}
	vstamp[i] = vgen;
	PROF_COUNT( STAT_VERTS, 1 );
#ifdef USE_SH4ZAM
	shz_dcache_alloc_line( c );	/* all 32 bytes written below, so no read from RAM */
#endif

	if ( xf.pos ) {
		const float *p = (const float *)( xf.pos + i * xf.posStride );
		x = p[0];
		y = p[1];
		z = xf.posZ ? p[2] : 0.0f;
	} else {
		x = ArrayFloat( &gl.vertexArray, i, 0 );
		y = ArrayFloat( &gl.vertexArray, i, 1 );
		z = gl.vertexArray.size > 2 ? ArrayFloat( &gl.vertexArray, i, 2 ) : 0.0f;
	}

	{
#ifdef USE_SH4ZAM
		shz_vec4_t t = shz_xmtrx_transform_vec4( shz_vec4_init( x, y, z, 1.0f ) );
		float cx = t.x, cy = t.y, cz = t.z, cw = t.w;
#else
		const float *m = xf.m;
		float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
		float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
		float cz = m[2] * x + m[6] * y + m[10] * z + m[14];
		float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
#endif

		c->d = cz + cw;
		c->code = OutCode( cx, cy, c->d, cw );
		if ( c->code & 1 ) {
			c->x = cx;
			c->y = cy;
			c->w = cw;
		} else {
			ToScreen( c, cx, cy, cw );
		}
	}

	if ( xf.st ) {
		const float *p = (const float *)( xf.st + i * xf.stStride );
		c->u = p[0];
		c->v = p[1];
	} else if ( gl.texCoordArray[0].enabled ) {
		c->u = ArrayFloat( &gl.texCoordArray[0], i, 0 );
		c->v = ArrayFloat( &gl.texCoordArray[0], i, 1 );
	} else {
		c->u = gl.texCoord[0];
		c->v = gl.texCoord[1];
	}
	c->argb = gl.colorArray.enabled ? ArrayColor( &gl.colorArray, i ) : gl.color;
	return c;
}

typedef int ( *indexFunc_t )( const void *indices, int i );

static int IndexDirect( const void *base, int i ) { return (int)(intptr_t)base + i; }
static int IndexUInt( const void *p, int i ) { return ( (const GLuint *)p )[i]; }
static int IndexUShort( const void *p, int i ) { return ( (const GLushort *)p )[i]; }
static int IndexUByte( const void *p, int i ) { return ( (const GLubyte *)p )[i]; }

/* a triangle that goes on from the one before as in a strip (the converter
   puts them so) is sent as one more vertex of a PVR strip, unless it's to be
   culled or clipped, which ends the strip */
static inline __attribute__((always_inline)) void DrawTriangles( listBuffer_t *l, int count, indexFunc_t idx, const void *indices ) {
	int last[3] = { -1, -1, -1 }, odd = 0, open = 0;
	int i;

	for ( i = 0; i + 2 < count; i += 3 ) {
		int a = idx( indices, i ), b = idx( indices, i + 1 ), c = idx( indices, i + 2 );
		const clipVert_t *va, *vb, *vc;

#ifdef USE_SH4ZAM
		/* the next triangle's new vertex (in strip order its last), on its way in */
		if ( xf.pos && i + 5 < count ) {
			SHZ_PREFETCH( xf.pos + idx( indices, i + 5 ) * xf.posStride );
		}
#endif
		va = FetchVertex( a );
		vb = FetchVertex( b );
		vc = FetchVertex( c );
		int on = odd ? a == last[0] && b == last[2] : a == last[2] && b == last[1];
		int ca = va->code, cb = vb->code, cc = vc->code;

		odd = on ? !odd : 0;
		last[0] = a; last[1] = b; last[2] = c;
		if ( ( ca | cb | cc ) & 1 || ca & cb & cc || Culled( va, vb, vc ) ) {
			if ( open ) {
				EndStrip( l );
				open = 0;
			}
			EmitTriangle( l, va, vb, vc );
			continue;
		}
		if ( !( on && open ) ) {
			if ( open ) {
				EndStrip( l );
			}
			/* every other triangle of a strip is turned (c b d after
			   a b c): from one of those, b c d, so the next (c d e)
			   goes on from its last two */
			EmitVertex( l, odd ? vb : va, PVR_CMD_VERTEX );
			EmitVertex( l, odd ? va : vb, PVR_CMD_VERTEX );
			open = 1;
		}
		PROF_COUNT( STAT_TRIS, 1 );
		EmitVertex( l, vc, PVR_CMD_VERTEX );
	}
	if ( open ) {
		EndStrip( l );
	}
}

static void DrawPrimitivesPVR( GLenum mode, int count, indexFunc_t idx, const void *indices, int maxIndex ) {
	listBuffer_t *l;
	int list, i;

	if ( count < 3 || !gl.vertexArray.enabled || !gl.vertexArray.ptr ) {
		return;
	}
	if ( mode != GL_TRIANGLES && mode != GL_TRIANGLE_STRIP && mode != GL_TRIANGLE_FAN
		 && mode != GL_QUADS && mode != GL_POLYGON && mode != GL_QUAD_STRIP ) {
		return;		/* TODO: lines and points */
	}

	list = BeginPrimitives();
	if ( list < 0 ) {
		return;
	}
	l = &gl.lists[list];

	UpdateMVP();
	{
		/* the viewport after the mvp: x' = x * xScale + w * xOfs, y' the same */
		float xScale = gl.vpW * 0.5f, xOfs = gl.vpX + gl.vpW * 0.5f;
		float yScale = gl.vpH * -0.5f, yOfs = PVRGL_HEIGHT - ( gl.vpY + gl.vpH * 0.5f );

		for ( i = 0; i < 16; i += 4 ) {
			xf.m[i] = gl.mvp[i] * xScale + gl.mvp[i + 3] * xOfs;
			xf.m[i + 1] = gl.mvp[i + 1] * yScale + gl.mvp[i + 3] * yOfs;
			xf.m[i + 2] = gl.mvp[i + 2];
			xf.m[i + 3] = gl.mvp[i + 3];
		}
		/* x < -w, x > w, y > w, y < -w */
		xf.xLo = xOfs - xScale;
		xf.xHi = xOfs + xScale;
		xf.yLo = yOfs + yScale;
		xf.yHi = yOfs - yScale;
	}
#ifdef USE_SH4ZAM
	shz_xmtrx_load_unaligned_4x4( xf.m );
#endif
	xf.depthScale = DepthScale();
	xf.pos = gl.vertexArray.type == GL_FLOAT ? gl.vertexArray.ptr : NULL;
	xf.posStride = gl.vertexArray.stride ? gl.vertexArray.stride : gl.vertexArray.size * 4;
	xf.posZ = gl.vertexArray.size > 2;
	xf.st = gl.texCoordArray[0].enabled && gl.texCoordArray[0].type == GL_FLOAT ? gl.texCoordArray[0].ptr : NULL;
	xf.stStride = gl.texCoordArray[0].stride ? gl.texCoordArray[0].stride : gl.texCoordArray[0].size * 4;
	GrowCache( maxIndex + 1 );
	if ( ++vgen == 0 ) {
		memset( vstamp, 0, vcacheSize * sizeof( *vstamp ) );
		vgen = 1;
	}

#define V( n ) FetchVertex( idx( indices, n ) )
	switch ( mode ) {
	case GL_TRIANGLES:
		/* each index type its own copy, the index read inlined */
		if ( idx == IndexUInt ) {
			DrawTriangles( l, count, IndexUInt, indices );
		} else if ( idx == IndexUShort ) {
			DrawTriangles( l, count, IndexUShort, indices );
		} else {
			DrawTriangles( l, count, idx, indices );
		}
		break;
	case GL_TRIANGLE_STRIP:
		for ( i = 0; i + 2 < count; i++ ) {
			if ( i & 1 ) {
				EmitTriangle( l, V( i + 1 ), V( i ), V( i + 2 ) );
			} else {
				EmitTriangle( l, V( i ), V( i + 1 ), V( i + 2 ) );
			}
		}
		break;
	case GL_TRIANGLE_FAN:
	case GL_POLYGON:
		for ( i = 1; i + 1 < count; i++ ) {
			EmitTriangle( l, V( 0 ), V( i ), V( i + 1 ) );
		}
		break;
	case GL_QUADS:
		for ( i = 0; i + 3 < count; i += 4 ) {
			EmitTriangle( l, V( i ), V( i + 1 ), V( i + 2 ) );
			EmitTriangle( l, V( i ), V( i + 2 ), V( i + 3 ) );
		}
		break;
	case GL_QUAD_STRIP:
		for ( i = 0; i + 3 < count; i += 2 ) {
			EmitTriangle( l, V( i ), V( i + 1 ), V( i + 3 ) );
			EmitTriangle( l, V( i ), V( i + 3 ), V( i + 2 ) );
		}
		break;
	}
#undef V
}

/* DC_PROF: its time as pvr */
static void DrawPrimitives( GLenum mode, int count, indexFunc_t idx, const void *indices, int maxIndex ) {
	PROF_BEGIN( PROF_PVR );
	PROF_COUNT( STAT_DRAWS, 1 );
	DrawPrimitivesPVR( mode, count, idx, indices, maxIndex );
	PROF_END( PROF_PVR );
}

void APIENTRY pvrglDrawArrays( GLenum mode, GLint first, GLsizei count ) {
	DrawPrimitives( mode, count, IndexDirect, (const void *)(intptr_t)first, first + count - 1 );
}

void APIENTRY pvrglDrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices ) {
	indexFunc_t f;
	int i, maxIndex = 0;

	switch ( type ) {
	case GL_UNSIGNED_INT:	f = IndexUInt; break;
	case GL_UNSIGNED_SHORT:	f = IndexUShort; break;
	case GL_UNSIGNED_BYTE:	f = IndexUByte; break;
	default:				return;
	}
#define MAX_INDEX( t ) for ( i = 0; i < count; i++ ) { int n = ( (const t *)indices )[i]; if ( n > maxIndex ) maxIndex = n; }
	if ( type == GL_UNSIGNED_INT ) {
		MAX_INDEX( GLuint )
	} else if ( type == GL_UNSIGNED_SHORT ) {
		MAX_INDEX( GLushort )
	} else {
		MAX_INDEX( GLubyte )
	}
#undef MAX_INDEX
	DrawPrimitives( mode, count, f, indices, maxIndex );
}

/* ===================================================================== */
/* immediate mode: collected into arrays and drawn at glEnd              */
/* ===================================================================== */

void APIENTRY pvrglBegin( GLenum mode ) {
	gl.immMode = mode;
	gl.immCount = 0;
	gl.numImmIndexes = 0;
	gl.immMaxIndex = 0;
}

/* p's room for n of size each, at least: grown by half again */
static void *ImmGrow( void *p, int *size, int n, int each ) {
	if ( n > *size ) {
		*size = n + n / 2 + 64;
		p = realloc( p, *size * each );
		if ( !p ) {
			fprintf( stderr, "pvr_gl: out of memory for %d immediate vertices\n", n );
			abort();
		}
	}
	return p;
}

static void ArrayElementVertex( GLint i );

static void ImmVertex( float x, float y, float z ) {
	immVert_t *v;

	if ( gl.numImmIndexes ) {
		/* glVertex after glArrayElement in one glBegin: theirs as vertices too */
		int k, n = gl.numImmIndexes;

		gl.numImmIndexes = 0;
		for ( k = 0; k < n; k++ ) {
			ArrayElementVertex( gl.immIndexes[k] );
		}
	}
	gl.immVerts = ImmGrow( gl.immVerts, &gl.immSize, gl.immCount + 1, sizeof( *gl.immVerts ) );
	v = &gl.immVerts[gl.immCount++];
	v->xyz[0] = x;
	v->xyz[1] = y;
	v->xyz[2] = z;
	v->st[0] = gl.texCoord[0];
	v->st[1] = gl.texCoord[1];
	v->rgba[0] = gl.color >> 16;
	v->rgba[1] = gl.color >> 8;
	v->rgba[2] = gl.color;
	v->rgba[3] = gl.color >> 24;
}

void APIENTRY pvrglVertex2f( GLfloat x, GLfloat y ) { ImmVertex( x, y, 0.0f ); }
void APIENTRY pvrglVertex3f( GLfloat x, GLfloat y, GLfloat z ) { ImmVertex( x, y, z ); }
void APIENTRY pvrglVertex3fv( const GLfloat *v ) { ImmVertex( v[0], v[1], v[2] ); }

/* glArrayElement as a glVertex: its array values the current ones */
static void ArrayElementVertex( GLint i ) {
	if ( gl.texCoordArray[0].enabled ) {
		gl.texCoord[0] = ArrayFloat( &gl.texCoordArray[0], i, 0 );
		gl.texCoord[1] = ArrayFloat( &gl.texCoordArray[0], i, 1 );
	}
	if ( gl.colorArray.enabled ) {
		gl.color = ArrayColor( &gl.colorArray, i );
	}
	if ( gl.vertexArray.enabled ) {
		ImmVertex( ArrayFloat( &gl.vertexArray, i, 0 ), ArrayFloat( &gl.vertexArray, i, 1 ),
				   gl.vertexArray.size > 2 ? ArrayFloat( &gl.vertexArray, i, 2 ) : 0.0f );
	}
}

/* only its index, unless glVertex has been used too: drawn at glEnd
   from the arrays bound, which give the same vertex as copying it would */
void APIENTRY pvrglArrayElement( GLint i ) {
	if ( gl.immCount ) {
		ArrayElementVertex( i );
		return;
	}
	gl.immIndexes = ImmGrow( gl.immIndexes, &gl.immIndexSize, gl.numImmIndexes + 1, sizeof( *gl.immIndexes ) );
	gl.immIndexes[gl.numImmIndexes++] = i;
	if ( i > gl.immMaxIndex ) {
		gl.immMaxIndex = i;
	}
}

void APIENTRY pvrglEnd( void ) {
	glArray_t saveV, saveC, saveT;

	if ( gl.numImmIndexes ) {
		DrawPrimitives( gl.immMode, gl.numImmIndexes, IndexUInt, gl.immIndexes, gl.immMaxIndex );
		gl.numImmIndexes = 0;
		return;
	}

	saveV = gl.vertexArray;
	saveC = gl.colorArray;
	saveT = gl.texCoordArray[0];
	SetArray( &gl.vertexArray, 3, GL_FLOAT, sizeof( immVert_t ), gl.immVerts ? gl.immVerts->xyz : NULL );
	SetArray( &gl.colorArray, 4, GL_UNSIGNED_BYTE, sizeof( immVert_t ), gl.immVerts ? gl.immVerts->rgba : NULL );
	SetArray( &gl.texCoordArray[0], 2, GL_FLOAT, sizeof( immVert_t ), gl.immVerts ? gl.immVerts->st : NULL );
	gl.vertexArray.enabled = gl.colorArray.enabled = gl.texCoordArray[0].enabled = 1;

	DrawPrimitives( gl.immMode, gl.immCount, IndexDirect, (const void *)0, gl.immCount - 1 );

	gl.vertexArray = saveV;
	gl.colorArray = saveC;
	gl.texCoordArray[0] = saveT;
	gl.immCount = 0;
}

/* ===================================================================== */
/* frame                                                                 */
/* ===================================================================== */

static void ResetLists( void ) {
	int i;
	for ( i = 0; i < 5; i++ ) {
		while ( gl.lists[i].numBlocks ) {
			gl.freeBlocks[gl.numFreeBlocks++] = gl.lists[i].blocks[--gl.lists[i].numBlocks];
		}
		gl.lists[i].used = gl.lists[i].whole = 0;
		gl.lists[i].overflowed = 0;
		gl.lists[i].hasLast = 0;
	}
	gl.clears = 0;
	gl.drawnSinceClear = 0;
	gl.clearedColor = 0;
}

void pvrgl_EndFrame( void ) {
	static const int order[3] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };
	int i;

	if ( !gl.inited ) {
		return;
	}

	PROF_BEGIN( PROF_GPU );
	pvr_wait_ready();
	PROF_END( PROF_GPU );
	pvr_set_bg_color( gl.clearColor[0], gl.clearColor[1], gl.clearColor[2] );
	PVR_SET( PVR_PT_ALPHA_REF, 0x80 );
	PROF_BEGIN( PROF_SUBMIT );
	pvr_scene_begin();
	if ( getenv( "PVRGL_STATS" ) ) {
		static int n;
		fprintf( stderr, "pvrgl frame %d: op %d pt %d tr %d KB, textures %d KB\n", n++, gl.lists[0].used / 1024,
				 gl.lists[PVR_LIST_PT_POLY].used / 1024, gl.lists[PVR_LIST_TR_POLY].used / 1024, gl.textureBytes / 1024 );
	}
	for ( i = 0; i < 3; i++ ) {
		listBuffer_t *l = &gl.lists[order[i]];
		int b;
		if ( !l->used ) {
			continue;
		}
		if ( getenv( "PVRGL_SKIP" ) && strchr( getenv( "PVRGL_SKIP" ), '0' + order[i] ) ) {
			continue;
		}
		if ( l->overflowed ) {
			fprintf( stderr, "pvr_gl: list %d overflowed: the lists have %d KB between them\n", order[i], VERTEX_BUFFER / 1024 );
		}
		pvr_list_begin( order[i] );
		for ( b = 0; b * LIST_BLOCK < l->used; b++ ) {
			pvr_prim( l->blocks[b], l->used - b * LIST_BLOCK < LIST_BLOCK ? l->used - b * LIST_BLOCK : LIST_BLOCK );
		}
		pvr_list_finish();
	}
	pvr_scene_finish();
	PROF_END( PROF_SUBMIT );

	ResetLists();
}

int pvrgl_Init( void ) {
	pvr_init_params_t params = {
		{ PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
		VERTEX_BUFFER,	/* vertex buffer */
		0,				/* no DMA */
		0,				/* no FSAA */
		1,				/* autosort disabled: translucent in submission order */
		3,				/* extra OPBs */
		0
	};

	if ( gl.inited ) {
		return 0;
	}
	if ( pvr_init( &params ) ) {
		return -1;
	}

	memset( &gl, 0, sizeof( gl ) );	/* the list blocks are made as they get used (ListWrite) */

	gl.matrixMode = GL_MODELVIEW;
	Mat_Identity( gl.modelview[0] );
	Mat_Identity( gl.projection[0] );
	gl.mvpDirty = 1;
	gl.vpW = PVRGL_WIDTH;
	gl.vpH = PVRGL_HEIGHT;
	gl.depthNear = 0.0f;
	gl.depthFar = 1.0f;
	gl.blendSrc = GL_ONE;
	gl.blendDst = GL_ZERO;
	gl.alphaFunc = GL_ALWAYS;
	gl.depthFunc = GL_LESS;
	gl.depthMask = 1;
	gl.colorMask = 1;
	gl.cullMode = GL_BACK;
	gl.texEnv = GL_MODULATE;
	gl.shadeModel = GL_SMOOTH;
	gl.color = 0xffffffff;
	pvr_set_pal_format( PVR_PAL_ARGB8888 );
	gl.inited = 1;
	return 0;
}

void pvrgl_Shutdown( void ) {
	int i;

	if ( !gl.inited ) {
		return;
	}
	for ( i = 0; i < MAX_TEXTURES; i++ ) {
		if ( gl.textures[i] ) {
			FreeTextureData( gl.textures[i] );
			free( gl.textures[i] );
		}
	}
	ResetLists();
	for ( i = 0; i < gl.numFreeBlocks; i++ ) {
		free( gl.freeBlocks[i] );
	}
	free( vcache );
	free( vstamp );
	vcache = NULL;
	vstamp = NULL;
	vcacheSize = 0;
	memset( &gl, 0, sizeof( gl ) );
	pvr_shutdown();
}
