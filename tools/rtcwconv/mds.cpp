/*
 * .mds (RtCW skeletal model) -> MDSC (the same model, animation reduced).
 *
 * Nearly all of an .mds is animation: every frame of every animation holds a
 * pose for each of its ~70 bones, in model space. Turned relative to the
 * parent bone (mdsc/mdsc.c), most bones hardly move, and each bone keeps only
 * the frames where interpolating between its neighbours isn't close enough
 * (strippy's keyframe reduction, but checked against interpolation rather
 * than against the last key). The direction from the parent never changes
 * relative to it, so is kept once. Every frame is then decoded again with the
 * game's own decoder to measure how far it moved.
 *
 * The bones, surfaces and tags are copied as they are, the triangles put in
 * strip order. The layout is described in mdsc/mdsc.h.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdsc.h"
#include "rtcwconv.h"

#define MDS_IDENT    ( ( 'W' << 24 ) + ( 'S' << 16 ) + ( 'D' << 8 ) + 'M' )
#define MDS_VERSION  4

/* the on-disk structures, as in qfiles.h */
struct MdsHeader {
	int32_t ident, version;
	char name[64];
	float lodScale, lodBias;
	int32_t numFrames, numBones, ofsFrames, ofsBones, torsoParent;
	int32_t numSurfaces, ofsSurfaces, numTags, ofsTags, ofsEnd;
};

struct MdsSurface {
	int32_t ident;
	char name[64], shader[64];
	int32_t shaderIndex, minLod, ofsHeader;
	int32_t numVerts, ofsVerts, numTriangles, ofsTriangles, ofsCollapseMap;
	int32_t numBoneReferences, ofsBoneReferences, ofsEnd;
};

#define FRAME_FLOATS     13     /* bounds[2], localOrigin, radius, parentOffset */
#define CULL_TOL         8.0f   /* how far the cull bounds may be off, before they're made bigger to hold it */
#define BONE_SHORTS      MDSC_POSE_SHORTS
#define BONE_INFO_SIZE   80     /* mdsBoneInfo_t */
#define TAG_SIZE         72     /* mdsTag_t */

struct Out {
	std::vector<uint8_t> b;
	size_t pos() const { return b.size(); }
	void put( const void *p, size_t n ) { b.insert( b.end(), (const uint8_t *)p, (const uint8_t *)p + n ); }
	void align() { while ( b.size() & 3 ) b.push_back( 0 ); }
	void set32( size_t at, int32_t v ) { memcpy( &b[at], &v, 4 ); }
};

/* ---- keyframe reduction ---- */

struct Quat {
	float v[4];
};

struct Dir {
	float v[3];
};

struct Matrix {
	mdscMatrix_t m;
};

/* bone tracks: can every frame between s and e be had by interpolating, the
   way the game will? */
static bool QuatSpanFits( const std::vector<Quat> &exact, const std::vector<uint32_t> &packed, int s, int e, float tol ) {
	const unsigned short keys[2] = { (unsigned short)s, (unsigned short)e };
	const unsigned int values[2] = { packed[s], packed[e] };

	for ( int f = s + 1; f < e; f++ ) {
		float q[4];
		MDSC_TrackQuat( keys, values, 2, f, q );
		if ( MDSC_QuatAngle( q, exact[f].v ) > tol ) {
			return false;
		}
	}
	return true;
}

static bool DirSpanFits( const std::vector<Dir> &exact, const std::vector<short> &packed, int s, int e, float tol ) {
	const unsigned short keys[2] = { (unsigned short)s, (unsigned short)e };
	short values[8];

	memcpy( values, &packed[s * 4], 8 );
	memcpy( values + 4, &packed[e * 4], 8 );
	for ( int f = s; f <= e; f++ ) {
		float d[3];
		MDSC_TrackDir( keys, values, 2, f, d );
		if ( MDSC_DirAngle( d, exact[f].v ) > tol ) {
			return false;
		}
	}
	return true;
}

/* frame floats first to first + n */
static bool FrameSpanFits( const std::vector<float> &v, int first, int n, int s, int e, float tol ) {
	for ( int f = s + 1; f < e; f++ ) {
		float t = (float)( f - s ) / (float)( e - s );
		for ( int c = first; c < first + n; c++ ) {
			float a = v[s * FRAME_FLOATS + c], b = v[e * FRAME_FLOATS + c];
			if ( fabsf( a + ( b - a ) * t - v[f * FRAME_FLOATS + c] ) > tol ) {
				return false;
			}
		}
	}
	return true;
}

/* greedy: from each key, reach as far as still fits */
template <typename Fits>
static std::vector<int> ReduceKeys( int numFrames, int maxSpan, Fits fits ) {
	std::vector<int> keys = { 0 };
	int s = 0;

	while ( s < numFrames - 1 ) {
		int best = s + 1;
		for ( int e = s + 2; e <= numFrames - 1 && e - s <= maxSpan; e++ ) {
			if ( !fits( s, e ) ) {
				break;
			}
			best = e;
		}
		keys.push_back( best );
		s = best;
	}
	return keys;
}

static void PutKeys( Out &o, size_t trackAt, size_t base, const std::vector<int> &keys ) {
	o.set32( trackAt, (int32_t)keys.size() );
	o.set32( trackAt + 4, (int32_t)( o.pos() - base ) );
	for ( int k : keys ) {
		uint16_t f = (uint16_t)k;
		o.put( &f, 2 );
	}
	o.align();
	o.set32( trackAt + 8, (int32_t)( o.pos() - base ) );
}

/* ---- conversion ---- */

bool ConvertMds( const std::vector<uint8_t> &in, std::vector<uint8_t> &result,
                 const MdsOptions &opt, MdsStats &st, const char *name ) {
	MdsHeader h;

	if ( in.size() < sizeof( h ) ) {
		fprintf( stderr, "%s: too small for an .mds\n", name );
		return false;
	}
	memcpy( &h, in.data(), sizeof( h ) );
	if ( h.ident != MDS_IDENT || h.version != MDS_VERSION ) {
		fprintf( stderr, "%s: not a version %d .mds\n", name, MDS_VERSION );
		return false;
	}
	if ( h.numFrames < 1 || h.numFrames > 65535 || h.numBones < 1 || (size_t)h.ofsEnd > in.size() ) {
		fprintf( stderr, "%s: bad header\n", name );
		return false;
	}

	const int nf = h.numFrames, nb = h.numBones;
	const size_t frameSize = FRAME_FLOATS * 4 + nb * BONE_SHORTS * 2;

	if ( nb > MDSC_MAX_BONES ) {
		fprintf( stderr, "%s: more than %d bones\n", name, MDSC_MAX_BONES );
		return false;
	}
	std::vector<int> parent( nb );
	std::vector<float> parentDist( nb );
	for ( int b = 0; b < nb; b++ ) {
		const uint8_t *bi = in.data() + h.ofsBones + b * BONE_INFO_SIZE;
		memcpy( &parent[b], bi + 64, 4 );
		memcpy( &parentDist[b], bi + 72, 4 );
		if ( parent[b] >= b ) {
			fprintf( stderr, "%s: bone %d comes before its parent\n", name, b );
			return false;
		}
	}

	/* pull the frames apart */
	std::vector<float> frameVals( nf * FRAME_FLOATS );
	std::vector<short> model( (size_t)nf * nb * BONE_SHORTS );
	for ( int f = 0; f < nf; f++ ) {
		const uint8_t *fr = in.data() + h.ofsFrames + f * frameSize;
		memcpy( &frameVals[f * FRAME_FLOATS], fr, FRAME_FLOATS * 4 );
		memcpy( &model[(size_t)f * nb * BONE_SHORTS], fr + FRAME_FLOATS * 4, nb * BONE_SHORTS * 2 );
	}

	Out o;
	o.put( &h, sizeof( h ) );

	/* bones and tags as they are */
	size_t ofsBones = o.pos();
	o.put( in.data() + h.ofsBones, nb * BONE_INFO_SIZE );
	size_t ofsTags = o.pos();
	o.put( in.data() + h.ofsTags, h.numTags * TAG_SIZE );

	/* surfaces as they are, triangles in strip order */
	size_t ofsSurfaces = o.pos();
	size_t at = h.ofsSurfaces;
	for ( int i = 0; i < h.numSurfaces; i++ ) {
		MdsSurface s;
		memcpy( &s, in.data() + at, sizeof( s ) );

		size_t surfAt = o.pos();
		o.put( in.data() + at, s.ofsEnd );
		o.set32( surfAt + offsetof( MdsSurface, ofsHeader ), -(int32_t)surfAt );

		std::vector<uint32_t> tris( s.numTriangles * 3 );
		memcpy( tris.data(), in.data() + at + s.ofsTriangles, tris.size() * 4 );
		int stripTris;
		int strips = StripOrder( tris, &stripTris );
		if ( strips < 0 ) {
			fprintf( stderr, "%s: %s: strips didn't check out, triangles left in order\n", name, s.name );
		} else {
			memcpy( &o.b[surfAt + s.ofsTriangles], tris.data(), tris.size() * 4 );
			st.strips += strips;
			st.stripTris += stripTris;
		}
		st.tris += s.numTriangles;

		at += s.ofsEnd;
	}

	/* the animation */
	o.align();
	size_t base = o.pos();
	size_t frameTrackAt = base;
	o.b.resize( base + sizeof( mdscAnim_t ) + sizeof( mdscTrack_t ) * ( 2 * nb - 1 ) );
	const size_t trackAt = base + offsetof( mdscAnim_t, tracks );

	std::vector<int> keys = ReduceKeys( nf, opt.maxSpan, [&]( int s, int e ) {
		return FrameSpanFits( frameVals, MDSC_CULL_FLOATS, MDSC_OFFSET_FLOATS, s, e, opt.offsetTol );
	} );
	PutKeys( o, frameTrackAt, base, keys );
	for ( int k : keys ) {
		o.put( &frameVals[k * FRAME_FLOATS + MDSC_CULL_FLOATS], MDSC_OFFSET_FLOATS * 4 );
	}
	st.frameKeys += keys.size();

	/* the cull bounds: within CULL_TOL, then that much bigger (the sphere's
	   centre may be off by it in each of x, y and z too) */
	keys = ReduceKeys( nf, opt.maxSpan, [&]( int s, int e ) {
		return FrameSpanFits( frameVals, 0, MDSC_CULL_FLOATS, s, e, CULL_TOL );
	} );
	PutKeys( o, frameTrackAt + offsetof( mdscAnim_t, cullTrack ), base, keys );
	for ( int k : keys ) {
		float v[MDSC_CULL_FLOATS];
		memcpy( v, &frameVals[k * FRAME_FLOATS], sizeof( v ) );
		for ( int c = 0; c < 3; c++ ) {
			v[c] -= CULL_TOL;
			v[3 + c] += CULL_TOL;
		}
		v[9] += CULL_TOL * 3;
		o.put( v, sizeof( v ) );
	}
	st.cullKeys += keys.size();

	/* bones, parents first, each against its parent as the game will have
	   decoded it so errors don't add up down the skeleton */
	std::vector<Matrix> decoded( (size_t)nb * nf );
	std::vector<std::vector<int>> rotKeys( nb ), dirKeys( nb );
	std::vector<std::vector<uint32_t>> rotVals( nb );
	std::vector<std::vector<short>> dirVals( nb );
	for ( int b = 0; b < nb; b++ ) {
		std::vector<Quat> exactRot( nf );
		std::vector<Dir> exactDir( nf );
		std::vector<uint32_t> packedRot( nf );
		std::vector<short> packedDir( nf * 4 );

		for ( int f = 0; f < nf; f++ ) {
			const short *pose = &model[( (size_t)f * nb + b ) * BONE_SHORTS];
			const float( *p )[3] = parent[b] < 0 ? NULL : decoded[(size_t)parent[b] * nf + f].m;

			MDSC_LocalRotation( p, pose, exactRot[f].v );
			packedRot[f] = MDSC_PackQuat( exactRot[f].v );
			if ( p ) {
				MDSC_LocalDir( p, pose + 4, exactDir[f].v );
			} else {
				exactDir[f] = { { 0, 0, 0 } };
			}
			MDSC_PackDir( exactDir[f].v, &packedDir[f * 4] );
		}

		rotKeys[b] = ReduceKeys( nf, opt.maxSpan, [&]( int s, int e ) {
			return QuatSpanFits( exactRot, packedRot, s, e, opt.angleTol );
		} );
		if ( parent[b] < 0 ) {
			dirKeys[b] = { 0 };
		} else {
			/* the direction is good enough when the bone is within offsetTol */
			float tol = parentDist[b] > opt.offsetTol ? ( opt.offsetTol / parentDist[b] ) * ( 180.0f / (float)M_PI ) : 180.0f;
			dirKeys[b] = ReduceKeys( nf, opt.maxSpan, [&]( int s, int e ) {
				return DirSpanFits( exactDir, packedDir, s, e, tol );
			} );
		}
		for ( int k : rotKeys[b] ) {
			rotVals[b].push_back( packedRot[k] );
		}
		for ( int k : dirKeys[b] ) {
			dirVals[b].insert( dirVals[b].end(), &packedDir[k * 4], &packedDir[k * 4 + 4] );
		}

		/* and what the game will make of it */
		std::vector<unsigned short> k16( rotKeys[b].begin(), rotKeys[b].end() );
		for ( int f = 0; f < nf; f++ ) {
			const float( *p )[3] = parent[b] < 0 ? NULL : decoded[(size_t)parent[b] * nf + f].m;
			float q[4];
			MDSC_TrackQuat( k16.data(), rotVals[b].data(), (int)k16.size(), f, q );
			MDSC_ChildMatrix( p, q, decoded[(size_t)b * nf + f].m );
		}
		st.keysOut += rotKeys[b].size();
		st.dirKeys += dirKeys[b].size();
	}

	for ( int b = 0; b < nb; b++ ) {
		PutKeys( o, trackAt + b * sizeof( mdscTrack_t ), base, rotKeys[b] );
		o.put( rotVals[b].data(), rotVals[b].size() * 4 );
	}
	for ( int b = 0; b < nb; b++ ) {
		PutKeys( o, trackAt + ( nb + b ) * sizeof( mdscTrack_t ), base, dirKeys[b] );
		o.put( dirVals[b].data(), dirVals[b].size() * 2 );
	}
	st.framesIn += (long)nf * nb;

	/* header pointing at it all */
	MdsHeader *oh = (MdsHeader *)o.b.data();
	oh->ident = MDSC_IDENT;
	oh->version = MDSC_VERSION;
	oh->ofsFrames = (int32_t)base;
	oh->ofsBones = (int32_t)ofsBones;
	oh->ofsTags = (int32_t)ofsTags;
	oh->ofsSurfaces = (int32_t)ofsSurfaces;
	oh->ofsEnd = (int32_t)o.pos();

	/* decode every frame as the game will and see how far the bones moved */
	std::vector<uint8_t> dec( MDSC_FRAME_SIZE( nb ) );
	std::vector<float> posA( nb * 3 ), posB( nb * 3 );
	for ( int f = 0; f < nf; f++ ) {
		const short *ma = &model[(size_t)f * nb * BONE_SHORTS];
		const short *mb = (const short *)( dec.data() + FRAME_FLOATS * 4 );
		const float *rootA = &frameVals[f * FRAME_FLOATS + 10];
		const float *rootB = (const float *)dec.data() + 10;

		MDSC_DecodeFrame( o.b.data(), f, dec.data() );

		/* the cull bounds must hold the frame's */
		{
			const float *a = &frameVals[f * FRAME_FLOATS], *b = (const float *)dec.data();
			float d = 0;
			bool holds = true;
			for ( int c = 0; c < 3; c++ ) {
				holds = holds && b[c] <= a[c] && b[3 + c] >= a[3 + c];
				d += ( a[6 + c] - b[6 + c] ) * ( a[6 + c] - b[6 + c] );
			}
			if ( !holds || sqrtf( d ) + a[9] > b[9] ) {
				fprintf( stderr, "%s: frame %d: cull bounds don't hold it\n", name, f );
				return false;
			}
		}

		/* bone positions, the way R_CalcBone places them */
		for ( int b = 0; b < nb; b++ ) {
			for ( int set = 0; set < 2; set++ ) {
				const short *p = ( set ? mb : ma ) + b * BONE_SHORTS;
				float *pos = &( set ? posB : posA )[b * 3];
				const float *root = set ? rootB : rootA;

				if ( parent[b] < 0 ) {
					memcpy( pos, root, 12 );
				} else {
					float pitch = p[4] * ( (float)M_PI / 32768.0f ), yaw = p[5] * ( (float)M_PI / 32768.0f );
					const float *pp = &( set ? posB : posA )[parent[b] * 3];
					pos[0] = pp[0] + parentDist[b] * cosf( pitch ) * cosf( yaw );
					pos[1] = pp[1] + parentDist[b] * cosf( pitch ) * sinf( yaw );
					pos[2] = pp[2] - parentDist[b] * sinf( pitch );
				}
			}
			float qa[4], qb[4];
			MDSC_LocalRotation( NULL, ma + b * BONE_SHORTS, qa );
			MDSC_LocalRotation( NULL, mb + b * BONE_SHORTS, qb );
			float ang = MDSC_QuatAngle( qa, qb );
			if ( ang > st.maxAngle ) {
				st.maxAngle = ang;
			}

			float d = sqrtf( ( posA[b * 3] - posB[b * 3] ) * ( posA[b * 3] - posB[b * 3] ) +
							 ( posA[b * 3 + 1] - posB[b * 3 + 1] ) * ( posA[b * 3 + 1] - posB[b * 3 + 1] ) +
							 ( posA[b * 3 + 2] - posB[b * 3 + 2] ) * ( posA[b * 3 + 2] - posB[b * 3 + 2] ) );
			if ( d > st.maxErr ) {
				st.maxErr = d;
			}
			st.sumErr += d;
			st.numErr++;
		}
	}

	st.files++;
	st.bytesIn += h.ofsEnd;
	st.bytesOut += o.pos();
	result.swap( o.b );
	return true;
}
