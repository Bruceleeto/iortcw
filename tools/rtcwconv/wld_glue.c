/*
 * The renderer's own curve code (SP/code/renderer tr_curve.c and
 * tr_stitch.c) built into rtcwconv, with just enough of the renderer under
 * it to run offline, so the .wld's curves are cut as the game would cut
 * them at load.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tr_local.h"

#include "wld_glue.h"

refimport_t ri;

static cvar_t subdivisions;
cvar_t *r_subdivisions = &subdivisions;

static void *Alloc( int size ) {
	void *p = calloc( 1, size > 0 ? size : 1 );
	if ( !p ) {
		fprintf( stderr, "wld_glue: out of memory\n" );
		exit( 1 );
	}
	return p;
}

/* tr_curve.c frees what it allocates itself; the rest goes with the grid */
static void *HunkAlloc( int size, ha_pref preference ) {
	return Alloc( size );
}

static void Free( void *p ) {
	free( p );
}

static void QDECL Printf( int printLevel, const char *fmt, ... ) {
}

static void QDECL Error( int errorLevel, const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	vfprintf( stderr, fmt, ap );
	va_end( ap );
	fputc( '\n', stderr );
	exit( 1 );
}

/* Z_Malloc, Com_Error and the like: cm_glue.c's */

static srfGridMesh_t **grids;
static int numGrids;

void WldCutCurves( int numPatches, const WldPatch *patches, float subdivisionSize ) {
	drawVert_t *points;
	int i, j;

	ri.Hunk_Alloc = HunkAlloc;
	ri.Hunk_AllocateTempMemory = Alloc;
	ri.Hunk_FreeTempMemory = Free;
	ri.Free = Free;
	ri.Printf = Printf;
	ri.Error = Error;
	subdivisions.value = subdivisionSize;
	subdivisions.integer = subdivisionSize;

	numGrids = numPatches;
	grids = Alloc( numPatches * sizeof( *grids ) );
	points = Alloc( MAX_PATCH_SIZE * MAX_PATCH_SIZE * sizeof( *points ) );
	for ( i = 0; i < numPatches; i++ ) {
		const WldPatch *p = &patches[i];
		if ( p->width * p->height > MAX_PATCH_SIZE * MAX_PATCH_SIZE ) {
			Error( 0, "patch %d: %dx%d is too big", i, p->width, p->height );
		}
		for ( j = 0; j < p->width * p->height; j++ ) {
			const WldPatchVert *v = &p->verts[j];
			memcpy( points[j].xyz, v->xyz, sizeof( v->xyz ) );
			memcpy( points[j].st, v->st, sizeof( v->st ) );
			memcpy( points[j].lightmap, v->lightmap, sizeof( v->lightmap ) );
			memcpy( points[j].normal, v->normal, sizeof( v->normal ) );
			memcpy( points[j].color, v->color, sizeof( v->color ) );
		}
		grids[i] = R_SubdividePatchToGrid( p->width, p->height, points );
	}
	free( points );

	/* curves that share an edge, cut alike so no cracks show between them */
	R_StitchAllPatches( (surfaceType_t **)grids, numGrids );
}

void WldCurve( int n, int *width, int *height, const WldPatchVert **verts ) {
	srfGridMesh_t *g = grids[n];
	static WldPatchVert *out;
	int i;

	free( out );
	out = Alloc( g->width * g->height * sizeof( *out ) );
	for ( i = 0; i < g->width * g->height; i++ ) {
		const drawVert_t *v = &g->verts[i];
		memcpy( out[i].xyz, v->xyz, sizeof( v->xyz ) );
		memcpy( out[i].st, v->st, sizeof( v->st ) );
		memcpy( out[i].lightmap, v->lightmap, sizeof( v->lightmap ) );
		memcpy( out[i].normal, v->normal, sizeof( v->normal ) );
		memcpy( out[i].color, v->color, sizeof( v->color ) );
	}
	*width = g->width;
	*height = g->height;
	*verts = out;
}

void WldFreeCurves( void ) {
	int i;
	for ( i = 0; i < numGrids; i++ ) {
		R_FreeSurfaceGridMesh( grids[i] );
	}
	free( grids );
	grids = NULL;
	numGrids = 0;
}
