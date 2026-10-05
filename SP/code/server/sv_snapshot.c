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
#include "../cgame/cg_public.h"


/*
=============================================================================

Build a client snapshot structure

=============================================================================
*/

typedef struct {
	int numSnapshotEntities;
	int snapshotEntities[MAX_SNAPSHOT_ENTITIES];
} snapshotEntityNumbers_t;

/*
=======================
SV_QsortEntityNumbers
=======================
*/
static int QDECL SV_QsortEntityNumbers( const void *a, const void *b ) {
	int *ea, *eb;

	ea = (int *)a;
	eb = (int *)b;

	if ( *ea == *eb ) {
		Com_Error( ERR_DROP, "SV_QsortEntityStates: duplicated entity" );
	}

	if ( *ea < *eb ) {
		return -1;
	}

	return 1;
}


/*
===============
SV_AddEntToSnapshot
===============
*/
static void SV_AddEntToSnapshot( svEntity_t *svEnt, sharedEntity_t *gEnt, snapshotEntityNumbers_t *eNums ) {
	// if we have already added this entity to this snapshot, don't add again
	if ( svEnt->snapshotCounter == sv.snapshotCounter ) {
		return;
	}
	svEnt->snapshotCounter = sv.snapshotCounter;

	// if we are full, silently discard entities
	if ( eNums->numSnapshotEntities == MAX_SNAPSHOT_ENTITIES ) {
		return;
	}

	eNums->snapshotEntities[ eNums->numSnapshotEntities ] = gEnt->s.number;
	eNums->numSnapshotEntities++;
}

/*
===============
SV_AddEntitiesVisibleFromPoint
===============
*/
static void SV_AddEntitiesVisibleFromPoint( vec3_t origin, clientSnapshot_t *frame,
//									snapshotEntityNumbers_t *eNums, qboolean portal, clientSnapshot_t *oldframe, qboolean localClient ) {
//									snapshotEntityNumbers_t *eNums, qboolean portal ) {
											snapshotEntityNumbers_t *eNums, qboolean portal, qboolean localClient  ) {
	int e, i;
	sharedEntity_t *ent, *playerEnt;
	svEntity_t  *svEnt;
	int l;
	int clientarea, clientcluster;
	int leafnum;
	byte    *clientpvs;
	byte    *bitvector;

	// during an error shutdown message we may need to transmit
	// the shutdown message after the server has shutdown, so
	// specfically check for it
	if ( !sv.state ) {
		return;
	}

	leafnum = CM_PointLeafnum( origin );
	clientarea = CM_LeafArea( leafnum );
	clientcluster = CM_LeafCluster( leafnum );

	// calculate the visible areas
	frame->areabytes = CM_WriteAreaBits( frame->areabits, clientarea );

	clientpvs = CM_ClusterPVS( clientcluster );

	playerEnt = SV_GentityNum( frame->ps.clientNum );

	for ( e = 0 ; e < sv.num_entities ; e++ ) {
		ent = SV_GentityNum( e );

		// never send entities that aren't linked in
		if ( !ent->r.linked ) {
			continue;
		}

		if ( ent->s.number != e ) {
			Com_DPrintf( "FIXING ENT->S.NUMBER!!!\n" );
			ent->s.number = e;
		}

		// entities can be flagged to explicitly not be sent to the client
		if ( ent->r.svFlags & SVF_NOCLIENT ) {
			continue;
		}

		// entities can be flagged to be sent to only one client
		if ( ent->r.svFlags & SVF_SINGLECLIENT ) {
			if ( ent->r.singleClient != frame->ps.clientNum ) {
				continue;
			}
		}
		// entities can be flagged to be sent to everyone but one client
		if ( ent->r.svFlags & SVF_NOTSINGLECLIENT ) {
			if ( ent->r.singleClient == frame->ps.clientNum ) {
				continue;
			}
		}

		svEnt = SV_SvEntityForGentity( ent );

		// don't double add an entity through portals
		if ( svEnt->snapshotCounter == sv.snapshotCounter ) {
			continue;
		}

		// if this client is viewing from a camera, only add ents visible from portal ents
		if ( ( playerEnt->s.eFlags & EF_VIEWING_CAMERA ) && !portal ) {
			if ( ent->r.svFlags & SVF_PORTAL ) {
				SV_AddEntToSnapshot( svEnt, ent, eNums );
//				SV_AddEntitiesVisibleFromPoint( ent->s.origin2, frame, eNums, qtrue, oldframe, localClient );
				SV_AddEntitiesVisibleFromPoint( ent->s.origin2, frame, eNums, qtrue, localClient );
			}
			continue;
		}

		// broadcast entities are always sent
		if ( ent->r.svFlags & SVF_BROADCAST ) {
			SV_AddEntToSnapshot( svEnt, ent, eNums );
			continue;
		}

		// ignore if not touching a PV leaf
		// check area
		if ( !CM_AreasConnected( clientarea, svEnt->areanum ) ) {
			// doors can legally straddle two areas, so
			// we may need to check another one
			if ( !CM_AreasConnected( clientarea, svEnt->areanum2 ) ) {
				goto notVisible;    // blocked by a door
			}
		}

		bitvector = clientpvs;

		// check individual leafs
		if ( !svEnt->numClusters ) {
			goto notVisible;
		}
		l = 0;
		for ( i = 0 ; i < svEnt->numClusters ; i++ ) {
			l = svEnt->clusternums[i];
			if ( bitvector[l >> 3] & ( 1 << ( l & 7 ) ) ) {
				break;
			}
		}

		// if we haven't found it to be visible,
		// check overflow clusters that coudln't be stored
		if ( i == svEnt->numClusters ) {
			if ( svEnt->lastCluster ) {
				for ( ; l <= svEnt->lastCluster ; l++ ) {
					if ( bitvector[l >> 3] & ( 1 << ( l & 7 ) ) ) {
						break;
					}
				}
				if ( l == svEnt->lastCluster ) {
					goto notVisible;    // not visible
				}
			} else {
				goto notVisible;
			}
		}

		//----(SA) added "visibility dummies"
		if ( ent->r.svFlags & SVF_VISDUMMY ) {
			sharedEntity_t *ment = 0;

			//find master;
			ment = SV_GentityNum( ent->s.otherEntityNum );

			if ( ment ) {
				svEntity_t *master = 0;
				master = SV_SvEntityForGentity( ment );

				if ( master->snapshotCounter == sv.snapshotCounter || !ment->r.linked ) {
					goto notVisible;
					//continue;
				}

				SV_AddEntToSnapshot( master, ment, eNums );
			}
			goto notVisible;
			//continue;	// master needs to be added, but not this dummy ent
		}
		//----(SA) end
		else if ( ent->r.svFlags & SVF_VISDUMMY_MULTIPLE ) {
			{
				int h;
				sharedEntity_t *ment = 0;
				svEntity_t *master = 0;

				for ( h = 0; h < sv.num_entities; h++ )
				{
					ment = SV_GentityNum( h );

					if ( ment == ent ) {
						continue;
					}

					if ( ment ) {
						master = SV_SvEntityForGentity( ment );
					} else {
						continue;
					}

					if ( !( ment->r.linked ) ) {
						continue;
					}

					if ( ment->s.number != h ) {
						Com_DPrintf( "FIXING vis dummy multiple ment->S.NUMBER!!!\n" );
						ment->s.number = h;
					}

					if ( ment->r.svFlags & SVF_NOCLIENT ) {
						continue;
					}

					if ( master->snapshotCounter == sv.snapshotCounter ) {
						continue;
					}

					if ( ment->s.otherEntityNum == ent->s.number ) {
						SV_AddEntToSnapshot( master, ment, eNums );
					}
				}
				goto notVisible;
			}
		}

		// add it
		SV_AddEntToSnapshot( svEnt, ent, eNums );

		// if it's a portal entity, add everything visible from its camera position
		if ( ent->r.svFlags & SVF_PORTAL ) {
//			SV_AddEntitiesVisibleFromPoint( ent->s.origin2, frame, eNums, qtrue, oldframe, localClient );
			SV_AddEntitiesVisibleFromPoint( ent->s.origin2, frame, eNums, qtrue, localClient );
		}

		continue;

notVisible:

		// Ridah, if this entity has changed events, then send it regardless of whether we can see it or not
		// DHM - Nerve :: not in multiplayer please
		if ( sv_gametype->integer == GT_SINGLE_PLAYER && localClient ) {
			if ( ent->r.eventTime == svs.time ) {
				ent->s.eFlags |= EF_NODRAW;     // don't draw, just process event
				SV_AddEntToSnapshot( svEnt, ent, eNums );
			} else if ( ent->s.eType == ET_PLAYER ) {
				// keep players around if they are alive and active (so sounds dont get messed up)
				if ( !( ent->s.eFlags & EF_DEAD ) ) {
					ent->s.eFlags |= EF_NODRAW;     // don't draw, just process events and sounds
					SV_AddEntToSnapshot( svEnt, ent, eNums );
				}
			}
		}

	}
}

/*
=============
SV_BuildClientSnapshot

Decides which entities are going to be visible to the client, and
copies off the playerstate and areabits.

This properly handles multiple recursive portals, but the render
currently doesn't.

For viewing through other player's eyes, clent can be something other than client->gentity
=============
*/
static void SV_BuildClientSnapshot( client_t *client ) {
	vec3_t org;
//	clientSnapshot_t			*frame, *oldframe;
	clientSnapshot_t            *frame;
	snapshotEntityNumbers_t entityNumbers;
	int i;
	sharedEntity_t              *ent;
	entityState_t               *state;
	svEntity_t                  *svEnt;
	sharedEntity_t              *clent;
	int clientNum;
	playerState_t               *ps;

	// bump the counter used to prevent double adding
	sv.snapshotCounter++;

	// this is the frame we are creating
	client->snapshotNum++;
	frame = &SV_ClientFrames( client )[ client->snapshotNum & PACKET_MASK ];

	// clear everything in this snapshot
	entityNumbers.numSnapshotEntities = 0;
	memset( frame->areabits, 0, sizeof( frame->areabits ) );
	frame->num_entities = 0;
	frame->first_entity = svs.nextSnapshotEntities;

	// send over the current server time so the client can drift
	// its view of time to try to match
	if ( client->oldServerTime ) {
		// The server has not yet got an acknowledgement of the
		// new gamestate from this client, so continue to send it
		// a time as if the server has not restarted. Note from
		// the client's perspective this time is strictly speaking
		// incorrect, but since it'll be busy loading a map at
		// the time it doesn't really matter.
		frame->serverTime = sv.time + client->oldServerTime;
	} else {
		frame->serverTime = sv.time;
	}
	frame->snapFlags = svs.snapFlagServerBit;
	if ( client->state != CS_ACTIVE ) {
		frame->snapFlags |= SNAPFLAG_NOT_ACTIVE;
	}
	// the server commands so far go before this snapshot
	frame->serverCommandNum = client->reliableSequence;

	clent = client->gentity;
	if ( !clent || client->state == CS_ZOMBIE ) {
		return;
	}

	// grab the current playerState_t
	ps = SV_GameClientNum( client - svs.clients );
	frame->ps = *ps;

	// never send client's own entity, because it can
	// be regenerated from the playerstate
	clientNum = frame->ps.clientNum;
	if ( clientNum < 0 || clientNum >= MAX_GENTITIES ) {
		Com_Error( ERR_DROP, "SV_SvEntityForGentity: bad gEnt" );
	}
	svEnt = &sv.svEntities[ clientNum ];

	svEnt->snapshotCounter = sv.snapshotCounter;

	// find the client's viewpoint
	VectorCopy( ps->origin, org );
	org[2] += ps->viewheight;

//----(SA)	added for 'lean'
	// need to account for lean, so areaportal doors draw properly
	if ( frame->ps.leanf != 0 ) {
		vec3_t right, v3ViewAngles;
		VectorCopy( ps->viewangles, v3ViewAngles );
		v3ViewAngles[2] += frame->ps.leanf / 2.0f;
		AngleVectors( v3ViewAngles, NULL, right, NULL );
		VectorMA( org, frame->ps.leanf, right, org );
	}
//----(SA)	end

	// add all the entities directly visible to the eye, which
	// may include portal entities that merge other viewpoints
	SV_AddEntitiesVisibleFromPoint( org, frame, &entityNumbers, qfalse, !client->bot );

	// if there were portals visible, there may be out of order entities
	// in the list which will need to be resorted for the delta compression
	// to work correctly.  This also catches the error condition
	// of an entity being included twice.
	qsort( entityNumbers.snapshotEntities, entityNumbers.numSnapshotEntities,
		   sizeof( entityNumbers.snapshotEntities[0] ), SV_QsortEntityNumbers );

	// now that all viewpoint's areabits have been OR'd together, invert
	// all of them to make it a mask vector, which is what the renderer wants
	for ( i = 0 ; i < MAX_MAP_AREA_BYTES / 4 ; i++ ) {
		( (int *)frame->areabits )[i] = ( (int *)frame->areabits )[i] ^ -1;
	}

	// copy the entity states out
	for ( i = 0 ; i < entityNumbers.numSnapshotEntities ; i++ ) {
		ent = SV_GentityNum( entityNumbers.snapshotEntities[i] );
		state = &svs.snapshotEntities[svs.nextSnapshotEntities % svs.numSnapshotEntities];
		*state = ent->s;
		svs.nextSnapshotEntities++;
		// this should never hit, map should always be restarted first in SV_Frame
		if ( svs.nextSnapshotEntities >= 0x7FFFFFFE ) {
			Com_Error( ERR_FATAL, "svs.nextSnapshotEntities wrapped" );
		}
		frame->num_entities++;
	}
}


/*
=======================
SV_SendClientSnapshot
=======================
*/
void SV_SendClientSnapshot( client_t *client ) {
	//RF, AI don't need snapshots built
	if ( client->gentity && client->gentity->r.svFlags & SVF_CASTAI ) {
		return;
	}

	// build the snapshot; the client reads it in place, bots query it
	SV_BuildClientSnapshot( client );
}


/*
=======================
SV_SendClientMessages

A snapshot for each client whenever the world has moved on
=======================
*/
void SV_SendClientMessages(void)
{
	int		i;
	client_t    *c;

	for(i=0; i < sv_maxclients->integer; i++)
	{
		c = &svs.clients[i];

		if ( c->state < CS_CONNECTED ) {
			continue;       // not connected
		}
		if ( c->lastSnapshotTime == svs.time ) {
			continue;       // nothing new since the last one
		}

		SV_SendClientSnapshot(c);
		c->lastSnapshotTime = svs.time;
	}
}


/* Cast AI do not need snapshot frames. Allocate them only when a client uses them. */
clientSnapshot_t *SV_ClientFrames( client_t *client ) {
	if ( !client->frames ) {
		client->frames = Z_Malloc( PACKET_BACKUP * sizeof( *client->frames ) );
	}
	return client->frames;
}


/*
=======================
SV_LocalSnapshotInfo

What the client keeps of the newest snapshot
=======================
*/
int SV_LocalSnapshotInfo( int *serverTime, int *snapFlags, int *serverCommandSequence, playerState_t *ps ) {
	client_t *cl = SV_LocalClient();
	clientSnapshot_t *frame;

	if ( !cl || !cl->frames || !cl->snapshotNum ) {
		return 0;
	}
	frame = &cl->frames[cl->snapshotNum & PACKET_MASK];
	*serverTime = frame->serverTime;
	*snapFlags = frame->snapFlags;
	*serverCommandSequence = cl->reliableSequence;
	*ps = frame->ps;
	return cl->snapshotNum;
}

/*
=======================
SV_LocalSnapshot

A snapshot the server built for the client, false once it is gone
=======================
*/
qboolean SV_LocalSnapshot( int snapshotNum, snapshot_t *snapshot ) {
	client_t *cl = SV_LocalClient();
	clientSnapshot_t *frame;
	int i, count;

	if ( !cl || !cl->frames || snapshotNum <= 0 || snapshotNum > cl->snapshotNum
		 || cl->snapshotNum - snapshotNum >= PACKET_BACKUP ) {
		return qfalse;
	}
	frame = &cl->frames[snapshotNum & PACKET_MASK];

	// if the entities in the frame have fallen out of their
	// circular buffer, we can't return it
	if ( svs.nextSnapshotEntities - frame->first_entity > svs.numSnapshotEntities ) {
		return qfalse;
	}

	snapshot->snapFlags = frame->snapFlags;
	snapshot->serverCommandSequence = frame->serverCommandNum;
	snapshot->ping = 0;
	snapshot->serverTime = frame->serverTime;
	memcpy( snapshot->areamask, frame->areabits, sizeof( snapshot->areamask ) );
	snapshot->ps = frame->ps;
	count = frame->num_entities;
	if ( count > MAX_ENTITIES_IN_SNAPSHOT ) {
		Com_DPrintf( "SV_LocalSnapshot: truncated %i entities to %i\n", count, MAX_ENTITIES_IN_SNAPSHOT );
		count = MAX_ENTITIES_IN_SNAPSHOT;
	}
	snapshot->numEntities = count;
	for ( i = 0 ; i < count ; i++ ) {
		snapshot->entities[i] = svs.snapshotEntities[( frame->first_entity + i ) % svs.numSnapshotEntities];
	}
	return qtrue;
}

/*
=======================
SV_LocalPlayerState

The player's state as the game has it now: after this frame's usercmd, which
CL_SendCmd ran through SV_LocalClientMessage before cgame draws, so cgame can
take it instead of predicting (CG_PredictPlayerState)
=======================
*/
qboolean SV_LocalPlayerState( playerState_t *ps ) {
	client_t *cl = SV_LocalClient();

	if ( !cl || cl->state != CS_ACTIVE || !cl->gentity ) {
		return qfalse;
	}
	*ps = *SV_GameClientNum( cl - svs.clients );
	return qtrue;
}
