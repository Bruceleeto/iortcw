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

/* where they are in an mdsHeader_t / mdsBoneInfo_t */
#define HDR_NUMFRAMES   80
#define HDR_NUMBONES    84
#define HDR_OFSFRAMES   88
#define HDR_OFSBONES    92
#define BONEINFO_SIZE   80
#define BONEINFO_PARENT 64

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

void MDSC_DecodeFrame( const void *mds, int frame, void *out ) {
	const unsigned char *base = (const unsigned char *)mds;
	int numFrames = ReadInt( base + HDR_NUMFRAMES );
	int numBones = ReadInt( base + HDR_NUMBONES );
	const unsigned char *animBase = base + ReadInt( base + HDR_OFSFRAMES );
	const mdscAnim_t *anim = (const mdscAnim_t *)animBase;
	const unsigned char *boneInfo = base + ReadInt( base + HDR_OFSBONES );
	float *floats = (float *)out;
	short *poses = (short *)( floats + MDSC_FRAME_FLOATS );
	static mdscMatrix_t model[MDSC_MAX_BONES];
	const unsigned short *keys;
	int i, j, k, c;

	if ( frame < 0 ) {
		frame = 0;
	} else if ( frame >= numFrames ) {
		frame = numFrames - 1;
	}

	/* bounds and offsets */
	keys = (const unsigned short *)( animBase + anim->frameTrack.ofsKeys );
	k = FindKey( keys, anim->frameTrack.numKeys, frame );
	{
		const float *v = (const float *)( animBase + anim->frameTrack.ofsValues ) + k * MDSC_FRAME_FLOATS;

		if ( keys[k] == frame || k + 1 >= anim->frameTrack.numKeys ) {
			memcpy( floats, v, MDSC_FRAME_FLOATS * 4 );
		} else {
			float t = (float)( frame - keys[k] ) / (float)( keys[k + 1] - keys[k] );
			for ( c = 0; c < MDSC_FRAME_FLOATS; c++ ) {
				floats[c] = v[c] + ( v[c + MDSC_FRAME_FLOATS] - v[c] ) * t;
			}
		}
	}

	/* bones, parents first */
	for ( i = 0; i < numBones; i++ ) {
		const mdscTrack_t *rot = &anim->tracks[i];
		int parent = ReadInt( boneInfo + i * BONEINFO_SIZE + BONEINFO_PARENT );
		short *pose = poses + i * MDSC_POSE_SHORTS;
		float q[4];

		MDSC_TrackQuat( (const unsigned short *)( animBase + rot->ofsKeys ),
						(const unsigned int *)( animBase + rot->ofsValues ), rot->numKeys, frame, q );
		if ( parent < 0 ) {
			MDSC_ChildMatrix( NULL, q, model[i] );
			pose[4] = pose[5] = 0;
		} else {
			const mdscTrack_t *dir = &anim->tracks[numBones + i];
			float d[3], md[3];

			MDSC_ChildMatrix( (const float (*)[3])model[parent], q, model[i] );
			MDSC_TrackDir( (const unsigned short *)( animBase + dir->ofsKeys ),
						   (const short *)( animBase + dir->ofsValues ), dir->numKeys, frame, d );
			for ( j = 0; j < 3; j++ ) {
				md[j] = model[parent][j][0] * d[0] + model[parent][j][1] * d[1] + model[parent][j][2] * d[2];
			}
			DirToAngles( md, &pose[4], &pose[5] );
		}
		MatrixToAngles( (const float (*)[3])model[i], pose );
		pose[3] = 0;
	}
}
