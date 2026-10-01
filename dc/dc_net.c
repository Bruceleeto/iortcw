/*
 * Dreamcast networking: none yet.  Replaces qcommon/net_ip.c.
 *
 * Only loopback (the local client talking to the local server) and bots
 * exist, which is all single player and a local MP game need.  Packets for
 * real addresses are dropped.
 *
 * Shared by SP and MP; compiled against each game's qcommon.h.
 */

#include <unistd.h>

#include "q_shared.h"
#include "qcommon.h"

void NET_Init( void ) {
	Com_Printf( "Networking disabled (Dreamcast build): loopback only\n" );
	Cmd_AddCommand( "net_restart", NET_Restart_f );
}

void NET_Shutdown( void ) {
	Cmd_RemoveCommand( "net_restart" );
}

void NET_Restart_f( void ) {
}

void NET_JoinMulticast6( void ) {
}

void NET_LeaveMulticast6( void ) {
}

void NET_Sleep( int msec ) {
	if ( msec > 0 ) {
		usleep( msec * 1000 );
	}
}

void Sys_SendPacket( int length, const void *data, netadr_t to ) {
	(void)length; (void)data; (void)to;
}

void Sys_ShowIP( void ) {
}

qboolean Sys_IsLANAddress( netadr_t adr ) {
	return adr.type == NA_LOOPBACK || adr.type == NA_BOT;
}

qboolean Sys_StringToAdr( const char *s, netadr_t *a, netadrtype_t family ) {
	(void)family;
	Com_Memset( a, 0, sizeof( *a ) );
	if ( !Q_stricmp( s, "localhost" ) || !Q_stricmp( s, "loopback" ) ) {
		a->type = NA_LOOPBACK;
		return qtrue;
	}
	return qfalse;
}

qboolean NET_CompareBaseAdrMask( netadr_t a, netadr_t b, int netmask ) {
	(void)netmask;
	if ( a.type != b.type ) {
		return qfalse;
	}
	if ( a.type == NA_IP ) {
		return !memcmp( a.ip, b.ip, sizeof( a.ip ) );
	}
	if ( a.type == NA_IP6 ) {
		return !memcmp( a.ip6, b.ip6, sizeof( a.ip6 ) );
	}
	return qtrue;
}

qboolean NET_CompareBaseAdr( netadr_t a, netadr_t b ) {
	return NET_CompareBaseAdrMask( a, b, -1 );
}

qboolean NET_CompareAdr( netadr_t a, netadr_t b ) {
	if ( !NET_CompareBaseAdr( a, b ) ) {
		return qfalse;
	}
	if ( a.type == NA_IP || a.type == NA_IP6 ) {
		return a.port == b.port;
	}
	return qtrue;
}

qboolean NET_IsLocalAddress( netadr_t adr ) {
	return adr.type == NA_LOOPBACK;
}

const char *NET_AdrToString( netadr_t a ) {
	static char s[NET_ADDRSTRMAXLEN];

	if ( a.type == NA_LOOPBACK ) {
		Com_sprintf( s, sizeof( s ), "loopback" );
	} else if ( a.type == NA_BOT ) {
		Com_sprintf( s, sizeof( s ), "bot" );
	} else if ( a.type == NA_IP ) {
		Com_sprintf( s, sizeof( s ), "%i.%i.%i.%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3] );
	} else {
		Com_sprintf( s, sizeof( s ), "unknown" );
	}
	return s;
}

const char *NET_AdrToStringwPort( netadr_t a ) {
	static char s[NET_ADDRSTRMAXLEN];

	if ( a.type == NA_IP ) {
		Com_sprintf( s, sizeof( s ), "%s:%hu", NET_AdrToString( a ), BigShort( a.port ) );
	} else {
		Q_strncpyz( s, NET_AdrToString( a ), sizeof( s ) );
	}
	return s;
}
