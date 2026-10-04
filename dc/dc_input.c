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

	for ( i = 0; i < ARRAY_LEN( padButtons ); i++ ) {
		uint32_t bit = padButtons[i].bit;

		if ( ( buttons & bit ) && !( padLast & bit ) ) {
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
		if ( !padQuit ) {
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
