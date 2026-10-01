/*
 * .mdc -> .mdb: the animated surfaces of a vertex animated model as rigid
 * bones (see SP/code/qcommon/qfiles.h), DMS style: a bone turn and move a
 * frame in place of every vertex a frame.
 *
 * Each animated surface's vertexes are grouped into bones that move rigidly
 * between its frames: a bone is fitted to its vertexes frame by frame (the
 * best turn, Horn's quaternion method), each vertex then goes to the bone
 * that keeps it nearest its place, and so on until it settles. Bones are
 * added, seeded at the vertex furthest off, until none is further from
 * where the .mdc has it in any frame than its tolerance: a quarter of how
 * far it moves, from MDB_MIN_ERROR to MDB_MAX_ERROR, so small motions (a
 * face's) are kept as well as big ones.
 *
 * Then only the frames that can't be lerped from the ones either side,
 * within the same tolerance, are kept, of the bones or (for what bones
 * can't do, cloth) of the .mdc's own vertexes, whichever is smaller. A
 * model that comes out no smaller gets no .mdb, and the .mdc is used.
 */
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rtcwconv.h"

#define MD3_XYZ_SCALE       ( 1.0 / 64 )
#define MDC_MAX_OFS         127.0
#define MDC_DIST_SCALE      0.05
#define MDB_BONES           -1
#define MDB_KEYS            -2

/* furthest a vertex may be from its place in the .mdc, in units: a
 * quarter of how far it moves, from MDB_MIN_ERROR (the .mdc's own step) up
 * to MDB_MAX_ERROR */
#define MDB_MAX_ERROR       0.5
#define MDB_MIN_ERROR       0.05
#define MDB_MOTION_PART     0.25
/* the .mdb has to be this much of the .mdc or less */
#define MDB_MAX_RATIO       0.8
#define MDB_MAX_BONES       255

static const float anormals[256][3] = {
#include "../../SP/code/renderer/anorms256.h"
};

namespace {

struct V3 {
	double x, y, z;
};
static V3 operator+( V3 a, V3 b ) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-( V3 a, V3 b ) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*( V3 a, double s ) { return { a.x * s, a.y * s, a.z * s }; }
static double Dot( V3 a, V3 b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double Len( V3 a ) { return sqrt( Dot( a, a ) ); }

/* a turn as a quaternion, and a move */
struct Pose {
	double q[4];    /* x y z w */
	V3 t;
};

static V3 Rotate( const double q[4], V3 v ) {
	/* v + 2w (u x v) + 2 u x (u x v), u = q.xyz */
	V3 u = { q[0], q[1], q[2] };
	V3 c = { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
	V3 cc = { u.y * c.z - u.z * c.y, u.z * c.x - u.x * c.z, u.x * c.y - u.y * c.x };
	return v + c * ( 2 * q[3] ) + cc * 2;
}

static V3 Apply( const Pose &p, V3 v ) {
	return Rotate( p.q, v ) + p.t;
}

/* the biggest eigenvector of a symmetric 4x4 (Jacobi) */
static void TopEigen( double a[4][4], double out[4] ) {
	double v[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
	for ( int sweep = 0; sweep < 50; sweep++ ) {
		double off = 0;
		for ( int i = 0; i < 4; i++ ) {
			for ( int j = i + 1; j < 4; j++ ) {
				off += a[i][j] * a[i][j];
			}
		}
		if ( off < 1e-22 ) {
			break;
		}
		for ( int p = 0; p < 4; p++ ) {
			for ( int q = p + 1; q < 4; q++ ) {
				if ( fabs( a[p][q] ) < 1e-30 ) {
					continue;
				}
				double theta = ( a[q][q] - a[p][p] ) / ( 2 * a[p][q] );
				double t = ( theta >= 0 ? 1 : -1 ) / ( fabs( theta ) + sqrt( theta * theta + 1 ) );
				double c = 1 / sqrt( t * t + 1 ), s = t * c;
				for ( int k = 0; k < 4; k++ ) {
					double akp = a[k][p], akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for ( int k = 0; k < 4; k++ ) {
					double apk = a[p][k], aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for ( int k = 0; k < 4; k++ ) {
					double vkp = v[k][p], vkq = v[k][q];
					v[k][p] = c * vkp - s * vkq;
					v[k][q] = s * vkp + c * vkq;
				}
			}
		}
	}
	int best = 0;
	for ( int i = 1; i < 4; i++ ) {
		if ( a[i][i] > a[best][best] ) {
			best = i;
		}
	}
	for ( int i = 0; i < 4; i++ ) {
		out[i] = v[i][best];
	}
}

/* the turn and move that best take the points from to the points to */
static Pose Fit( const std::vector<V3> &from, const std::vector<V3> &to, const std::vector<int> &which ) {
	Pose p;
	V3 cf = { 0, 0, 0 }, ct = { 0, 0, 0 };
	for ( int i : which ) {
		cf = cf + from[i];
		ct = ct + to[i];
	}
	cf = cf * ( 1.0 / which.size() );
	ct = ct * ( 1.0 / which.size() );
	double s[3][3] = {};
	for ( int i : which ) {
		V3 a = from[i] - cf, b = to[i] - ct;
		double av[3] = { a.x, a.y, a.z }, bv[3] = { b.x, b.y, b.z };
		for ( int r = 0; r < 3; r++ ) {
			for ( int c = 0; c < 3; c++ ) {
				s[r][c] += av[r] * bv[c];
			}
		}
	}
	/* Horn: w x y z */
	double n[4][4] = {
		{ s[0][0] + s[1][1] + s[2][2], s[1][2] - s[2][1], s[2][0] - s[0][2], s[0][1] - s[1][0] },
		{ s[1][2] - s[2][1], s[0][0] - s[1][1] - s[2][2], s[0][1] + s[1][0], s[2][0] + s[0][2] },
		{ s[2][0] - s[0][2], s[0][1] + s[1][0], -s[0][0] + s[1][1] - s[2][2], s[1][2] + s[2][1] },
		{ s[0][1] - s[1][0], s[2][0] + s[0][2], s[1][2] + s[2][1], -s[0][0] - s[1][1] + s[2][2] },
	};
	double e[4];
	TopEigen( n, e );
	double l = sqrt( e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3] );
	if ( l < 1e-12 ) {
		e[0] = 1;
		e[1] = e[2] = e[3] = 0;
		l = 1;
	}
	p.q[0] = e[1] / l;
	p.q[1] = e[2] / l;
	p.q[2] = e[3] / l;
	p.q[3] = e[0] / l;
	p.t = ct - Rotate( p.q, cf );
	return p;
}

/* as the renderer will have it: the quaternion as shorts, normalized */
static void Quantize( Pose &p ) {
	double l = 0;
	short s[4];
	for ( int i = 0; i < 4; i++ ) {
		s[i] = (short)lrint( p.q[i] * 32767 );
		l += (double)s[i] * s[i];
	}
	l = l > 0 ? sqrt( l ) : 1;
	for ( int i = 0; i < 4; i++ ) {
		p.q[i] = s[i] / l;
	}
	p.t = { (float)p.t.x, (float)p.t.y, (float)p.t.z };
}

template <typename T> static T Get( const std::vector<uint8_t> &d, size_t o ) {
	T v;
	memcpy( &v, d.data() + o, sizeof( v ) );
	return v;
}

template <typename T> static void Put( std::vector<uint8_t> &d, const T &v ) {
	const uint8_t *p = (const uint8_t *)&v;
	d.insert( d.end(), p, p + sizeof( v ) );
}

template <typename T> static void Set( std::vector<uint8_t> &d, size_t o, const T &v ) {
	memcpy( d.data() + o, &v, sizeof( v ) );
}

/* mdcSurface_t's fields, by offset */
enum {
	S_IDENT = 0, S_NAME = 4, S_FLAGS = 68, S_NUMCOMP = 72, S_NUMBASE = 76, S_NUMSHADERS = 80,
	S_NUMVERTS = 84, S_NUMTRIS = 88, S_OFSTRIS = 92, S_OFSSHADERS = 96, S_OFSST = 100,
	S_OFSXYZ = 104, S_OFSCOMP = 108, S_OFSBASEFRAMES = 112, S_OFSCOMPFRAMES = 116, S_OFSEND = 120,
	S_SIZE = 124
};
/* mdcHeader_t's */
enum {
	H_NUMFRAMES = 76, H_NUMSURFACES = 84, H_OFSFRAMES = 92, H_OFSTAGNAMES = 96, H_OFSTAGS = 100,
	H_OFSSURFACES = 104, H_OFSEND = 108, H_SIZE = 112
};

struct Surface {
	int numVerts, numTris;
	std::vector<std::vector<V3>> pos;       /* [frame][vertex] */
	std::vector<V3> normal;                 /* at rest */
	std::vector<double> tol;                /* each vertex's */
	std::vector<int> base, comp;            /* the .mdc's stored frames, each frame's */
};

/* how far each vertex may be off: see MDB_MOTION_PART */
static void Tolerances( Surface &s ) {
	s.tol.resize( s.numVerts );
	for ( int v = 0; v < s.numVerts; v++ ) {
		double motion = 0;
		for ( size_t f = 1; f < s.pos.size(); f++ ) {
			motion = std::max( motion, Len( s.pos[f][v] - s.pos[0][v] ) );
		}
		s.tol[v] = std::max( MDB_MIN_ERROR, std::min( MDB_MAX_ERROR, motion * MDB_MOTION_PART ) );
	}
}

/* the bones for one surface; false if they'd need more than maxBones */
struct Bones {
	std::vector<int> bone;                  /* each vertex's */
	std::vector<std::vector<Pose>> poses;   /* [bone][frame] */
	double maxErr;
	double sumErr;
};

static double VertErr( const Surface &s, const std::vector<Pose> &poses, int v ) {
	double e = 0;
	for ( size_t f = 0; f < s.pos.size(); f++ ) {
		e = std::max( e, Len( Apply( poses[f], s.pos[0][v] ) - s.pos[f][v] ) );
	}
	return e;
}

static void FitBones( const Surface &s, Bones &b, int numBones ) {
	int numFrames = s.pos.size();
	b.poses.assign( numBones, std::vector<Pose>( numFrames ) );
	for ( int k = 0; k < numBones; k++ ) {
		std::vector<int> which;
		for ( int v = 0; v < s.numVerts; v++ ) {
			if ( b.bone[v] == k ) {
				which.push_back( v );
			}
		}
		for ( int f = 0; f < numFrames; f++ ) {
			if ( which.empty() ) {
				b.poses[k][f] = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
			} else {
				b.poses[k][f] = Fit( s.pos[0], s.pos[f], which );
			}
			Quantize( b.poses[k][f] );
		}
	}
}

static bool MakeBones( const Surface &s, Bones &b, int maxBones ) {
	int numBones = 1;
	b.bone.assign( s.numVerts, 0 );
	FitBones( s, b, numBones );
	for ( ;; ) {
		/* settle: each vertex to its best bone, the bones fitted again */
		std::vector<double> err( s.numVerts );
		for ( int iter = 0; iter < 8; iter++ ) {
			bool changed = false;
			for ( int v = 0; v < s.numVerts; v++ ) {
				int best = b.bone[v];
				double bestErr = VertErr( s, b.poses[best], v );
				for ( int k = 0; k < numBones && bestErr > 0; k++ ) {
					if ( k != best ) {
						double e = VertErr( s, b.poses[k], v );
						if ( e < bestErr - 1e-9 ) {
							best = k;
							bestErr = e;
						}
					}
				}
				changed |= best != b.bone[v];
				b.bone[v] = best;
				err[v] = bestErr;
			}
			if ( !changed ) {
				break;
			}
			FitBones( s, b, numBones );
		}
		int worst = 0;
		b.maxErr = b.sumErr = 0;
		for ( int v = 0; v < s.numVerts; v++ ) {
			err[v] = VertErr( s, b.poses[b.bone[v]], v );
			b.sumErr += err[v];
			b.maxErr = std::max( b.maxErr, err[v] );
			if ( err[v] / s.tol[v] > err[worst] / s.tol[worst] ) {
				worst = v;
			}
		}
		if ( err[worst] <= s.tol[worst] ) {
			return true;
		}
		if ( numBones == maxBones ) {
			return false;
		}
		/* a new bone, at the vertex furthest off, with the ones off that
		 * move as it does */
		int k = numBones++;
		for ( int v = 0; v < s.numVerts; v++ ) {
			if ( v == worst || err[v] <= s.tol[v] ) {
				continue;
			}
			double d = 0, tol = std::min( s.tol[v], s.tol[worst] );
			for ( size_t f = 0; f < s.pos.size() && d <= tol; f++ ) {
				d = std::max( d, Len( ( s.pos[f][v] - s.pos[0][v] ) - ( s.pos[f][worst] - s.pos[0][worst] ) ) );
			}
			if ( d <= tol ) {
				b.bone[v] = k;
			}
		}
		b.bone[worst] = k;
		FitBones( s, b, numBones );
	}
}

static short LatLong( V3 n ) {
	double l = Len( n );
	if ( l <= 0 ) {
		return 0;
	}
	n = n * ( 1 / l );
	int lat = (int)lrint( atan2( n.y, n.x ) * ( 256 / ( 2 * M_PI ) ) ) & 255;
	int lng = (int)lrint( acos( std::max( -1.0, std::min( 1.0, n.z ) ) ) * ( 256 / ( 2 * M_PI ) ) ) & 255;
	return (short)( ( lat << 8 ) | lng );
}

/* a pose between two, as the renderer has it */
static Pose LerpPose( const Pose &a, const Pose &b, double t ) {
	Pose p;
	double d = 0, l = 0;
	for ( int i = 0; i < 4; i++ ) {
		d += a.q[i] * b.q[i];
	}
	double s = d < 0 ? -t : t;
	for ( int i = 0; i < 4; i++ ) {
		p.q[i] = a.q[i] * ( 1 - t ) + b.q[i] * s;
		l += p.q[i] * p.q[i];
	}
	l = l > 0 ? sqrt( l ) : 1;
	for ( int i = 0; i < 4; i++ ) {
		p.q[i] /= l;
	}
	p.t = a.t * ( 1 - t ) + b.t * t;
	return p;
}

/* how far frame m is from kept frame a to kept frame c, as stored */
static double KeyLerp( int a, int c, int m ) {
	return lrint( (double)( m - a ) / ( c - a ) * 32767 ) / 32767.0;
}

/* the frames to keep: from each kept one, as far on as every frame between
 * can be lerped from it and the next kept one (ok says if frame m can) */
template <typename Ok> static std::vector<int> PickKeys( int numFrames, Ok ok ) {
	std::vector<int> keys = { 0 };
	int a = 0;
	while ( a < numFrames - 1 ) {
		int b = a + 1;
		while ( b + 1 < numFrames ) {
			int c = b + 1;
			bool good = true;
			for ( int m = a + 1; m < c && good; m++ ) {
				good = ok( a, c, m );
			}
			if ( !good ) {
				break;
			}
			b = c;
		}
		keys.push_back( b );
		a = b;
	}
	return keys;
}

/* each frame's kept frame and how far on to the next (mdbFrame_t) */
static void PutFrameTable( std::vector<uint8_t> &out, const std::vector<int> &keys, int numFrames ) {
	size_t k = 0;
	for ( int f = 0; f < numFrames; f++ ) {
		while ( k + 1 < keys.size() && keys[k + 1] <= f ) {
			k++;
		}
		short lerp = k + 1 < keys.size() ? (short)lrint( KeyLerp( keys[k], keys[k + 1], f ) * 32767 ) : 0;
		Put<short>( out, (short)k );
		Put<short>( out, lerp );
	}
}

/* frame f as the kept frames give it back */
static void KeyOf( const std::vector<int> &keys, int f, int &a, int &c, double &t ) {
	size_t k = std::upper_bound( keys.begin(), keys.end(), f ) - keys.begin() - 1;
	a = keys[k];
	c = k + 1 < keys.size() ? keys[k + 1] : a;
	t = c > a ? KeyLerp( a, c, f ) : 0;
}

struct Encoded {
	std::vector<uint8_t> data;
	int numBones;               /* 0: vertexes */
	int numKeys;
	double maxErr, sumErr;
};

/* the surface header, shaders, triangles (through remap) and st (in order) */
static size_t PutCommon( std::vector<uint8_t> &out, const std::vector<uint8_t> &in, size_t surf, const Surface &s,
						 const std::vector<int> &order, const std::vector<int> &remap ) {
	out.insert( out.end(), in.begin() + surf, in.begin() + surf + S_SIZE );

	int numShaders = Get<int>( in, surf + S_NUMSHADERS );
	Set<int>( out, S_OFSSHADERS, (int)out.size() );
	size_t sh = surf + Get<int>( in, surf + S_OFSSHADERS );
	out.insert( out.end(), in.begin() + sh, in.begin() + sh + numShaders * 68 );

	Set<int>( out, S_OFSTRIS, (int)out.size() );
	size_t tri = surf + Get<int>( in, surf + S_OFSTRIS );
	for ( int t = 0; t < s.numTris * 3; t++ ) {
		Put<int>( out, remap[Get<int>( in, tri + t * 4 )] );
	}

	Set<int>( out, S_OFSST, (int)out.size() );
	size_t stOfs = surf + Get<int>( in, surf + S_OFSST );
	for ( int v : order ) {
		out.insert( out.end(), in.begin() + stOfs + v * 8, in.begin() + stOfs + v * 8 + 8 );
	}
	return out.size();
}

/* rigid bones, at the frames that can't be lerped */
static Encoded EncodeBones( const std::vector<uint8_t> &in, size_t surf, const Surface &s, const Bones &b ) {
	Encoded e;
	int numFrames = s.pos.size(), numBones = b.poses.size();

	/* a vertex's place in frame m, lerped from a and c */
	std::vector<Pose> lerped( numBones );
	auto place = [&]( int a, int c, int m ) {
		double t = c > a ? KeyLerp( a, c, m ) : 0;
		for ( int k = 0; k < numBones; k++ ) {
			lerped[k] = LerpPose( b.poses[k][a], b.poses[k][c], t );
		}
	};
	std::vector<int> keys = PickKeys( numFrames, [&]( int a, int c, int m ) {
		place( a, c, m );
		for ( int v = 0; v < s.numVerts; v++ ) {
			if ( Len( Apply( lerped[b.bone[v]], s.pos[0][v] ) - s.pos[m][v] ) > s.tol[v] ) {
				return false;
			}
		}
		return true;
	} );
	e.maxErr = e.sumErr = 0;
	for ( int v = 0; v < s.numVerts; v++ ) {
		double err = 0;
		for ( int f = 0; f < numFrames; f++ ) {
			int a, c;
			double t;
			KeyOf( keys, f, a, c, t );
			Pose p = LerpPose( b.poses[b.bone[v]][a], b.poses[b.bone[v]][c], t );
			err = std::max( err, Len( Apply( p, s.pos[0][v] ) - s.pos[f][v] ) );
		}
		e.maxErr = std::max( e.maxErr, err );
		e.sumErr += err;
	}

	/* the vertexes bone by bone, empty bones gone */
	std::vector<int> order, count, used;
	for ( int k = 0; k < numBones; k++ ) {
		int c = 0;
		for ( int v = 0; v < s.numVerts; v++ ) {
			if ( b.bone[v] == k ) {
				order.push_back( v );
				c++;
			}
		}
		if ( c ) {
			used.push_back( k );
			count.push_back( c );
		}
	}
	std::vector<int> remap( s.numVerts );
	for ( int i = 0; i < s.numVerts; i++ ) {
		remap[order[i]] = i;
	}

	std::vector<uint8_t> &out = e.data;
	PutCommon( out, in, surf, s, order, remap );
	Set<int>( out, S_NUMCOMP, MDB_BONES );
	Set<int>( out, S_NUMBASE, (int)used.size() );

	Set<int>( out, S_OFSXYZ, (int)out.size() );
	for ( int v : order ) {
		Put<short>( out, (short)lrint( s.pos[0][v].x / MD3_XYZ_SCALE ) );
		Put<short>( out, (short)lrint( s.pos[0][v].y / MD3_XYZ_SCALE ) );
		Put<short>( out, (short)lrint( s.pos[0][v].z / MD3_XYZ_SCALE ) );
		Put<short>( out, LatLong( s.normal[v] ) );
	}

	Set<int>( out, S_OFSCOMP, (int)out.size() );
	for ( int c : count ) {
		Put<short>( out, (short)c );
	}
	if ( count.size() & 1 ) {
		Put<short>( out, 0 );
	}

	Set<int>( out, S_OFSBASEFRAMES, (int)out.size() );
	for ( int f : keys ) {
		for ( int k : used ) {
			const Pose &p = b.poses[k][f];
			for ( int j = 0; j < 4; j++ ) {
				Put<short>( out, (short)lrint( p.q[j] * 32767 ) );
			}
			Put<float>( out, (float)p.t.x );
			Put<float>( out, (float)p.t.y );
			Put<float>( out, (float)p.t.z );
		}
	}
	Set<int>( out, S_OFSCOMPFRAMES, (int)out.size() );
	PutFrameTable( out, keys, numFrames );
	Set<int>( out, S_OFSEND, (int)out.size() );

	e.numBones = used.size();
	e.numKeys = keys.size();
	return e;
}

/* the .mdc's own vertexes, at the frames that can't be lerped */
static Encoded EncodeVertexes( const std::vector<uint8_t> &in, size_t surf, const Surface &s ) {
	Encoded e;
	int numFrames = s.pos.size();
	std::vector<int> keys = PickKeys( numFrames, [&]( int a, int c, int m ) {
		double t = KeyLerp( a, c, m );
		for ( int v = 0; v < s.numVerts; v++ ) {
			if ( Len( s.pos[a][v] * ( 1 - t ) + s.pos[c][v] * t - s.pos[m][v] ) > s.tol[v] ) {
				return false;
			}
		}
		return true;
	} );
	e.maxErr = e.sumErr = 0;
	for ( int v = 0; v < s.numVerts; v++ ) {
		double err = 0;
		for ( int f = 0; f < numFrames; f++ ) {
			int a, c;
			double t;
			KeyOf( keys, f, a, c, t );
			err = std::max( err, Len( s.pos[a][v] * ( 1 - t ) + s.pos[c][v] * t - s.pos[f][v] ) );
		}
		e.maxErr = std::max( e.maxErr, err );
		e.sumErr += err;
	}

	/* the stored frames the kept ones use */
	std::vector<int> bases, comps;
	for ( int f : keys ) {
		if ( std::find( bases.begin(), bases.end(), s.base[f] ) == bases.end() ) {
			bases.push_back( s.base[f] );
		}
		if ( s.comp[f] >= 0 && std::find( comps.begin(), comps.end(), s.comp[f] ) == comps.end() ) {
			comps.push_back( s.comp[f] );
		}
	}

	std::vector<int> order( s.numVerts );
	for ( int v = 0; v < s.numVerts; v++ ) {
		order[v] = v;
	}
	std::vector<uint8_t> &out = e.data;
	PutCommon( out, in, surf, s, order, order );
	Set<int>( out, S_NUMCOMP, MDB_KEYS );
	Set<int>( out, S_NUMBASE, (int)bases.size() );

	Set<int>( out, S_OFSXYZ, (int)out.size() );
	size_t xyz = surf + Get<int>( in, surf + S_OFSXYZ );
	for ( int i : bases ) {
		out.insert( out.end(), in.begin() + xyz + (size_t)i * s.numVerts * 8, in.begin() + xyz + (size_t)( i + 1 ) * s.numVerts * 8 );
	}
	Set<int>( out, S_OFSCOMP, (int)out.size() );
	size_t comp = surf + Get<int>( in, surf + S_OFSCOMP );
	for ( int i : comps ) {
		out.insert( out.end(), in.begin() + comp + (size_t)i * s.numVerts * 4, in.begin() + comp + (size_t)( i + 1 ) * s.numVerts * 4 );
	}

	Set<int>( out, S_OFSBASEFRAMES, (int)out.size() );
	for ( int f : keys ) {
		Put<short>( out, (short)( std::find( bases.begin(), bases.end(), s.base[f] ) - bases.begin() ) );
		Put<short>( out, (short)( s.comp[f] < 0 ? -1 : std::find( comps.begin(), comps.end(), s.comp[f] ) - comps.begin() ) );
	}
	Set<int>( out, S_OFSCOMPFRAMES, (int)out.size() );
	PutFrameTable( out, keys, numFrames );
	Set<int>( out, S_OFSEND, (int)out.size() );

	e.numBones = 0;
	e.numKeys = keys.size();
	return e;
}

}

bool ConvertMdc( const std::vector<uint8_t> &in, std::vector<uint8_t> &out, MdcStats &st, const char *name ) {
	out.clear();
	if ( in.size() < H_SIZE || memcmp( in.data(), "IDPC", 4 ) || Get<int>( in, 4 ) != 2 ) {
		fprintf( stderr, "%s: not an .mdc\n", name );
		return false;
	}
	int numFrames = Get<int>( in, H_NUMFRAMES ), numSurfaces = Get<int>( in, H_NUMSURFACES );
	int ofsSurfaces = Get<int>( in, H_OFSSURFACES ), ofsEnd = Get<int>( in, H_OFSEND );
	if ( numFrames < 1 || numSurfaces < 0 || ofsSurfaces < H_SIZE || ofsEnd != (int)in.size() ||
		 Get<int>( in, H_OFSFRAMES ) > ofsSurfaces || Get<int>( in, H_OFSTAGS ) > ofsSurfaces ||
		 Get<int>( in, H_OFSTAGNAMES ) > ofsSurfaces ) {
		fprintf( stderr, "%s: funny .mdc\n", name );
		return false;
	}
	st.files++;
	st.bytesIn += in.size();
	if ( numFrames == 1 ) {
		return true;    /* nothing moves */
	}

	/* header, frames and tags as they are */
	out.assign( in.begin(), in.begin() + ofsSurfaces );
	size_t surf = ofsSurfaces;
	double maxErr = 0, sumErr = 0;
	long numErr = 0, bones = 0, keys = 0, boneSurfs = 0;
	for ( int i = 0; i < numSurfaces; i++ ) {
		if ( surf + S_SIZE > in.size() ) {
			fprintf( stderr, "%s: funny .mdc\n", name );
			out.clear();
			return false;
		}
		int surfEnd = Get<int>( in, surf + S_OFSEND );
		int numComp = Get<int>( in, surf + S_NUMCOMP ), numBase = Get<int>( in, surf + S_NUMBASE );
		Surface s;
		s.numVerts = Get<int>( in, surf + S_NUMVERTS );
		s.numTris = Get<int>( in, surf + S_NUMTRIS );
		int ofsXyz = Get<int>( in, surf + S_OFSXYZ ), ofsComp = Get<int>( in, surf + S_OFSCOMP );
		int ofsBaseFrames = Get<int>( in, surf + S_OFSBASEFRAMES ), ofsCompFrames = Get<int>( in, surf + S_OFSCOMPFRAMES );
		if ( surfEnd <= 0 || surf + surfEnd > in.size() || s.numVerts < 0 || numComp < 0 || numBase < 1 ) {
			fprintf( stderr, "%s: funny .mdc\n", name );
			out.clear();
			return false;
		}

		if ( !s.numVerts ) {
			out.insert( out.end(), in.begin() + surf, in.begin() + surf + surfEnd );
			surf += surfEnd;
			continue;
		}

		/* every frame, decoded as the renderer does */
		s.pos.assign( numFrames, std::vector<V3>( s.numVerts ) );
		s.normal.resize( s.numVerts );
		s.base.resize( numFrames );
		s.comp.resize( numFrames );
		for ( int f = 0; f < numFrames; f++ ) {
			int base = Get<short>( in, surf + ofsBaseFrames + f * 2 );
			int comp = numComp ? Get<short>( in, surf + ofsCompFrames + f * 2 ) : -1;
			if ( base < 0 || base >= numBase || comp >= numComp ) {
				fprintf( stderr, "%s: funny .mdc\n", name );
				out.clear();
				return false;
			}
			s.base[f] = base;
			s.comp[f] = comp;
			for ( int v = 0; v < s.numVerts; v++ ) {
				size_t xo = surf + ofsXyz + ( (size_t)base * s.numVerts + v ) * 8;
				V3 p = { Get<short>( in, xo ) * MD3_XYZ_SCALE, Get<short>( in, xo + 2 ) * MD3_XYZ_SCALE,
						 Get<short>( in, xo + 4 ) * MD3_XYZ_SCALE };
				V3 n;
				if ( comp >= 0 ) {
					uint32_t ofs = Get<uint32_t>( in, surf + ofsComp + ( (size_t)comp * s.numVerts + v ) * 4 );
					p = p + V3 { ( ( ofs & 255 ) - MDC_MAX_OFS ) * MDC_DIST_SCALE,
								 ( ( ( ofs >> 8 ) & 255 ) - MDC_MAX_OFS ) * MDC_DIST_SCALE,
								 ( ( ( ofs >> 16 ) & 255 ) - MDC_MAX_OFS ) * MDC_DIST_SCALE };
					n = { anormals[ofs >> 24][0], anormals[ofs >> 24][1], anormals[ofs >> 24][2] };
				} else {
					int ll = (unsigned short)Get<short>( in, xo + 6 );
					double lat = ( ( ll >> 8 ) & 255 ) * ( 2 * M_PI / 256 ), lng = ( ll & 255 ) * ( 2 * M_PI / 256 );
					n = { cos( lat ) * sin( lng ), sin( lat ) * sin( lng ), cos( lng ) };
				}
				s.pos[f][v] = p;
				if ( f == 0 ) {
					s.normal[v] = n;
				}
			}
		}
		Tolerances( s );

		/* the .mdc's vertexes at fewer frames, or rigid bones if they come
		 * out smaller; as few bones as keep it within tolerance, and fewer
		 * than would make it no smaller */
		Encoded best = EncodeVertexes( in, surf, s );
		int fixedSize = S_SIZE + ( Get<int>( in, surf + S_NUMSHADERS ) * 68 ) + s.numTris * 12 + s.numVerts * 16;
		int maxBones = std::min( { MDB_MAX_BONES, std::max( 1, s.numVerts ),
								   std::max( 1, (int)( ( surfEnd * MDB_MAX_RATIO - fixedSize ) / ( numFrames * 20 + 2 ) ) ) } );
		Bones b;
		if ( MakeBones( s, b, maxBones ) ) {
			Encoded e = EncodeBones( in, surf, s, b );
			if ( e.data.size() < best.data.size() ) {
				best = std::move( e );
			}
		}

		out.insert( out.end(), best.data.begin(), best.data.end() );
		bones += best.numBones;
		boneSurfs += best.numBones > 0;
		keys += best.numKeys;
		maxErr = std::max( maxErr, best.maxErr );
		sumErr += best.sumErr;
		numErr += s.numVerts;

		surf += surfEnd;
	}

	if ( out.size() > in.size() * MDB_MAX_RATIO ) {
		out.clear();
		st.kept++;
		return true;
	}
	Set<int>( out, H_OFSEND, (int)out.size() );
	st.converted++;
	st.bytesConverted += in.size();
	st.bytesOut += out.size();
	st.bones += bones;
	st.boneSurfaces += boneSurfs;
	st.surfaces += numSurfaces;
	st.frames += (long)numFrames * numSurfaces;
	st.keys += keys;
	st.maxErr = std::max( st.maxErr, maxErr );
	st.sumErr += sumErr;
	st.numErr += numErr;
	return true;
}
