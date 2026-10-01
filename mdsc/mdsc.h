/*
 * MDSC: an RtCW .mds skeletal model with its animation made small.
 *
 * Made by tools/rtcwconv, read by the SP and MP renderers. This file and
 * mdsc.c are shared by all three, so the converter checks its error with the
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
#define MDSC_VERSION        1

#define MDSC_MAX_BONES      128     /* MDS_MAX_BONES */
#define MDSC_FRAME_FLOATS   13      /* bounds[2], localOrigin, radius, parentOffset */
#define MDSC_POSE_SHORTS    6       /* angles[4] (4th unused), ofsAngles[2] */

/* the size of one decoded frame: an mdsFrame_t with numBones bones */
#define MDSC_FRAME_SIZE( numBones )	( MDSC_FRAME_FLOATS * 4 + ( numBones ) * MDSC_POSE_SHORTS * 2 )

typedef struct {
	int numKeys;
	int ofsKeys;                    /* unsigned short frames[numKeys], padded to 4 */
	int ofsValues;                  /* the key values [numKeys] */
} mdscTrack_t;

typedef struct {
	mdscTrack_t frameTrack;         /* values: float[MDSC_FRAME_FLOATS] (mdsFrame_t's) */
	mdscTrack_t tracks[1];          /* [numBones] rotations: packed quaternions (uint32),
	                                   then [numBones] directions: short[4] (x, y, z, 0) */
} mdscAnim_t;                       /* all offsets are from the start of this */

#define MDSC_DIR_SCALE      32767.0f

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

#ifdef __cplusplus
}
#endif

#endif
