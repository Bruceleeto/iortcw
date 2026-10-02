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

// tr_map.c

#include "tr_local.h"

/*

Loads and prepares a map file for scene rendering.

A single entry point:

void RE_LoadWorldMap( const char *name );

*/

static world_t s_worldData;

int c_subdivisions;
int c_gridVerts;

//===============================================================================

static void HSVtoRGB( float h, float s, float v, float rgb[3] ) {
	int i;
	float f;
	float p, q, t;

	h *= 5;

	i = floor( h );
	f = h - i;

	p = v * ( 1 - s );
	q = v * ( 1 - s * f );
	t = v * ( 1 - s * ( 1 - f ) );

	switch ( i )
	{
	case 0:
		rgb[0] = v;
		rgb[1] = t;
		rgb[2] = p;
		break;
	case 1:
		rgb[0] = q;
		rgb[1] = v;
		rgb[2] = p;
		break;
	case 2:
		rgb[0] = p;
		rgb[1] = v;
		rgb[2] = t;
		break;
	case 3:
		rgb[0] = p;
		rgb[1] = q;
		rgb[2] = v;
		break;
	case 4:
		rgb[0] = t;
		rgb[1] = p;
		rgb[2] = v;
		break;
	case 5:
		rgb[0] = v;
		rgb[1] = p;
		rgb[2] = q;
		break;
	}
}

/*
===============
R_ColorShiftLightingBytes

===============
*/
static void R_ColorShiftLightingBytes( byte in[4], byte out[4] ) {
	int shift, r, g, b;

	// shift the color data based on overbright range
	shift = r_mapOverBrightBits->integer - tr.overbrightBits;

	// shift the data based on overbright range
	r = in[0] << shift;
	g = in[1] << shift;
	b = in[2] << shift;

	// normalize by color instead of saturating to white
	if ( ( r | g | b ) > 255 ) {
		int max;

		max = r > g ? r : g;
		max = max > b ? max : b;
		r = r * 255 / max;
		g = g * 255 / max;
		b = b * 255 / max;
	}

	out[0] = r;
	out[1] = g;
	out[2] = b;
	out[3] = in[3];
}

/*
===============
R_LoadLightmaps

===============
*/
#define LIGHTMAP_SIZE   128
static void R_LoadLightmaps( fileHandle_t f, const lump_t *l ) {
	bspLump_t lump = { NULL, 0 };
	byte        *buf_p;
	int len;
	byte *image = NULL;     // 64K: off the stack, only when there's no .dt
	int i, j;
	float maxIntensity = 0;
	double sumIntensity = 0;

	len = l->filelen;
	if ( !len ) {
		return;
	}

	// we are about to upload textures
	R_IssuePendingRenderCommands();

	// create all the lightmaps
	tr.numLightmaps = len / ( LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3 );
	if ( tr.numLightmaps == 1 ) {
		//FIXME: HACK: maps with only one lightmap turn up fullbright for some reason.
		//this avoids this, but isn't the correct solution.
		tr.numLightmaps++;
	}

	// if we are in r_vertexLight mode, we don't need the lightmaps at all
	if ( r_vertexLight->integer || glConfig.hardwareType == GLHW_PERMEDIA2 ) {
		return;
	}

	tr.lightmaps = ri.Hunk_Alloc( tr.numLightmaps * sizeof(image_t *), h_low );
	for ( i = 0 ; i < tr.numLightmaps ; i++ ) {
#ifdef USE_PVR
		// the VQ copy `make assets` made, shifted as it would be here by default
		if ( r_lightmap->integer != 2 && r_mapOverBrightBits->integer - tr.overbrightBits == 2 ) {
			char name[MAX_QPATH], dtName[MAX_QPATH];

			Com_sprintf( name, sizeof( name ), "*lightmap%d", i );
			Com_sprintf( dtName, sizeof( dtName ), "maps/%s/lm_%04d.dt", s_worldData.baseName, i );
			tr.lightmaps[i] = R_CreateImageDT( name, dtName, IMGTYPE_COLORALPHA,
				IMGFLAG_NOLIGHTSCALE | IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE );
			if ( tr.lightmaps[i] ) {
				continue;
			}
		}
#endif
		// expand the 24 bit on-disk to 32 bit; the lump is only read for this
		if ( !lump.data ) {
			ri.CM_ReadLump( f, l, &lump );
			image = ri.Hunk_AllocateTempMemory( LIGHTMAP_SIZE * LIGHTMAP_SIZE * 4 );
		}
		buf_p = (byte *)lump.data + i * LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3;

		if ( r_lightmap->integer == 2 ) { // color code by intensity as development tool	(FIXME: check range)
			for ( j = 0; j < LIGHTMAP_SIZE * LIGHTMAP_SIZE; j++ )
			{
				float r = buf_p[j * 3 + 0];
				float g = buf_p[j * 3 + 1];
				float b = buf_p[j * 3 + 2];
				float intensity;
				float out[3] = {0.0, 0.0, 0.0};

				intensity = 0.33f * r + 0.685f * g + 0.063f * b;

				if ( intensity > 255 ) {
					intensity = 1.0f;
				} else {
					intensity /= 255.0f;
				}

				if ( intensity > maxIntensity ) {
					maxIntensity = intensity;
				}

				HSVtoRGB( intensity, 1.00, 0.50, out );

				image[j * 4 + 0] = out[0] * 255;
				image[j * 4 + 1] = out[1] * 255;
				image[j * 4 + 2] = out[2] * 255;
				image[j * 4 + 3] = 255;

				sumIntensity += intensity;
			}
		} else {
			for ( j = 0 ; j < LIGHTMAP_SIZE * LIGHTMAP_SIZE; j++ ) {
				R_ColorShiftLightingBytes( &buf_p[j * 3], &image[j * 4] );
				image[j * 4 + 3] = 255;
			}
		}
		tr.lightmaps[i] = R_CreateImage( va( "*lightmap%d",i ), image,
			LIGHTMAP_SIZE, LIGHTMAP_SIZE, IMGTYPE_COLORALPHA,
			IMGFLAG_NOLIGHTSCALE | IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, 0 );
	}
	if ( image ) {
		ri.Hunk_FreeTempMemory( image );
	}
	ri.CM_FreeLump( &lump );

	if ( r_lightmap->integer == 2 ) {
		ri.Printf( PRINT_ALL, "Brightest lightmap value: %d\n", ( int ) ( maxIntensity * 255 ) );
	}
}


/*
=================
RE_SetWorldVisData

This is called by the clipmodel subsystem so we can share the 1.8 megs of
space in big maps...
=================
*/
void        RE_SetWorldVisData( const byte *vis ) {
	tr.externalVisData = vis;
}


/*
=================
R_LoadVisibility
=================
*/
static void R_LoadVisibility( void ) {
	int len;

	len = ( s_worldData.numClusters + 63 ) & ~63;
	s_worldData.novis = ri.Hunk_Alloc( len, h_low );
	memset( s_worldData.novis, 0xff, len );

	// the collision map's: CM_LoadMap has always loaded this map first
	s_worldData.vis = ri.CM_WorldVis( &s_worldData.numClusters, &s_worldData.clusterBytes );
}

//===============================================================================


/*
===============
ShaderForShaderNum
===============
*/
static shader_t *ShaderForShaderNum( int shaderNum, int lightmapNum ) {
	shader_t    *shader;
	dshader_t   *dsh;

	int _shaderNum = LittleLong( shaderNum );
	if ( _shaderNum < 0 || _shaderNum >= s_worldData.numShaders ) {
		ri.Error( ERR_DROP, "ShaderForShaderNum: bad num %i", _shaderNum );
	}
	dsh = &s_worldData.shaders[ _shaderNum ];

	if ( r_vertexLight->integer || glConfig.hardwareType == GLHW_PERMEDIA2 ) {
		lightmapNum = LIGHTMAP_BY_VERTEX;
	}

	if ( r_fullbright->integer ) {
		lightmapNum = LIGHTMAP_WHITEIMAGE;
	}

	shader = R_FindShader( dsh->shader, lightmapNum, qtrue );

	// if the shader had errors, just use default shader
	if ( shader->defaultShader ) {
		return tr.defaultShader;
	}

	return shader;
}

// Ridah, optimizations here
// memory block for use by surfaces
static byte *surfHunkPtr;
static int surfHunkSize;
#define SURF_HUNK_MAXSIZE 0x40000
#define LL( x ) LittleLong( x )

/*
==============
R_InitSurfMemory
==============
*/
void R_InitSurfMemory( void ) {
	// allocate a new chunk
	surfHunkPtr = ri.Hunk_Alloc( SURF_HUNK_MAXSIZE, h_low );
	surfHunkSize = 0;
}

/*
==============
R_GetSurfMemory
==============
*/
void *R_GetSurfMemory( int size ) {
	byte *retval;

	// round to cacheline
	size = ( size + 31 ) & ~31;

	surfHunkSize += size;
	if ( surfHunkSize >= SURF_HUNK_MAXSIZE ) {
		// allocate a new chunk
		R_InitSurfMemory();
		surfHunkSize += size;   // since it just got reset
	}
	retval = surfHunkPtr;
	surfHunkPtr += size;

	return (void *)retval;
}

/*
===============
ParseFace
===============
*/
static void ParseFace( dsurface_t *ds, drawVert_t *verts, msurface_t *surf, int *indexes  ) {
	int i, j;
	srfSurfaceFace_t    *cv;
	int numPoints, numIndexes;
	int lightmapNum;
	int sfaceSize, ofsIndexes;

	lightmapNum = LittleLong( ds->lightmapNum );

	// get fog volume
	surf->fogIndex = LittleLong( ds->fogNum ) + 1;

	// get shader value
	surf->shader = ShaderForShaderNum( ds->shaderNum, lightmapNum );
	if ( r_singleShader->integer && !surf->shader->isSky ) {
		surf->shader = tr.defaultShader;
	}

	numPoints = LittleLong( ds->numVerts );
	if ( numPoints > MAX_FACE_POINTS ) {
		ri.Printf( PRINT_WARNING, "WARNING: MAX_FACE_POINTS exceeded: %i\n", numPoints );
		numPoints = MAX_FACE_POINTS;
		surf->shader = tr.defaultShader;
	}

	numIndexes = LittleLong( ds->numIndexes );

	// create the srfSurfaceFace_t
	sfaceSize = offsetof( srfSurfaceFace_t, points ) + sizeof( *cv->points ) * numPoints;
	ofsIndexes = sfaceSize;
	sfaceSize += sizeof( int ) * numIndexes;

	//cv = ri.Hunk_Alloc( sfaceSize );
	cv = R_GetSurfMemory( sfaceSize );

	cv->surfaceType = SF_FACE;
	cv->numPoints = numPoints;
	cv->numIndices = numIndexes;
	cv->ofsIndices = ofsIndexes;

	verts += LittleLong( ds->firstVert );
	for ( i = 0 ; i < numPoints ; i++ ) {
		for ( j = 0 ; j < 3 ; j++ ) {
			cv->points[i][j] = LittleFloat( verts[i].xyz[j] );
		}
		for ( j = 0 ; j < 2 ; j++ ) {
			cv->points[i][3 + j] = LittleFloat( verts[i].st[j] );
			cv->points[i][5 + j] = LittleFloat( verts[i].lightmap[j] );
		}
		R_ColorShiftLightingBytes( verts[i].color, (byte *)&cv->points[i][7] );
	}

	indexes += LittleLong( ds->firstIndex );
	for ( i = 0 ; i < numIndexes ; i++ ) {
		( ( int * )( (byte *)cv + cv->ofsIndices ) )[i] = LittleLong( indexes[ i ] );
	}

	// take the plane information from the lightmap vector
	for ( i = 0 ; i < 3 ; i++ ) {
		cv->plane.normal[i] = LittleFloat( ds->lightmapVecs[2][i] );
	}
	cv->plane.dist = DotProduct( cv->points[0], cv->plane.normal );
	SetPlaneSignbits( &cv->plane );
	cv->plane.type = PlaneTypeForNormal( cv->plane.normal );

	surf->data = (surfaceType_t *)cv;
}


/*
===============
ParseMesh
===============
*/
static void ParseMesh( dsurface_t *ds, drawVert_t *verts, msurface_t *surf ) {
	srfGridMesh_t   *grid;
	int i, j;
	int width, height, numPoints;
	drawVert_t *points;     // sized to the patch, off the stack
	int lightmapNum;
	vec3_t bounds[2];
	vec3_t tmpVec;
	static surfaceType_t skipData = SF_SKIP;

	lightmapNum = LittleLong( ds->lightmapNum );

	// get fog volume
	surf->fogIndex = LittleLong( ds->fogNum ) + 1;

	// get shader value
	surf->shader = ShaderForShaderNum( ds->shaderNum, lightmapNum );
	if ( r_singleShader->integer && !surf->shader->isSky ) {
		surf->shader = tr.defaultShader;
	}

	// we may have a nodraw surface, because they might still need to
	// be around for movement clipping
	if ( s_worldData.shaders[ LittleLong( ds->shaderNum ) ].surfaceFlags & SURF_NODRAW ) {
		surf->data = &skipData;
		return;
	}

	width = LittleLong( ds->patchWidth );
	height = LittleLong( ds->patchHeight );

	verts += LittleLong( ds->firstVert );
	numPoints = width * height;
	points = ri.Hunk_AllocateTempMemory( numPoints * sizeof( *points ) );
	for ( i = 0 ; i < numPoints ; i++ ) {
		for ( j = 0 ; j < 3 ; j++ ) {
			points[i].xyz[j] = LittleFloat( verts[i].xyz[j] );
			points[i].normal[j] = LittleFloat( verts[i].normal[j] );
		}
		for ( j = 0 ; j < 2 ; j++ ) {
			points[i].st[j] = LittleFloat( verts[i].st[j] );
			points[i].lightmap[j] = LittleFloat( verts[i].lightmap[j] );
		}
		R_ColorShiftLightingBytes( verts[i].color, points[i].color );
	}

	// pre-tesseleate
	grid = R_SubdividePatchToGrid( width, height, points );
	ri.Hunk_FreeTempMemory( points );
	surf->data = (surfaceType_t *)grid;

	// copy the level of detail origin, which is the center
	// of the group of all curves that must subdivide the same
	// to avoid cracking
	for ( i = 0 ; i < 3 ; i++ ) {
		bounds[0][i] = LittleFloat( ds->lightmapVecs[0][i] );
		bounds[1][i] = LittleFloat( ds->lightmapVecs[1][i] );
	}
	VectorAdd( bounds[0], bounds[1], bounds[1] );
	VectorScale( bounds[1], 0.5f, grid->lodOrigin );
	VectorSubtract( bounds[0], grid->lodOrigin, tmpVec );
	grid->lodRadius = VectorLength( tmpVec );
}

/*
===============
ParseTriSurf
===============
*/
static void ParseTriSurf( dsurface_t *ds, drawVert_t *verts, msurface_t *surf, int *indexes ) {
	srfTriangles_t  *tri;
	int i, j;
	int numVerts, numIndexes;

	// get fog volume
	surf->fogIndex = LittleLong( ds->fogNum ) + 1;

	// get shader
	surf->shader = ShaderForShaderNum( ds->shaderNum, LIGHTMAP_BY_VERTEX );
	if ( r_singleShader->integer && !surf->shader->isSky ) {
		surf->shader = tr.defaultShader;
	}

	numVerts = LittleLong( ds->numVerts );
	numIndexes = LittleLong( ds->numIndexes );

	//tri = ri.Hunk_Alloc( sizeof( *tri ) + numVerts * sizeof( tri->verts[0] )
	//	+ numIndexes * sizeof( tri->indexes[0] ) );
	tri = R_GetSurfMemory( sizeof( *tri ) + numVerts * sizeof( tri->verts[0] )
						   + numIndexes * sizeof( tri->indexes[0] ) );

	tri->surfaceType = SF_TRIANGLES;
	tri->numVerts = numVerts;
	tri->numIndexes = numIndexes;
	tri->verts = ( drawVert_t * )( tri + 1 );
	tri->indexes = ( int * )( tri->verts + tri->numVerts );

	surf->data = (surfaceType_t *)tri;

	// copy vertexes
	ClearBounds( tri->bounds[0], tri->bounds[1] );
	verts += LittleLong( ds->firstVert );
	for ( i = 0 ; i < numVerts ; i++ ) {
		for ( j = 0 ; j < 3 ; j++ ) {
			tri->verts[i].xyz[j] = LittleFloat( verts[i].xyz[j] );
			tri->verts[i].normal[j] = LittleFloat( verts[i].normal[j] );
		}
		AddPointToBounds( tri->verts[i].xyz, tri->bounds[0], tri->bounds[1] );
		for ( j = 0 ; j < 2 ; j++ ) {
			tri->verts[i].st[j] = LittleFloat( verts[i].st[j] );
			tri->verts[i].lightmap[j] = LittleFloat( verts[i].lightmap[j] );
		}

		R_ColorShiftLightingBytes( verts[i].color, tri->verts[i].color );
	}

	// copy indexes
	indexes += LittleLong( ds->firstIndex );
	for ( i = 0 ; i < numIndexes ; i++ ) {
		tri->indexes[i] = LittleLong( indexes[i] );
		if ( tri->indexes[i] < 0 || tri->indexes[i] >= numVerts ) {
			ri.Error( ERR_DROP, "Bad index in triangle surface" );
		}
	}
}

/*
===============
ParseFlare
===============
*/
static void ParseFlare( dsurface_t *ds, drawVert_t *verts, msurface_t *surf, int *indexes ) {
	srfFlare_t      *flare;
	int i;

	// get fog volume
	surf->fogIndex = LittleLong( ds->fogNum ) + 1;

	// get shader
	surf->shader = ShaderForShaderNum( ds->shaderNum, LIGHTMAP_BY_VERTEX );
	if ( r_singleShader->integer && !surf->shader->isSky ) {
		surf->shader = tr.defaultShader;
	}

	flare = ri.Hunk_Alloc( sizeof( *flare ), h_low );
	flare->surfaceType = SF_FLARE;

	surf->data = (surfaceType_t *)flare;

	for ( i = 0 ; i < 3 ; i++ ) {
		flare->origin[i] = LittleFloat( ds->lightmapOrigin[i] );
		flare->color[i] = LittleFloat( ds->lightmapVecs[0][i] );
		flare->normal[i] = LittleFloat( ds->lightmapVecs[2][i] );
	}
}



/*
===============
R_MovePatchSurfacesToHunk
===============
*/
void R_MovePatchSurfacesToHunk( void ) {
	int i, size;
	srfGridMesh_t *grid, *hunkgrid;

	for ( i = 0; i < s_worldData.numsurfaces; i++ ) {
		//
		grid = (srfGridMesh_t *) s_worldData.surfaces[i].data;
		// if this surface is not a grid
		if ( grid->surfaceType != SF_GRID ) {
			continue;
		}
		//
		size = ( grid->width * grid->height - 1 ) * sizeof( drawVert_t ) + sizeof( *grid );
		hunkgrid = ri.Hunk_Alloc( size, h_low );
		Com_Memcpy( hunkgrid, grid, size );

		hunkgrid->widthLodError = ri.Hunk_Alloc( grid->width * 4, h_low );
		Com_Memcpy( hunkgrid->widthLodError, grid->widthLodError, grid->width * 4 );

		hunkgrid->heightLodError = ri.Hunk_Alloc( grid->height * 4, h_low );
		Com_Memcpy( hunkgrid->heightLodError, grid->heightLodError, grid->height * 4 );

		R_FreeSurfaceGridMesh( grid );

		s_worldData.surfaces[i].data = (void *) hunkgrid;
	}
}

/*
===============
R_LoadSurfaces
===============
*/
static void R_LoadSurfaces( bspLump_t *surfs, bspLump_t *verts, bspLump_t *indexLump ) {
	dsurface_t  *in;
	msurface_t  *out;
	drawVert_t  *dv;
	int         *indexes;
	int count;
	int numFaces, numMeshes, numTriSurfs, numFlares;
	int i;

	numFaces = 0;
	numMeshes = 0;
	numTriSurfs = 0;
	numFlares = 0;

	in = surfs->data;
	if ( surfs->len % sizeof( *in ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	count = surfs->len / sizeof( *in );

	dv = verts->data;
	if ( verts->len % sizeof( *dv ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}

	indexes = indexLump->data;
	if ( indexLump->len % sizeof( *indexes ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}

	out = ri.Hunk_Alloc( count * sizeof( *out ), h_low );

	s_worldData.surfaces = out;
	s_worldData.numsurfaces = count;

	// Ridah, init the surface memory. This is optimization, so we don't have to
	// look for memory for each surface, we allocate a big block and just chew it up
	// as we go
	R_InitSurfMemory();

	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		switch ( LittleLong( in->surfaceType ) ) {
		case MST_PATCH:
			ParseMesh( in, dv, out );
			numMeshes++;
			break;
		case MST_TRIANGLE_SOUP:
			ParseTriSurf( in, dv, out, indexes );
			numTriSurfs++;
			break;
		case MST_PLANAR:
			ParseFace( in, dv, out, indexes );
			numFaces++;
			break;
		case MST_FLARE:
			ParseFlare( in, dv, out, indexes );
			numFlares++;
			break;
		default:
			ri.Error( ERR_DROP, "Bad surfaceType" );
		}
	}

	{
		surfaceType_t **surfs = ri.Hunk_AllocateTempMemory( count * sizeof( *surfs ) );

		for ( i = 0 ; i < count ; i++ ) {
			surfs[i] = s_worldData.surfaces[i].data;
		}
#ifdef PATCH_STITCHING
		R_StitchAllPatches( surfs, count );
#endif
		R_FixSharedVertexLodError( surfs, count );
		for ( i = 0 ; i < count ; i++ ) {
			s_worldData.surfaces[i].data = surfs[i];
		}
		ri.Hunk_FreeTempMemory( surfs );
	}

#ifdef PATCH_STITCHING
	R_MovePatchSurfacesToHunk();
#endif

	ri.Printf( PRINT_ALL, "...loaded %d faces, %i meshes, %i trisurfs, %i flares\n",
			   numFaces, numMeshes, numTriSurfs, numFlares );
}



/*
=================
R_LoadSubmodels
=================
*/
static void R_LoadSubmodels( bspLump_t *l ) {
	dmodel_t    *in;
	bmodel_t    *out;
	int i, j, count;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	count = l->len / sizeof( *in );

	s_worldData.bmodels = out = ri.Hunk_Alloc( count * sizeof( *out ), h_low );

	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		model_t *model;

		model = R_AllocModel();

		assert( model != NULL );            // this should never happen
		if ( model == NULL ) {
			ri.Error(ERR_DROP, "R_LoadSubmodels: R_AllocModel() failed");
		}

		model->type = MOD_BRUSH;
		model->bmodel = out;
		Com_sprintf( model->name, sizeof( model->name ), "*%d", i );

		for ( j = 0 ; j < 3 ; j++ ) {
			out->bounds[0][j] = LittleFloat( in->mins[j] );
			out->bounds[1][j] = LittleFloat( in->maxs[j] );
		}

		out->firstSurface = s_worldData.surfaces + LittleLong( in->firstSurface );
		out->numSurfaces = LittleLong( in->numSurfaces );
	}
}



//==================================================================

/*
=================
R_SetParent
=================
*/
static void R_SetParent( mnode_t *node, mnode_t *parent ) {
	node->parent = parent;
	if ( node->contents != -1 ) {
		return;
	}
	R_SetParent( node->children[0], node );
	R_SetParent( node->children[1], node );
}

/*
=================
R_LoadNodesAndLeafs
=================
*/
static void R_LoadNodesAndLeafs( bspLump_t *nodeLump, bspLump_t *leafLump ) {
	int i, j, p;
	dnode_t     *in;
	dleaf_t     *inLeaf;
	mnode_t     *out;
	int numNodes, numLeafs;

	in = nodeLump->data;
	if ( nodeLump->len % sizeof( dnode_t ) ||
		 leafLump->len % sizeof( dleaf_t ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	numNodes = nodeLump->len / sizeof( dnode_t );
	numLeafs = leafLump->len / sizeof( dleaf_t );

	out = ri.Hunk_Alloc( ( numNodes + numLeafs ) * sizeof( *out ), h_low );

	s_worldData.nodes = out;
	s_worldData.numnodes = numNodes + numLeafs;
	s_worldData.numDecisionNodes = numNodes;

	// load nodes
	for ( i = 0 ; i < numNodes; i++, in++, out++ )
	{
		for ( j = 0 ; j < 3 ; j++ )
		{
			out->mins[j] = LittleLong( in->mins[j] );
			out->maxs[j] = LittleLong( in->maxs[j] );
		}

		p = LittleLong( in->planeNum );
		out->plane = s_worldData.planes + p;

		out->contents = CONTENTS_NODE;  // differentiate from leafs

		for ( j = 0 ; j < 2 ; j++ )
		{
			p = LittleLong( in->children[j] );
			if ( p >= 0 ) {
				out->children[j] = s_worldData.nodes + p;
			} else {
				out->children[j] = s_worldData.nodes + numNodes + ( -1 - p );
			}
		}
	}

	// load leafs
	inLeaf = leafLump->data;
	for ( i = 0 ; i < numLeafs ; i++, inLeaf++, out++ )
	{
		for ( j = 0 ; j < 3 ; j++ )
		{
			out->mins[j] = LittleLong( inLeaf->mins[j] );
			out->maxs[j] = LittleLong( inLeaf->maxs[j] );
		}

		out->cluster = LittleLong( inLeaf->cluster );
		out->area = LittleLong( inLeaf->area );

		if ( out->cluster >= s_worldData.numClusters ) {
			s_worldData.numClusters = out->cluster + 1;
		}

		out->firstmarksurface = s_worldData.marksurfaces +
								LittleLong( inLeaf->firstLeafSurface );
		out->nummarksurfaces = LittleLong( inLeaf->numLeafSurfaces );
	}

	// chain decendants
	R_SetParent( s_worldData.nodes, NULL );
}

//=============================================================================

/*
=================
R_LoadShaders
=================
*/
static void R_LoadShaders( bspLump_t *l ) {
	int i, count;
	dshader_t   *in, *out;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	count = l->len / sizeof( *in );
	out = ri.Hunk_Alloc( count * sizeof( *out ), h_low );

	s_worldData.shaders = out;
	s_worldData.numShaders = count;

	memcpy( out, in, count * sizeof( *out ) );

	for ( i = 0 ; i < count ; i++ ) {
		out[i].surfaceFlags = LittleLong( out[i].surfaceFlags );
		out[i].contentFlags = LittleLong( out[i].contentFlags );
	}
}


/*
=================
R_LoadMarksurfaces
=================
*/
static void R_LoadMarksurfaces( bspLump_t *l ) {
	int i, j, count;
	int     *in;
	msurface_t **out;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	count = l->len / sizeof( *in );
	out = ri.Hunk_Alloc( count * sizeof( *out ), h_low );

	s_worldData.marksurfaces = out;
	s_worldData.nummarksurfaces = count;

	for ( i = 0 ; i < count ; i++ )
	{
		j = LittleLong( in[i] );
		out[i] = s_worldData.surfaces + j;
	}
}


/*
=================
R_LoadPlanes
=================
*/
static void R_LoadPlanes( void ) {
	s_worldData.planes = ri.CM_WorldPlanes( &s_worldData.numplanes );
}

/*
=================
R_FogParms

From the fog's shader
=================
*/
static void R_FogParms( fog_t *out, const char *shaderName ) {
	shader_t    *shader;
	float d;

	shader = R_FindShader( shaderName, LIGHTMAP_NONE, qtrue );

	out->parms = shader->fogParms;

	out->colorInt = ColorBytes4( shader->fogParms.color[0] * tr.identityLight,
								 shader->fogParms.color[1] * tr.identityLight,
								 shader->fogParms.color[2] * tr.identityLight, 1.0 );

	d = shader->fogParms.depthForOpaque < 1 ? 1 : shader->fogParms.depthForOpaque;
	out->tcScale = 1.0f / ( d * 8 );
}

/*
=================
R_LoadFogs

=================
*/
static void R_LoadFogs( bspLump_t *l, bspLump_t *brushesLump, bspLump_t *sidesLump ) {
	int i;
	fog_t       *out;
	dfog_t      *fogs;
	dbrush_t    *brushes, *brush;
	dbrushside_t    *sides;
	int count, brushesCount, sidesCount;
	int sideNum;
	int planeNum;
	int firstSide;

	fogs = l->data;
	if ( l->len % sizeof( *fogs ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	count = l->len / sizeof( *fogs );

	// create fog structures for them
	s_worldData.numfogs = count + 1;
	s_worldData.fogs = ri.Hunk_Alloc( s_worldData.numfogs * sizeof( *out ), h_low );
	out = s_worldData.fogs + 1;

	if ( !count ) {
		return;
	}

	brushes = brushesLump->data;
	if ( brushesLump->len % sizeof( *brushes ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	brushesCount = brushesLump->len / sizeof( *brushes );

	sides = sidesLump->data;
	if ( sidesLump->len % sizeof( *sides ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	sidesCount = sidesLump->len / sizeof( *sides );

	for ( i = 0 ; i < count ; i++, fogs++ ) {
		out->originalBrushNumber = LittleLong( fogs->brushNum );

		if ( (unsigned)out->originalBrushNumber >= brushesCount ) {
			ri.Error( ERR_DROP, "fog brushNumber out of range" );
		}
		brush = brushes + out->originalBrushNumber;

		firstSide = LittleLong( brush->firstSide );

		if ( (unsigned)firstSide > sidesCount - 6 ) {
			ri.Error( ERR_DROP, "fog brush sideNumber out of range" );
		}

		// brushes are always sorted with the axial sides first
		sideNum = firstSide + 0;
		planeNum = LittleLong( sides[ sideNum ].planeNum );
		out->bounds[0][0] = -s_worldData.planes[ planeNum ].dist;

		sideNum = firstSide + 1;
		planeNum = LittleLong( sides[ sideNum ].planeNum );
		out->bounds[1][0] = s_worldData.planes[ planeNum ].dist;

		sideNum = firstSide + 2;
		planeNum = LittleLong( sides[ sideNum ].planeNum );
		out->bounds[0][1] = -s_worldData.planes[ planeNum ].dist;

		sideNum = firstSide + 3;
		planeNum = LittleLong( sides[ sideNum ].planeNum );
		out->bounds[1][1] = s_worldData.planes[ planeNum ].dist;

		sideNum = firstSide + 4;
		planeNum = LittleLong( sides[ sideNum ].planeNum );
		out->bounds[0][2] = -s_worldData.planes[ planeNum ].dist;

		sideNum = firstSide + 5;
		planeNum = LittleLong( sides[ sideNum ].planeNum );
		out->bounds[1][2] = s_worldData.planes[ planeNum ].dist;

		R_FogParms( out, fogs->shader );

		// set the gradient vector
		sideNum = LittleLong( fogs->visibleSide );

		if ( sideNum == -1 ) {
			out->hasSurface = qfalse;
		} else {
			out->hasSurface = qtrue;
			planeNum = LittleLong( sides[ firstSide + sideNum ].planeNum );
			VectorSubtract( vec3_origin, s_worldData.planes[ planeNum ].normal, out->surface );
			out->surface[3] = -s_worldData.planes[ planeNum ].dist;
		}

		out++;
	}

}


/*
==============
R_FindLightGridBounds
==============
*/
void R_FindLightGridBounds( vec3_t mins, vec3_t maxs ) {
	world_t *w;
	msurface_t  *surf;
	srfSurfaceFace_t *surfFace;
//	cplane_t	*plane;
	struct shader_s     *shd;

	qboolean foundGridBrushes = qfalse;
	int i,j;

	w = &s_worldData;

//----(SA)	temp - disable this whole thing for now
	VectorCopy( w->bmodels[0].bounds[0], mins );
	VectorCopy( w->bmodels[0].bounds[1], maxs );
	return;
//----(SA)	temp




	ClearBounds( mins, maxs );

// wrong!
	for ( i = 0; i < w->bmodels[0].numSurfaces; i++ ) {
		surf = w->bmodels[0].firstSurface + i;
		shd = surf->shader;

		if ( !( *surf->data == SF_FACE ) ) {
			continue;
		}

		if ( !( shd->contentFlags & CONTENTS_LIGHTGRID ) ) {
			continue;
		}

		foundGridBrushes = qtrue;
	}


// wrong!
	for ( i = 0; i < w->numsurfaces; i++ ) {
		surf = &w->surfaces[i];
		shd = surf->shader;
		if ( !( *surf->data == SF_FACE ) ) {
			continue;
		}

		if ( !( shd->contentFlags & CONTENTS_LIGHTGRID ) ) {
			continue;
		}

		foundGridBrushes = qtrue;

		surfFace = ( srfSurfaceFace_t * )surf->data;

		for ( j = 0; j < surfFace->numPoints; j++ ) {
			AddPointToBounds( surfFace->points[j], mins, maxs );
		}

	}

	// go through brushes looking for lightgrid
//	for ( i = 0 ; i < numbrushes ; i++ ) {
//		db = &dbrushes[i];
//
//		if (!(dshaders[db->shaderNum].contentFlags & CONTENTS_LIGHTGRID)) {
//			continue;
//		}
//
//		foundGridBrushes = qtrue;
//
//		// go through light grid surfaces for bounds
//		for ( j = 0 ; j < db->numSides ; j++ ) {
//			s = &dbrushsides[ db->firstSide + j ];
//
//			surfmin[0] = -dplanes[ dbrushsides[ db->firstSide + 0 ].planeNum ].dist - 1;
//			surfmin[1] = -dplanes[ dbrushsides[ db->firstSide + 2 ].planeNum ].dist - 1;
//			surfmin[2] = -dplanes[ dbrushsides[ db->firstSide + 4 ].planeNum ].dist - 1;
//			surfmax[0] = dplanes[ dbrushsides[ db->firstSide + 1 ].planeNum ].dist + 1;
//			surfmax[1] = dplanes[ dbrushsides[ db->firstSide + 3 ].planeNum ].dist + 1;
//			surfmax[2] = dplanes[ dbrushsides[ db->firstSide + 5 ].planeNum ].dist + 1;
//			AddPointToBounds (surfmin, mins, maxs);
//			AddPointToBounds (surfmax, mins, maxs);
//		}
//	}


//----(SA)	temp
	foundGridBrushes = qfalse;  // disable this whole thing for now
//----(SA)	temp

	if ( !foundGridBrushes ) {
		VectorCopy( w->bmodels[0].bounds[0], mins );
		VectorCopy( w->bmodels[0].bounds[1], maxs );
	}
}

/*
================
R_LightGridSize

Where the grid is and how many points it has
================
*/
static int R_LightGridSize( void ) {
	int i;
	vec3_t maxs;
	world_t *w;
//	float	*wMins, *wMaxs;
	vec3_t wMins, wMaxs;

	w = &s_worldData;

	w->lightGridInverseSize[0] = 1.0 / w->lightGridSize[0];
	w->lightGridInverseSize[1] = 1.0 / w->lightGridSize[1];
	w->lightGridInverseSize[2] = 1.0 / w->lightGridSize[2];

//----(SA)	modified
	R_FindLightGridBounds( wMins, wMaxs );
//	wMins = w->bmodels[0].bounds[0];
//	wMaxs = w->bmodels[0].bounds[1];
//----(SA)	end

	for ( i = 0 ; i < 3 ; i++ ) {
		w->lightGridOrigin[i] = w->lightGridSize[i] * ceil( wMins[i] / w->lightGridSize[i] );
		maxs[i] = w->lightGridSize[i] * floor( wMaxs[i] / w->lightGridSize[i] );
		w->lightGridBounds[i] = ( maxs[i] - w->lightGridOrigin[i] ) / w->lightGridSize[i] + 1;
	}

	return w->lightGridBounds[0] * w->lightGridBounds[1] * w->lightGridBounds[2];
}

/*
================
R_LoadLightGrid

================
*/
void R_LoadLightGrid( fileHandle_t f, const lump_t *l ) {
	int i;
	int numGridPoints;
	world_t *w;

	w = &s_worldData;
	numGridPoints = R_LightGridSize();

	if ( l->filelen != numGridPoints * 8 ) {
		ri.Printf( PRINT_WARNING, "WARNING: light grid mismatch\n" );
		w->lightGridData = NULL;
		return;
	}

	w->lightGridData = ri.Hunk_Alloc( l->filelen, h_low );
	ri.CM_ReadLumpInto( f, l, w->lightGridData );

	// deal with overbright bits
	for ( i = 0 ; i < numGridPoints ; i++ ) {
		R_ColorShiftLightingBytes( &w->lightGridData[i * 8], &w->lightGridData[i * 8] );
		R_ColorShiftLightingBytes( &w->lightGridData[i * 8 + 3], &w->lightGridData[i * 8 + 3] );
	}
}

/*
================
R_LoadEntities
================
*/
void R_LoadEntities( void ) {
	char *p, *token, *s;
	char keyname[MAX_TOKEN_CHARS];
	char value[MAX_TOKEN_CHARS];
	world_t *w;

	w = &s_worldData;
	w->lightGridSize[0] = 64;
	w->lightGridSize[1] = 64;
	w->lightGridSize[2] = 128;

	// the collision map's, also for the cgame
	w->entityString = ri.CM_EntityString();
	w->entityParsePoint = w->entityString;
	p = w->entityString;

	token = COM_ParseExt( &p, qtrue );
	if ( !*token || *token != '{' ) {
		return;
	}

	// only parse the world spawn
	while ( 1 ) {
		// parse key
		token = COM_ParseExt( &p, qtrue );

		if ( !*token || *token == '}' ) {
			break;
		}
		Q_strncpyz( keyname, token, sizeof( keyname ) );

		// parse value
		token = COM_ParseExt( &p, qtrue );

		if ( !*token || *token == '}' ) {
			break;
		}
		Q_strncpyz( value, token, sizeof( value ) );

		// check for remapping of shaders for vertex lighting
		s = "vertexremapshader";
		if ( !Q_strncmp( keyname, s, strlen( s ) ) ) {
			s = strchr( value, ';' );
			if ( !s ) {
				ri.Printf( PRINT_WARNING, "WARNING: no semi colon in vertexshaderremap '%s'\n", value );
				break;
			}
			*s++ = 0;
			if ( r_vertexLight->integer ) {
				R_RemapShader( value, s, "0" );
			}
			continue;
		}
		// check for remapping of shaders
		s = "remapshader";
		if ( !Q_strncmp( keyname, s, strlen( s ) ) ) {
			s = strchr( value, ';' );
			if ( !s ) {
				ri.Printf( PRINT_WARNING, "WARNING: no semi colon in shaderremap '%s'\n", value );
				break;
			}
			*s++ = 0;
			R_RemapShader( value, s, "0" );
			continue;
		}
		// check for a different grid size
		if ( !Q_stricmp( keyname, "gridsize" ) ) {
			sscanf( value, "%f %f %f", &w->lightGridSize[0], &w->lightGridSize[1], &w->lightGridSize[2] );
			continue;
		}
	}
}

/*
=================
R_GetEntityToken
=================
*/
qboolean R_GetEntityToken( char *buffer, int size ) {
	const char  *s;

	s = COM_Parse( &s_worldData.entityParsePoint );
	Q_strncpyz( buffer, s, size );
	if ( !s_worldData.entityParsePoint && !s[0] ) {
		s_worldData.entityParsePoint = s_worldData.entityString;
		return qfalse;
	} else {
		return qtrue;
	}
}

/*
=================
R_LoadWldFogs
=================
*/
static void R_LoadWldFogs( bspLump_t *l ) {
	wldFog_t    *in;
	fog_t       *out;
	int i, count;

	in = l->data;
	if ( l->len % sizeof( *in ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	count = l->len / sizeof( *in );

	s_worldData.numfogs = count + 1;
	s_worldData.fogs = ri.Hunk_Alloc( s_worldData.numfogs * sizeof( *out ), h_low );
	out = s_worldData.fogs + 1;

	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		in->shader[sizeof( in->shader ) - 1] = 0;
		VectorCopy( in->bounds[0], out->bounds[0] );
		VectorCopy( in->bounds[1], out->bounds[1] );
		R_FogParms( out, in->shader );
		out->hasSurface = in->hasSurface != 0;
		Vector4Copy( in->surface, out->surface );
	}
}

/*
=================
R_LoadWldSurfaces

The vertexes and indexes are read straight into the hunk, as they are drawn
=================
*/
static void R_LoadWldSurfaces( fileHandle_t f, const wldHeader_t *h ) {
	const lump_t *vl = (const lump_t *)&h->lumps[WLD_LUMP_VERTS];
	const lump_t *il = (const lump_t *)&h->lumps[WLD_LUMP_INDEXES];
	bspLump_t l;
	wldSurface_t *in;
	msurface_t *out;
	wldVert_t *verts;
	unsigned short *indexes;
	int i, j, count, numVerts, numIndexes;
	int numSurfs = 0, numFlares = 0;

	if ( vl->filelen % sizeof( *verts ) || il->filelen % sizeof( *indexes ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	numVerts = vl->filelen / sizeof( *verts );
	numIndexes = il->filelen / sizeof( *indexes );
	verts = ri.Hunk_Alloc( vl->filelen, h_low );
	ri.CM_ReadLumpInto( f, vl, verts );
	indexes = ri.Hunk_Alloc( il->filelen, h_low );
	ri.CM_ReadLumpInto( f, il, indexes );

	for ( i = 0 ; i < numVerts ; i++ ) {
		R_ColorShiftLightingBytes( verts[i].color, verts[i].color );
	}

	ri.CM_ReadLump( f, (const lump_t *)&h->lumps[WLD_LUMP_SURFACES], &l );
	in = l.data;
	if ( l.len % sizeof( *in ) ) {
		ri.Error( ERR_DROP, "LoadMap: funny lump size in %s",s_worldData.name );
	}
	count = l.len / sizeof( *in );

	out = ri.Hunk_Alloc( count * sizeof( *out ), h_low );
	s_worldData.surfaces = out;
	s_worldData.numsurfaces = count;

	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		out->fogIndex = in->fogNum + 1;
		out->shader = ShaderForShaderNum( in->shaderNum, in->lightmapNum );
		if ( r_singleShader->integer && !out->shader->isSky ) {
			out->shader = tr.defaultShader;
		}

		if ( in->kind == WLD_FLARE ) {
			srfFlare_t *flare = ri.Hunk_Alloc( sizeof( *flare ), h_low );

			flare->surfaceType = SF_FLARE;
			VectorCopy( in->flare[0], flare->origin );
			VectorCopy( in->flare[1], flare->color );
			VectorCopy( in->flare[2], flare->normal );
			out->data = (surfaceType_t *)flare;
			numFlares++;
		} else {
			srfWorld_t *srf = ri.Hunk_Alloc( sizeof( *srf ), h_low );

			if ( in->kind < WLD_PLANAR || in->kind > WLD_TRIANGLES ||
				 in->firstVert < 0 || in->numVerts < 0 || in->firstVert + in->numVerts > numVerts ||
				 in->firstIndex < 0 || in->numIndexes < 3 || in->numIndexes % 3 ||
				 in->firstIndex + in->numIndexes > numIndexes ||
				 in->numVerts >= SHADER_MAX_VERTEXES || in->numIndexes >= SHADER_MAX_INDEXES ) {
				ri.Error( ERR_DROP, "LoadMap: bad surface %d in %s", i, s_worldData.name );
			}
			srf->surfaceType = SF_WORLD;
			srf->kind = in->kind;
			srf->numVerts = in->numVerts;
			srf->numIndexes = in->numIndexes;
			srf->verts = verts + in->firstVert;
			srf->indexes = indexes + in->firstIndex;
			for ( j = 0 ; j < srf->numIndexes ; j++ ) {
				if ( srf->indexes[j] >= srf->numVerts ) {
					ri.Error( ERR_DROP, "LoadMap: bad index in surface %d in %s", i, s_worldData.name );
				}
			}
			VectorCopy( in->bounds[0], srf->bounds[0] );
			VectorCopy( in->bounds[1], srf->bounds[1] );
			VectorCopy( in->origin, srf->origin );
			srf->xyzStep = in->xyzStep;
			srf->stOrigin[0] = in->stOrigin[0];
			srf->stOrigin[1] = in->stOrigin[1];
			srf->stStep = in->stStep;
			srf->hasPlane = in->plane[0] || in->plane[1] || in->plane[2];
			if ( srf->hasPlane ) {
				VectorCopy( in->plane, srf->plane.normal );
				srf->plane.dist = in->plane[3];
				SetPlaneSignbits( &srf->plane );
				srf->plane.type = PlaneTypeForNormal( srf->plane.normal );
			}
			out->data = (surfaceType_t *)srf;
			numSurfs++;
		}
	}
	ri.CM_FreeLump( &l );

	ri.Printf( PRINT_ALL, "...loaded %d surfaces, %i flares, %i vertexes\n", numSurfs, numFlares, numVerts );
}

/*
================
R_LoadWldLightGrid

At most 256 points, and a byte a grid point, which of them it is
================
*/
static void R_LoadWldLightGrid( fileHandle_t f, const lump_t *l ) {
	world_t *w = &s_worldData;
	int numGridPoints, numPoints, i;
	lump_t part;

	numGridPoints = R_LightGridSize();
	if ( l->filelen < 4 ) {
		ri.Printf( PRINT_WARNING, "WARNING: light grid mismatch\n" );
		return;
	}
	part.fileofs = l->fileofs;
	part.filelen = 4;
	ri.CM_ReadLumpInto( f, &part, &numPoints );
	numPoints = LittleLong( numPoints );
	if ( numPoints < 1 || numPoints > 256 || l->filelen != 4 + numPoints * 8 + numGridPoints ) {
		ri.Printf( PRINT_WARNING, "WARNING: light grid mismatch\n" );
		return;
	}

	w->lightGridData = ri.Hunk_Alloc( numPoints * 8, h_low );
	part.fileofs = l->fileofs + 4;
	part.filelen = numPoints * 8;
	ri.CM_ReadLumpInto( f, &part, w->lightGridData );
	w->lightGridIndex = ri.Hunk_Alloc( numGridPoints, h_low );
	part.fileofs += part.filelen;
	part.filelen = numGridPoints;
	ri.CM_ReadLumpInto( f, &part, w->lightGridIndex );

	for ( i = 0 ; i < numGridPoints ; i++ ) {
		if ( w->lightGridIndex[i] >= numPoints ) {
			ri.Error( ERR_DROP, "LoadMap: bad light grid in %s", s_worldData.name );
		}
	}

	// deal with overbright bits
	for ( i = 0 ; i < numPoints ; i++ ) {
		R_ColorShiftLightingBytes( &w->lightGridData[i * 8], &w->lightGridData[i * 8] );
		R_ColorShiftLightingBytes( &w->lightGridData[i * 8 + 3], &w->lightGridData[i * 8 + 3] );
	}
}

/*
=================
R_LoadWld

maps/<map>.wld in place of the .bsp, when there is one (wldfile.h)
=================
*/
static qboolean R_LoadWld( void ) {
	char name[MAX_QPATH];
	fileHandle_t f;
	wldHeader_t h;
	lump_t hl = { 0, sizeof( h ) };
	bspLump_t a, b;
	int i;

	COM_StripExtension( s_worldData.name, name, sizeof( name ) );
	Q_strcat( name, sizeof( name ), ".wld" );
	if ( ri.FS_FOpenFileRead( name, &f, qtrue ) <= 0 || !f ) {
		return qfalse;
	}
	ri.CM_ReadLumpInto( f, &hl, &h );
	for ( i = 0 ; i < sizeof( h ) / 4 ; i++ ) {
		( (int *)&h )[i] = LittleLong( ( (int *)&h )[i] );
	}
	if ( h.ident != WLD_IDENT || h.version != WLD_VERSION ) {
		ri.Printf( PRINT_WARNING, "WARNING: %s isn't a version %d .wld, loading the .bsp\n", name, WLD_VERSION );
		ri.FS_FCloseFile( f );
		return qfalse;
	}

	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	ri.CM_ReadLump( f, (lump_t *)&h.lumps[WLD_LUMP_SHADERS], &a );
	R_LoadShaders( &a );
	ri.CM_FreeLump( &a );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	R_LoadLightmaps( f, (lump_t *)&h.lumps[WLD_LUMP_LIGHTMAPS] );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	R_LoadPlanes();
	ri.CM_ReadLump( f, (lump_t *)&h.lumps[WLD_LUMP_FOGS], &a );
	R_LoadWldFogs( &a );
	ri.CM_FreeLump( &a );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	R_LoadWldSurfaces( f, &h );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	ri.CM_ReadLump( f, (lump_t *)&h.lumps[WLD_LUMP_LEAFSURFACES], &a );
	R_LoadMarksurfaces( &a );
	ri.CM_FreeLump( &a );
	ri.CM_ReadLump( f, (lump_t *)&h.lumps[WLD_LUMP_NODES], &a );
	ri.CM_ReadLump( f, (lump_t *)&h.lumps[WLD_LUMP_LEAFS], &b );
	R_LoadNodesAndLeafs( &a, &b );
	ri.CM_FreeLump( &b );
	ri.CM_FreeLump( &a );
	ri.CM_ReadLump( f, (lump_t *)&h.lumps[WLD_LUMP_MODELS], &a );
	R_LoadSubmodels( &a );
	ri.CM_FreeLump( &a );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	R_LoadVisibility();
	R_LoadEntities();
	R_LoadWldLightGrid( f, (lump_t *)&h.lumps[WLD_LUMP_LIGHTGRID] );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );

	ri.FS_FCloseFile( f );
	return qtrue;
}

/*
=================
R_LoadBsp

The .bsp, a lump at a time, sharing what the collision map has loaded
=================
*/
static void R_LoadBsp( const char *name ) {
	dheader_t header;
	fileHandle_t f;
	bspLump_t a, b, c;

	f = ri.CM_OpenBsp( name, &header );

	// load into heap
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	ri.CM_ReadLump( f, &header.lumps[LUMP_SHADERS], &a );
	R_LoadShaders( &a );
	ri.CM_FreeLump( &a );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	R_LoadLightmaps( f, &header.lumps[LUMP_LIGHTMAPS] );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	R_LoadPlanes();
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	ri.CM_ReadLump( f, &header.lumps[LUMP_FOGS], &a );
	ri.CM_ReadLump( f, &header.lumps[LUMP_BRUSHES], &b );
	ri.CM_ReadLump( f, &header.lumps[LUMP_BRUSHSIDES], &c );
	R_LoadFogs( &a, &b, &c );
	ri.CM_FreeLump( &c );
	ri.CM_FreeLump( &b );
	ri.CM_FreeLump( &a );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	ri.CM_ReadLump( f, &header.lumps[LUMP_DRAWVERTS], &a );
	ri.CM_ReadLump( f, &header.lumps[LUMP_DRAWINDEXES], &b );
	ri.CM_ReadLump( f, &header.lumps[LUMP_SURFACES], &c );
	R_LoadSurfaces( &c, &a, &b );
	ri.CM_FreeLump( &c );
	ri.CM_FreeLump( &b );
	ri.CM_FreeLump( &a );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	ri.CM_ReadLump( f, &header.lumps[LUMP_LEAFSURFACES], &a );
	R_LoadMarksurfaces( &a );
	ri.CM_FreeLump( &a );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	ri.CM_ReadLump( f, &header.lumps[LUMP_NODES], &a );
	ri.CM_ReadLump( f, &header.lumps[LUMP_LEAFS], &b );
	R_LoadNodesAndLeafs( &a, &b );
	ri.CM_FreeLump( &b );
	ri.CM_FreeLump( &a );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	ri.CM_ReadLump( f, &header.lumps[LUMP_MODELS], &a );
	R_LoadSubmodels( &a );
	ri.CM_FreeLump( &a );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	R_LoadVisibility();
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	R_LoadEntities();
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );
	R_LoadLightGrid( f, &header.lumps[LUMP_LIGHTGRID] );
	ri.Cmd_ExecuteText( EXEC_NOW, "updatescreen\n" );

	ri.FS_FCloseFile( f );
}

/*
=================
RE_LoadWorldMap

Called directly from cgame
=================
*/
void RE_LoadWorldMap( const char *name ) {
	byte        *startMarker;

	skyboxportal = 0;

	if ( tr.worldMapLoaded ) {
		ri.Error( ERR_DROP, "ERROR: attempted to redundantly load world map" );
	}

	// set default sun direction to be used if it isn't
	// overridden by a shader
	tr.sunDirection[0] = 0.45;
	tr.sunDirection[1] = 0.3;
	tr.sunDirection[2] = 0.9;

	tr.sunShader = 0;   // clear sunshader so it's not there if the level doesn't specify it

	// invalidate fogs (likely to be re-initialized to new values by the current map)
	// TODO:(SA)this is sort of silly.  I'm going to do a general cleanup on fog stuff
	//			now that I can see how it's been used.  (functionality can narrow since
	//			it's not used as much as it's designed for.)
	R_SetFog( FOG_SKY,       0, 0, 0, 0, 0, 0 );
	R_SetFog( FOG_PORTALVIEW,0, 0, 0, 0, 0, 0 );
	R_SetFog( FOG_HUD,       0, 0, 0, 0, 0, 0 );
	R_SetFog( FOG_MAP,       0, 0, 0, 0, 0, 0 );
	R_SetFog( FOG_CURRENT,   0, 0, 0, 0, 0, 0 );
	R_SetFog( FOG_TARGET,    0, 0, 0, 0, 0, 0 );
	R_SetFog( FOG_WATER,     0, 0, 0, 0, 0, 0 );
	R_SetFog( FOG_SERVER,    0, 0, 0, 0, 0, 0 );

	VectorNormalize( tr.sunDirection );

	tr.worldMapLoaded = qtrue;

	// clear tr.world so if the level fails to load, the next
	// try will not look at the partially loaded version
	tr.world = NULL;

	memset( &s_worldData, 0, sizeof( s_worldData ) );
	Q_strncpyz( s_worldData.name, name, sizeof( s_worldData.name ) );

	Q_strncpyz( s_worldData.baseName, COM_SkipPath( s_worldData.name ), sizeof( s_worldData.name ) );
	COM_StripExtension(s_worldData.baseName, s_worldData.baseName, sizeof(s_worldData.baseName));

	startMarker = ri.Hunk_Alloc( 0, h_low );
	c_gridVerts = 0;

	if ( !R_LoadWld() ) {
		R_LoadBsp( name );
	}

	s_worldData.dataSize = (byte *)ri.Hunk_Alloc( 0, h_low ) - startMarker;

	// only set tr.world now that we know the entire level has loaded properly
	tr.world = &s_worldData;

	// reset fog to world fog (if present)
//	R_SetFog(FOG_CMD_SWITCHFOG, FOG_MAP,20,0,0,0,0);

//----(SA)	set the sun shader if there is one
	if ( tr.sunShaderName ) {
		tr.sunShader = R_FindShader( tr.sunShaderName, LIGHTMAP_NONE, qtrue );
	}

//----(SA)	end
}

