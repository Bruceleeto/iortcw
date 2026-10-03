/*
 * .rcd -> .rcd without the routes worked out ahead: the bot route cache
 * dump (AAS_WriteRouteCache in SP/code/botlib/be_aas_route.c) is
 *
 *   header         ident, version, numareas, numclusters, 3 crcs,
 *                  numportalcache, numareacache
 *   the caches     numportalcache + numareacache, each its size first
 *   visibility     numareas, each its size first
 *   waypoints      numareas vec3_ts
 *
 * The Dreamcast botlib keeps no worked out routes (they're made as they're
 * wanted, into a small cache), so the caches, most of the file (4.2 of
 * factory's 4.4MB), go and the header says there are none. The visibility
 * and waypoints are copied as they are.
 *
 * The file is walked to its end first: one that isn't laid out so, to the
 * byte, fails.
 */
#include <stdio.h>
#include <string.h>

#include "rtcwconv.h"

#define RCID            ( ( 'C' << 24 ) + ( 'R' << 16 ) + ( 'E' << 8 ) + 'M' )
#define RCVERSION       15

struct RcdHeader {
	int32_t ident, version, numareas, numclusters, areacrc, clustercrc, reachcrc;
	int32_t numportalcache, numareacache;
};

/* the int at p, if it's in the file */
static bool ReadInt( const std::vector<uint8_t> &in, size_t p, int32_t &v ) {
	if ( p + 4 > in.size() ) {
		return false;
	}
	memcpy( &v, &in[p], 4 );
	return true;
}

bool ConvertRcd( const std::vector<uint8_t> &in, std::vector<uint8_t> &out, RcdStats &st, const char *name ) {
	RcdHeader h;

	if ( in.size() < sizeof( h ) ) {
		fprintf( stderr, "%s: not a route cache dump\n", name );
		return false;
	}
	memcpy( &h, in.data(), sizeof( h ) );
	if ( h.ident != RCID || h.version != RCVERSION || h.numareas < 0 || h.numportalcache < 0 || h.numareacache < 0 ) {
		fprintf( stderr, "%s: not a version %d route cache dump\n", name, RCVERSION );
		return false;
	}

	/* the caches: each size counts itself */
	size_t p = sizeof( h );
	for ( long i = 0; i < (long)h.numportalcache + h.numareacache; i++ ) {
		int32_t size;
		if ( !ReadInt( in, p, size ) || size < 4 || p + size > in.size() ) {
			fprintf( stderr, "%s: cache %ld cut short\n", name, i );
			return false;
		}
		p += size;
	}
	size_t keep = p;

	/* the visibility: each size doesn't count itself; then the waypoints */
	for ( int i = 0; i < h.numareas; i++ ) {
		int32_t size;
		if ( !ReadInt( in, p, size ) || size < 0 || p + 4 + size > in.size() ) {
			fprintf( stderr, "%s: visibility of area %d cut short\n", name, i );
			return false;
		}
		p += 4 + size;
	}
	if ( p + (size_t)h.numareas * 12 != in.size() ) {
		fprintf( stderr, "%s: %zu bytes after the visibility, not %d areas' waypoints\n", name, in.size() - p,
				 h.numareas );
		return false;
	}

	RcdHeader oh = h;
	oh.numportalcache = oh.numareacache = 0;
	out.resize( sizeof( oh ) );
	memcpy( out.data(), &oh, sizeof( oh ) );
	out.insert( out.end(), in.begin() + keep, in.end() );

	st.files++;
	st.caches += h.numportalcache + h.numareacache;
	st.bytesIn += in.size();
	st.bytesOut += out.size();
	return true;
}
