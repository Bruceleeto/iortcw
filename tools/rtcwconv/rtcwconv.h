/*
 * rtcwconv: converts RtCW assets into compact Dreamcast versions.
 *
 * Grown from strippy, the DMS-Engine converter
 * (DMS-Engine-next/converter): its tri stripping core and keyframe idea are
 * kept, its glTF side is replaced by RtCW's own formats.
 */
#ifndef RTCWCONV_H
#define RTCWCONV_H

#include <filesystem>
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

/* ---- tex.cpp ---- */

struct TexJob {
	std::string rel;            /* as the game names it, for messages */
	std::string in;             /* the image file, or: */
	std::vector<uint8_t> rgba;  /* the image itself, w x h */
	int w, h;
	bool noMip;
	std::filesystem::path out;  /* the .dt */
};

struct TexOptions {
	std::string pvrtex;         /* the pvrtex binary */
	int maxSize;                /* longest side kept */
	int maxSize2D;              /* the same for 2D art (menus, fonts, HUD) */
	int jobs;                   /* pvrtex runs at once, 0: a core each */
	bool verbose;
};

struct TexStats {
	int files;
	size_t bytesIn;             /* as 16 bit texels */
	size_t bytesOut;
};

/* ---- bsp.cpp ---- */

/* the lightmap shift the renderer does on PVR with the default cvars
 * (r_mapOverBrightBits 2, no hardware gamma), baked into the .dt */
#define LIGHTMAP_DT_SHIFT 2

/* A job for each lightmap of the .bsp, out to <outDir>/maps/<map>/lm_NNNN.dt.
 * False (with a message) if it isn't a bsp. */
bool BspLightmapJobs(const std::vector<uint8_t> &bsp, const std::string &mapName,
                     const std::filesystem::path &outDir, std::vector<TexJob> &jobs);

/* Converts each image to its .dt. Returns how many failed. */
int ConvertTextures(const std::vector<TexJob> &jobs, const TexOptions &opt, TexStats &st);

#endif
