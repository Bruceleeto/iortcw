/*
 * The lightmaps of a .bsp -> one .dt each. The renderer makes a texture of
 * each 128x128 lightmap after shifting it brighter (R_ColorShiftLightingBytes);
 * on the PVR it loads these, shifted here, instead.
 */
#include <stdio.h>
#include <string.h>

#include "rtcwconv.h"

#define BSP_IDENT           ( ( 'P' << 24 ) + ( 'S' << 16 ) + ( 'B' << 8 ) + 'I' )
#define BSP_VERSION         47
#define LUMP_LIGHTMAPS      14
#define LIGHTMAP_SIZE       128

static int ReadInt( const std::vector<uint8_t> &b, size_t ofs ) {
	return b[ofs] | ( b[ofs + 1] << 8 ) | ( b[ofs + 2] << 16 ) | ( b[ofs + 3] << 24 );
}

/* R_ColorShiftLightingBytes */
static void ColorShift( const uint8_t *in, uint8_t *out ) {
	int r = in[0] << LIGHTMAP_DT_SHIFT, g = in[1] << LIGHTMAP_DT_SHIFT, b = in[2] << LIGHTMAP_DT_SHIFT;

	// normalize by color instead of saturating to white
	if ( ( r | g | b ) > 255 ) {
		int max = r > g ? r : g;
		max = max > b ? max : b;
		r = r * 255 / max;
		g = g * 255 / max;
		b = b * 255 / max;
	}
	out[0] = r;
	out[1] = g;
	out[2] = b;
	out[3] = 255;
}

bool BspLightmapJobs( const std::vector<uint8_t> &bsp, const std::string &mapName,
					  const std::filesystem::path &outDir, std::vector<TexJob> &jobs ) {
	if ( bsp.size() < 8 + 17 * 8 || ReadInt( bsp, 0 ) != BSP_IDENT || ReadInt( bsp, 4 ) != BSP_VERSION ) {
		fprintf( stderr, "maps/%s.bsp: not a version %d bsp\n", mapName.c_str(), BSP_VERSION );
		return false;
	}
	size_t ofs = ReadInt( bsp, 8 + LUMP_LIGHTMAPS * 8 ), len = ReadInt( bsp, 12 + LUMP_LIGHTMAPS * 8 );
	if ( ofs + len > bsp.size() ) {
		fprintf( stderr, "maps/%s.bsp: bad lightmap lump\n", mapName.c_str() );
		return false;
	}

	const size_t size = LIGHTMAP_SIZE * LIGHTMAP_SIZE;
	for ( size_t i = 0; i < len / ( size * 3 ); i++ ) {
		char name[64];
		TexJob job;

		snprintf( name, sizeof( name ), "lm_%04zu.dt", i );
		job.rel = "maps/" + mapName + "/" + name;
		job.out = outDir / job.rel;
		job.w = job.h = LIGHTMAP_SIZE;
		job.noMip = true;
		job.rgba.resize( size * 4 );
		for ( size_t j = 0; j < size; j++ ) {
			ColorShift( &bsp[ofs + ( i * size + j ) * 3], &job.rgba[j * 4] );
		}
		jobs.push_back( std::move( job ) );
	}
	return true;
}
