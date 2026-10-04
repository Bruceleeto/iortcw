/*
 * GLimp for the PVR renderer: replaces sdl_glimp.c and sdl_gamma.c.
 *
 * The renderer's qgl* pointers are bound to pvr_gl.c.  The screen is always
 * 640x480.  On PC the PVR runs on tools/gpu_pvr, which owns the SDL window
 * that sdl_input.c reads its events from.
 *
 * Compiled against the game's tr_local.h.
 */

#include "tr_local.h"

#include "pvr_gl.h"

#ifdef _arch_dreamcast
#	include <dc/video.h>
#else
#	include "SDL.h"
#	include "pvr_host.h"
#endif

/* globals sdl_glimp.c would define */
#ifdef _arch_dreamcast
static void *SDL_window = NULL;		/* input needs no window on the DC */
#else
SDL_Window *SDL_window = NULL;
#endif

cvar_t *r_allowSoftwareGL;
cvar_t *r_allowResize;
cvar_t *r_centerWindow;
cvar_t *r_sdlDriver;

int qglMajorVersion, qglMinorVersion;
int qglesMajorVersion, qglesMinorVersion;

void (APIENTRYP qglActiveTextureARB) (GLenum texture);
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture);
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t);

void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count);
void (APIENTRYP qglUnlockArraysEXT) (void);

#define GLE(ret, name, ...) name##proc * qgl##name = NULL;
QGL_1_1_PROCS;
QGL_1_1_FIXED_FUNCTION_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
QGL_ES_1_1_PROCS;
QGL_ES_1_1_FIXED_FUNCTION_PROCS;
QGL_1_3_PROCS;
QGL_1_5_PROCS;
QGL_2_0_PROCS;
QGL_3_0_PROCS;
QGL_ARB_occlusion_query_PROCS;
QGL_ARB_framebuffer_object_PROCS;
QGL_ARB_vertex_array_object_PROCS;
QGL_EXT_direct_state_access_PROCS;
#undef GLE

static void GLimp_BindProcs( void ) {
#define GLE( ret, name, ... ) qgl##name = pvrgl##name;
	QGL_1_1_PROCS;
	QGL_1_1_FIXED_FUNCTION_PROCS;
	QGL_DESKTOP_1_1_PROCS;
	QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
#undef GLE
	qglMajorVersion = 1;
	qglMinorVersion = 1;
	qglesMajorVersion = 0;
	qglesMinorVersion = 0;

	/* one texture unit; compiled vertex arrays, so each surface's
	   positions are made once for all its stages */
	qglActiveTextureARB = NULL;
	qglClientActiveTextureARB = NULL;
	qglMultiTexCoord2fARB = NULL;
	qglLockArraysEXT = pvrglLockArraysEXT;
	qglUnlockArraysEXT = pvrglUnlockArraysEXT;
}

static void GLimp_ClearProcs( void ) {
#define GLE( ret, name, ... ) qgl##name = NULL;
	QGL_1_1_PROCS;
	QGL_1_1_FIXED_FUNCTION_PROCS;
	QGL_DESKTOP_1_1_PROCS;
	QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
#undef GLE
}

void GLimp_Init( qboolean fixedFunction ) {
	(void)fixedFunction;

	ri.Printf( PRINT_ALL, "Initializing PVR renderer\n" );

	r_allowSoftwareGL = ri.Cvar_Get( "r_allowSoftwareGL", "0", CVAR_LATCH );
	r_sdlDriver = ri.Cvar_Get( "r_sdlDriver", "", CVAR_ROM );
	r_allowResize = ri.Cvar_Get( "r_allowResize", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_centerWindow = ri.Cvar_Get( "r_centerWindow", "0", CVAR_ARCHIVE | CVAR_LATCH );

#ifdef _arch_dreamcast
	vid_set_mode( DM_640x480, PM_RGB565 );
#else
	if ( pvr_host_open( CLIENT_WINDOW_TITLE, 1 ) ) {
		ri.Error( ERR_FATAL, "GLimp_Init() - could not open the PVR window" );
	}
	SDL_window = pvr_host_window();
#endif
	if ( pvrgl_Init() ) {
		ri.Error( ERR_FATAL, "GLimp_Init() - could not initialise the PVR" );
	}
	GLimp_BindProcs();

	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = qfalse;
	glConfig.vidWidth = PVRGL_WIDTH;
	glConfig.vidHeight = PVRGL_HEIGHT;
	glConfig.windowAspect = (float)PVRGL_WIDTH / (float)PVRGL_HEIGHT;
	glConfig.colorBits = 16;
	glConfig.depthBits = 32;
	glConfig.stencilBits = 0;
	glConfig.displayFrequency = 60;
	glConfig.isFullscreen = qfalse;
	glConfig.stereoEnabled = qfalse;
	glConfig.textureCompression = TC_NONE;
	glConfig.textureEnvAddAvailable = qfalse;
	glConfig.numTextureUnits = 1;

	Q_strncpyz( glConfig.vendor_string, (char *)qglGetString( GL_VENDOR ), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (char *)qglGetString( GL_RENDERER ), sizeof( glConfig.renderer_string ) );
	Q_strncpyz( glConfig.version_string, (char *)qglGetString( GL_VERSION ), sizeof( glConfig.version_string ) );
	Q_strncpyz( glConfig.extensions_string, (char *)qglGetString( GL_EXTENSIONS ), sizeof( glConfig.extensions_string ) );

	ri.Cvar_Get( "r_availableModes", "", CVAR_ROM );

	ri.IN_Init( SDL_window );
}

void GLimp_Shutdown( void ) {
	ri.IN_Shutdown();

	pvrgl_Shutdown();
	GLimp_ClearProcs();
#ifndef _arch_dreamcast
	pvr_host_close();
#endif
	SDL_window = NULL;
}

void GLimp_EndFrame( void ) {
	pvrgl_EndFrame();
}

void GLimp_LogComment( char *comment ) {
	(void)comment;
}

void GLimp_Minimize( void ) {
#ifndef _arch_dreamcast
	if ( SDL_window ) {
		SDL_MinimizeWindow( SDL_window );
	}
#endif
}

void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] ) {
	/* TODO: no hardware gamma on the PVR; could be folded into textures */
	(void)red; (void)green; (void)blue;
}
