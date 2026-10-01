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


#include "server.h"


serverStatic_t svs;                 // persistant server info
server_t sv;                        // local server
vm_t            *gvm = NULL;                // game virtual machine

cvar_t  *sv_fps = NULL;		// time rate for running non-clients
cvar_t  *sv_timeout;            // seconds without any message
cvar_t  *sv_zombietime;         // seconds to sink messages after disconnect
cvar_t  *sv_maxclients;
cvar_t  *sv_hostname;
cvar_t  *sv_killserver;         // menu system can set to 1 to shut server down
cvar_t  *sv_mapname;
cvar_t  *sv_mapChecksum;
cvar_t  *sv_serverid;
cvar_t  *sv_gametype;
cvar_t  *sv_pure;


// Rafael gameskill
cvar_t  *sv_gameskill;
// done

cvar_t  *sv_reloading;  //----(SA)	added

/*
=============================================================================

EVENT MESSAGES

=============================================================================
*/

/*
===============
SV_ExpandNewlines

Converts newlines to "\n" so a line prints nicer
===============
*/
static char	*SV_ExpandNewlines( char *in ) {
	static char string[1024];
	int l;

	l = 0;
	while ( *in && l < sizeof( string ) - 3 ) {
		if ( *in == '\n' ) {
			string[l++] = '\\';
			string[l++] = 'n';
		} else {
			string[l++] = *in;
		}
		in++;
	}
	string[l] = 0;

	return string;
}

/*
======================
SV_ReplacePendingServerCommands

FIXME: This is ugly
======================
*/
#if 0   // RF, not used anymore
static int SV_ReplacePendingServerCommands( client_t *client, const char *cmd ) {
	int i, index, csnum1, csnum2;

	for ( i = client->reliableSent + 1; i <= client->reliableSequence; i++ ) {
		index = i & ( MAX_RELIABLE_COMMANDS - 1 );
		//
		//if ( !Q_strncmp(cmd, client->reliableCommands[ index ], strlen("cs")) ) {
		if ( !Q_strncmp( cmd, SV_GetReliableCommand( client, index ), strlen( "cs" ) ) ) {
			sscanf( cmd, "cs %i", &csnum1 );
			//sscanf(client->reliableCommands[ index ], "cs %i", &csnum2);
			sscanf( SV_GetReliableCommand( client, index ), "cs %i", &csnum2 );
			if ( csnum1 == csnum2 ) {
				//Q_strncpyz( client->reliableCommands[ index ], cmd, sizeof( client->reliableCommands[ index ] ) );

				/*
				if ( !client->bot ) {
					Com_Printf( "WARNING: client %i removed double pending config string %i: %s\n", client-svs.clients, csnum1, cmd );
				}
				*/
				return qtrue;
			}
		}
	}
	return qfalse;
}
#endif

/*
======================
SV_AddServerCommand

The given command will be transmitted to the client, and is guaranteed to
not have future snapshot_t executed before it is executed
======================
*/
void SV_AddServerCommand( client_t *client, const char *cmd ) {
	int index, i;

	// do not send commands until the gamestate has been sent
	if( client->state < CS_PRIMED )
		return;

	client->reliableSequence++;
	// if we would be losing an old command that hasn't been acknowledged,
	// we must drop the connection
	// we check == instead of >= so a broadcast print added by SV_DropClient()
	// doesn't cause a recursive drop client
	if ( client->reliableSequence - client->reliableAcknowledge == MAX_RELIABLE_COMMANDS + 1 ) {
		Com_Printf( "===== pending server commands =====\n" );
		for ( i = client->reliableAcknowledge + 1 ; i <= client->reliableSequence ; i++ ) {
			//Com_Printf( "cmd %5d: %s\n", i, client->reliableCommands[ i & (MAX_RELIABLE_COMMANDS-1) ] );
			Com_Printf( "cmd %5d: %s\n", i, SV_GetReliableCommand( client, i & ( MAX_RELIABLE_COMMANDS - 1 ) ) );
		}
		Com_Printf( "cmd %5d: %s\n", i, cmd );
		SV_DropClient( client, "Server command overflow" );
		return;
	}
	index = client->reliableSequence & ( MAX_RELIABLE_COMMANDS - 1 );
	//Q_strncpyz( client->reliableCommands[ index ], cmd, sizeof( client->reliableCommands[ index ] ) );
	SV_AddReliableCommand( client, index, cmd );
}


/*
=================
SV_SendServerCommand

Sends a reliable command string to be interpreted by
the client game module: "cp", "print", "chat", etc
A NULL client will broadcast to all clients
=================
*/
void QDECL SV_SendServerCommand( client_t *cl, const char *fmt, ... ) {
	va_list argptr;
	byte message[BIG_INFO_STRING];
	client_t    *client;
	int j;

	va_start( argptr,fmt );
	Q_vsnprintf ((char *)message, sizeof(message), fmt,argptr);
	va_end( argptr );

	// Fix to http://aluigi.altervista.org/adv/q3msgboom-adv.txt
	// The actual cause of the bug is probably further downstream
	// and should maybe be addressed later, but this certainly
	// fixes the problem for now
	if ( strlen ((char *)message) > 1022 ) {
		return;
	}

	if ( cl != NULL ) {
		SV_AddServerCommand( cl, (char *)message );
		return;
	}

	// hack to echo broadcast prints to console
	if ( com_dedicated->integer && !strncmp( (char *)message, "print", 5 ) ) {
		Com_Printf( "broadcast: %s\n", SV_ExpandNewlines( (char *)message ) );
	}

	// send the data to all relevent clients
	for ( j = 0, client = svs.clients; j < sv_maxclients->integer ; j++, client++ ) {
		// Ridah, don't need to send messages to AI
		if ( client->gentity && client->gentity->r.svFlags & SVF_CASTAI ) {
			continue;
		}
		// done.
		SV_AddServerCommand( client, (char *)message );
	}
}


/*
===================
SV_CalcPings

Updates the cl->ping variables
===================
*/
static void SV_CalcPings( void ) {
	int i;
	client_t    *cl;
	playerState_t   *ps;

	for ( i = 0 ; i < sv_maxclients->integer ; i++ ) {
		cl = &svs.clients[i];
		if ( cl->state != CS_ACTIVE ) {
			cl->ping = 999;
			continue;
		}
		if ( !cl->gentity ) {
			cl->ping = 999;
			continue;
		}
		if ( cl->gentity->r.svFlags & SVF_BOT ) {
			cl->ping = 0;
			continue;
		}

		if ( !cl->frames ) {
			cl->ping = 999;
			continue;
		}
		// the player is in the same program
		cl->ping = 0;

		// let the game dll know about the ping
		ps = SV_GameClientNum( i );
		ps->ping = cl->ping;
	}
}

/*
==================
SV_CheckTimeouts

If a packet has not been received from a client for timeout->integer
seconds, drop the conneciton.  Server time is used instead of
realtime to avoid dropping the local client while debugging.

When a client is normally dropped, the client_t goes into a zombie state
for a few seconds to make sure any final reliable message gets resent
if necessary
==================
*/
static void SV_CheckTimeouts( void ) {
	int i;
	client_t    *cl;
	int droppoint;
	int zombiepoint;

	droppoint = svs.time - 1000 * sv_timeout->integer;
	zombiepoint = svs.time - 1000 * sv_zombietime->integer;

	for ( i = 0,cl = svs.clients ; i < sv_maxclients->integer ; i++,cl++ ) {
		// message times may be wrong across a changelevel
		if ( cl->lastPacketTime > svs.time ) {
			cl->lastPacketTime = svs.time;
		}

		if ( cl->state == CS_ZOMBIE
			 && cl->lastPacketTime < zombiepoint ) {
			Com_DPrintf( "Going from CS_ZOMBIE to CS_FREE for %s\n", cl->name );
			cl->state = CS_FREE;    // can now be reused
			continue;
		}
		// Ridah, AI's don't time out
		if ( cl->gentity && !( cl->gentity->r.svFlags & SVF_CASTAI ) ) {
			if ( cl->state >= CS_CONNECTED && cl->lastPacketTime < droppoint ) {
				// wait several frames so a debugger session doesn't
				// cause a timeout
				if ( ++cl->timeoutCount > 5 ) {
					SV_DropClient( cl, "timed out" );
					cl->state = CS_FREE;    // don't bother with zombie state
				}
			} else {
				cl->timeoutCount = 0;
			}
		}
	}
}


/*
==================
SV_CheckPaused
==================
*/
static qboolean SV_CheckPaused( void ) {
	int count;
	client_t    *cl;
	int i;

	if ( !cl_paused->integer ) {
		return qfalse;
	}

	// only pause if there is just a single client connected
	count = 0;
	for ( i = 0,cl = svs.clients ; i < sv_maxclients->integer ; i++,cl++ ) {
		if ( cl->state >= CS_CONNECTED && !cl->bot ) {
			count++;
		}
	}

	if ( count > 1 ) {
		// don't pause
		sv_paused->integer = 0;
		return qfalse;
	}

	sv_paused->integer = 1;
	return qtrue;
}

/*
==================
SV_FrameMsec
Return time in millseconds until processing of the next server frame.
==================
*/
int SV_FrameMsec()
{
	if(sv_fps)
	{
		int frameMsec;
		
		frameMsec = 1000.0f / sv_fps->value;
		
		if(frameMsec < sv.timeResidual)
			return 0;
		else
			return frameMsec - sv.timeResidual;
	}
	else
		return 1;
}

/*
==================
SV_Frame

Player movement occurs as a result of packet events, which
happen before SV_Frame is called
==================
*/
void SV_Frame( int msec ) {
	int frameMsec;
	int startTime;

	// the menu kills the server with this cvar
	if ( sv_killserver->integer ) {
		SV_Shutdown( "Server was killed" );
		Cvar_Set( "sv_killserver", "0" );
		return;
	}

	if (!com_sv_running->integer)
	{
		// Running as a server, but no map loaded
#ifdef DEDICATED
		// Block until something interesting happens
		Sys_Sleep(-1);
#endif
		return;
	}

	// allow pause if only the local client is connected
	if ( SV_CheckPaused() ) {
		return;
	}

	// if it isn't time for the next frame, do nothing
	if ( sv_fps->integer < 1 ) {
		Cvar_Set( "sv_fps", "10" );
	}

	frameMsec = 1000 / sv_fps->integer * com_timescale->value;
	// don't let it scale below 1ms
	if(frameMsec < 1)
	{
		Cvar_Set("timescale", va("%f", sv_fps->integer / 1000.0f));
		frameMsec = 1;
	}

	sv.timeResidual += msec;

	if (!com_dedicated->integer) SV_BotFrame (sv.time + sv.timeResidual);

	// if time is about to hit the 32nd bit, kick all clients
	// and clear sv.time, rather
	// than checking for negative time wraparound everywhere.
	// 2giga-milliseconds = 23 days, so it won't be too often
	if ( svs.time > 0x70000000 ) {
		SV_Shutdown( "Restarting server due to time wrapping" );
		Cbuf_AddText( va( "spmap %s\n", Cvar_VariableString( "mapname" ) ) );
		return;
	}
	// this can happen considerably earlier when lots of clients play and the map doesn't change
	if ( svs.nextSnapshotEntities >= 0x7FFFFFFE - svs.numSnapshotEntities ) {
		SV_Shutdown( "Restarting server due to numSnapshotEntities wrapping" );
		Cbuf_AddText( va( "spmap %s\n", Cvar_VariableString( "mapname" ) ) );
		return;
	}

	if( sv.restartTime && sv.time >= sv.restartTime ) {
		sv.restartTime = 0;
		Cbuf_AddText( "map_restart 0\n" );
		return;
	}

	// update infostrings if anything has been changed
	if ( cvar_modifiedFlags & CVAR_SERVERINFO ) {
		SV_SetConfigstring( CS_SERVERINFO, Cvar_InfoString( CVAR_SERVERINFO ) );
		cvar_modifiedFlags &= ~CVAR_SERVERINFO;
	}
	if ( cvar_modifiedFlags & CVAR_SYSTEMINFO ) {
		SV_SetConfigstring( CS_SYSTEMINFO, Cvar_InfoString_Big( CVAR_SYSTEMINFO ) );
		cvar_modifiedFlags &= ~CVAR_SYSTEMINFO;
	}

	if ( com_speeds->integer ) {
		startTime = Sys_Milliseconds();
	} else {
		startTime = 0;  // quiet a compiler warning
	}

	// update ping based on the all received frames
	SV_CalcPings();

	if ( com_dedicated->integer ) {
		SV_BotFrame( sv.time );
	}

	// run the game simulation in chunks
	while ( sv.timeResidual >= frameMsec ) {
		sv.timeResidual -= frameMsec;
		svs.time += frameMsec;
		sv.time += frameMsec;

		// let everything in the world think and move
		VM_Call( gvm, GAME_RUN_FRAME, sv.time );
	}

	if ( com_speeds->integer ) {
		time_game = Sys_Milliseconds() - startTime;
	}

	// check timeouts
	SV_CheckTimeouts();

	// send messages back to the clients
	SV_SendClientMessages();
}

#ifdef DCSIM
// how many entity slots the game has used: the most it has had at once
int SV_NumEntities( void ) {
	return sv.state == SS_DEAD ? 0 : sv.num_entities;
}
#endif
