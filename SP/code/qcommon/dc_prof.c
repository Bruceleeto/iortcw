/*
 * DC_PROF: the time each system takes (dc_prof.h), and a line every
 * com_profile seconds (0: none) with the frames a second and each system's
 * milliseconds a frame:
 *
 *   PROF 24.6 fps (40.7 ms, worst 81): game 2.1 ai 6.0 pathing 0.8 ...
 *
 * and under it the renderer's counts a frame (profStats):
 *
 *   RSTAT draws 410 verts 21000 tris 18000 culled 7000 clipped 30 emitted 15000
 *
 * and the collision time, by the section that asked for the traces
 * (milliseconds a frame, then traces a frame):
 *
 *   COLL ai 2.1/31 game 0.8/22 cg_static 0.3/9
 *
 * and on the Dreamcast, the SH4 performance counter event com_perfEvent
 * (a PMCR mode, perfctr.h, in decimal: 37 cycles stalled on the data cache,
 * 36 on the instruction cache, 41 on the FPU, 19 instructions; 0: none)
 * counted in each section, thousands a frame; each line the next of
 * dcache stalls (37), instructions (19), icache stalls (36), fpu stalls (41):
 *
 *   PERF 0x25: engine 12 game 40 ... pvr 2100 ...
 *
 * and the sections of any one frame longer than com_profileSpike ms (0:
 * none), at most one a second (the print itself costs), to see what a
 * hitch is made of when the lines above average it away:
 *
 *   SPIKE 83 ms: think 41.2 cg_localents 20.1 collision 9.8 ...
 */
#ifdef DC_PROF

#include "q_shared.h"
#include "qcommon.h"
#ifdef _arch_dreamcast
#include <dc/perfctr.h>
#endif

static const char *const sectionNames[PROF_NUM] = {
	"engine", "game", "ai", "pathing", "collision", "cgame",
	"cg_snaps", "cg_predict", "cg_ents", "cg_players", "cg_tags", "cg_static", "cg_marks", "cg_particles",
	"cg_localents", "cg_weapon", "cg_trails", "cg_2d", "ui",
	"scene", "sc_leaves", "sc_nodes", "sc_ents", "sc_sort", "draw", "world", "models",
	"shade", "deform", "colors", "lighting", "texcoords", "dlights", "fog",
	"sky", "flares", "pvr", "pvr_setup", "pvr_xform", "pvr_world", "pvr_tris", "submit", "gpu", "sound", "idle"
};

static const char *const statNames[STAT_NUM] = {
	"draws", "verts", "tris", "culled", "clipped", "emitted", "framehits", "framemisses", "bonecalls", "bonemisses", "tr"
};

int profStats[STAT_NUM];

#define PROF_DEPTH  32

static long long sectionTime[PROF_NUM];  // microseconds since the last line
static long long collisionTime[PROF_NUM];  // of collision's, by the section it was called from
static int collisionCount[PROF_NUM];       // and the traces
static profSection_t stack[PROF_DEPTH];
static int depth;
static long long last;                  // when the time was last given out
static int frameTicks[PROF_NUM];        // this frame's, for the SPIKE line

static cvar_t *com_profile;
static cvar_t *com_profileSpike;

/* the clock: on the Dreamcast the cycle counter KOS keeps in PRFC0 (5 ns a
   tick, two register reads), not timer_us_gettime64 with its 64-bit divides,
   which cost more than some of the sections measured */
#ifdef _arch_dreamcast
#define PROF_TICKS_PER_US   200
/* the counter registers read here, not through perf_cntr_count: a call out
   to KOS from the hottest loops, three thousand times a frame */
#define PMCTR_HIGH( c )     ( *( (volatile uint32_t *)0xff100004 + ( ( c ) << 1 ) ) )
#define PMCTR_LOW( c )      ( *( (volatile uint32_t *)0xff100008 + ( ( c ) << 1 ) ) )
static ID_INLINE uint64_t ProfCounter( int c ) {
	uint32_t hi, lo, hi2;

	do {
		hi = PMCTR_HIGH( c );
		lo = PMCTR_LOW( c );
		hi2 = PMCTR_HIGH( c );
	} while ( hi != hi2 );
	return ( (uint64_t)hi << 32 ) | lo;
}
static ID_INLINE long long ProfNow( void ) {
	return (long long)ProfCounter( PRFC0 );
}
#else
#define PROF_TICKS_PER_US   1
#define ProfNow()           Sys_Microseconds()
#endif

#ifdef _arch_dreamcast
static cvar_t *com_perfEvent;
static int perfEvent;                   // the one counting, 0: none
static uint64_t sectionEvents[PROF_NUM];
static uint64_t lastEvents;
#endif

// what's gone since last to the section now running
static ID_INLINE void Charge( long long now ) {
	profSection_t running = depth ? stack[depth - 1] : PROF_ENGINE;

	if ( last ) {
		sectionTime[running] += now - last;
		frameTicks[running] += (int)( now - last );
		if ( running == PROF_COLLISION ) {
			collisionTime[depth > 1 ? stack[depth - 2] : PROF_ENGINE] += now - last;
		}
	}
	last = now;
#ifdef _arch_dreamcast
	if ( perfEvent ) {
		uint64_t events = ProfCounter( PRFC1 );

		sectionEvents[running] += events - lastEvents;
		lastEvents = events;
	}
#endif
}

DC_HOT( "10a" ) void Com_ProfBegin( profSection_t section ) {
	Charge( ProfNow() );
	if ( section == PROF_COLLISION ) {
		collisionCount[depth ? stack[depth - 1] : PROF_ENGINE]++;
	}
	if ( depth < PROF_DEPTH ) {
		stack[depth] = section;
	}
	depth++;
}

DC_HOT( "10a" ) void Com_ProfEnd( profSection_t section ) {
	Charge( ProfNow() );
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
		com_profileSpike = Cvar_Get( "com_profileSpike", "40", 0 );
	}
#ifdef _arch_dreamcast
	if ( !com_perfEvent ) {
		com_perfEvent = Cvar_Get( "com_perfEvent", "37", 0 );
	}
	if ( com_perfEvent->integer != perfEvent ) {
		perf_cntr_stop( PRFC1 );
		perf_cntr_clear( PRFC1 );
		perfEvent = com_perfEvent->integer;
		if ( perfEvent ) {
			perf_cntr_start( PRFC1, (perf_cntr_event_t)perfEvent, PMCR_COUNT_CPU_CYCLES );
		}
		lastEvents = perf_cntr_count( PRFC1 );
		memset( sectionEvents, 0, sizeof( sectionEvents ) );
	}
#endif
	now = ProfNow();
	Charge( now );
	depth = 0;

	if ( frameStart ) {
		static long long lastSpike;

		frameMs = (int)( ( now - frameStart ) / ( 1000 * PROF_TICKS_PER_US ) );
		if ( frameMs > worst ) {
			worst = frameMs;
		}
		frames++;
		if ( com_profileSpike->integer > 0 && frameMs >= com_profileSpike->integer
			 && now - lastSpike >= 1000000LL * PROF_TICKS_PER_US ) {
			// the frame's sections, the largest first, down to half a millisecond
			char line[MAX_STRING_CHARS];
			int n;

			lastSpike = now;
			Com_sprintf( line, sizeof( line ), "SPIKE %d ms:", frameMs );
			for ( n = 0; n < 12; n++ ) {
				int best = -1;

				for ( i = 0; i < PROF_NUM; i++ ) {
					if ( frameTicks[i] >= 500 * PROF_TICKS_PER_US && ( best < 0 || frameTicks[i] > frameTicks[best] ) ) {
						best = i;
					}
				}
				if ( best < 0 ) {
					break;
				}
				Q_strcat( line, sizeof( line ), va( " %s %.1f", sectionNames[best], frameTicks[best] / ( 1000.0 * PROF_TICKS_PER_US ) ) );
				frameTicks[best] = 0;
			}
			Com_Printf( "%s\n", line );
		}
	}
	memset( frameTicks, 0, sizeof( frameTicks ) );
	frameStart = now;
	if ( !lineStart ) {
		lineStart = now;
	}

	if ( com_profile->value > 0 && frames && now - lineStart >= (long long)( com_profile->value * 1000000 ) * PROF_TICKS_PER_US ) {
		char line[MAX_STRING_CHARS];
		double total = (double)( now - lineStart ) / PROF_TICKS_PER_US;

		Com_sprintf( line, sizeof( line ), "PROF %.1f fps (%.1f ms, worst %d):", frames * 1000000.0 / total,
					 total / 1000.0 / frames, worst );
		for ( i = 0; i < PROF_NUM; i++ ) {
			Q_strcat( line, sizeof( line ), va( " %s %.1f", sectionNames[i], sectionTime[i] / ( 1000.0 * PROF_TICKS_PER_US ) / frames ) );
		}
		Com_Printf( "%s\n", line );

		Q_strncpyz( line, "RSTAT", sizeof( line ) );
		for ( i = 0; i < STAT_NUM; i++ ) {
			Q_strcat( line, sizeof( line ), va( " %s %d", statNames[i], profStats[i] / frames ) );
		}
		Com_Printf( "%s\n", line );

		Q_strncpyz( line, "COLL", sizeof( line ) );
		for ( i = 0; i < PROF_NUM; i++ ) {
			if ( collisionCount[i] ) {
				Q_strcat( line, sizeof( line ), va( " %s %.1f/%d", sectionNames[i], collisionTime[i] / ( 1000.0 * PROF_TICKS_PER_US ) / frames, collisionCount[i] / frames ) );
			}
		}
		Com_Printf( "%s\n", line );

#ifdef _arch_dreamcast
		if ( perfEvent ) {
			// the next line counts the next of these, to see them all in one run
			static const struct { int event; const char *name; } cycle[] = {
				{ PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, "dcache stalls" },
				{ PMCR_INSTRUCTION_ISSUED_MODE, "instructions" },
				{ PMCR_PIPELINE_FREEZE_BY_ICACHE_MISS_MODE, "icache stalls" },
				{ PMCR_PIPELINE_FREEZE_BY_FPU_MODE, "fpu stalls" },
				{ PMCR_PIPELINE_FREEZE_BY_BRANCH_MODE, "branch stalls" },
				{ PMCR_PIPELINE_FREEZE_BY_CPU_REGISTER_MODE, "register stalls" },
			};
			int c;

			for ( c = 0; c < ARRAY_LEN( cycle ) - 1 && cycle[c].event != perfEvent; c++ ) {
			}
			Com_sprintf( line, sizeof( line ), "PERF 0x%02x %s:", perfEvent,
						 cycle[c].event == perfEvent ? cycle[c].name : "" );
			for ( i = 0; i < PROF_NUM; i++ ) {
				Q_strcat( line, sizeof( line ), va( " %s %d", sectionNames[i], (int)( sectionEvents[i] / frames / 1000 ) ) );
			}
			Com_Printf( "%s\n", line );
			memset( sectionEvents, 0, sizeof( sectionEvents ) );
			if ( cycle[c].event == perfEvent ) {
				Cvar_Set( "com_perfEvent", va( "%d", cycle[( c + 1 ) % ARRAY_LEN( cycle )].event ) );
			}
		}
#endif
		memset( sectionTime, 0, sizeof( sectionTime ) );
		memset( collisionTime, 0, sizeof( collisionTime ) );
		memset( collisionCount, 0, sizeof( collisionCount ) );
		memset( profStats, 0, sizeof( profStats ) );
		lineStart = now;
		frames = 0;
		worst = 0;
		// the lines themselves (12 ms down the dcload link) are nobody's
		now = ProfNow();
		last = now;
		frameStart = now;
		lineStart = now;
	}
}

static long long loadStart, loadLast;   // 0: not loading
extern int fs_profOpens, fs_profProbes, fs_profReads, fs_profSeeks;  // files.c
static int lastOpens, lastProbes, lastReads, lastSeeks;

void Com_LoadStart( void ) {
	loadStart = loadLast = Sys_Microseconds();
	lastOpens = fs_profOpens;
	lastProbes = fs_profProbes;
	lastReads = fs_profReads;
	lastSeeks = fs_profSeeks;
}

void Com_LoadStep( const char *what ) {
	long long now;

	if ( !loadStart ) {
		return;
	}
	now = Sys_Microseconds();
	// and the files it opened, its reads and seeks (each a trip to the disc or dcload, unless
	// the read's in what stdio has buffered)
	Com_Printf( "LOAD %-24s %6d ms  (total %6d)  opens %d probes %d reads %d seeks %d\n", what,
				(int)( ( now - loadLast ) / 1000 ), (int)( ( now - loadStart ) / 1000 ),
				fs_profOpens - lastOpens, fs_profProbes - lastProbes, fs_profReads - lastReads, fs_profSeeks - lastSeeks );
	loadLast = now;
	lastOpens = fs_profOpens;
	lastProbes = fs_profProbes;
	lastReads = fs_profReads;
	lastSeeks = fs_profSeeks;
}

void Com_LoadDone( void ) {
	Com_LoadStep( "first frame" );
	loadStart = 0;
}

#endif
