/*
===========================================================================
snd_aica.h: the AICA side of snd_aica.c (the Dreamcast's sound chip)

dc/dc_aica.c drives it through AICAflow (deps/AICAflow); sys/dcsim_aica.c is
the sim's, with nothing to play on: the same calls, the same main RAM, no
sound. Everything else (the voices, the sounds in AICA RAM, the streams) is
in snd_aica.c, the same in both.
===========================================================================
*/

#ifndef SND_AICA_H
#define SND_AICA_H

// AICA channel register words, as AICAflow's fields (AFX_FIELD_*): word n
// is the low 16 bits of the channel register at n * 4
enum {
	AICA_CONTROL, AICA_SA_LOW, AICA_LSA, AICA_LEA, AICA_ENV_AD, AICA_ENV_DR,
	AICA_PITCH, AICA_LFO, AICA_DSP_SEND, AICA_DIRECT, AICA_MIX,
	AICA_FILTER_LEVEL0, AICA_FILTER_LEVEL1, AICA_FILTER_LEVEL2,
	AICA_FILTER_LEVEL3, AICA_FILTER_LEVEL4, AICA_FILTER_AD, AICA_FILTER_DR,
	AICA_FIELDS
};

// AICA_CONTROL bits
#define AICA_KEYON_EXECUTE	0x8000	// key every channel on or off as its AICA_KEYON says
#define AICA_KEYON			0x4000
#define AICA_LOOP			0x0200
#define AICA_PCM16			0x0000
#define AICA_ADPCM			0x0100
#define AICA_ADPCM_STREAM	0x0180	// ADPCM that carries on through a loop: a ring

// the sound arena in AICA RAM: what's left after the driver; *base its AICA
// address, *bytes its size. voices: the channels snd_aica.c plays on
qboolean	AICA_Init( int voices, unsigned *base, unsigned *bytes );
void		AICA_Shutdown( void );

// copy to / from AICA RAM, at an AICA address; addr and bytes a multiple of 4
void		AICA_Write( unsigned addr, const void *data, int bytes );
void		AICA_Read( unsigned addr, void *data, int bytes );

// write a voice's register words: values[] the ones in mask, in field order.
// CONTROL goes last, so a key on comes after the rest. qfalse: the driver's
// queue is full, try again next frame
qboolean	AICA_Voice( int voice, unsigned mask, const unsigned short *values );

// once a frame
void		AICA_Update( void );

#endif
