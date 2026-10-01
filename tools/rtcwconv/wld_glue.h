/* wld_glue.c: the renderer's curve code, called from wld.cpp */
#ifndef WLD_GLUE_H
#define WLD_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	float xyz[3];
	float st[2];
	float lightmap[2];
	float normal[3];
	unsigned char color[4];
} WldPatchVert;

typedef struct {
	int width, height;
	const WldPatchVert *verts;
} WldPatch;

/* Cuts every curve into a grid of points as the renderer does
 * (R_SubdividePatchToGrid at r_subdivisions subdivisionSize), then stitches
 * the grids that share an edge (R_StitchAllPatches). */
void WldCutCurves( int numPatches, const WldPatch *patches, float subdivisionSize );

/* Curve n's grid, width x height points row by row; *verts lives until the
 * next call */
void WldCurve( int n, int *width, int *height, const WldPatchVert **verts );

void WldFreeCurves( void );

#ifdef __cplusplus
}
#endif

#endif
