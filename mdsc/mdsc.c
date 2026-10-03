/*
 * MDSC decoding, and the conversions the converter encodes with.
 * See mdsc.h.
 *
 * A bone's .mds angles become its matrix with AnglesToAxis, and the renderer
 * applies it as model = M * local (each row dotted with the vertex offset).
 * So the bone relative to its parent is L = Mp^T * M, and back M = Mp * L.
 */
#include <math.h>
#include <string.h>

#include "mdsc.h"

/* where they are in an mdsHeader_t / mdscBoneInfo_t */
#define HDR_NUMFRAMES   80
#define HDR_NUMBONES    84
#define HDR_OFSFRAMES   88
#define HDR_OFSBONES    92
#define BONEINFO_SIZE   ( (int)sizeof( mdscBoneInfo_t ) )
#define BONEINFO_PARENT 0

#define DEG2RAD( a )    ( (a) * ( (float)M_PI / 180.0f ) )
#define RAD2DEG( a )    ( (a) * ( 180.0f / (float)M_PI ) )

#define QUAT_RANGE      0.70710678f     /* the 3 smaller components are within +-1/sqrt(2) */

static float ShortToAngle( short s ) {
	return s * ( 360.0f / 65536.0f );
}

static short AngleToShort( float a ) {
	return (short)(unsigned short)( lrintf( a * ( 65536.0f / 360.0f ) ) & 65535 );
}

/* q_math.c's AnglesToAxis */
void MDSC_AnglesToMatrix( const short angles[3], mdscMatrix_t m ) {
	float p = DEG2RAD( ShortToAngle( angles[0] ) );
	float y = DEG2RAD( ShortToAngle( angles[1] ) );
	float r = DEG2RAD( ShortToAngle( angles[2] ) );
	float sp = sinf( p ), cp = cosf( p ), sy = sinf( y ), cy = cosf( y ), sr = sinf( r ), cr = cosf( r );

	m[0][0] = cp * cy;
	m[0][1] = cp * sy;
	m[0][2] = -sp;
	m[1][0] = sr * sp * cy - cr * sy;
	m[1][1] = sr * sp * sy + cr * cy;
	m[1][2] = sr * cp;
	m[2][0] = cr * sp * cy + sr * sy;
	m[2][1] = cr * sp * sy - sr * cy;
	m[2][2] = cr * cp;
}

/* and back */
static void MatrixToAngles( const mdscMatrix_t m, short angles[3] ) {
	float horiz = sqrtf( m[0][0] * m[0][0] + m[0][1] * m[0][1] );

	if ( horiz > 1e-5f ) {
		angles[0] = AngleToShort( RAD2DEG( atan2f( -m[0][2], horiz ) ) );
		angles[1] = AngleToShort( RAD2DEG( atan2f( m[0][1], m[0][0] ) ) );
		angles[2] = AngleToShort( RAD2DEG( atan2f( m[1][2], m[2][2] ) ) );
	} else {
		/* straight up or down: yaw and roll are the same turn, call it yaw */
		angles[0] = AngleToShort( m[0][2] < 0 ? 90.0f : -90.0f );
		angles[1] = AngleToShort( RAD2DEG( atan2f( -m[1][0], m[1][1] ) ) );
		angles[2] = 0;
	}
}

/* rotation matrix (applied as M * v) <-> quaternion w, x, y, z */
static void QuatToMatrix( const float q[4], mdscMatrix_t m ) {
	float w = q[0], x = q[1], y = q[2], z = q[3];

	m[0][0] = 1 - 2 * ( y * y + z * z );
	m[0][1] = 2 * ( x * y - w * z );
	m[0][2] = 2 * ( x * z + w * y );
	m[1][0] = 2 * ( x * y + w * z );
	m[1][1] = 1 - 2 * ( x * x + z * z );
	m[1][2] = 2 * ( y * z - w * x );
	m[2][0] = 2 * ( x * z - w * y );
	m[2][1] = 2 * ( y * z + w * x );
	m[2][2] = 1 - 2 * ( x * x + y * y );
}

static void MatrixToQuat( const mdscMatrix_t m, float q[4] ) {
	float t = m[0][0] + m[1][1] + m[2][2], s;

	if ( t > 0 ) {
		s = sqrtf( t + 1 ) * 2;
		q[0] = s / 4;
		q[1] = ( m[2][1] - m[1][2] ) / s;
		q[2] = ( m[0][2] - m[2][0] ) / s;
		q[3] = ( m[1][0] - m[0][1] ) / s;
	} else if ( m[0][0] > m[1][1] && m[0][0] > m[2][2] ) {
		s = sqrtf( 1 + m[0][0] - m[1][1] - m[2][2] ) * 2;
		q[0] = ( m[2][1] - m[1][2] ) / s;
		q[1] = s / 4;
		q[2] = ( m[0][1] + m[1][0] ) / s;
		q[3] = ( m[0][2] + m[2][0] ) / s;
	} else if ( m[1][1] > m[2][2] ) {
		s = sqrtf( 1 + m[1][1] - m[0][0] - m[2][2] ) * 2;
		q[0] = ( m[0][2] - m[2][0] ) / s;
		q[1] = ( m[0][1] + m[1][0] ) / s;
		q[2] = s / 4;
		q[3] = ( m[1][2] + m[2][1] ) / s;
	} else {
		s = sqrtf( 1 + m[2][2] - m[0][0] - m[1][1] ) * 2;
		q[0] = ( m[1][0] - m[0][1] ) / s;
		q[1] = ( m[0][2] + m[2][0] ) / s;
		q[2] = ( m[1][2] + m[2][1] ) / s;
		q[3] = s / 4;
	}
}

/* q_math.c's AngleVectors forward, roll 0 */
static void DirFromAngles( short pitch, short yaw, float dir[3] ) {
	float p = DEG2RAD( ShortToAngle( pitch ) ), y = DEG2RAD( ShortToAngle( yaw ) );

	dir[0] = cosf( p ) * cosf( y );
	dir[1] = cosf( p ) * sinf( y );
	dir[2] = -sinf( p );
}

static void DirToAngles( const float dir[3], short *pitch, short *yaw ) {
	float horiz = sqrtf( dir[0] * dir[0] + dir[1] * dir[1] );

	*pitch = AngleToShort( RAD2DEG( atan2f( -dir[2], horiz ) ) );
	*yaw = horiz > 1e-5f ? AngleToShort( RAD2DEG( atan2f( dir[1], dir[0] ) ) ) : 0;
}

void MDSC_LocalRotation( const mdscMatrix_t p, const short angles[3], float q[4] ) {
	mdscMatrix_t m, l;
	int i, j;

	MDSC_AnglesToMatrix( angles, m );
	if ( !p ) {
		MatrixToQuat( m, q );
		return;
	}
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			l[i][j] = p[0][i] * m[0][j] + p[1][i] * m[1][j] + p[2][i] * m[2][j];
		}
	}
	MatrixToQuat( l, q );
}

void MDSC_LocalDir( const mdscMatrix_t p, const short ofsAngles[2], float dir[3] ) {
	float d[3];
	int i;

	DirFromAngles( ofsAngles[0], ofsAngles[1], d );
	for ( i = 0; i < 3; i++ ) {
		dir[i] = p[0][i] * d[0] + p[1][i] * d[1] + p[2][i] * d[2];
	}
}

unsigned int MDSC_PackQuat( const float q[4] ) {
	unsigned int packed;
	float sign;
	int i, big = 0;

	for ( i = 1; i < 4; i++ ) {
		if ( fabsf( q[i] ) > fabsf( q[big] ) ) {
			big = i;
		}
	}
	/* q and -q are the same rotation: make the largest positive, leave it out */
	sign = q[big] < 0 ? -1.0f : 1.0f;
	packed = big;
	for ( i = 0; i < 4; i++ ) {
		float v;
		int n;

		if ( i == big ) {
			continue;
		}
		v = q[i] * sign;
		n = (int)lrintf( ( v + QUAT_RANGE ) * ( 1023.0f / ( 2 * QUAT_RANGE ) ) );
		n = n < 0 ? 0 : n > 1023 ? 1023 : n;
		packed = ( packed << 10 ) | n;
	}
	return packed;
}

static void UnpackQuat( unsigned int packed, float q[4] ) {
	int big = packed >> 30, i, shift = 20;
	float sum = 0;

	for ( i = 0; i < 4; i++ ) {
		if ( i == big ) {
			continue;
		}
		q[i] = ( ( packed >> shift ) & 1023 ) * ( 2 * QUAT_RANGE / 1023.0f ) - QUAT_RANGE;
		sum += q[i] * q[i];
		shift -= 10;
	}
	q[big] = sum < 1 ? sqrtf( 1 - sum ) : 0;
}

void MDSC_PackDir( const float dir[3], short packed[4] ) {
	int i;

	for ( i = 0; i < 3; i++ ) {
		packed[i] = (short)lrintf( dir[i] * MDSC_DIR_SCALE );
	}
	packed[3] = 0;
}

/* the short way round, normalized */
static void LerpQuat( const float a[4], const float b[4], float t, float out[4] ) {
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	float tb = dot < 0 ? -t : t, len;
	int i;

	for ( i = 0; i < 4; i++ ) {
		out[i] = a[i] * ( 1 - t ) + b[i] * tb;
	}
	len = sqrtf( out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3] );
	for ( i = 0; i < 4; i++ ) {
		out[i] /= len;
	}
}

float MDSC_QuatAngle( const float a[4], const float b[4] ) {
	float dot = fabsf( a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] );

	return dot >= 1 ? 0 : RAD2DEG( 2 * acosf( dot ) );
}

float MDSC_DirAngle( const float a[3], const float b[3] ) {
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	float la = sqrtf( a[0] * a[0] + a[1] * a[1] + a[2] * a[2] );
	float lb = sqrtf( b[0] * b[0] + b[1] * b[1] + b[2] * b[2] );

	dot /= la * lb;
	return dot >= 1 ? 0 : dot <= -1 ? 180 : RAD2DEG( acosf( dot ) );
}

static int ReadInt( const unsigned char *p ) {
	int v;
	memcpy( &v, p, 4 );
	return v;
}

/* the keys either side of frame: k with keys[k] <= frame < keys[k + 1] */
static int FindKey( const unsigned short *keys, int numKeys, int frame ) {
	int lo = 0, hi = numKeys - 1;

	while ( lo < hi ) {
		int mid = ( lo + hi + 1 ) / 2;
		if ( keys[mid] <= frame ) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	return lo;
}

void MDSC_TrackQuat( const unsigned short *keys, const unsigned int *values, int numKeys, int frame, float q[4] ) {
	int k = FindKey( keys, numKeys, frame );

	if ( keys[k] == frame || k + 1 >= numKeys ) {
		UnpackQuat( values[k], q );
	} else {
		float a[4], b[4];

		UnpackQuat( values[k], a );
		UnpackQuat( values[k + 1], b );
		LerpQuat( a, b, (float)( frame - keys[k] ) / (float)( keys[k + 1] - keys[k] ), q );
	}
}

void MDSC_TrackDir( const unsigned short *keys, const short *values, int numKeys, int frame, float dir[3] ) {
	int k = FindKey( keys, numKeys, frame ), i;
	const short *a = values + k * 4;

	if ( keys[k] == frame || k + 1 >= numKeys ) {
		for ( i = 0; i < 3; i++ ) {
			dir[i] = a[i];
		}
	} else {
		float t = (float)( frame - keys[k] ) / (float)( keys[k + 1] - keys[k] );
		for ( i = 0; i < 3; i++ ) {
			dir[i] = a[i] + ( a[i + 4] - a[i] ) * t;
		}
	}
	/* left unnormalized: only its direction is used */
}

void MDSC_ChildMatrix( const mdscMatrix_t p, const float q[4], mdscMatrix_t m ) {
	mdscMatrix_t l;
	int i, j;

	if ( !p ) {
		QuatToMatrix( q, m );
		return;
	}
	QuatToMatrix( q, l );
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			m[i][j] = p[i][0] * l[0][j] + p[i][1] * l[1][j] + p[i][2] * l[2][j];
		}
	}
}

/* set bits in a word, a byte at a time (the SH4 has no instruction for it) */
static const unsigned char bitCount[256] = {
#define B2( n ) n, n + 1, n + 1, n + 2
#define B4( n ) B2( n ), B2( n + 1 ), B2( n + 1 ), B2( n + 2 )
#define B6( n ) B4( n ), B4( n + 1 ), B4( n + 1 ), B4( n + 2 )
	B6( 0 ), B6( 1 ), B6( 1 ), B6( 2 )
#undef B2
#undef B4
#undef B6
};

static int CountBits( unsigned int w ) {
	return bitCount[w & 255] + bitCount[( w >> 8 ) & 255] + bitCount[( w >> 16 ) & 255] + bitCount[w >> 24];
}

static int HighBit( unsigned int w ) {   /* w not 0 */
	int b = 31;
	while ( !( w & 0x80000000u ) ) {
		w <<= 1;
		b--;
	}
	return b;
}

static int LowBit( unsigned int w ) {    /* w not 0 */
	int b = 0;
	while ( !( w & 1 ) ) {
		w >>= 1;
		b++;
	}
	return b;
}

/* a track's key at or before frame: its number (the return), its frame, and
   the next key's frame, -1 if it's the last */
static int TrackKey( const unsigned char *animBase, const mdscTrack_t *track, int frame, int *keyFrame, int *nextFrame ) {
	const unsigned short *keys = (const unsigned short *)( animBase + track->ofsKeys );
	int numKeys = MDSC_NUMKEYS( track );
	int numBlocks, numWords, w, k;
	const unsigned int *bits;
	unsigned int mask, m;

	if ( !( track->numKeys & MDSC_KEYS_BITMAP ) ) {
		k = FindKey( keys, numKeys, frame );
		*keyFrame = keys[k];
		*nextFrame = k + 1 < numKeys ? keys[k + 1] : -1;
		return k;
	}

	numBlocks = keys[0];
	numWords = numBlocks * 2;
	bits = (const unsigned int *)( keys + ( ( 1 + numBlocks + 1 ) & ~1 ) );
	w = frame >> 5;
	if ( w >= numWords ) {
		/* past its last key */
		w = numWords - 1;
		mask = 0xffffffffu;
	} else {
		mask = ( 2u << ( frame & 31 ) ) - 1;   /* the bits up to frame */
	}
	k = keys[1 + ( w >> 1 )] + ( w & 1 ? CountBits( bits[w - 1] ) : 0 ) + CountBits( bits[w] & mask ) - 1;

	/* this key's frame: frame 0 is one, so there's always one */
	for ( m = bits[w] & mask; !m; m = bits[--w] ) {
	}
	*keyFrame = w * 32 + HighBit( m );

	/* the next */
	w = frame >> 5;
	m = w < numWords ? bits[w] & ~mask : 0;
	while ( !m && ++w < numWords ) {
		m = bits[w];
	}
	*nextFrame = m ? w * 32 + LowBit( m ) : -1;
	return k;
}

/* n floats a key */
static void TrackFloats( const unsigned char *animBase, const mdscTrack_t *track, int n, int frame, float *out ) {
	int kf, nf, k = TrackKey( animBase, track, frame, &kf, &nf );
	const float *v = (const float *)( animBase + track->ofsValues ) + k * n;
	int c;

	if ( kf == frame || nf < 0 ) {
		memcpy( out, v, n * 4 );
	} else {
		float t = (float)( frame - kf ) / (float)( nf - kf );
		for ( c = 0; c < n; c++ ) {
			out[c] = v[c] + ( v[c + n] - v[c] ) * t;
		}
	}
}

/* a rotation track's value at frame */
static void TrackQuatAt( const unsigned char *animBase, const mdscTrack_t *track, int frame, float q[4] ) {
	int kf, nf, k = TrackKey( animBase, track, frame, &kf, &nf );
	const unsigned int *values = (const unsigned int *)( animBase + track->ofsValues );

	if ( kf == frame || nf < 0 ) {
		UnpackQuat( values[k], q );
	} else {
		float a[4], b[4];

		UnpackQuat( values[k], a );
		UnpackQuat( values[k + 1], b );
		LerpQuat( a, b, (float)( frame - kf ) / (float)( nf - kf ), q );
	}
}

/* a direction track's */
static void TrackDirAt( const unsigned char *animBase, const mdscTrack_t *track, int frame, float dir[3] ) {
	int kf, nf, k = TrackKey( animBase, track, frame, &kf, &nf ), i;
	const short *a = (const short *)( animBase + track->ofsValues ) + k * 4;

	if ( kf == frame || nf < 0 ) {
		for ( i = 0; i < 3; i++ ) {
			dir[i] = a[i];
		}
	} else {
		float t = (float)( frame - kf ) / (float)( nf - kf );
		for ( i = 0; i < 3; i++ ) {
			dir[i] = a[i] + ( a[i + 4] - a[i] ) * t;
		}
	}
}

/* the parent offsets and bones of frame `frame` of the mdscAnim_t at
   animBase, of the MDSC at base (for its bones), into the decoded frame out */
static void DecodePose( const unsigned char *base, const unsigned char *animBase, int frame, void *out ) {
	int numBones = ReadInt( base + HDR_NUMBONES );
	const mdscAnim_t *anim = (const mdscAnim_t *)animBase;
	const unsigned char *boneInfo = base + ReadInt( base + HDR_OFSBONES );
	float *floats = (float *)out;
	short *poses = (short *)( floats + MDSC_FRAME_FLOATS );
	static mdscMatrix_t model[MDSC_MAX_BONES];
	int i, j;

	TrackFloats( animBase, &anim->frameTrack, MDSC_OFFSET_FLOATS, frame, floats + MDSC_CULL_FLOATS );

	/* bones, parents first */
	for ( i = 0; i < numBones; i++ ) {
		const mdscTrack_t *rot = &anim->tracks[i];
		int parent = ReadInt( boneInfo + i * BONEINFO_SIZE + BONEINFO_PARENT );
		short *pose = poses + i * MDSC_POSE_SHORTS;
		float q[4];

		TrackQuatAt( animBase, rot, frame, q );
		if ( parent < 0 ) {
			MDSC_ChildMatrix( NULL, q, model[i] );
			pose[4] = pose[5] = 0;
		} else {
			const mdscTrack_t *dir = &anim->tracks[numBones + i];
			float d[3], md[3];

			MDSC_ChildMatrix( (const float (*)[3])model[parent], q, model[i] );
			TrackDirAt( animBase, dir, frame, d );
			for ( j = 0; j < 3; j++ ) {
				md[j] = model[parent][j][0] * d[0] + model[parent][j][1] * d[1] + model[parent][j][2] * d[2];
			}
			DirToAngles( md, &pose[4], &pose[5] );
		}
		MatrixToAngles( (const float (*)[3])model[i], pose );
		pose[3] = 0;
	}
}

static int ClampFrame( const unsigned char *base, int frame ) {
	int numFrames = ReadInt( base + HDR_NUMFRAMES );

	if ( frame < 0 ) {
		return 0;
	}
	return frame >= numFrames ? numFrames - 1 : frame;
}

void MDSC_DecodeFrame( const void *mds, int frame, void *out ) {
	const unsigned char *base = (const unsigned char *)mds;
	const unsigned char *animBase = base + ReadInt( base + HDR_OFSFRAMES );

	frame = ClampFrame( base, frame );
	TrackFloats( animBase, &( (const mdscAnim_t *)animBase )->cullTrack, MDSC_CULL_FLOATS, frame, (float *)out );
	DecodePose( base, animBase, frame, out );
}

void MDSC_DecodeSharedFrame( const void *mds, const void *base, int frame, void *out ) {
	const unsigned char *own = (const unsigned char *)mds;
	const unsigned char *shareBase = own + ReadInt( own + HDR_OFSFRAMES );
	const mdscShare_t *share = (const mdscShare_t *)shareBase;
	const mdscSegment_t *seg = (const mdscSegment_t *)( shareBase + share->ofsSegments );
	const unsigned char *animBase = shareBase + share->ofsAnim;
	int lo = 0, hi = share->numSegments - 1;

	frame = ClampFrame( own, frame );

	/* the cull bounds are its own, the rest the base's or its own */
	TrackFloats( animBase, &( (const mdscAnim_t *)animBase )->cullTrack, MDSC_CULL_FLOATS, frame, (float *)out );
	while ( lo < hi ) {
		int mid = ( lo + hi + 1 ) / 2;
		if ( seg[mid].first <= frame ) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	seg += lo;
	if ( seg->fromBase ) {
		const unsigned char *b = (const unsigned char *)base;
		DecodePose( b, b + ReadInt( b + HDR_OFSFRAMES ), ClampFrame( b, seg->srcFirst + frame - seg->first ), out );
	} else {
		DecodePose( own, animBase, seg->srcFirst + frame - seg->first, out );
	}
}

/* ---- the mesh ---- */

/* an mdsVertex_t's: normal[3], texCoords[2], numWeights, fixedParent,
   fixedDist; then an mdsWeight_t's: boneIndex, boneWeight, offset[3] */
#define MDS_VERTEX_SIZE 32
#define MDS_WEIGHT_SIZE 20

static int PackShort( float f, float scale, short *out ) {
	long v = lrintf( f * scale );

	if ( v < -32768 || v > 32767 ) {
		return 0;
	}
	*out = (short)v;
	return 1;
}

int MDSC_PackVertexes( const void *in, int numVerts, void *out, int *inSize ) {
	const unsigned char *src = (const unsigned char *)in;
	unsigned char *dst = (unsigned char *)out;
	int i, k, j;

	for ( i = 0; i < numVerts; i++ ) {
		float normal[3], texCoords[2];
		int numWeights;
		mdscVertex_t v;

		memcpy( normal, src, 12 );
		memcpy( texCoords, src + 12, 8 );
		memcpy( &numWeights, src + 20, 4 );
		if ( numWeights < 1 || numWeights > 255 ) {
			return -1;
		}
		/* the weights first, each read before it's written over (a vertex's
		   start comes out no later than its start in, nor its weights) */
		for ( k = 0; k < numWeights; k++ ) {
			const unsigned char *sw = src + MDS_VERTEX_SIZE + k * MDS_WEIGHT_SIZE;
			int boneIndex;
			float boneWeight, offset[3];
			long weight;
			mdscWeight_t w;

			memcpy( &boneIndex, sw, 4 );
			memcpy( &boneWeight, sw + 4, 4 );
			memcpy( offset, sw + 8, 12 );
			weight = lrintf( boneWeight * MDSC_WEIGHT_SCALE );
			if ( boneIndex < 0 || boneIndex > 255 || weight < 0 || weight > 65535 ) {
				return -1;
			}
			for ( j = 0; j < 3; j++ ) {
				if ( !PackShort( offset[j], MDSC_OFS_SCALE, &w.offset[j] ) ) {
					return -1;
				}
			}
			w.boneWeight = (unsigned short)weight;
			w.boneIndex = (unsigned char)boneIndex;
			w.pad = 0;
			memcpy( dst + sizeof( v ) + k * sizeof( w ), &w, sizeof( w ) );
		}
		for ( j = 0; j < 2; j++ ) {
			if ( !PackShort( texCoords[j], MDSC_TC_SCALE, &v.texCoords[j] ) ) {
				return -1;
			}
		}
		for ( j = 0; j < 3; j++ ) {
			long n = lrintf( normal[j] * MDSC_NORMAL_SCALE );
			v.normal[j] = (signed char)( n < -127 ? -127 : n > 127 ? 127 : n );
		}
		v.numWeights = (unsigned char)numWeights;
		memcpy( dst, &v, sizeof( v ) );

		src += MDS_VERTEX_SIZE + numWeights * MDS_WEIGHT_SIZE;
		dst += sizeof( v ) + numWeights * sizeof( mdscWeight_t );
	}
	*inSize = (int)( src - (const unsigned char *)in );
	return (int)( dst - (unsigned char *)out );
}
