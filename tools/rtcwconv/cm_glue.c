/*
 * The engine's own curve collision (SP/code/qcommon/cm_patch.c) built into
 * rtcwconv, with just enough of qcommon under it to run offline, so the .col
 * holds exactly what CM_GeneratePatchCollide would make in game.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cm_local.h"
#include "cm_patch.h"

#include "col_glue.h"

/* everything one patch allocates, freed once it's written out */
#define MAX_ALLOCS 16
static void *allocs[MAX_ALLOCS];
static int numAllocs;

static void *Alloc( int size ) {
	void *p = calloc( 1, size > 0 ? size : 1 );
	if ( !p || numAllocs == MAX_ALLOCS ) {
		fprintf( stderr, "cm_glue: out of memory\n" );
		exit( 1 );
	}
	allocs[numAllocs++] = p;
	return p;
}

void *Hunk_Alloc( int size, ha_pref preference ) {
	return Alloc( size );
}

void *Hunk_AllocateTempMemory( int size ) {
	return Alloc( size );
}

void Hunk_FreeTempMemory( void *buf ) {
}

/* windings (cm_polylib.c): many, each freed as it goes */
void *Z_Malloc( int size ) {
	void *p = calloc( 1, size );
	if ( !p ) {
		fprintf( stderr, "cm_glue: out of memory\n" );
		exit( 1 );
	}
	return p;
}

void Z_Free( void *ptr ) {
	free( ptr );
}

static jmp_buf *errorJump;

void QDECL Com_Error( int code, const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	vfprintf( stderr, fmt, ap );
	va_end( ap );
	fputc( '\n', stderr );
	longjmp( *errorJump, 1 );
}

void QDECL Com_Printf( const char *fmt, ... ) {
}

void QDECL Com_DPrintf( const char *fmt, ... ) {
}

static cvar_t zeroCvar;

cvar_t *Cvar_Get( const char *var_name, const char *value, int flags ) {
	return &zeroCvar;
}

cvar_t *cm_playerCurveClip = &zeroCvar;

qboolean CM_BoundsIntersect( const vec3_t mins, const vec3_t maxs, const vec3_t mins2, const vec3_t maxs2 ) {
	return qfalse;   /* traces only, never while generating */
}

static void PutInt( unsigned char **p, int v ) {
	memcpy( *p, &v, 4 );
	*p += 4;
}

static void PutFloat( unsigned char **p, float v ) {
	memcpy( *p, &v, 4 );
	*p += 4;
}

int ColGeneratePatch( int width, int height, const float *xyz, unsigned char **out ) {
	vec3_t *points = Alloc( width * height * sizeof( vec3_t ) );
	patchCollide_t *pc;
	jmp_buf jump;
	unsigned char *p;
	int i, j, size;

	errorJump = &jump;
	if ( setjmp( jump ) ) {
		ColFreePatch();
		return -1;
	}
	memcpy( points, xyz, width * height * sizeof( vec3_t ) );
	pc = CM_GeneratePatchCollide( width, height, points );

	size = 6 * 4 + 2 * 4 + pc->numPlanes * 5 * 4;
	for ( i = 0; i < pc->numFacets; i++ ) {
		size += 2 * 4 + pc->facets[i].numBorders * 3 * 4;
	}
	*out = p = Alloc( size );

	for ( i = 0; i < 2; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			PutFloat( &p, pc->bounds[i][j] );
		}
	}
	PutInt( &p, pc->numPlanes );
	PutInt( &p, pc->numFacets );
	for ( i = 0; i < pc->numPlanes; i++ ) {
		for ( j = 0; j < 4; j++ ) {
			PutFloat( &p, pc->planes[i].plane[j] );
		}
		PutInt( &p, pc->planes[i].signbits );
	}
	for ( i = 0; i < pc->numFacets; i++ ) {
		const cFacet_t *f = &pc->facets[i];
		const unsigned short *b = pc->borders + f->firstBorder;
		PutInt( &p, f->surfacePlane );
		PutInt( &p, f->numBorders );
		for ( j = 0; j < f->numBorders; j++ ) {
			PutInt( &p, BORDER_PLANE( b[j] ) );
			PutInt( &p, BORDER_IS_INWARD( b[j] ) );
			PutInt( &p, ( b[j] & BORDER_NOADJUST ) != 0 );
		}
	}
	return size;
}

void ColFreePatch( void ) {
	while ( numAllocs ) {
		free( allocs[--numAllocs] );
	}
}
