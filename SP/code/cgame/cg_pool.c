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


// cg_pool.c -- the effect pools (local entities, marks, particles, trails,
// flames): malloc'd a chunk at a time as a level needs them instead of all
// at once, freed when the level is cleared

#include "cg_local.h"

/*
===============
CG_PoolGrow

A new zeroed chunk of pool->chunkItems items, or NULL if the pool has all it
may have (or there is no memory left): the caller then reuses an old one
===============
*/
void *CG_PoolGrow( cgPool_t *pool ) {
	void *chunk;

	if ( ( pool->numChunks + 1 ) * pool->chunkItems > pool->maxItems || pool->numChunks == CG_POOL_MAX_CHUNKS ) {
		return NULL;
	}
	chunk = calloc( pool->chunkItems, pool->itemSize );
	if ( !chunk ) {
		return NULL;
	}
	pool->chunks[pool->numChunks++] = chunk;
	return chunk;
}

/*
===============
CG_PoolItem

The item at index, NULL if the pool hasn't grown that far
===============
*/
void *CG_PoolItem( cgPool_t *pool, int index ) {
	int chunk = index / pool->chunkItems;

	if ( index < 0 || chunk >= pool->numChunks ) {
		return NULL;
	}
	return (byte *)pool->chunks[chunk] + ( index % pool->chunkItems ) * pool->itemSize;
}

/*
===============
CG_PoolIndex

The index of an item of the pool
===============
*/
int CG_PoolIndex( cgPool_t *pool, const void *item ) {
	int i, ofs;

	for ( i = 0 ; i < pool->numChunks ; i++ ) {
		ofs = (const byte *)item - (const byte *)pool->chunks[i];
		if ( ofs >= 0 && ofs < pool->chunkItems * pool->itemSize ) {
			return i * pool->chunkItems + ofs / pool->itemSize;
		}
	}
	CG_Error( "CG_PoolIndex: not in the pool" );
	return -1;
}

/*
===============
CG_PoolFree
===============
*/
void CG_PoolFree( cgPool_t *pool ) {
	int i;

	for ( i = 0 ; i < pool->numChunks ; i++ ) {
		free( pool->chunks[i] );
	}
	pool->numChunks = 0;
}

/*
===============
CG_PoolItems

How many items the pool has
===============
*/
int CG_PoolItems( const cgPool_t *pool ) {
	return pool->numChunks * pool->chunkItems;
}

/*
===============
CG_FreeEffectPools

When the cgame shuts down
===============
*/
void CG_FreeEffectPools( void ) {
	extern cgPool_t cg_localEntityPool, cg_markPolyPool, cg_particlePool, cg_trailPool, cg_flameChunkPool;

	CG_PoolFree( &cg_localEntityPool );
	CG_PoolFree( &cg_markPolyPool );
	CG_PoolFree( &cg_particlePool );
	CG_PoolFree( &cg_trailPool );
	CG_PoolFree( &cg_flameChunkPool );
}
