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

// client.h -- primary header for client

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../renderer/tr_public.h"
#include "../ui/ui_public.h"
#include "keys.h"
#include "snd_public.h"
#include "../cgame/cg_public.h"
#include "../game/bg_public.h"



// file full of random crap that gets used to create cl_guid
#define QKEY_FILE "wkey"
#define QKEY_SIZE 2048

#define LIMBOCHAT_WIDTH		140     // NERVE - SMF
#define LIMBOCHAT_HEIGHT	7       // NERVE - SMF

// the newest snapshot the server has built for the client; cgame reads
// them all from the server with CL_GetSnapshot
typedef struct {
	qboolean valid;                 // there has been one this gamestate
	int snapFlags;                  // SNAPFLAG_NOT_ACTIVE, etc
	int serverTime;                 // server time the snapshot is valid for (in msec)
	int messageNum;                 // the snapshot number
	playerState_t ps;               // complete information about the current player at this time
} clSnapshot_t;


/*
=============================================================================

the clientActive_t structure is wiped completely at every
new gamestate_t, potentially several times during an established connection

=============================================================================
*/



extern int g_console_field_width;

typedef struct {
	clSnapshot_t snap;              // latest received from server

	int serverTime;                 // may be paused during play
	int oldServerTime;              // to prevent time from flowing bakcwards
	int oldFrameServerTime;         // to check tournament restarts
	int serverTimeDelta;            // cl.serverTime = cls.realtime + cl.serverTimeDelta
									// this value changes as net lag varies
	qboolean extrapolatedSnapshot;      // set if any cgame frame has been forced to extrapolate
	// cleared when CL_AdjustTimeDelta looks at it
	qboolean newSnapshots;          // set when the server has built a new one

	gameState_t gameState;          // configstrings
	char mapname[MAX_QPATH];        // extracted from CS_SERVERINFO

	int mouseDx[2], mouseDy[2];         // added to by mouse events
	int mouseIndex;
	int joystickAxis[MAX_JOYSTICK_AXIS];            // set by joystick events

	// cgame communicates a few values to the client system
	int cgameUserCmdValue;              // current weapon to add to usercmd_t
	int cgameUserHoldableValue;         // current holdable item to add to usercmd_t	//----(SA)	added
	float cgameSensitivity;
	int cgameCld;                       // NERVE - SMF

	// cmds[cmdNumber] is the predicted command, [cmdNumber-1] is the last
	// properly generated command
	usercmd_t cmds[CMD_BACKUP];     // each mesage will send several old cmds
	int cmdNumber;                  // incremented each frame
	int sentCmdNumber;              // the newest one the server has run

	// the client maintains its own idea of view angles, which are
	// sent to the server each frame.  It is cleared to 0 upon entering each level.
	// the server sends a delta each frame which is added to the locally
	// tracked view angles to account for standing on rotating objects,
	// and teleport direction changes
	vec3_t viewangles;

	int serverId;                   // included in each client message so the server
									// can tell if it is for a prior map_restart
	// NERVE - SMF
	char limboChatMsgs[LIMBOCHAT_HEIGHT][LIMBOCHAT_WIDTH * 3 + 1];
	int limboChatPos;
	// -NERVE - SMF

	qboolean cameraMode;    //----(SA)	added for control of input while watching cinematics

} clientActive_t;

extern clientActive_t cl;

/*
=============================================================================

the clientConnection_t structure is wiped when disconnecting from a server,
either to go to a full screen console or connect to a different server

A connection is to a server through the network layer.

=============================================================================
*/


typedef struct {

	connstate_t	state;				// connection status

	int clientNum;

	char		servername[MAX_OSPATH];		// "localhost", shown while connecting
	int connectPacketCount;                 // for display on connection dialog
	char serverMessage[MAX_STRING_TOKENS];          // for display on connection dialog

	int checksumFeed;                       // from the server for checksum calculations

	// our commands to the server, until it has run them
	int reliableSequence;
	int reliableAcknowledge;                // the last one the server has executed
	char *reliableCommands[MAX_RELIABLE_COMMANDS];

	// server snapshot and command sequence numbers are NOT cleared at
	// level changes, but continue to increase as long as the connection
	// is valid
	int serverMessageSequence;              // the newest snapshot number

	// the server keeps its commands for us until we have run them
	int serverCommandSequence;
	int lastExecutedServerCommand;              // last server command grabbed or executed with CL_GetServerCommand
} clientConnection_t;

extern clientConnection_t clc;

/*
==================================================================

the clientStatic_t structure is never wiped, and is used even when
no client connection is active at all
(except when CL_Shutdown is called)
==================================================================
*/


typedef struct {

	qboolean cddialog;              // bring up the cd needed dialog next frame
	qboolean endgamemenu;           // bring up the end game credits menu next frame

	// when the server clears the hunk, all of these must be restarted
	qboolean rendererStarted;
	qboolean soundStarted;
	qboolean soundRegistered;
	qboolean uiStarted;
	qboolean cgameStarted;

	int framecount;
	int frametime;                  // msec since last frame

	int realtime;                   // ignores pause
	int realFrametime;              // ignoring pause, so console always works

	char updateInfoString[MAX_INFO_STRING];	// shown by the connect screen

	// rendering info
	glconfig_t glconfig;
	qhandle_t charSetShader;
	qhandle_t whiteShader;
	qhandle_t consoleShader;
	qhandle_t consoleShader2;   //----(SA)	added
} clientStatic_t;

extern clientStatic_t cls;

extern	char		cl_oldGame[MAX_QPATH];
extern	qboolean	cl_oldGameSet;

//=============================================================================

extern vm_t            *cgvm;   // interface to cgame dll or vm
extern vm_t            *uivm;   // interface to ui dll or vm
extern refexport_t re;          // interface to refresh .dll


//
// cvars
//
extern cvar_t  *cl_debugMove;
extern cvar_t  *cl_noprint;
extern cvar_t  *cl_timegraph;
extern cvar_t  *cl_timeNudge;
extern cvar_t  *cl_showTimeDelta;

extern cvar_t  *cl_yawspeed;
extern cvar_t  *cl_pitchspeed;
extern cvar_t  *cl_run;
extern cvar_t  *cl_anglespeedkey;

extern cvar_t  *cl_recoilPitch;     // RF

extern cvar_t  *cl_sensitivity;
extern cvar_t  *cl_freelook;

extern cvar_t  *cl_mouseAccel;
extern	cvar_t	*cl_mouseAccelOffset;
extern	cvar_t	*cl_mouseAccelStyle;
extern cvar_t  *cl_showMouseRate;

extern cvar_t  *m_pitch;
extern cvar_t  *m_yaw;
extern cvar_t  *m_forward;
extern cvar_t  *m_side;
extern cvar_t  *m_filter;

extern	cvar_t	*j_pitch;
extern	cvar_t	*j_yaw;
extern	cvar_t	*j_forward;
extern	cvar_t	*j_side;
extern	cvar_t	*j_up;
extern	cvar_t	*j_pitch_axis;
extern	cvar_t	*j_yaw_axis;
extern	cvar_t	*j_forward_axis;
extern	cvar_t	*j_side_axis;
extern	cvar_t	*j_up_axis;


extern cvar_t  *cl_activeAction;

extern cvar_t  *cl_conXOffset;
extern cvar_t  *cl_inGameVideo;


extern	cvar_t	*cl_consoleKeys;



extern cvar_t  *cl_missionStats;
extern cvar_t  *cl_waitForFire;

// NERVE - SMF - localization
extern cvar_t  *cl_language;
// -NERVE - SMF

//=================================================

//
// cl_main
//

void CL_Init( void );
void CL_AddReliableCommand(const char *cmd, qboolean isDisconnectCmd);
void CL_FreeReliableCommands( int last );
void CL_DownloadsComplete( void );

void CL_StartHunkUsers( qboolean rendererOnly );

void CL_Disconnect_f( void );
void CL_Vid_Restart_f( void );
void CL_Snd_Restart_f( void );



void CL_ShutdownRef( void );
void CL_InitRef( void );
qboolean CL_CDKeyValidate( const char *key, const char *checksum );

qboolean CL_CheckPaused(void);

void CL_AddToLimboChat( const char *str );                  // NERVE - SMF
qboolean CL_GetLimboString( int index, char *buf );         // NERVE - SMF

//
// cl_input
//
typedef struct {
	int down[2];                // key nums holding it down
	unsigned downtime;          // msec timestamp
	unsigned msec;              // msec down this frame if both a down and up happened
	qboolean active;            // current state
	qboolean wasPressed;        // set when down, not cleared when up
} kbutton_t;

typedef enum {
	KB_LEFT,
	KB_RIGHT,
	KB_FORWARD,
	KB_BACK,
	KB_LOOKUP,
	KB_LOOKDOWN,
	KB_MOVELEFT,
	KB_MOVERIGHT,
	KB_STRAFE,
	KB_SPEED,
	KB_UP,
	KB_DOWN,
	KB_BUTTONS0,
	KB_BUTTONS1,
	KB_BUTTONS2,
	KB_BUTTONS3,
	KB_BUTTONS4,
	KB_BUTTONS5,
	KB_BUTTONS6,
	KB_BUTTONS7,
	KB_WBUTTONS0,
	KB_WBUTTONS1,
	KB_WBUTTONS2,
	KB_WBUTTONS3,
	KB_WBUTTONS4,
	KB_WBUTTONS5,
	KB_WBUTTONS6,
	KB_WBUTTONS7,
	KB_MLOOK,
	KB_KICK,
	NUM_BUTTONS
} kbuttons_t;


void CL_ClearKeys( void );

void CL_InitInput( void );
void CL_ShutdownInput( void );
void CL_SendCmd( void );
void CL_ClearState( void );

void CL_WritePacket( void );
void IN_CenterView( void );
void IN_Notebook( void );
void IN_Help( void );

//----(SA) salute
void IN_Salute( void );
//----(SA)

void CL_VerifyCode( void );

float CL_KeyState( kbutton_t *key );
int Key_StringToKeynum( char *str );
char *Key_KeynumToString( int keynum, qboolean bTranslate );

//
// cl_parse.c
//
extern int cl_connectedToPureServer;
extern int cl_connectedToCheatServer;


void CL_SystemInfoChanged( void );
void CL_LocalFrame( void );

//====================================================================

void    CL_FavoriteServers_f( void );


//
// console
//
void Con_DrawCharacter( int cx, int line, int num );

void Con_CheckResize( void );
void Con_Init( void );
void Con_Shutdown( void );
void Con_Clear_f( void );
void Con_ToggleConsole_f( void );
void Con_DrawNotify( void );
void Con_ClearNotify( void );
void Con_RunConsole( void );
void Con_DrawConsole( void );
void Con_PageUp( void );
void Con_PageDown( void );
void Con_Top( void );
void Con_Bottom( void );
void Con_Close( void );

void CL_LoadConsoleHistory( void );
void CL_SaveConsoleHistory( void );

//
// cl_scrn.c
//
void    SCR_Init( void );
void    SCR_UpdateScreen( void );

void	SCR_DebugGraph (float value);

int     SCR_GetBigStringWidth( const char *str );   // returns in virtual 640x480 coordinates

void    SCR_AdjustFrom640( float *x, float *y, float *w, float *h );
void    SCR_FillRect( float x, float y, float width, float height,
					  const float *color );
void    SCR_DrawPic( float x, float y, float width, float height, qhandle_t hShader );
void    SCR_DrawNamedPic( float x, float y, float width, float height, const char *picname );

void	SCR_DrawBigString( int x, int y, const char *s, float alpha, qboolean noColorEscape );			// draws a string with embedded color control characters with fade
void	SCR_DrawBigStringColor( int x, int y, const char *s, vec4_t color, qboolean noColorEscape );	// ignores embedded color control characters
void	SCR_DrawSmallStringExt( int x, int y, const char *string, float *setColor, qboolean forceColor, qboolean noColorEscape );
void    SCR_DrawSmallChar( int x, int y, int ch );


//
// cl_cin.c
//

void CL_PlayCinematic_f( void );
void SCR_DrawCinematic( void );
void SCR_RunCinematic( void );
void SCR_StopCinematic( void );
int CIN_PlayCinematic( const char *arg0, int xpos, int ypos, int width, int height, int bits );
e_status CIN_StopCinematic( int handle );
e_status CIN_RunCinematic( int handle );
void CIN_DrawCinematic( int handle );
void CIN_SetExtents( int handle, int x, int y, int w, int h );
void CIN_SetLooping( int handle, qboolean loop );
void CIN_UploadCinematic( int handle );
void CIN_CloseAllVideos( void );

//
// cl_cgame.c
//
void CL_InitCGame( void );
void CL_ShutdownCGame( void );
qboolean CL_GameCommand( void );
void CL_CGameRendering( stereoFrame_t stereo );
void CL_SetCGameTime( void );
void CL_FirstSnapshot( void );
void CL_ShaderStateChanged( void );
void CL_UpdateLevelHunkUsage( void );
//
// cl_ui.c
//
void CL_InitUI( void );
void CL_ShutdownUI( void );
int Key_GetCatcher( void );
void Key_SetCatcher( int catcher );
