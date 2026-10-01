/*
 * PC host side of the KOS PVR shim.
 *
 * On the Dreamcast, KOS supplies pvr_init()/pvr_shutdown() and the PVR scans
 * the front buffer out to the TV by itself.  On PC, kos_pvr.cpp + libpvr do
 * the rendering in software, and this file covers the rest: it brings libpvr
 * up for pvr_init(), and after every finished scene (kos_pvr_present) it
 * copies the 640x480 front buffer into an SDL window.
 *
 * Code written against <dc/pvr.h> should not need to know about this file,
 * apart from pumping SDL events like any other SDL program.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <SDL.h>

#include <dc/pvr.h>
#include "kos_pvr.h"
#include "pvr_host.h"

#define FRAME_W 640
#define FRAME_H 480

static SDL_Window   *window;
static SDL_Renderer *renderer;
static SDL_Texture  *texture;
static uint8_t      *frame_rgb;
static int          inited;

int pvr_host_open( const char *title, int scale ) {
	if ( window ) {
		return 0;
	}
	if ( scale < 1 ) {
		scale = 1;
	}
	if ( !SDL_WasInit( SDL_INIT_VIDEO ) && SDL_InitSubSystem( SDL_INIT_VIDEO ) ) {
		fprintf( stderr, "pvr_host: SDL video init failed: %s\n", SDL_GetError() );
		return -1;
	}
	window = SDL_CreateWindow( title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
							   FRAME_W * scale, FRAME_H * scale, 0 );
	if ( !window ) {
		fprintf( stderr, "pvr_host: cannot create window: %s\n", SDL_GetError() );
		return -1;
	}
	renderer = SDL_CreateRenderer( window, -1, 0 );
	if ( !renderer ) {
		renderer = SDL_CreateRenderer( window, -1, SDL_RENDERER_SOFTWARE );
	}
	if ( !renderer ) {
		fprintf( stderr, "pvr_host: cannot create renderer: %s\n", SDL_GetError() );
		SDL_DestroyWindow( window );
		window = NULL;
		return -1;
	}
	SDL_RenderSetLogicalSize( renderer, FRAME_W, FRAME_H );
	texture = SDL_CreateTexture( renderer, SDL_PIXELFORMAT_RGB24,
								 SDL_TEXTUREACCESS_STREAMING, FRAME_W, FRAME_H );
	return texture ? 0 : -1;
}

void pvr_host_close( void ) {
	if ( texture ) {
		SDL_DestroyTexture( texture );
	}
	if ( renderer ) {
		SDL_DestroyRenderer( renderer );
	}
	if ( window ) {
		SDL_DestroyWindow( window );
	}
	texture = NULL;
	renderer = NULL;
	window = NULL;
}

SDL_Window *pvr_host_window( void ) {
	return window;
}

int pvr_host_read_front( uint8_t *rgb, int stride ) {
	return kos_pvr_read_front( rgb, stride, FRAME_W, FRAME_H );
}

/* GPU_PVR_DUMP=<every>:<prefix> writes every Nth frame to <prefix><n>.ppm */
static void dump_frame( void ) {
	static int frame, every = -1;
	static char prefix[256];
	const char *env;
	char name[300];
	FILE *f;

	if ( every < 0 ) {
		every = 0;
		env = getenv( "GPU_PVR_DUMP" );
		if ( env && sscanf( env, "%d:%255s", &every, prefix ) != 2 ) {
			every = 0;
		}
	}
	frame++;
	if ( every <= 0 || frame % every ) {
		return;
	}
	snprintf( name, sizeof( name ), "%s%05d.ppm", prefix, frame );
	f = fopen( name, "wb" );
	if ( f ) {
		fprintf( f, "P6\n%d %d\n255\n", FRAME_W, FRAME_H );
		fwrite( frame_rgb, 1, FRAME_W * FRAME_H * 3, f );
		fclose( f );
	}
}

/* called by kos_pvr.cpp at the end of every non-RTT pvr_scene_finish() */
void kos_pvr_present( void ) {
	if ( !frame_rgb ) {
		return;
	}
	if ( !kos_pvr_read_front( frame_rgb, FRAME_W * 3, FRAME_W, FRAME_H ) ) {
		return;
	}
	dump_frame();
	if ( !texture ) {
		return;
	}
	SDL_UpdateTexture( texture, NULL, frame_rgb, FRAME_W * 3 );
	SDL_RenderClear( renderer );
	SDL_RenderCopy( renderer, texture, NULL, NULL );
	SDL_RenderPresent( renderer );
}

/* --- KOS entry points kos_pvr.cpp leaves out ----------------------------- */

int pvr_init( const pvr_init_params_t *params ) {
	/* kos_pvr.cpp has a fixed layout (opb {16,0,16,8,16}, 768K vertex
	 * buffer); only autosort_disabled is taken from params */

	if ( inited ) {
		return 0;
	}
	kos_pvr_set_presort( params && params->autosort_disabled );
	if ( kos_pvr_init() ) {
		return -1;
	}
	frame_rgb = malloc( FRAME_W * FRAME_H * 3 );
	if ( !frame_rgb ) {
		kos_pvr_shutdown();
		return -1;
	}
	inited = 1;
	return 0;
}

int pvr_init_defaults( void ) {
	return pvr_init( NULL );
}

int pvr_shutdown( void ) {
	if ( !inited ) {
		return 0;
	}
	kos_pvr_shutdown();
	free( frame_rgb );
	frame_rgb = NULL;
	inited = 0;
	return 0;
}
