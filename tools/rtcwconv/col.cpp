/*
 * .bsp -> .col: the collision lumps as they are, plus the curve collision
 * made here instead of at load (see SP/code/qcommon/colfile.h).
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

	int numShaders, numVerts, numSurfaces;
	const BspShader *shaders = BspLumpArray<BspShader>( bsp, BSP_SHADERS, numShaders );
	const BspVert *verts = BspLumpArray<BspVert>( bsp, BSP_DRAWVERTS, numVerts );
	const BspSurface *surfaces = BspLumpArray<BspSurface>( bsp, BSP_SURFACES, numSurfaces );
	if ( !shaders || !verts || !surfaces ) {
		fprintf( stderr, "%s: bad lumps\n", name );
		return false;
	}

	/* the curve collision */
	std::vector<uint8_t> patches;
	int numPatches = 0;
	PutInt( patches, numSurfaces );
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
		if ( !shaders[s.shaderNum].contentFlags ) {
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
		PutInt( patches, i );
		PutInt( patches, s.shaderNum );
		patches.insert( patches.end(), pc, pc + size );
		ColFreePatch();
		numPatches++;
	}
	memcpy( patches.data() + 4, &numPatches, 4 );

	/* the header, then the lumps */
	colHeader_t h = {};
	h.ident = COL_IDENT;
	h.version = COL_VERSION;
	out.assign( sizeof( h ), 0 );
	for ( int n = 0; n < COL_LUMPS; n++ ) {
		const uint8_t *p;
		size_t len;
		if ( n == COL_LUMP_PATCHES ) {
			p = patches.data();
			len = patches.size();
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
