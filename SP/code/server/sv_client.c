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

// sv_client.c -- server code for dealing with clients

#include "server.h"

/*
=====================
SV_FreeClient

Destructor for data allocated in a client structure
=====================
*/
void SV_FreeClient(client_t *client)
{
	Z_Free( client->frames );
	client->frames = NULL;
}

/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_Shutdown() will handle that
=====================
*/
void SV_DropClient( client_t *drop, const char *reason ) {
	const qboolean isBot = drop->bot;

	if ( drop->state == CS_ZOMBIE ) {
		return;     // already dropped
	}

	// Free all allocated data on the client structure
	SV_FreeClient(drop);

	// Ridah, no need to tell the player if an AI drops
	if ( !( drop->gentity && drop->gentity->r.svFlags & SVF_CASTAI ) ) {
		// tell everyone why they got dropped
		SV_SendServerCommand( NULL, "print \"%s" S_COLOR_WHITE " %s\n\"", drop->name, reason );
	}

	// call the prog function for removing a client
	// this will remove the body, among other things
	VM_Call( gvm, GAME_CLIENT_DISCONNECT, drop - svs.clients );

	// Ridah, no need to tell the player if an AI drops
	if ( !( drop->gentity && drop->gentity->r.svFlags & SVF_CASTAI ) ) {
		// add the disconnect command
		SV_SendServerCommand( drop, "disconnect" );
	}
	// done.

	if ( isBot ) {
		SV_BotFreeClient( drop - svs.clients );

		// bots shouldn't go zombie, as there's no real net connection.
		drop->state = CS_FREE;
	} else {
		Com_DPrintf( "Going to CS_ZOMBIE for %s\n", drop->name );
		drop->state = CS_ZOMBIE;		// become free in a few seconds
	}

	// nuke user info
	SV_SetUserinfo( drop - svs.clients, "" );

	// RF, nuke reliable commands
	SV_FreeReliableCommandsForClient( drop );
}

/*
================
SV_SendClientGameState

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each new map load.
The client picks it up with SV_LocalGameState.
================
*/
static void SV_SendClientGameState( client_t *client ) {
	Com_DPrintf( "SV_SendClientGameState() for %s\n", client->name );
	Com_DPrintf( "Going from CS_CONNECTED to CS_PRIMED for %s\n", client->name );
	client->state = CS_PRIMED;
	client->gamestatePending = qtrue;
}


/*
==================
SV_ClientEnterWorld
==================
*/
void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd ) {
	int clientNum;
	sharedEntity_t *ent;

	Com_DPrintf( "Going from CS_PRIMED to CS_ACTIVE for %s\n", client->name );
	client->state = CS_ACTIVE;

	// resend all configstrings using the cs commands since these are
	// no longer sent when the client is CS_PRIMED
	SV_UpdateConfigstrings( client );

	// set up the entity for the client
	clientNum = client - svs.clients;
	ent = SV_GentityNum( clientNum );
	ent->s.number = clientNum;
	client->gentity = ent;

	client->lastSnapshotTime = 0;	// generate a snapshot immediately

	if(cmd)
		memcpy(&client->lastUsercmd, cmd, sizeof(client->lastUsercmd));
	else
		Com_Memset(&client->lastUsercmd, '\0', sizeof(client->lastUsercmd));

	// call the game begin function
	VM_Call( gvm, GAME_CLIENT_BEGIN, client - svs.clients );
}

/*
============================================================

CLIENT COMMAND EXECUTION

============================================================
*/
/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately  FIXME: move to game?
=================
*/
static void SV_Disconnect_f( client_t *cl ) {
	SV_DropClient( cl, "disconnected" );
}

/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string
into a more C friendly form.
=================
*/
void SV_UserinfoChanged( client_t *cl ) {
	char    *val;
	int i;

	// name for C code
	Q_strncpyz( cl->name, Info_ValueForKey( cl->userinfo, "name" ), sizeof( cl->name ) );

	val = Info_ValueForKey( cl->userinfo, "handicap" );
	if ( strlen( val ) ) {
		i = atoi( val );
		if ( i <= 0 || i > 100 || strlen( val ) > 4 ) {
			Info_SetValueForKey( cl->userinfo, "handicap", "100" );
		}
	}

	// the game expects an ip
	if ( strlen( cl->userinfo ) + 16 >= MAX_INFO_STRING ) {
		SV_DropClient( cl, "userinfo string length exceeded" );
	} else {
		Info_SetValueForKey( cl->userinfo, "ip", cl->bot ? "bot" : "localhost" );
	}
}


/*
==================
SV_UpdateUserinfo_f
==================
*/
static void SV_UpdateUserinfo_f( client_t *cl ) {
	Q_strncpyz( cl->userinfo, Cmd_Argv( 1 ), sizeof( cl->userinfo ) );

	SV_UserinfoChanged( cl );
	// call prog code to allow overrides
	VM_Call( gvm, GAME_CLIENT_USERINFO_CHANGED, cl - svs.clients );
}


typedef struct {
	char    *name;
	void ( *func )( client_t *cl );
} ucmd_t;

static ucmd_t ucmds[] = {
	{"userinfo", SV_UpdateUserinfo_f},
	{"disconnect", SV_Disconnect_f},

	{NULL, NULL}
};

/*
==================
SV_ExecuteClientCommand

Also called by bot code
==================
*/
void SV_ExecuteClientCommand( client_t *cl, const char *s, qboolean clientOK ) {
	ucmd_t  *u;

	Cmd_TokenizeString( s );

	// see if it is a server level command
	for ( u = ucmds ; u->name ; u++ ) {
		if ( !strcmp( Cmd_Argv( 0 ), u->name ) ) {
			u->func( cl );
			break;
		}
	}

	if ( clientOK ) {
		// pass unknown strings to the game
		if (!u->name && sv.state == SS_GAME && (cl->state == CS_ACTIVE || cl->state == CS_PRIMED)) {
			Cmd_Args_Sanitize();
			VM_Call( gvm, GAME_CLIENT_COMMAND, cl - svs.clients );
		}
	}
}


//==================================================================================


/*
==================
SV_ClientThink

Also called by bot code
==================
*/
void SV_ClientThink( client_t *cl, usercmd_t *cmd ) {
	cl->lastUsercmd = *cmd;

	if ( cl->state != CS_ACTIVE ) {
		return;     // may have been kicked during the last usercmd
	}

	VM_Call( gvm, GAME_CLIENT_THINK, cl - svs.clients );
}


/*
============================================================================

THE LOCAL CLIENT

The client runs in the same program, so what packets carried both ways is
handed over directly: the client calls these each frame.

============================================================================
*/

/*
==================
SV_LocalClient

The client slot the player has, NULL if none
==================
*/
client_t *SV_LocalClient( void ) {
	int i;
	client_t *cl;

	if ( !com_sv_running || !com_sv_running->integer || !svs.clients ) {
		return NULL;
	}
	for ( i = 0, cl = svs.clients ; i < sv_maxclients->integer ; i++, cl++ ) {
		if ( cl->state != CS_FREE && !cl->bot ) {
			return cl;
		}
	}
	return NULL;
}

/*
==================
SV_LocalConnect

Returns the client number, or -1 while the server is still loading
==================
*/
int SV_LocalConnect( const char *userinfo ) {
	int i;
	client_t *cl, *newcl;
	intptr_t denied;

	if ( !com_sv_running->integer || sv.state != SS_GAME ) {
		return -1;
	}

	newcl = SV_LocalClient();
	if ( newcl ) {
		// disconnect the client from the game first so any flags the
		// player might have are dropped
		VM_Call( gvm, GAME_CLIENT_DISCONNECT, newcl - svs.clients );
	} else {
		for ( i = 0, cl = svs.clients ; i < sv_maxclients->integer ; i++, cl++ ) {
			if ( cl->state == CS_FREE ) {
				newcl = cl;
				break;
			}
		}
		if ( !newcl ) {
			newcl = &svs.clients[sv_maxclients->integer - 1];
			if ( !newcl->bot ) {
				Com_Error( ERR_FATAL, "server is full on local connect" );
			}
			SV_DropClient( newcl, "only bots on server" );
		}
	}

	// this is the only place a client_t is ever initialized
	SV_FreeClient( newcl );
	SV_FreeReliableCommandsForClient( newcl );
	Com_Memset( newcl, 0, sizeof( *newcl ) );
	newcl->gentity = SV_GentityNum( newcl - svs.clients );
	Q_strncpyz( newcl->userinfo, userinfo, sizeof( newcl->userinfo ) );

	// get the game a chance to reject this connection or modify the userinfo
	denied = VM_Call( gvm, GAME_CLIENT_CONNECT, newcl - svs.clients, qtrue, qfalse ); // firstTime = qtrue
	if ( denied ) {
		// we can't just use VM_ArgPtr, because that is only valid inside a VM_Call
		Com_Error( ERR_DROP, "%s", (char *)VM_ExplicitArgPtr( gvm, denied ) );
	}

	SV_InitReliableCommandsForClient( newcl, MAX_RELIABLE_COMMANDS );
	SV_UserinfoChanged( newcl );

	Com_DPrintf( "Going from CS_FREE to CS_CONNECTED for %s\n", newcl->name );

	newcl->state = CS_CONNECTED;
	newcl->lastPacketTime = svs.time;
	newcl->lastConnectTime = svs.time;

	return newcl - svs.clients;
}

/*
==================
SV_LocalConnected

False once the server has dropped the player
==================
*/
qboolean SV_LocalConnected( void ) {
	client_t *cl = SV_LocalClient();

	return cl && cl->state >= CS_CONNECTED;
}

/*
==================
SV_LocalClientMessage

What a client packet held: the gamestate the client has (serverId), the last
server command it ran, its client commands, its newest usercmd
==================
*/
int SV_LocalClientMessage( int serverId, int serverCommandAck, char **commands,
						   int firstCommand, int lastCommand, usercmd_t *cmd ) {
	client_t *cl = SV_LocalClient();
	int i;

	if ( !cl || cl->state < CS_CONNECTED ) {
		return lastCommand;
	}
	cl->lastPacketTime = svs.time;

	cl->reliableAcknowledge = serverCommandAck;
	if ( cl->reliableSequence - cl->reliableAcknowledge >= MAX_RELIABLE_COMMANDS
		 || cl->reliableSequence - cl->reliableAcknowledge < 0 ) {
		cl->reliableAcknowledge = cl->reliableSequence;
		return cl->lastClientCommand;
	}

	// if this is from a previous gamestate, ignore it or send the current one
	if ( serverId != sv.serverId ) {
		if ( serverId >= sv.restartedServerId && serverId < sv.serverId ) { // TTimo - use a comparison here to catch multiple map_restart
			// they just haven't caught the map_restart yet
			Com_DPrintf( "%s : ignoring pre map_restart / outdated client message\n", cl->name );
		} else if ( cl->state != CS_ACTIVE && !cl->gamestatePending ) {
			SV_SendClientGameState( cl );
		}
		return cl->lastClientCommand;
	}

	// RF, kill any reliableCommands that have been acknowledged
	SV_FreeAcknowledgedReliableCommands( cl );

	// this client has acknowledged the new gamestate so it's
	// safe to start sending it the real time again
	if ( cl->oldServerTime ) {
		Com_DPrintf( "%s acknowledged gamestate\n", cl->name );
		cl->oldServerTime = 0;
	}

	for ( i = cl->lastClientCommand + 1 ; i <= lastCommand ; i++ ) {
		if ( i < firstCommand ) {
			Com_Printf( "Client %s lost %i clientCommands\n", cl->name, firstCommand - i );
			SV_DropClient( cl, "Lost reliable commands" );
			return lastCommand;
		}
		Com_DPrintf( "clientCommand: %s : %i : %s\n", cl->name, i, commands[i & ( MAX_RELIABLE_COMMANDS - 1 )] );
		SV_ExecuteClientCommand( cl, commands[i & ( MAX_RELIABLE_COMMANDS - 1 )], qtrue );
		cl->lastClientCommand = i;
		if ( cl->state == CS_ZOMBIE ) {
			return lastCommand;     // disconnect command
		}
	}

	if ( !cmd ) {
		return cl->lastClientCommand;
	}

	// if this is the first usercmd we have received
	// this gamestate, put the client into the world
	if ( cl->state == CS_PRIMED ) {
		SV_ClientEnterWorld( cl, cmd );
	}
	if ( cl->state == CS_ACTIVE && cmd->serverTime > cl->lastUsercmd.serverTime ) {
		SV_ClientThink( cl, cmd );
	}
	return cl->lastClientCommand;
}

/*
==================
SV_LocalGameState
==================
*/
qboolean SV_LocalGameState( int *clientNum, int *checksumFeed, int *serverCommandSequence, int *snapshotNum ) {
	client_t *cl = SV_LocalClient();

	if ( !cl || !cl->gamestatePending ) {
		return qfalse;
	}
	cl->gamestatePending = qfalse;

	*clientNum = cl - svs.clients;
	*checksumFeed = sv.checksumFeed;
	*serverCommandSequence = cl->reliableSequence;
	*snapshotNum = cl->snapshotNum;
	return qtrue;
}

/*
==================
SV_LocalConfigstring
==================
*/
const char *SV_LocalConfigstring( int index ) {
	if ( index < 0 || index >= MAX_CONFIGSTRINGS || !sv.configstrings[index] ) {
		return "";
	}
	return sv.configstrings[index];
}

/*
==================
SV_LocalServerCommand

NULL once it is no longer kept
==================
*/
const char *SV_LocalServerCommand( int serverCommandNum ) {
	client_t *cl = SV_LocalClient();

	if ( !cl || serverCommandNum > cl->reliableSequence
		 || serverCommandNum <= cl->reliableSequence - MAX_RELIABLE_COMMANDS ) {
		return NULL;
	}
	return SV_GetReliableCommand( cl, serverCommandNum & ( MAX_RELIABLE_COMMANDS - 1 ) );
}
