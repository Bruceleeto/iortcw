/*
 * MDSC: an RtCW .mds skeletal model with its animation made small.
 *
 * Made by tools/rtcwconv, read by the renderer. This file and
 * mdsc.c are shared by both, so the converter checks its error with the
 * very decoder the game uses. Plain C, nothing from the engine.
 *
 * An MDSC is an .mds (mdsHeader_t, bones, surfaces, tags as they are) whose
 * ident is MDSC_IDENT and whose ofsFrames points to an mdscAnim_t instead of
 * the frames.
 *
 * An .mds frame gives every bone, for every frame, its rotation and the
 * direction to it from its parent, both in model space (12 bytes). MDSC keeps:
 *  - the rotation relative to the parent bone, where fingers, toes and much
 *    else hardly ever move, as a quaternion packed in 4 bytes;
 *  - the direction from the parent, in the parent's space, where it hardly
 *    ever changes (thighs and tails do), as 4 shorts;
 * each only on the frames where interpolating its neighbours isn't close
 * enough (a key). The converter encodes each bone against its parent as
 * decoding will rebuild it, so errors don't pile up down the skeleton.
 * Decoding puts the model-space .mds frame back together.
 */
#ifndef MDSC_H
#define MDSC_H

#ifdef __cplusplus
extern "C" {
#endif

#define MDSC_IDENT          ( ( 'C' << 24 ) + ( 'S' << 16 ) + ( 'D' << 8 ) + 'M' )
/* its surfaces' triangles as strips (STRIP_START in tr_local.h), the rest
   of each surface moved up into the room that leaves (4 and 5 had them as
   ints, 2 and 3 only key lists) */
#define MDSC_VERSION        6
#define MDSC_VERSION_SHARED 7       /* frames shared with another MDSC: mdscShare_t */

#define MDSC_MAX_BONES      128     /* MDS_MAX_BONES */
#define MDSC_FRAME_FLOATS   13      /* bounds[2], localOrigin, radius, parentOffset */
#define MDSC_POSE_SHORTS    6       /* angles[4] (4th unused), ofsAngles[2] */

/* the size of one decoded frame: an mdsFrame_t with numBones bones */
#define MDSC_FRAME_SIZE( numBones )	( MDSC_FRAME_FLOATS * 4 + ( numBones ) * MDSC_POSE_SHORTS * 2 )

typedef struct {
	int numKeys;                    /* | MDSC_KEYS_BITMAP for a bitmap of them */
	int ofsKeys;                    /* which frames are keys: a list or a bitmap */
	int ofsValues;                  /* the key values [numKeys] */
} mdscTrack_t;

/*
 * A track's keys are at frame 0, its last frame, and between. Where they're
 * many, a bit a frame is less than their frame numbers, so a track has
 * whichever is smaller (the converter picks):
 *  - a list: unsigned short frames[numKeys], padded to 4;
 *  - with MDSC_KEYS_BITMAP: unsigned short numBlocks, then the keys before
 *    each block of MDSC_KEY_BLOCK frames, unsigned short rank[numBlocks],
 *    padded to 4; then unsigned int bits[numBlocks * 2], bit f & 31 of word
 *    f >> 5 set where frame f is a key.
 * Either gives the same keys, so the same frames.
 */
#define MDSC_KEYS_BITMAP    0x40000000
#define MDSC_NUMKEYS( t )   ( ( t )->numKeys & ~MDSC_KEYS_BITMAP )
#define MDSC_KEY_BLOCK      64

/* an mdsFrame_t's floats: bounds[2], localOrigin and radius, for culling
 * (their own keys, made a little bigger so they still hold the model where
 * interpolated), then parentOffset */
#define MDSC_CULL_FLOATS    10
#define MDSC_OFFSET_FLOATS  3

typedef struct {
	mdscTrack_t frameTrack;         /* values: float[MDSC_OFFSET_FLOATS] (parentOffset) */
	mdscTrack_t cullTrack;          /* values: float[MDSC_CULL_FLOATS] */
	mdscTrack_t tracks[1];          /* [numBones] rotations: packed quaternions (uint32),
	                                   then [numBones] directions: short[4] (x, y, z, 0) */
} mdscAnim_t;                       /* all offsets are from the start of this */

#define MDSC_DIR_SCALE      32767.0f

/*
 * Shared frames (MDSC_VERSION_SHARED). Characters with the one skeleton
 * (the guards: infantryss, officerss, trench) have most of their animation
 * frames the same, byte for byte, but for the cull bounds, the mesh's. Such
 * a frame is kept once, in a base: an MDSC (MDSC_VERSION) with no surfaces
 * or tags, of the frames they share. The character's ofsFrames points to an
 * mdscShare_t: which of its frames are the base's and which its own, and an
 * mdscAnim_t of its own frames, whose cull bounds track is by the
 * character's frame numbers (all its frames), the rest by its own frames in
 * order. Frame numbers, wolfanim.cfg and all, stay as they were.
 */
typedef struct {
	int first, count;               /* the character's frames first to first + count - 1 */
	int srcFirst;                   /* are the base's (fromBase) or its own frames from srcFirst */
	int fromBase;
} mdscSegment_t;

typedef struct {
	char base[64];                  /* the base, as the game registers it (.mds) */
	int baseHandle;                 /* the renderer's: the base's model handle once loaded */
	int numSegments;
	int ofsSegments;                /* mdscSegment_t[numSegments] by first, from the start of this */
	int ofsAnim;                    /* its mdscAnim_t, from the start of this */
} mdscShare_t;

typedef float mdscMatrix_t[3][3];   /* rows are the bone's axes, as AnglesToAxis */

/* ---- for the converter ---- */

void MDSC_AnglesToMatrix( const short angles[3], mdscMatrix_t m );

/* A bone's rotation relative to its parent's (parent NULL: the root, as it
 * is), from its .mds angles. q is w, x, y, z. */
void MDSC_LocalRotation( const mdscMatrix_t parent, const short angles[3], float q[4] );

/* The direction to a bone from its parent, in the parent's space. */
void MDSC_LocalDir( const mdscMatrix_t parent, const short ofsAngles[2], float dir[3] );

/* 2 bits for the largest component, 10 for each of the others */
unsigned int MDSC_PackQuat( const float q[4] );
void MDSC_PackDir( const float dir[3], short packed[4] );

/* the angle between two rotations / two directions, in degrees */
float MDSC_QuatAngle( const float a[4], const float b[4] );
float MDSC_DirAngle( const float a[3], const float b[3] );

/* ---- for both ---- */

/* a track's value at a frame, interpolating between its keys */
void MDSC_TrackQuat( const unsigned short *keys, const unsigned int *values, int numKeys, int frame, float q[4] );
void MDSC_TrackDir( const unsigned short *keys, const short *values, int numKeys, int frame, float dir[3] );

/* a bone's model-space matrix from its parent's (NULL: the root) */
void MDSC_ChildMatrix( const mdscMatrix_t parent, const float q[4], mdscMatrix_t m );

/* Rebuilds frame `frame` of the MDSC whose file starts at `mds` into `out`,
 * laid out as an mdsFrame_t (MDSC_FRAME_SIZE bytes). Bones must come after
 * their parents, which the converter checks. */
void MDSC_DecodeFrame( const void *mds, int frame, void *out );

/* The same for an MDSC_VERSION_SHARED one, `base` its base's file. */
void MDSC_DecodeSharedFrame( const void *mds, const void *base, int frame, void *out );

#ifdef __cplusplus
}
#endif

#endif
