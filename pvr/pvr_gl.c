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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dc/pvr.h>

#include "pvr_gl.h"

#ifndef PVR_PT_ALPHA_REF
#define PVR_PT_ALPHA_REF	0x011c
#endif

#define MAX_MATRIX_DEPTH	32
#define MAX_TEXTURES		16384
#define MAX_IMMEDIATE		4096
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
} pvrTexture_t;

typedef struct {
	GLint		size;
	GLenum		type;
	GLsizei		stride;
	const GLvoid	*ptr;
	int			enabled;
} glArray_t;

/* a vertex on its way to the PVR */
typedef struct {
	float		x, y, z, w;		/* clip space */
	float		u, v;
	uint32_t	argb;
} clipVert_t;

typedef struct {
	uint8_t		*blocks[LIST_BLOCKS];
	int			numBlocks;
	int			used;			/* bytes, in all its blocks */
	int			whole;			/* used, to the end of its last whole primitive */
	int			overflowed;
	pvr_poly_hdr_t	last;		/* last header written to this list */
	int			hasLast;
} listBuffer_t;

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
	GLuint		bound;
	GLuint		nextName;
	int			textureBytes;
	int			outOfVram;

	/* immediate mode */
	GLenum		immMode;
	int			immCount;
	float		immXYZ[MAX_IMMEDIATE][3];
	float		immST[MAX_IMMEDIATE][2];
	uint8_t		immRGBA[MAX_IMMEDIATE][4];

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
}

void APIENTRY pvrglGenTextures( GLsizei n, GLuint *textures ) {
	int i;
	for ( i = 0; i < n; i++ ) {
		/* the renderer picks its own names from 1024 up; stay below them */
		textures[i] = ++gl.nextName;
	}
}

void APIENTRY pvrglBindTexture( GLenum target, GLuint texture ) {
	(void)target;
	gl.bound = texture;
}

void APIENTRY pvrglDeleteTextures( GLsizei n, const GLuint *textures ) {
	int i;
	for ( i = 0; i < n; i++ ) {
		if ( textures[i] < MAX_TEXTURES && gl.textures[textures[i]] ) {
			FreeTextureData( gl.textures[textures[i]] );
			free( gl.textures[textures[i]] );
			gl.textures[textures[i]] = NULL;
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
	int dataOfs, bytes, pf;

	if ( len < (int)sizeof( *h ) || memcmp( h->fourcc, "DcTx", 4 ) || h->version != 0 ) {
		return 0;
	}
	dataOfs = ( h->headerSize + 1 ) * 32;
	bytes = (int)h->chunkSize - dataOfs;
	pf = DT_PIXEL_FORMAT( h->pvrType );
	if ( bytes <= 0 || dataOfs + bytes > len || pf > 2 ) {
		return 0;	/* palettes, YUV and normal maps aren't made */
	}
	t = BoundTexture( 1 );
	if ( !t ) {
		return 0;
	}

	FreeTextureData( t );
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
	t->mipmap = ( h->pvrType & DT_MIPMAP ) != 0;
	t->alpha = t->format != PVR_TXRFMT_RGB565;
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

static void ListWrite( listBuffer_t *l, const void *data ) {
	if ( l->overflowed ) {
		return;
	}
	if ( l->used == l->numBlocks * LIST_BLOCK ) {
		uint8_t *block = NULL;

		if ( gl.numFreeBlocks ) {
			block = gl.freeBlocks[--gl.numFreeBlocks];
		} else if ( gl.numBlocks < LIST_BLOCKS && ( block = malloc( LIST_BLOCK ) ) ) {
			gl.numBlocks++;
		}
		if ( !block ) {
			// no half a primitive for the PVR
			l->overflowed = 1;
			l->used = l->whole;
			return;
		}
		l->blocks[l->numBlocks++] = block;
	}
	memcpy( l->blocks[l->used / LIST_BLOCK] + l->used % LIST_BLOCK, data, 32 );
	l->used += 32;
	if ( *(const uint32_t *)data != PVR_CMD_VERTEX ) {
		l->whole = l->used;
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
	return s * powf( DEPTH_SCALE_CLEAR, (float)gl.clears );
}

static void EmitVertex( listBuffer_t *l, const clipVert_t *c, uint32_t flags, float depthScale ) {
	pvr_vertex_t v;
	float invw = 1.0f / c->w;

	v.flags = flags;
	v.x = gl.vpX + ( c->x * invw * 0.5f + 0.5f ) * gl.vpW;
	v.y = PVRGL_HEIGHT - ( gl.vpY + ( c->y * invw * 0.5f + 0.5f ) * gl.vpH );
	v.z = invw * depthScale;
	v.u = c->u;
	v.v = c->v;
	v.argb = c->argb;
	v.oargb = 0;
	ListWrite( l, &v );
}

/* GL window space signed area: > 0 is counter-clockwise (front facing) */
static float WindowArea( const clipVert_t *a, const clipVert_t *b, const clipVert_t *c ) {
	float ax = a->x / a->w, ay = a->y / a->w;
	float bx = b->x / b->w, by = b->y / b->w;
	float cx = c->x / c->w, cy = c->y / c->w;
	return ( bx - ax ) * ( cy - ay ) - ( cx - ax ) * ( by - ay );
}

static int Culled( const clipVert_t *a, const clipVert_t *b, const clipVert_t *c ) {
	float area;

	if ( !gl.cullFace ) {
		return 0;
	}
	area = WindowArea( a, b, c );
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

	out->x = a->x + ( b->x - a->x ) * t;
	out->y = a->y + ( b->y - a->y ) * t;
	out->z = a->z + ( b->z - a->z ) * t;
	out->w = a->w + ( b->w - a->w ) * t;
	out->u = a->u + ( b->u - a->u ) * t;
	out->v = a->v + ( b->v - a->v ) * t;
	out->argb = 0;
	for ( i = 0; i < 32; i += 8 ) {
		int ca = ( a->argb >> i ) & 0xff, cb = ( b->argb >> i ) & 0xff;
		out->argb |= (uint32_t)( ca + ( cb - ca ) * t + 0.5f ) << i;
	}
}

static int OutCode( const clipVert_t *v ) {
	int c = 0;
	if ( v->z < -v->w ) c |= 1;
	if ( v->x < -v->w ) c |= 2;
	if ( v->x > v->w ) c |= 4;
	if ( v->y < -v->w ) c |= 8;
	if ( v->y > v->w ) c |= 16;
	return c;
}

static void EmitTriangle( listBuffer_t *l, const clipVert_t *a, const clipVert_t *b,
						  const clipVert_t *c, float depthScale ) {
	int ca = OutCode( a ), cb = OutCode( b ), cc = OutCode( c );
	clipVert_t in[3], out[4];
	int i, n;

	if ( ca & cb & cc ) {
		return;		/* all outside one plane */
	}

	if ( !( ( ca | cb | cc ) & 1 ) ) {
		if ( Culled( a, b, c ) ) {
			return;
		}
		EmitVertex( l, a, PVR_CMD_VERTEX, depthScale );
		EmitVertex( l, b, PVR_CMD_VERTEX, depthScale );
		EmitVertex( l, c, PVR_CMD_VERTEX_EOL, depthScale );
		return;
	}

	/* clip against the near plane z = -w */
	in[0] = *a; in[1] = *b; in[2] = *c;
	n = 0;
	for ( i = 0; i < 3; i++ ) {
		const clipVert_t *p = &in[i], *q = &in[( i + 1 ) % 3];
		float dp = p->z + p->w, dq = q->z + q->w;

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
		EmitVertex( l, &out[0], PVR_CMD_VERTEX, depthScale );
		EmitVertex( l, &out[1], PVR_CMD_VERTEX, depthScale );
		EmitVertex( l, &out[2], PVR_CMD_VERTEX_EOL, depthScale );
	} else {
		/* quad 0 1 2 3 as the strip 0 1 3 2 */
		EmitVertex( l, &out[0], PVR_CMD_VERTEX, depthScale );
		EmitVertex( l, &out[1], PVR_CMD_VERTEX, depthScale );
		EmitVertex( l, &out[3], PVR_CMD_VERTEX, depthScale );
		EmitVertex( l, &out[2], PVR_CMD_VERTEX_EOL, depthScale );
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
	vcacheSize = count + 1024;
	vcache = realloc( vcache, vcacheSize * sizeof( *vcache ) );
	vstamp = realloc( vstamp, vcacheSize * sizeof( *vstamp ) );
	memset( vstamp, 0, vcacheSize * sizeof( *vstamp ) );
	vgen = 0;
}

static const clipVert_t *FetchVertex( int i ) {
	clipVert_t *c = &vcache[i];
	const float *m = gl.mvp;
	float x, y, z;

	if ( vstamp[i] == vgen ) {
		return c;
	}
	vstamp[i] = vgen;

	x = ArrayFloat( &gl.vertexArray, i, 0 );
	y = ArrayFloat( &gl.vertexArray, i, 1 );
	z = gl.vertexArray.size > 2 ? ArrayFloat( &gl.vertexArray, i, 2 ) : 0.0f;

	c->x = m[0] * x + m[4] * y + m[8] * z + m[12];
	c->y = m[1] * x + m[5] * y + m[9] * z + m[13];
	c->z = m[2] * x + m[6] * y + m[10] * z + m[14];
	c->w = m[3] * x + m[7] * y + m[11] * z + m[15];

	if ( gl.texCoordArray[0].enabled ) {
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

static void DrawPrimitives( GLenum mode, int count, indexFunc_t idx, const void *indices, int maxIndex ) {
	listBuffer_t *l;
	float depthScale;
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
	depthScale = DepthScale();
	GrowCache( maxIndex + 1 );
	if ( ++vgen == 0 ) {
		memset( vstamp, 0, vcacheSize * sizeof( *vstamp ) );
		vgen = 1;
	}

#define V( n ) FetchVertex( idx( indices, n ) )
	switch ( mode ) {
	case GL_TRIANGLES:
		for ( i = 0; i + 2 < count; i += 3 ) {
			EmitTriangle( l, V( i ), V( i + 1 ), V( i + 2 ), depthScale );
		}
		break;
	case GL_TRIANGLE_STRIP:
		for ( i = 0; i + 2 < count; i++ ) {
			if ( i & 1 ) {
				EmitTriangle( l, V( i + 1 ), V( i ), V( i + 2 ), depthScale );
			} else {
				EmitTriangle( l, V( i ), V( i + 1 ), V( i + 2 ), depthScale );
			}
		}
		break;
	case GL_TRIANGLE_FAN:
	case GL_POLYGON:
		for ( i = 1; i + 1 < count; i++ ) {
			EmitTriangle( l, V( 0 ), V( i ), V( i + 1 ), depthScale );
		}
		break;
	case GL_QUADS:
		for ( i = 0; i + 3 < count; i += 4 ) {
			EmitTriangle( l, V( i ), V( i + 1 ), V( i + 2 ), depthScale );
			EmitTriangle( l, V( i ), V( i + 2 ), V( i + 3 ), depthScale );
		}
		break;
	case GL_QUAD_STRIP:
		for ( i = 0; i + 3 < count; i += 2 ) {
			EmitTriangle( l, V( i ), V( i + 1 ), V( i + 3 ), depthScale );
			EmitTriangle( l, V( i ), V( i + 3 ), V( i + 2 ), depthScale );
		}
		break;
	}
#undef V
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
	for ( i = 0; i < count; i++ ) {
		int n = f( indices, i );
		if ( n > maxIndex ) {
			maxIndex = n;
		}
	}
	DrawPrimitives( mode, count, f, indices, maxIndex );
}

/* ===================================================================== */
/* immediate mode: collected into arrays and drawn at glEnd              */
/* ===================================================================== */

void APIENTRY pvrglBegin( GLenum mode ) {
	gl.immMode = mode;
	gl.immCount = 0;
}

static void ImmVertex( float x, float y, float z ) {
	int n = gl.immCount;

	if ( n >= MAX_IMMEDIATE ) {
		return;
	}
	gl.immXYZ[n][0] = x;
	gl.immXYZ[n][1] = y;
	gl.immXYZ[n][2] = z;
	gl.immST[n][0] = gl.texCoord[0];
	gl.immST[n][1] = gl.texCoord[1];
	gl.immRGBA[n][0] = gl.color >> 16;
	gl.immRGBA[n][1] = gl.color >> 8;
	gl.immRGBA[n][2] = gl.color;
	gl.immRGBA[n][3] = gl.color >> 24;
	gl.immCount++;
}

void APIENTRY pvrglVertex2f( GLfloat x, GLfloat y ) { ImmVertex( x, y, 0.0f ); }
void APIENTRY pvrglVertex3f( GLfloat x, GLfloat y, GLfloat z ) { ImmVertex( x, y, z ); }
void APIENTRY pvrglVertex3fv( const GLfloat *v ) { ImmVertex( v[0], v[1], v[2] ); }

void APIENTRY pvrglArrayElement( GLint i ) {
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

void APIENTRY pvrglEnd( void ) {
	glArray_t saveV = gl.vertexArray, saveC = gl.colorArray, saveT = gl.texCoordArray[0];

	SetArray( &gl.vertexArray, 3, GL_FLOAT, 0, gl.immXYZ );
	SetArray( &gl.colorArray, 4, GL_UNSIGNED_BYTE, 0, gl.immRGBA );
	SetArray( &gl.texCoordArray[0], 2, GL_FLOAT, 0, gl.immST );
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

	pvr_wait_ready();
	pvr_set_bg_color( gl.clearColor[0], gl.clearColor[1], gl.clearColor[2] );
	PVR_SET( PVR_PT_ALPHA_REF, 0x80 );
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
	gl.nextName = 0;
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
