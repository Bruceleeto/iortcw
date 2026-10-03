/*
 * maps/<map>.wld: what the renderer draws of a map, made offline by
 * tools/rtcwconv from the .bsp. RE_LoadWorldMap loads it in place of the
 * .bsp when there is one. Planes, visibility and the entity string are the
 * collision map's (cm_load.c), so they aren't here.
 *
 * The map's surfaces are made ready to draw, as DMS does its models: curves
 * cut into triangles here instead of at load, and the surfaces that are
 * drawn alike (same model, shader, lightmap, fog and kind, and seen from
 * the same leafs) joined into one, so a leaf holds a few big surfaces. All
 * little endian:
 *
 *   SHADERS       the .bsp's shader lump as it is (dshader_t)
 *   LIGHTMAPS     empty: the map is lit by vertex (r_vertexLight), each
 *                 vertex's colour its lightmap's light where it is
 *   FOGS          wldFog_t: the fog brush's box and fog surface, worked out
 *   SURFACES      wldSurface_t
 *   VERTS         wldVert_t, each surface's from its firstVert; one a
 *                 surface for all its vertexes alike
 *   INDEXES       unsigned short, each surface's numStripIndexes from its
 *                 firstIndex, counted from the surface's firstVert: its
 *                 triangles as strips, the first index of each with
 *                 WLD_STRIP_START. A strip a b c d e is the triangles
 *                 a b c, c b d, c d e, ... (every other one turned so they
 *                 all keep their winding), in that order
 *   LEAFSURFACES  int, into SURFACES
 *   NODES, LEAFS  the .bsp's, leafs pointing into the LEAFSURFACES here,
 *                 nodes into the .col's planes (CM_WorldPlanes)
 *   MODELS        the .bsp's, pointing into the SURFACES here
 *   LIGHTGRID     the .bsp's light grid (8 bytes a point) as at most 256
 *                 points, near ones made one: int numPoints, then numPoints
 *                 points, then a byte a grid point, which of them it is
 */
#ifndef WLDFILE_H
#define WLDFILE_H

#define WLD_IDENT       ( ( 'D' << 24 ) + ( 'L' << 16 ) + ( 'W' << 8 ) + 'R' )   // "RWLD"
#define WLD_VERSION     6

enum {
	WLD_LUMP_SHADERS,
	WLD_LUMP_LIGHTMAPS,
	WLD_LUMP_FOGS,
	WLD_LUMP_SURFACES,
	WLD_LUMP_VERTS,
	WLD_LUMP_INDEXES,
	WLD_LUMP_LEAFSURFACES,
	WLD_LUMP_NODES,
	WLD_LUMP_LEAFS,
	WLD_LUMP_MODELS,
	WLD_LUMP_LIGHTGRID,
	WLD_LUMPS
};

typedef struct {
	int ident;
	int version;
	struct {
		int fileofs, filelen;
	} lumps[WLD_LUMPS];
} wldHeader_t;

// 16 bytes
typedef struct {
	short xyz[3];                   // from the surface's origin, in its xyzStep units
	unsigned char normal[2];        // as an MD3 normal: around z, then down from z, 256 to a turn
	short st[2];                    // from the surface's stOrigin, in its stStep units
	unsigned char color[4];         // the light, from the lightmap
} wldVert_t;

// a surface's xyzStep: this, or more for one too big to fit a short so
#define WLD_XYZ_STEP        0.125f
// a surface's stStep: this, or more for st too far apart to fit a short so
#define WLD_ST_STEP         ( 1.0f / 2048 )
// how far apart a surface's st are at most, so it keeps that stStep (more
// than one piece's are, and it has its own step)
#define WLD_MAX_ST          30.0f
// vertexes alike but for their colour's low bits this many are one
#define WLD_WELD_COLOR      2
// how far across surfaces are joined into one at most, so they keep the
// finest xyzStep
#define WLD_MAX_EXTENT      8000.0f

// what a surface was made from, which says how marks (decals) go on it
enum {
	WLD_PLANAR,         // flat faces, maybe on more than one plane
	WLD_CURVE,          // curves, cut into triangles
	WLD_TRIANGLES,      // triangle soups (misc_models)
	WLD_FLARE
};

// a surface never has more than this, so it fits the shader's tess at once
#define WLD_MAX_VERTS       2048
#define WLD_MAX_INDEXES     ( 3 * 2048 )    // as triangles, 3 each

// on the first index of a strip (a surface's vertexes are fewer)
#define WLD_STRIP_START     0x8000

typedef struct {
	int kind;                       // WLD_*
	int shaderNum;
	int lightmapNum;                // LIGHTMAP_BY_VERTEX for triangles and flares
	int fogNum;                     // -1 for none, as in the .bsp
	int firstVert, numVerts;
	int firstIndex, numIndexes;     // numIndexes: 3 a triangle, as drawn
	int numStripIndexes;            // what's at firstIndex: its strips
	float bounds[2][3];
	float origin[3];                // what the xyz are from, on the xyzStep grid
	float xyzStep;                  // a power of two
	float stOrigin[2];              // what the st are from
	float stStep;                   // a power of two
	float plane[4];                 // when every triangle is on one plane, else 0 0 0 0
	float flare[3][3];              // WLD_FLARE: origin, color, normal
} wldSurface_t;

typedef struct {
	char shader[64];
	float bounds[2][3];
	int hasSurface;
	float surface[4];
} wldFog_t;

#endif
