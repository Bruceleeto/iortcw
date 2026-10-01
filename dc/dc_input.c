/*
 * Dreamcast input: maple keyboard and mouse.  Replaces sdl/sdl_input.c.
 *
 * Both devices are polled once per IN_Frame and compared with the previous
 * poll, so key and button changes become SE_KEY / SE_CHAR / SE_MOUSE events
 * on the main thread (the KOS keyboard callback runs from the maple driver).
 *
 * Compiled against the game's client.h.
 */

#include <dc/maple.h>
#include <dc/maple/keyboard.h>
#include <dc/maple/mouse.h>

#include "client.h"

static cvar_t *in_mouse;

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

void IN_Frame( void ) {
	int time = Sys_Milliseconds();

	DC_Keyboard( time );
	DC_Mouse( time );
}

void IN_Init( void *windowData ) {
	(void)windowData;

	Com_DPrintf( "\n------- Input Initialization -------\n" );
	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );

	memset( kbdDown, 0, sizeof( kbdDown ) );
	lastMods = 0;
	lastButtons = 0;
	Com_DPrintf( "------------------------------------\n" );
}

void IN_Shutdown( void ) {
}

void IN_Restart( void ) {
	IN_Init( NULL );
}
