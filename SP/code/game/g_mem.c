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

//
// g_mem.c
//


#include "g_local.h"

// What G_Alloc hands out comes from blocks malloc'd as the level needs them,
// all freed when it ends (G_FreeMemory): nothing is held for the worst level.
// Not the hunk, as some of it is allocated in play, after the hunk's mark.
#define MEM_BLOCK       ( 128 * 1024 )
#define MEM_ALIGN( x )  ( ( ( x ) + 7 ) & ~7 )

typedef struct memBlock_s {
	struct memBlock_s *next;
	int size, used;
} memBlock_t;

#define MEM_HEADER      MEM_ALIGN( sizeof( memBlock_t ) )

static memBlock_t *memBlocks;  // the one being filled first
static int memUsed, memHeld;

static memBlock_t *G_NewMemBlock( int size ) {
	memBlock_t *b;

	b = malloc( MEM_HEADER + size );
	if ( !b ) {
		G_Error( "G_Alloc: out of memory for %i bytes", size );
	}
	b->size = size;
	b->used = 0;
	memHeld += MEM_HEADER + size;
	return b;
}

void *G_Alloc( int size ) {
	memBlock_t *b;
	char    *p;

	size = MEM_ALIGN( size );

	if ( size > MEM_BLOCK / 2 ) {
		// a block of its own, behind the one being filled
		b = G_NewMemBlock( size );
		if ( memBlocks ) {
			b->next = memBlocks->next;
			memBlocks->next = b;
		} else {
			b->next = NULL;
			memBlocks = b;
		}
	} else {
		b = memBlocks;
		if ( !b || b->used + size > b->size ) {
			b = G_NewMemBlock( MEM_BLOCK );
			b->next = memBlocks;
			memBlocks = b;
		}
	}

	p = (char *)b + MEM_HEADER + b->used;
	b->used += size;
	memUsed += size;
	memset( p, 0, size );

	if ( g_debugAlloc.integer ) {
		G_Printf( "G_Alloc of %i bytes (%i in use, %i held)\n", size, memUsed, memHeld );
	}
	return p;
}

void G_FreeMemory( void ) {
	memBlock_t *b, *next;

	for ( b = memBlocks ; b ; b = next ) {
		next = b->next;
		free( b );
	}
	memBlocks = NULL;
	memUsed = memHeld = 0;
}

void G_InitMemory( void ) {
	G_FreeMemory();
}

void Svcmd_GameMem_f( void ) {
	G_Printf( "Game memory status: %i bytes allocated, %i held\n", memUsed, memHeld );
}

#ifndef _arch_dreamcast
/*
=================
G_PoolStatsFrame / G_PoolStatsReport

The most of each game pool a level uses at once, printed when the level
ends. PC only.
=================
*/
static int peakEnts, peakInuse, peakClients;

void G_PoolStatsFrame( void ) {
	int i, n;

	if ( level.num_entities > peakEnts ) {
		peakEnts = level.num_entities;
	}
	for ( i = n = 0 ; i < level.num_entities ; i++ ) {
		if ( g_entities[i].inuse ) {
			n++;
		}
	}
	if ( n > peakInuse ) {
		peakInuse = n;
	}
	for ( i = n = 0 ; i < level.maxclients ; i++ ) {
		if ( g_entities[i].inuse ) {
			n++;
		}
	}
	if ( n > peakClients ) {
		peakClients = n;
	}
}

void G_PoolStatsReport( void ) {
	char mapname[MAX_QPATH];

	trap_Cvar_VariableStringBuffer( "mapname", mapname, sizeof( mapname ) );
	G_Printf( "POOL game %s: gentities %d in use (highest num %d) x %d; clients %d of %d x %d; g_mem %d (held %d)\n",
			  mapname, peakInuse, peakEnts, (int)sizeof( gentity_t ), peakClients, level.maxclients,
			  (int)sizeof( gclient_t ), memUsed, memHeld );
	peakEnts = peakInuse = peakClients = 0;
}
#endif
