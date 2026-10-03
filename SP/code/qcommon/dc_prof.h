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
	PROF_DRAW,          // its back end: the render commands, to the PVR's lists;
						// what the ones below don't have (2D, sorting, state)
	PROF_WORLD,         //   the world's surfaces into tess (rb_surfaceTable)
	PROF_MODELS,        //   entities' surfaces into tess: model lerp, MDS skinning
	PROF_SHADE,         //   a shader's stages: colours, texcoords, deforms
	PROF_SKY,           //   the sky's (RB_StageIteratorSky) and the sun
	PROF_FLARES,        //   RB_RenderFlares
	PROF_PVR,           //   pvr_gl: transform, clip, PVR vertices into the lists
	PROF_SUBMIT,        //   pvr_gl: the lists to the PVR at the end of the frame
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

/*
 * A map load's steps: LOAD_START when one begins (SV_SpawnServer), then
 * LOAD_STEP( what ) after each, printing what that step took and the total
 * so far, until LOAD_DONE (the first snapshot, in the game):
 *
 *   LOAD collision map            412 ms  (total   1530)
 */
#ifdef DC_PROF
void Com_LoadStart( void );
void Com_LoadStep( const char *what );
void Com_LoadDone( void );
#define LOAD_START()        Com_LoadStart()
#define LOAD_STEP( what )   Com_LoadStep( what )
#define LOAD_DONE()         Com_LoadDone()
#else
#define LOAD_START()
#define LOAD_STEP( what )
#define LOAD_DONE()
#endif

#endif
