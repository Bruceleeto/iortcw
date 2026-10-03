/*
 * Dreamcast input: the controller, and a maple keyboard and mouse when one
 * is attached.  Replaces sdl/sdl_input.c.
 *
 * Each device is polled once per IN_Frame and compared with the previous
 * poll, so key and button changes become SE_KEY / SE_CHAR / SE_MOUSE events
 * on the main thread (the KOS keyboard callback runs from the maple driver).
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
#include <dc/maple/keyboard.h>
#include <dc/maple/mouse.h>

#include "client.h"

static cvar_t *in_mouse;
static cvar_t *in_joyLook;
static cvar_t *in_joyCursor;

static qboolean	kbdDown[KBD_MAX_KEYS];
static uint8_t	lastMods;
static uint32_t	lastButtons;

/* DC raw key -> engine key; 0 = use the unshifted ASCII value */
static int DC_KeyToEngine( int key ) {
	if ( key >= KBD_KEY_F1 && key <= KBD_KEY_F12 ) {
		return K_F1 + ( key - KBD_KEY_F1 );
	}
	switch ( key ) {
	case KBD_KEY_ENTER:			return K_ENTER;
	case KBD_KEY_ESCAPE:		return K_ESCAPE;
	case KBD_KEY_BACKSPACE:		return K_BACKSPACE;
	case KBD_KEY_TAB:			return K_TAB;
	case KBD_KEY_SPACE:			return K_SPACE;
	case KBD_KEY_TILDE:			return K_CONSOLE;
	case KBD_KEY_CAPSLOCK:		return K_CAPSLOCK;
	case KBD_KEY_PAUSE:			return K_PAUSE;
	case KBD_KEY_INSERT:		return K_INS;
	case KBD_KEY_HOME:			return K_HOME;
	case KBD_KEY_PGUP:			return K_PGUP;
	case KBD_KEY_DEL:			return K_DEL;
	case KBD_KEY_END:			return K_END;
	case KBD_KEY_PGDOWN:		return K_PGDN;
	case KBD_KEY_RIGHT:			return K_RIGHTARROW;
	case KBD_KEY_LEFT:			return K_LEFTARROW;
	case KBD_KEY_DOWN:			return K_DOWNARROW;
	case KBD_KEY_UP:			return K_UPARROW;
	case KBD_KEY_PAD_NUMLOCK:	return K_KP_NUMLOCK;
	case KBD_KEY_PAD_DIVIDE:	return K_KP_SLASH;
	case KBD_KEY_PAD_ENTER:		return K_KP_ENTER;
	case KBD_KEY_PAD_1:			return K_KP_END;
	case KBD_KEY_PAD_1 + 1:		return K_KP_DOWNARROW;
	case KBD_KEY_PAD_1 + 2:		return K_KP_PGDN;
	case KBD_KEY_PAD_1 + 3:		return K_KP_LEFTARROW;
	case KBD_KEY_PAD_1 + 4:		return K_KP_5;
	case KBD_KEY_PAD_1 + 5:		return K_KP_RIGHTARROW;
	case KBD_KEY_PAD_1 + 6:		return K_KP_HOME;
	case KBD_KEY_PAD_1 + 7:		return K_KP_UPARROW;
	case KBD_KEY_PAD_1 + 8:		return K_KP_PGUP;
	case KBD_KEY_PAD_0:			return K_KP_INS;
	case KBD_KEY_PAD_PERIOD:	return K_KP_DEL;
	case KBD_KEY_PAD_DIVIDE + 1:	return K_KP_STAR;
	case KBD_KEY_PAD_DIVIDE + 2:	return K_KP_MINUS;
	case KBD_KEY_PAD_DIVIDE + 3:	return K_KP_PLUS;
	default:					return 0;
	}
}

static void DC_Keyboard( int time ) {
	static const kbd_leds_t noLeds = { .raw = 0 };
	static const kbd_mods_t noMods = { .raw = 0 };
	maple_device_t *dev = maple_enum_type( 0, MAPLE_FUNC_KEYBOARD );
	kbd_state_t *state;
	qboolean down[KBD_MAX_KEYS];
	uint8_t mods;
	int i;

	if ( !dev || !( state = kbd_get_state( dev ) ) ) {
		return;
	}

	/* modifiers are bits, not keys */
	mods = state->cond.modifiers.raw;
	if ( ( mods ^ lastMods ) & KBD_MOD_SHIFT ) {
		Com_QueueEvent( time, SE_KEY, K_SHIFT, ( mods & KBD_MOD_SHIFT ) != 0, 0, NULL );
	}
	if ( ( mods ^ lastMods ) & KBD_MOD_CTRL ) {
		Com_QueueEvent( time, SE_KEY, K_CTRL, ( mods & KBD_MOD_CTRL ) != 0, 0, NULL );
	}
	if ( ( mods ^ lastMods ) & KBD_MOD_ALT ) {
		Com_QueueEvent( time, SE_KEY, K_ALT, ( mods & KBD_MOD_ALT ) != 0, 0, NULL );
	}
	lastMods = mods;

	for ( i = 0; i < KBD_MAX_KEYS; i++ ) {
		down[i] = state->key_states[i].is_down;
	}

	for ( i = 0; i < KBD_MAX_KEYS; i++ ) {
		int key;

		if ( down[i] == kbdDown[i] ) {
			continue;
		}
		kbdDown[i] = down[i];

		key = DC_KeyToEngine( i );
		if ( !key ) {
			key = (unsigned char)kbd_key_to_ascii( i, state->region, noMods, noLeds );
			if ( key >= 'A' && key <= 'Z' ) {
				key += 'a' - 'A';
			}
		}
		if ( !key ) {
			continue;
		}
		Com_QueueEvent( time, SE_KEY, key, down[i], 0, NULL );

		/* typed text for the console and menus */
		if ( down[i] && key != K_CONSOLE ) {
			int ch;

			if ( key == K_BACKSPACE ) {
				ch = '\b';
			} else if ( key == K_ENTER || key == K_KP_ENTER ) {
				ch = '\r';
			} else {
				ch = (unsigned char)kbd_key_to_ascii( i, state->region, state->cond.modifiers, state->cond.leds );
			}
			if ( ch ) {
				Com_QueueEvent( time, SE_CHAR, ch, 0, 0, NULL );
			}
		}
	}
}

static void DC_Mouse( int time ) {
	static const struct { uint32_t bit; int key; } buttons[] = {
		{ MOUSE_LEFTBUTTON,		K_MOUSE1 },
		{ MOUSE_RIGHTBUTTON,	K_MOUSE2 },
		{ MOUSE_MIDDLEBUTTON,	K_MOUSE3 },
		{ MOUSE_SIDEBUTTON,		K_MOUSE4 },
	};
	maple_device_t *dev;
	mouse_state_t *state;
	unsigned i;

	if ( !in_mouse->integer ) {
		return;
	}
	dev = maple_enum_type( 0, MAPLE_FUNC_MOUSE );
	if ( !dev || !( state = maple_dev_status( dev ) ) ) {
		return;
	}

	if ( state->dx || state->dy ) {
		Com_QueueEvent( time, SE_MOUSE, state->dx, state->dy, 0, NULL );
	}
	if ( state->dz ) {
		int key = state->dz < 0 ? K_MWHEELUP : K_MWHEELDOWN;
		Com_QueueEvent( time, SE_KEY, key, qtrue, 0, NULL );
		Com_QueueEvent( time, SE_KEY, key, qfalse, 0, NULL );
	}

	for ( i = 0; i < ARRAY_LEN( buttons ); i++ ) {
		if ( ( state->buttons ^ lastButtons ) & buttons[i].bit ) {
			Com_QueueEvent( time, SE_KEY, buttons[i].key, ( state->buttons & buttons[i].bit ) != 0, 0, NULL );
		}
	}
	lastButtons = state->buttons;
}

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
	if ( msec < 0 || msec > 100 ) {
		msec = 0;
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
	DC_Keyboard( time );
	DC_Mouse( time );
}

void IN_Init( void *windowData ) {
	unsigned i;

	(void)windowData;

	Com_DPrintf( "\n------- Input Initialization -------\n" );
	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );
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

	memset( kbdDown, 0, sizeof( kbdDown ) );
	lastMods = 0;
	lastButtons = 0;
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
