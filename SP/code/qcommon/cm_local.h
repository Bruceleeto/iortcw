/*
===========================================================================

Return to Castle Wolfenstein single player GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company. 

This file is part of the Return to Castle Wolfenstein single player GPL Source Code (RTCW SP Source Code).  

RTCW SP Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTCW SP Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTCW SP Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the RTCW SP Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the RTCW SP Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/


#include "q_shared.h"
#include "qcommon.h"
#include "cm_polylib.h"


//	(SA) DM needs more than 256 since this includes func_static and func_explosives
//#define	MAX_SUBMODELS		256
//#define	BOX_MODEL_HANDLE	255

#define MAX_SUBMODELS           512
#define BOX_MODEL_HANDLE        511
#define CAPSULE_MODEL_HANDLE    510


typedef struct {
	cplane_t    *plane;
	int children[2];                // negative numbers are leafs
} cNode_t;

typedef struct {
	short cluster;                  // checked at load (CMod_LoadLeafs)
	short area;

	int firstLeafBrush;             // not a short: submodels' are their own blocks' distances from cm.leafbrushes
	int firstLeafSurface;

	unsigned short numLeafBrushes;
	unsigned short numLeafSurfaces; // CM_LEAF_NEARLADDER | the count (CM_LeafSurfaces)
} cLeaf_t;

// a ladder brush is within reach of this leaf (a .col's COL_LEAF_NEARLADDER;
// every leaf of a .bsp): CM_PointContents reports CONTENTS_NEARLADDER
#define CM_LEAF_NEARLADDER      0x8000
#define CM_LeafSurfaces( l )    ( ( l )->numLeafSurfaces & 0x7fff )

typedef struct cmodel_s {
	vec3_t mins, maxs;
	cLeaf_t leaf;               // submodels don't reference the main tree
} cmodel_t;

typedef struct {
	unsigned short planeNum;    // into cm.planes (CM_SidePlane)
	unsigned short shaderNum;   // whose surfaceFlags it has (CM_SideSurfaceFlags), or CM_NO_SHADER
} cbrushside_t;

#define CM_NO_SHADER    0xffff  // a box hull side's: no surfaceFlags
#define CM_SidePlane( s )           ( &cm.planes[( s )->planeNum] )
#define CM_SideSurfaceFlags( s )    ( ( s )->shaderNum == CM_NO_SHADER ? 0 : cm.shaders[( s )->shaderNum].surfaceFlags )

// 32 bytes: one SH4 cache line, and never written (the per-trace checkcount
// is in cm.brushChecks). A plain axial box (CM_BRUSH_AXIAL: 6 sides, in
// CM_BoundBrush's order -x +x -y +y -z +z) is traced from bounds alone,
// without reading its sides or planes.
typedef struct {
	int contents;               // its shader's
	vec3_t bounds[2];
	unsigned int sides;         // first side << CM_BRUSH_SIDES_SHIFT | CM_BRUSH_AXIAL | number of sides
} cbrush_t;

#define CM_BRUSH_SIDES_SHIFT    9
#define CM_BRUSH_AXIAL          0x100
#define CM_BRUSH_NUMSIDES_MASK  0xff
#define CM_BrushNumSides( b )   ( ( b )->sides & CM_BRUSH_NUMSIDES_MASK )
#define CM_BrushSides( b )      ( cm.brushsides + ( ( b )->sides >> CM_BRUSH_SIDES_SHIFT ) )
#define CM_BrushAxial( b )      ( ( b )->sides & CM_BRUSH_AXIAL )
#define CM_BrushSidesWord( first, num, axial )  ( ( (unsigned)( first ) << CM_BRUSH_SIDES_SHIFT ) | ( ( axial ) ? CM_BRUSH_AXIAL : 0 ) | ( num ) )


typedef struct {
	int checkcount;                     // to avoid repeated testings
	int surfaceFlags;
	int contents;
	struct patchCollide_s   *pc;
} cPatch_t;


typedef struct {
	int floodnum;
	int floodvalid;
} cArea_t;

// rows of a compressed vis kept unpacked: a caller's row stays good while
// fewer than this others are asked for
#define PVS_ROWS        8

typedef struct {
	char name[MAX_QPATH];

	int numShaders;
	dshader_t   *shaders;

	int numBrushSides;
	cbrushside_t *brushsides;

	int numPlanes;
	cplane_t    *planes;

	int numNodes;
	cNode_t     *nodes;

	int numLeafs;
	cLeaf_t     *leafs;

	int numLeafBrushes;
	unsigned short *leafbrushes;    // brushes and surfaces are under 64K (checked at load)

	int numLeafSurfaces;
	unsigned short *leafsurfaces;

	int numSubModels;
	cmodel_t    *cmodels;

	int numBrushes;
	cbrush_t    *brushes;
	int         *brushChecks;   // a brush's: cm.checkcount once this trace tested it

	int numClusters;
	int clusterBytes;
	byte        *visibility;
	qboolean vised;             // if false, visibility is just a single cluster of ffs
	int         *visOffsets;    // a .col's: each row's in visibility, compressed (colfile.h)
	int visLen;
	byte        *pvsRows;       // the last PVS_ROWS rows unpacked (CM_ClusterPVS)
	int pvsRowCluster[PVS_ROWS];
	int pvsNextRow;

	int numEntityChars;
	char        *entityString;  // NULL once freed (CM_FreeEntityString)
	char entityFile[MAX_QPATH];
	lump_t entityLump;

	int numAreas;
	cArea_t     *areas;
	int         *areaPortals;   // [ numAreas*numAreas ] reference counts

	int numSurfaces;
	cPatch_t    **surfaces;         // non-patches will be NULL

	int floodvalid;
	int checkcount;                         // incremented on each trace
} clipMap_t;


// keep 1/8 unit away to keep the position valid before network snapping
// and to avoid various numeric issues
#define SURFACE_CLIP_EPSILON    ( 0.125f )

extern clipMap_t cm;
extern int c_pointcontents;
extern int c_traces, c_brush_traces, c_patch_traces;
extern cvar_t      *cm_noAreas;
extern cvar_t      *cm_noCurves;
extern cvar_t      *cm_playerCurveClip;

// cm_test.c

// Used for oriented capsule collision detection
typedef struct
{
	qboolean use;
	float radius;
	float halfheight;
	vec3_t offset;
} sphere_t;

typedef struct {
	vec3_t start;
	vec3_t end;
	vec3_t size[2];         // size of the box being swept through the model
	vec3_t offsets[8];      // [signbits][x] = either size[0][x] or size[1][x]
	float maxOffset;        // longest corner length from origin
	vec3_t extents;         // greatest of abs(size[0]) and abs(size[1])
	vec3_t bounds[2];       // enclosing box of start and end surrounding by size
	vec3_t modelOrigin;     // origin of the model tracing through
	int contents;           // ored contents of the model tracing through
	qboolean isPoint;       // optimized case
	trace_t trace;          // returned from trace call
	sphere_t sphere;        // sphere for oriendted capsule collision
} traceWork_t;

typedef struct leafList_s {
	int count;
	int maxcount;
	qboolean overflowed;
	int     *list;
	vec3_t bounds[2];
	int lastLeaf;           // for overflows where each leaf can't be stored individually
	void ( *storeLeafs )( struct leafList_s *ll, int nodenum );
} leafList_t;


int CM_BoxBrushes( const vec3_t mins, const vec3_t maxs, cbrush_t **list, int listsize );

void CM_StoreLeafs( leafList_t *ll, int nodenum );
void CM_StoreBrushes( leafList_t *ll, int nodenum );

void CM_BoxLeafnums_r( leafList_t *ll, int nodenum );

cmodel_t    *CM_ClipHandleToModel( clipHandle_t handle );
qboolean CM_BoundsIntersect( const vec3_t mins, const vec3_t maxs, const vec3_t mins2, const vec3_t maxs2 );
qboolean CM_BoundsIntersectPoint( const vec3_t mins, const vec3_t maxs, const vec3_t point );

// cm_patch.c

struct patchCollide_s   *CM_GeneratePatchCollide( int width, int height, vec3_t *points );
void CM_TraceThroughPatchCollide( traceWork_t *tw, const struct patchCollide_s *pc );
qboolean CM_PositionTestInPatchCollide( traceWork_t *tw, const struct patchCollide_s *pc );
void CM_ClearLevelPatches( void );
