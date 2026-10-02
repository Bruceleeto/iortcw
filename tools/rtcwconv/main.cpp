/*
 * rtcwconv <in dir> <out dir>
 *
 * Converts every asset it knows under <in dir> (game data unpacked from the
 * pk3s) and writes it next to the same path under <out dir>, ready to be
 * zipped into a pk3. Run by `make assets`.
 *
 *   .mds   skeletal models -> .mdsc (mds.cpp), which the renderer looks
 *          for first
 *   .tga / .jpg   images -> .dt (tex.cpp, with -p), which the PVR renderer
 *          looks for first; a .tga wins over a .jpg of the same name, as
 *          in the game
 *   .bsp   its lightmaps -> maps/<map>/lm_NNNN.dt (bsp.cpp, with -p);
 *          its collision -> maps/<map>.col (col.cpp), which the collision
 *          code loads in place of the .bsp;
 *          its surfaces -> maps/<map>.wld (wld.cpp), which the renderer
 *          loads in place of the .bsp
 *   .mdc   vertex animated models -> .mdb with rigid bones in place of
 *          the vertexes a frame (mdc.cpp), which the renderer looks for
 *          first; none when the .mdc is better kept
 *   .aas   bot navigation -> .aasc without the faces the game doesn't read
 *          (aas.cpp), which the botlib looks for first
 *          and an empty maps/<map>_b1.aasc for a map with no big characters
 *          (the only ones that use the second world), which the botlib
 *          then doesn't load
 *   .shader  the shaders anything names -> scripts/dc.shaders
 *          (shaders.cpp), which the renderer loads in place of them all;
 *          the names are looked for in every file and in -n's
 */
#include <algorithm>
#include <map>
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

/*
Map edits, a line each, # for a comment:
	<map> drop <shader prefix>    the map's surfaces with such a shader left out
*/
struct MapEdits {
	std::vector<std::string> drop;
};

static bool ReadEdits( const char *file, std::map<std::string, MapEdits> &edits ) {
	FILE *f = fopen( file, "r" );
	char line[512];
	int n = 0;

	if ( !f ) {
		fprintf( stderr, "%s: %s\n", file, strerror( errno ) );
		return false;
	}
	while ( fgets( line, sizeof( line ), f ) ) {
		char map[128], what[32], arg[256];
		n++;
		char *hash = strchr( line, '#' );
		if ( hash ) {
			*hash = 0;
		}
		int got = sscanf( line, "%127s %31s %255s", map, what, arg );
		if ( got <= 0 ) {
			continue;
		}
		if ( got == 3 && !strcmp( what, "drop" ) ) {
			edits[map].drop.push_back( arg );
		} else {
			fprintf( stderr, "%s:%d: not an edit\n", file, n );
			fclose( f );
			return false;
		}
	}
	fclose( f );
	return true;
}

static void Usage( void ) {
	fprintf( stderr,
		"usage: rtcwconv [options] <in dir> <out dir>\n"
		"  -a <f>   bone turn allowed against its parent, in degrees (default 1)\n"
		"  -o <f>   frame bounds / offset error allowed, in units (default 1)\n"
		"  -s <n>   most frames between two keys (default 255)\n"
		"  -p <f>   pvrtex binary: convert images to .dt too\n"
		"  -t <n>   largest texture side (default 128; 2D art 256)\n"
		"  -j <n>   pvrtex runs at once (default: one per core)\n"
		"  -c <f>   curve subdivisions, as r_subdivisions (default 12)\n"
		"  -n <d>   more files that name shaders (the game's source), for dc.shaders\n"
		"  -e <f>   map edits (see ReadEdits)\n"
		"  -v       a line per file\n" );
	exit( 1 );
}

int main( int argc, char **argv ) {
	MdsOptions opt = { 1.0f, 1.0f, 255 };
	TexOptions texOpt = { "", 128, 256, 0, false };
	bool verbose = false;
	float subdivisions = 12;
	std::vector<std::string> nameDirs;
	std::map<std::string, MapEdits> edits;
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
		} else if ( i + 1 < argc && !strcmp( argv[i], "-p" ) ) {
			texOpt.pvrtex = argv[++i];
		} else if ( i + 1 < argc && !strcmp( argv[i], "-t" ) ) {
			texOpt.maxSize = atoi( argv[++i] );
		} else if ( i + 1 < argc && !strcmp( argv[i], "-j" ) ) {
			texOpt.jobs = atoi( argv[++i] );
		} else if ( i + 1 < argc && !strcmp( argv[i], "-c" ) ) {
			subdivisions = atof( argv[++i] );
		} else if ( i + 1 < argc && !strcmp( argv[i], "-n" ) ) {
			nameDirs.push_back( argv[++i] );
		} else if ( i + 1 < argc && !strcmp( argv[i], "-e" ) ) {
			if ( !ReadEdits( argv[++i], edits ) ) {
				return 1;
			}
		} else {
			Usage();
		}
	}
	if ( argc - i != 2 ) {
		Usage();
	}
	fs::path inDir = argv[i], outDir = argv[i + 1];
	texOpt.verbose = verbose;

	MdsStats mds = {};
	TexStats tex = {};
	AasStats aas = {};
	ColStats col = {};
	WldStats wld = {};
	MdcStats mdc = {};
	ShaderStats shaders = {};
	std::vector<fs::path> smallMaps;	/* .bsps with no big characters, without extension */
	std::map<std::string, TexJob> images;	/* by name without extension */
	std::vector<TexJob> lightmaps;
	int failed = 0;

	for ( const auto &e : fs::recursive_directory_iterator( inDir ) ) {
		if ( !e.is_regular_file() ) {
			continue;
		}
		fs::path rel = fs::relative( e.path(), inDir );
		std::string ext = e.path().extension().string();

		/* what may name a shader: all but images and sounds (a .bsp's
		 * entities and shader names, below) */
		if ( strcasecmp( ext.c_str(), ".tga" ) && strcasecmp( ext.c_str(), ".jpg" ) && strcasecmp( ext.c_str(), ".wav" ) &&
			 strcasecmp( ext.c_str(), ".bsp" ) && strcasecmp( ext.c_str(), ".aas" ) ) {
			std::vector<uint8_t> in;
			if ( !ReadFile( e.path(), in ) ) {
				failed++;
				continue;
			}
			if ( !strcasecmp( ext.c_str(), ".shader" ) ) {
				if ( !ShaderFile( e.path().filename().string(), in, shaders ) ) {
					failed++;
				}
				continue;
			}
			ShaderNames( in.data(), in.size() );
		}

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
		} else if ( !strcasecmp( ext.c_str(), ".mdc" ) ) {
			std::vector<uint8_t> in, out;
			fs::path mdbPath = outDir / rel;
			mdbPath.replace_extension( ".mdb" );
			if ( !ReadFile( e.path(), in ) || !ConvertMdc( in, out, mdc, rel.c_str() ) ||
				 ( !out.empty() && !WriteFile( mdbPath, out ) ) ) {
				failed++;
			} else if ( verbose && !out.empty() ) {
				printf( "%-48s %6.1f K -> %6.1f K\n", rel.c_str(), in.size() / 1024.0, out.size() / 1024.0 );
			}
		} else if ( !strcasecmp( ext.c_str(), ".aas" ) ) {
			std::vector<uint8_t> in, out;
			if ( !ReadFile( e.path(), in ) || !ConvertAas( in, out, aas, rel.c_str() ) ||
				 !WriteFile( ( outDir / rel ).concat( "c" ), out ) ) {
				failed++;
			}
		} else if ( !texOpt.pvrtex.empty() &&
					( !strcasecmp( ext.c_str(), ".tga" ) || !strcasecmp( ext.c_str(), ".jpg" ) ) ) {
			fs::path stem = rel;
			stem.replace_extension();
			auto it = images.find( stem.string() );
			if ( it == images.end() || !strcasecmp( ext.c_str(), ".tga" ) ) {
				TexJob job;
				job.rel = rel.string();
				job.in = e.path().string();
				job.noMip = false;
				job.out = ( outDir / stem ).concat( ".dt" );
				images[stem.string()] = job;
			}
		} else if ( !strcasecmp( ext.c_str(), ".bsp" ) ) {
			std::vector<uint8_t> in, out, wldOut;
			fs::path colPath = outDir / rel, wldPath = outDir / rel;
			colPath.replace_extension( ".col" );
			wldPath.replace_extension( ".wld" );
			if ( ReadFile( e.path(), in ) ) {
				/* the big characters, which use the second aas world
				 * (BBOX_LARGE in ai_cast_characters.c) */
				static const char *big[] = { "\"ai_loper\"", "\"ai_stimsoldier_dual\"", "\"ai_stimsoldier_rocket\"",
					"\"ai_stimsoldier_tesla\"", "\"ai_supersoldier\"", "\"ai_protosoldier\"", "\"ai_boss_helga\"",
					"\"ai_boss_heinrich\"" };
				uint32_t entOfs, entLen;
				memcpy( &entOfs, &in[8], 4 );
				memcpy( &entLen, &in[12], 4 );
				if ( in.size() >= 16 && entOfs <= in.size() && entLen <= in.size() - entOfs ) {
					std::string ents( in.begin() + entOfs, in.begin() + entOfs + entLen );
					std::transform( ents.begin(), ents.end(), ents.begin(), ::tolower );
					bool any = false;
					for ( const char *b : big ) {
						any |= ents.find( b ) != std::string::npos;
					}
					if ( !any ) {
						fs::path p = rel;
						smallMaps.push_back( p.replace_extension() );
					}
				}
				/* entities, shaders and fogs name shaders */
				for ( int lump : { 0, 1, 12 } ) {
					uint32_t ofs, len;
					if ( in.size() >= 8 + ( lump + 1 ) * 8u ) {
						memcpy( &ofs, &in[8 + lump * 8], 4 );
						memcpy( &len, &in[12 + lump * 8], 4 );
						if ( ofs <= in.size() && len <= in.size() - ofs ) {
							ShaderNames( &in[ofs], len );
						}
					}
				}
			}
			if ( in.empty() || !ConvertCol( in, out, col, rel.c_str() ) || !WriteFile( colPath, out ) ||
				 !ConvertWld( in, wldOut, wld, rel.c_str(), subdivisions, edits[e.path().stem().string()].drop ) || !WriteFile( wldPath, wldOut ) ) {
				failed++;
			} else if ( !texOpt.pvrtex.empty() && !BspLightmapJobs( in, e.path().stem().string(), outDir, lightmaps ) ) {
				failed++;
			}
		}
	}

	/* after the .aas, whatever order they came in */
	for ( const auto &m : smallMaps ) {
		fs::path b1 = outDir / m;
		b1 += "_b1.aasc";
		if ( fs::exists( b1 ) ) {
			if ( !WriteFile( b1, {} ) ) {
				failed++;
			}
			aas.skipped++;
		}
	}

	for ( const auto &d : nameDirs ) {
		for ( const auto &e : fs::recursive_directory_iterator( d ) ) {
			std::vector<uint8_t> in;
			if ( e.is_regular_file() && ReadFile( e.path(), in ) ) {
				ShaderNames( in.data(), in.size() );
			}
		}
	}
	{
		std::vector<uint8_t> out;
		WriteShaders( out, shaders );
		if ( !out.empty() && !WriteFile( outDir / "scripts" / "dc.shaders", out ) ) {
			failed++;
		}
	}

	for ( auto &job : lightmaps ) {
		images[job.rel] = std::move( job );
	}

	if ( !images.empty() ) {
		std::vector<TexJob> jobs;
		for ( auto &kv : images ) {
			jobs.push_back( kv.second );
		}
		failed += ConvertTextures( jobs, texOpt, tex );
	}

	if ( mds.files ) {
		printf( "mds: %d files, %.1f MB -> %.1f MB; bone poses kept %.1f%% (directions %.1f%%); bones off by %.3f units on average, %.2f / %.2f deg at most\n"
				"     %d of %d triangles in %d strips\n",
				mds.files, mds.bytesIn / 1048576.0, mds.bytesOut / 1048576.0,
				100.0 * mds.keysOut / mds.framesIn, 100.0 * mds.dirKeys / mds.framesIn, mds.sumErr / mds.numErr, mds.maxErr, mds.maxAngle,
				mds.stripTris, mds.tris, mds.strips );
	}
	if ( mdc.files ) {
		printf( "mdc: %d files, %.1f MB; %d animated ones to .mdb, %.1f MB -> %.1f MB, %ld of %ld surfaces as %ld bones, %.1f%% of frames kept,\n"
				"     vertexes off by %.3f units on average, %.2f at most; %d kept\n",
				mdc.files, mdc.bytesIn / 1048576.0, mdc.converted, mdc.bytesConverted / 1048576.0, mdc.bytesOut / 1048576.0,
				mdc.boneSurfaces, mdc.surfaces, mdc.bones, mdc.frames ? 100.0 * mdc.keys / mdc.frames : 0.0,
				mdc.numErr ? mdc.sumErr / mdc.numErr : 0.0, mdc.maxErr, mdc.kept );
	}
	if ( shaders.files ) {
		printf( "shaders: %d files, %.0f K -> %.0f K; %d of %d shaders named somewhere\n",
				shaders.files, shaders.bytesIn / 1024.0, shaders.bytesOut / 1024.0, shaders.kept, shaders.shaders );
	}
	if ( col.files ) {
		printf( "col: %d files, %.1f MB of bsp -> %.1f MB; %d patches (%d with no contents left out)\n",
				col.files, col.bytesIn / 1048576.0, col.bytesOut / 1048576.0, col.patches, col.patchesSkipped );
	}
	if ( wld.files ) {
		printf( "wld: %d files, %.1f MB of bsp -> %.1f MB; %ld surfaces -> %ld; %ld vertexes, %ld triangles; light grids %.1f MB -> %.1f MB\n",
				wld.files, wld.bytesIn / 1048576.0, wld.bytesOut / 1048576.0, wld.surfacesIn, wld.surfacesOut,
				wld.verts, wld.triangles, wld.gridIn / 1048576.0, wld.gridOut / 1048576.0 );
		if ( wld.dropped ) {
			printf( "wld: %ld surfaces left out by map edits\n", wld.dropped );
		}
	}
	if ( aas.files ) {
		printf( "aas: %d files, %.1f MB -> %.1f MB; %ld of %ld faces kept; %d maps with no big characters, their second world left out\n",
				aas.files, aas.bytesIn / 1048576.0, aas.bytesOut / 1048576.0, aas.facesOut, aas.facesIn, aas.skipped );
	}
	if ( tex.files ) {
		printf( "tex: %d files, %.1f MB of 16 bit texels -> %.1f MB of .dt\n",
				tex.files, tex.bytesIn / 1048576.0, tex.bytesOut / 1048576.0 );
	}
	if ( failed ) {
		fprintf( stderr, "%d file(s) failed\n", failed );
		return 1;
	}
	return 0;
}
