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

// cl_main.c  -- client main loop

#include "client.h"
#include <limits.h>

#include "../sys/sys_local.h"
#include "../sys/sys_loadlib.h"




#ifdef USE_RENDERER_DLOPEN
cvar_t	*cl_renderer;
#endif

cvar_t  *cl_debugMove;

cvar_t  *cl_noprint;


cvar_t  *cl_timeNudge;
cvar_t  *cl_showTimeDelta;


cvar_t  *cl_freelook;
cvar_t  *cl_sensitivity;

cvar_t  *cl_mouseAccel;
cvar_t	*cl_mouseAccelOffset;
cvar_t	*cl_mouseAccelStyle;
cvar_t  *cl_showMouseRate;

cvar_t  *m_pitch;
cvar_t  *m_yaw;
cvar_t  *m_forward;
cvar_t  *m_side;
cvar_t  *m_filter;

cvar_t	*j_pitch;
cvar_t	*j_yaw;
cvar_t	*j_forward;
cvar_t	*j_side;
cvar_t	*j_up;
cvar_t	*j_pitch_axis;
cvar_t	*j_yaw_axis;
cvar_t	*j_forward_axis;
cvar_t	*j_side_axis;
cvar_t	*j_up_axis;

cvar_t  *cl_activeAction;

cvar_t  *cl_motdString;

cvar_t  *cl_conXOffset;
cvar_t  *cl_inGameVideo;

cvar_t  *cl_serverStatusResendTime;
cvar_t  *cl_missionStats;
cvar_t  *cl_waitForFire;

// NERVE - SMF - localization
cvar_t  *cl_language;
cvar_t  *cl_debugTranslation;
// -NERVE - SMF


cvar_t	*cl_consoleKeys;

cvar_t	*cl_rate;

clientActive_t cl;
clientConnection_t clc;
clientStatic_t cls;
vm_t                *cgvm;

char				cl_oldGame[MAX_QPATH];
qboolean			cl_oldGameSet;

// Structure containing functions exported from refresh DLL
refexport_t re;
#ifdef USE_RENDERER_DLOPEN
static void	*rendererLib = NULL;
#endif

static int noGameRestart = qfalse;

extern void SV_BotFrame( int time );
void CL_CheckForResend( void );


/*
==============
CL_EndgameMenu

Called by Com_Error when a game has ended and is dropping out to main menu in the "endgame" menu ('credits' right now)
==============
*/
void CL_EndgameMenu( void ) {
	cls.endgamemenu = qtrue;    // start it next frame
}


/*
===============
CL_CDDialog

Called by Com_Error when a cd is needed
===============
*/
void CL_CDDialog( void ) {
	cls.cddialog = qtrue;   // start it next frame
}




/*
=======================================================================

CLIENT RELIABLE COMMAND COMMUNICATION

=======================================================================
*/

/*
======================
CL_AddReliableCommand

The given command will be transmitted to the server, and is gauranteed to
not have future usercmd_t executed before it is executed
======================
*/
void CL_AddReliableCommand(const char *cmd, qboolean isDisconnectCmd)
{
	int unacknowledged = clc.reliableSequence - clc.reliableAcknowledge;

	// if we would be losing an old command that hasn't been acknowledged,
	// we must drop the connection
	// also leave one slot open for the disconnect command in this case.
	
	if ((isDisconnectCmd && unacknowledged > MAX_RELIABLE_COMMANDS) ||
	    (!isDisconnectCmd && unacknowledged >= MAX_RELIABLE_COMMANDS))
	{
		if(com_errorEntered)
			return;
		else
			Com_Error(ERR_DROP, "Client command overflow");
	}

	clc.reliableSequence++;
	clc.reliableCommands[clc.reliableSequence & (MAX_RELIABLE_COMMANDS - 1)] = CopyString( cmd );
}

/*
======================
CL_FreeReliableCommands

Frees the commands up to and including the given one
======================
*/
void CL_FreeReliableCommands( int last )
{
	int i;
	char **cmd;

	for ( i = last ; i > last - MAX_RELIABLE_COMMANDS ; i-- ) {
		cmd = &clc.reliableCommands[i & (MAX_RELIABLE_COMMANDS - 1)];
		if ( *cmd ) {
			Z_Free( *cmd );
			*cmd = NULL;
		}
	}
}

//======================================================================

/*
=====================
CL_ShutdownAll
=====================
*/
void CL_ShutdownAll(qboolean shutdownRef)
{

	// clear sounds
	S_DisableSounds();
	// shutdown CGame
	CL_ShutdownCGame();
	// shutdown UI
	CL_ShutdownUI();

	// shutdown the renderer
	if(shutdownRef)
		CL_ShutdownRef();
	else if(re.Shutdown)
		re.Shutdown(qfalse);		// don't destroy window or context

	cls.uiStarted = qfalse;
	cls.cgameStarted = qfalse;
	cls.rendererStarted = qfalse;
	cls.soundRegistered = qfalse;
}

/*
=================
CL_ClearMemory

Called by Com_GameRestart
=================
*/
void CL_ClearMemory(qboolean shutdownRef)
{
	// shutdown all the client stuff
	CL_ShutdownAll(shutdownRef);

	// if not running a server clear the whole hunk
	if ( !com_sv_running || !com_sv_running->integer ) {
		// clear the whole hunk
		Hunk_Clear();
		// clear collision map data
		CM_ClearMap();
	} else {
		// clear all the client data on the hunk
		Hunk_ClearToMark();
	}
}

/*
=================
CL_FlushMemory

Called by CL_MapLoading, CL_Connect_f, and CL_ParseGamestate the only
ways a client gets into a game
Also called by Com_Error
=================
*/
void CL_FlushMemory(void)
{
	CL_ClearMemory(qfalse);
	CL_StartHunkUsers(qfalse);
}

/*
=====================
CL_MapLoading

A local server is starting to load a map, so update the
screen to let the user know about it, then dump all client
memory on the hunk from cgame, ui, and renderer
=====================
*/
void CL_MapLoading( void ) {
	if ( com_dedicated->integer ) {
		clc.state = CA_DISCONNECTED;
		Key_SetCatcher( KEYCATCH_CONSOLE );
		return;
	}

	if ( !com_cl_running->integer ) {
		return;
	}

	Con_Close();
	Key_SetCatcher( 0 );

	// if we are already connected to the local host, stay connected
	if ( clc.state >= CA_CONNECTED && !Q_stricmp( clc.servername, "localhost" ) ) {
		clc.state = CA_CONNECTED;		// so the connect screen is drawn
		Com_Memset( cls.updateInfoString, 0, sizeof( cls.updateInfoString ) );
		Com_Memset( clc.serverMessage, 0, sizeof( clc.serverMessage ) );
		Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );
		SCR_UpdateScreen();
	} else {
		// clear nextmap so the cinematic shutdown doesn't execute it
		Cvar_Set( "nextmap", "" );
		CL_Disconnect( qtrue );
		Q_strncpyz( clc.servername, "localhost", sizeof(clc.servername) );
		clc.state = CA_CHALLENGING;		// so the connect screen is drawn
		Key_SetCatcher( 0 );
		SCR_UpdateScreen();
		CL_CheckForResend();
	}

	// make sure sound is quiet
//	S_FadeAllSounds( 0, 0 );
}

/*
=====================
CL_ClearState

Called before parsing a gamestate
=====================
*/
void CL_ClearState( void ) {

	S_StopAllSounds();

	memset( &cl, 0, sizeof( cl ) );
}

/*
====================
CL_UpdateGUID

update cl_guid using QKEY_FILE and optional prefix
====================
*/
static void CL_UpdateGUID( const char *prefix, int prefix_len )
{
	fileHandle_t f;
	int len;

	len = FS_SV_FOpenFileRead( QKEY_FILE, &f );
	FS_FCloseFile( f );

	if( len != QKEY_SIZE )
		Cvar_Set( "cl_guid", "" );
	else
		Cvar_Set( "cl_guid", Com_MD5File( QKEY_FILE, QKEY_SIZE,
			prefix, prefix_len ) );
}

static void CL_OldGame(void)
{
	if(cl_oldGameSet)
	{
		// change back to previous fs_game
		cl_oldGameSet = qfalse;
		Cvar_Set2("fs_game", cl_oldGame, qtrue);
		FS_ConditionalRestart(clc.checksumFeed, qfalse);
	}
}

/*
=====================
CL_Disconnect

Called when a connection or cinematic is being terminated.
Goes from a connected state to either a menu state or a console state
Sends a disconnect message to the server
This is also called on Com_Error and Com_Quit, so it shouldn't cause any errors
=====================
*/
void CL_Disconnect( qboolean showMainMenu ) {
	if ( !com_cl_running || !com_cl_running->integer ) {
		return;
	}

	// shutting down the client so enter full screen ui mode
	Cvar_Set( "r_uiFullScreen", "1" );

	if ( uivm && showMainMenu ) {
		VM_Call( uivm, UI_SET_ACTIVE_MENU, UIMENU_NONE );
	}

	SCR_StopCinematic();
	S_ClearSoundBuffer();

	// tell the server
	if ( clc.state >= CA_CONNECTED ) {
		CL_AddReliableCommand("disconnect", qtrue);
		CL_WritePacket();
	}

	// Remove pure paks
	FS_PureServerSetLoadedPaks("", "");
	FS_PureServerSetReferencedPaks( "", "" );

	CL_ClearState();

	CL_FreeReliableCommands( clc.reliableSequence );

	// wipe the client connection
	Com_Memset( &clc, 0, sizeof( clc ) );

	clc.state = CA_DISCONNECTED;

	// allow cheats locally
#ifndef WOLF_SP_DEMO
	// except for demo
	Cvar_Set( "sv_cheats", "1" );
#endif

	// not connected to a pure server anymore
	cl_connectedToPureServer = qfalse;


	CL_UpdateGUID( NULL, 0 );

	if(!noGameRestart)
		CL_OldGame();
	else
		noGameRestart = qfalse;
}

/*
===================
CL_ForwardCommandToServer

adds the current command line as a clientCommand
things like godmode, noclip, etc, are commands directed to the server,
so when they are typed in at the console, they will need to be forwarded.
===================
*/
void CL_ForwardCommandToServer( const char *string ) {
	char	*cmd;

	cmd = Cmd_Argv( 0 );

	// ignore key up commands
	if ( cmd[0] == '-' ) {
		return;
	}

	if ( clc.state < CA_CONNECTED || cmd[0] == '+' ) {
		Com_Printf ("Unknown command \"%s" S_COLOR_WHITE "\"\n", cmd);
		return;
	}

	if ( Cmd_Argc() > 1 ) {
		CL_AddReliableCommand(string, qfalse);
	} else {
		CL_AddReliableCommand(cmd, qfalse);
	}
}

/*
===================
CL_RequestMotd

===================
*/
void CL_RequestMotd( void ) {
}

/*
===================
CL_RequestAuthorization

Authorization server protocol
-----------------------------

All commands are text in Q3 out of band packets (leading 0xff 0xff 0xff 0xff).

Whenever the client tries to get a challenge from the server it wants to
connect to, it also blindly fires off a packet to the authorize server:

getKeyAuthorize <challenge> <cdkey>

cdkey may be "demo"


#OLD The authorize server returns a:
#OLD
#OLD keyAthorize <challenge> <accept | deny>
#OLD
#OLD A client will be accepted if the cdkey is valid and it has not been used by any other IP
#OLD address in the last 15 minutes.


The server sends a:

getIpAuthorize <challenge> <ip>

The authorize server returns a:

ipAuthorize <challenge> <accept | deny | demo | unknown >

A client will be accepted if a valid cdkey was sent by that ip (only) in the last 15 minutes.
If no response is received from the authorize server after two tries, the client will be let
in anyway.
===================
*/
#ifndef STANDALONE
#endif

/*
======================================================================

CONSOLE COMMANDS

======================================================================
*/

/*
==================
CL_ForwardToServer_f
==================
*/
void CL_ForwardToServer_f( void ) {
	if ( clc.state != CA_ACTIVE ) {
		Com_Printf( "Not connected to a server.\n" );
		return;
	}

	// don't forward the first argument
	if ( Cmd_Argc() > 1 ) {
		CL_AddReliableCommand(Cmd_Args(), qfalse);
	}
}

/*
==================
CL_Disconnect_f
==================
*/
void CL_Disconnect_f( void ) {
	SCR_StopCinematic();
	// RF, make sure loading variables are turned off
	Cvar_Set( "savegame_loading", "0" );
	Cvar_Set( "g_reloading", "0" );
	if ( clc.state != CA_DISCONNECTED && clc.state != CA_CINEMATIC ) {
		Com_Error( ERR_DISCONNECT, "Disconnected from server" );
	}
}


/*
=================
CL_Vid_Restart_f

Restart the video subsystem

we also have to reload the UI and CGame because the renderer
doesn't know what graphics to reload
=================
*/
void CL_Vid_Restart_f( void ) {

	vmCvar_t musicCvar;

	// RF, don't show percent bar, since the memory usage will just sit at the same level anyway
	Cvar_Set( "com_expectedhunkusage", "-1" );

	// don't let them loop during the restart
	S_StopAllSounds();

	if(!FS_ConditionalRestart(clc.checksumFeed, qtrue))
	{
		// if not running a server clear the whole hunk
		if(com_sv_running->integer)
		{
			// clear all the client data on the hunk
			Hunk_ClearToMark();
		}
		else
		{
			// clear the whole hunk
			Hunk_Clear();
		}

		// shutdown the UI
		CL_ShutdownUI();
		// shutdown the CGame
		CL_ShutdownCGame();
		// shutdown the renderer and clear the renderer interface
		CL_ShutdownRef();
		// clear pak references
		FS_ClearPakReferences( FS_UI_REF | FS_CGAME_REF );
		// reinitialize the filesystem if the game directory or checksum has changed

		cls.rendererStarted = qfalse;
		cls.uiStarted = qfalse;
		cls.cgameStarted = qfalse;
		cls.soundRegistered = qfalse;

		// unpause so the cgame definately gets a snapshot and renders a frame
		Cvar_Set( "cl_paused", "0" );

		// initialize the renderer interface
		CL_InitRef();

		// startup all the client stuff
		CL_StartHunkUsers(qfalse);

		// start the cgame if connected
		if(clc.state > CA_CONNECTED && clc.state != CA_CINEMATIC)
		{
			cls.cgameStarted = qtrue;
			CL_InitCGame();
		}

		// start music if there was any

		Cvar_Register( &musicCvar, "s_currentMusic", "", CVAR_ROM );
		if ( strlen( musicCvar.string ) ) {
			S_StartBackgroundTrack( musicCvar.string, musicCvar.string );
		}

		// fade up volume
//		S_FadeAllSounds( 1, 0 );
	}
	LOAD_STEP( "vid_restart" );
}

/*
=================
CL_Snd_Shutdown

Shut down the sound subsystem
=================
*/
void CL_Snd_Shutdown(void)
{
	S_Shutdown();
	cls.soundStarted = qfalse;
}

/*
=================
CL_Snd_Restart_f

Restart the sound subsystem
The cgame and game must also be forced to restart because
handles will be invalid
=================
*/
void CL_Snd_Restart_f(void)
{
	CL_Snd_Shutdown();
	// sound will be reinitialized by vid_restart
	CL_Vid_Restart_f();
}

/*
==================
CL_PK3List_f
==================
*/
void CL_OpenedPK3List_f( void ) {
	Com_Printf( "Opened PK3 Names: %s\n", FS_LoadedPakNames() );
}

/*
==================
CL_PureList_f
==================
*/
void CL_ReferencedPK3List_f( void ) {
	Com_Printf( "Referenced PK3 Names: %s\n", FS_ReferencedPakNames() );
}

/*
==================
CL_Configstrings_f
==================
*/
void CL_Configstrings_f( void ) {
	int	i;
	int	ofs;

	if ( clc.state != CA_ACTIVE ) {
		Com_Printf( "Not connected to a server.\n" );
		return;
	}

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		ofs = cl.gameState.stringOffsets[ i ];
		if ( !ofs ) {
			continue;
		}
		Com_Printf( "%4i: %s\n", i, cl.gameState.stringData + ofs );
	}
}

/*
==============
CL_Clientinfo_f
==============
*/
void CL_Clientinfo_f( void ) {
	Com_Printf( "--------- Client Information ---------\n" );
	Com_Printf( "state: %i\n", clc.state );
	Com_Printf( "Server: %s\n", clc.servername );
	Com_Printf( "User info settings:\n" );
	Info_Print( Cvar_InfoString( CVAR_USERINFO ) );
	Com_Printf( "--------------------------------------\n" );
}


//====================================================================

/*
=================
CL_DownloadsComplete

Called when all downloading has been completed
=================
*/
void CL_DownloadsComplete( void ) {
	// let the client game init and load data
	clc.state = CA_LOADING;

	Com_EventLoop();

	// if the gamestate was changed by calling Com_EventLoop
	// then we loaded everything already and we don't want to do it again.
	if ( clc.state != CA_LOADING ) {
		return;
	}

	// starting to load a map so we get out of full screen ui mode
	Cvar_Set( "r_uiFullScreen", "0" );

	// flush client memory and start loading stuff
	// this will also (re)load the UI
	// if this is a local client then only the client part of the hunk
	// will be cleared, note that this is done after the hunk mark has been set
	CL_FlushMemory();

	// initialize the CGame
	cls.cgameStarted = qtrue;
	CL_InitCGame();
}

/*
=================
CL_CheckForResend

Connect to the local server once it is running a level
=================
*/
void CL_CheckForResend( void ) {
	int clientNum;

	if ( clc.state != CA_CONNECTING && clc.state != CA_CHALLENGING ) {
		return;
	}

	// the server says no until it has a level running
	clc.connectPacketCount++;
	clientNum = SV_LocalConnect( Cvar_InfoString( CVAR_USERINFO ) );
	if ( clientNum < 0 ) {
		return;
	}

	// the most current userinfo has been sent, so watch for any
	// newer changes to userinfo variables
	cvar_modifiedFlags &= ~CVAR_USERINFO;

	clc.clientNum = clientNum;
	clc.state = CA_CONNECTED;
}

/*
==================
CL_CheckPaused
Check whether client has been paused.
==================
*/
qboolean CL_CheckPaused(void)
{
	// if cl_paused->modified is set, the cvar has only been changed in
	// this frame. Keep paused in this frame to ensure the server doesn't
	// lag behind.
	if(cl_paused->integer || cl_paused->modified)
		return qtrue;

	return qfalse;
}


//============================================================================

/*
==================
CL_CheckUserinfo

==================
*/
void CL_CheckUserinfo( void ) {
	// don't add reliable commands when not yet connected
	if(clc.state < CA_CONNECTED)
		return;

	// don't overflow the reliable command buffer when paused
	if(CL_CheckPaused())
		return;

	// send a reliable userinfo update if needed
	if ( cvar_modifiedFlags & CVAR_USERINFO ) {
		cvar_modifiedFlags &= ~CVAR_USERINFO;
		CL_AddReliableCommand(va("userinfo \"%s\"", Cvar_InfoString( CVAR_USERINFO ) ), qfalse);
	}
}

#ifdef DCSIM
/*
==================
CL_DCSimNewGame

+set dcsim_newgame 1: play the start of the game as a player would, from the
menu: New Game (play.menu's action), the pregame menu's arrow after each
load (uiScript playerstart), skip the cutscene as a key does, then
quit 10 seconds into escape1; at once on an error, and after a minute
whatever happens. 2: die 3 seconds into escape1 (kill), fire till the
level is loaded again as dying does, and quit 10 seconds after.
+set dcsim_map <map>: straight to that map (spmap) instead, for escape1
==================
*/
static void CL_DCSimNewGame( void ) {
	static int start, started, escape, lastSkip, pregameServerId = -1;
	static int killed, killedServerId, lastFire, reloaded;
	int now = Sys_Milliseconds();
	const char *map = Cvar_VariableString( "dcsim_map" );

	if ( !Cvar_VariableIntegerValue( "dcsim_newgame" ) ) {
		return;
	}
	if ( !map[0] ) {
		map = "escape1";
	}
	if ( !start ) {
		start = now;
	}
	if ( now - start > 60000 ) {
		Com_Printf( "DCSIM newgame: a minute and not done, quitting\n" );
		Cbuf_AddText( "quit\n" );
		return;
	}
	if ( !started ) {
		// the menu is up
		if ( clc.state == CA_DISCONNECTED && ( Key_GetCatcher() & KEYCATCH_UI ) && now - start > 1000 ) {
			Com_Printf( "DCSIM newgame: New Game\n" );
			Cbuf_AddText( va( "set g_gameskill 1; spmap %s\n", Q_stricmp( map, "escape1" ) ? map : "cutscene1" ) );
			started = now;
		}
		return;
	}
	if ( clc.state == CA_DISCONNECTED && now - started > 2000 ) {
		Com_Printf( "DCSIM newgame: back at the menu (an error), quitting\n" );
		Cbuf_AddText( "quit\n" );
		return;
	}
	if ( clc.state != CA_ACTIVE ) {
		return;
	}
	// the pregame menu's arrow: what uiScript playerstart does
	if ( ( Key_GetCatcher() & KEYCATCH_UI ) && cl.serverId != pregameServerId ) {
		Com_Printf( "DCSIM newgame: the pregame arrow\n" );
		Cbuf_AddText( "fade 0 0 0 0 3\n" );
		Cvar_Set( "g_playerstart", "1" );
		VM_Call( uivm, UI_SET_ACTIVE_MENU, UIMENU_NONE );
		pregameServerId = cl.serverId;
		return;
	}
	if ( cl.cameraMode && now - lastSkip > 500 ) {
		CL_AddReliableCommand( "cameraInterrupt", qfalse );
		lastSkip = now;
	}
	if ( Cvar_VariableIntegerValue( "dcsim_newgame" ) == 2 && escape ) {
		if ( !killed && now - escape > 3000 ) {
			Com_Printf( "DCSIM newgame: dying\n" );
			CL_AddReliableCommand( "kill", qfalse );
			killed = now;
			killedServerId = cl.serverId;
		} else if ( killed && !reloaded && cl.serverId != killedServerId ) {
			Com_Printf( "DCSIM newgame: loaded again\n" );
			Com_MemoryReport( "loaded again" );
			Cbuf_AddText( "-attack\n" );
			reloaded = now;
		} else if ( killed && !reloaded && now - killed > 2000 && now - lastFire > 500 ) {
			Cbuf_AddText( ( now / 500 ) & 1 ? "+attack\n" : "-attack\n" );
			lastFire = now;
		} else if ( reloaded && now - reloaded > 10000 ) {
			Com_Printf( "DCSIM newgame: 10 seconds after, quitting\n" );
			Com_MemoryReport( "10 seconds after dying" );
			Cbuf_AddText( "quit\n" );
			reloaded = now + 100000;
		}
		return;
	}
	if ( !Q_stricmp( Cvar_VariableString( "mapname" ), map ) ) {
		if ( !escape ) {
			Com_Printf( "DCSIM newgame: in %s\n", map );
			escape = now;
		} else if ( now - escape > 10000 ) {
			Com_Printf( "DCSIM newgame: 10 seconds into %s, quitting\n", map );
			Com_MemoryReport( va( "10 seconds into %s", map ) );
			Cbuf_AddText( "quit\n" );
			escape = now + 100000;
		}
	}
}
#endif

/*
==================
CL_Frame

==================
*/
void CL_Frame( int msec ) {

	if ( !com_cl_running->integer ) {
		return;
	}
#ifdef DCSIM
	CL_DCSimNewGame();
#endif


	if ( cls.cddialog ) {
		// bring up the cd error dialog if needed
		cls.cddialog = qfalse;
		VM_Call( uivm, UI_SET_ACTIVE_MENU, UIMENU_NEED_CD );
	} else if ( cls.endgamemenu ) {
		cls.endgamemenu = qfalse;
		VM_Call( uivm, UI_SET_ACTIVE_MENU, UIMENU_ENDGAME );
	} else	if ( clc.state == CA_DISCONNECTED && !( Key_GetCatcher( ) & KEYCATCH_UI )
		&& !com_sv_running->integer && uivm ) {
		// if disconnected, bring up the menu
		S_StopAllSounds();
		VM_Call( uivm, UI_SET_ACTIVE_MENU, UIMENU_MAIN );
	}

	// save the msec before checking pause
	cls.realFrametime = msec;

	// decide the simulation time
	cls.frametime = msec;

	cls.realtime += cls.frametime;

	if ( cl_timegraph->integer ) {
		SCR_DebugGraph ( cls.realFrametime * 0.25 );
	}

	// see if we need to update any userinfo
	CL_CheckUserinfo();

	// send intentions now
	CL_SendCmd();

	// connect to the server if necessary
	CL_CheckForResend();

	// pick up the gamestate and the snapshot
	CL_LocalFrame();

	// decide on the serverTime to render
	CL_SetCGameTime();

	// update the screen
	SCR_UpdateScreen();

	// update audio
	S_Update();



	// advance local effects for next frame
	SCR_RunCinematic();

	Con_RunConsole();

	cls.framecount++;
}



/*
================
CL_MapRestart_f
================
*/
void CL_MapRestart_f( void ) {
	if ( !com_cl_running ) {
		return;
	}
	if ( !com_cl_running->integer ) {
		return;
	}
	Com_Printf( "This command is no longer functional.\nUse \"loadgame current\" to load the current map." );
}

/*
================
CL_SetRecommended_f
================
*/
void CL_SetRecommended_f( void ) {
	if ( Cmd_Argc() > 1 ) {
		Com_SetRecommended( qtrue );
	} else {
		Com_SetRecommended( qfalse );
	}

}

/*
================
CL_RefPrintf

DLL glue
================
*/
static __attribute__ ((format (printf, 2, 3))) void QDECL CL_RefPrintf( int print_level, const char *fmt, ...) {
	va_list	argptr;
	char	msg[MAXPRINTMSG];

	va_start( argptr,fmt );
	Q_vsnprintf( msg, sizeof ( msg ), fmt, argptr );
	va_end( argptr );

	if ( print_level == PRINT_ALL ) {
		Com_Printf( "%s", msg );
	} else if ( print_level == PRINT_WARNING ) {
		Com_Printf( S_COLOR_YELLOW "%s", msg );		// yellow
	} else if ( print_level == PRINT_ERROR ) {
		Com_Printf (S_COLOR_RED "%s", msg);		// red
	} else if ( print_level == PRINT_DEVELOPER ) {
		Com_DPrintf( S_COLOR_RED "%s", msg );		// red - developer only
	}
}

/*
============
CL_ShutdownRef
============
*/
void CL_ShutdownRef( void ) {
	if ( re.Shutdown ) {
		re.Shutdown( qtrue );
	}

	memset( &re, 0, sizeof( re ) );

#ifdef USE_RENDERER_DLOPEN
	if ( rendererLib ) {
		Sys_UnloadLibrary( rendererLib );
		rendererLib = NULL;
	}
#endif
}

/*
============
CL_InitRenderer
============
*/
void CL_InitRenderer( void ) {
	// this sets up the renderer and calls R_Init
	re.BeginRegistration( &cls.glconfig );

	// load character sets
	cls.charSetShader = re.RegisterShader( "gfx/2d/bigchars" );
	cls.whiteShader = re.RegisterShader( "white" );
	cls.consoleShader = re.RegisterShader( "console" );
	cls.consoleShader2 = re.RegisterShader( "console2" );
	g_console_field_width = cls.glconfig.vidWidth / SMALLCHAR_WIDTH - 2;
	g_consoleField.widthInChars = g_console_field_width;
}

/*
============================
CL_StartHunkUsers

After the server has cleared the hunk, these will need to be restarted
This is the only place that any of these functions are called from
============================
*/
void CL_StartHunkUsers( qboolean rendererOnly ) {
	if ( !com_cl_running ) {
		return;
	}

	if ( !com_cl_running->integer ) {
		return;
	}

	if ( !cls.rendererStarted ) {
		cls.rendererStarted = qtrue;
		CL_InitRenderer();
		LOAD_STEP( "renderer start" );
	}

	if ( rendererOnly ) {
		return;
	}

	if ( !cls.soundStarted ) {
		cls.soundStarted = qtrue;
		S_Init();
		LOAD_STEP( "sound start" );
	}

	if ( !cls.soundRegistered ) {
		cls.soundRegistered = qtrue;
		S_BeginRegistration();
	}

	if( com_dedicated->integer ) {
		return;
	}

	if ( !cls.uiStarted ) {
		cls.uiStarted = qtrue;
#if defined( _arch_dreamcast ) || defined( DCSIM )
		// its menus and text in one read, not hundreds (tools/ui_blob.sh)
		FS_LoadBlob( "ui.blob" );
#endif
		CL_InitUI();
#if defined( _arch_dreamcast ) || defined( DCSIM )
		FS_FreeBlob();
#endif
		LOAD_STEP( "ui start" );
	}
}

int CL_ScaledMilliseconds( void ) {
	return Sys_Milliseconds() * com_timescale->value;
}

/*
============
CL_InitRef
============
*/
void CL_InitRef( void ) {
	refimport_t ri;
	refexport_t *ret;
#ifdef USE_RENDERER_DLOPEN
	GetRefAPI_t		GetRefAPI;
	char			dllName[MAX_OSPATH];
#endif

	Com_Printf( "----- Initializing Renderer ----\n" );

#ifdef USE_RENDERER_DLOPEN
	cl_renderer = Cvar_Get("cl_renderer", "opengl1", CVAR_ARCHIVE | CVAR_LATCH);

	Com_sprintf(dllName, sizeof(dllName), "renderer_sp_%s_" ARCH_STRING DLL_EXT, cl_renderer->string);

	if(!(rendererLib = Sys_LoadDll(dllName, qfalse)) && strcmp(cl_renderer->string, cl_renderer->resetString))
	{
		Com_Printf("failed:\n\"%s\"\n", Sys_LibraryError());
		Cvar_ForceReset("cl_renderer");

		Com_sprintf(dllName, sizeof(dllName), "renderer_sp_opengl1_" ARCH_STRING DLL_EXT);
		rendererLib = Sys_LoadDll(dllName, qfalse);
	}

	if(!rendererLib)
	{
		Com_Printf("failed:\n\"%s\"\n", Sys_LibraryError());
		Com_Error(ERR_FATAL, "Failed to load renderer");
	}

	GetRefAPI = Sys_LoadFunction(rendererLib, "GetRefAPI");
	if(!GetRefAPI)
	{
		Com_Error(ERR_FATAL, "Can't load symbol GetRefAPI: '%s'",  Sys_LibraryError());
	}
#endif

	ri.Cmd_AddCommand = Cmd_AddCommand;
	ri.Cmd_RemoveCommand = Cmd_RemoveCommand;
	ri.Cmd_Argc = Cmd_Argc;
	ri.Cmd_Argv = Cmd_Argv;
	ri.Cmd_ExecuteText = Cbuf_ExecuteText;
	ri.Printf = CL_RefPrintf;
	ri.Error = Com_Error;
	ri.Milliseconds = CL_ScaledMilliseconds;

	ri.Z_Malloc = Z_Malloc;
	ri.Free = Z_Free;
	ri.Hunk_Clear = Hunk_ClearToMark;
#ifdef HUNK_DEBUG
	ri.Hunk_AllocDebug = Hunk_AllocDebug;
#else
	ri.Hunk_Alloc = Hunk_Alloc;
#endif
	ri.Hunk_AllocateTempMemory = Hunk_AllocateTempMemory;
	ri.Hunk_FreeTempMemory = Hunk_FreeTempMemory;

	ri.CM_ClusterPVS = CM_ClusterPVS;
	ri.CM_OpenBsp = CM_OpenBsp;
	ri.CM_ReadLump = CM_ReadLump;
	ri.CM_ReadLumpInto = CM_ReadLumpInto;
	ri.CM_FreeLump = CM_FreeLump;
	ri.CM_WorldPlanes = CM_WorldPlanes;
	ri.CM_WorldVis = CM_WorldVis;
	ri.CM_EntityString = CM_EntityString;
	ri.FS_FOpenFileRead = FS_FOpenFileRead;
	ri.FS_FCloseFile = FS_FCloseFile;
	ri.FS_Read = FS_Read;
	ri.FS_Seek = FS_Seek;
	ri.CM_DrawDebugSurface = CM_DrawDebugSurface;
	ri.FS_ReadFile = FS_ReadFile;
	ri.FS_FreeFile = FS_FreeFile;
	ri.FS_WriteFile = FS_WriteFile;
	ri.FS_FreeFileList = FS_FreeFileList;
	ri.FS_ListFiles = FS_ListFiles;
	ri.FS_FileIsInPAK = FS_FileIsInPAK;
	ri.FS_FileExists = FS_FileExists;
	ri.Cvar_Get = Cvar_Get;
	ri.Cvar_Set = Cvar_Set;
	ri.Cvar_SetValue = Cvar_SetValue;
	ri.Cvar_CheckRange = Cvar_CheckRange;
	ri.Cvar_VariableIntegerValue = Cvar_VariableIntegerValue;

	// cinematic stuff

	ri.CIN_UploadCinematic = CIN_UploadCinematic;
	ri.CIN_PlayCinematic = CIN_PlayCinematic;
	ri.CIN_RunCinematic = CIN_RunCinematic;


	ri.IN_Init = IN_Init;
	ri.IN_Shutdown = IN_Shutdown;
	ri.IN_Restart = IN_Restart;

	ri.ftol = Q_ftol;

	ri.Sys_SetEnv = Sys_SetEnv;
	ri.Sys_GLimpSafeInit = Sys_GLimpSafeInit;
	ri.Sys_GLimpInit = Sys_GLimpInit;
	ri.Sys_LowPhysicalMemory = Sys_LowPhysicalMemory;

	ret = GetRefAPI( REF_API_VERSION, &ri );

	if ( !ret ) {
		Com_Error( ERR_FATAL, "Couldn't initialize refresh" );
	}

	re = *ret;

	Com_Printf( "---- Renderer Initialization Complete ----\n" );

	// unpause so the cgame definately gets a snapshot and renders a frame
	Cvar_Set( "cl_paused", "0" );
}

// RF, trap manual client damage commands so users can't issue them manually
void CL_ClientDamageCommand( void ) {
	// do nothing
}

#if defined (__i386__)
#define BIN_STRING "x86"
#endif

// NERVE - SMF
void CL_startMultiplayer_f( void ) {
	char binName[MAX_OSPATH];

#if defined(_WIN64) || defined(__WIN64__)
	Com_sprintf(binName, sizeof(binName), "ioWolfMP." ARCH_STRING ".exe");
	Sys_StartProcess( binName, qtrue );
#elif defined(_WIN32) || defined(__WIN32__)
	Com_sprintf(binName, sizeof(binName), "ioWolfMP." ARCH_STRING ".exe");
	Sys_StartProcess( binName, qtrue );
#elif defined(__i386__) && (!defined(_WIN32) || !defined(__WIN32__))
	Com_sprintf(binName, sizeof(binName), "./iowolfmp." BIN_STRING );
	Sys_StartProcess( binName, qtrue );
#else
	Com_sprintf(binName, sizeof(binName), "./iowolfmp." ARCH_STRING );
	Sys_StartProcess( binName, qtrue );
#endif
}
// -NERVE - SMF

//----(SA) added
/*
==============
CL_ShellExecute_URL_f
Format:
  shellExecute "open" <url> <doExit>

TTimo
  show_bug.cgi?id=447
  only supporting "open" syntax for URL openings, others are not portable or need to be added on a case-by-case basis
  the shellExecute syntax as been kept to remain compatible with win32 SP demo pk3, but this thing only does open URL

==============
*/

void CL_ShellExecute_URL_f( void ) {
	qboolean doexit;

	Com_DPrintf( "CL_ShellExecute_URL_f\n" );

	if ( Q_stricmp( Cmd_Argv( 1 ),"open" ) ) {
		Com_DPrintf( "invalid CL_ShellExecute_URL_f syntax (shellExecute \"open\" <url> <doExit>)\n" );
		return;
	}

	if ( Cmd_Argc() < 4 ) {
		doexit = qtrue;
	} else {
		doexit = (qboolean)( atoi( Cmd_Argv( 3 ) ) );
	}

	Sys_OpenURL( Cmd_Argv( 2 ),doexit );
}
//----(SA) end

//===========================================================================================


/*
===============
CL_GenerateQKey

test to see if a valid QKEY_FILE exists.  If one does not, try to generate
it by filling it with 2048 bytes of random data.
===============
*/
static void CL_GenerateQKey(void)
{
	int len = 0;
	unsigned char buff[ QKEY_SIZE ];
	fileHandle_t f;

#ifdef _arch_dreamcast
	return;		// nowhere to keep it
#endif

	len = FS_SV_FOpenFileRead( QKEY_FILE, &f );
	FS_FCloseFile( f );
	if( len == QKEY_SIZE ) {
		Com_Printf( "RTCWKEY found.\n" );
		return;
	}
	else {
		if( len > 0 ) {
			Com_Printf( "RTCWKEY file size != %d, regenerating\n",
				QKEY_SIZE );
		}

		Com_Printf( "RTCWKEY building random string\n" );
		Com_RandomBytes( buff, sizeof(buff) );

		f = FS_SV_FOpenFileWrite( QKEY_FILE );
		if( !f ) {
			Com_Printf( "RTCWKEY could not open %s for write\n",
				QKEY_FILE );
			return;
		}
		FS_Write( buff, sizeof(buff), f );
		FS_FCloseFile( f );
		Com_Printf( "RTCWKEY generated\n" );
	}
}

/*
====================
CL_Init
====================
*/
void CL_Init( void ) {
	Com_Printf( "----- Client Initialization -----\n" );

	Con_Init();

	if(!com_fullyInitialized)
	{
		CL_ClearState();
		clc.state = CA_DISCONNECTED;	// no longer CA_UNINITIALIZED
		cl_oldGameSet = qfalse;
	}

	cls.realtime = 0;

	CL_InitInput();

	//
	// register our variables
	//
	cl_noprint = Cvar_Get( "cl_noprint", "0", 0 );

	cl_timeNudge = Cvar_Get( "cl_timeNudge", "0", CVAR_TEMP );
	cl_showTimeDelta = Cvar_Get( "cl_showTimeDelta", "0", CVAR_TEMP );
	cl_activeAction = Cvar_Get( "activeAction", "", CVAR_TEMP );

	cl_yawspeed = Cvar_Get( "cl_yawspeed", "140", CVAR_ARCHIVE );
	cl_pitchspeed = Cvar_Get( "cl_pitchspeed", "140", CVAR_ARCHIVE );
	cl_anglespeedkey = Cvar_Get( "cl_anglespeedkey", "1.5", 0 );

	cl_run = Cvar_Get( "cl_run", "1", CVAR_ARCHIVE );
	cl_sensitivity = Cvar_Get( "sensitivity", "5", CVAR_ARCHIVE );
	cl_mouseAccel = Cvar_Get( "cl_mouseAccel", "0", CVAR_ARCHIVE );
	cl_freelook = Cvar_Get( "cl_freelook", "1", CVAR_ARCHIVE );

	// 0: legacy mouse acceleration
	// 1: new implementation
	cl_mouseAccelStyle = Cvar_Get( "cl_mouseAccelStyle", "0", CVAR_ARCHIVE );
	// offset for the power function (for style 1, ignored otherwise)
	// this should be set to the max rate value
	cl_mouseAccelOffset = Cvar_Get( "cl_mouseAccelOffset", "5", CVAR_ARCHIVE );
	Cvar_CheckRange(cl_mouseAccelOffset, 0.001f, 50000.0f, qfalse);

	cl_showMouseRate = Cvar_Get( "cl_showmouserate", "0", 0 );
#ifdef USE_CURL_DLOPEN
	cl_cURLLib = Cvar_Get("cl_cURLLib", DEFAULT_CURL_LIB, CVAR_ARCHIVE | CVAR_PROTECTED);
#endif

	// init autoswitch so the ui will have it correctly even
	// if the cgame hasn't been started
	Cvar_Get( "cg_autoswitch", "2", CVAR_ARCHIVE );

	// Rafael - particle switch
	Cvar_Get( "cg_wolfparticles", "1", CVAR_ARCHIVE );
	// done

	cl_conXOffset = Cvar_Get( "cl_conXOffset", "0", 0 );
	cl_inGameVideo = Cvar_Get( "r_inGameVideo", "1", CVAR_ARCHIVE );

	cl_serverStatusResendTime = Cvar_Get( "cl_serverStatusResendTime", "750", 0 );

	// RF
	cl_recoilPitch = Cvar_Get( "cg_recoilPitch", "0", CVAR_ROM );

	m_pitch = Cvar_Get( "m_pitch", "0.022", CVAR_ARCHIVE );
	m_yaw = Cvar_Get( "m_yaw", "0.022", CVAR_ARCHIVE );
	m_forward = Cvar_Get( "m_forward", "0.25", CVAR_ARCHIVE );
	m_side = Cvar_Get( "m_side", "0.25", CVAR_ARCHIVE );
	m_filter = Cvar_Get( "m_filter", "0", CVAR_ARCHIVE );

	j_pitch =        Cvar_Get ("j_pitch",        "0.022", CVAR_ARCHIVE);
	j_yaw =          Cvar_Get ("j_yaw",          "-0.022", CVAR_ARCHIVE);
	j_forward =      Cvar_Get ("j_forward",      "-0.25", CVAR_ARCHIVE);
	j_side =         Cvar_Get ("j_side",         "0.25", CVAR_ARCHIVE);
	j_up =           Cvar_Get ("j_up",           "0", CVAR_ARCHIVE);

	j_pitch_axis =   Cvar_Get ("j_pitch_axis",   "3", CVAR_ARCHIVE);
	j_yaw_axis =     Cvar_Get ("j_yaw_axis",     "2", CVAR_ARCHIVE);
	j_forward_axis = Cvar_Get ("j_forward_axis", "1", CVAR_ARCHIVE);
	j_side_axis =    Cvar_Get ("j_side_axis",    "0", CVAR_ARCHIVE);
	j_up_axis =      Cvar_Get ("j_up_axis",      "4", CVAR_ARCHIVE);

	Cvar_CheckRange(j_pitch_axis, 0, MAX_JOYSTICK_AXIS-1, qtrue);
	Cvar_CheckRange(j_yaw_axis, 0, MAX_JOYSTICK_AXIS-1, qtrue);
	Cvar_CheckRange(j_forward_axis, 0, MAX_JOYSTICK_AXIS-1, qtrue);
	Cvar_CheckRange(j_side_axis, 0, MAX_JOYSTICK_AXIS-1, qtrue);
	Cvar_CheckRange(j_up_axis, 0, MAX_JOYSTICK_AXIS-1, qtrue);

	cl_motdString = Cvar_Get( "cl_motdString", "", CVAR_ROM );

	Cvar_Get( "cl_maxPing", "800", CVAR_ARCHIVE );

	// ~ and `, as keys and characters
	cl_consoleKeys = Cvar_Get( "cl_consoleKeys", "~ ` 0x7e 0x60", CVAR_ARCHIVE);

	// userinfo
	Cvar_Get( "name", "WolfPlayer", CVAR_USERINFO | CVAR_ARCHIVE );
	cl_rate = Cvar_Get( "rate", "25000", CVAR_USERINFO | CVAR_ARCHIVE );     // NERVE - SMF - changed from 3000
	Cvar_Get( "snaps", "20", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get( "model", "bj2", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get( "head", "default", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get( "color", "4", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get( "handicap", "100", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get( "sex", "male", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get( "cl_anonymous", "0", CVAR_USERINFO | CVAR_ARCHIVE );

	Cvar_Get( "password", "", CVAR_USERINFO );
	Cvar_Get( "cg_predictItems", "1", CVAR_USERINFO | CVAR_ARCHIVE );



//----(SA) added
	Cvar_Get( "cg_autoactivate", "1", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get( "cg_emptyswitch", "0", CVAR_USERINFO | CVAR_ARCHIVE );
//----(SA) end

	// cgame might not be initialized before menu is used
	Cvar_Get( "cg_viewsize", "100", CVAR_ARCHIVE );
	// Make sure cg_stereoSeparation is zero as that variable is deprecated and should not be used anymore.
	Cvar_Get ("cg_stereoSeparation", "0", CVAR_ROM);

	cl_missionStats = Cvar_Get( "g_missionStats", "0", CVAR_ROM );
	cl_waitForFire = Cvar_Get( "cl_waitForFire", "0", CVAR_ROM );

	// NERVE - SMF - localization
	cl_language = Cvar_Get( "cl_language", "0", CVAR_ARCHIVE );
	cl_debugTranslation = Cvar_Get( "cl_debugTranslation", "0", 0 );
	// -NERVE - SMF

	//
	// register our commands
	//
	Cmd_AddCommand( "cmd", CL_ForwardToServer_f );
	Cmd_AddCommand( "configstrings", CL_Configstrings_f );
	Cmd_AddCommand( "clientinfo", CL_Clientinfo_f );
	Cmd_AddCommand( "snd_restart", CL_Snd_Restart_f );
	Cmd_AddCommand( "vid_restart", CL_Vid_Restart_f );
	Cmd_AddCommand( "disconnect", CL_Disconnect_f );
	Cmd_AddCommand( "cinematic", CL_PlayCinematic_f );
	Cmd_AddCommand( "fs_openedList", CL_OpenedPK3List_f );
	Cmd_AddCommand( "fs_referencedList", CL_ReferencedPK3List_f );

	// Ridah, startup-caching system

	Cmd_AddCommand( "updatehunkusage", CL_UpdateLevelHunkUsage );
	Cmd_AddCommand( "updatescreen", SCR_UpdateScreen );
	// done.

	// RF, add this command so clients can't bind a key to send client damage commands to the server
	Cmd_AddCommand( "cld", CL_ClientDamageCommand );

	Cmd_AddCommand( "startMultiplayer", CL_startMultiplayer_f );        // NERVE - SMF

	Cmd_AddCommand( "shellExecute", CL_ShellExecute_URL_f );

	// RF, prevent users from issuing a map_restart manually
	Cmd_AddCommand( "map_restart", CL_MapRestart_f );

	Cmd_AddCommand( "setRecommended", CL_SetRecommended_f );

	CL_InitRef();

	SCR_Init();

//	Cbuf_Execute();

	Cvar_Set( "cl_running", "1" );

	CL_GenerateQKey();
	Cvar_Get( "cl_guid", "", CVAR_USERINFO | CVAR_ROM );
	CL_UpdateGUID( NULL, 0 );

	Com_Printf( "----- Client Initialization Complete -----\n" );
}


/*
===============
CL_Shutdown

===============
*/
void CL_Shutdown( char *finalmsg, qboolean disconnect, qboolean quit ) {
	static qboolean recursive = qfalse;

	// check whether the client is running at all.
	if(!(com_cl_running && com_cl_running->integer))
		return;

	Com_Printf( "----- Client Shutdown (%s) -----\n", finalmsg );

	if ( recursive ) {
		Com_Printf( "WARNING: Recursive shutdown\n" );
		return;
	}
	recursive = qtrue;

	noGameRestart = quit;

	if(disconnect)
		CL_Disconnect(qtrue);

	CL_ClearMemory(qtrue);
	CL_Snd_Shutdown();

	Cmd_RemoveCommand( "cmd" );
	Cmd_RemoveCommand( "configstrings" );
	Cmd_RemoveCommand ("clientinfo");
	Cmd_RemoveCommand( "snd_restart" );
	Cmd_RemoveCommand( "vid_restart" );
	Cmd_RemoveCommand( "disconnect" );
	Cmd_RemoveCommand( "cinematic" );
	Cmd_RemoveCommand( "connect" );
	Cmd_RemoveCommand ("reconnect");
	Cmd_RemoveCommand( "rcon" );
	Cmd_RemoveCommand( "showip" );
	Cmd_RemoveCommand ("fs_openedList");
	Cmd_RemoveCommand ("fs_referencedList");
	Cmd_RemoveCommand( "model" );

	// Ridah, startup-caching system

	Cmd_RemoveCommand( "updatehunkusage" );
	// done.

	CL_ShutdownInput();
	Con_Shutdown();

	Cvar_Set( "cl_running", "0" );

	recursive = qfalse;

	memset( &cls, 0, sizeof( cls ) );
	Key_SetCatcher( 0 );

	Com_Printf( "-----------------------\n" );
}


/*
=================
CL_CDKeyValidate
=================
*/
qboolean CL_CDKeyValidate( const char *key, const char *checksum ) {
#ifdef STANDALONE
	return qtrue;
#else
	char ch;
	byte sum;
	char chs[3];
	int i, len;

	len = strlen( key );
	if ( len != CDKEY_LEN ) {
		return qfalse;
	}

	if ( checksum && strlen( checksum ) != CDCHKSUM_LEN ) {
		return qfalse;
	}

	sum = 0;
	// for loop gets rid of conditional assignment warning
	for ( i = 0; i < len; i++ ) {
		ch = *key++;
		if ( ch >= 'a' && ch <= 'z' ) {
			ch -= 32;
		}
		switch ( ch ) {
		case '2':
		case '3':
		case '7':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'G':
		case 'H':
		case 'J':
		case 'L':
		case 'P':
		case 'R':
		case 'S':
		case 'T':
		case 'W':
			sum += ch;
			continue;
		default:
			return qfalse;
		}
	}

	Com_sprintf( chs, sizeof( chs ), "%02x", sum );

	if ( checksum && !Q_stricmp( chs, checksum ) ) {
		return qtrue;
	}

	if ( !checksum ) {
		return qtrue;
	}

	return qfalse;
#endif
}

// NERVE - SMF
/*
=======================
CL_AddToLimboChat

=======================
*/
void CL_AddToLimboChat( const char *str ) {
	int len = 0;
	char *p;
	int i;

	cl.limboChatPos = LIMBOCHAT_HEIGHT - 1;

	// copy old strings
	for ( i = cl.limboChatPos; i > 0; i-- ) {
		strcpy( cl.limboChatMsgs[i], cl.limboChatMsgs[i - 1] );
	}

	// copy new string
	p = cl.limboChatMsgs[0];
	*p = 0;

	while ( *str ) {
		if ( len > LIMBOCHAT_WIDTH - 1 ) {
			break;
		}

		if ( Q_IsColorString( str ) ) {
			*p++ = *str++;
			*p++ = *str++;
			continue;
		}
		*p++ = *str++;
		len++;
	}
	*p = 0;
}

/*
=======================
CL_GetLimboString

=======================
*/
qboolean CL_GetLimboString( int index, char *buf ) {
	if ( index >= LIMBOCHAT_HEIGHT ) {
		return qfalse;
	}

	strncpy( buf, cl.limboChatMsgs[index], 140 );
	return qtrue;
}
// -NERVE - SMF
