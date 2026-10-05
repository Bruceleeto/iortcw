/*
 * maps/<map>.col: what the collision code (cm_*) reads from a map, made
 * offline by tools/rtcwconv from the .bsp. CM_LoadMap loads it in place of
 * the .bsp when there is one.
 *
 * The lumps before COL_LUMP_VISIBILITY are the .bsp lumps of the same name,
 * but for only the planes used (by nodes and brush sides, renumbered; a
 * .wld's nodes use these) and only the surfaces that are curves that can be
 * hit (numbered from 0: leafsurfaces, and models' firstSurface/numSurfaces). COL_LUMP_VISIBILITY is the .bsp's compressed, a row
 * unpacked when wanted (CM_ClusterPVS):
 *
 *   int numClusters, clusterBytes
 *   int rowOffset[numClusters]  (from the end of these)
 *   the rows: a byte not 0 as it is; a 0 byte, then how many 0 bytes (1-255)
 *
 * or empty for no vis. COL_LUMP_PATCHES holds the curve collision
 * CM_GeneratePatchCollide makes, so the .bsp's vertexes and surfaces are
 * never read. All little endian, all 4 byte fields:
 *
 *   int numSurfaces            (those kept, which leafsurfaces index)
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
#define COL_VERSION     4

// set in a leaf's numLeafSurfaces when a ladder brush's bounds, grown by
// COL_LADDER_REACH, touch the leaf's: PM_CheckLadderMove traces only there
// (CM_PointContents reports it as CONTENTS_NEARLADDER). Version 4.
#define COL_LEAF_NEARLADDER 0x40000000
#define COL_LADDER_REACH    80      // the player's half width (18) + TRACE_LADDER_DIST (48) + slack

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
