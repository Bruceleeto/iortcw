/*
===========================================================================
dcsim_aica.c: snd_aica.h in the sim, with no AICA to play on

snd_aica.c runs here as on the Dreamcast (the sounds read, encoded and
"written", the voices started and stopped, the rings filled), so main RAM
and the load work are the Dreamcast's; just nothing is heard. AICA RAM isn't
kept (writes go nowhere, reads are silence).
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/snd_aica.h"

// AICAflow's arena on the Dreamcast: from the end of its driver's own RAM to
// its control block (deps/AICAflow, protocol.h), less the voices' flow
#define SIM_ARENA_BASE		0x00008000u
#define SIM_ARENA_END		0x001fc000u
// AICAflow's tables in main RAM (allocator.c's, grown as assets come)
#define SIM_HOST_BYTES		1024

static void			*sim_host;
static int			sim_numVoices;

qboolean AICA_Init( int voices, unsigned *base, unsigned *bytes ) {
	sim_host = malloc( SIM_HOST_BYTES );
	sim_numVoices = voices;
	*base = SIM_ARENA_BASE;
	*bytes = SIM_ARENA_END - SIM_ARENA_BASE;
	Com_Printf( "AICA (sim, silent): %d voices, %u K of sound RAM\n", voices, *bytes >> 10 );
	return qtrue;
}

void AICA_Shutdown( void ) {
	free( sim_host );
	sim_host = NULL;
}

void AICA_Write( unsigned addr, const void *data, int bytes ) {
	if ( ( addr & 3 ) || addr + bytes > SIM_ARENA_END || addr < SIM_ARENA_BASE ) {
		Com_Error( ERR_FATAL, "AICA_Write: 0x%x + %d outside the arena", addr, bytes );
	}
}

void AICA_Read( unsigned addr, void *data, int bytes ) {
	if ( ( addr & 3 ) || addr + bytes > SIM_ARENA_END || addr < SIM_ARENA_BASE ) {
		Com_Error( ERR_FATAL, "AICA_Read: 0x%x + %d outside the arena", addr, bytes );
	}
	Com_Memset( data, 0, bytes );
}

qboolean AICA_Voice( int voice, unsigned mask, const unsigned short *values ) {
	if ( voice < 0 || voice >= sim_numVoices ) {
		Com_Error( ERR_FATAL, "AICA_Voice: voice %d", voice );
	}
	return qtrue;
}

void AICA_Update( void ) {
}
