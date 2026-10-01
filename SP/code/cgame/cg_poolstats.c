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


// cg_poolstats.c -- the most of each cgame pool a level uses at once,
// printed when the level ends: what a per-level budget has to hold.
// PC only.

#include "cg_local.h"

#ifndef _arch_dreamcast

int CG_PoolParticles( int *size );
int CG_PoolTrailJuncs( int *size );
int CG_PoolFlameChunks( int *size );
int UI_PoolMemoryUsed( int *strings );

extern localEntity_t cg_activeLocalEntities;
extern markPoly_t cg_activeMarkPolys;
extern int numSoundScripts, numSoundScriptSounds;

static struct {
	int ents, entMax, snapEnts;
	int localEnts, marks, particles, trails, flames;
	int particleSize, trailSize, flameSize;
} peak;

/*
=================
CG_PoolStatsFrame

Called every frame
=================
*/
void CG_PoolStatsFrame( void ) {
	localEntity_t *le;
	markPoly_t *mp;
	int i, n, size;

	for ( i = n = 0 ; i < MAX_GENTITIES ; i++ ) {
		if ( cg_entities[i].currentValid ) {
			n++;
			if ( i + 1 > peak.entMax ) {
				peak.entMax = i + 1;
			}
		}
	}
	if ( n > peak.ents ) {
		peak.ents = n;
	}
	if ( cg.snap && cg.snap->numEntities > peak.snapEnts ) {
		peak.snapEnts = cg.snap->numEntities;
	}

	for ( n = 0, le = cg_activeLocalEntities.next ; le != &cg_activeLocalEntities ; le = le->next ) {
		n++;
	}
	if ( n > peak.localEnts ) {
		peak.localEnts = n;
	}

	for ( n = 0, mp = cg_activeMarkPolys.nextMark ; mp != &cg_activeMarkPolys ; mp = mp->nextMark ) {
		n++;
	}
	if ( n > peak.marks ) {
		peak.marks = n;
	}

	n = CG_PoolParticles( &peak.particleSize );
	if ( n > peak.particles ) {
		peak.particles = n;
	}
	n = CG_PoolTrailJuncs( &peak.trailSize );
	if ( n > peak.trails ) {
		peak.trails = n;
	}
	n = CG_PoolFlameChunks( &peak.flameSize );
	if ( n > peak.flames ) {
		peak.flames = n;
	}
	(void)size;
}

/*
=================
CG_PoolStatsReport

Called when the level ends
=================
*/
void CG_PoolStatsReport( void ) {
	int strings, mem;

	mem = UI_PoolMemoryUsed( &strings );
	CG_Printf( "POOL cgame %s: centities %d (highest num %d, snapshot %d) x %d; "
			   "localents %d x %d; marks %d x %d; particles %d x %d; trailjuncs %d x %d; flames %d x %d; "
			   "soundscripts %d x %d, sounds %d x %d; ui mem %d strings %d\n",
			   cgs.mapname, peak.ents, peak.entMax, peak.snapEnts, (int)sizeof( centity_t ),
			   peak.localEnts, (int)sizeof( localEntity_t ), peak.marks, (int)sizeof( markPoly_t ),
			   peak.particles, peak.particleSize, peak.trails, peak.trailSize, peak.flames, peak.flameSize,
			   numSoundScripts, (int)sizeof( soundScript_t ), numSoundScriptSounds, (int)sizeof( soundScriptSound_t ),
			   mem, strings );
	memset( &peak, 0, sizeof( peak ) );
}

#endif
