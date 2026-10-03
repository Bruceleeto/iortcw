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

// cmodel.c -- model loading

#include "cm_local.h"
#include "cm_patch.h"
#include "colfile.h"

#ifdef BSPC

#include "../bspc/l_qfiles.h"

void SetPlaneSignbits( cplane_t *out ) {
	int bits, j;

	// for fast box on planeside test
	bits = 0;
	for ( j = 0 ; j < 3 ; j++ ) {
		if ( out->normal[j] < 0 ) {
			bits |= 1 << j;
		}
	}
	out->signbits = bits;
}
#endif //BSPC

// to allow boxes to be treated as brush models, we allocate
// some extra indexes along with those needed by the map
#define BOX_BRUSHES     1
#define BOX_SIDES       6
#define BOX_LEAFS       2
#define BOX_PLANES      12

#define LL( x ) x = LittleLong( x )


clipMap_t cm;
int c_pointcontents;
int c_traces, c_brush_traces, c_patch_traces;


#ifndef BSPC
cvar_t      *cm_noAreas;
cvar_t      *cm_noCurves;
cvar_t      *cm_playerCurveClip;
#endif

cmodel_t box_model;
cplane_t    *box_planes;
cbrush_t    *box_brush;



void    CM_InitBoxHull( void );
void    CM_FloodAreaConnections( void );


/*
===============================================================================

					MAP LOADING

===============================================================================
*/

/*
=================
CMod_LoadShaders
=================
*/
void CMod_LoadShaders( bspLump_t *l ) {
	dshader_t   *in, *out;
	int i, count;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "CMod_LoadShaders: funny lump size" );
	}
	count = l->len / sizeof( *in );

	if ( count < 1 ) {
		Com_Error( ERR_DROP, "Map with no shaders" );
	}
	cm.shaders = Hunk_Alloc( count * sizeof( *cm.shaders ), h_high );
	cm.numShaders = count;

	Com_Memcpy( cm.shaders, in, count * sizeof( *cm.shaders ) );

	if ( LittleLong( 1 ) != 1 ) {
		out = cm.shaders;
		for ( i = 0 ; i < count ; i++, in++, out++ ) {
			out->contentFlags = LittleLong( out->contentFlags );
			out->surfaceFlags = LittleLong( out->surfaceFlags );
		}
	}
}


/*
=================
CMod_LoadSubmodels
=================
*/
void CMod_LoadSubmodels( bspLump_t *l ) {
	dmodel_t    *in;
	cmodel_t    *out;
	int i, j, count;
	unsigned short *indexes;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "CMod_LoadSubmodels: funny lump size" );
	}
	count = l->len / sizeof( *in );

	if ( count < 1 ) {
		Com_Error( ERR_DROP, "Map with no models" );
	}
	cm.cmodels = Hunk_Alloc( count * sizeof( *cm.cmodels ), h_high );
	cm.numSubModels = count;

	if ( count > MAX_SUBMODELS ) {
		Com_Error( ERR_DROP, "MAX_SUBMODELS exceeded" );
	}

	for ( i=0 ; i<count ; i++, in++)
	{
		out = &cm.cmodels[i];

		for ( j = 0 ; j < 3 ; j++ )
		{   // spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat( in->mins[j] ) - 1;
			out->maxs[j] = LittleFloat( in->maxs[j] ) + 1;
		}

		if ( i == 0 ) {
			continue;   // world model doesn't need other info
		}

		// make a "leaf" just to hold the model's brushes and surfaces
		if ( (unsigned)LittleLong( in->numBrushes ) > 0xffff || (unsigned)LittleLong( in->numSurfaces ) > 0xffff
			 || (unsigned)( LittleLong( in->firstBrush ) + LittleLong( in->numBrushes ) ) > 0x10000
			 || (unsigned)( LittleLong( in->firstSurface ) + LittleLong( in->numSurfaces ) ) > 0x10000 ) {
			Com_Error( ERR_DROP, "CMod_LoadSubmodels: model %i out of range", i );
		}
		out->leaf.numLeafBrushes = LittleLong( in->numBrushes );
		indexes = Hunk_Alloc( out->leaf.numLeafBrushes * sizeof( *indexes ), h_high );
		out->leaf.firstLeafBrush = indexes - cm.leafbrushes;
		for ( j = 0 ; j < out->leaf.numLeafBrushes ; j++ ) {
			indexes[j] = LittleLong( in->firstBrush ) + j;
		}

		out->leaf.numLeafSurfaces = LittleLong( in->numSurfaces );
		indexes = Hunk_Alloc( out->leaf.numLeafSurfaces * sizeof( *indexes ), h_high );
		out->leaf.firstLeafSurface = indexes - cm.leafsurfaces;
		for ( j = 0 ; j < out->leaf.numLeafSurfaces ; j++ ) {
			indexes[j] = LittleLong( in->firstSurface ) + j;
		}
	}
}


/*
=================
CMod_LoadNodes

=================
*/
void CMod_LoadNodes( bspLump_t *l ) {
	dnode_t     *in;
	int child;
	cNode_t     *out;
	int i, j, count;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->len / sizeof( *in );

	if ( count < 1 ) {
		Com_Error( ERR_DROP, "Map has no nodes" );
	}
	cm.nodes = Hunk_Alloc( count * sizeof( *cm.nodes ), h_high );
	cm.numNodes = count;

	out = cm.nodes;

	for ( i = 0 ; i < count ; i++, out++, in++ )
	{
		out->plane = cm.planes + LittleLong( in->planeNum );
		for ( j = 0 ; j < 2 ; j++ )
		{
			child = LittleLong( in->children[j] );
			out->children[j] = child;
		}
	}

}

/*
=================
CM_BoundBrush

=================
*/
void CM_BoundBrush( cbrush_t *b ) {
	b->bounds[0][0] = -CM_SidePlane( &b->sides[0] )->dist;
	b->bounds[1][0] = CM_SidePlane( &b->sides[1] )->dist;

	b->bounds[0][1] = -CM_SidePlane( &b->sides[2] )->dist;
	b->bounds[1][1] = CM_SidePlane( &b->sides[3] )->dist;

	b->bounds[0][2] = -CM_SidePlane( &b->sides[4] )->dist;
	b->bounds[1][2] = CM_SidePlane( &b->sides[5] )->dist;
}


/*
=================
CMod_LoadBrushes

=================
*/
void CMod_LoadBrushes( bspLump_t *l ) {
	dbrush_t    *in;
	cbrush_t    *out;
	int i, count, shaderNum;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->len / sizeof( *in );
	// cm.leafbrushes are shorts, the box brush's too
	if ( BOX_BRUSHES + count > 0x10000 ) {
		Com_Error( ERR_DROP, "CMod_LoadBrushes: too many brushes (%i)", count );
	}

	cm.brushes = Hunk_Alloc( ( BOX_BRUSHES + count ) * sizeof( *cm.brushes ), h_high );
	cm.numBrushes = count;

	out = cm.brushes;

	for ( i = 0 ; i < count ; i++, out++, in++ ) {
		out->sides = cm.brushsides + LittleLong( in->firstSide );
		out->numsides = LittleLong( in->numSides );

		shaderNum = LittleLong( in->shaderNum );
		if ( shaderNum < 0 || shaderNum >= cm.numShaders ) {
			Com_Error( ERR_DROP, "CMod_LoadBrushes: bad shaderNum: %i", shaderNum );
		}
		out->contents = cm.shaders[shaderNum].contentFlags;

		CM_BoundBrush( out );
	}

}

/*
=================
CMod_LoadLeafs
=================
*/
void CMod_LoadLeafs( bspLump_t *l ) {
	int i;
	cLeaf_t     *out;
	dleaf_t     *in;
	int count;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->len / sizeof( *in );

	if ( count < 1 ) {
		Com_Error( ERR_DROP, "Map with no leafs" );
	}

	cm.leafs = Hunk_Alloc( ( BOX_LEAFS + count ) * sizeof( *cm.leafs ), h_high );
	cm.numLeafs = count;

	out = cm.leafs;
	for ( i = 0 ; i < count ; i++, in++, out++ )
	{
		int cluster = LittleLong( in->cluster ), area = LittleLong( in->area );
		int numBrushes = LittleLong( in->numLeafBrushes ), numSurfaces = LittleLong( in->numLeafSurfaces );

		if ( cluster < -1 || cluster > 0x7fff || area < -1 || area > 0x7fff
			 || (unsigned)numBrushes > 0xffff || (unsigned)numSurfaces > 0xffff ) {
			Com_Error( ERR_DROP, "CMod_LoadLeafs: leaf %i out of range", i );
		}
		out->cluster = cluster;
		out->area = area;
		out->firstLeafBrush = LittleLong( in->firstLeafBrush );
		out->numLeafBrushes = numBrushes;
		out->firstLeafSurface = LittleLong( in->firstLeafSurface );
		out->numLeafSurfaces = numSurfaces;

		if ( out->cluster >= cm.numClusters ) {
			cm.numClusters = out->cluster + 1;
		}
		if ( out->area >= cm.numAreas ) {
			cm.numAreas = out->area + 1;
		}
	}

	cm.areas = Hunk_Alloc( cm.numAreas * sizeof( *cm.areas ), h_high );
	cm.areaPortals = Hunk_Alloc( cm.numAreas * cm.numAreas * sizeof( *cm.areaPortals ), h_high );
}

/*
=================
CMod_LoadPlanes
=================
*/
void CMod_LoadPlanes( bspLump_t *l ) {
	int i, j;
	cplane_t    *out;
	dplane_t    *in;
	int count;
	int bits;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->len / sizeof( *in );

	if ( count < 1 ) {
		Com_Error( ERR_DROP, "Map with no planes" );
	}
	cm.planes = Hunk_Alloc( ( BOX_PLANES + count ) * sizeof( *cm.planes ), h_high );
	cm.numPlanes = count;

	out = cm.planes;

	for ( i = 0 ; i < count ; i++, in++, out++ )
	{
		bits = 0;
		for ( j = 0 ; j < 3 ; j++ )
		{
			out->normal[j] = LittleFloat( in->normal[j] );
			if ( out->normal[j] < 0 ) {
				bits |= 1 << j;
			}
		}

		out->dist = LittleFloat( in->dist );
		out->type = PlaneTypeForNormal( out->normal );
		out->signbits = bits;
	}
}

/*
=================
CMod_LoadLeafBrushes
=================
*/
void CMod_LoadLeafBrushes( bspLump_t *l ) {
	int i;
	unsigned short *out;
	int         *in;
	int count;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->len / sizeof( *in );

	// and the box brush's (CM_InitBoxHull)
	cm.leafbrushes = Hunk_Alloc( ( BOX_BRUSHES + count ) * sizeof( *cm.leafbrushes ), h_high );
	cm.numLeafBrushes = count;

	out = cm.leafbrushes;

	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		if ( (unsigned)LittleLong( *in ) > 0xffff ) {
			Com_Error( ERR_DROP, "CMod_LoadLeafBrushes: bad brush %i", LittleLong( *in ) );
		}
		*out = LittleLong( *in );
	}
}

/*
=================
CMod_LoadLeafSurfaces
=================
*/
void CMod_LoadLeafSurfaces( bspLump_t *l ) {
	int i;
	unsigned short *out;
	int         *in;
	int count;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->len / sizeof( *in );

	cm.leafsurfaces = Hunk_Alloc( count * sizeof( *cm.leafsurfaces ), h_high );
	cm.numLeafSurfaces = count;

	out = cm.leafsurfaces;

	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		if ( (unsigned)LittleLong( *in ) > 0xffff ) {
			Com_Error( ERR_DROP, "CMod_LoadLeafSurfaces: bad surface %i", LittleLong( *in ) );
		}
		*out = LittleLong( *in );
	}
}

/*
=================
CMod_LoadBrushSides
=================
*/
void CMod_LoadBrushSides( bspLump_t *l ) {
	int i;
	cbrushside_t    *out;
	dbrushside_t    *in;
	int count;
	int num, shaderNum;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->len / sizeof( *in );

	// their plane and shader numbers in shorts: the box hull's planes too
	if ( cm.numPlanes + BOX_PLANES > 0x10000 || cm.numShaders >= CM_NO_SHADER ) {
		Com_Error( ERR_DROP, "CMod_LoadBrushSides: %i planes, %i shaders: too many", cm.numPlanes, cm.numShaders );
	}
	cm.brushsides = Hunk_Alloc( ( BOX_SIDES + count ) * sizeof( *cm.brushsides ), h_high );
	cm.numBrushSides = count;

	out = cm.brushsides;

	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		num = LittleLong( in->planeNum );
		if ( num < 0 || num >= cm.numPlanes ) {
			Com_Error( ERR_DROP, "CMod_LoadBrushSides: bad planeNum: %i", num );
		}
		out->planeNum = num;
		shaderNum = LittleLong( in->shaderNum );
		if ( shaderNum < 0 || shaderNum >= cm.numShaders ) {
			Com_Error( ERR_DROP, "CMod_LoadBrushSides: bad shaderNum: %i", shaderNum );
		}
		out->shaderNum = shaderNum;
	}
}


/*
=================
CMod_LoadEntityString

Read only while a level loads, so freed after (CM_FreeEntityString) and read
again from the file if wanted again (CM_EntityString)
=================
*/
void CMod_LoadEntityString( fileHandle_t f, const char *fileName, const lump_t *l ) {
	Q_strncpyz( cm.entityFile, fileName, sizeof( cm.entityFile ) );
	cm.entityLump = *l;
	cm.entityString = Z_Malloc( l->filelen + 1 );
	cm.numEntityChars = l->filelen;
	CM_ReadLumpInto( f, l, cm.entityString );
}

/*
=================
CM_FreeEntityString
=================
*/
void CM_FreeEntityString( void ) {
	if ( cm.entityString ) {
		Z_Free( cm.entityString );
		cm.entityString = NULL;
	}
}

/*
=================
CMod_LoadVisibility
=================
*/
#define VIS_HEADER  8
void CMod_LoadVisibility( fileHandle_t f, const lump_t *l ) {
	int len;
	int header[2];

	len = l->filelen;
	if ( !len ) {
		cm.clusterBytes = ( cm.numClusters + 31 ) & ~31;
		cm.visibility = Hunk_Alloc( cm.clusterBytes, h_high );
		Com_Memset( cm.visibility, 255, cm.clusterBytes );
		return;
	}

	FS_Seek( f, l->fileofs, FS_SEEK_SET );
	FS_Read( header, sizeof( header ), f );
	cm.vised = qtrue;
	cm.numClusters = LittleLong( header[0] );
	cm.clusterBytes = LittleLong( header[1] );
	cm.visibility = Hunk_Alloc( len - VIS_HEADER, h_high );
	FS_Read( cm.visibility, len - VIS_HEADER, f );
}

/*
=================
CMod_LoadColVisibility

A .col's, compressed (colfile.h)
=================
*/
static void CMod_LoadColVisibility( fileHandle_t f, const lump_t *l ) {
	int header[2], i;

	if ( !l->filelen ) {
		CMod_LoadVisibility( f, l );
		return;
	}
	FS_Seek( f, l->fileofs, FS_SEEK_SET );
	if ( l->filelen < sizeof( header ) || FS_Read( header, sizeof( header ), f ) != sizeof( header ) ) {
		Com_Error( ERR_DROP, "CMod_LoadColVisibility: bad vis" );
	}
	cm.numClusters = LittleLong( header[0] );
	cm.clusterBytes = LittleLong( header[1] );
	if ( cm.numClusters <= 0 || cm.clusterBytes <= 0 || cm.clusterBytes * 8 < cm.numClusters ||
		 l->filelen < sizeof( header ) + cm.numClusters * 4 ) {
		Com_Error( ERR_DROP, "CMod_LoadColVisibility: bad vis" );
	}
	cm.vised = qtrue;
	cm.visOffsets = Hunk_Alloc( cm.numClusters * 4, h_high );
	FS_Read( cm.visOffsets, cm.numClusters * 4, f );
	cm.visLen = l->filelen - sizeof( header ) - cm.numClusters * 4;
	cm.visibility = Hunk_Alloc( cm.visLen, h_high );
	FS_Read( cm.visibility, cm.visLen, f );
	for ( i = 0 ; i < cm.numClusters ; i++ ) {
		cm.visOffsets[i] = LittleLong( cm.visOffsets[i] );
		if ( cm.visOffsets[i] < 0 || cm.visOffsets[i] >= cm.visLen ) {
			Com_Error( ERR_DROP, "CMod_LoadColVisibility: bad vis" );
		}
	}
	cm.pvsRows = Hunk_Alloc( PVS_ROWS * cm.clusterBytes, h_high );
	for ( i = 0 ; i < PVS_ROWS ; i++ ) {
		cm.pvsRowCluster[i] = -1;
	}
}

//==================================================================


/*
=================
CMod_LoadPatches
=================
*/
#define MAX_PATCH_VERTS     1024
void CMod_LoadPatches( bspLump_t *surfs, bspLump_t *verts ) {
	drawVert_t  *dv, *dv_p;
	dsurface_t  *in;
	int count;
	int i, j;
	int c;
	cPatch_t    *patch;
	vec3_t points[MAX_PATCH_VERTS];
	int width, height;
	int shaderNum;

	in = surfs->data;
	if ( surfs->len % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	cm.numSurfaces = count = surfs->len / sizeof( *in );
	cm.surfaces = Hunk_Alloc( cm.numSurfaces * sizeof( cm.surfaces[0] ), h_high );

	dv = verts->data;
	if ( verts->len % sizeof( *dv ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}

	// scan through all the surfaces, but only load patches,
	// not planar faces
	for ( i = 0 ; i < count ; i++, in++ ) {
		if ( LittleLong( in->surfaceType ) != MST_PATCH ) {
			continue;       // ignore other surfaces
		}
		// FIXME: check for non-colliding patches

		cm.surfaces[ i ] = patch = Hunk_Alloc( sizeof( *patch ), h_high );

		// load the full drawverts onto the stack
		width = LittleLong( in->patchWidth );
		height = LittleLong( in->patchHeight );
		c = width * height;
		if ( c > MAX_PATCH_VERTS ) {
			Com_Error( ERR_DROP, "ParseMesh: MAX_PATCH_VERTS" );
		}

		dv_p = dv + LittleLong( in->firstVert );
		for ( j = 0 ; j < c ; j++, dv_p++ ) {
			points[j][0] = LittleFloat( dv_p->xyz[0] );
			points[j][1] = LittleFloat( dv_p->xyz[1] );
			points[j][2] = LittleFloat( dv_p->xyz[2] );
		}

		shaderNum = LittleLong( in->shaderNum );
		patch->contents = cm.shaders[shaderNum].contentFlags;
		patch->surfaceFlags = cm.shaders[shaderNum].surfaceFlags;

		// create the internal facet structure
		patch->pc = CM_GeneratePatchCollide( width, height, points );
	}
}

//==================================================================


#if 0 //BSPC
/*
==================
CM_FreeMap

Free any loaded map and all submodels
==================
*/
void CM_FreeMap( void ) {
	Com_Memset( &cm, 0, sizeof( cm ) );
	Hunk_ClearHigh();
	CM_ClearLevelPatches();
}
#endif //BSPC

/*
==================
CM_OpenBsp

A .bsp is read a lump at a time (CM_ReadLump), never as a whole, and the
renderer reads the same way and shares what the collision map keeps
(CM_WorldPlanes, CM_WorldVis, CM_EntityString), so a level is in memory once.
==================
*/
fileHandle_t CM_OpenBsp( const char *name, dheader_t *header ) {
	fileHandle_t f;
	int i;

	if ( FS_FOpenFileRead( name, &f, qtrue ) <= 0 || !f ) {
		Com_Error( ERR_DROP, "Couldn't load %s", name );
	}
	if ( FS_Read( header, sizeof( *header ), f ) != sizeof( *header ) ) {
		FS_FCloseFile( f );
		Com_Error( ERR_DROP, "%s is too short", name );
	}
	for ( i = 0 ; i < sizeof( dheader_t ) / 4 ; i++ ) {
		( (int *)header )[i] = LittleLong( ( (int *)header )[i] );
	}
#ifndef _SKIP_BSP_CHECK
	if ( header->version != BSP_VERSION ) {
		FS_FCloseFile( f );
		Com_Error( ERR_DROP, "%s has wrong version number (%i should be %i)",
				   name, header->version, BSP_VERSION );
	}
#endif
	return f;
}

/*
==================
CM_ReadMapEntities

A map's entities, before it is loaded, from its .col or else its .bsp: in
temp memory, for Hunk_FreeTempMemory
==================
*/
char *CM_ReadMapEntities( const char *name ) {
	char colName[MAX_QPATH];
	fileHandle_t f;
	lump_t l;
	char *text;

	COM_StripExtension( name, colName, sizeof( colName ) );
	Q_strcat( colName, sizeof( colName ), ".col" );
	if ( FS_FOpenFileRead( colName, &f, qtrue ) > 0 && f ) {
		colHeader_t header;

		if ( FS_Read( &header, sizeof( header ), f ) != sizeof( header ) ||
			 LittleLong( header.ident ) != COL_IDENT || LittleLong( header.version ) != COL_VERSION ) {
			FS_FCloseFile( f );
			Com_Error( ERR_DROP, "%s isn't a version %i .col", colName, COL_VERSION );
		}
		l.fileofs = LittleLong( header.lumps[COL_LUMP_ENTITIES].fileofs );
		l.filelen = LittleLong( header.lumps[COL_LUMP_ENTITIES].filelen );
	} else {
		dheader_t header;

		f = CM_OpenBsp( name, &header );
		l = header.lumps[LUMP_ENTITIES];
	}
	text = Hunk_AllocateTempMemory( l.filelen + 1 );
	CM_ReadLumpInto( f, &l, text );
	text[l.filelen] = 0;
	FS_FCloseFile( f );
	return text;
}

/*
==================
CM_ReadLumpInto
==================
*/
void CM_ReadLumpInto( fileHandle_t f, const lump_t *l, void *dest ) {
	FS_Seek( f, l->fileofs, FS_SEEK_SET );
	if ( FS_Read( dest, l->filelen, f ) != l->filelen ) {
		Com_Error( ERR_DROP, "CM_ReadLumpInto: short read" );
	}
}

/*
==================
CM_ReadLump

Into temp memory (as a file), which CM_FreeLump gives back: the last read first
==================
*/
void CM_ReadLump( fileHandle_t f, const lump_t *l, bspLump_t *out ) {
	out->len = l->filelen;
	out->data = NULL;
	if ( out->len ) {
		out->data = FS_AllocFileMemory( out->len );
		CM_ReadLumpInto( f, l, out->data );
	}
}

/*
==================
CM_FreeLump
==================
*/
void CM_FreeLump( bspLump_t *l ) {
	if ( l->data ) {
		FS_FreeFile( l->data );
		l->data = NULL;
	}
}

static void CMod_LoadColLump( fileHandle_t f, const colHeader_t *header, int lump, void ( *load )( bspLump_t *l ) );

/*
=================
CMod_LoadColPatches

The curve collision rtcwconv made (see colfile.h)
=================
*/
static void CMod_LoadColPatches( bspLump_t *l ) {
	const int *in = l->data;
	const int *end = (const int *)( (const byte *)l->data + l->len );
	int i, j, k, numPatches, numBorders;

#define COL_INT()   ( in < end ? LittleLong( *in++ ) : ( Com_Error( ERR_DROP, "CMod_LoadColPatches: short lump" ), 0 ) )
#define COL_FLOAT() ( in < end ? LittleFloat( *(const float *)in++ ) : ( Com_Error( ERR_DROP, "CMod_LoadColPatches: short lump" ), 0.0f ) )

	cm.numSurfaces = COL_INT();
	numPatches = COL_INT();
	if ( cm.numSurfaces < 0 || numPatches < 0 || numPatches > cm.numSurfaces ) {
		Com_Error( ERR_DROP, "CMod_LoadColPatches: bad counts" );
	}
	cm.surfaces = Hunk_Alloc( cm.numSurfaces * sizeof( cm.surfaces[0] ), h_high );

	for ( i = 0 ; i < numPatches ; i++ ) {
		int surfaceNum = COL_INT();
		int shaderNum = COL_INT();
		cPatch_t *patch;
		patchCollide_t *pc;

		if ( surfaceNum < 0 || surfaceNum >= cm.numSurfaces || shaderNum < 0 || shaderNum >= cm.numShaders ) {
			Com_Error( ERR_DROP, "CMod_LoadColPatches: bad patch %i", i );
		}
		cm.surfaces[ surfaceNum ] = patch = Hunk_Alloc( sizeof( *patch ), h_high );
		patch->contents = cm.shaders[shaderNum].contentFlags;
		patch->surfaceFlags = cm.shaders[shaderNum].surfaceFlags;

		patch->pc = pc = Hunk_Alloc( sizeof( *pc ), h_high );
		for ( j = 0 ; j < 2 ; j++ ) {
			for ( k = 0 ; k < 3 ; k++ ) {
				pc->bounds[j][k] = COL_FLOAT();
			}
		}
		pc->numPlanes = COL_INT();
		pc->numFacets = COL_INT();
		if ( pc->numPlanes < 0 || pc->numPlanes > MAX_PATCH_PLANES || pc->numFacets < 0 || pc->numFacets > MAX_FACETS ) {
			Com_Error( ERR_DROP, "CMod_LoadColPatches: bad patch %i", i );
		}
		pc->planes = Hunk_Alloc( pc->numPlanes * sizeof( *pc->planes ), h_high );
		for ( j = 0 ; j < pc->numPlanes ; j++ ) {
			for ( k = 0 ; k < 4 ; k++ ) {
				pc->planes[j].plane[k] = COL_FLOAT();
			}
			pc->planes[j].signbits = COL_INT();
		}
		// the borders all the facets have, to keep just those (cFacet_t)
		{
			const int *facetsIn = in;

			numBorders = 0;
			for ( j = 0 ; j < pc->numFacets ; j++ ) {
				int n;

				COL_INT();
				n = COL_INT();
				if ( n < 0 || n > ARRAY_LEN( ( (facet_t *)0 )->borderPlanes ) || end - in < n * 3 ) {
					Com_Error( ERR_DROP, "CMod_LoadColPatches: bad facet in patch %i", i );
				}
				in += n * 3;
				numBorders += n;
			}
			in = facetsIn;
		}
		pc->facets = Hunk_Alloc( pc->numFacets * sizeof( *pc->facets ), h_high );
		pc->borders = Hunk_Alloc( numBorders * sizeof( *pc->borders ), h_high );
		numBorders = 0;
		for ( j = 0 ; j < pc->numFacets ; j++ ) {
			cFacet_t *f = &pc->facets[j];
			int surfacePlane = COL_INT();

			if ( surfacePlane < 0 || surfacePlane >= pc->numPlanes ) {
				Com_Error( ERR_DROP, "CMod_LoadColPatches: bad facet in patch %i", i );
			}
			f->surfacePlane = surfacePlane;
			f->numBorders = COL_INT();
			f->firstBorder = numBorders;
			for ( k = 0 ; k < f->numBorders ; k++ ) {
				int plane = COL_INT();
				int inward = COL_INT();
				int noAdjust = COL_INT();

				if ( plane < 0 || plane >= pc->numPlanes ) {
					Com_Error( ERR_DROP, "CMod_LoadColPatches: bad facet in patch %i", i );
				}
				pc->borders[numBorders++] = plane | ( inward ? BORDER_INWARD : 0 ) | ( noAdjust ? BORDER_NOADJUST : 0 );
			}
		}
	}
#undef COL_INT
#undef COL_FLOAT
}

/*
=================
CM_LoadCol

Loads the map's .col in place of the .bsp; qfalse if it has none
=================
*/
static qboolean CM_LoadCol( const char *name ) {
	char colName[MAX_QPATH];
	fileHandle_t f;
	colHeader_t header;
	bspLump_t patches;
	int i;

	COM_StripExtension( name, colName, sizeof( colName ) );
	Q_strcat( colName, sizeof( colName ), ".col" );
	if ( FS_FOpenFileRead( colName, &f, qtrue ) <= 0 || !f ) {
		if ( f ) {
			FS_FCloseFile( f );
		}
		return qfalse;
	}
	if ( FS_Read( &header, sizeof( header ), f ) != sizeof( header ) ) {
		FS_FCloseFile( f );
		Com_Error( ERR_DROP, "%s is too short", colName );
	}
	for ( i = 0 ; i < sizeof( header ) / 4 ; i++ ) {
		( (int *)&header )[i] = LittleLong( ( (int *)&header )[i] );
	}
	if ( header.ident != COL_IDENT || header.version != COL_VERSION ) {
		FS_FCloseFile( f );
		Com_Error( ERR_DROP, "%s isn't a version %i .col", colName, COL_VERSION );
	}

	CMod_LoadColLump( f, &header, COL_LUMP_SHADERS, CMod_LoadShaders );
	CMod_LoadColLump( f, &header, COL_LUMP_LEAFS, CMod_LoadLeafs );
	CMod_LoadColLump( f, &header, COL_LUMP_LEAFBRUSHES, CMod_LoadLeafBrushes );
	CMod_LoadColLump( f, &header, COL_LUMP_LEAFSURFACES, CMod_LoadLeafSurfaces );
	CMod_LoadColLump( f, &header, COL_LUMP_PLANES, CMod_LoadPlanes );
	CMod_LoadColLump( f, &header, COL_LUMP_BRUSHSIDES, CMod_LoadBrushSides );
	CMod_LoadColLump( f, &header, COL_LUMP_BRUSHES, CMod_LoadBrushes );
	CMod_LoadColLump( f, &header, COL_LUMP_MODELS, CMod_LoadSubmodels );
	CMod_LoadColLump( f, &header, COL_LUMP_NODES, CMod_LoadNodes );
	CMod_LoadEntityString( f, colName, (const lump_t *)&header.lumps[COL_LUMP_ENTITIES] );
	CMod_LoadColVisibility( f, (const lump_t *)&header.lumps[COL_LUMP_VISIBILITY] );

	CM_ReadLump( f, (const lump_t *)&header.lumps[COL_LUMP_PATCHES], &patches );
	CMod_LoadColPatches( &patches );
	CM_FreeLump( &patches );

	FS_FCloseFile( f );
	return qtrue;
}

/*
==================
CMod_LoadLump
==================
*/
static void CMod_LoadLump( fileHandle_t f, const dheader_t *header, int lump, void ( *load )( bspLump_t *l ) ) {
	bspLump_t l;

	CM_ReadLump( f, &header->lumps[lump], &l );
	load( &l );
	CM_FreeLump( &l );
}

static void CMod_LoadColLump( fileHandle_t f, const colHeader_t *header, int lump, void ( *load )( bspLump_t *l ) ) {
	bspLump_t l;

	CM_ReadLump( f, (const lump_t *)&header->lumps[lump], &l );
	load( &l );
	CM_FreeLump( &l );
}

/*
==================
CM_LoadMap

Loads in the map and all submodels
==================
*/
void CM_LoadMap( const char *name, qboolean clientload, int *checksum ) {
	fileHandle_t f;
	dheader_t header;
	bspLump_t surfs, verts;

	if ( !name || !name[0] ) {
		Com_Error( ERR_DROP, "CM_LoadMap: NULL name" );
	}

#ifndef BSPC
	cm_noAreas = Cvar_Get( "cm_noAreas", "0", CVAR_CHEAT );
	cm_noCurves = Cvar_Get( "cm_noCurves", "0", CVAR_CHEAT );
	cm_playerCurveClip = Cvar_Get( "cm_playerCurveClip", "1", CVAR_ARCHIVE | CVAR_CHEAT );
#endif
	Com_DPrintf( "CM_LoadMap( %s, %i )\n", name, clientload );

	if ( !strcmp( cm.name, name ) && clientload ) {
		*checksum = 0;
		return;
	}

	// free old stuff
	CM_FreeEntityString();
	Com_Memset( &cm, 0, sizeof( cm ) );
	CM_ClearLevelPatches();

	if ( !name[0] ) {
		cm.numLeafs = 1;
		cm.numClusters = 1;
		cm.numAreas = 1;
		cm.cmodels = Hunk_Alloc( sizeof( *cm.cmodels ), h_high );
		*checksum = 0;
		return;
	}

	// no whole file to checksum: the .aas is trusted to be made from this .bsp
	*checksum = 0;

	if ( CM_LoadCol( name ) ) {
		goto loaded;
	}

	f = CM_OpenBsp( name, &header );

	CMod_LoadLump( f, &header, LUMP_SHADERS, CMod_LoadShaders );
	CMod_LoadLump( f, &header, LUMP_LEAFS, CMod_LoadLeafs );
	CMod_LoadLump( f, &header, LUMP_LEAFBRUSHES, CMod_LoadLeafBrushes );
	CMod_LoadLump( f, &header, LUMP_LEAFSURFACES, CMod_LoadLeafSurfaces );
	CMod_LoadLump( f, &header, LUMP_PLANES, CMod_LoadPlanes );
	CMod_LoadLump( f, &header, LUMP_BRUSHSIDES, CMod_LoadBrushSides );
	CMod_LoadLump( f, &header, LUMP_BRUSHES, CMod_LoadBrushes );
	CMod_LoadLump( f, &header, LUMP_MODELS, CMod_LoadSubmodels );
	CMod_LoadLump( f, &header, LUMP_NODES, CMod_LoadNodes );
	CMod_LoadEntityString( f, name, &header.lumps[LUMP_ENTITIES] );
	CMod_LoadVisibility( f, &header.lumps[LUMP_VISIBILITY] );

	CM_ReadLump( f, &header.lumps[LUMP_DRAWVERTS], &verts );
	CM_ReadLump( f, &header.lumps[LUMP_SURFACES], &surfs );
	CMod_LoadPatches( &surfs, &verts );
	CM_FreeLump( &surfs );
	CM_FreeLump( &verts );

	FS_FCloseFile( f );

loaded:
	CM_InitBoxHull();

	CM_FloodAreaConnections();

	// allow this to be cached if it is loaded by the server
	if ( !clientload ) {
		Q_strncpyz( cm.name, name, sizeof( cm.name ) );
	}
}

/*
==================
CM_ClearMap
==================
*/
void CM_ClearMap( void ) {
	CM_FreeEntityString();
	Com_Memset( &cm, 0, sizeof( cm ) );
	CM_ClearLevelPatches();
}

/*
==================
CM_ClipHandleToModel
==================
*/
cmodel_t    *CM_ClipHandleToModel( clipHandle_t handle ) {
	if ( handle < 0 ) {
		Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i", handle );
	}
	if ( handle < cm.numSubModels ) {
		return &cm.cmodels[handle];
	}
	if ( handle == BOX_MODEL_HANDLE || handle == CAPSULE_MODEL_HANDLE ) {
		return &box_model;
	}
	if ( handle < MAX_SUBMODELS ) {
		Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i < %i < %i",
				   cm.numSubModels, handle, MAX_SUBMODELS );
	}
	Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i", handle + MAX_SUBMODELS );

	return NULL;

}

/*
==================
CM_InlineModel
==================
*/
clipHandle_t    CM_InlineModel( int index ) {
	if ( index < 0 || index >= cm.numSubModels ) {
		Com_Error( ERR_DROP, "CM_InlineModel: bad number" );
	}
	return index;
}

int     CM_NumClusters( void ) {
	return cm.numClusters;
}

int     CM_NumInlineModels( void ) {
	return cm.numSubModels;
}

char    *CM_EntityString( void ) {
	fileHandle_t f;

	if ( !cm.entityString && cm.entityFile[0] ) {
		// freed once the level loaded: from the file again
		if ( FS_FOpenFileRead( cm.entityFile, &f, qtrue ) <= 0 || !f ) {
			Com_Error( ERR_DROP, "CM_EntityString: can't open %s", cm.entityFile );
		}
		cm.entityString = Z_Malloc( cm.entityLump.filelen + 1 );
		CM_ReadLumpInto( f, &cm.entityLump, cm.entityString );
		FS_FCloseFile( f );
	}
	return cm.entityString;
}

int     CM_LeafCluster( int leafnum ) {
	if ( leafnum < 0 || leafnum >= cm.numLeafs ) {
		Com_Error( ERR_DROP, "CM_LeafCluster: bad number" );
	}
	return cm.leafs[leafnum].cluster;
}

int     CM_LeafArea( int leafnum ) {
	if ( leafnum < 0 || leafnum >= cm.numLeafs ) {
		Com_Error( ERR_DROP, "CM_LeafArea: bad number" );
	}
	return cm.leafs[leafnum].area;
}

//=======================================================================


/*
===================
CM_InitBoxHull

Set up the planes and nodes so that the six floats of a bounding box
can just be stored out and get a proper clipping hull structure.
===================
*/
void CM_InitBoxHull( void ) {
	int i;
	int side;
	cplane_t    *p;
	cbrushside_t    *s;

	box_planes = &cm.planes[cm.numPlanes];

	box_brush = &cm.brushes[cm.numBrushes];
	box_brush->numsides = 6;
	box_brush->sides = cm.brushsides + cm.numBrushSides;
	box_brush->contents = CONTENTS_BODY;

	box_model.leaf.numLeafBrushes = 1;
//	box_model.leaf.firstLeafBrush = cm.numBrushes;
	box_model.leaf.firstLeafBrush = cm.numLeafBrushes;
	cm.leafbrushes[cm.numLeafBrushes] = cm.numBrushes;

	for ( i = 0 ; i < 6 ; i++ )
	{
		side = i & 1;

		// brush sides
		s = &cm.brushsides[cm.numBrushSides + i];
		s->planeNum = cm.numPlanes + i * 2 + side;
		s->shaderNum = CM_NO_SHADER;

		// planes
		p = &box_planes[i * 2];
		p->type = i >> 1;
		p->signbits = 0;
		VectorClear( p->normal );
		p->normal[i >> 1] = 1;

		p = &box_planes[i * 2 + 1];
		p->type = 3 + ( i >> 1 );
		p->signbits = 0;
		VectorClear( p->normal );
		p->normal[i >> 1] = -1;

		SetPlaneSignbits( p );
	}
}

/*
===================
CM_TempBoxModel

To keep everything totally uniform, bounding boxes are turned into small
BSP trees instead of being compared directly.
Capsules are handled differently though.
===================
*/
clipHandle_t CM_TempBoxModel( const vec3_t mins, const vec3_t maxs, int capsule ) {

	VectorCopy( mins, box_model.mins );
	VectorCopy( maxs, box_model.maxs );

	box_planes[0].dist = maxs[0];
	box_planes[1].dist = -maxs[0];
	box_planes[2].dist = mins[0];
	box_planes[3].dist = -mins[0];
	box_planes[4].dist = maxs[1];
	box_planes[5].dist = -maxs[1];
	box_planes[6].dist = mins[1];
	box_planes[7].dist = -mins[1];
	box_planes[8].dist = maxs[2];
	box_planes[9].dist = -maxs[2];
	box_planes[10].dist = mins[2];
	box_planes[11].dist = -mins[2];

	VectorCopy( mins, box_brush->bounds[0] );
	VectorCopy( maxs, box_brush->bounds[1] );

	if ( capsule ) {
		return CAPSULE_MODEL_HANDLE;
	}

	return BOX_MODEL_HANDLE;
}

/*
===================
CM_ModelBounds
===================
*/
void CM_ModelBounds( clipHandle_t model, vec3_t mins, vec3_t maxs ) {
	cmodel_t    *cmod;

	cmod = CM_ClipHandleToModel( model );
	VectorCopy( cmod->mins, mins );
	VectorCopy( cmod->maxs, maxs );
}

/*
==================
CM_WorldPlanes

The renderer's planes too
==================
*/
cplane_t *CM_WorldPlanes( int *numPlanes ) {
	*numPlanes = cm.numPlanes;
	return cm.planes;
}

/*
==================
CM_WorldVis

The renderer's vis too (CM_ClusterPVS): qfalse if the map has none
==================
*/
qboolean CM_WorldVis( int *numClusters, int *clusterBytes ) {
	*numClusters = cm.numClusters;
	*clusterBytes = cm.clusterBytes;
	return cm.vised;
}
