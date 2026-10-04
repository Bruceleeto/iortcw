/*
 * Images (.tga / .jpg) -> pvrtex .dt, which the PVR renderer uploads as it
 * is instead of the image: VQ compressed (about 1/8 of 16 bit texels),
 * mipmapped where the PVR can, at most opt.maxSize on a side (2D art
 * opt.maxSize2D).
 *
 * pvrtex (KOS utils/pvrtex) does the work; this picks what each image gets
 * and keeps all cores busy with it.
 */
#include <algorithm>
#include <atomic>
#include <cmath>
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


/* pvrtex writes a pal8 image's palette to <out>.pal ("DPAL", count, ARGB8888
   entries): the entries go on the end of the .dt, where the renderer looks */
static bool AppendPalette( const fs::path &out ) {
	fs::path pal = out.string() + ".pal";
	FILE *f = fopen( pal.c_str(), "rb" );
	if ( !f ) {
		return false;
	}
	char fourcc[4];
	uint32_t count = 0, entries[256];
	bool ok = fread( fourcc, 4, 1, f ) == 1 && !memcmp( fourcc, "DPAL", 4 ) && fread( &count, 4, 1, f ) == 1 &&
			  count >= 1 && count <= 256 && fread( entries, 4, count, f ) == count;
	fclose( f );
	fs::remove( pal );
	if ( !ok || !( f = fopen( out.c_str(), "ab" ) ) ) {
		return false;
	}
	ok = fwrite( entries, 4, count, f ) == count;
	return fclose( f ) == 0 && ok;
}

/* the pvrtex -f for an image left to pvrtex: AUTO (RGB565) for an opaque one,
   else ARGB4444 or ARGB1555, whichever is nearer the image drawn blended over
   black and white, in lightness (cube root: darks count as much as the eye
   sees them; 4444's 16 levels lose dark colours, 1555's alpha is on/off) */
static const char *PixelFormat( const unsigned char *pic, int w, int h ) {
	bool opaque = true;
	double err4444 = 0, err1555 = 0;

	for ( const unsigned char *p = pic; p < pic + (size_t)w * h * 4; p += 4 ) {
		int a = p[3], a4 = ( a * 15 + 127 ) / 255 * 17, a1 = a >= 128 ? 255 : 0;
		opaque &= a == 255;
		for ( int c = 0; c < 3; c++ ) {
			int q5 = ( p[c] * 31 + 127 ) / 255;
			int v4 = ( p[c] * 15 + 127 ) / 255 * 17, v1 = ( q5 << 3 ) | ( q5 >> 2 );
			for ( int bg = 0; bg <= 255; bg += 255 ) {
				double o = cbrt( p[c] * a + bg * ( 255 - a ) );
				double e4 = cbrt( v4 * a4 + bg * ( 255 - a4 ) ) - o, e1 = cbrt( v1 * a1 + bg * ( 255 - a1 ) ) - o;
				err4444 += e4 * e4;
				err1555 += e1 * e1;
			}
		}
	}
	return opaque ? "AUTO" : err1555 <= err4444 ? "ARGB1555" : "ARGB4444";
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
	std::string name = fs::path( job.rel ).replace_extension().string();
	std::transform( name.begin(), name.end(), name.begin(), ::tolower );
	auto size = opt.sizes.find( name );
	if ( size != opt.sizes.end() ) {
		nw = size->second.first;
		nh = size->second.second;
	}
	if ( nw != w || nh != h ) {
		unsigned char *small = (unsigned char *)malloc( (size_t)nw * nh * 4 );
		stbir_resize_uint8( pic, w, h, 0, small, nw, nh, 0, 4 );
		free( pic );
		pic = small;
	}

	/* texsizes.txt's format: [vq]565 / 1555 / 4444 / yuv, raw, pal8; VQ (the default)
	   unless it says raw or a pixel format without vq */
	auto format = opt.formats.find( name );
	std::string fmt = format != opt.formats.end() ? format->second : "";
	bool vq = fmt.empty() || !fmt.compare( 0, 2, "vq" );
	std::string pixels = vq ? fmt.substr( std::min<size_t>( 2, fmt.size() ) ) : fmt;
	std::string pvrFormat = pixels == "565" ? "RGB565" : pixels == "1555" ? "ARGB1555" :
							pixels == "4444" ? "ARGB4444" : pixels == "yuv" ? "YUV422" :
							pixels == "pal8" ? "PAL8BPP" : PixelFormat( pic, nw, nh );

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
					  "' -r NEAR -f " + pvrFormat;
	if ( vq ) {
		cmd += " -c small";
	}
	if ( !flat ) {
		cmd += " -m -R OPT";	/* square ones only: the PVR's mipmaps are */
	}
	cmd += " >/dev/null 2>&1";
	int rc = system( cmd.c_str() );
	fs::remove( tmp );
	if ( rc == 0 && fmt == "pal8" && !AppendPalette( job.out ) ) {
		rc = -1;
	}

	std::error_code ec;
	auto bytes = fs::file_size( job.out, ec );
	if ( rc != 0 || ec ) {
		std::lock_guard<std::mutex> g( lock );
		fprintf( stderr, "%s: pvrtex failed (%d)\n", job.rel.c_str(), WIFEXITED( rc ) ? WEXITSTATUS( rc ) : rc );
		return false;
	}

	std::lock_guard<std::mutex> g( lock );
	st.files++;
	st.bytesIn += (size_t)nw * nh * 2;
	st.bytesOut += bytes;
	if ( opt.verbose ) {
		printf( "%-56s %4dx%-4d -> %7zu bytes%s\n", job.rel.c_str(), w, h, (size_t)bytes,
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
