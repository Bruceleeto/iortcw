/*
 * rtcwconv <in dir> <out dir>
 *
 * Converts every asset it knows under <in dir> (game data unpacked from the
 * pk3s) and writes it next to the same path under <out dir>, ready to be
 * zipped into a pk3. Run by `make assets`.
 *
 *   .mds   skeletal models -> .mdsc (mds.cpp), which the renderer looks
 *          for first
 */
#include <errno.h>
#include <filesystem>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rtcwconv.h"

namespace fs = std::filesystem;

static bool ReadFile( const fs::path &p, std::vector<uint8_t> &out ) {
	FILE *f = fopen( p.c_str(), "rb" );
	if ( !f ) {
		return false;
	}
	fseek( f, 0, SEEK_END );
	out.resize( ftell( f ) );
	fseek( f, 0, SEEK_SET );
	bool ok = fread( out.data(), 1, out.size(), f ) == out.size();
	fclose( f );
	return ok;
}

static bool WriteFile( const fs::path &p, const std::vector<uint8_t> &data ) {
	fs::create_directories( p.parent_path() );
	FILE *f = fopen( p.c_str(), "wb" );
	if ( !f ) {
		fprintf( stderr, "%s: %s\n", p.c_str(), strerror( errno ) );
		return false;
	}
	bool ok = fwrite( data.data(), 1, data.size(), f ) == data.size();
	fclose( f );
	return ok;
}

static void Usage( void ) {
	fprintf( stderr,
		"usage: rtcwconv [options] <in dir> <out dir>\n"
		"  -a <f>   bone turn allowed against its parent, in degrees (default 0.5)\n"
		"  -o <f>   frame bounds / offset error allowed, in units (default 0.1)\n"
		"  -s <n>   most frames between two keys (default 255)\n"
		"  -v       a line per file\n" );
	exit( 1 );
}

int main( int argc, char **argv ) {
	MdsOptions opt = { 0.5f, 0.1f, 255 };
	bool verbose = false;
	int i;

	for ( i = 1; i < argc && argv[i][0] == '-'; i++ ) {
		if ( !strcmp( argv[i], "-v" ) ) {
			verbose = true;
		} else if ( i + 1 < argc && !strcmp( argv[i], "-a" ) ) {
			opt.angleTol = atof( argv[++i] );
		} else if ( i + 1 < argc && !strcmp( argv[i], "-o" ) ) {
			opt.offsetTol = atof( argv[++i] );
		} else if ( i + 1 < argc && !strcmp( argv[i], "-s" ) ) {
			opt.maxSpan = atoi( argv[++i] );
		} else {
			Usage();
		}
	}
	if ( argc - i != 2 ) {
		Usage();
	}
	fs::path inDir = argv[i], outDir = argv[i + 1];

	MdsStats mds = {};
	int failed = 0;

	for ( const auto &e : fs::recursive_directory_iterator( inDir ) ) {
		if ( !e.is_regular_file() ) {
			continue;
		}
		fs::path rel = fs::relative( e.path(), inDir );
		std::string ext = e.path().extension().string();

		if ( !strcasecmp( ext.c_str(), ".mds" ) ) {
			std::vector<uint8_t> in, out;
			MdsStats before = mds;

			if ( !ReadFile( e.path(), in ) || !ConvertMds( in, out, opt, mds, rel.c_str() ) ||
				 !WriteFile( ( outDir / rel ).concat( "c" ), out ) ) {
				failed++;
				continue;
			}
			if ( verbose ) {
				printf( "%-48s %7.2f MB -> %6.2f MB  keys %5.1f%%\n", rel.c_str(),
						( mds.bytesIn - before.bytesIn ) / 1048576.0, ( mds.bytesOut - before.bytesOut ) / 1048576.0,
						100.0 * ( mds.keysOut - before.keysOut ) / ( mds.framesIn - before.framesIn ) );
			}
		}
	}

	if ( mds.files ) {
		printf( "mds: %d files, %.1f MB -> %.1f MB; bone poses kept %.1f%% (directions %.1f%%); bones off by %.3f units on average, %.2f / %.2f deg at most\n"
				"     %d of %d triangles in %d strips\n",
				mds.files, mds.bytesIn / 1048576.0, mds.bytesOut / 1048576.0,
				100.0 * mds.keysOut / mds.framesIn, 100.0 * mds.dirKeys / mds.framesIn, mds.sumErr / mds.numErr, mds.maxErr, mds.maxAngle,
				mds.stripTris, mds.tris, mds.strips );
	}
	if ( failed ) {
		fprintf( stderr, "%d file(s) failed\n", failed );
		return 1;
	}
	return 0;
}
