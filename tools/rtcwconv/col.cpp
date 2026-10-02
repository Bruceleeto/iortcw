/*
 * .bsp -> .col: the collision lumps, plus the curve collision made here
 * instead of at load, and the vis compressed; only the planes used and the
 * curves that can be hit (see SP/code/qcommon/colfile.h).
 */
#include <stdio.h>
#include <string.h>

#include "bspfile.h"
#include "colfile.h"
#include "col_glue.h"
#include "rtcwconv.h"

/* the .bsp lump each .col lump is copied from */
static const int colFromBsp[COL_LUMP_PATCHES] = {
	BSP_SHADERS, BSP_LEAFS, BSP_LEAFBRUSHES, BSP_LEAFSURFACES, BSP_PLANES, BSP_BRUSHSIDES,
	BSP_BRUSHES, BSP_MODELS, BSP_NODES, BSP_ENTITIES, BSP_VISIBILITY
};

static void PutInt( std::vector<uint8_t> &out, int32_t v ) {
	out.insert( out.end(), (uint8_t *)&v, (uint8_t *)&v + 4 );
}

template <class T> static void PutArray( std::vector<uint8_t> &out, const std::vector<T> &a ) {
	out.assign( (const uint8_t *)a.data(), (const uint8_t *)( a.data() + a.size() ) );
}

bool BspPlaneRemap( const std::vector<uint8_t> &bsp, std::vector<int32_t> &remap, int &numUsed ) {
	int numPlanes, numNodes, numSides;
	const BspPlane *planes = BspLumpArray<BspPlane>( bsp, BSP_PLANES, numPlanes );
	const BspNode *nodes = BspLumpArray<BspNode>( bsp, BSP_NODES, numNodes );
	const BspBrushSide *sides = BspLumpArray<BspBrushSide>( bsp, BSP_BRUSHSIDES, numSides );
	if ( !planes || !nodes || !sides ) {
		return false;
	}
	remap.assign( numPlanes, -1 );
	for ( int i = 0; i < numNodes; i++ ) {
		if ( nodes[i].planeNum < 0 || nodes[i].planeNum >= numPlanes ) {
			return false;
		}
		remap[nodes[i].planeNum] = 0;
	}
	for ( int i = 0; i < numSides; i++ ) {
		if ( sides[i].planeNum < 0 || sides[i].planeNum >= numPlanes ) {
			return false;
		}
		remap[sides[i].planeNum] = 0;
	}
	numUsed = 0;
	for ( int i = 0; i < numPlanes; i++ ) {
		if ( !remap[i] ) {
			remap[i] = numUsed++;
		}
	}
	return true;
}

/* the .bsp's vis (int numClusters, clusterBytes, then a row of clusterBytes
 * a cluster) with each row's runs of 0 bytes as a 0 and how many */
static bool CompressVis( const uint8_t *in, size_t len, std::vector<uint8_t> &out ) {
	int32_t numClusters, clusterBytes;
	if ( !len ) {
		out.clear();
		return true;
	}
	if ( len < 8 ) {
		return false;
	}
	memcpy( &numClusters, in, 4 );
	memcpy( &clusterBytes, in + 4, 4 );
	if ( numClusters < 0 || clusterBytes < 0 || 8 + (size_t)numClusters * clusterBytes > len ) {
		return false;
	}
	std::vector<uint8_t> rows;
	out.clear();
	PutInt( out, numClusters );
	PutInt( out, clusterBytes );
	for ( int c = 0; c < numClusters; c++ ) {
		const uint8_t *row = in + 8 + (size_t)c * clusterBytes;
		PutInt( out, rows.size() );
		for ( int i = 0; i < clusterBytes; ) {
			if ( row[i] ) {
				rows.push_back( row[i++] );
				continue;
			}
			int run = 0;
			while ( i < clusterBytes && !row[i] && run < 255 ) {
				i++;
				run++;
			}
			rows.push_back( 0 );
			rows.push_back( run );
		}
	}
	out.insert( out.end(), rows.begin(), rows.end() );
	return true;
}

bool ConvertCol( const std::vector<uint8_t> &bsp, std::vector<uint8_t> &out, ColStats &st, const char *name ) {
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
	const BspLump *lumps = (const BspLump *)( bsp.data() + 8 );

	int numShaders, numVerts, numSurfaces, numPlanes, numNodes, numSides, numLeafs, numLeafSurfaces, numModels;
	const BspShader *shaders = BspLumpArray<BspShader>( bsp, BSP_SHADERS, numShaders );
	const BspVert *verts = BspLumpArray<BspVert>( bsp, BSP_DRAWVERTS, numVerts );
	const BspSurface *surfaces = BspLumpArray<BspSurface>( bsp, BSP_SURFACES, numSurfaces );
	const BspPlane *planes = BspLumpArray<BspPlane>( bsp, BSP_PLANES, numPlanes );
	const BspNode *nodes = BspLumpArray<BspNode>( bsp, BSP_NODES, numNodes );
	const BspBrushSide *sides = BspLumpArray<BspBrushSide>( bsp, BSP_BRUSHSIDES, numSides );
	const BspLeaf *leafs = BspLumpArray<BspLeaf>( bsp, BSP_LEAFS, numLeafs );
	const int32_t *leafSurfaces = BspLumpArray<int32_t>( bsp, BSP_LEAFSURFACES, numLeafSurfaces );
	const BspModel *models = BspLumpArray<BspModel>( bsp, BSP_MODELS, numModels );
	std::vector<int32_t> planeRemap;
	int numPlanesUsed;
	if ( !shaders || !verts || !surfaces || !planes || !nodes || !sides || !leafs || !leafSurfaces || !models ||
		 !BspPlaneRemap( bsp, planeRemap, numPlanesUsed ) ) {
		fprintf( stderr, "%s: bad lumps\n", name );
		return false;
	}

	/* rewritten lumps, in place of the .bsp's */
	std::vector<uint8_t> rewritten[COL_LUMPS];
	bool isRewritten[COL_LUMPS] = {};

	/* the planes used, renumbered */
	{
		std::vector<BspPlane> outPlanes( numPlanesUsed );
		for ( int i = 0; i < numPlanes; i++ ) {
			if ( planeRemap[i] >= 0 ) {
				outPlanes[planeRemap[i]] = planes[i];
			}
		}
		std::vector<BspNode> outNodes( nodes, nodes + numNodes );
		for ( BspNode &n : outNodes ) {
			n.planeNum = planeRemap[n.planeNum];
		}
		std::vector<BspBrushSide> outSides( sides, sides + numSides );
		for ( BspBrushSide &s : outSides ) {
			s.planeNum = planeRemap[s.planeNum];
		}
		PutArray( rewritten[COL_LUMP_PLANES], outPlanes );
		PutArray( rewritten[COL_LUMP_NODES], outNodes );
		PutArray( rewritten[COL_LUMP_BRUSHSIDES], outSides );
		isRewritten[COL_LUMP_PLANES] = isRewritten[COL_LUMP_NODES] = isRewritten[COL_LUMP_BRUSHSIDES] = true;
		st.planesIn += numPlanes;
		st.planesOut += numPlanesUsed;
	}

	/* the surfaces kept: the curves that can be hit, numbered from 0 */
	std::vector<int32_t> surfRemap( numSurfaces, -1 );
	int numKept = 0;
	for ( int i = 0; i < numSurfaces; i++ ) {
		const BspSurface &s = surfaces[i];
		if ( s.surfaceType == MST_PATCH && s.shaderNum >= 0 && s.shaderNum < numShaders &&
			 shaders[s.shaderNum].contentFlags ) {
			surfRemap[i] = numKept++;
		}
	}
	{
		std::vector<int32_t> outLeafSurfaces;
		std::vector<BspLeaf> outLeafs( leafs, leafs + numLeafs );
		for ( BspLeaf &l : outLeafs ) {
			if ( l.firstLeafSurface < 0 || l.numLeafSurfaces < 0 || l.firstLeafSurface + l.numLeafSurfaces > numLeafSurfaces ) {
				fprintf( stderr, "%s: bad leaf\n", name );
				return false;
			}
			int first = outLeafSurfaces.size();
			for ( int k = 0; k < l.numLeafSurfaces; k++ ) {
				int s = leafSurfaces[l.firstLeafSurface + k];
				if ( s >= 0 && s < numSurfaces && surfRemap[s] >= 0 ) {
					outLeafSurfaces.push_back( surfRemap[s] );
				}
			}
			l.firstLeafSurface = first;
			l.numLeafSurfaces = outLeafSurfaces.size() - first;
		}
		/* a submodel's surfaces are a run, and so are the ones of them kept */
		std::vector<BspModel> outModels( models, models + numModels );
		for ( BspModel &m : outModels ) {
			if ( m.firstSurface < 0 || m.numSurfaces < 0 || m.firstSurface + m.numSurfaces > numSurfaces ) {
				fprintf( stderr, "%s: bad model\n", name );
				return false;
			}
			int first = numKept, num = 0;
			for ( int k = 0; k < m.numSurfaces; k++ ) {
				int n = surfRemap[m.firstSurface + k];
				if ( n >= 0 ) {
					first = num ? first : n;
					num++;
				}
			}
			m.firstSurface = first;
			m.numSurfaces = num;
		}
		st.leafSurfacesIn += numLeafSurfaces;
		st.leafSurfacesOut += outLeafSurfaces.size();
		PutArray( rewritten[COL_LUMP_LEAFSURFACES], outLeafSurfaces );
		PutArray( rewritten[COL_LUMP_LEAFS], outLeafs );
		PutArray( rewritten[COL_LUMP_MODELS], outModels );
		isRewritten[COL_LUMP_LEAFSURFACES] = isRewritten[COL_LUMP_LEAFS] = isRewritten[COL_LUMP_MODELS] = true;
	}

	/* the curve collision */
	std::vector<uint8_t> patches;
	int numPatches = 0;
	PutInt( patches, numKept );
	PutInt( patches, 0 );   /* numPatches, filled in below */
	for ( int i = 0; i < numSurfaces; i++ ) {
		const BspSurface &s = surfaces[i];
		if ( s.surfaceType != MST_PATCH ) {
			continue;
		}
		if ( s.shaderNum < 0 || s.shaderNum >= numShaders || s.firstVert < 0 ||
			 s.patchWidth <= 0 || s.patchHeight <= 0 || s.firstVert + s.patchWidth * s.patchHeight > numVerts ) {
			fprintf( stderr, "%s: bad patch %d\n", name, i );
			return false;
		}
		if ( surfRemap[i] < 0 ) {
			st.patchesSkipped++;
			continue;   /* nothing can hit it */
		}

		std::vector<float> xyz;
		for ( int v = 0; v < s.patchWidth * s.patchHeight; v++ ) {
			xyz.insert( xyz.end(), verts[s.firstVert + v].xyz, verts[s.firstVert + v].xyz + 3 );
		}
		unsigned char *pc;
		int size = ColGeneratePatch( s.patchWidth, s.patchHeight, xyz.data(), &pc );
		if ( size < 0 ) {
			fprintf( stderr, "%s: patch %d failed\n", name, i );
			return false;
		}
		PutInt( patches, surfRemap[i] );
		PutInt( patches, s.shaderNum );
		patches.insert( patches.end(), pc, pc + size );
		ColFreePatch();
		numPatches++;
	}
	memcpy( patches.data() + 4, &numPatches, 4 );

	std::vector<uint8_t> vis;
	{
		const BspLump &l = lumps[BSP_VISIBILITY];
		if ( l.ofs < 0 || l.len < 0 || (size_t)l.ofs + l.len > bsp.size() ||
			 !CompressVis( bsp.data() + l.ofs, l.len, vis ) ) {
			fprintf( stderr, "%s: bad vis\n", name );
			return false;
		}
	}

	/* the header, then the lumps */
	colHeader_t h = {};
	h.ident = COL_IDENT;
	h.version = COL_VERSION;
	out.assign( sizeof( h ), 0 );
	for ( int n = 0; n < COL_LUMPS; n++ ) {
		const uint8_t *p;
		size_t len;
		if ( isRewritten[n] ) {
			p = rewritten[n].data();
			len = rewritten[n].size();
		} else if ( n == COL_LUMP_PATCHES ) {
			p = patches.data();
			len = patches.size();
		} else if ( n == COL_LUMP_VISIBILITY ) {
			p = vis.data();
			len = vis.size();
		} else {
			const BspLump &l = lumps[colFromBsp[n]];
			if ( l.ofs < 0 || l.len < 0 || (size_t)l.ofs + l.len > bsp.size() ) {
				fprintf( stderr, "%s: bad lump %d\n", name, colFromBsp[n] );
				return false;
			}
			p = bsp.data() + l.ofs;
			len = l.len;
		}
		h.lumps[n].fileofs = out.size();
		h.lumps[n].filelen = len;
		out.insert( out.end(), p, p + len );
		while ( out.size() & 3 ) {
			out.push_back( 0 );
		}
	}
	memcpy( out.data(), &h, sizeof( h ) );

	st.files++;
	st.bytesIn += bsp.size();
	st.bytesOut += out.size();
	st.patches += numPatches;
	return true;
}
