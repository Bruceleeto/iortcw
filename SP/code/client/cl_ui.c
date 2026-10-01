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


#include "client.h"

#include "../botlib/botlib.h"

extern botlib_export_t *botlib_export;

vm_t *uivm;

extern char cl_cdkey[34];


/*
====================
GetClientState
====================
*/
static void GetClientState( uiClientState_t *state ) {
	state->connectPacketCount = clc.connectPacketCount;
	state->connState = clc.state;
	Q_strncpyz( state->servername, clc.servername, sizeof( state->servername ) );
	Q_strncpyz( state->updateInfoString, cls.updateInfoString, sizeof( state->updateInfoString ) );
	Q_strncpyz( state->messageString, clc.serverMessage, sizeof( state->messageString ) );
	state->clientNum = cl.snap.ps.clientNum;
}

/*
====================
CL_GetGlConfig
====================
*/
static void CL_GetGlconfig( glconfig_t *config ) {
	*config = cls.glconfig;
}

/*
====================
CL_GetClipboardData
====================
*/
static void CL_GetClipboardData( char *buf, int buflen ) {
	char    *cbd;

	cbd = Sys_GetClipboardData();

	if ( !cbd ) {
		*buf = 0;
		return;
	}

	Q_strncpyz( buf, cbd, buflen );

	Z_Free( cbd );
}

/*
====================
Key_KeynumToStringBuf
====================
*/
static void Key_KeynumToStringBuf( int keynum, char *buf, int buflen ) {
	Q_strncpyz( buf, Key_KeynumToString( keynum, qtrue ), buflen );
}

/*
====================
Key_GetBindingBuf
====================
*/
static void Key_GetBindingBuf( int keynum, char *buf, int buflen ) {
	char    *value;

	value = Key_GetBinding( keynum );
	if ( value ) {
		Q_strncpyz( buf, value, buflen );
	} else {
		*buf = 0;
	}
}

/*
====================
CLUI_GetCDKey
====================
*/
static void CLUI_GetCDKey( char *buf, int buflen ) {
#ifndef STANDALONE
	const char *gamedir;
	gamedir = Cvar_VariableString( "fs_game" );
	if ( UI_usesUniqueCDKey() && gamedir[0] != 0 ) {
 		Com_Memcpy( buf, &cl_cdkey[16], 16 );
		buf[16] = 0;
	} else {
		Com_Memcpy( buf, cl_cdkey, 16 );
		buf[16] = 0;
	}
#else
	*buf = 0;
#endif
}


/*
====================
CLUI_SetCDKey
====================
*/
#ifndef STANDALONE
static void CLUI_SetCDKey( char *buf ) {
	const char *gamedir;
	gamedir = Cvar_VariableString( "fs_game" );
	if ( UI_usesUniqueCDKey() && gamedir[0] != 0 ) {
 		Com_Memcpy( &cl_cdkey[16], buf, 16 );
		cl_cdkey[32] = 0;
		// set the flag so the file will be written at the next opportunity
		cvar_modifiedFlags |= CVAR_ARCHIVE;
	} else {
		Com_Memcpy( cl_cdkey, buf, 16 );
		// set the flag so the file will be written at the next opportunity
		cvar_modifiedFlags |= CVAR_ARCHIVE;
	}
}
#endif

/*
====================
GetConfigString
====================
*/
static int GetConfigString( int index, char *buf, int size ) {
	int offset;

	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		return qfalse;
	}

	offset = cl.gameState.stringOffsets[index];
	if ( !offset ) {
		if ( size ) {
			buf[0] = 0;
		}
		return qfalse;
	}

	Q_strncpyz( buf, cl.gameState.stringData + offset, size );

	return qtrue;
}

/*
====================
FloatAsInt
====================
*/
static int FloatAsInt( float f ) {
	floatint_t fi;
	fi.f = f;
	return fi.i;
}

/*
====================
CL_UISystemCalls

The ui module is making a system call
====================
*/
intptr_t CL_UISystemCalls( intptr_t *args ) {
	switch ( args[0] ) {
	case UI_ERROR:
		Com_Error( ERR_DROP, "%s", (const char*)VMA(1) );
		return 0;

	case UI_PRINT:
		Com_Printf( "%s", (const char*)VMA(1) );
		return 0;

	case UI_MILLISECONDS:
		return Sys_Milliseconds();

	case UI_CVAR_REGISTER:
		Cvar_Register( VMA( 1 ), VMA( 2 ), VMA( 3 ), args[4] );
		return 0;

	case UI_CVAR_UPDATE:
		Cvar_Update( VMA( 1 ) );
		return 0;

	case UI_CVAR_SET:
		Cvar_SetSafe( VMA(1), VMA(2) );
		return 0;

	case UI_CVAR_VARIABLEVALUE:
		return FloatAsInt( Cvar_VariableValue( VMA( 1 ) ) );

	case UI_CVAR_VARIABLESTRINGBUFFER:
		Cvar_VariableStringBuffer( VMA( 1 ), VMA( 2 ), args[3] );
		return 0;

	case UI_CVAR_SETVALUE:
		Cvar_SetValueSafe( VMA(1), VMF(2) );
		return 0;

	case UI_CVAR_RESET:
		Cvar_Reset( VMA( 1 ) );
		return 0;

	case UI_CVAR_CREATE:
		Cvar_Register( NULL, VMA(1), VMA(2), args[3] );
		return 0;

	case UI_CVAR_INFOSTRINGBUFFER:
		Cvar_InfoStringBuffer( args[1], VMA( 2 ), args[3] );
		return 0;

	case UI_ARGC:
		return Cmd_Argc();

	case UI_ARGV:
		Cmd_ArgvBuffer( args[1], VMA( 2 ), args[3] );
		return 0;

	case UI_CMD_EXECUTETEXT:
		if(args[1] == EXEC_NOW
		&& (!strncmp(VMA(2), "snd_restart", 11)
		|| !strncmp(VMA(2), "vid_restart", 11)
		|| !strncmp(VMA(2), "quit", 5)))
		{
			Com_Printf (S_COLOR_YELLOW "turning EXEC_NOW '%.11s' into EXEC_INSERT\n", (const char*)VMA(2));
			args[1] = EXEC_INSERT;
		}
		Cbuf_ExecuteText( args[1], VMA( 2 ) );
		return 0;

	case UI_FS_FOPENFILE:
		return FS_FOpenFileByMode( VMA( 1 ), VMA( 2 ), args[3] );

	case UI_FS_READ:
		FS_Read( VMA( 1 ), args[2], args[3] );
		return 0;

//----(SA)	added
	case UI_FS_SEEK:
		FS_Seek( args[1], args[2], args[3] );
		return 0;
//----(SA)	end

	case UI_FS_WRITE:
		FS_Write( VMA( 1 ), args[2], args[3] );
		return 0;

	case UI_FS_FCLOSEFILE:
		FS_FCloseFile( args[1] );
		return 0;

	case UI_FS_DELETEFILE:
		return FS_Delete( VMA( 1 ) );

	case UI_FS_GETFILELIST:
		return FS_GetFileList( VMA( 1 ), VMA( 2 ), VMA( 3 ), args[4] );

	case UI_R_REGISTERMODEL:
		return re.RegisterModel( VMA( 1 ) );

	case UI_R_REGISTERSKIN:
		return re.RegisterSkin( VMA( 1 ) );

	case UI_R_REGISTERSHADERNOMIP:
		return re.RegisterShaderNoMip( VMA( 1 ) );

	case UI_R_CLEARSCENE:
		re.ClearScene();
		return 0;

	case UI_R_ADDREFENTITYTOSCENE:
		re.AddRefEntityToScene( VMA( 1 ) );
		return 0;

	case UI_R_ADDPOLYTOSCENE:
		re.AddPolyToScene( args[1], args[2], VMA( 3 ) );
		return 0;

		// Ridah
	case UI_R_ADDPOLYSTOSCENE:
		re.AddPolysToScene( args[1], args[2], VMA( 3 ), args[4] );
		return 0;
		// done.

	case UI_R_ADDLIGHTTOSCENE:
		re.AddLightToScene( VMA( 1 ), VMF( 2 ), VMF( 3 ), VMF( 4 ), VMF( 5 ), args[6] );
		return 0;

	case UI_R_ADDCORONATOSCENE:
		re.AddCoronaToScene( VMA( 1 ), VMF( 2 ), VMF( 3 ), VMF( 4 ), VMF( 5 ), args[6], args[7] );
		return 0;

	case UI_R_RENDERSCENE:
		re.RenderScene( VMA( 1 ) );
		return 0;

	case UI_R_SETCOLOR:
		re.SetColor( VMA( 1 ) );
		return 0;

	case UI_R_DRAWSTRETCHPIC:
		re.DrawStretchPic( VMF( 1 ), VMF( 2 ), VMF( 3 ), VMF( 4 ), VMF( 5 ), VMF( 6 ), VMF( 7 ), VMF( 8 ), args[9] );
		return 0;

	case UI_R_MODELBOUNDS:
		re.ModelBounds( args[1], VMA( 2 ), VMA( 3 ) );
		return 0;

	case UI_UPDATESCREEN:
		SCR_UpdateScreen();
		return 0;

	case UI_CM_LERPTAG:
		return re.LerpTag( VMA( 1 ), VMA( 2 ), VMA( 3 ), args[4] );

	case UI_S_REGISTERSOUND:
#ifdef DOOMSOUND    ///// (SA) DOOMSOUND
		return S_RegisterSound( VMA( 1 ) );
#else
		return S_RegisterSound( VMA( 1 ), qfalse );
#endif  ///// (SA) DOOMSOUND

	case UI_S_STARTLOCALSOUND:
		S_StartLocalSound( args[1], args[2] );
		return 0;

//----(SA)	added
	case UI_S_FADESTREAMINGSOUND:
		S_FadeStreamingSound( VMF( 1 ), args[2], args[3] );
		return 0;

	case UI_S_FADEALLSOUNDS:
		S_FadeAllSounds( VMF( 1 ), args[2] );
		return 0;
//----(SA)	end

	case UI_KEY_KEYNUMTOSTRINGBUF:
		Key_KeynumToStringBuf( args[1], VMA( 2 ), args[3] );
		return 0;

	case UI_KEY_GETBINDINGBUF:
		Key_GetBindingBuf( args[1], VMA( 2 ), args[3] );
		return 0;

	case UI_KEY_SETBINDING:
		Key_SetBinding( args[1], VMA( 2 ) );
		return 0;

	case UI_KEY_ISDOWN:
		return Key_IsDown( args[1] );

	case UI_KEY_GETOVERSTRIKEMODE:
		return Key_GetOverstrikeMode();

	case UI_KEY_SETOVERSTRIKEMODE:
		Key_SetOverstrikeMode( args[1] );
		return 0;

	case UI_KEY_CLEARSTATES:
		Key_ClearStates();
		return 0;

	case UI_KEY_GETCATCHER:
		return Key_GetCatcher();

	case UI_KEY_SETCATCHER:
		// Don't allow the ui module to close the console
		Key_SetCatcher( args[1] | ( Key_GetCatcher( ) & KEYCATCH_CONSOLE ) );
		return 0;

	case UI_GETCLIPBOARDDATA:
		CL_GetClipboardData( VMA(1), args[2] );
		return 0;

	case UI_GETCLIENTSTATE:
		GetClientState( VMA( 1 ) );
		return 0;

	case UI_GETGLCONFIG:
		CL_GetGlconfig( VMA( 1 ) );
		return 0;

	case UI_GETCONFIGSTRING:
		return GetConfigString( args[1], VMA( 2 ), args[3] );

	// SP has no multiplayer server browser; preserve the UI syscall ABI.
	case UI_LAN_GETPING:
		if (args[3] > 0) ((char *)VMA(2))[0] = '\0';
		*(int *)VMA(4) = 0;
		return 0;
	case UI_LAN_GETLOCALSERVERADDRESSSTRING:
	case UI_LAN_GETGLOBALSERVERADDRESSSTRING:
	case UI_LAN_GETPINGINFO:
		if (args[3] > 0) ((char *)VMA(2))[0] = '\0';
		return 0;
	case UI_LAN_GETSERVERADDRESSSTRING:
	case UI_LAN_GETSERVERINFO:
		if (args[4] > 0) ((char *)VMA(3))[0] = '\0';
		return 0;
	case UI_LAN_SERVERSTATUS:
		if (VMA(2) && args[3] > 0) ((char *)VMA(2))[0] = '\0';
		return 0;
	case UI_LAN_ADDSERVER:
	case UI_LAN_GETSERVERPING:
		return -1;
	case UI_LAN_LOADCACHEDSERVERS:
	case UI_LAN_SAVECACHEDSERVERS:
	case UI_LAN_REMOVESERVER:
	case UI_LAN_GETPINGQUEUECOUNT:
	case UI_LAN_CLEARPING:
	case UI_LAN_GETLOCALSERVERCOUNT:
	case UI_LAN_GETGLOBALSERVERCOUNT:
	case UI_LAN_GETSERVERCOUNT:
	case UI_LAN_MARKSERVERVISIBLE:
	case UI_LAN_SERVERISVISIBLE:
	case UI_LAN_UPDATEVISIBLEPINGS:
	case UI_LAN_RESETPINGS:
	case UI_LAN_COMPARESERVERS:
		return 0;

	case UI_MEMORY_REMAINING:
		return Hunk_MemoryRemaining();

	case UI_GET_CDKEY:
		CLUI_GetCDKey( VMA( 1 ), args[2] );
		return 0;

	case UI_SET_CDKEY:
#ifndef STANDALONE
		CLUI_SetCDKey( VMA( 1 ) );
#endif
		return 0;

	case UI_R_REGISTERFONT:
		re.RegisterFont( VMA( 1 ), args[2], VMA( 3 ) );
		return 0;

	case UI_MEMSET:
		Com_Memset( VMA(1), args[2], args[3] );
		return args[1];

	case UI_MEMCPY:
		Com_Memcpy( VMA(1), VMA(2), args[3] );
		return args[1];

	case UI_STRNCPY:
		strncpy( VMA(1), VMA(2), args[3] );
		return args[1];

	case UI_SIN:
		return FloatAsInt( sin( VMF( 1 ) ) );

	case UI_COS:
		return FloatAsInt( cos( VMF( 1 ) ) );

	case UI_ATAN2:
		return FloatAsInt( atan2( VMF( 1 ), VMF( 2 ) ) );

	case UI_SQRT:
		return FloatAsInt( sqrt( VMF( 1 ) ) );

	case UI_FLOOR:
		return FloatAsInt( floor( VMF( 1 ) ) );

	case UI_CEIL:
		return FloatAsInt( ceil( VMF( 1 ) ) );

	case UI_PC_ADD_GLOBAL_DEFINE:
		return botlib_export->PC_AddGlobalDefine( VMA( 1 ) );
	case UI_PC_LOAD_SOURCE:
		return botlib_export->PC_LoadSourceHandle( VMA( 1 ) );
	case UI_PC_FREE_SOURCE:
		return botlib_export->PC_FreeSourceHandle( args[1] );
	case UI_PC_READ_TOKEN:
		return botlib_export->PC_ReadTokenHandle( args[1], VMA( 2 ) );
	case UI_PC_SOURCE_FILE_AND_LINE:
		return botlib_export->PC_SourceFileAndLine( args[1], VMA( 2 ), VMA( 3 ) );

	case UI_S_STOPBACKGROUNDTRACK:
		S_StopBackgroundTrack();
		return 0;
	case UI_S_STARTBACKGROUNDTRACK:
		S_StartBackgroundTrack( VMA( 1 ), VMA( 2 ) );
		return 0;

	case UI_REAL_TIME:
		return Com_RealTime( VMA( 1 ) );

	case UI_CIN_PLAYCINEMATIC:
		Com_DPrintf( "UI_CIN_PlayCinematic\n" );
		return CIN_PlayCinematic( VMA( 1 ), args[2], args[3], args[4], args[5], args[6] );

	case UI_CIN_STOPCINEMATIC:
		return CIN_StopCinematic( args[1] );

	case UI_CIN_RUNCINEMATIC:
		return CIN_RunCinematic( args[1] );

	case UI_CIN_DRAWCINEMATIC:
		CIN_DrawCinematic( args[1] );
		return 0;

	case UI_CIN_SETEXTENTS:
		CIN_SetExtents( args[1], args[2], args[3], args[4], args[5] );
		return 0;

	case UI_R_REMAP_SHADER:
		re.RemapShader( VMA( 1 ), VMA( 2 ), VMA( 3 ) );
		return 0;

	case UI_VERIFY_CDKEY:
		return CL_CDKeyValidate( VMA( 1 ), VMA( 2 ) );

		// NERVE - SMF
	case UI_CL_GETLIMBOSTRING:
		return CL_GetLimboString( args[1], VMA( 2 ) );
		// -NERVE - SMF

	// New in IORTCW
	case UI_ALLOC:
		return VM_Alloc( args[1] );

	default:
		Com_Error( ERR_DROP, "Bad UI system trap: %ld", (long int) args[0] );

	}

	return 0;
}

/*
====================
CL_ShutdownUI
====================
*/
void CL_ShutdownUI( void ) {
	Key_SetCatcher( Key_GetCatcher( ) & ~KEYCATCH_UI );
	cls.uiStarted = qfalse;
	if ( !uivm ) {
		return;
	}
	VM_Call( uivm, UI_SHUTDOWN );
	VM_Free( uivm );
	uivm = NULL;
}

/*
====================
CL_InitUI
====================
*/

void CL_InitUI( void ) {
	int v;

	// load the dll or bytecode
	uivm = VM_Create( "ui", CL_UISystemCalls, Cvar_VariableValue("vm_ui") );
	if ( !uivm ) {
		Com_Error( ERR_FATAL, "VM_Create on UI failed" );
	}

	// sanity check
	v = VM_Call( uivm, UI_GETAPIVERSION );
	if ( v != UI_API_VERSION ) {
		Com_Error( ERR_FATAL, "User Interface is version %d, expected %d", v, UI_API_VERSION );
		cls.uiStarted = qfalse;
	}

	// init for this gamestate
	VM_Call( uivm, UI_INIT, ( clc.state >= CA_AUTHORIZING && clc.state < CA_ACTIVE ) );
}


#ifndef STANDALONE
qboolean UI_usesUniqueCDKey( void ) {
	if ( uivm ) {
		return ( VM_Call( uivm, UI_HASUNIQUECDKEY ) == qtrue );
	} else {
		return qfalse;
	}
}
#endif

/*
====================
UI_GameCommand

See if the current console command is claimed by the ui
====================
*/
qboolean UI_GameCommand( void ) {
	if ( !uivm ) {
		return qfalse;
	}

	return VM_Call( uivm, UI_CONSOLE_COMMAND, cls.realtime );
}
