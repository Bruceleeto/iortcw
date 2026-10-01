/*
 * DCSIM=1 (Makefile): the PC build with the Dreamcast's heap. Put in front of
 * every source file (-include), so all the game's mallocs go to
 * sys/dcsim_malloc.c: the Dreamcast's own malloc on just the RAM the
 * Dreamcast has free. A map that runs out here runs out there.
 */
// C only: the C++ (splines/, the camera paths) is left on the system's
#if !defined( DCSIM_H ) && !defined( __ASSEMBLER__ ) && !defined( __cplusplus )
#define DCSIM_H

#include <stddef.h>

void *dcsim_malloc( size_t size );
void *dcsim_calloc( size_t n, size_t size );
void *dcsim_realloc( void *p, size_t size );
void *dcsim_memalign( size_t align, size_t size );
void dcsim_free( void *p );
// what the Dreamcast reports: malloc in use, and free (in the heap and above it)
void DCSim_Info( int *inUse, int *freeBytes );
// with DCSIM_ALLOCS set: the blocks in use, and who made them, to a file
void DCSim_DumpAllocs( const char *when );

#define malloc      dcsim_malloc
#define calloc      dcsim_calloc
#define realloc     dcsim_realloc
#define memalign    dcsim_memalign
#define free        dcsim_free

#endif
