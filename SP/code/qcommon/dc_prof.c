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
 * and on the Dreamcast, the SH4 performance counter event com_perfEvent
 * (a PMCR mode, perfctr.h, in decimal: 37 cycles stalled on the data cache,
 * 36 on the instruction cache, 41 on the FPU, 19 instructions; 0: none)
 * counted in each section, thousands a frame; each line the next of
 * dcache stalls (37), instructions (19), icache stalls (36), fpu stalls (41):
 *
 *   PERF 0x25: engine 12 game 40 ... pvr 2100 ...
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
	"scene", "draw", "world", "models", "shade", "deform", "colors", "lighting", "texcoords", "dlights", "fog",
	"sky", "flares", "pvr", "submit", "gpu", "sound", "idle"
};

static const char *const statNames[STAT_NUM] = {
	"draws", "verts", "tris", "culled", "clipped", "emitted", "framehits", "framemisses", "bonecalls", "bonemisses", "tr", "vbufmax"
};

int profStats[STAT_NUM];

#define PROF_DEPTH  32

static long long sectionTime[PROF_NUM];  // microseconds since the last line
static profSection_t stack[PROF_DEPTH];
static int depth;
static long long last;                  // when the time was last given out

static cvar_t *com_profile;

#ifdef _arch_dreamcast
static cvar_t *com_perfEvent;
static int perfEvent;                   // the one counting, 0: none
static uint64_t sectionEvents[PROF_NUM];
static uint64_t lastEvents;
#endif

// what's gone since last to the section now running
static void Charge( long long now ) {
	profSection_t running = depth ? stack[depth - 1] : PROF_ENGINE;

	if ( last ) {
		sectionTime[running] += now - last;
	}
	last = now;
#ifdef _arch_dreamcast
	if ( perfEvent ) {
		uint64_t events = perf_cntr_count( PRFC1 );

		sectionEvents[running] += events - lastEvents;
		lastEvents = events;
	}
#endif
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

		Q_strncpyz( line, "RSTAT", sizeof( line ) );
		for ( i = 0; i < STAT_NUM; i++ ) {
			Q_strcat( line, sizeof( line ), va( " %s %d", statNames[i], i == STAT_VBUFMAX ? profStats[i] : profStats[i] / frames ) );
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
		memset( profStats, 0, sizeof( profStats ) );
		lineStart = now;
		frames = 0;
		worst = 0;
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
