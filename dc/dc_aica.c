/*
===========================================================================
dc_aica.c: snd_aica.h on the Dreamcast, through AICAflow (deps/AICAflow)

AICAflow's ARM driver (aicaflow.drv, linked in by dc_aica_fw.S) runs on the
AICA's ARM7. It plays "flows", scripts of register writes; here there's one,
which holds every voice snd_aica.c plays on and does nothing by itself (its
script is just PARK). snd_aica.c plays a sound by patching a voice's
registers: afx_instance_patch writes them, raw, CONTROL last, so a key on can
go with them. The sounds' part of AICA RAM is one AICAflow allocation, all of
the arena, which snd_aica.c shares out itself.
===========================================================================
*/

#include <kos.h>
#include <dc/spu.h>

#include <aicaflow/bank.h>
#include <aicaflow/codec.h>
#include <aicaflow/host.h>

#include "q_shared.h"
#include "qcommon.h"
#include "snd_aica.h"

extern const unsigned char aica_firmware[], aica_firmware_end[];

static afx_asset_t		dc_flow;
static afx_instance_t	dc_voices;
static unsigned			dc_arena;
static int				dc_numVoices;

// a flow of voices channels: controlled (patched from here), no samples, no
// setup, its script one PARK; the header as afx_file_header_t
static int DC_AICA_VoiceFlow( int voices ) {
	static uint8_t file[96 + 4] __attribute__(( aligned( 32 ) ));
	uint8_t *image = file + 96;

	memset( file, 0, sizeof( file ) );
	image[0] = AFX_OP_PARK;
	afx_write32( file, AFX_FILE_MAGIC );
	afx_write32( file + 4, AFX_FILE_VERSION );
	afx_write32( file + 8, 96 + 1 );		// total size
	afx_write32( file + 12, AFX_FLAG_CONTROLLED );
	afx_write32( file + 16, 96 );			// image offset
	afx_write32( file + 20, 1 );			// image size
	afx_write32( file + 24, 0 );			// stream offset: the image's start, no setups
	afx_write32( file + 28, 1 );			// stream size
	afx_write32( file + 32, afx_control_id( image, 1 ) );
	afx_write32( file + 64, voices );		// required channels
	afx_write32( file + 68, 1000 );			// ticks: 1000 a second
	afx_write32( file + 72, 1 );
	return afx_bank_flow_upload( NULL, file, 96 + 1, &dc_flow );
}

qboolean AICA_Init( int voices, unsigned *base, unsigned *bytes ) {
	afx_mem_stats_t	stats;
	int				err;

	err = afx_init( aica_firmware, aica_firmware_end - aica_firmware );
	if ( err ) {
		Com_Printf( S_COLOR_RED "AICA: driver didn't start (%d)\n", err );
		return qfalse;
	}
	err = DC_AICA_VoiceFlow( voices );
	if ( !err ) {
		err = afx_instance_activate( dc_flow, &dc_voices );
	}
	if ( err ) {
		Com_Printf( S_COLOR_RED "AICA: no voices (%d)\n", err );
		afx_shutdown();
		return qfalse;
	}
	dc_numVoices = voices;

	// every byte left, for snd_aica.c
	if ( afx_mem_stats( &stats ) || stats.largest_free_block < 256 * 1024 ) {
		Com_Printf( S_COLOR_RED "AICA: no sound RAM\n" );
		afx_shutdown();
		return qfalse;
	}
	*bytes = stats.largest_free_block & ~31u;
	dc_arena = afx_mem_alloc( *bytes, 32 );
	if ( !dc_arena ) {
		Com_Printf( S_COLOR_RED "AICA: no sound RAM\n" );
		afx_shutdown();
		return qfalse;
	}
	*base = dc_arena;
	Com_Printf( "AICA: %d voices, %u K of sound RAM at 0x%x\n", voices, *bytes >> 10, *base );
	return qtrue;
}

void AICA_Shutdown( void ) {
	afx_shutdown();
	dc_flow = 0;
	dc_voices = 0;
	dc_arena = 0;
}

void AICA_Write( unsigned addr, const void *data, int bytes ) {
	if ( bytes > 0 ) {
		afx_mem_upload( addr, data, bytes );
	}
}

void AICA_Read( unsigned addr, void *data, int bytes ) {
	if ( bytes > 0 ) {
		spu_memread( data, addr, bytes );
	}
}

qboolean AICA_Voice( int voice, unsigned mask, const unsigned short *values ) {
	int err = afx_instance_patch( dc_voices, voice, mask, values );

	if ( err == -AFX_IPC_FULL || err == -AFX_BUSY ) {
		return qfalse;
	}
	if ( err ) {
		Com_DPrintf( "AICA: patch voice %d: %d\n", voice, err );
	}
	return qtrue;
}

void AICA_Update( void ) {
	afx_update();
}
