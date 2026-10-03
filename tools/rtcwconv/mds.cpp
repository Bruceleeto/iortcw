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
#include <algorithm>
#include <map>
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

/* a track's keys, as a list or a bitmap, whichever is smaller (mdsc.h) */
static void PutKeys( Out &o, size_t trackAt, size_t base, const std::vector<int> &keys ) {
	const int numBlocks = keys.back() / MDSC_KEY_BLOCK + 1;
	const size_t listSize = ( keys.size() * 2 + 3 ) & ~3;
	const size_t bitmapSize = ( ( 1 + numBlocks ) * 2 + 3 & ~3 ) + numBlocks * 8;
	const bool bitmap = bitmapSize < listSize;

	if ( keys[0] != 0 || keys.size() > 0xffff || numBlocks > 0xffff ) {
		fprintf( stderr, "PutKeys: keys not from frame 0, or too many\n" );
		exit( 1 );
	}
	o.set32( trackAt, (int32_t)keys.size() | ( bitmap ? MDSC_KEYS_BITMAP : 0 ) );
	o.set32( trackAt + 4, (int32_t)( o.pos() - base ) );
	if ( bitmap ) {
		std::vector<uint16_t> rank( numBlocks, 0 );
		std::vector<uint32_t> bits( numBlocks * 2, 0 );
		uint16_t n = (uint16_t)numBlocks;

		for ( int k : keys ) {
			bits[k >> 5] |= 1u << ( k & 31 );
		}
		for ( int b = 1, c = 0; b < numBlocks; b++ ) {
			for ( int w = ( b - 1 ) * 2; w < b * 2; w++ ) {
				c += __builtin_popcount( bits[w] );
			}
			rank[b] = (uint16_t)c;
		}
		o.put( &n, 2 );
		o.put( rank.data(), numBlocks * 2 );
		o.align();
		o.put( bits.data(), bits.size() * 4 );
	} else {
		for ( int k : keys ) {
			uint16_t f = (uint16_t)k;
			o.put( &f, 2 );
		}
		o.align();
	}
	o.set32( trackAt + 8, (int32_t)( o.pos() - base ) );
}

/* ---- conversion ---- */

/* an .mds pulled apart */
struct MdsSrc {
	MdsHeader h;
	const std::vector<uint8_t> *in;
	int nf, nb;
	std::vector<int> parent;
	std::vector<float> parentDist;
	std::vector<float> frameVals;   /* [nf][FRAME_FLOATS] */
	std::vector<short> model;       /* [nf][nb][BONE_SHORTS] */
};

static bool ParseMds( const std::vector<uint8_t> &in, MdsSrc &s, const char *name ) {
	MdsHeader &h = s.h;

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
	s.in = &in;
	s.nf = nf;
	s.nb = nb;
	s.parent.resize( nb );
	s.parentDist.resize( nb );
	for ( int b = 0; b < nb; b++ ) {
		const uint8_t *bi = in.data() + h.ofsBones + b * BONE_INFO_SIZE;
		memcpy( &s.parent[b], bi + 64, 4 );
		memcpy( &s.parentDist[b], bi + 72, 4 );
		if ( s.parent[b] >= b ) {
			fprintf( stderr, "%s: bone %d comes before its parent\n", name, b );
			return false;
		}
	}

	/* pull the frames apart */
	s.frameVals.resize( nf * FRAME_FLOATS );
	s.model.resize( (size_t)nf * nb * BONE_SHORTS );
	for ( int f = 0; f < nf; f++ ) {
		const uint8_t *fr = in.data() + h.ofsFrames + f * frameSize;
		memcpy( &s.frameVals[f * FRAME_FLOATS], fr, FRAME_FLOATS * 4 );
		memcpy( &s.model[(size_t)f * nb * BONE_SHORTS], fr + FRAME_FLOATS * 4, nb * BONE_SHORTS * 2 );
	}
	return true;
}

/* the header, then the bones, and the tags and surfaces (mesh), as they
   are but for the triangles put in strip order; the header's offsets set */
static void PutMesh( Out &o, const MdsSrc &s, bool mesh, MdsStats &st, const char *name ) {
	const std::vector<uint8_t> &in = *s.in;
	const MdsHeader &h = s.h;

	o.put( &h, sizeof( h ) );

	size_t ofsBones = o.pos();
	o.put( in.data() + h.ofsBones, s.nb * BONE_INFO_SIZE );
	size_t ofsTags = o.pos();
	if ( mesh ) {
		o.put( in.data() + h.ofsTags, h.numTags * TAG_SIZE );
	}

	size_t ofsSurfaces = o.pos();
	size_t at = h.ofsSurfaces;
	for ( int i = 0; mesh && i < h.numSurfaces; i++ ) {
		MdsSurface surf;
		memcpy( &surf, in.data() + at, sizeof( surf ) );

		std::vector<uint32_t> tris( surf.numTriangles * 3 );
		memcpy( tris.data(), in.data() + at + surf.ofsTriangles, tris.size() * 4 );
		int stripTris;
		int strips = StripOrder( tris, &stripTris );
		if ( strips < 0 ) {
			fprintf( stderr, "%s: %s: strips didn't check out, triangles left in order\n", name, surf.name );
		} else {
			st.stripTris += stripTris;
		}
		st.tris += surf.numTriangles;

		/* the surface with its triangles as strips (MDSC_VERSION), the rest
		   moved up into the room that leaves */
		long numStrips = 0;
		std::vector<uint16_t> strip = StripIndexes( tris, &numStrips );
		st.strips += numStrips;
		strip.resize( ( strip.size() + 1 ) & ~1, 0 );
		const int triBytes = surf.numTriangles * 12, stripBytes = strip.size() * 2, delta = triBytes - stripBytes;
		size_t surfAt = o.pos();
		o.put( in.data() + at, surf.ofsTriangles );
		o.put( strip.data(), stripBytes );
		o.put( in.data() + at + surf.ofsTriangles + triBytes, surf.ofsEnd - surf.ofsTriangles - triBytes );
		o.set32( surfAt + offsetof( MdsSurface, ofsHeader ), -(int32_t)surfAt );
		for ( size_t f : { offsetof( MdsSurface, ofsVerts ), offsetof( MdsSurface, ofsCollapseMap ),
						   offsetof( MdsSurface, ofsBoneReferences ), offsetof( MdsSurface, ofsEnd ) } ) {
			int32_t v;
			memcpy( &v, &o.b[surfAt + f], 4 );
			if ( v >= surf.ofsTriangles + triBytes ) {
				o.set32( surfAt + f, v - delta );
			} else if ( v > surf.ofsTriangles ) {
				fprintf( stderr, "%s: %s: something in its triangles\n", name, surf.name );
				exit( 1 );
			}
		}

		at += surf.ofsEnd;
	}

	MdsHeader *oh = (MdsHeader *)o.b.data();
	oh->ident = MDSC_IDENT;
	oh->version = MDSC_VERSION;
	oh->ofsBones = (int32_t)ofsBones;
	oh->ofsTags = (int32_t)ofsTags;
	oh->ofsSurfaces = (int32_t)ofsSurfaces;
	if ( !mesh ) {
		oh->numTags = 0;
		oh->numSurfaces = 0;
	}
}

/* An mdscAnim_t, at the end of o (aligned): its offsets and bones of s's
   frames `pose` in that order, its cull bounds of s's frames `cull`.
   Returns where it starts. */
static size_t PutAnim( Out &o, const MdsSrc &s, const std::vector<int> &pose, const std::vector<int> &cull,
					   const MdsOptions &opt, MdsStats &st ) {
	const int nb = s.nb, nf = (int)pose.size(), ncf = (int)cull.size();

	/* just those frames */
	std::vector<float> frameVals( nf * FRAME_FLOATS ), cullVals( ncf * FRAME_FLOATS );
	std::vector<short> model( (size_t)nf * nb * BONE_SHORTS );
	for ( int f = 0; f < nf; f++ ) {
		memcpy( &frameVals[f * FRAME_FLOATS], &s.frameVals[pose[f] * FRAME_FLOATS], FRAME_FLOATS * 4 );
		memcpy( &model[(size_t)f * nb * BONE_SHORTS], &s.model[(size_t)pose[f] * nb * BONE_SHORTS], nb * BONE_SHORTS * 2 );
	}
	for ( int f = 0; f < ncf; f++ ) {
		memcpy( &cullVals[f * FRAME_FLOATS], &s.frameVals[cull[f] * FRAME_FLOATS], FRAME_FLOATS * 4 );
	}

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
	keys = ReduceKeys( ncf, opt.maxSpan, [&]( int s, int e ) {
		return FrameSpanFits( cullVals, 0, MDSC_CULL_FLOATS, s, e, CULL_TOL );
	} );
	PutKeys( o, frameTrackAt + offsetof( mdscAnim_t, cullTrack ), base, keys );
	for ( int k : keys ) {
		float v[MDSC_CULL_FLOATS];
		memcpy( v, &cullVals[k * FRAME_FLOATS], sizeof( v ) );
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
		const int parent = s.parent[b];

		for ( int f = 0; f < nf; f++ ) {
			const short *p = &model[( (size_t)f * nb + b ) * BONE_SHORTS];
			const float( *pm )[3] = parent < 0 ? NULL : decoded[(size_t)parent * nf + f].m;

			MDSC_LocalRotation( pm, p, exactRot[f].v );
			packedRot[f] = MDSC_PackQuat( exactRot[f].v );
			if ( pm ) {
				MDSC_LocalDir( pm, p + 4, exactDir[f].v );
			} else {
				exactDir[f] = { { 0, 0, 0 } };
			}
			MDSC_PackDir( exactDir[f].v, &packedDir[f * 4] );
		}

		rotKeys[b] = ReduceKeys( nf, opt.maxSpan, [&]( int s, int e ) {
			return QuatSpanFits( exactRot, packedRot, s, e, opt.angleTol );
		} );
		if ( parent < 0 ) {
			dirKeys[b] = { 0 };
		} else {
			/* the direction is good enough when the bone is within offsetTol */
			float tol = s.parentDist[b] > opt.offsetTol ? ( opt.offsetTol / s.parentDist[b] ) * ( 180.0f / (float)M_PI ) : 180.0f;
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
			const float( *pm )[3] = parent < 0 ? NULL : decoded[(size_t)parent * nf + f].m;
			float q[4];
			MDSC_TrackQuat( k16.data(), rotVals[b].data(), (int)k16.size(), f, q );
			MDSC_ChildMatrix( pm, q, decoded[(size_t)b * nf + f].m );
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
	return base;
}

/* decode every frame of the MDSC made of s (with its base, if shared) as the
   game will, and see how far the bones moved */
static bool CheckMdsc( const std::vector<uint8_t> &file, const uint8_t *baseFile, const MdsSrc &s, MdsStats &st, const char *name ) {
	const int nf = s.nf, nb = s.nb;
	std::vector<uint8_t> dec( MDSC_FRAME_SIZE( nb ) );
	std::vector<float> posA( nb * 3 ), posB( nb * 3 );

	for ( int f = 0; f < nf; f++ ) {
		const short *ma = &s.model[(size_t)f * nb * BONE_SHORTS];
		const short *mb = (const short *)( dec.data() + FRAME_FLOATS * 4 );
		const float *rootA = &s.frameVals[f * FRAME_FLOATS + 10];
		const float *rootB = (const float *)dec.data() + 10;

		if ( baseFile ) {
			MDSC_DecodeSharedFrame( file.data(), baseFile, f, dec.data() );
		} else {
			MDSC_DecodeFrame( file.data(), f, dec.data() );
		}

		/* the cull bounds must hold the frame's */
		{
			const float *a = &s.frameVals[f * FRAME_FLOATS], *b = (const float *)dec.data();
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

				if ( s.parent[b] < 0 ) {
					memcpy( pos, root, 12 );
				} else {
					float pitch = p[4] * ( (float)M_PI / 32768.0f ), yaw = p[5] * ( (float)M_PI / 32768.0f );
					const float *pp = &( set ? posB : posA )[s.parent[b] * 3];
					pos[0] = pp[0] + s.parentDist[b] * cosf( pitch ) * cosf( yaw );
					pos[1] = pp[1] + s.parentDist[b] * cosf( pitch ) * sinf( yaw );
					pos[2] = pp[2] - s.parentDist[b] * sinf( pitch );
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
	return true;
}

bool ConvertMds( const std::vector<uint8_t> &in, std::vector<uint8_t> &result,
                 const MdsOptions &opt, MdsStats &st, const char *name ) {
	MdsSrc s;

	if ( !ParseMds( in, s, name ) ) {
		return false;
	}

	Out o;
	PutMesh( o, s, true, st, name );
	std::vector<int> all( s.nf );
	for ( int f = 0; f < s.nf; f++ ) {
		all[f] = f;
	}
	size_t anim = PutAnim( o, s, all, all, opt, st );
	MdsHeader *oh = (MdsHeader *)o.b.data();
	oh->ofsFrames = (int32_t)anim;
	oh->ofsEnd = (int32_t)o.pos();

	if ( !CheckMdsc( o.b, NULL, s, st, name ) ) {
		return false;
	}
	st.files++;
	st.bytesIn += s.h.ofsEnd;
	st.bytesOut += o.pos();
	result.swap( o.b );
	return true;
}

/* ---- shared frames (mdsc.h, mdscShare_t) ---- */

const MdsGroup mdsGroups[] = {
	/* the guards: of officerss' 5335 frames, 4205 are infantryss' (but for
	   the cull bounds), of trench's 5383, 4156 */
	{ "models/players/guards_anim.mds",
	  { "models/players/infantryss/body.mds", "models/players/officerss/body.mds", "models/players/trench/body.mds" } },
};
const int numMdsGroups = sizeof( mdsGroups ) / sizeof( mdsGroups[0] );

bool ConvertMdsGroup( const MdsGroup &g, const std::vector<std::vector<uint8_t>> &ins,
					  std::vector<std::vector<uint8_t>> &outs, std::vector<uint8_t> &baseOut,
					  const MdsOptions &opt, MdsStats &st ) {
	const int n = (int)ins.size();
	std::vector<MdsSrc> src( n );

	for ( int m = 0; m < n; m++ ) {
		if ( !ParseMds( ins[m], src[m], g.members[m].c_str() ) ) {
			return false;
		}
		if ( src[m].nb != src[0].nb || src[m].parent != src[0].parent ) {
			fprintf( stderr, "%s: not %s's skeleton\n", g.members[m].c_str(), g.members[0].c_str() );
			return false;
		}
	}
	const MdsSrc &first = src[0];

	/* each frame of each, as one of the first's where it is one: the next
	   of the last one's where that will do, so they come in runs */
	std::map<std::vector<uint8_t>, std::vector<int>> byPose;
	for ( int f = 0; f < first.nf; f++ ) {
		std::vector<uint8_t> k( (const uint8_t *)&first.frameVals[f * FRAME_FLOATS + MDSC_CULL_FLOATS],
								(const uint8_t *)&first.frameVals[f * FRAME_FLOATS + FRAME_FLOATS] );
		k.insert( k.end(), (const uint8_t *)&first.model[(size_t)f * first.nb * BONE_SHORTS],
				  (const uint8_t *)&first.model[(size_t)( f + 1 ) * first.nb * BONE_SHORTS] );
		byPose[k].push_back( f );
	}
	std::vector<std::vector<int>> match( n );
	std::vector<bool> used( first.nf, false );
	for ( int m = 1; m < n; m++ ) {
		const MdsSrc &s = src[m];
		int last = -2;
		match[m].assign( s.nf, -1 );
		for ( int f = 0; f < s.nf; f++ ) {
			std::vector<uint8_t> k( (const uint8_t *)&s.frameVals[f * FRAME_FLOATS + MDSC_CULL_FLOATS],
									(const uint8_t *)&s.frameVals[f * FRAME_FLOATS + FRAME_FLOATS] );
			k.insert( k.end(), (const uint8_t *)&s.model[(size_t)f * s.nb * BONE_SHORTS],
					  (const uint8_t *)&s.model[(size_t)( f + 1 ) * s.nb * BONE_SHORTS] );
			auto it = byPose.find( k );
			if ( it == byPose.end() ) {
				last = -2;
				continue;
			}
			const std::vector<int> &c = it->second;
			int pick = std::find( c.begin(), c.end(), last + 1 ) != c.end() ? last + 1 : c[0];
			match[m][f] = last = pick;
			used[pick] = true;
		}
	}
	match[0].assign( first.nf, -1 );
	for ( int f = 0; f < first.nf; f++ ) {
		if ( used[f] ) {
			match[0][f] = f;
		}
	}

	/* the base: the first's frames any other has, in order */
	std::vector<int> shared, sharedAt( first.nf, -1 );
	for ( int f = 0; f < first.nf; f++ ) {
		if ( used[f] ) {
			sharedAt[f] = (int)shared.size();
			shared.push_back( f );
		}
	}
	if ( shared.empty() ) {
		fprintf( stderr, "%s: they share no frames\n", g.base.c_str() );
		return false;
	}
	{
		Out o;
		PutMesh( o, first, false, st, g.base.c_str() );
		size_t anim = PutAnim( o, first, shared, shared, opt, st );
		MdsHeader *oh = (MdsHeader *)o.b.data();
		oh->numFrames = (int32_t)shared.size();
		oh->ofsFrames = (int32_t)anim;
		oh->ofsEnd = (int32_t)o.pos();
		baseOut.swap( o.b );
		st.bytesOut += baseOut.size();
	}

	/* each: its mesh, which frames are the base's, its own */
	outs.resize( n );
	for ( int m = 0; m < n; m++ ) {
		const MdsSrc &s = src[m];
		const char *name = g.members[m].c_str();
		std::vector<int> own, all( s.nf );
		std::vector<mdscSegment_t> segs;

		for ( int f = 0; f < s.nf; f++ ) {
			int fromBase = match[m][f] >= 0;
			int at = fromBase ? sharedAt[match[m][f]] : (int)own.size();
			all[f] = f;
			if ( !fromBase ) {
				own.push_back( f );
			}
			if ( !segs.empty() && segs.back().fromBase == fromBase &&
				 segs.back().srcFirst + segs.back().count == at ) {
				segs.back().count++;
			} else {
				segs.push_back( { f, 1, at, fromBase } );
			}
		}
		if ( own.empty() ) {
			own.push_back( 0 );     /* (an mdscAnim_t needs a frame) */
		}

		Out o;
		PutMesh( o, s, true, st, name );
		o.align();
		size_t shareAt = o.pos();
		mdscShare_t share = {};
		snprintf( share.base, sizeof( share.base ), "%s", g.base.c_str() );
		share.numSegments = (int)segs.size();
		o.put( &share, sizeof( share ) );
		o.set32( shareAt + offsetof( mdscShare_t, ofsSegments ), (int32_t)( o.pos() - shareAt ) );
		o.put( segs.data(), segs.size() * sizeof( mdscSegment_t ) );
		size_t anim = PutAnim( o, s, own, all, opt, st );
		o.set32( shareAt + offsetof( mdscShare_t, ofsAnim ), (int32_t)( anim - shareAt ) );
		MdsHeader *oh = (MdsHeader *)o.b.data();
		oh->version = MDSC_VERSION_SHARED;
		oh->ofsFrames = (int32_t)shareAt;
		oh->ofsEnd = (int32_t)o.pos();

		if ( !CheckMdsc( o.b, baseOut.data(), s, st, name ) ) {
			return false;
		}
		int baseFrames = 0, runs = 0;
		for ( const mdscSegment_t &x : segs ) {
			baseFrames += x.fromBase ? x.count : 0;
			runs += x.fromBase;
		}
		printf( "mds: %s: %d of its %d frames in %s, %d runs of them\n", name, baseFrames, s.nf, g.base.c_str(), runs );
		st.files++;
		st.bytesIn += s.h.ofsEnd;
		st.bytesOut += o.pos();
		outs[m].swap( o.b );
	}
	return true;
}
