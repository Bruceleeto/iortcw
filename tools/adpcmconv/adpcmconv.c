/*
 * adpcmconv [-j jobs] <in dir> <out dir>
 *
 * Every .wav under <in dir> -> a .adp at the same path under <out dir>: the
 * sound in AICA ADPCM (4 bits a sample), as the Dreamcast's sound chip plays
 * it, so snd_aica.c copies it to AICA RAM (or streams it) as it is, with no
 * work on the SH4. Run by `make assets`, into sp_dcsnd.pk3.
 *
 * The encoder is AICAforge's (deps/AICAflow/dependencies/AICAforge,
 * src/afx_ya2beam.c): a beam search over the whole sound, keeping the 32
 * best ways to code it so far, so the error is the least it can be, not just
 * the least each sample.
 *
 * .adp, little endian:
 *   0  "ADP1"
 *   4  rate (Hz)
 *   8  frames (a channel's samples; the sound's, not the padding)
 *  12  channels: 1, or 2 (the music)
 *  16  ADP_BLOCK: the samples of a channel in a block
 *  20  12 bytes of 0
 *  32  the data: blocks of ADP_BLOCK samples (ADP_BLOCK / 2 bytes), a block
 *      of each channel in turn (left then right); the last padded with
 *      silence. Each channel is one ADPCM stream from the start (signal 0,
 *      step 127, as the AICA starts), the first sample of a byte in its low
 *      four bits
 */
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ADP_BLOCK	1024
#define BEAM_WIDTH	32

int ya2beam_encode( const int16_t *pcm, int n, int width, uint8_t *out );

typedef struct {
	char	*in, *out;
} job_t;

static job_t			*jobs;
static int				numJobs, nextJob, failed;
static long long		bytesIn, bytesOut;
static pthread_mutex_t	lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t Get32( const uint8_t *p ) {
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t Get16( const uint8_t *p ) {
	return p[0] | p[1] << 8;
}

static void Put32( uint8_t *p, uint32_t v ) {
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static uint8_t *ReadWhole( const char *path, long *size ) {
	FILE	*f = fopen( path, "rb" );
	uint8_t	*buf;

	if ( !f ) {
		return NULL;
	}
	fseek( f, 0, SEEK_END );
	*size = ftell( f );
	fseek( f, 0, SEEK_SET );
	buf = malloc( *size ? *size : 1 );
	if ( buf && fread( buf, 1, *size, f ) != (size_t)*size ) {
		free( buf );
		buf = NULL;
	}
	fclose( f );
	return buf;
}

static int MakeDirs( char *path ) {
	char *s;

	for ( s = path + 1; *s; s++ ) {
		if ( *s == '/' ) {
			*s = 0;
			if ( mkdir( path, 0777 ) && errno != EEXIST ) {
				*s = '/';
				return 0;
			}
			*s = '/';
		}
	}
	return 1;
}

static int Convert( const job_t *job ) {
	long		size, at;
	uint8_t		*wav = ReadWhole( job->in, &size ), *data = NULL, *out = NULL, *coded[2] = { NULL, NULL };
	int			channels = 0, rate = 0, bits = 0, ok = 0;
	uint32_t	dataBytes = 0, frames, padded, c, i;
	int16_t		*pcm = NULL;
	FILE		*f;

	if ( !wav || size < 12 || memcmp( wav, "RIFF", 4 ) || memcmp( wav + 8, "WAVE", 4 ) ) {
		fprintf( stderr, "%s: not a .wav\n", job->in );
		goto done;
	}
	for ( at = 12; at + 8 <= size; ) {
		uint32_t len = Get32( wav + at + 4 );

		if ( !memcmp( wav + at, "fmt ", 4 ) && len >= 16 && at + 8 + 16 <= size ) {
			if ( Get16( wav + at + 8 ) != 1 ) {
				fprintf( stderr, "%s: not PCM\n", job->in );
				goto done;
			}
			channels = Get16( wav + at + 10 );
			rate = Get32( wav + at + 12 );
			bits = Get16( wav + at + 22 );
		} else if ( !memcmp( wav + at, "data", 4 ) ) {
			data = wav + at + 8;
			dataBytes = len;
			if ( dataBytes > size - ( at + 8 ) ) {
				dataBytes = size - ( at + 8 );	// some are cut short
			}
			break;
		}
		at += 8 + len + ( len & 1 );
	}
	if ( !data || ( channels != 1 && channels != 2 ) || ( bits != 8 && bits != 16 ) || rate <= 0 ) {
		fprintf( stderr, "%s: can't convert (%d channels, %d bits, %d Hz)\n", job->in, channels, bits, rate );
		goto done;
	}

	frames = dataBytes / ( bits / 8 ) / channels;
	padded = ( frames + ADP_BLOCK - 1 ) / ADP_BLOCK * ADP_BLOCK;
	if ( !padded ) {
		padded = ADP_BLOCK;
	}
	pcm = calloc( padded, sizeof( *pcm ) );
	for ( c = 0; c < (uint32_t)channels; c++ ) {
		coded[c] = malloc( padded / 2 );
	}
	out = malloc( 32 + padded / 2 * channels );
	if ( !pcm || !coded[0] || ( channels == 2 && !coded[1] ) || !out ) {
		fprintf( stderr, "%s: out of memory\n", job->in );
		goto done;
	}

	// each channel its own stream, padded with silence
	for ( c = 0; c < (uint32_t)channels; c++ ) {
		for ( i = 0; i < frames; i++ ) {
			uint32_t s = i * channels + c;

			pcm[i] = bits == 8 ? ( data[s] - 128 ) << 8 : (int16_t)Get16( data + s * 2 );
		}
		if ( ya2beam_encode( pcm, padded, BEAM_WIDTH, coded[c] ) ) {
			fprintf( stderr, "%s: encoder failed\n", job->in );
			goto done;
		}
	}

	memset( out, 0, 32 );
	memcpy( out, "ADP1", 4 );
	Put32( out + 4, rate );
	Put32( out + 8, frames );
	Put32( out + 12, channels );
	Put32( out + 16, ADP_BLOCK );
	for ( i = 0; i < padded / ADP_BLOCK; i++ ) {
		for ( c = 0; c < (uint32_t)channels; c++ ) {
			memcpy( out + 32 + ( i * channels + c ) * ( ADP_BLOCK / 2 ), coded[c] + i * ( ADP_BLOCK / 2 ), ADP_BLOCK / 2 );
		}
	}

	if ( !MakeDirs( job->out ) || !( f = fopen( job->out, "wb" ) ) ) {
		fprintf( stderr, "%s: can't write\n", job->out );
		goto done;
	}
	ok = fwrite( out, 1, 32 + padded / 2 * channels, f ) == 32 + padded / 2 * channels;
	ok &= !fclose( f );
	if ( ok ) {
		pthread_mutex_lock( &lock );
		bytesIn += size;
		bytesOut += 32 + padded / 2 * channels;
		pthread_mutex_unlock( &lock );
	}
done:
	free( wav );
	free( pcm );
	free( coded[0] );
	free( coded[1] );
	free( out );
	return ok;
}

static void *Worker( void *arg ) {
	for ( ;; ) {
		int j;

		pthread_mutex_lock( &lock );
		j = nextJob++;
		pthread_mutex_unlock( &lock );
		if ( j >= numJobs ) {
			return NULL;
		}
		if ( !Convert( &jobs[j] ) ) {
			pthread_mutex_lock( &lock );
			failed++;
			pthread_mutex_unlock( &lock );
		}
	}
}

static void Find( const char *inDir, const char *outDir, const char *rel ) {
	char			path[4096];
	DIR				*d;
	struct dirent	*e;

	snprintf( path, sizeof( path ), "%s/%s", inDir, rel );
	d = opendir( path );
	if ( !d ) {
		return;
	}
	while ( ( e = readdir( d ) ) ) {
		char		sub[4096];
		struct stat	st;
		size_t		len = strlen( e->d_name );

		if ( e->d_name[0] == '.' ) {
			continue;
		}
		snprintf( sub, sizeof( sub ), "%s%s%s", rel, *rel ? "/" : "", e->d_name );
		snprintf( path, sizeof( path ), "%s/%s", inDir, sub );
		if ( stat( path, &st ) ) {
			continue;
		}
		if ( S_ISDIR( st.st_mode ) ) {
			Find( inDir, outDir, sub );
		} else if ( len > 4 && !strcasecmp( e->d_name + len - 4, ".wav" ) ) {
			jobs = realloc( jobs, ( numJobs + 1 ) * sizeof( *jobs ) );
			jobs[numJobs].in = strdup( path );
			snprintf( path, sizeof( path ), "%s/%.*s.adp", outDir, (int)strlen( sub ) - 4, sub );
			jobs[numJobs].out = strdup( path );
			numJobs++;
		}
	}
	closedir( d );
}

int main( int argc, char **argv ) {
	int			n = (int)sysconf( _SC_NPROCESSORS_ONLN ), i = 1, t;
	pthread_t	*threads;

	if ( i + 1 < argc && !strcmp( argv[i], "-j" ) ) {
		n = atoi( argv[i + 1] );
		i += 2;
	}
	if ( argc - i != 2 ) {
		fprintf( stderr, "usage: adpcmconv [-j jobs] <in dir> <out dir>\n" );
		return 2;
	}
	if ( n < 1 ) {
		n = 1;
	}
	Find( argv[i], argv[i + 1], "" );

	threads = malloc( n * sizeof( *threads ) );
	for ( t = 0; t < n; t++ ) {
		pthread_create( &threads[t], NULL, Worker, NULL );
	}
	for ( t = 0; t < n; t++ ) {
		pthread_join( threads[t], NULL );
	}

	printf( "adp: %d .wav, %.1f MB -> %.1f MB of AICA ADPCM\n", numJobs - failed, bytesIn / 1048576.0, bytesOut / 1048576.0 );
	if ( failed ) {
		fprintf( stderr, "%d file(s) failed\n", failed );
		return 1;
	}
	return 0;
}
