/*
 * .bsp -> .wld: the map's surfaces made ready to draw, as DMS does its
 * models (see SP/code/renderer/wldfile.h): curves cut into triangles here,
 * 24 byte vertexes, 16 bit indexes, and the surfaces drawn alike in the same
 * leafs joined into one. The light grid keeps each different point once.
 */
#include <map>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <tuple>
#include <unordered_map>

#include "bspfile.h"
#include "rtcwconv.h"
#include "wld_glue.h"
#include "wldfile.h"

#define LIGHTMAP_BY_VERTEX  -3

/* a vertex before it's packed into a wldVert_t, which needs its surface */
struct Vert {
	float xyz[3], st[2], lightmap[2], normal[3];
	uint8_t color[4];
};

/* one .bsp surface's triangles */
struct Piece {
	std::vector<Vert> verts;
	std::vector<int> indexes;
	bool hasPlane;
	float plane[4];
};

static Vert MakeVert( const float *xyz, const float *st, const float *lightmap, const float *normal, const uint8_t *color ) {
	Vert v;
	memcpy( v.xyz, xyz, sizeof( v.xyz ) );
	memcpy( v.st, st, sizeof( v.st ) );
	memcpy( v.lightmap, lightmap, sizeof( v.lightmap ) );
	memcpy( v.normal, normal, sizeof( v.normal ) );
	memcpy( v.color, color, sizeof( v.color ) );
	return v;
}

/* the lat / long bytes R_WorldVertNormal reads back */
static void PackNormal( const float *normal, unsigned char out[2] ) {
	float len = sqrtf( normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2] );
	float z = len > 0 ? normal[2] / len : 1;
	out[0] = (unsigned char)( lrintf( atan2f( normal[1], normal[0] ) * ( 256 / ( 2 * M_PI ) ) ) & 255 );
	out[1] = (unsigned char)lrintf( acosf( z < -1 ? -1 : z > 1 ? 1 : z ) * ( 256 / ( 2 * M_PI ) ) );
}

/* as the renderer reads it back: R_WorldVertXyz, R_WorldVertSt, R_LatLongToNormal */
static wldVert_t PackVert( const Vert &in, const wldSurface_t &s ) {
	wldVert_t v = {};
	for ( int i = 0; i < 3; i++ ) {
		v.xyz[i] = (short)lrintf( ( in.xyz[i] - s.origin[i] ) / s.xyzStep );
	}
	PackNormal( in.normal, v.normal );
	for ( int i = 0; i < 2; i++ ) {
		v.st[i] = (short)lrintf( ( in.st[i] - s.stOrigin[i] ) / s.stStep );
	}
	memcpy( v.color, in.color, 4 );
	return v;
}

/* what makes two vertexes of a surface the same one: the xyz and st as
 * finely as they're kept, the normal as packed, the colour near enough */
static std::string WeldKey( const Vert &v ) {
	int32_t k[8];
	for ( int i = 0; i < 3; i++ ) {
		k[i] = lrintf( v.xyz[i] / WLD_XYZ_STEP );
	}
	for ( int i = 0; i < 2; i++ ) {
		k[3 + i] = lrintf( v.st[i] / WLD_ST_STEP );
	}
	unsigned char n[2];
	PackNormal( v.normal, n );
	k[5] = n[0] << 8 | n[1];
	k[6] = ( v.color[0] >> WLD_WELD_COLOR ) << 16 | ( v.color[1] >> WLD_WELD_COLOR ) << 8 | ( v.color[2] >> WLD_WELD_COLOR );
	k[7] = v.color[3];
	return std::string( (const char *)k, sizeof( k ) );
}

/* the surfaces being made, a group of pieces at a time; a vertex the open
 * surface has already is used again */
struct Builder {
	std::vector<wldSurface_t> surfaces;
	std::vector<Vert> verts;
	std::vector<uint16_t> indexes;
	bool open = false;
	float mins[5], maxs[5];     /* the open surface's xyz and st */
	std::unordered_map<std::string, int> have;  /* the open surface's vertexes */
	long welded = 0;

	wldSurface_t &Cur() { return surfaces.back(); }

	void Start( const wldSurface_t &proto ) {
		Close();
		wldSurface_t s = proto;
		s.firstVert = verts.size();
		s.firstIndex = indexes.size();
		s.numVerts = s.numIndexes = 0;
		surfaces.push_back( s );
		open = true;
		have.clear();
		for ( int i = 0; i < 5; i++ ) {
			mins[i] = 1e30f;
			maxs[i] = -1e30f;
		}
	}

	/* the finest step a range fits a short at, from its middle */
	static float StepFor( float lo, float hi, float step, float *origin ) {
		while ( ( hi - lo ) * 0.5f / step + 2 > 32767 ) {
			step *= 2;
		}
		*origin = roundf( ( lo + hi ) * 0.5f / step ) * step;
		return step;
	}

	/* bounds, steps, and the plane if every piece put in was on the same one */
	void Close() {
		if ( !open ) {
			return;
		}
		open = false;
		wldSurface_t &s = Cur();
		if ( !s.numIndexes ) {
			surfaces.pop_back();
			return;
		}
		float half = 0;
		for ( int i = 0; i < 3; i++ ) {
			half = fmaxf( half, ( maxs[i] - mins[i] ) * 0.5f );
		}
		s.xyzStep = WLD_XYZ_STEP;
		while ( half / s.xyzStep + 2 > 32767 ) {
			s.xyzStep *= 2;
		}
		for ( int i = 0; i < 3; i++ ) {
			s.bounds[0][i] = mins[i];
			s.bounds[1][i] = maxs[i];
			s.origin[i] = roundf( ( mins[i] + maxs[i] ) * 0.5f / s.xyzStep ) * s.xyzStep;
		}
		float o[2];
		s.stStep = fmaxf( StepFor( mins[3], maxs[3], WLD_ST_STEP, &o[0] ), StepFor( mins[4], maxs[4], WLD_ST_STEP, &o[1] ) );
		for ( int i = 0; i < 2; i++ ) {
			s.stOrigin[i] = roundf( ( mins[3 + i] + maxs[3 + i] ) * 0.5f / s.stStep ) * s.stStep;
		}
	}

	/* and still no wider than WLD_MAX_EXTENT, nor its st than WLD_MAX_ST,
	 * with bmins / bmaxs in it */
	bool Fits( int numVerts, int numIndexes, const float *bmins, const float *bmaxs ) {
		if ( Cur().numVerts + numVerts > WLD_MAX_VERTS || Cur().numIndexes + numIndexes > WLD_MAX_INDEXES ) {
			return false;
		}
		if ( !Cur().numVerts ) {
			return true;    /* on its own, whatever its size */
		}
		for ( int i = 0; i < 5; i++ ) {
			if ( fmaxf( maxs[i], bmaxs[i] ) - fminf( mins[i], bmins[i] ) > ( i < 3 ? WLD_MAX_EXTENT : WLD_MAX_ST ) ) {
				return false;
			}
		}
		return true;
	}

	void Grow( const float *bmins, const float *bmaxs ) {
		for ( int i = 0; i < 5; i++ ) {
			mins[i] = fminf( mins[i], bmins[i] );
			maxs[i] = fmaxf( maxs[i], bmaxs[i] );
		}
	}

	void PlaneOf( const Piece &p, bool first ) {
		wldSurface_t &s = Cur();
		if ( first ) {
			memcpy( s.plane, p.plane, sizeof( s.plane ) );
			if ( !p.hasPlane ) {
				memset( s.plane, 0, sizeof( s.plane ) );
			}
			return;
		}
		if ( !p.hasPlane || fabsf( s.plane[0] - p.plane[0] ) > 0.001f || fabsf( s.plane[1] - p.plane[1] ) > 0.001f ||
			 fabsf( s.plane[2] - p.plane[2] ) > 0.001f || fabsf( s.plane[3] - p.plane[3] ) > 0.1f ) {
			memset( s.plane, 0, sizeof( s.plane ) );
		}
	}

	/* the open surface's index for v, put in if it hasn't one like it */
	int Put( const Vert &v ) {
		wldSurface_t &s = Cur();
		auto it = have.emplace( WeldKey( v ), s.numVerts );
		if ( !it.second ) {
			welded++;
			return it.first->second;
		}
		verts.push_back( v );
		return s.numVerts++;
	}

	/* the piece as it is, when it fits; else a triangle at a time */
	void Add( const Piece &p, const wldSurface_t &proto ) {
		int nv = p.verts.size(), ni = p.indexes.size();
		float pmins[5] = { 1e30f, 1e30f, 1e30f, 1e30f, 1e30f }, pmaxs[5] = { -1e30f, -1e30f, -1e30f, -1e30f, -1e30f };
		for ( const Vert &v : p.verts ) {
			Grow2( pmins, pmaxs, v );
		}
		bool small = nv <= WLD_MAX_VERTS && ni <= WLD_MAX_INDEXES;
		for ( int i = 0; i < 5; i++ ) {
			small &= pmaxs[i] - pmins[i] <= ( i < 3 ? WLD_MAX_EXTENT : WLD_MAX_ST );
		}
		if ( !Fits( nv, ni, pmins, pmaxs ) && small ) {
			Start( proto );
		}
		if ( Fits( nv, ni, pmins, pmaxs ) ) {
			wldSurface_t &s = Cur();
			Grow( pmins, pmaxs );
			PlaneOf( p, !s.numIndexes );
			std::vector<int> remap( nv );
			for ( int i = 0; i < nv; i++ ) {
				remap[i] = Put( p.verts[i] );
			}
			for ( int i : p.indexes ) {
				indexes.push_back( remap[i] );
			}
			s.numIndexes += ni;
			return;
		}
		std::vector<int> remap( nv, -1 );
		for ( int t = 0; t < ni; t += 3 ) {
			int need = 0;
			float tmins[5] = { 1e30f, 1e30f, 1e30f, 1e30f, 1e30f }, tmaxs[5] = { -1e30f, -1e30f, -1e30f, -1e30f, -1e30f };
			for ( int k = 0; k < 3; k++ ) {
				need += remap[p.indexes[t + k]] < 0;
				Grow2( tmins, tmaxs, p.verts[p.indexes[t + k]] );
			}
			if ( !Fits( need, 3, tmins, tmaxs ) ) {
				Start( proto );
				std::fill( remap.begin(), remap.end(), -1 );
			}
			Grow( tmins, tmaxs );
			wldSurface_t &s = Cur();
			if ( !s.numIndexes ) {
				PlaneOf( p, true );
			}
			for ( int k = 0; k < 3; k++ ) {
				int &r = remap[p.indexes[t + k]];
				if ( r < 0 ) {
					r = Put( p.verts[p.indexes[t + k]] );
				}
				indexes.push_back( r );
				s.numIndexes++;
			}
		}
	}

	static void Grow2( float *bmins, float *bmaxs, const Vert &v ) {
		for ( int i = 0; i < 5; i++ ) {
			float x = i < 3 ? v.xyz[i] : v.st[i - 3];
			bmins[i] = fminf( bmins[i], x );
			bmaxs[i] = fmaxf( bmaxs[i], x );
		}
	}
};

static void PutLump( std::vector<uint8_t> &out, wldHeader_t &h, int n, const void *data, size_t len ) {
	h.lumps[n].fileofs = out.size();
	h.lumps[n].filelen = len;
	out.insert( out.end(), (const uint8_t *)data, (const uint8_t *)data + len );
	while ( out.size() & 3 ) {
		out.push_back( 0 );
	}
}

static bool CopyLump( const std::vector<uint8_t> &bsp, std::vector<uint8_t> &out, wldHeader_t &h, int n, int bspLump ) {
	int len;
	const uint8_t *p = BspLumpArray<uint8_t>( bsp, bspLump, len );
	if ( !p ) {
		return false;
	}
	PutLump( out, h, n, p, len );
	return true;
}

static bool Dropped( const char *shader, const std::vector<std::string> &drop ) {
	for ( const std::string &d : drop ) {
		if ( !strncasecmp( shader, d.c_str(), d.size() ) ) {
			return true;
		}
	}
	return false;
}

bool ConvertWld( const std::vector<uint8_t> &bsp, std::vector<uint8_t> &out, WldStats &st, const char *name, float subdivisions,
				 const std::vector<std::string> &drop ) {
	int32_t ident, version;
	if ( bsp.size() < 8 + BSP_LUMPS * sizeof( BspLump ) ) {
		fprintf( stderr, "%s: not a bsp\n", name );
		return false;
	}
	memcpy( &ident, bsp.data(), 4 );
	memcpy( &version, bsp.data() + 4, 4 );
	if ( ident != BSP_IDENT || version != BSP_VERSION ) {
		fprintf( stderr, "%s: not a version %d bsp\n", name, BSP_VERSION );
		return false;
	}

	int numShaders, numPlanes, numNodes, numLeafs, numLeafSurfaces, numModels, numBrushes, numSides;
	int numFogs, numVerts, numIndexes, numSurfaces;
	const BspShader *shaders = BspLumpArray<BspShader>( bsp, BSP_SHADERS, numShaders );
	const BspPlane *planes = BspLumpArray<BspPlane>( bsp, BSP_PLANES, numPlanes );
	const BspNode *nodes = BspLumpArray<BspNode>( bsp, BSP_NODES, numNodes );
	const BspLeaf *leafs = BspLumpArray<BspLeaf>( bsp, BSP_LEAFS, numLeafs );
	const int32_t *leafSurfaces = BspLumpArray<int32_t>( bsp, BSP_LEAFSURFACES, numLeafSurfaces );
	const BspModel *models = BspLumpArray<BspModel>( bsp, BSP_MODELS, numModels );
	const BspBrush *brushes = BspLumpArray<BspBrush>( bsp, BSP_BRUSHES, numBrushes );
	const BspBrushSide *sides = BspLumpArray<BspBrushSide>( bsp, BSP_BRUSHSIDES, numSides );
	const BspFog *fogs = BspLumpArray<BspFog>( bsp, BSP_FOGS, numFogs );
	const BspVert *verts = BspLumpArray<BspVert>( bsp, BSP_DRAWVERTS, numVerts );
	const int32_t *indexes = BspLumpArray<int32_t>( bsp, BSP_DRAWINDEXES, numIndexes );
	const BspSurface *surfaces = BspLumpArray<BspSurface>( bsp, BSP_SURFACES, numSurfaces );
	if ( !shaders || !planes || !nodes || !leafs || !leafSurfaces || !models || !brushes || !sides ||
		 !fogs || !verts || !indexes || !surfaces || !numModels ) {
		fprintf( stderr, "%s: bad lumps\n", name );
		return false;
	}

	/* each surface's triangles; curves cut (and stitched) all at once first */
	std::vector<Piece> pieces( numSurfaces );
	std::vector<int> kinds( numSurfaces, -1 );     /* -1: not drawn */
	std::vector<std::vector<WldPatchVert>> patchVerts;
	std::vector<int> patchSurfaces;
	for ( int i = 0; i < numSurfaces; i++ ) {
		const BspSurface &s = surfaces[i];
		if ( s.shaderNum < 0 || s.shaderNum >= numShaders || s.firstVert < 0 || s.numVerts < 0 ||
			 s.firstVert + s.numVerts > numVerts || s.firstIndex < 0 || s.numIndexes < 0 ||
			 s.firstIndex + s.numIndexes > numIndexes || s.numIndexes % 3 ) {
			fprintf( stderr, "%s: bad surface %d\n", name, i );
			return false;
		}
		Piece &p = pieces[i];
		p.hasPlane = false;
		if ( Dropped( shaders[s.shaderNum].name, drop ) ) {
			st.dropped++;
			continue;
		}
		switch ( s.surfaceType ) {
		case MST_PLANAR:
		case MST_TRIANGLE_SOUP:
			kinds[i] = s.surfaceType == MST_PLANAR ? WLD_PLANAR : WLD_TRIANGLES;
			for ( int v = 0; v < s.numVerts; v++ ) {
				const BspVert &bv = verts[s.firstVert + v];
				/* a face's normal is its plane's, as the renderer had it */
				p.verts.push_back( MakeVert( bv.xyz, bv.st, bv.lightmap,
					s.surfaceType == MST_PLANAR ? s.lightmapVecs[2] : bv.normal, bv.color ) );
			}
			for ( int k = 0; k < s.numIndexes; k++ ) {
				int idx = indexes[s.firstIndex + k];
				if ( idx < 0 || idx >= s.numVerts ) {
					fprintf( stderr, "%s: bad index in surface %d\n", name, i );
					return false;
				}
				p.indexes.push_back( idx );
			}
			if ( s.surfaceType == MST_PLANAR && s.numVerts ) {
				p.hasPlane = true;
				memcpy( p.plane, s.lightmapVecs[2], 3 * sizeof( float ) );
				p.plane[3] = p.verts[0].xyz[0] * p.plane[0] + p.verts[0].xyz[1] * p.plane[1] + p.verts[0].xyz[2] * p.plane[2];
			}
			break;
		case MST_PATCH:
			/* nodraw curves are only there to be collided with */
			if ( shaders[s.shaderNum].surfaceFlags & BSP_SURF_NODRAW ) {
				break;
			}
			if ( s.patchWidth <= 0 || s.patchHeight <= 0 || s.patchWidth * s.patchHeight > s.numVerts ) {
				fprintf( stderr, "%s: bad patch %d\n", name, i );
				return false;
			}
			kinds[i] = WLD_CURVE;
			patchSurfaces.push_back( i );
			patchVerts.emplace_back();
			for ( int v = 0; v < s.patchWidth * s.patchHeight; v++ ) {
				const BspVert &bv = verts[s.firstVert + v];
				WldPatchVert pv;
				memcpy( pv.xyz, bv.xyz, sizeof( pv.xyz ) );
				memcpy( pv.st, bv.st, sizeof( pv.st ) );
				memcpy( pv.lightmap, bv.lightmap, sizeof( pv.lightmap ) );
				memcpy( pv.normal, bv.normal, sizeof( pv.normal ) );
				memcpy( pv.color, bv.color, sizeof( pv.color ) );
				patchVerts.back().push_back( pv );
			}
			break;
		case MST_FLARE:
			kinds[i] = WLD_FLARE;
			break;
		default:
			fprintf( stderr, "%s: bad surface type in %d\n", name, i );
			return false;
		}
	}

	std::vector<WldPatch> patches;
	for ( size_t k = 0; k < patchSurfaces.size(); k++ ) {
		const BspSurface &s = surfaces[patchSurfaces[k]];
		patches.push_back( { s.patchWidth, s.patchHeight, patchVerts[k].data() } );
	}
	WldCutCurves( patches.size(), patches.data(), subdivisions );
	for ( size_t k = 0; k < patchSurfaces.size(); k++ ) {
		Piece &p = pieces[patchSurfaces[k]];
		int w, h;
		const WldPatchVert *gv;
		WldCurve( k, &w, &h, &gv );
		for ( int v = 0; v < w * h; v++ ) {
			p.verts.push_back( MakeVert( gv[v].xyz, gv[v].st, gv[v].lightmap, gv[v].normal, gv[v].color ) );
		}
		/* the triangles RB_SurfaceGrid made, every row and column */
		for ( int r = 0; r < h - 1; r++ ) {
			for ( int c = 0; c < w - 1; c++ ) {
				int v1 = r * w + c + 1, v2 = v1 - 1, v3 = v2 + w, v4 = v3 + 1;
				p.indexes.insert( p.indexes.end(), { v2, v3, v1, v1, v3, v4 } );
			}
		}
	}
	WldFreeCurves();

	/* faces and curves lit as Quake 2 on the Dreamcast does it: each vertex
	 * gets its lightmap's light where it sits, in place of the colour the map
	 * has for it (which q3map only works out for vertex lit surfaces), so the
	 * renderer draws them in one pass with no lightmaps (r_vertexLight) */
	{
		int lightLen;
		const uint8_t *light = BspLumpArray<uint8_t>( bsp, BSP_LIGHTMAPS, lightLen );
		const int size = 128, page = size * size * 3;
		int numPages = light ? lightLen / page : 0;
		for ( int i = 0; i < numSurfaces; i++ ) {
			const BspSurface &s = surfaces[i];
			if ( ( kinds[i] != WLD_PLANAR && kinds[i] != WLD_CURVE ) || s.lightmapNum < 0 || s.lightmapNum >= numPages ) {
				continue;
			}
			const uint8_t *lm = light + s.lightmapNum * page;
			for ( Vert &v : pieces[i].verts ) {
				/* bilinear, between the texel centres */
				float x = v.lightmap[0] * size - 0.5f, y = v.lightmap[1] * size - 0.5f;
				x = fminf( fmaxf( x, 0 ), size - 1 );
				y = fminf( fmaxf( y, 0 ), size - 1 );
				int x0 = (int)x, y0 = (int)y;
				int x1 = x0 + 1 < size ? x0 + 1 : x0, y1 = y0 + 1 < size ? y0 + 1 : y0;
				float fx = x - x0, fy = y - y0;
				for ( int c = 0; c < 3; c++ ) {
					float a = lm[( y0 * size + x0 ) * 3 + c] * ( 1 - fx ) + lm[( y0 * size + x1 ) * 3 + c] * fx;
					float b = lm[( y1 * size + x0 ) * 3 + c] * ( 1 - fx ) + lm[( y1 * size + x1 ) * 3 + c] * fx;
					v.color[c] = (uint8_t)lrintf( a * ( 1 - fy ) + b * fy );
				}
			}
		}
	}

	/* the leafs each surface is seen from */
	std::vector<std::vector<int>> leafsOf( numSurfaces );
	for ( int l = 0; l < numLeafs; l++ ) {
		const BspLeaf &leaf = leafs[l];
		if ( leaf.firstLeafSurface < 0 || leaf.numLeafSurfaces < 0 ||
			 leaf.firstLeafSurface + leaf.numLeafSurfaces > numLeafSurfaces ) {
			fprintf( stderr, "%s: bad leaf %d\n", name, l );
			return false;
		}
		for ( int k = 0; k < leaf.numLeafSurfaces; k++ ) {
			int s = leafSurfaces[leaf.firstLeafSurface + k];
			if ( s < 0 || s >= numSurfaces ) {
				fprintf( stderr, "%s: bad leaf surface in leaf %d\n", name, l );
				return false;
			}
			if ( leafsOf[s].empty() || leafsOf[s].back() != l ) {
				leafsOf[s].push_back( l );
			}
		}
	}

	/* a model at a time, the surfaces drawn alike in the same leafs joined
	 * (in the order the first of each comes in the .bsp) */
	Builder b;
	std::vector<BspModel> outModels( models, models + numModels );
	std::vector<std::pair<int, int>> groupOf( numSurfaces, { 0, 0 } );   /* first surface out, count */
	for ( int m = 0; m < numModels; m++ ) {
		const BspModel &model = models[m];
		if ( model.firstSurface < 0 || model.numSurfaces < 0 || model.firstSurface + model.numSurfaces > numSurfaces ) {
			fprintf( stderr, "%s: bad model %d\n", name, m );
			return false;
		}
		typedef std::tuple<int, int, int, int, std::vector<int>> Key;
		std::map<Key, std::vector<int>> groups;
		std::vector<Key> order;
		for ( int i = model.firstSurface; i < model.firstSurface + model.numSurfaces; i++ ) {
			const BspSurface &s = surfaces[i];
			/* all lit by vertex now, so which lightmap doesn't part them */
			int lightmapNum = LIGHTMAP_BY_VERTEX;
			if ( kinds[i] < 0 || ( kinds[i] != WLD_FLARE && pieces[i].indexes.empty() ) ) {
				continue;
			}
			/* flares are never joined */
			Key key( kinds[i], s.shaderNum, lightmapNum, s.fogNum, kinds[i] == WLD_FLARE ? std::vector<int>{ -1 - i } : leafsOf[i] );
			auto &g = groups[key];
			if ( g.empty() ) {
				order.push_back( key );
			}
			g.push_back( i );
		}

		outModels[m].firstSurface = b.surfaces.size();
		for ( const Key &key : order ) {
			const std::vector<int> &g = groups[key];
			wldSurface_t proto = {};
			proto.kind = std::get<0>( key );
			proto.shaderNum = std::get<1>( key );
			proto.lightmapNum = std::get<2>( key );
			proto.fogNum = std::get<3>( key );
			int first = b.surfaces.size();
			if ( proto.kind == WLD_FLARE ) {
				const BspSurface &s = surfaces[g[0]];
				b.Close();
				b.surfaces.push_back( proto );
				wldSurface_t &f = b.surfaces.back();
				f.firstVert = b.verts.size();
				f.firstIndex = b.indexes.size();
				memcpy( f.flare[0], s.lightmapOrigin, sizeof( f.flare[0] ) );
				memcpy( f.flare[1], s.lightmapVecs[0], sizeof( f.flare[1] ) );
				memcpy( f.flare[2], s.lightmapVecs[2], sizeof( f.flare[2] ) );
				memcpy( f.bounds[0], s.lightmapOrigin, sizeof( f.bounds[0] ) );
				memcpy( f.bounds[1], s.lightmapOrigin, sizeof( f.bounds[1] ) );
			} else {
				b.Start( proto );
				for ( int i : g ) {
					b.Add( pieces[i], proto );
				}
				b.Close();
			}
			for ( int i : g ) {
				groupOf[i] = { first, (int)b.surfaces.size() - first };
			}
		}
		outModels[m].numSurfaces = b.surfaces.size() - outModels[m].firstSurface;
	}
	b.Close();

	/* each leaf's surfaces, now the joined ones */
	std::vector<BspLeaf> outLeafs( leafs, leafs + numLeafs );
	std::vector<int32_t> outLeafSurfaces;
	std::vector<int> seen( b.surfaces.size(), -1 );
	for ( int l = 0; l < numLeafs; l++ ) {
		const BspLeaf &leaf = leafs[l];
		outLeafs[l].firstLeafSurface = outLeafSurfaces.size();
		for ( int k = 0; k < leaf.numLeafSurfaces; k++ ) {
			auto g = groupOf[leafSurfaces[leaf.firstLeafSurface + k]];
			for ( int s = g.first; s < g.first + g.second; s++ ) {
				if ( seen[s] != l ) {
					seen[s] = l;
					outLeafSurfaces.push_back( s );
				}
			}
		}
		outLeafs[l].numLeafSurfaces = outLeafSurfaces.size() - outLeafs[l].firstLeafSurface;
	}

	/* the fog brushes' boxes; brushes always have their axial sides first */
	std::vector<wldFog_t> outFogs( numFogs );
	for ( int i = 0; i < numFogs; i++ ) {
		wldFog_t &f = outFogs[i];
		int bn = fogs[i].brushNum;
		if ( bn < 0 || bn >= numBrushes || brushes[bn].firstSide < 0 || brushes[bn].firstSide + 6 > numSides ) {
			fprintf( stderr, "%s: bad fog %d\n", name, i );
			return false;
		}
		const BspBrushSide *bs = sides + brushes[bn].firstSide;
		for ( int k = 0; k < 6; k++ ) {
			if ( bs[k].planeNum < 0 || bs[k].planeNum >= numPlanes ) {
				fprintf( stderr, "%s: bad fog %d\n", name, i );
				return false;
			}
		}
		memcpy( f.shader, fogs[i].shader, sizeof( f.shader ) );
		f.shader[sizeof( f.shader ) - 1] = 0;
		for ( int k = 0; k < 3; k++ ) {
			f.bounds[0][k] = -planes[bs[k * 2].planeNum].dist;
			f.bounds[1][k] = planes[bs[k * 2 + 1].planeNum].dist;
		}
		int vs = fogs[i].visibleSide;
		f.hasSurface = vs != -1;
		if ( f.hasSurface ) {
			if ( vs < 0 || vs >= brushes[bn].numSides || brushes[bn].firstSide + vs >= numSides ||
				 bs[vs].planeNum < 0 || bs[vs].planeNum >= numPlanes ) {
				fprintf( stderr, "%s: bad fog %d\n", name, i );
				return false;
			}
			const BspPlane &pl = planes[bs[vs].planeNum];
			for ( int k = 0; k < 3; k++ ) {
				f.surface[k] = -pl.normal[k];
			}
			f.surface[3] = -pl.dist;
		}
	}

	std::vector<wldVert_t> outVerts( b.verts.size() );
	for ( const wldSurface_t &s : b.surfaces ) {
		for ( int v = s.firstVert; v < s.firstVert + s.numVerts; v++ ) {
			outVerts[v] = PackVert( b.verts[v], s );
		}
	}

	/* each different light grid point once, when that's under 65536 of
	 * them; else as it is */
	int gridLen;
	const uint8_t *grid = BspLumpArray<uint8_t>( bsp, BSP_LIGHTGRID, gridLen );
	if ( !grid || gridLen % 8 ) {
		fprintf( stderr, "%s: bad light grid\n", name );
		return false;
	}
	std::vector<uint8_t> outGrid( 4, 0 );
	{
		std::map<uint64_t, int> which;
		std::vector<uint64_t> points;
		std::vector<uint16_t> index;
		for ( int i = 0; i < gridLen; i += 8 ) {
			uint64_t pt;
			memcpy( &pt, grid + i, 8 );
			auto it = which.find( pt );
			if ( it == which.end() ) {
				it = which.emplace( pt, points.size() ).first;
				points.push_back( pt );
			}
			index.push_back( it->second );
		}
		int32_t n = points.size();
		if ( n <= 65536 && n * 8 + index.size() * 2 < (size_t)gridLen ) {
			memcpy( outGrid.data(), &n, 4 );
			outGrid.insert( outGrid.end(), (uint8_t *)points.data(), (uint8_t *)( points.data() + n ) );
			outGrid.insert( outGrid.end(), (uint8_t *)index.data(), (uint8_t *)( index.data() + index.size() ) );
		} else {
			outGrid.insert( outGrid.end(), grid, grid + gridLen );
		}
		st.gridIn += gridLen;
		st.gridOut += outGrid.size();
	}

	wldHeader_t h = {};
	h.ident = WLD_IDENT;
	h.version = WLD_VERSION;
	out.assign( sizeof( h ), 0 );
	if ( !CopyLump( bsp, out, h, WLD_LUMP_SHADERS, BSP_SHADERS ) ) {
		fprintf( stderr, "%s: bad lumps\n", name );
		return false;
	}
	PutLump( out, h, WLD_LUMP_LIGHTMAPS, NULL, 0 );     /* lit by vertex: none */
	PutLump( out, h, WLD_LUMP_FOGS, outFogs.data(), outFogs.size() * sizeof( outFogs[0] ) );
	PutLump( out, h, WLD_LUMP_SURFACES, b.surfaces.data(), b.surfaces.size() * sizeof( b.surfaces[0] ) );
	PutLump( out, h, WLD_LUMP_VERTS, outVerts.data(), outVerts.size() * sizeof( outVerts[0] ) );
	PutLump( out, h, WLD_LUMP_INDEXES, b.indexes.data(), b.indexes.size() * sizeof( b.indexes[0] ) );
	PutLump( out, h, WLD_LUMP_LEAFSURFACES, outLeafSurfaces.data(), outLeafSurfaces.size() * sizeof( outLeafSurfaces[0] ) );
	PutLump( out, h, WLD_LUMP_NODES, nodes, numNodes * sizeof( nodes[0] ) );
	PutLump( out, h, WLD_LUMP_LEAFS, outLeafs.data(), outLeafs.size() * sizeof( outLeafs[0] ) );
	PutLump( out, h, WLD_LUMP_MODELS, outModels.data(), outModels.size() * sizeof( outModels[0] ) );
	PutLump( out, h, WLD_LUMP_LIGHTGRID, outGrid.data(), outGrid.size() );
	memcpy( out.data(), &h, sizeof( h ) );

	st.files++;
	st.bytesIn += bsp.size();
	st.bytesOut += out.size();
	st.surfacesIn += numSurfaces;
	st.surfacesOut += b.surfaces.size();
	st.verts += b.verts.size();
	st.triangles += b.indexes.size() / 3;
	return true;
}
