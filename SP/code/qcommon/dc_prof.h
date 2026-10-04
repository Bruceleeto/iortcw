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
	PROF_CGAME,         // the client game module: what the ones below don't have
	PROF_CG_SNAPS,      //   CG_ProcessSnapshots: entity events (shots, impacts, sounds)
	PROF_CG_PREDICT,    //   CG_PredictPlayerState
	PROF_CG_ENTS,       //   CG_AddPacketEntities, less players
	PROF_CG_PLAYERS,    //     CG_Player
	PROF_CG_TAGS,       //   R_LerpTag, from wherever (tags of MDS: bones)
	PROF_CG_STATIC,     //   CG_AddStaticEntities
	PROF_CG_MARKS,      //   CG_AddMarks
	PROF_CG_PARTICLES,  //   CG_AddParticles
	PROF_CG_LOCALENTS,  //   CG_AddLocalEntities: gibs, brass, smoke, blood
	PROF_CG_WEAPON,     //   CG_AddViewWeapon
	PROF_CG_TRAILS,     //   CG_AddFlameChunks, CG_AddTrails
	PROF_CG_2D,         //   CG_DrawActive, less its scene
	PROF_UI,            // the menus
	PROF_SCENE,         // the renderer's front end: RE_RenderScene
	PROF_DRAW,          // its back end: the render commands, to the PVR's lists;
						// what the ones below don't have (2D, sorting, state)
	PROF_WORLD,         //   the world's surfaces into tess (rb_surfaceTable)
	PROF_MODELS,        //   entities' surfaces into tess: model lerp, MDS skinning
	PROF_SHADE,         //   a shader's stages: what the ones below don't have (state, binds, draws' setup)
	PROF_DEFORM,        //     deformVertexes (RB_DeformTessGeometry)
	PROF_COLORS,        //     rgbGen / alphaGen (ComputeColors)
	PROF_LIGHTING,      //     model lighting (RB_CalcDiffuseColor)
	PROF_TEXCOORDS,     //     tcGen / tcMod (ComputeTexCoords)
	PROF_DLIGHTS,       //     dynamic lights' passes (ProjectDlightTexture), less their pvr
	PROF_FOG,           //     fog passes (RB_FogPass), less their pvr
	PROF_SKY,           //   the sky's (RB_StageIteratorSky) and the sun
	PROF_FLARES,        //   RB_RenderFlares
	PROF_PVR,           //   pvr_gl: transform, clip, PVR vertices into the lists
	PROF_SUBMIT,        //   pvr_gl: the lists to the PVR at the end of the frame
	PROF_GPU,           // waiting for the PVR to finish the frame before
	PROF_SOUND,         // S_Update
	PROF_IDLE,          // Com_Frame waiting for its next frame (com_maxfps)
	PROF_NUM
} profSection_t;

/* counts a frame, printed under the PROF line as RSTAT */
typedef enum {
	STAT_DRAWS,         // pvr_gl draw calls
	STAT_VERTS,         // vertexes transformed (once each a draw)
	STAT_TRIS,          // triangles in
	STAT_CULLED,        // of them, back facing or off screen
	STAT_CLIPPED,       // of them, through the near plane
	STAT_EMITTED,       // PVR vertexes written
	STAT_FRAMEHITS,     // R_MDSFrame found in its cache
	STAT_FRAMEMISSES,   // R_MDSFrame decoded
	STAT_BONECALLS,     // R_CalcBones
	STAT_BONEMISSES,    // of them, an entity not in the bone cache
	STAT_TR,            // translucent list entries (vertexes + headers)
	STAT_VBUFMAX,       // most TA vertex buffer KB a frame used (a peak, not an average)
	STAT_NUM
} profStat_t;

#ifdef DC_PROF
extern int profStats[STAT_NUM];
#define PROF_COUNT( stat, n )   ( profStats[stat] += ( n ) )
#define PROF_MAX( stat, n )     ( profStats[stat] = profStats[stat] > ( n ) ? profStats[stat] : ( n ) )
#else
#define PROF_COUNT( stat, n )
#define PROF_MAX( stat, n )
#endif

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
