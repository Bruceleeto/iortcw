/*
===========================================================================
snd_aica.c: sound on the Dreamcast's AICA, the sound chip mixing

The software mixer (snd_dma.c, snd_mix.c) keeps every sound in main RAM and
mixes on the CPU. Here the sounds live in AICA RAM (2 MB of its own) and each
playing sound is one of the AICA's channels, so a sound costs no main RAM and
no mixing. snd_aica.h is the chip side: dc/dc_aica.c (AICAflow) on the
Dreamcast, sys/dcsim_aica.c in the sim, which runs all of this the same way
with nothing to hear, so its main RAM is the Dreamcast's.

- a sound is AICA ADPCM (4 bits a sample): its .adp, made by make assets
  (tools/adpcmconv) and read as it is, or else its .wav, encoded here
- a sound is loaded when it's registered, a piece at a time, into the arena;
  when the arena is full the sound used longest ago goes
- a sound plays straight from there on a free voice; its volume and pan
  follow the listener every frame (only a change is sent)
- a channel counts to 65535 samples (3 s at 22 kHz), so a longer sound (the
  dialog, mostly: up to 99 s) plays through a ring: a voice going round a
  small buffer that each frame gets the next piece of the sound, read from its
  file as it plays; it's never kept, so the short sounds have AICA RAM
- the music is two rings, left and right, filled from its file
===========================================================================
*/

#include "client.h"
#include "snd_codec.h"
#include "snd_local.h"
#include "snd_public.h"
#include "snd_aica.h"

// what cl_cin.c reads of the software mixer; no RoQ sound here
dma_t	dma;
int		s_soundtime;
int		s_rawend[MAX_RAW_STREAMS];

#define AICA_VOICES			48		// of the AICA's 64 channels
#define VOICE_MUSIC_L		0
#define VOICE_MUSIC_R		1
#define FIRST_SFX_VOICE		2

#define MAX_AICA_SFX		512		// swf registers 397
#define SFX_HASH			256
#define MAX_RINGS			6		// long sounds at once
#define RING_SAMPLES		16384	// 0.74 s at 22 kHz
#define MUSIC_RING_SAMPLES	32768
#define MAX_LOOP_SOUNDS		64
#define ADP_HEADER			32
#define ADP_BLOCK			1024	// a piece of a sound: a block of an .adp, a channel's samples

#define	SOUND_FULLVOLUME	80
#define SOUND_RANGE_DEFAULT	1250

typedef struct {
	char			*name;
	unsigned		addr;			// in AICA RAM; 0: not there
	unsigned		bytes;			// its allocation
	int				frames;
	int				rate;
	unsigned short	pitch;
	byte			missing;		// no such file: plays nothing
	byte			streamed;		// longer than a channel plays: read as it plays, through a ring
	int				lastUsed;
	int				registration;	// the S_BeginRegistration it was last asked for in
	short			hashNext;
	short			arenaNext;		// the next in AICA RAM, by address
} aicaSfx_t;

typedef enum {
	VOICE_FREE,
	VOICE_ONESHOT,
	VOICE_LOOP,
	VOICE_MUSIC
} voiceKind_t;

typedef struct {
	byte			kind;
	byte			starting;		// key on still to send
	byte			stopping;		// key off still to send
	byte			fixedOrigin;
	byte			fullVolume;
	short			sfx;
	short			ring;			// -1: plays straight from the sound
	int				entnum;
	int				entchannel;
	int				flags;
	vec3_t			origin;
	int				masterVol;
	int				allocTime;
	int				endTime;		// a one-shot: when it's done, from its key on
	int				leftvol, rightvol;
	int				loopFrame;
	unsigned short	direct, mix;	// what's been sent; 0: nothing yet
} voice_t;

typedef struct {
	int				signal, step;
} adpcmState_t;

// a sound's file, a block at a time: its .adp, or its .wav made ADPCM here
typedef struct {
	fileHandle_t	adp;
	snd_stream_t	*wav;
	adpcmState_t	adpcm[2];		// the .wav's encoders, left and right
	int				rate, frames, channels;
	int				pos;			// samples read
} aicaSrc_t;

typedef struct {
	unsigned		addr;			// in AICA RAM
	int				samples;
	aicaSrc_t		src;
	int				voice;			// -1: free
	int				sfx;			// the sound played, -1 for the music
	qboolean		loop;			// the sound starts over at its end
	int				wr;				// the next sample of the ring to write
	int				written;		// since the key on, silence too
	int				realEnd;		// written when the sound ran out
	// where the voice is: worked out from when it was keyed on and the rate
	// it plays at, not read from the chip; the ring's kept half full, so the
	// two clocks can drift half a ring apart either way
	qboolean		keyed;
	int				keyTime;		// Com_Milliseconds of the key on
	float			rate;			// the samples a ms it plays
	int				played;
} ring_t;

typedef struct {
	int				entnum;
	int				sfx;
	vec3_t			origin;
	float			range;
	int				vol;
	qboolean		kill;			// gone at the next S_ClearLoopingSounds
} loopSound_t_aica;

static qboolean		s_started;
static qboolean		s_disabled;		// S_DisableSounds until S_BeginRegistration
static int			s_registration;

static unsigned		s_arenaBase, s_arenaEnd;	// the sounds' part of AICA RAM
static short		s_arenaFirst = -1;
static unsigned		s_arenaUsed, s_arenaPeak;
static int			s_evictions, s_loads;

static aicaSfx_t	s_sfx[MAX_AICA_SFX];
static int			s_numSfx;
static short		s_sfxHash[SFX_HASH];

static voice_t		s_voices[AICA_VOICES];
static ring_t		s_rings[MAX_RINGS];
static ring_t		s_musicRing;	// the left voice's; the right's is s_musicRightAddr
static unsigned		s_musicRightAddr;

static loopSound_t_aica	s_loops[MAX_LOOP_SOUNDS];
static int			s_numLoops;
static int			s_loopFrame;

static vec3_t		s_entityOrigins[MAX_GENTITIES];
static int			s_listener;
static vec3_t		s_listenerOrigin;
static vec3_t		s_listenerAxis[3];

// the music
static aicaSrc_t	s_music;
static char			s_musicLoop[MAX_QPATH];
static float		s_musicFade = 1.0f, s_musicFadeFrom, s_musicFadeTo = 1.0f;
static int			s_musicFadeStart, s_musicFadeTime;

static float		s_fade = 1.0f, s_fadeFrom, s_fadeTo = 1.0f;
static int			s_fadeStart, s_fadeTime;

// a block of a .wav (stereo 16 bit at most), and a block of ADPCM of each side
static short		s_chunk[ADP_BLOCK * 2];
static byte			s_coded[2][ADP_BLOCK / 2];

static cvar_t		*s_show;

static void S_AICA_StopAllSounds( void );
static void S_AICA_StopBackgroundTrack( void );

/*
===============================================================================

AICA ADPCM: 4 bits a sample, the Yamaha decoder's (as KallistiOS's wav2adpcm)

===============================================================================
*/

static const int adpcmDiff[16] = { 1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15 };
static const int adpcmScale[8] = { 0x0e6, 0x0e6, 0x0e6, 0x0e6, 0x133, 0x199, 0x200, 0x266 };

static int S_AdpcmNibble( adpcmState_t *st, int sample ) {
	int diff = ( ( sample - st->signal ) * 8 ) / st->step;
	int val = abs( diff ) / 2;

	if ( val > 7 ) {
		val = 7;
	}
	if ( diff < 0 ) {
		val += 8;
	}
	st->signal += ( st->step * adpcmDiff[val] ) / 8;
	if ( st->signal > 32767 ) {
		st->signal = 32767;
	} else if ( st->signal < -32768 ) {
		st->signal = -32768;
	}
	st->step = ( st->step * adpcmScale[val & 7] ) >> 8;
	if ( st->step < 0x7f ) {
		st->step = 0x7f;
	} else if ( st->step > 0x6000 ) {
		st->step = 0x6000;
	}
	return val;
}

static void S_AdpcmReset( adpcmState_t *st ) {
	st->signal = 0;
	st->step = 0x7f;
}


/*
===============================================================================

AICA registers

===============================================================================
*/

// the pitch word for a sample rate: octave (signed, 4 bits) and a 10 bit
// fraction, on 44100 Hz
static unsigned short S_AICA_Pitch( int rate ) {
	float	f = (float)rate / 44100.0f;
	int		oct = 0, fns;

	while ( f < 1.0f && oct > -8 ) {
		f *= 2.0f;
		oct--;
	}
	while ( f >= 2.0f && oct < 7 ) {
		f *= 0.5f;
		oct++;
	}
	fns = (int)( ( f - 1.0f ) * 1024.0f + 0.5f );
	if ( fns > 1023 ) {
		fns = 1023;
	} else if ( fns < 0 ) {
		fns = 0;
	}
	return (unsigned short)( ( ( oct & 15 ) << 11 ) | fns );
}

// the samples a second a pitch word plays
static float S_AICA_PitchRate( unsigned short pitch ) {
	int		oct = ( pitch >> 11 ) & 15;
	float	rate = 44100.0f * ( 1.0f + ( pitch & 1023 ) / 1024.0f );

	if ( oct & 8 ) {
		oct -= 16;
	}
	return oct >= 0 ? rate * ( 1 << oct ) : rate / ( 1 << -oct );
}

// ms from a key on to a one-shot's end: its length at the rate it plays,
// and some over (the key on goes through the driver's queue)
static int S_AICA_PlayTime( int sfx ) {
	return (int)( s_sfx[sfx].frames * 1000.0f / S_AICA_PitchRate( s_sfx[sfx].pitch ) ) + 50;
}

// volume and pan: left and right 0-255 (256 the sound as it is), times scale.
// The direct word: send level (3 dB steps, 0 off) and pan (bit 4 the left,
// the other side down 3 dB a step); the mix word: total level, 0.1875 dB a
// step (KallistiOS's), and the filter off
static void S_AICA_Levels( int left, int right, float scale, unsigned short *direct, unsigned short *mix ) {
	int		loud = left > right ? left : right;
	int		quiet = left > right ? right : left;
	float	g = loud * scale * ( 1.0f / 256.0f );
	int		tl, pan;

	if ( g < 1.0f / 256.0f ) {
		*direct = 0x0001;		// nothing out (not 0: that's "not sent")
		*mix = 0xff20;
		return;
	}
	tl = g >= 1.0f ? 0 : (int)( -20.0f * log10f( g ) / 0.1875f + 0.5f );
	if ( tl > 255 ) {
		tl = 255;
	}
	if ( quiet <= 0 ) {
		pan = 15;
	} else {
		pan = (int)( 20.0f * log10f( (float)loud / quiet ) / 3.0f + 0.5f );
		if ( pan > 15 ) {
			pan = 15;
		}
	}
	if ( pan && left > right ) {
		pan |= 0x10;
	}
	*direct = 0x0f00 | pan;
	*mix = ( tl << 8 ) | 0x20;
}

// every register word of a voice playing len samples (its loop end) from
// addr, its key on with them
static void S_AICA_VoiceSetup( unsigned short *v, unsigned addr, int len, unsigned format, qboolean loop,
							   unsigned short pitch, unsigned short direct, unsigned short mix, qboolean execute ) {
	int i;

	v[AICA_CONTROL] = format | ( loop ? AICA_LOOP : 0 ) | ( ( addr >> 16 ) & 0x7f ) | AICA_KEYON |
					  ( execute ? AICA_KEYON_EXECUTE : 0 );
	v[AICA_SA_LOW] = addr & 0xffff;
	v[AICA_LSA] = 0;
	v[AICA_LEA] = len;
	v[AICA_ENV_AD] = 0x001f;	// straight to full, held
	v[AICA_ENV_DR] = 0x001f;	// gone at once at the key off
	v[AICA_PITCH] = pitch;
	v[AICA_LFO] = 0;
	v[AICA_DSP_SEND] = 0;
	v[AICA_DIRECT] = direct;
	v[AICA_MIX] = mix;
	for ( i = AICA_FILTER_LEVEL0; i <= AICA_FILTER_LEVEL4; i++ ) {
		v[i] = 0x1fff;
	}
	v[AICA_FILTER_AD] = v[AICA_FILTER_DR] = 0x1f1f;
}

static qboolean S_AICA_KeyOff( int voice ) {
	unsigned short control = AICA_KEYON_EXECUTE;

	return AICA_Voice( voice, 1u << AICA_CONTROL, &control );
}

/*
===============================================================================

the arena: the sounds in AICA RAM, kept in address order

===============================================================================
*/

static qboolean S_AICA_SfxPlaying( int sfx ) {
	int i;

	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		if ( s_voices[i].kind != VOICE_FREE && s_voices[i].sfx == sfx ) {
			return qtrue;
		}
	}
	return qfalse;
}

static void S_AICA_Unload( int sfx ) {
	short *link;

	for ( link = &s_arenaFirst; *link >= 0; link = &s_sfx[*link].arenaNext ) {
		if ( *link == sfx ) {
			*link = s_sfx[sfx].arenaNext;
			break;
		}
	}
	s_arenaUsed -= s_sfx[sfx].bytes;
	s_sfx[sfx].addr = 0;
	s_sfx[sfx].bytes = 0;
	s_sfx[sfx].arenaNext = -1;
}

// the smallest gap that fits; 0 if none
static unsigned S_AICA_FindGap( unsigned bytes, short **linkOut ) {
	unsigned	start = s_arenaBase, best = 0, bestSize = 0xffffffffu;
	short		*link = &s_arenaFirst, *bestLink = NULL;

	for ( ;; ) {
		unsigned end = *link >= 0 ? s_sfx[*link].addr : s_arenaEnd;

		if ( end - start >= bytes && end - start < bestSize ) {
			best = start;
			bestSize = end - start;
			bestLink = link;
		}
		if ( *link < 0 ) {
			break;
		}
		start = s_sfx[*link].addr + s_sfx[*link].bytes;
		link = &s_sfx[*link].arenaNext;
	}
	*linkOut = bestLink;
	return best;
}

// room for sfx's bytes, the sound used longest ago (and not playing) going
// until there is
static qboolean S_AICA_Alloc( int sfx, unsigned bytes ) {
	for ( ;; ) {
		short		*link;
		int			i, oldest = -1;
		unsigned	addr = S_AICA_FindGap( bytes, &link );

		if ( addr ) {
			s_sfx[sfx].addr = addr;
			s_sfx[sfx].bytes = bytes;
			s_sfx[sfx].arenaNext = *link;
			*link = sfx;
			s_arenaUsed += bytes;
			if ( s_arenaUsed > s_arenaPeak ) {
				s_arenaPeak = s_arenaUsed;
			}
			return qtrue;
		}
		for ( i = s_arenaFirst; i >= 0; i = s_sfx[i].arenaNext ) {
			if ( i != sfx && ( oldest < 0 || s_sfx[i].lastUsed < s_sfx[oldest].lastUsed ) &&
				 !S_AICA_SfxPlaying( i ) ) {
				oldest = i;
			}
		}
		if ( oldest < 0 ) {
			return qfalse;
		}
		Com_DPrintf( "S_AICA: %s out of AICA RAM for %s\n", s_sfx[oldest].name, s_sfx[sfx].name );
		s_evictions++;
		S_AICA_Unload( oldest );
	}
}

/*
===============================================================================

loading

===============================================================================
*/

/*
===============================================================================

the sounds' files

===============================================================================
*/

static void S_AICA_SrcClose( aicaSrc_t *src ) {
	if ( src->adp ) {
		FS_FCloseFile( src->adp );
		src->adp = 0;
	}
	if ( src->wav ) {
		S_CodecCloseStream( src->wav );
		src->wav = NULL;
	}
}

static int S_AICA_Get32( const byte *p ) {
	return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
}

// name's .adp, else name (a .wav); the encoders (a .wav's) start over only
// when keepState is qfalse: a ring's voice carries on from where they are
static qboolean S_AICA_SrcOpen( aicaSrc_t *src, const char *name, qboolean keepState ) {
	char	adpName[MAX_QPATH];
	byte	h[ADP_HEADER];

	S_AICA_SrcClose( src );
	if ( !keepState ) {
		Com_Memset( src, 0, sizeof( *src ) );
		S_AdpcmReset( &src->adpcm[0] );
		S_AdpcmReset( &src->adpcm[1] );
	}
	src->pos = 0;

	COM_StripExtension( name, adpName, sizeof( adpName ) );
	Q_strcat( adpName, sizeof( adpName ), ".adp" );
	FS_FOpenFileRead( adpName, &src->adp, qtrue );
	if ( src->adp ) {
		if ( FS_Read( h, ADP_HEADER, src->adp ) == ADP_HEADER && !memcmp( h, "ADP1", 4 ) &&
			 S_AICA_Get32( h + 4 ) > 0 && S_AICA_Get32( h + 8 ) > 0 &&
			 ( S_AICA_Get32( h + 12 ) == 1 || S_AICA_Get32( h + 12 ) == 2 ) && S_AICA_Get32( h + 16 ) == ADP_BLOCK ) {
			src->rate = S_AICA_Get32( h + 4 );
			src->frames = S_AICA_Get32( h + 8 );
			src->channels = S_AICA_Get32( h + 12 );
			return qtrue;
		}
		Com_Printf( S_COLOR_YELLOW "WARNING: %s isn't an .adp this can play\n", adpName );
		FS_FCloseFile( src->adp );
		src->adp = 0;
	}

	src->wav = S_CodecOpenStream( name );
	if ( !src->wav ) {
		return qfalse;
	}
	if ( src->wav->info.samples <= 0 || src->wav->info.rate <= 0 ||
		 ( src->wav->info.width != 1 && src->wav->info.width != 2 ) ||
		 ( src->wav->info.channels != 1 && src->wav->info.channels != 2 ) ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: %s: can't play its format\n", name );
		S_CodecCloseStream( src->wav );
		src->wav = NULL;
		return qfalse;
	}
	src->rate = src->wav->info.rate;
	src->frames = src->wav->info.samples;
	src->channels = src->wav->info.channels;
	return qtrue;
}

// the next block, into s_coded[0] (the left, or both mixed when !stereo) and
// with stereo s_coded[1] (the right: the left again from a mono sound); how
// many of its samples were the sound's, the rest silence
static int S_AICA_SrcRead( aicaSrc_t *src, qboolean stereo ) {
	int real = src->frames - src->pos;

	if ( real > ADP_BLOCK ) {
		real = ADP_BLOCK;
	}
	if ( real <= 0 || ( !src->adp && !src->wav ) ) {
		Com_Memset( s_coded, 0x80, sizeof( s_coded ) );		// + and - the least step: silence
		return 0;
	}

	if ( src->adp ) {
		// a block of each channel; a stereo one's right is just skipped for a mono voice
		if ( FS_Read( s_coded[0], ADP_BLOCK / 2, src->adp ) != ADP_BLOCK / 2 ||
			 ( src->channels == 2 && FS_Read( s_coded[1], ADP_BLOCK / 2, src->adp ) != ADP_BLOCK / 2 ) ) {
			src->pos = src->frames;
			Com_Memset( s_coded, 0x80, sizeof( s_coded ) );
			return 0;
		}
		if ( src->channels == 1 ) {
			Com_Memcpy( s_coded[1], s_coded[0], ADP_BLOCK / 2 );
		}
	} else {
		snd_info_t	*info = &src->wav->info;
		int			frame = info->width * info->channels;
		int			got = S_CodecReadStream( src->wav, real * frame, s_chunk ), i;
		const byte	*b = (const byte *)s_chunk;

		got = got > 0 ? got / frame : 0;
		if ( got < real ) {
			real = got;
			src->pos = src->frames - real;	// the end, after this
		}
		for ( i = 0; i < ADP_BLOCK; i++ ) {
			int l = 0, r = 0, shift = ( i & 1 ) * 4;

			if ( i < got ) {
				if ( info->width == 1 ) {
					l = ( b[i * info->channels] - 128 ) << 8;
					r = ( b[i * info->channels + info->channels - 1] - 128 ) << 8;
				} else {
					l = s_chunk[i * info->channels];
					r = s_chunk[i * info->channels + info->channels - 1];
				}
			}
			if ( !shift ) {
				s_coded[0][i >> 1] = s_coded[1][i >> 1] = 0;
			}
			if ( stereo ) {
				s_coded[0][i >> 1] |= S_AdpcmNibble( &src->adpcm[0], l ) << shift;
				s_coded[1][i >> 1] |= S_AdpcmNibble( &src->adpcm[1], r ) << shift;
			} else {
				s_coded[0][i >> 1] |= S_AdpcmNibble( &src->adpcm[0], ( l + r ) >> 1 ) << shift;
			}
		}
	}
	src->pos += ADP_BLOCK;
	return real;
}

/*
===============================================================================

loading

===============================================================================
*/

static qboolean S_AICA_Load( int sfxNum ) {
	static const byte	zero[32];
	aicaSfx_t			*sfx = &s_sfx[sfxNum];
	aicaSrc_t			src;
	unsigned			bytes, at;
	int					done;

	Com_Memset( &src, 0, sizeof( src ) );
	if ( !S_AICA_SrcOpen( &src, sfx->name, qfalse ) ) {
		return qfalse;
	}
	sfx->frames = src.frames;
	sfx->rate = src.rate;
	sfx->pitch = S_AICA_Pitch( src.rate );
	if ( src.frames > 65535 ) {
		// too long for a channel: read as it plays
		S_AICA_SrcClose( &src );
		sfx->streamed = qtrue;
		return qtrue;
	}

	// whole blocks, and 32 bytes after: the chip reads a sample on
	bytes = ( ( src.frames + ADP_BLOCK - 1 ) / ADP_BLOCK * ( ADP_BLOCK / 2 ) + 32 + 31 ) & ~31;
	if ( !S_AICA_Alloc( sfxNum, bytes ) ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: no room in AICA RAM for %s\n", sfx->name );
		S_AICA_SrcClose( &src );
		return qfalse;
	}
	s_loads++;

	at = sfx->addr;
	for ( done = 0; done < src.frames; done += ADP_BLOCK ) {
		S_AICA_SrcRead( &src, qfalse );
		AICA_Write( at, s_coded[0], ADP_BLOCK / 2 );
		at += ADP_BLOCK / 2;
	}
	S_AICA_SrcClose( &src );
	AICA_Write( at, zero, sizeof( zero ) );
	return qtrue;
}

// its data in AICA RAM, loading it again if it went
static qboolean S_AICA_Ready( int sfx ) {
	if ( s_sfx[sfx].missing ) {
		return qfalse;
	}
	if ( s_sfx[sfx].streamed || s_sfx[sfx].addr ) {
		return qtrue;
	}
	return S_AICA_Load( sfx );
}

/*
===============================================================================

registration

===============================================================================
*/

static int S_AICA_Hash( const char *name ) {
	int		hash = 0, i;

	for ( i = 0; name[i]; i++ ) {
		char c = tolower( name[i] );

		if ( c == '\\' ) {
			c = '/';
		}
		hash = hash * 31 + c;
	}
	return hash & ( SFX_HASH - 1 );
}

static void S_AICA_FreeSfx( int i ) {
	short *link;

	for ( link = &s_sfxHash[S_AICA_Hash( s_sfx[i].name )]; *link >= 0; link = &s_sfx[*link].hashNext ) {
		if ( *link == i ) {
			*link = s_sfx[i].hashNext;
			break;
		}
	}
	if ( s_sfx[i].addr ) {
		S_AICA_Unload( i );
	}
	Z_Free( s_sfx[i].name );
	Com_Memset( &s_sfx[i], 0, sizeof( s_sfx[i] ) );
	s_sfx[i].hashNext = s_sfx[i].arenaNext = -1;
}

static int S_AICA_FindName( const char *name ) {
	int		hash = S_AICA_Hash( name ), i;

	for ( i = s_sfxHash[hash]; i >= 0; i = s_sfx[i].hashNext ) {
		if ( !Q_stricmp( s_sfx[i].name, name ) ) {
			return i;
		}
	}

	// a free one, or one nothing has asked for since the last registration
	for ( i = 1; i < s_numSfx; i++ ) {
		if ( !s_sfx[i].name ) {
			break;
		}
	}
	if ( i == s_numSfx ) {
		if ( s_numSfx < MAX_AICA_SFX ) {
			s_numSfx++;
		} else {
			int oldest = -1;

			for ( i = 1; i < s_numSfx; i++ ) {
				if ( s_sfx[i].registration != s_registration && !S_AICA_SfxPlaying( i ) &&
					 ( oldest < 0 || s_sfx[i].lastUsed < s_sfx[oldest].lastUsed ) ) {
					oldest = i;
				}
			}
			if ( oldest < 0 ) {
				Com_Printf( S_COLOR_YELLOW "WARNING: more than %d sounds: no %s\n", MAX_AICA_SFX, name );
				return 0;
			}
			S_AICA_FreeSfx( oldest );
			i = oldest;
		}
	}

	Com_Memset( &s_sfx[i], 0, sizeof( s_sfx[i] ) );
	s_sfx[i].name = CopyString( name );
	s_sfx[i].arenaNext = -1;
	s_sfx[i].hashNext = s_sfxHash[hash];
	s_sfxHash[hash] = i;
	return i;
}

static sfxHandle_t S_AICA_RegisterSound( const char *name, qboolean compressed ) {
	int sfx;

	if ( !s_started || !name || !name[0] ) {
		return 0;
	}
	if ( strlen( name ) >= MAX_QPATH ) {
		Com_DPrintf( "Sound name exceeds MAX_QPATH\n" );
		return 0;
	}

	sfx = S_AICA_FindName( name );
	if ( !sfx ) {
		return 0;
	}
	s_sfx[sfx].registration = s_registration;
	if ( s_sfx[sfx].missing ) {
		return 0;
	}
	if ( !s_sfx[sfx].addr && !s_sfx[sfx].streamed ) {
		s_sfx[sfx].lastUsed = Com_Milliseconds();
		if ( !S_AICA_Load( sfx ) ) {
			// no such file (or no room): nothing again till the next map
			if ( !s_sfx[sfx].addr ) {
				s_sfx[sfx].missing = qtrue;
			}
			Com_DPrintf( S_COLOR_YELLOW "WARNING: could not load %s\n", name );
			return 0;
		}
	}
	return sfx;
}

static void S_AICA_BeginRegistration( void ) {
	int i;

	s_disabled = qfalse;
	s_registration++;
	if ( !s_numSfx ) {
		// 0 is no sound
		for ( i = 0; i < SFX_HASH; i++ ) {
			s_sfxHash[i] = -1;
		}
		Com_Memset( &s_sfx[0], 0, sizeof( s_sfx[0] ) );
		s_sfx[0].name = CopyString( "***DEFAULT***" );
		s_sfx[0].missing = qtrue;
		s_sfx[0].hashNext = s_sfx[0].arenaNext = -1;
		s_numSfx = 1;
	}
	// try again what wasn't there
	for ( i = 1; i < s_numSfx; i++ ) {
		s_sfx[i].missing = qfalse;
	}
}

static void S_AICA_DisableSounds( void ) {
	S_AICA_StopAllSounds();
	s_disabled = qtrue;
}

/*
===============================================================================

spatializing (as snd_dma.c)

===============================================================================
*/

static void S_AICA_SpatializeOrigin( const vec3_t origin, int masterVol, int *leftVol, int *rightVol, float range ) {
	vec_t	dot, dist, lscale, rscale, scale;
	vec3_t	sourceVec, vec;
	float	distFullvol = range * 0.064f;		// default range of 1250 gives 80

	VectorSubtract( origin, s_listenerOrigin, sourceVec );
	dist = VectorNormalize( sourceVec ) - distFullvol;
	if ( dist < 0 ) {
		dist = 0;
	}
	if ( dist ) {
		dist = dist / range;
	}
	VectorRotate( sourceVec, s_listenerAxis, vec );

	dot = -vec[1];
	rscale = 0.5f * ( 1.0f + dot );
	lscale = 0.5f * ( 1.0f - dot );
	if ( rscale < 0 ) {
		rscale = 0;
	}
	if ( lscale < 0 ) {
		lscale = 0;
	}

	scale = ( 1.0f - dist ) * rscale;
	*rightVol = masterVol * scale;
	if ( *rightVol < 0 ) {
		*rightVol = 0;
	}
	scale = ( 1.0f - dist ) * lscale;
	*leftVol = masterVol * scale;
	if ( *leftVol < 0 ) {
		*leftVol = 0;
	}
}

static qboolean S_AICA_HearingThroughEntity( int entityNum, const vec3_t origin ) {
	vec3_t sorigin;

	if ( entityNum != s_listener ) {
		return qfalse;
	}
	if ( origin ) {
		VectorCopy( origin, sorigin );
	} else {
		VectorCopy( s_entityOrigins[entityNum], sorigin );
	}
	// first person, not third
	return DistanceSquared( sorigin, s_listenerOrigin ) <= THIRD_PERSON_THRESHOLD_SQ;
}

static void S_AICA_SpatializeVoice( voice_t *v ) {
	if ( v->fullVolume ) {
		v->leftvol = v->rightvol = v->masterVol;
	} else {
		S_AICA_SpatializeOrigin( v->fixedOrigin ? v->origin : s_entityOrigins[v->entnum], v->masterVol,
								 &v->leftvol, &v->rightvol, SOUND_RANGE_DEFAULT );
	}
}

/*
===============================================================================

voices

===============================================================================
*/

static void S_AICA_FreeRing( int ring ) {
	if ( ring >= 0 ) {
		S_AICA_SrcClose( &s_rings[ring].src );
		s_rings[ring].voice = -1;
	}
}

// stop a voice: its key off goes out with the next update
static void S_AICA_StopVoice( voice_t *v ) {
	if ( v->kind == VOICE_FREE ) {
		return;
	}
	S_AICA_FreeRing( v->ring );
	v->ring = -1;
	v->stopping = !v->starting;		// never keyed on: nothing to key off
	v->starting = qfalse;
	v->kind = VOICE_FREE;
	v->sfx = -1;
	v->entnum = -1;
}

static voice_t *S_AICA_FreeVoice( void ) {
	int i;

	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		if ( s_voices[i].kind == VOICE_FREE ) {
			return &s_voices[i];
		}
	}
	return NULL;
}

static int S_AICA_GetRing( int sfx, qboolean loop ) {
	int i;

	for ( i = 0; i < MAX_RINGS; i++ ) {
		ring_t *r = &s_rings[i];

		if ( r->voice < 0 ) {
			if ( !S_AICA_SrcOpen( &r->src, s_sfx[sfx].name, qfalse ) ) {
				return -1;
			}
			r->sfx = sfx;
			r->loop = loop;
			r->wr = 0;
			r->written = r->played = 0;
			r->realEnd = 0x7fffffff;
			r->keyed = qfalse;
			return i;
		}
	}
	return -1;
}

// start sfx on a voice: key on with the next update, once its volume's known
static qboolean S_AICA_StartVoice( voice_t *v, int sfx, voiceKind_t kind ) {
	int ring = -1;

	if ( s_sfx[sfx].streamed ) {
		ring = S_AICA_GetRing( sfx, kind == VOICE_LOOP );
		if ( ring < 0 ) {
			Com_DPrintf( "S_AICA: no ring for %s\n", s_sfx[sfx].name );
			return qfalse;
		}
		s_rings[ring].voice = v - s_voices;
	}
	v->kind = kind;
	v->starting = qtrue;
	v->sfx = sfx;
	v->ring = ring;
	v->direct = v->mix = 0;
	v->allocTime = Com_Milliseconds();
	v->endTime = v->allocTime + S_AICA_PlayTime( sfx );	// again at the key on
	return qtrue;
}

/*
====================
S_AICA_StartSoundEx (as snd_dma.c's S_Base_MainStartSound)

  flags (not for looping sounds):
	SND_NORMAL		0		- cut off only by the same sound on this channel
	SND_OKTOCUT		0x001	- cut off by any following sounds on this channel
	SND_REQUESTCUT	0x002	- cut off on this channel only by sounds asking for it
	SND_CUTOFF		0x004	- cut off sounds on this channel marked SND_REQUESTCUT
	SND_CUTOFF_ALL	0x008	- cut off every sound on this channel
====================
*/
static void S_AICA_MainStartSound( vec3_t origin, int entityNum, int entchannel, sfxHandle_t sfxHandle, int flags ) {
	voice_t		*v;
	int			i, oldest, chosen, time, inplay, allowed;

	if ( !s_started || s_disabled ) {
		return;
	}
	if ( !origin && ( entityNum < 0 || entityNum >= MAX_GENTITIES ) ) {
		Com_Error( ERR_DROP, "S_StartSound: bad entitynum %i", entityNum );
	}
	if ( sfxHandle < 0 || sfxHandle >= s_numSfx || !s_sfx[sfxHandle].name ) {
		Com_Printf( S_COLOR_YELLOW "S_StartSound: handle %i out of range\n", sfxHandle );
		return;
	}
	if ( s_show->integer == 1 ) {
		Com_Printf( "%i : %s\n", Com_Milliseconds(), s_sfx[sfxHandle].name );
	}

	time = Com_Milliseconds();

	allowed = entityNum == s_listener ? 8 : 4;
	inplay = 0;
	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		v = &s_voices[i];
		if ( v->kind == VOICE_ONESHOT && v->entnum == entityNum && v->sfx == sfxHandle ) {
			if ( time - v->allocTime < 50 ) {
				return;		// started twice
			}
			inplay++;
		}
	}
	if ( inplay > allowed ) {
		return;
	}

	s_sfx[sfxHandle].lastUsed = time;

	// shut off other sounds on this channel if necessary
	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		v = &s_voices[i];
		if ( v->kind != VOICE_ONESHOT || v->entnum != entityNum || v->entchannel != entchannel ) {
			continue;
		}
		if ( flags & SND_CUTOFF_ALL ) {
			S_AICA_StopVoice( v );
			continue;
		}
		if ( v->flags & SND_NOCUT ) {
			continue;
		}
		// let client voice sounds be overwritten
		if ( entityNum < MAX_CLIENTS && v->entchannel != CHAN_AUTO && v->entchannel != CHAN_WEAPON ) {
			S_AICA_StopVoice( v );
			continue;
		}
		if ( v->flags & SND_OKTOCUT ) {
			S_AICA_StopVoice( v );
			continue;
		}
		if ( ( flags & SND_CUTOFF ) && ( v->flags & SND_REQUESTCUT ) ) {
			S_AICA_StopVoice( v );
			continue;
		}
	}

	// a null weapon sound only kills the weapon sounds playing (a guy dying)
	if ( !sfxHandle || !S_AICA_Ready( sfxHandle ) ) {
		return;
	}

	// re-use a voice if applicable
	v = NULL;
	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		voice_t *o = &s_voices[i];

		if ( o->kind == VOICE_ONESHOT && o->entnum == entityNum && o->entchannel == entchannel &&
			 entchannel != CHAN_AUTO && !( o->flags & SND_NOCUT ) && o->sfx == sfxHandle ) {
			S_AICA_StopVoice( o );
			v = o;
			break;
		}
	}
	if ( !v ) {
		v = S_AICA_FreeVoice();
	}
	if ( !v ) {
		// steal one: this entity's same sound, else the oldest not the listener's
		oldest = time;
		chosen = -1;
		for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
			voice_t *o = &s_voices[i];

			if ( o->kind != VOICE_ONESHOT ) {
				continue;
			}
			if ( o->entnum == entityNum && o->sfx == sfxHandle ) {
				chosen = i;
				break;
			}
			if ( o->entnum != s_listener && o->entnum == entityNum && o->allocTime < oldest &&
				 o->entchannel != CHAN_ANNOUNCER ) {
				oldest = o->allocTime;
				chosen = i;
			}
		}
		if ( chosen < 0 ) {
			for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
				voice_t *o = &s_voices[i];

				if ( o->kind == VOICE_ONESHOT && o->entnum != s_listener && o->allocTime < oldest &&
					 o->entchannel != CHAN_ANNOUNCER ) {
					oldest = o->allocTime;
					chosen = i;
				}
			}
		}
		if ( chosen < 0 && entityNum == s_listener ) {
			for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
				voice_t *o = &s_voices[i];

				if ( o->kind == VOICE_ONESHOT && o->allocTime < oldest ) {
					oldest = o->allocTime;
					chosen = i;
				}
			}
		}
		if ( chosen < 0 ) {
			return;		// dropped
		}
		v = &s_voices[chosen];
		S_AICA_StopVoice( v );
	}

	if ( !S_AICA_StartVoice( v, sfxHandle, VOICE_ONESHOT ) ) {
		return;
	}
	if ( origin ) {
		VectorCopy( origin, v->origin );
		v->fixedOrigin = qtrue;
	} else {
		v->fixedOrigin = qfalse;
	}
	v->flags = flags;
	v->masterVol = 127;
	v->entnum = entityNum;
	v->entchannel = entchannel;
	v->fullVolume = S_AICA_HearingThroughEntity( entityNum, origin );
}

static void S_AICA_StartSoundEx( vec3_t origin, int entityNum, int entchannel, sfxHandle_t sfx, int flags ) {
	if ( !s_started || s_disabled || ( clc.state != CA_ACTIVE && clc.state != CA_DISCONNECTED ) ) {
		return;
	}
	// a lot of null sounds would only take voices; a null weapon sound still
	// cuts the weapon sounds
	if ( !sfx && entchannel != CHAN_WEAPON ) {
		return;
	}
	S_AICA_MainStartSound( origin, entityNum, entchannel, sfx, flags );
}

static void S_AICA_StartSound( vec3_t origin, int entityNum, int entchannel, sfxHandle_t sfx ) {
	S_AICA_MainStartSound( origin, entityNum, entchannel, sfx, 0 );
}

static void S_AICA_StartLocalSound( sfxHandle_t sfx, int channelNum ) {
	if ( !s_started || s_disabled ) {
		return;
	}
	if ( sfx < 0 || sfx >= s_numSfx ) {
		Com_Printf( S_COLOR_YELLOW "S_StartLocalSound: handle %i out of range\n", sfx );
		return;
	}
	S_AICA_MainStartSound( NULL, s_listener, channelNum, sfx, 0 );
}

/*
===============================================================================

looping sounds: added each frame, the same sound's merged on one voice

===============================================================================
*/

static loopSound_t_aica *S_AICA_Loop( int entityNum ) {
	int i;

	for ( i = 0; i < s_numLoops; i++ ) {
		if ( s_loops[i].entnum == entityNum ) {
			return &s_loops[i];
		}
	}
	if ( s_numLoops == MAX_LOOP_SOUNDS ) {
		return NULL;
	}
	return &s_loops[s_numLoops++];
}

static void S_AICA_StopLoopingSound( int entityNum ) {
	int i;

	for ( i = 0; i < s_numLoops; i++ ) {
		if ( s_loops[i].entnum == entityNum ) {
			s_loops[i] = s_loops[--s_numLoops];
			return;
		}
	}
}

static void S_AICA_ClearLoopingSounds( qboolean killall ) {
	int i;

	for ( i = 0; i < s_numLoops; ) {
		if ( killall || s_loops[i].kill ) {
			s_loops[i] = s_loops[--s_numLoops];
		} else {
			i++;
		}
	}
}

#define UNDERWATER_BIT	8

static void S_AICA_AddLoop( int entityNum, const vec3_t origin, const int range, sfxHandle_t sfx, int volume, qboolean kill ) {
	loopSound_t_aica *loop;

	if ( !s_started || s_disabled ) {
		return;
	}
	if ( sfx < 0 || sfx >= s_numSfx ) {
		Com_Printf( S_COLOR_YELLOW "S_AddLoopingSound: handle %i out of range\n", sfx );
		return;
	}
	if ( entityNum < 0 || entityNum >= MAX_LOOPSOUNDS || !sfx || s_sfx[sfx].missing ) {
		return;
	}
	loop = S_AICA_Loop( entityNum );
	if ( !loop ) {
		return;
	}
	loop->entnum = entityNum;
	loop->sfx = sfx;
	VectorCopy( origin, loop->origin );
	loop->range = range ? range : SOUND_RANGE_DEFAULT;
	volume &= ~( 1 << UNDERWATER_BIT );
	loop->vol = volume > 255 ? 255 : volume < 0 ? 0 : volume;
	loop->kill = kill;
}

static void S_AICA_AddLoopingSound( int entityNum, const vec3_t origin, const vec3_t velocity, const int range, sfxHandle_t sfx, int volume ) {
	if ( clc.state != CA_ACTIVE || !volume ) {
		return;
	}
	S_AICA_AddLoop( entityNum, origin, range, sfx, volume, qtrue );
}

static void S_AICA_AddRealLoopingSound( int entityNum, const vec3_t origin, const vec3_t velocity, const int range, sfxHandle_t sfx ) {
	S_AICA_AddLoop( entityNum, origin, range, sfx, 255, qfalse );
}

// each sound looping: the spatialized volumes of all its loops summed, on
// its voice (snd_dma.c's S_AddLoopSounds)
static void S_AICA_UpdateLoops( void ) {
	int		i, j, time = Com_Milliseconds();
	int		frame = ++s_loopFrame;

	for ( i = 0; i < s_numLoops; i++ ) {
		loopSound_t_aica	*loop = &s_loops[i];
		int					left = 0, right = 0, l, r;
		voice_t				*v = NULL;

		for ( j = 0; j < i; j++ ) {
			if ( s_loops[j].sfx == loop->sfx ) {
				break;
			}
		}
		if ( j < i ) {
			continue;		// already summed with an earlier one
		}
		for ( j = i; j < s_numLoops; j++ ) {
			if ( s_loops[j].sfx != loop->sfx ) {
				continue;
			}
			S_AICA_SpatializeOrigin( s_loops[j].origin, 90, &l, &r, s_loops[j].range );
			left += s_loops[j].vol * l / 256;
			right += s_loops[j].vol * r / 256;
		}
		if ( !left && !right ) {
			continue;		// not heard
		}
		s_sfx[loop->sfx].lastUsed = time;

		for ( j = FIRST_SFX_VOICE; j < AICA_VOICES; j++ ) {
			if ( s_voices[j].kind == VOICE_LOOP && s_voices[j].sfx == loop->sfx ) {
				v = &s_voices[j];
				break;
			}
		}
		if ( !v ) {
			v = S_AICA_FreeVoice();
			if ( !v || !S_AICA_Ready( loop->sfx ) || !S_AICA_StartVoice( v, loop->sfx, VOICE_LOOP ) ) {
				continue;
			}
			v->entnum = -1;
			v->flags = 0;
		}
		v->leftvol = left > 255 ? 255 : left;
		v->rightvol = right > 255 ? 255 : right;
		v->loopFrame = frame;
	}

	// what stopped looping, or isn't heard
	for ( j = FIRST_SFX_VOICE; j < AICA_VOICES; j++ ) {
		if ( s_voices[j].kind == VOICE_LOOP && s_voices[j].loopFrame != frame ) {
			S_AICA_StopVoice( &s_voices[j] );
		}
	}
}

/*
===============================================================================

rings: a long sound or the music, a piece at a time

===============================================================================
*/

// the next block of a ring's sound into s_coded[0]; how many samples were
// the sound's (all of them, looping)
static int S_AICA_RingSource( ring_t *r ) {
	int real = S_AICA_SrcRead( &r->src, qfalse );

	if ( r->loop ) {
		if ( !real ) {
			// from the start again: the .wav's encoder carries on, as the voice does
			S_AICA_SrcOpen( &r->src, s_sfx[r->sfx].name, qtrue );
			S_AICA_SrcRead( &r->src, qfalse );
		}
		return ADP_BLOCK;
	}
	return real;
}

// the next block of the music, both sides; how many samples were music
static int S_AICA_MusicSource( void ) {
	int real = S_AICA_SrcRead( &s_music, qtrue );

	if ( !real && s_musicLoop[0] && ( s_music.adp || s_music.wav ) ) {
		// the intro's done: the loop, again and again
		if ( S_AICA_SrcOpen( &s_music, s_musicLoop, qtrue ) ) {
			real = S_AICA_SrcRead( &s_music, qtrue );
		}
	}
	if ( !real ) {
		S_AICA_SrcClose( &s_music );
	}
	return real;
}

// the ring's voice keyed on now, at pitch
static void S_AICA_RingKeyed( ring_t *r, unsigned short pitch ) {
	r->keyed = qtrue;
	r->keyTime = Com_Milliseconds();
	r->rate = S_AICA_PitchRate( pitch ) * 0.001f;	// a ms
	r->played = 0;
}

// fill a ring to half a ring ahead of where its voice is; qfalse when its
// sound has played to the end
static qboolean S_AICA_FillRing( ring_t *r, qboolean music ) {
	int ahead;

	if ( r->keyed ) {
		r->played = (int)( (float)( Com_Milliseconds() - r->keyTime ) * r->rate );
		if ( r->played >= r->realEnd ) {
			return qfalse;
		}
		if ( r->played > r->written ) {
			// behind (a long frame): it's played what was there; on from
			// a block or two ahead of it
			r->written = ( r->played / ADP_BLOCK + 2 ) * ADP_BLOCK;
			r->wr = r->written % r->samples;
		}
	}

	// a block at a time
	ahead = r->written - r->played;
	while ( ahead + ADP_BLOCK <= r->samples / 2 ) {
		int real;

		if ( music ) {
			real = S_AICA_MusicSource();
			AICA_Write( r->addr + r->wr / 2, s_coded[0], ADP_BLOCK / 2 );
			AICA_Write( s_musicRightAddr + r->wr / 2, s_coded[1], ADP_BLOCK / 2 );
		} else {
			real = S_AICA_RingSource( r );
			AICA_Write( r->addr + r->wr / 2, s_coded[0], ADP_BLOCK / 2 );
		}
		if ( real < ADP_BLOCK && r->realEnd == 0x7fffffff ) {
			r->realEnd = r->written + real;
		}
		r->wr += ADP_BLOCK;
		if ( r->wr >= r->samples ) {
			r->wr = 0;
		}
		r->written += ADP_BLOCK;
		ahead += ADP_BLOCK;
	}
	return qtrue;
}

/*
===============================================================================

the music

===============================================================================
*/

static void S_AICA_StopBackgroundTrack( void ) {
	S_AICA_SrcClose( &s_music );
	s_musicLoop[0] = 0;
	if ( s_voices[VOICE_MUSIC_L].kind == VOICE_MUSIC ) {
		s_voices[VOICE_MUSIC_L].kind = s_voices[VOICE_MUSIC_R].kind = VOICE_FREE;
		s_voices[VOICE_MUSIC_L].stopping = s_voices[VOICE_MUSIC_R].stopping = !s_voices[VOICE_MUSIC_L].starting;
		s_voices[VOICE_MUSIC_L].starting = s_voices[VOICE_MUSIC_R].starting = qfalse;
	}
	Cvar_Set( "s_currentMusic", "" );
}

static void S_AICA_StartBackgroundTrack( const char *intro, const char *loop ) {
	ring_t *r = &s_musicRing;

	if ( !intro ) {
		intro = "";
	}
	if ( !loop || !loop[0] ) {
		loop = intro;
	}
	Com_DPrintf( "S_StartBackgroundTrack( %s, %s )\n", intro, loop );

	S_AICA_StopBackgroundTrack();
	if ( !*intro || !s_started ) {
		return;
	}

	if ( !S_AICA_SrcOpen( &s_music, intro, qfalse ) ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: couldn't open music file %s\n", intro );
		return;
	}
	Q_strncpyz( s_musicLoop, loop, sizeof( s_musicLoop ) );
	Cvar_Set( "s_currentMusic", s_musicLoop );		// so the savegame has the right music

	// the left voice keeps the ring's count; both start on the right's key on
	r->sfx = -1;
	r->loop = qfalse;
	r->wr = r->written = r->played = 0;
	r->realEnd = 0x7fffffff;
	r->keyed = qfalse;
	S_AICA_FillRing( r, qtrue );

	s_voices[VOICE_MUSIC_L].kind = s_voices[VOICE_MUSIC_R].kind = VOICE_MUSIC;
	s_voices[VOICE_MUSIC_L].starting = s_voices[VOICE_MUSIC_R].starting = qtrue;
	s_voices[VOICE_MUSIC_L].direct = s_voices[VOICE_MUSIC_R].direct = 0;
	s_voices[VOICE_MUSIC_L].sfx = s_voices[VOICE_MUSIC_R].sfx = -1;
	s_voices[VOICE_MUSIC_L].allocTime = s_music.rate;	// its pitch, for the key on
}

static void S_AICA_FadeStreamingSound( float targetvol, int time, int ssNum ) {
	if ( ssNum != 0 ) {
		return;		// 0 is the music; no other streaming sounds here
	}
	s_musicFadeFrom = s_musicFade;
	s_musicFadeTo = targetvol;
	s_musicFadeStart = Com_Milliseconds();
	s_musicFadeTime = time;
}

static void S_AICA_FadeAllSounds( float targetvol, int time ) {
	s_fadeFrom = s_fade;
	s_fadeTo = targetvol;
	s_fadeStart = Com_Milliseconds();
	s_fadeTime = time;
}

static float S_AICA_FadeNow( float from, float to, int start, int time ) {
	int t = Com_Milliseconds() - start;

	if ( time <= 0 || t >= time ) {
		return to;
	}
	return from + ( to - from ) * t / time;
}

/*
===============================================================================

the frame

===============================================================================
*/

static void S_AICA_UpdateEntityPosition( int entityNum, const vec3_t origin ) {
	if ( entityNum < 0 || entityNum >= MAX_GENTITIES ) {
		Com_Error( ERR_DROP, "S_UpdateEntityPosition: bad entitynum %i", entityNum );
	}
	VectorCopy( origin, s_entityOrigins[entityNum] );
}

static void S_AICA_Respatialize( int entityNum, const vec3_t head, vec3_t axis[3], int inwater ) {
	s_listener = entityNum;
	VectorCopy( head, s_listenerOrigin );
	VectorCopy( axis[0], s_listenerAxis[0] );
	VectorCopy( axis[1], s_listenerAxis[1] );
	VectorCopy( axis[2], s_listenerAxis[2] );
}

// the key ons and volume changes; qfalse when the driver's queue is full
static qboolean S_AICA_SendVoices( float volume, float music ) {
	unsigned short	regs[AICA_FIELDS], direct, mix;
	int				i;

	// the music: both sides together, on the right's key on
	if ( s_voices[VOICE_MUSIC_L].kind == VOICE_MUSIC ) {
		voice_t *l = &s_voices[VOICE_MUSIC_L], *r = &s_voices[VOICE_MUSIC_R];
		unsigned format = AICA_ADPCM_STREAM;

		S_AICA_Levels( 256, 0, music, &direct, &mix );
		if ( l->starting ) {
			unsigned short pitch = S_AICA_Pitch( l->allocTime );

			if ( !S_AICA_KeyOff( VOICE_MUSIC_L ) || !S_AICA_KeyOff( VOICE_MUSIC_R ) ) {
				return qfalse;
			}
			S_AICA_VoiceSetup( regs, s_musicRing.addr, s_musicRing.samples, format, qtrue, pitch, direct, mix, qfalse );
			if ( !AICA_Voice( VOICE_MUSIC_L, ( 1u << AICA_FIELDS ) - 1, regs ) ) {
				return qfalse;
			}
			S_AICA_Levels( 0, 256, music, &direct, &mix );
			S_AICA_VoiceSetup( regs, s_musicRightAddr, s_musicRing.samples, format, qtrue, pitch, direct, mix, qtrue );
			if ( !AICA_Voice( VOICE_MUSIC_R, ( 1u << AICA_FIELDS ) - 1, regs ) ) {
				return qfalse;
			}
			l->starting = r->starting = qfalse;
			l->mix = mix;
			S_AICA_RingKeyed( &s_musicRing, pitch );
		} else if ( mix != l->mix ) {
			if ( !AICA_Voice( VOICE_MUSIC_L, 1u << AICA_MIX, &mix ) || !AICA_Voice( VOICE_MUSIC_R, 1u << AICA_MIX, &mix ) ) {
				return qfalse;
			}
			l->mix = mix;
		}
	}

	for ( i = 0; i < AICA_VOICES; i++ ) {
		voice_t *v = &s_voices[i];

		if ( v->stopping ) {
			if ( !S_AICA_KeyOff( i ) ) {
				return qfalse;
			}
			v->stopping = qfalse;
		}
		if ( v->kind != VOICE_ONESHOT && v->kind != VOICE_LOOP ) {
			continue;
		}
		S_AICA_Levels( v->leftvol, v->rightvol, volume, &direct, &mix );
		if ( v->starting ) {
			aicaSfx_t	*sfx = &s_sfx[v->sfx];
			unsigned	format;

			if ( !S_AICA_KeyOff( i ) ) {
				return qfalse;
			}
			if ( v->ring >= 0 ) {
				ring_t *r = &s_rings[v->ring];

				S_AICA_FillRing( r, qfalse );
				format = AICA_ADPCM_STREAM;
				S_AICA_VoiceSetup( regs, r->addr, r->samples, format, qtrue, sfx->pitch, direct, mix, qtrue );
			} else {
				format = AICA_ADPCM;
				S_AICA_VoiceSetup( regs, sfx->addr, sfx->frames, format, v->kind == VOICE_LOOP, sfx->pitch,
								   direct, mix, qtrue );
			}
			if ( !AICA_Voice( i, ( 1u << AICA_FIELDS ) - 1, regs ) ) {
				return qfalse;
			}
			v->starting = qfalse;
			v->direct = direct;
			v->mix = mix;
			if ( v->ring >= 0 ) {
				S_AICA_RingKeyed( &s_rings[v->ring], sfx->pitch );
			} else {
				v->endTime = Com_Milliseconds() + S_AICA_PlayTime( v->sfx );
			}
		} else if ( direct != v->direct || mix != v->mix ) {
			unsigned short dm[2] = { direct, mix };

			if ( !AICA_Voice( i, ( 1u << AICA_DIRECT ) | ( 1u << AICA_MIX ), dm ) ) {
				return qfalse;
			}
			v->direct = direct;
			v->mix = mix;
		}
	}
	return qtrue;
}

static void S_AICA_Update( void ) {
	int		i, time;
	float	volume, music;

	if ( !s_started ) {
		return;
	}
	AICA_Update();
	if ( s_disabled ) {
		return;
	}
	time = Com_Milliseconds();

	s_fade = S_AICA_FadeNow( s_fadeFrom, s_fadeTo, s_fadeStart, s_fadeTime );
	s_musicFade = S_AICA_FadeNow( s_musicFadeFrom, s_musicFadeTo, s_musicFadeStart, s_musicFadeTime );
	volume = s_muted->integer ? 0 : s_volume->value * s_fade;
	music = volume * s_musicVolume->value * s_musicFade;

	// one-shots done; rings that have played out
	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		voice_t *v = &s_voices[i];

		if ( v->kind == VOICE_ONESHOT && v->ring < 0 && !v->starting && time >= v->endTime ) {
			// it's stopped by itself, but its key on bit is still set: the
			// next key on anywhere could start it again
			S_AICA_StopVoice( v );
		}
	}

	S_AICA_UpdateLoops();
	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		if ( s_voices[i].kind == VOICE_ONESHOT ) {
			S_AICA_SpatializeVoice( &s_voices[i] );
		}
	}

	if ( s_show->integer == 2 ) {
		int total = 0;

		for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
			voice_t *v = &s_voices[i];

			if ( v->kind != VOICE_FREE && ( v->leftvol || v->rightvol ) ) {
				Com_Printf( "%d %d %s\n", v->leftvol, v->rightvol, s_sfx[v->sfx].name );
				total++;
			}
		}
		Com_Printf( "----(%i)----\n", total );
	}

	// the rings: those already playing; a new one fills as it's keyed on
	for ( i = 0; i < MAX_RINGS; i++ ) {
		ring_t *r = &s_rings[i];

		if ( r->voice < 0 || s_voices[r->voice].starting ) {
			continue;
		}
		if ( !S_AICA_FillRing( r, qfalse ) ) {
			S_AICA_StopVoice( &s_voices[r->voice] );
		}
	}
	if ( s_voices[VOICE_MUSIC_L].kind == VOICE_MUSIC && !s_voices[VOICE_MUSIC_L].starting ) {
		if ( !S_AICA_FillRing( &s_musicRing, qtrue ) ) {
			S_AICA_StopBackgroundTrack();
		}
	}

	S_AICA_SendVoices( volume * 2.0f, music * 2.0f );

	// a key on waiting too long (the queue full): not now
	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		voice_t *v = &s_voices[i];

		if ( v->starting && v->kind == VOICE_ONESHOT && time - v->allocTime > 200 ) {
			S_AICA_StopVoice( v );
		}
	}
}

/*
===============================================================================

the rest of the interface

===============================================================================
*/

static void S_AICA_StopAllSounds( void ) {
	int i;

	if ( !s_started ) {
		return;
	}
	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		S_AICA_StopVoice( &s_voices[i] );
	}
	s_numLoops = 0;
	S_AICA_StopBackgroundTrack();
	S_AICA_SendVoices( 0, 0 );
}

static void S_AICA_ClearSoundBuffer( void ) {
	int i;

	if ( !s_started ) {
		return;
	}
	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		S_AICA_StopVoice( &s_voices[i] );
	}
	s_numLoops = 0;
	Com_Memset( s_rawend, 0, sizeof( s_rawend ) );
	S_AICA_SendVoices( 0, 0 );
}

// no streaming sounds (none of the SP code starts one) nor raw samples (no
// RoQ video here)
static void S_AICA_StartStreamingSound( const char *intro, const char *loop, int entnum, int channel, int attenuation ) {
}

static void S_AICA_StopEntStreamingSound( int entnum ) {
}

static void S_AICA_RawSamples( int stream, int samples, int rate, int width, int channels, const byte *data, float volume, int entityNum ) {
}

// the mouth of someone talking: no samples in main RAM to measure, so it
// moves while a voice sound of theirs plays
static int S_AICA_GetVoiceAmplitude( int entityNum ) {
	int i;

	if ( entityNum < 0 || entityNum >= MAX_CLIENTS ) {
		Com_Printf( "Error: S_GetVoiceAmplitude() called for a non-client\n" );
		return 0;
	}
	for ( i = FIRST_SFX_VOICE; i < AICA_VOICES; i++ ) {
		voice_t *v = &s_voices[i];

		if ( v->kind == VOICE_ONESHOT && v->entnum == entityNum && v->entchannel == CHAN_VOICE ) {
			unsigned t = (unsigned)( Com_Milliseconds() - v->allocTime ) / 90u;

			return 64 + ( ( t * 2654435761u + entityNum * 40503u ) >> 24 ) % 192;
		}
	}
	return 0;
}

static void S_AICA_SoundInfo( void ) {
	unsigned	used = 0;
	int			i, sounds = 0, voices = 0;

	Com_Printf( "----- Sound Info -----\n" );
	if ( !s_started ) {
		Com_Printf( "sound system not started\n" );
	} else {
		for ( i = s_arenaFirst; i >= 0; i = s_sfx[i].arenaNext ) {
			used += s_sfx[i].bytes;
			sounds++;
		}
		for ( i = 0; i < AICA_VOICES; i++ ) {
			voices += s_voices[i].kind != VOICE_FREE;
		}
		Com_Printf( "AICA: %d voices playing of %d\n", voices, AICA_VOICES );
		Com_Printf( "%d sounds registered of %d, %d in AICA RAM: %u K of %u K\n", s_numSfx, MAX_AICA_SFX,
					sounds, used >> 10, ( s_arenaEnd - s_arenaBase ) >> 10 );
		Com_Printf( "music: %s\n", s_music.adp || s_music.wav ? s_musicLoop : "none" );
	}
	Com_Printf( "----------------------\n" );
}

static void S_AICA_SoundList( void ) {
	int			i;
	unsigned	total = 0;

	for ( i = 1; i < s_numSfx; i++ ) {
		aicaSfx_t *sfx = &s_sfx[i];

		if ( !sfx->name ) {
			continue;
		}
		if ( sfx->addr ) {
			Com_Printf( "%6u %6d Hz         %s\n", sfx->bytes, sfx->rate, sfx->name );
			total += sfx->bytes;
		} else if ( sfx->streamed ) {
			Com_Printf( "%6s %6d Hz stream  %s\n", "-", sfx->rate, sfx->name );
		} else {
			Com_Printf( "%6s           %s %s\n", "-", sfx->missing ? "missing" : "       ", sfx->name );
		}
	}
	Com_Printf( "Total AICA RAM in use: %u K\n", total >> 10 );
}

static void S_AICA_Shutdown( void ) {
	int i, named = 0;

	if ( !s_started ) {
		return;
	}
	for ( i = 1; i < s_numSfx; i++ ) {
		named += s_sfx[i].name != NULL;
	}
	Com_Printf( "AICA: %d sounds of %d, %d loads, %d pushed out; AICA RAM %u K in use, %u K at most, of %u K\n",
				named, MAX_AICA_SFX, s_loads, s_evictions, s_arenaUsed >> 10, s_arenaPeak >> 10,
				( s_arenaEnd - s_arenaBase ) >> 10 );
	S_AICA_StopAllSounds();
	for ( i = 0; i < s_numSfx; i++ ) {
		if ( s_sfx[i].name ) {
			Z_Free( s_sfx[i].name );
		}
	}
	Com_Memset( s_sfx, 0, sizeof( s_sfx ) );
	s_numSfx = 0;
	s_arenaFirst = -1;
	s_arenaUsed = s_arenaPeak = 0;
	s_evictions = s_loads = 0;
	AICA_Shutdown();
	s_started = qfalse;
}

qboolean S_AICA_Init( soundInterface_t *si ) {
	unsigned	base, bytes, top, musicBytes, ringBytes;
	int			i;

	s_show = Cvar_Get( "s_show", "0", CVAR_CHEAT );

	if ( !AICA_Init( AICA_VOICES, &base, &bytes ) ) {
		return qfalse;
	}

	// the rings at the top, the sounds below
	musicBytes = MUSIC_RING_SAMPLES / 2;
	ringBytes = RING_SAMPLES / 2;
	top = base + bytes;
	s_musicRing.samples = MUSIC_RING_SAMPLES;
	s_musicRing.addr = top -= musicBytes;
	s_musicRing.voice = VOICE_MUSIC_L;
	s_musicRightAddr = top -= musicBytes;
	for ( i = 0; i < MAX_RINGS; i++ ) {
		s_rings[i].samples = RING_SAMPLES;
		s_rings[i].addr = top -= ringBytes;
		s_rings[i].voice = -1;
	}
	s_arenaBase = base;
	s_arenaEnd = top & ~31u;
	s_arenaFirst = -1;

	for ( i = 0; i < AICA_VOICES; i++ ) {
		Com_Memset( &s_voices[i], 0, sizeof( s_voices[i] ) );
		s_voices[i].sfx = -1;
		s_voices[i].ring = -1;
		s_voices[i].entnum = -1;
	}
	for ( i = 0; i < SFX_HASH; i++ ) {
		s_sfxHash[i] = -1;
	}
	s_numSfx = 0;
	s_numLoops = 0;
	s_started = qtrue;
	s_disabled = qtrue;		// till S_BeginRegistration

	si->Shutdown = S_AICA_Shutdown;
	si->StartSound = S_AICA_StartSound;
	si->StartSoundEx = S_AICA_StartSoundEx;
	si->StartLocalSound = S_AICA_StartLocalSound;
	si->StartBackgroundTrack = S_AICA_StartBackgroundTrack;
	si->StopBackgroundTrack = S_AICA_StopBackgroundTrack;
	si->FadeStreamingSound = S_AICA_FadeStreamingSound;
	si->FadeAllSounds = S_AICA_FadeAllSounds;
	si->StartStreamingSound = S_AICA_StartStreamingSound;
	si->StopEntStreamingSound = S_AICA_StopEntStreamingSound;
	si->GetVoiceAmplitude = S_AICA_GetVoiceAmplitude;
	si->RawSamples = S_AICA_RawSamples;
	si->StopAllSounds = S_AICA_StopAllSounds;
	si->ClearLoopingSounds = S_AICA_ClearLoopingSounds;
	si->AddLoopingSound = S_AICA_AddLoopingSound;
	si->AddRealLoopingSound = S_AICA_AddRealLoopingSound;
	si->StopLoopingSound = S_AICA_StopLoopingSound;
	si->Respatialize = S_AICA_Respatialize;
	si->UpdateEntityPosition = S_AICA_UpdateEntityPosition;
	si->Update = S_AICA_Update;
	si->DisableSounds = S_AICA_DisableSounds;
	si->BeginRegistration = S_AICA_BeginRegistration;
	si->RegisterSound = S_AICA_RegisterSound;
	si->ClearSoundBuffer = S_AICA_ClearSoundBuffer;
	si->SoundInfo = S_AICA_SoundInfo;
	si->SoundList = S_AICA_SoundList;
	return qtrue;
}
