/* cm_glue.c: the engine's curve collision, called from col.cpp */
#ifndef COL_GLUE_H
#define COL_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Runs CM_GeneratePatchCollide over width x height points (xyz, 3 floats
 * each) and points *out at the patch as the .col stores it, from bounds on
 * (see colfile.h). Returns its size, or -1 if the engine code gave up.
 * *out lives until ColFreePatch. */
int ColGeneratePatch( int width, int height, const float *xyz, unsigned char **out );
void ColFreePatch( void );

#ifdef __cplusplus
}
#endif

#endif
