/*
 * DCSIM: the Dreamcast's malloc (newlib's, dcsim_mallocr.inc) on an arena
 * the size of the RAM it has, from the end of the Dreamcast build's image
 * (DCSIM_HEAP_START, the .elf's _end) to the top of its 16MB less the
 * kernel stack, as KallistiOS gives it out (mm_sbrk). Less DCSIM_KOS_BYTES,
 * what KallistiOS mallocs itself before the game starts. See dcsim.h.
 *
 * Built with -fsanitize=address (make sp DCSIM=1 ASAN=1), every block is the
 * system's instead, which AddressSanitizer watches: a write to one after
 * it's freed is caught where it happens. The arena's limits are gone then.
 */
#undef malloc
#undef calloc
#undef realloc
#undef memalign
#undef free

#include <execinfo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DC_MEM_TOP          ( 0x8c000000u + 18 * 1024 * 1024 - 64 * 1024 )

static unsigned char *arena;        // where DCSIM_HEAP_START is
static uintptr_t sbrkBase;          // as a Dreamcast address

static void *dcsim_sbrk( ptrdiff_t increment ) {
	uintptr_t base = sbrkBase, newBase;

	increment = ( increment + 3 ) & ~3;
	newBase = base + increment;
	if ( newBase >= DC_MEM_TOP ) {
		printf( "Out of memory. Requested sbrk_base 0x%x, was 0x%x, diff %d\n",
				(unsigned)newBase, (unsigned)base, (int)increment );
		return (void *)-1;
	}
	sbrkBase = newBase;
	return arena + ( base - DCSIM_HEAP_START );
}

// the Dreamcast's malloc, all its names dl_
#define malloc              dl_malloc
#define calloc              dl_calloc
#define realloc             dl_realloc
#define memalign            dl_memalign
#define free                dl_free
#define cfree               dl_cfree
#define valloc              dl_valloc
#define pvalloc             dl_pvalloc
#define mallinfo            dl_mallinfo
#define mallopt             dl_mallopt
#define malloc_stats        dl_malloc_stats
#define malloc_trim         dl_malloc_trim
#define malloc_usable_size  dl_malloc_usable_size
#define HAVE_MMAP           0
#define MORECORE            dcsim_sbrk
#define MORECORE_CLEARS     0
#define malloc_getpagesize  ( 4096 )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "dcsim_mallocr.inc"
#pragma GCC diagnostic pop
#undef malloc
#undef calloc
#undef realloc
#undef memalign
#undef free

static int inArena( void *p ) {
	return arena && (unsigned char *)p >= arena && (unsigned char *)p < arena + ( DC_MEM_TOP - DCSIM_HEAP_START );
}

static void init( void ) {
	static unsigned char *block;
	size_t size = DC_MEM_TOP - DCSIM_HEAP_START;

	if ( arena ) {
		return;
	}
	// the same place in a page as on the Dreamcast, so it lines up alike
	// (a size aligned_alloc takes: a multiple of the alignment)
	block = aligned_alloc( 4096, ( size + 8192 + 4095 ) & ~(size_t)4095 );
	if ( !block ) {
		fprintf( stderr, "DCSIM: no %u bytes for the arena\n", (unsigned)size );
		exit( 1 );
	}
	arena = block + ( DCSIM_HEAP_START & 4095 );
	sbrkBase = DCSIM_HEAP_START;
	// what KallistiOS has taken before the game starts
	dl_malloc( DCSIM_KOS_BYTES );
	printf( "DCSIM: Dreamcast heap %u K (image ends 0x%x), %u K of it KallistiOS's\n",
			(unsigned)( size / 1024 ), (unsigned)DCSIM_HEAP_START, (unsigned)( DCSIM_KOS_BYTES / 1024 ) );
}

/*
 * With DCSIM_ALLOCS set (a file name start), every block in the arena is
 * kept with who asked for it, and DCSim_DumpAllocs writes them out: a line a
 * block, its size and the calls that made it, for addr2line.
 */
#define TRACK_FRAMES    10
#define TRACK_SLOTS     ( 1 << 20 )

typedef struct {
	void *p;            // NULL: empty; (void *)1: was one
	unsigned size;
	void *frames[TRACK_FRAMES];
} track_t;

static track_t *tracked;
static int tracking = -1;

static unsigned slotFor( void *p ) {
	return (unsigned)( ( (uintptr_t)p >> 3 ) * 2654435761u ) & ( TRACK_SLOTS - 1 );
}

static void track( void *p, size_t size ) {
	void *frames[TRACK_FRAMES + 2];
	unsigned i;
	int n;

	if ( tracking < 0 ) {
		tracking = getenv( "DCSIM_ALLOCS" ) != NULL;
		if ( tracking ) {
			tracked = calloc( TRACK_SLOTS, sizeof( *tracked ) );
		}
	}
	if ( !tracking || !p ) {
		return;
	}
	n = backtrace( frames, TRACK_FRAMES + 2 );
	if ( size >= 32 * 1024 && n > 3 ) {
		// a big one: where it went, to tell what a hole was (DCSim_DumpHoles)
		fprintf( stderr, "DCSIM: + %u K at %lu K: %p %p\n", (unsigned)( size / 1024 ),
				(unsigned long)( (char *)p - (char *)arena ) / 1024, frames[2], frames[3] );
	}
	for ( i = slotFor( p ); tracked[i].p && tracked[i].p != (void *)1; i = ( i + 1 ) & ( TRACK_SLOTS - 1 ) ) {
	}
	tracked[i].p = p;
	tracked[i].size = size;
	memset( tracked[i].frames, 0, sizeof( tracked[i].frames ) );
	// past track and the dcsim_ call
	if ( n > 2 ) {
		memcpy( tracked[i].frames, frames + 2, ( n - 2 ) * sizeof( void * ) );
	}
}

static void untrack( void *p ) {
	unsigned i;

	if ( tracking <= 0 || !p ) {
		return;
	}
	for ( i = slotFor( p ); tracked[i].p; i = ( i + 1 ) & ( TRACK_SLOTS - 1 ) ) {
		if ( tracked[i].p == p ) {
			if ( tracked[i].size >= 32 * 1024 ) {
				fprintf( stderr, "DCSIM: - %u K at %lu K\n", tracked[i].size / 1024,
						(unsigned long)( (char *)p - (char *)arena ) / 1024 );
			}
			tracked[i].p = (void *)1;
			return;
		}
	}
}

void DCSim_DumpAllocs( const char *when ) {
	static int count;
	char name[1024];
	FILE *f;
	unsigned i;
	int j;

	if ( tracking <= 0 ) {
		return;
	}
	snprintf( name, sizeof( name ), "%s%d.txt", getenv( "DCSIM_ALLOCS" ), ++count );
	if ( !( f = fopen( name, "w" ) ) ) {
		return;
	}
	fprintf( f, "# %s\n", when );
	for ( i = 0; i < TRACK_SLOTS; i++ ) {
		if ( tracked[i].p && tracked[i].p != (void *)1 ) {
			fprintf( f, "%u", tracked[i].size );
			for ( j = 0; j < TRACK_FRAMES && tracked[i].frames[j]; j++ ) {
				fprintf( f, " %p", tracked[i].frames[j] );
			}
			fprintf( f, "\n" );
		}
	}
	fclose( f );
	printf( "DCSIM: blocks in use written to %s\n", name );
}

void *dcsim_malloc( size_t size ) {
	void *p;

#ifdef __SANITIZE_ADDRESS__
	return malloc( size );
#endif
	init();
	p = dl_malloc( size );
	track( p, size );
	return p;
}

void *dcsim_calloc( size_t n, size_t size ) {
	void *p;

#ifdef __SANITIZE_ADDRESS__
	return calloc( n, size );
#endif
	init();
	p = dl_calloc( n, size );
	track( p, n * size );
	return p;
}

void *dcsim_memalign( size_t align, size_t size ) {
	void *p;

#ifdef __SANITIZE_ADDRESS__
	if ( posix_memalign( &p, align < sizeof( void * ) ? sizeof( void * ) : align, size ) ) {
		return NULL;
	}
	return p;
#endif
	init();
	p = dl_memalign( align, size );
	track( p, size );
	return p;
}

// a block from the system's malloc (strdup and the like) goes back to it
void dcsim_free( void *p ) {
	if ( !p ) {
		return;
	}
	if ( inArena( p ) ) {
		untrack( p );
		dl_free( p );
	} else {
		free( p );
	}
}

void *dcsim_realloc( void *p, size_t size ) {
	init();
	if ( p && !inArena( p ) ) {
		return realloc( p, size );
	}
	untrack( p );
	p = dl_realloc( p, size );
	track( p, size );
	return p;
}

// the tracked block at q (a chunk), or NULL
static track_t *trackedChunk( mchunkptr q ) {
	void *p = chunk2mem( q );
	unsigned i;

	for ( i = slotFor( p ); tracked[i].p; i = ( i + 1 ) & ( TRACK_SLOTS - 1 ) ) {
		if ( tracked[i].p == p ) {
			return &tracked[i];
		}
	}
	return NULL;
}

static void printNeighbour( const char *side, mchunkptr q ) {
	track_t *t = trackedChunk( q );
	int j;

	fprintf( stderr, "DCSIM:   %s %lu bytes:", side, (unsigned long)chunksize( q ) );
	for ( j = 0; t && j < TRACK_FRAMES && t->frames[j]; j++ ) {
		fprintf( stderr, " %p", t->frames[j] );
	}
	fprintf( stderr, t ? "\n" : " (not tracked)\n" );
}

void DCSim_DumpHoles( const char *when ) {
	unsigned long misalign;
	mchunkptr p, prev = NULL;

	if ( tracking <= 0 || sbrk_base == (char *)-1 ) {
		return;
	}
	fprintf( stderr, "DCSIM: holes of 32K or more, %s:\n", when );
	misalign = (unsigned long)chunk2mem( sbrk_base ) & MALLOC_ALIGN_MASK;
	p = (mchunkptr)( sbrk_base + ( misalign ? MALLOC_ALIGNMENT - misalign : 0 ) );
	for ( ; p < top && chunksize( p ); prev = p, p = next_chunk( p ) ) {
		if ( inuse( p ) || chunksize( p ) < 32 * 1024 ) {
			continue;
		}
		fprintf( stderr, "DCSIM: hole %lu K at %lu K\n", (unsigned long)chunksize( p ) / 1024,
				(unsigned long)( (char *)p - (char *)arena ) / 1024 );
		if ( prev ) {
			printNeighbour( "before:", prev );
		}
		if ( next_chunk( p ) < top ) {
			printNeighbour( "after: ", next_chunk( p ) );
		}
	}
	fprintf( stderr, "DCSIM: top %lu K at %lu K\n", (unsigned long)chunksize( top ) / 1024,
			(unsigned long)( (char *)top - (char *)arena ) / 1024 );
}

int DCSim_LargestFree( void ) {
	// the top chunk and the RAM above it, or the biggest free chunk below
	unsigned long largest = chunksize( top ) + ( DC_MEM_TOP - sbrkBase );
	mbinptr b;
	mchunkptr p;
	int i;

	init();
	for ( i = 1; i < NAV; i++ ) {
		b = bin_at( i );
		for ( p = last( b ); p != b; p = p->bk ) {
			if ( chunksize( p ) > largest ) {
				largest = chunksize( p );
			}
		}
	}
	return largest;
}

void DCSim_Info( int *inUse, int *freeBytes ) {
	struct dl_mallinfo mi;

	init();
	mi = dl_mallinfo();
	*inUse = mi.uordblks;
	*freeBytes = ( DC_MEM_TOP - sbrkBase ) + mi.fordblks;
}
