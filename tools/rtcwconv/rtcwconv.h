/*
 * rtcwconv: converts RtCW assets into compact Dreamcast versions.
 *
 * Grown from strippy, the DMS-Engine converter
 * (DMS-Engine-next/converter): its tri stripping core and keyframe idea are
 * kept, its glTF side is replaced by RtCW's own formats.
 */
#ifndef RTCWCONV_H
#define RTCWCONV_H

#include <stdint.h>
#include <string>
#include <vector>

/* ---- strip.cpp ---- */

/* Reorders a triangle list (3 indices each) so triangles that make a strip
 * follow each other, each still wound as it was. Returns the number of strips
 * found, or -1 (and leaves tris alone) if the result didn't check out. */
int StripOrder(std::vector<uint32_t> &tris, int *stripTris);

/* ---- mds.cpp ---- */

struct MdsStats {
	int files;
	size_t bytesIn, bytesOut;
	long framesIn, keysOut;     /* bone poses before / rotation keys after */
	long dirKeys;
	int strips, stripTris, tris;
	float maxErr;               /* furthest a bone moved, in units */
	float maxAngle;             /* most a bone turned, in degrees */
	double sumErr;
	long numErr;
};

struct MdsOptions {
	float angleTol;             /* most a bone may turn against its parent, in degrees */
	float offsetTol;            /* most frame bounds / offsets may move, in units */
	int maxSpan;                /* most frames between two keys */
};

/* Converts one .mds file. False (with a message) if it isn't one. */
bool ConvertMds(const std::vector<uint8_t> &in, std::vector<uint8_t> &out,
                const MdsOptions &opt, MdsStats &st, const char *name);

#endif
