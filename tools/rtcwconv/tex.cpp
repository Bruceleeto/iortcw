/*
 * Images (.tga / .jpg) -> pvrtex .dt, which the PVR renderer uploads as it
 * is instead of the image: VQ compressed (about 1/8 of 16 bit texels),
 * mipmapped where the PVR can, at most opt.maxSize on a side (2D art
 * opt.maxSize2D).
 *
 * pvrtex (KOS utils/pvrtex) does the work; this picks what each image gets
 * and keeps all cores busy with it.
 */
#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <thread>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include "stb/stb_image_resize.h"

#include "rtcwconv.h"

namespace fs = std::filesystem;

static bool HasPrefix( const std::string &s, const char *prefix ) {
	return !strncasecmp( s.c_str(), prefix, strlen( prefix ) );
}

/* 2D art: drawn at about its own size, never minified much */
static bool Is2D( const std::string &rel ) {
	return HasPrefix( rel, "gfx/" ) || HasPrefix( rel, "ui/" ) || HasPrefix( rel, "ui_mp/" ) ||
		   HasPrefix( rel, "menu/" ) || HasPrefix( rel, "levelshots/" ) || HasPrefix( rel, "fonts/" );
}


static bool ConvertOne( const TexJob &job, const TexOptions &opt, TexStats &st, std::mutex &lock ) {
	int w = job.w, h = job.h, n;
	unsigned char *pic;

	if ( job.rgba.empty() ) {
		pic = stbi_load( job.in.c_str(), &w, &h, &n, 4 );
	} else {
		pic = (unsigned char *)malloc( job.rgba.size() );
		memcpy( pic, job.rgba.data(), job.rgba.size() );
	}

	if ( !pic ) {
		std::lock_guard<std::mutex> g( lock );
		fprintf( stderr, "%s: %s\n", job.rel.c_str(), stbi_failure_reason() );
		return false;
	}

	bool flat = job.noMip || Is2D( job.rel );
	int maxSize = Is2D( job.rel ) ? opt.maxSize2D : opt.maxSize;
	int nw = w, nh = h;
	while ( nw > maxSize || nh > maxSize ) {
		nw = nw > 1 ? nw / 2 : 1;
		nh = nh > 1 ? nh / 2 : 1;
	}
	if ( nw != w || nh != h ) {
		unsigned char *small = (unsigned char *)malloc( (size_t)nw * nh * 4 );
		stbir_resize_uint8( pic, w, h, 0, small, nw, nh, 0, 4 );
		free( pic );
		pic = small;
	}

	fs::create_directories( job.out.parent_path() );
	fs::path tmp = job.out;
	tmp.replace_extension( ".tmp.tga" );
	bool ok = stbi_write_tga( tmp.c_str(), nw, nh, 4, pic );
	free( pic );	/* stb_image allocates with malloc */
	if ( !ok ) {
		std::lock_guard<std::mutex> g( lock );
		fprintf( stderr, "%s: can't write %s\n", job.rel.c_str(), tmp.c_str() );
		return false;
	}

	std::string cmd = "'" + opt.pvrtex + "' -i '" + tmp.string() + "' -o '" + job.out.string() +
					  "' -f AUTO -r NEAR -c small";
	if ( !flat ) {
		cmd += " -m -R OPT";	/* square ones only: the PVR's mipmaps are */
	}
	cmd += " >/dev/null 2>&1";
	int rc = system( cmd.c_str() );
	fs::remove( tmp );

	std::error_code ec;
	auto size = fs::file_size( job.out, ec );
	if ( rc != 0 || ec ) {
		std::lock_guard<std::mutex> g( lock );
		fprintf( stderr, "%s: pvrtex failed (%d)\n", job.rel.c_str(), WIFEXITED( rc ) ? WEXITSTATUS( rc ) : rc );
		return false;
	}

	std::lock_guard<std::mutex> g( lock );
	st.files++;
	st.bytesIn += (size_t)nw * nh * 2;
	st.bytesOut += size;
	if ( opt.verbose ) {
		printf( "%-56s %4dx%-4d -> %7zu bytes%s\n", job.rel.c_str(), w, h, (size_t)size,
				flat ? " (2D)" : "" );
	}
	return true;
}

int ConvertTextures( const std::vector<TexJob> &jobs, const TexOptions &opt, TexStats &st ) {
	std::atomic<size_t> next( 0 );
	std::atomic<int> failed( 0 );
	std::mutex lock;
	std::vector<std::thread> threads;
	int n = opt.jobs > 0 ? opt.jobs : (int)std::thread::hardware_concurrency();

	for ( int i = 0; i < ( n > 0 ? n : 1 ); i++ ) {
		threads.emplace_back( [&]() {
			for ( size_t j; ( j = next++ ) < jobs.size(); ) {
				if ( !ConvertOne( jobs[j], opt, st, lock ) ) {
					failed++;
				}
			}
		} );
	}
	for ( auto &t : threads ) {
		t.join();
	}
	return failed;
}
