/*
 * maps/<map>.col: what the collision code (cm_*) reads from a map, made
 * offline by tools/rtcwconv from the .bsp. CM_LoadMap loads it in place of
 * the .bsp when there is one.
 *
 * The lumps up to COL_LUMP_VISIBILITY are the .bsp lumps of the same name,
 * copied as they are. COL_LUMP_PATCHES holds the curve collision
 * CM_GeneratePatchCollide makes, so the .bsp's vertexes and surfaces are
 * never read. All little endian, all 4 byte fields:
 *
 *   int numSurfaces            (the .bsp's, which leafsurfaces index)
 *   int numPatches
 *   numPatches times:
 *     int surfaceNum, shaderNum
 *     float bounds[2][3]
 *     int numPlanes, numFacets
 *     numPlanes times:  float plane[4]; int signbits
 *     numFacets times:  int surfacePlane, numBorders
 *                       numBorders times: int plane, inward, noAdjust
 *
 * Patches whose shader has no contents can't be hit and aren't stored.
 */
#ifndef COLFILE_H
#define COLFILE_H

#define COL_IDENT       ( ( 'L' << 24 ) + ( 'O' << 16 ) + ( 'C' << 8 ) + 'R' )   // "RCOL"
#define COL_VERSION     1

enum {
	COL_LUMP_SHADERS,
	COL_LUMP_LEAFS,
	COL_LUMP_LEAFBRUSHES,
	COL_LUMP_LEAFSURFACES,
	COL_LUMP_PLANES,
	COL_LUMP_BRUSHSIDES,
	COL_LUMP_BRUSHES,
	COL_LUMP_MODELS,
	COL_LUMP_NODES,
	COL_LUMP_ENTITIES,
	COL_LUMP_VISIBILITY,
	COL_LUMP_PATCHES,
	COL_LUMPS
};

typedef struct {
	int ident;
	int version;
	struct {
		int fileofs, filelen;
	} lumps[COL_LUMPS];
} colHeader_t;

#endif
