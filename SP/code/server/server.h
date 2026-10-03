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

// server.h

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../game/g_public.h"
#include "../game/bg_public.h"

//=============================================================================

#define PERS_SCORE              0       // !!! MUST NOT CHANGE, SERVER AND
										// GAME BOTH REFERENCE !!!

#define MAX_ENT_CLUSTERS    8       // (was 16) past it, the range lastCluster gives is checked


typedef struct svEntity_s {
	struct worldSector_s *worldSector;
	struct svEntity_s *nextEntityInWorldSector;

	int numClusters;                // if -1, use headnode instead
	int clusternums[MAX_ENT_CLUSTERS];
	int lastCluster;                // if all the clusters don't fit in clusternums
	int areanum, areanum2;
	int snapshotCounter;            // used to prevent double adding from portal views
} svEntity_t;

typedef enum {
	SS_DEAD,            // no map loaded
	SS_LOADING,         // spawning level entities
	SS_GAME             // actively running
} serverState_t;

typedef struct {
	serverState_t state;
	qboolean restarting;                // if true, send configstring changes during SS_LOADING
	int serverId;                       // changes each server start
	int restartedServerId;              // serverId before a map_restart
	int checksumFeed;                   //
	int snapshotCounter;                // incremented for each snapshot built
	int timeResidual;                   // <= 1000 / sv_frame->value
	int nextFrameTime;                  // when time > nextFrameTime, process world
	char            *configstrings[MAX_CONFIGSTRINGS];
	svEntity_t svEntities[MAX_GENTITIES];

	char            *entityParsePoint;  // used during game VM init

	// the game virtual machine will update these on init and changes
	sharedEntity_t  *gentities;
	int gentitySize;
	int num_entities;                   // current number, <= MAX_GENTITIES

	playerState_t   *gameClients;
	int gameClientSize;                 // will be > sizeof(playerState_t) due to game private data

	int restartTime;
	int				time;
} server_t;





typedef struct {
	int areabytes;
	byte areabits[MAX_MAP_AREA_BYTES];                  // portalarea visibility bits
	playerState_t ps;
	int num_entities;
	int first_entity;                   // into the circular svs.snapshotEntities[]
	int serverTime;
	int snapFlags;
	int serverCommandNum;               // the client runs the server commands up to this first
} clientSnapshot_t;

typedef enum {
	CS_FREE,        // can be reused for a new connection
	CS_ZOMBIE,      // client has been disconnected, but don't reuse
					// connection for a couple seconds
	CS_CONNECTED,   // has been assigned to a client_t, but no gamestate yet
	CS_PRIMED,      // gamestate has been sent, but client hasn't sent a usercmd
	CS_ACTIVE       // client is fully in game
} clientState_t;

// RF, now using a global string buffer to hold all reliable commands
//#define	RELIABLE_COMMANDS_MULTI		128
//#define	RELIABLE_COMMANDS_SINGLE	256		// need more for loadgame situations

#define RELIABLE_COMMANDS_START     ( 16 * 1024 )   // what a client's buffer starts at; it grows when full

typedef struct {
	int bufSize;
	char    *buf;               // actual strings
	char    **commands;         // pointers to actual strings
	int     *commandLengths;    // lengths of actual strings
	//
	char    *rover;
} reliableCommands_t;

typedef struct client_s {
	clientState_t state;
	qboolean bot;                           // an AI, not the player
	char userinfo[MAX_INFO_STRING / 2];             // name, etc (was MAX_INFO_STRING: SP's are short)

	reliableCommands_t reliableCommands;
	int reliableSequence;                   // last added reliable message, not necesarily sent or acknowledged yet
	int reliableAcknowledge;                // last acknowledged reliable message

	qboolean gamestatePending;              // SV_LocalGameState hasn't picked it up yet

	usercmd_t lastUsercmd;
	int lastClientCommand;              // reliable client message sequence
	sharedEntity_t  *gentity;           // SV_GentityNum(clientnum)
	char name[MAX_NAME_LENGTH];                     // extracted from userinfo, high bits masked

	int lastPacketTime;                 // svs.time when packet was last received
	int lastConnectTime;                // svs.time when connection started
	int lastSnapshotTime;		    // svs.time of last snapshot built
	int timeoutCount;                   // must timeout a few frames in a row so debugging doesn't break
	clientSnapshot_t *frames;     // the last PACKET_BACKUP snapshots, frames[snapshotNum & PACKET_MASK] the newest
	int snapshotNum;
	int ping;

	int				oldServerTime;
	byte			csUpdated[MAX_CONFIGSTRINGS / 8];	// a bit a configstring (64 clients: 8K each as qbooleans)
} client_t;

//=============================================================================


// this structure will be cleared only when the game dll changes
typedef struct {
	qboolean initialized;                   // sv_init has completed

	int time;                               // will be strictly increasing across level changes

	int snapFlagServerBit;                  // ^= SNAPFLAG_SERVERCOUNT every SV_SpawnServer()

	client_t    *clients;                   // [sv_maxclients->integer];
	int numSnapshotEntities;                // PACKET_BACKUP*MAX_SNAPSHOT_ENTITIES
	int nextSnapshotEntities;               // next snapshotEntities to use
	entityState_t   *snapshotEntities;      // [numSnapshotEntities]
} serverStatic_t;

//=============================================================================

extern serverStatic_t svs;                  // persistant server info across maps
extern server_t sv;                         // cleared each map
extern vm_t            *gvm;                // game virtual machine

extern cvar_t  *sv_fps;
extern cvar_t  *sv_timeout;
extern cvar_t  *sv_zombietime;
extern cvar_t  *sv_maxclients;
extern cvar_t  *sv_hostname;
extern cvar_t  *sv_killserver;
extern cvar_t  *sv_mapname;
extern cvar_t  *sv_mapChecksum;
extern cvar_t  *sv_serverid;
extern cvar_t  *sv_gametype;
extern cvar_t  *sv_pure;



// Rafael gameskill
extern cvar_t  *sv_gameskill;
// done

extern cvar_t  *sv_reloading;   //----(SA)	added

//===========================================================

//
// sv_main.c
//
void QDECL SV_SendServerCommand( client_t *cl, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));


void SV_AddOperatorCommands( void );
void SV_RemoveOperatorCommands( void );



//
// sv_init.c
//
void SV_SetConfigstring( int index, const char *val );
void SV_GetConfigstring( int index, char *buffer, int bufferSize );
void SV_UpdateConfigstrings( client_t *client );

void SV_SetUserinfo( int index, const char *val );
void SV_GetUserinfo( int index, char *buffer, int bufferSize );

void SV_ChangeMaxClients( void );
void SV_SpawnServer( char *server, qboolean killBots );

//RF, reliable commands
char *SV_GetReliableCommand( client_t *cl, int index );
void SV_FreeAcknowledgedReliableCommands( client_t *cl );
qboolean SV_AddReliableCommand( client_t *cl, int index, const char *cmd );
void SV_InitReliableCommandsForClient( client_t *cl, int commands );
void SV_FreeReliableCommandsForClient( client_t *cl );


//
// sv_client.c
//

client_t *SV_LocalClient( void );
void SV_UserinfoChanged( client_t *cl );

void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd );
void SV_FreeClient(client_t *client);
clientSnapshot_t *SV_ClientFrames(client_t *client);
void SV_DropClient( client_t *drop, const char *reason );

void SV_ExecuteClientCommand( client_t *cl, const char *s, qboolean clientOK );
void SV_ClientThink( client_t *cl, usercmd_t *cmd );


//
// sv_ccmds.c
//

//
// sv_snapshot.c
//
void SV_AddServerCommand( client_t *client, const char *cmd );
void SV_SendClientMessages( void );
void SV_SendClientSnapshot( client_t *client );

//
// sv_game.c
//
int SV_NumForGentity( sharedEntity_t *ent );
sharedEntity_t *SV_GentityNum( int num );
playerState_t *SV_GameClientNum( int num );
svEntity_t  *SV_SvEntityForGentity( sharedEntity_t *gEnt );
sharedEntity_t *SV_GEntityForSvEntity( svEntity_t *svEnt );
void        SV_InitGameProgs( void );
void        SV_ShutdownGameProgs( void );
void        SV_RestartGameProgs( void );
qboolean    SV_inPVS( const vec3_t p1, const vec3_t p2 );
qboolean SV_GetTag( int clientNum, char *tagname, orientation_t * or );

//
// sv_bot.c
//
void        SV_BotFrame( int time );
int         SV_BotAllocateClient( void );
void        SV_BotFreeClient( int clientNum );

void        SV_BotInitCvars( void );
int         SV_BotLibSetup( void );
int         SV_BotLibShutdown( void );
int         SV_BotGetSnapshotEntity( int client, int ent );
int         SV_BotGetConsoleMessage( int client, char *buf, int size );

int BotImport_DebugPolygonCreate( int color, int numPoints, vec3_t *points );
void BotImport_DebugPolygonDelete( int id );

void SV_BotInitBotLib(void);

//============================================================
//
// high level object sorting to reduce interaction tests
//

void SV_ClearWorld( void );
// called after the world model has been loaded, before linking any entities

void SV_UnlinkEntity( sharedEntity_t *ent );
// call before removing an entity, and before trying to move one,
// so it doesn't clip against itself

void SV_LinkEntity( sharedEntity_t *ent );
// Needs to be called any time an entity changes origin, mins, maxs,
// or solid.  Automatically unlinks if needed.
// sets ent->r.absmin and ent->r.absmax
// sets ent->leafnums[] for pvs determination even if the entity
// is not solid


clipHandle_t SV_ClipHandleForEntity( const sharedEntity_t *ent );


void SV_SectorList_f( void );


int SV_AreaEntities( const vec3_t mins, const vec3_t maxs, int *entityList, int maxcount );
// fills in a table of entity numbers with entities that have bounding boxes
// that intersect the given area.  It is possible for a non-axial bmodel
// to be returned that doesn't actually intersect the area on an exact
// test.
// returns the number of pointers filled in
// The world entity is never returned in this list.


int SV_PointContents( const vec3_t p, int passEntityNum );
// returns the CONTENTS_* value from the world and all entities at the given point.


void SV_Trace( trace_t *results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int passEntityNum, int contentmask, int capsule );
// mins and maxs are relative

// if the entire move stays in a solid volume, trace.allsolid will be set,
// trace.startsolid will be set, and trace.fraction will be 0

// if the starting point is in a solid, it will be allowed to move out
// to an open area

// passEntityNum is explicitly excluded from clipping checks (normally ENTITYNUM_NONE)


void SV_ClipToEntity( trace_t *trace, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int entityNum, int contentmask, int capsule );
// clip to a specific entity

