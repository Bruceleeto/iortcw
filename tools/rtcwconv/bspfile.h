/* the .bsp as q3map writes it (SP/code/qcommon/qfiles.h), for col.cpp and
 * wld.cpp */
#ifndef BSPFILE_H
#define BSPFILE_H

#include <stdint.h>
#include <vector>

#define BSP_IDENT       ( ( 'P' << 24 ) + ( 'S' << 16 ) + ( 'B' << 8 ) + 'I' )
#define BSP_VERSION     47
#define BSP_LUMPS       17

enum {
	BSP_ENTITIES, BSP_SHADERS, BSP_PLANES, BSP_NODES, BSP_LEAFS, BSP_LEAFSURFACES, BSP_LEAFBRUSHES,
	BSP_MODELS, BSP_BRUSHES, BSP_BRUSHSIDES, BSP_DRAWVERTS, BSP_DRAWINDEXES, BSP_FOGS, BSP_SURFACES,
	BSP_LIGHTMAPS, BSP_LIGHTGRID, BSP_VISIBILITY
};

enum { MST_BAD, MST_PLANAR, MST_PATCH, MST_TRIANGLE_SOUP, MST_FLARE };

#define BSP_SURF_NODRAW 0x80

struct BspLump { int32_t ofs, len; };
struct BspShader { char name[64]; int32_t surfaceFlags, contentFlags; };
struct BspPlane { float normal[3], dist; };
struct BspNode { int32_t planeNum, children[2], mins[3], maxs[3]; };
struct BspLeaf {
	int32_t cluster, area, mins[3], maxs[3];
	int32_t firstLeafSurface, numLeafSurfaces, firstLeafBrush, numLeafBrushes;
};
struct BspModel { float mins[3], maxs[3]; int32_t firstSurface, numSurfaces, firstBrush, numBrushes; };
struct BspBrush { int32_t firstSide, numSides, shaderNum; };
struct BspBrushSide { int32_t planeNum, shaderNum; };
struct BspFog { char shader[64]; int32_t brushNum, visibleSide; };
struct BspVert { float xyz[3], st[2], lightmap[2], normal[3]; uint8_t color[4]; };
struct BspSurface {
	int32_t shaderNum, fogNum, surfaceType, firstVert, numVerts, firstIndex, numIndexes;
	int32_t lightmapNum, lightmapX, lightmapY, lightmapWidth, lightmapHeight;
	float lightmapOrigin[3], lightmapVecs[3][3];
	int32_t patchWidth, patchHeight;
};

/* lump n as an array of T; null if it's out of the file or not a whole
 * number of them */
template <class T> static const T *BspLumpArray( const std::vector<uint8_t> &bsp, int n, int &count ) {
	const BspLump &l = ( (const BspLump *)( bsp.data() + 8 ) )[n];
	if ( l.ofs < 0 || l.len < 0 || (size_t)l.ofs + l.len > bsp.size() || l.len % sizeof( T ) ) {
		return nullptr;
	}
	count = l.len / sizeof( T );
	return (const T *)( bsp.data() + l.ofs );
}

#endif
