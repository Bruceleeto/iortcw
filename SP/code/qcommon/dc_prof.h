/*
 * DC_PROF: where a frame's time goes, by system, printed every com_profile
 * seconds (Com_ProfFrame, qcommon/dc_prof.c). A system's PROF_BEGIN /
 * PROF_END mark its time; they nest, and each section counts only its own
 * time, so game is without the AI, collision and pathing it calls. What no
 * section has is the engine's. Without DC_PROF they're nothing.
 *
 * Its own header (and called directly, not through syscalls) so the game
 * modules and pvr/ use it too: everything is linked into one executable.
 */
#ifndef DC_PROF_H
#define DC_PROF_H

typedef enum {
	PROF_ENGINE,        // nothing else: events, network, client and server upkeep
	PROF_GAME,          // the game module (qagame)
	PROF_AI,            // its AI: AICast_StartFrame / AICast_StartServerFrame
	PROF_PATHING,       // the botlib (AAS routing), from the game
	PROF_COLLISION,     // traces (CM_BoxTrace, CM_TransformedBoxTrace)
	PROF_CGAME,         // the client game module
	PROF_UI,            // the menus
	PROF_SCENE,         // the renderer's front end: RE_RenderScene
	PROF_DRAW,          // its back end: the render commands, to the PVR's lists
	PROF_GPU,           // waiting for the PVR to finish the frame before
	PROF_SOUND,         // S_Update
	PROF_IDLE,          // Com_Frame waiting for its next frame (com_maxfps)
	PROF_NUM
} profSection_t;

#ifdef DC_PROF
void Com_ProfBegin( profSection_t section );
void Com_ProfEnd( profSection_t section );
#define PROF_BEGIN( section )   Com_ProfBegin( section )
#define PROF_END( section )     Com_ProfEnd( section )
#else
#define PROF_BEGIN( section )
#define PROF_END( section )
#endif

#endif
