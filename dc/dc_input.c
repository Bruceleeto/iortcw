/*
 * Dreamcast input: the controller (KOS has no keyboard or mouse driver in,
 * KOS_INIT_FLAGS in sys_main.c).  Replaces sdl/sdl_input.c.
 *
 * It's polled once per IN_Frame and compared with the previous poll, so
 * button changes become SE_KEY / SE_MOUSE events on the main thread.
 *
 * The controller (the first on any port):
 *
 *   in game                            in menus and the console
 *   stick      look                    stick      the cursor
 *   R          fire                    A          click
 *   L          crouch (held)           B          back (escape)
 *   A          forward; tap twice      Y          enter
 *              (and hold) to jump      D-pad      arrows
 *   B          back
 *   Y          use
 *   X          reload
 *   D-pad      left/right: weapon, up: alt fire (scope, silencer...),
 *              down: binoculars
 *   Start      tap: the menu (escape); hold 2 seconds: quit the game
 *
 * In game the buttons are keys JOY1-JOY11 and do what they're bound to (the
 * binds above are put on them at IN_Init when they have none), so they can
 * be bound again from the console.  The stick turns as the mouse does
 * (sensitivity), its full tilt in_joyLook mouse counts a millisecond;
 * in_joyCursor is the cursor's in menus.
 *
 * Compiled against the game's client.h.
 */

#include <dc/maple.h>
#include <dc/maple/controller.h>

#include "client.h"

static cvar_t *in_joyLook;
static cvar_t *in_joyCursor;

/* the triggers as buttons, past the controller's own bits */
#define PAD_LTRIG		( 1 << 16 )
#define PAD_RTRIG		( 1 << 17 )
#define PAD_TRIG_DOWN	64		/* trigger pressed past this (0-255) ... */
#define PAD_TRIG_UP		32		/* ... and let go below this */
#define PAD_DEADZONE	24		/* of the stick's 128 */
#define PAD_DOUBLE_TAP	250		/* ms between A's presses that jump */
#define PAD_QUIT_HOLD	2000	/* ms Start is held to quit */
#define K_PAD_JUMP		K_JOY11	/* A's second press */

static const struct {
	uint32_t	bit;
	int			gameKey;		/* in game, bound to binding (if it has none) */
	const char	*binding;
	int			menuKey;		/* in menus and the console; 0: nothing */
} padButtons[] = {
	{ CONT_A,			K_JOY1,		"+forward",		K_MOUSE1 },
	{ CONT_B,			K_JOY2,		"+back",		K_ESCAPE },
	{ CONT_X,			K_JOY3,		"+reload",		0 },
	{ CONT_Y,			K_JOY4,		"+activate",	K_ENTER },
	{ PAD_RTRIG,		K_JOY5,		"+attack",		0 },
	{ PAD_LTRIG,		K_JOY6,		"+movedown",	0 },
	{ CONT_DPAD_UP,		K_JOY7,		"weapalt",		K_UPARROW },
	{ CONT_DPAD_DOWN,	K_JOY8,		"+zoom",		K_DOWNARROW },
	{ CONT_DPAD_LEFT,	K_JOY9,		"weapprev",		K_LEFTARROW },
	{ CONT_DPAD_RIGHT,	K_JOY10,	"weapnext",		K_RIGHTARROW },
};

/* the cheat menus, hold Y and: Up twice, every level (spdevmap, so the
   cheats work in it); Down twice, the cheats and the guns, which stay up to
   pick more. Up/Down/Left/Right to pick, A to go, B or Start to close. They
   eat the buttons while up */
#define CHEAT_ROWS		16
typedef struct {
	const char	*label;
	const char	*cmd;		/* NULL: a level, spdevmap label */
} cheatItem_t;
static const cheatItem_t cheatLevels[] = {
	{ "cutscene1" }, { "escape1" }, { "escape2" }, { "tram" }, { "village1" }, { "crypt1" },
	{ "crypt2" }, { "church" }, { "boss1" }, { "cutscene6" }, { "forest" }, { "rocket" },
	{ "baseout" }, { "assault" }, { "cutscene9" }, { "sfm" }, { "factory" }, { "trainyard" },
	{ "swf" }, { "cutscene11" }, { "norway" }, { "xlabs" }, { "boss2" }, { "cutscene14" },
	{ "dam" }, { "village2" }, { "chateau" }, { "dark" }, { "dig" }, { "castle" },
	{ "end" }, { "cutscene19" }
};
/* give takes bg_itemlist's pickup names */
static const cheatItem_t cheatCheats[] = {
	{ "god mode", "god" }, { "no clip", "noclip" }, { "no target", "notarget" },
	{ "no fatigue", "nofatigue" }, { "give all", "give all" }, { "health", "give health" },
	{ "armor", "give armor" }, { "ammo (all guns)", "give allammo 999" }, { "keys", "give keys" },
	{ "knife", "give Knife" }, { "luger", "give Luger" }, { "silencer", "give Silencer" },
	{ "colt", "give Colt" }, { "dual colts", "give Dual Colts" }, { "mp40", "give MP40" },
	{ "thompson", "give Thompson" }, { "sten", "give Sten" }, { "mauser", "give Mauser Rifle" },
	{ "sniper scope", "give Sniper Scope" }, { "snooper rifle", "give Snooper Rifle" },
	{ "fg42", "give FG42 Paratroop Rifle" }, { "fg42 scope", "give FG42 Scope" },
	{ "panzerfaust", "give Panzerfaust" }, { "venom", "give Venom" },
	{ "flamethrower", "give Flamethrower" }, { "tesla", "give Tesla Gun" },
	{ "grenades", "give Grenade" }, { "pineapples", "give Pineapple" },
	{ "dynamite", "give Dynamite Weapon" }
};
static const cheatItem_t	*cheatMenu;		/* the one up; NULL: none */
static int		cheatCount;
static int		cheatSel;
static int		cheatUps, cheatDowns;		/* their presses with Y held */

static void DC_CheatOpen( const cheatItem_t *items, int count ) {
	cheatMenu = items;
	cheatCount = count;
	cheatSel = 0;
	cheatUps = cheatDowns = 0;
}

static uint32_t	padLast;					/* buttons down the last poll */
static int		padSent[ARRAY_LEN( padButtons )];	/* the key each one's press sent, to let go */
static int		padLastA;					/* when A was last pressed */
static qboolean	padJump;					/* K_PAD_JUMP is down */
static int		padStartTime;				/* when Start went down; 0: it's up */
static qboolean	padQuit;
static float	padDx, padDy;				/* the stick's movement not yet sent */
static int		padTime;

/* the stick's axis, -1 to 1 past the dead zone, squared for fine aim near the middle */
static float DC_StickAxis( int v ) {
	float f;

	if ( v > -PAD_DEADZONE && v < PAD_DEADZONE ) {
		return 0.0f;
	}
	f = ( v > 0 ? v - PAD_DEADZONE : v + PAD_DEADZONE ) / (float)( 128 - PAD_DEADZONE );
	if ( f > 1.0f ) {
		f = 1.0f;
	} else if ( f < -1.0f ) {
		f = -1.0f;
	}
	return f * fabs( f );
}

/* a button pressed: qtrue if a cheat menu takes it */
static qboolean DC_CheatButton( uint32_t bit, uint32_t buttons ) {
	if ( cheatMenu ) {
		const cheatItem_t *item = &cheatMenu[cheatSel];

		switch ( bit ) {
		case CONT_DPAD_UP:
			cheatSel = ( cheatSel + cheatCount - 1 ) % cheatCount;
			break;
		case CONT_DPAD_DOWN:
			cheatSel = ( cheatSel + 1 ) % cheatCount;
			break;
		case CONT_DPAD_LEFT:
			if ( cheatSel >= CHEAT_ROWS ) {
				cheatSel -= CHEAT_ROWS;
			}
			break;
		case CONT_DPAD_RIGHT:
			if ( cheatSel + CHEAT_ROWS < cheatCount ) {
				cheatSel += CHEAT_ROWS;
			}
			break;
		case CONT_A:
			if ( item->cmd ) {
				Cbuf_AddText( va( "%s\n", item->cmd ) );
			} else {
				Cbuf_AddText( va( "spdevmap %s\n", item->label ) );
				cheatMenu = NULL;
			}
			break;
		case CONT_B:
			cheatMenu = NULL;
			break;
		}
		return qtrue;
	}
	if ( !( buttons & CONT_Y ) ) {
		return qfalse;
	}
	if ( bit == CONT_DPAD_UP ) {
		cheatDowns = 0;
		if ( ++cheatUps >= 2 ) {
			DC_CheatOpen( cheatLevels, ARRAY_LEN( cheatLevels ) );
		}
		return qtrue;
	}
	if ( bit == CONT_DPAD_DOWN ) {
		cheatUps = 0;
		if ( ++cheatDowns >= 2 ) {
			DC_CheatOpen( cheatCheats, ARRAY_LEN( cheatCheats ) );
		}
		return qtrue;
	}
	return qfalse;
}

/* the cheat menu up, over everything (SCR_DrawScreenField) */
void IN_DrawCheats( void ) {
	static vec4_t back = { 0.0f, 0.0f, 0.0f, 0.8f };
	static vec4_t white = { 1.0f, 1.0f, 1.0f, 1.0f };
	static vec4_t yellow = { 1.0f, 1.0f, 0.0f, 1.0f };
	int i, x, y;

	if ( !cheatMenu ) {
		return;
	}
	SCR_FillRect( 48, 40, 544, 24 + ( CHEAT_ROWS + 2 ) * SMALLCHAR_HEIGHT, back );
	SCR_DrawSmallStringExt( 64, 52, cheatMenu == cheatLevels ? "LEVELS  A: go  B: close" :
							"CHEATS  A: use  B: close  (a level from Y+Up+Up)", yellow, qtrue, qtrue );
	for ( i = 0; i < cheatCount; i++ ) {
		x = 64 + ( i / CHEAT_ROWS ) * 272;
		y = 52 + ( 2 + i % CHEAT_ROWS ) * SMALLCHAR_HEIGHT;
		SCR_DrawSmallStringExt( x, y, va( "%c %s", i == cheatSel ? '>' : ' ', cheatMenu[i].label ),
								i == cheatSel ? yellow : white, qtrue, qtrue );
	}
	re.SetColor( NULL );
}

static void DC_Controller( int time ) {
	maple_device_t *dev = maple_enum_type( 0, MAPLE_FUNC_CONTROLLER );
	cont_state_t *state;
	qboolean menu = ( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE | KEYCATCH_CGAME ) ) != 0;
	uint32_t buttons;
	float speed;
	int msec, dx, dy;
	unsigned i;

	msec = time - padTime;
	padTime = time;
	/* capped, not dropped: a slow frame (under 10 fps) still turns */
	if ( msec < 0 ) {
		msec = 0;
	} else if ( msec > 250 ) {
		msec = 250;
	}

	if ( !dev || !( state = maple_dev_status( dev ) ) ) {
		buttons = 0;	/* unplugged: let go of everything */
		state = NULL;
	} else {
		buttons = state->buttons & ~( PAD_LTRIG | PAD_RTRIG );
		if ( state->ltrig > ( ( padLast & PAD_LTRIG ) ? PAD_TRIG_UP : PAD_TRIG_DOWN ) ) {
			buttons |= PAD_LTRIG;
		}
		if ( state->rtrig > ( ( padLast & PAD_RTRIG ) ? PAD_TRIG_UP : PAD_TRIG_DOWN ) ) {
			buttons |= PAD_RTRIG;
		}
	}

	if ( !( buttons & CONT_Y ) ) {
		cheatUps = cheatDowns = 0;
	}
	for ( i = 0; i < ARRAY_LEN( padButtons ); i++ ) {
		uint32_t bit = padButtons[i].bit;

		if ( ( buttons & bit ) && !( padLast & bit ) ) {
			if ( DC_CheatButton( bit, buttons ) ) {
				padSent[i] = 0;
				continue;
			}
			padSent[i] = menu ? padButtons[i].menuKey : padButtons[i].gameKey;
			if ( padSent[i] ) {
				Com_QueueEvent( time, SE_KEY, padSent[i], qtrue, 0, NULL );
			}
			/* A again soon after: jump, held as long as A is */
			if ( bit == CONT_A && !menu ) {
				if ( time - padLastA < PAD_DOUBLE_TAP ) {
					Com_QueueEvent( time, SE_KEY, K_PAD_JUMP, qtrue, 0, NULL );
					padJump = qtrue;
				}
				padLastA = time;
			}
		} else if ( !( buttons & bit ) && ( padLast & bit ) ) {
			if ( padSent[i] ) {
				Com_QueueEvent( time, SE_KEY, padSent[i], qfalse, 0, NULL );
				padSent[i] = 0;
			}
			if ( bit == CONT_A && padJump ) {
				Com_QueueEvent( time, SE_KEY, K_PAD_JUMP, qfalse, 0, NULL );
				padJump = qfalse;
			}
		}
	}

	/* Start: a tap is escape (the menu), held it quits */
	if ( buttons & CONT_START ) {
		if ( !padStartTime ) {
			padStartTime = time ? time : 1;
		} else if ( !padQuit && time - padStartTime >= PAD_QUIT_HOLD ) {
			padQuit = qtrue;
			Cbuf_AddText( "quit\n" );
		}
	} else if ( padStartTime ) {
		if ( cheatMenu ) {
			cheatMenu = NULL;
		} else if ( !padQuit ) {
			Com_QueueEvent( time, SE_KEY, K_ESCAPE, qtrue, 0, NULL );
			Com_QueueEvent( time, SE_KEY, K_ESCAPE, qfalse, 0, NULL );
		}
		padStartTime = 0;
	}

	padLast = buttons;

	/* the stick: mouse movement, so it looks in game and moves the cursor in menus */
	if ( !state ) {
		padDx = padDy = 0.0f;
		return;
	}
	speed = ( menu ? in_joyCursor->value : in_joyLook->value ) * msec;
	padDx += DC_StickAxis( state->joyx ) * speed;
	padDy += DC_StickAxis( state->joyy ) * speed;
	dx = (int)padDx;
	dy = (int)padDy;
	padDx -= dx;
	padDy -= dy;
	if ( dx || dy ) {
		Com_QueueEvent( time, SE_MOUSE, dx, dy, 0, NULL );
	}
}

void IN_Frame( void ) {
	int time = Sys_Milliseconds();

	DC_Controller( time );
}

void IN_Init( void *windowData ) {
	unsigned i;

	(void)windowData;

	Com_DPrintf( "\n------- Input Initialization -------\n" );
	in_joyLook = Cvar_Get( "in_joyLook", "1.6", CVAR_ARCHIVE );		/* about 180 degrees a second at sensitivity 5 */
	in_joyCursor = Cvar_Get( "in_joyCursor", "0.6", CVAR_ARCHIVE );	/* about 600 of the menus' 640 a second */

	/* the controller's binds, where nothing has bound its keys */
	for ( i = 0; i < ARRAY_LEN( padButtons ); i++ ) {
		const char *b = Key_GetBinding( padButtons[i].gameKey );
		if ( !b || !b[0] ) {
			Key_SetBinding( padButtons[i].gameKey, padButtons[i].binding );
		}
	}
	if ( !Key_GetBinding( K_PAD_JUMP ) || !Key_GetBinding( K_PAD_JUMP )[0] ) {
		Key_SetBinding( K_PAD_JUMP, "+moveup" );
	}

	memset( padSent, 0, sizeof( padSent ) );
	padLast = 0;
	padJump = qfalse;
	padStartTime = 0;
	padQuit = qfalse;
	padDx = padDy = 0.0f;
	padTime = Sys_Milliseconds();
	Com_DPrintf( "------------------------------------\n" );
}

void IN_Shutdown( void ) {
}

void IN_Restart( void ) {
	IN_Init( NULL );
}
