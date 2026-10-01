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


/*****************************************************************************
 * name:		be_aas_debug.c
 *
 * desc:		AAS debug code
 *
 *
 *****************************************************************************/

#include "../qcommon/q_shared.h"
#include "l_memory.h"
#include "l_script.h"
#include "l_precomp.h"
#include "l_struct.h"
#include "l_libvar.h"
#include "aasfile.h"
#include "botlib.h"
#include "be_aas.h"
#include "be_interface.h"
#include "be_aas_funcs.h"
#include "be_aas_def.h"

//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_ClearShownPolygons( void ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_ClearShownPolygons
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_ShowPolygon( int color, int numpoints, vec3_t *points ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_ShowPolygon
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_ClearShownDebugLines( void ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_ClearShownDebugLines
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_DebugLine( vec3_t start, vec3_t end, int color ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_DebugLine
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
void AAS_PermanentLine( vec3_t start, vec3_t end, int color ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_PermenentLine
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
void AAS_DrawPermanentCross( vec3_t origin, float size, int color ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_DrawPermanentCross
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_DrawPlaneCross( vec3_t point, vec3_t normal, float dist, int type, int color ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_DrawPlaneCross
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_ShowBoundingBox( vec3_t origin, vec3_t mins, vec3_t maxs ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_ShowBoundingBox
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_ShowFace( int facenum ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_ShowFace
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_ShowFacePolygon( int facenum, int color, int flip ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_ShowFacePolygon
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_ShowArea( int areanum, int groundfacesonly ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_ShowArea
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_ShowAreaPolygons( int areanum, int color, int groundfacesonly ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_ShowAreaPolygons
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_DrawCross( vec3_t origin, float size, int color ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_DrawCross
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_PrintTravelType( int traveltype ) {
#ifdef DEBUG
	char *str;
	//
	switch ( traveltype )
	{
	case TRAVEL_INVALID: str = "TRAVEL_INVALID"; break;
	case TRAVEL_WALK: str = "TRAVEL_WALK"; break;
	case TRAVEL_CROUCH: str = "TRAVEL_CROUCH"; break;
	case TRAVEL_BARRIERJUMP: str = "TRAVEL_BARRIERJUMP"; break;
	case TRAVEL_JUMP: str = "TRAVEL_JUMP"; break;
	case TRAVEL_LADDER: str = "TRAVEL_LADDER"; break;
	case TRAVEL_WALKOFFLEDGE: str = "TRAVEL_WALKOFFLEDGE"; break;
	case TRAVEL_SWIM: str = "TRAVEL_SWIM"; break;
	case TRAVEL_WATERJUMP: str = "TRAVEL_WATERJUMP"; break;
	case TRAVEL_TELEPORT: str = "TRAVEL_TELEPORT"; break;
	case TRAVEL_ELEVATOR: str = "TRAVEL_ELEVATOR"; break;
	case TRAVEL_ROCKETJUMP: str = "TRAVEL_ROCKETJUMP"; break;
	case TRAVEL_BFGJUMP: str = "TRAVEL_BFGJUMP"; break;
	case TRAVEL_GRAPPLEHOOK: str = "TRAVEL_GRAPPLEHOOK"; break;
	case TRAVEL_JUMPPAD: str = "TRAVEL_JUMPPAD"; break;
	case TRAVEL_FUNCBOB: str = "TRAVEL_FUNCBOB"; break;
	default: str = "UNKNOWN TRAVEL TYPE"; break;
	} //end switch
	botimport.Print( PRT_MESSAGE, "%s", str );
#endif //DEBUG
} //end of the function AAS_PrintTravelType
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_DrawArrow( vec3_t start, vec3_t end, int linecolor, int arrowcolor ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_DrawArrow
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_ShowReachability( aas_reachability_t *reach ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function AAS_ShowReachability
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_ShowReachableAreas( int areanum ) {
	// Navigation debug geometry is disabled in SP.
} //end of the function ShowReachableAreas
