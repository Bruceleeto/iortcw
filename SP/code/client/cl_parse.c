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

// cl_parse.c  -- take the gamestate and snapshots from the local server

#include "client.h"

int cl_connectedToPureServer;
int cl_connectedToCheatServer;

/*
==================
CL_SystemInfoChanged

The systeminfo configstring has been changed, so parse
new information out of it.  This will happen at every
gamestate, and possibly during gameplay.
==================
*/
void CL_SystemInfoChanged( void ) {
	char	*systemInfo;
	const char	*s, *t;
	char	key[BIG_INFO_KEY];
	char	value[BIG_INFO_VALUE];
	qboolean	gameSet;

	systemInfo = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SYSTEMINFO ];
	cl.serverId = atoi( Info_ValueForKey( systemInfo, "sv_serverid" ) );


	s = Info_ValueForKey( systemInfo, "sv_cheats" );
	cl_connectedToCheatServer = atoi( s );
	if ( !cl_connectedToCheatServer ) {
		Cvar_SetCheatState();
	}

	// check pure server string
	s = Info_ValueForKey( systemInfo, "sv_paks" );
	t = Info_ValueForKey( systemInfo, "sv_pakNames" );
	FS_PureServerSetLoadedPaks( s, t );

	s = Info_ValueForKey( systemInfo, "sv_referencedPaks" );
	t = Info_ValueForKey( systemInfo, "sv_referencedPakNames" );
	FS_PureServerSetReferencedPaks( s, t );

	gameSet = qfalse;
	// scan through all the variables in the systeminfo and locally set cvars to match
	s = systemInfo;
	while ( s ) {
		int cvar_flags;

		Info_NextPair( &s, key, value );
		if ( !key[0] ) {
			break;
		}

		// ehw!
		if (!Q_stricmp(key, "fs_game"))
		{
			if(FS_InvalidGameDir(value))
			{
				Com_Printf(S_COLOR_YELLOW "WARNING: Server sent invalid fs_game value %s\n", value);
				continue;
			}
				
			gameSet = qtrue;
		}

		if((cvar_flags = Cvar_Flags(key)) == CVAR_NONEXISTENT)
			Cvar_Get(key, value, CVAR_SERVER_CREATED | CVAR_ROM);
		else
		{
			// If this cvar may not be modified by a server discard the value.
			if(!(cvar_flags & (CVAR_SYSTEMINFO | CVAR_SERVER_CREATED | CVAR_USER_CREATED)))
			{
#ifndef STANDALONE
				if(Q_stricmp(key, "g_synchronousClients") && Q_stricmp(key, "pmove_fixed") &&
				   Q_stricmp(key, "pmove_msec"))
#endif
				{
					Com_DPrintf(S_COLOR_YELLOW "WARNING: server is not allowed to set %s=%s\n", key, value);
					continue;
				}
			}

			Cvar_SetSafe(key, value);
		}
	}
	// if game folder should not be set and it is set at the client side
	if ( !gameSet && *Cvar_VariableString("fs_game") ) {
		Cvar_Set( "fs_game", "" );
	}
	cl_connectedToPureServer = Cvar_VariableValue( "sv_pure" );
}

/*
==================
CL_LocalGamestate

The server has sent the client a new gamestate
==================
*/
static void CL_LocalGamestate( int clientNum, int checksumFeed, int serverCommandSequence, int snapshotNum ) {
	int i, len;
	const char *s;
	char oldGame[MAX_QPATH];

	Con_Close();

	clc.connectPacketCount = 0;

	// wipe local client state
	CL_ClearState();

	// a gamestate always marks a server command sequence
	clc.serverCommandSequence = serverCommandSequence;
	clc.serverMessageSequence = snapshotNum;
	cl.snap.messageNum = snapshotNum;	// snapshots from before the gamestate are of no use

	// copy all the configstrings
	cl.gameState.dataCount = 1; // leave a 0 at the beginning for uninitialized configstrings
	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		s = SV_LocalConfigstring( i );
		if ( !s[0] ) {
			continue;
		}
		len = strlen( s );
		if ( len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
			Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
		}

		// append it to the gameState string buffer
		cl.gameState.stringOffsets[ i ] = cl.gameState.dataCount;
		memcpy( cl.gameState.stringData + cl.gameState.dataCount, s, len + 1 );
		cl.gameState.dataCount += len + 1;
	}

	clc.clientNum = clientNum;
	clc.checksumFeed = checksumFeed;

	// save old gamedir
	Cvar_VariableStringBuffer("fs_game", oldGame, sizeof(oldGame));

	// parse serverId and other cvars
	CL_SystemInfoChanged();

	// reinitialize the filesystem if the game directory has changed
	if(!cl_oldGameSet && (Cvar_Flags("fs_game") & CVAR_MODIFIED))
	{
		cl_oldGameSet = qtrue;
		Q_strncpyz(cl_oldGame, oldGame, sizeof(cl_oldGame));
	}

	FS_ConditionalRestart(clc.checksumFeed, qfalse);

	// load the cgame
	CL_DownloadsComplete();

	// make sure the game starts
	Cvar_Set( "cl_paused", "0" );
}

/*
==================
CL_LocalFrame

Picks up what the server has for the client: a new gamestate, the newest
snapshot, or the news that it dropped the player
==================
*/
void CL_LocalFrame( void ) {
	int clientNum, checksumFeed, serverCommandSequence, snapshotNum;
	int serverTime, snapFlags;
	int n;

	if ( clc.state < CA_CONNECTED || clc.state == CA_CINEMATIC ) {
		return;
	}

	if ( !SV_LocalConnected() ) {
		Com_Error( ERR_SERVERDISCONNECT, "Server disconnected" );
	}

	if ( SV_LocalGameState( &clientNum, &checksumFeed, &serverCommandSequence, &snapshotNum ) ) {
		CL_LocalGamestate( clientNum, checksumFeed, serverCommandSequence, snapshotNum );
	}

	if ( clc.state < CA_LOADING ) {
		return;
	}

	n = SV_LocalSnapshotInfo( &serverTime, &snapFlags, &serverCommandSequence, &cl.snap.ps );
	if ( n <= cl.snap.messageNum ) {
		return;
	}

	// if we were just unpaused, we can only *now* really let the
	// change come into effect or the client hangs.
	cl_paused->modified = 0;

	cl.snap.valid = qtrue;
	cl.snap.messageNum = n;
	cl.snap.serverTime = serverTime;
	cl.snap.snapFlags = snapFlags;
	clc.serverMessageSequence = n;
	clc.serverCommandSequence = serverCommandSequence;
	cl.newSnapshots = qtrue;
}
