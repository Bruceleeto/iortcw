/*
 * DC_PROF: the time each system takes (dc_prof.h), and a line every
 * com_profile seconds (0: none) with the frames a second and each system's
 * milliseconds a frame:
 *
 *   PROF 24.6 fps (40.7 ms, worst 81): game 2.1 ai 6.0 pathing 0.8 ...
 */
#ifdef DC_PROF

#include "q_shared.h"
#include "qcommon.h"

static const char *const sectionNames[PROF_NUM] = {
	"engine", "game", "ai", "pathing", "collision", "cgame", "ui",
	"scene", "draw", "gpu", "sound", "idle"
};

#define PROF_DEPTH  32

static long long sectionTime[PROF_NUM];  // microseconds since the last line
static profSection_t stack[PROF_DEPTH];
static int depth;
static long long last;                  // when the time was last given out

static cvar_t *com_profile;

// what's gone since last to the section now running
static void Charge( long long now ) {
	if ( last ) {
		sectionTime[depth ? stack[depth - 1] : PROF_ENGINE] += now - last;
	}
	last = now;
}

void Com_ProfBegin( profSection_t section ) {
	Charge( Sys_Microseconds() );
	if ( depth < PROF_DEPTH ) {
		stack[depth] = section;
	}
	depth++;
}

void Com_ProfEnd( profSection_t section ) {
	Charge( Sys_Microseconds() );
	if ( depth > 0 ) {
		depth--;
	}
	(void)section;
}

/*
==================
Com_ProfFrame

At the start of each frame. A section an error left (Com_Error's longjmp)
is closed, and every com_profile seconds the line is printed.
==================
*/
void Com_ProfFrame( void ) {
	static long long lineStart, frameStart;
	static int frames, worst;
	long long now;
	int i, frameMs;

	if ( !com_profile ) {
		com_profile = Cvar_Get( "com_profile", "5", 0 );
	}
	now = Sys_Microseconds();
	Charge( now );
	depth = 0;

	if ( frameStart ) {
		frameMs = (int)( ( now - frameStart ) / 1000 );
		if ( frameMs > worst ) {
			worst = frameMs;
		}
		frames++;
	}
	frameStart = now;
	if ( !lineStart ) {
		lineStart = now;
	}

	if ( com_profile->value > 0 && frames && now - lineStart >= (long long)( com_profile->value * 1000000 ) ) {
		char line[MAX_STRING_CHARS];
		double total = (double)( now - lineStart );

		Com_sprintf( line, sizeof( line ), "PROF %.1f fps (%.1f ms, worst %d):", frames * 1000000.0 / total,
					 total / 1000.0 / frames, worst );
		for ( i = 0; i < PROF_NUM; i++ ) {
			Q_strcat( line, sizeof( line ), va( " %s %.1f", sectionNames[i], sectionTime[i] / 1000.0 / frames ) );
		}
		Com_Printf( "%s\n", line );

		memset( sectionTime, 0, sizeof( sectionTime ) );
		lineStart = now;
		frames = 0;
		worst = 0;
	}
}

#endif
