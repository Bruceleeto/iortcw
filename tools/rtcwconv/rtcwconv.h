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

/* ---- aas.cpp ---- */

struct AasStats {
	int files;
	size_t bytesIn, bytesOut;
	long facesIn, facesOut;
	int skipped;                /* second worlds left empty: no big characters */
};

/* .aas -> .aasc: only the ladder faces (and their edges and vertexes) kept.
 * False (with a message) if it isn't one. */
bool ConvertAas(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, AasStats &st, const char *name);

/* ---- col.cpp ---- */

struct ColStats {
	int files;
	size_t bytesIn, bytesOut;
	int patches, patchesSkipped;
};

/* .bsp -> .col: what the collision code reads, curve collision included.
 * False (with a message) if it isn't a bsp. */
bool ConvertCol(const std::vector<uint8_t> &bsp, std::vector<uint8_t> &out, ColStats &st, const char *name);

/* ---- mdc.cpp ---- */

struct MdcStats {
	int files, converted, kept;
	size_t bytesIn;             /* every .mdc */
	size_t bytesConverted;      /* the ones made into .mdb */
	size_t bytesOut;            /* their .mdb */
	long bones;
	long surfaces, boneSurfaces;
	long frames, keys;          /* surface frames, and those kept */
	double maxErr, sumErr;      /* how far a vertex is off, in units */
	long numErr;
};

/* .mdc -> .mdb: its animated surfaces as rigid bones, or its own vertexes,
 * at the frames that can't be lerped. out is left empty when
 * the .mdc is better kept (nothing moves, or bones don't fit or save).
 * False (with a message) if it isn't an .mdc. */
bool ConvertMdc(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, MdcStats &st, const char *name);

/* ---- wld.cpp ---- */

struct WldStats {
	int files;
	size_t bytesIn, bytesOut;
	long surfacesIn, surfacesOut, verts, triangles;
	size_t gridIn, gridOut;
	long dropped;                   /* surfaces left out by a map edit */
};

/* .bsp -> .wld: the surfaces ready to draw, curves cut at r_subdivisions
 * subdivisions, but for those whose shader begins with one of drop. False
 * (with a message) if it isn't a bsp. */
bool ConvertWld(const std::vector<uint8_t> &bsp, std::vector<uint8_t> &out, WldStats &st, const char *name, float subdivisions,
				const std::vector<std::string> &drop);

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

/* ---- shaders.cpp ---- */

struct ShaderStats {
	int files, shaders, kept;
	size_t bytesIn, bytesOut;
};

/* the words in a file that may name a shader */
void ShaderNames(const uint8_t *data, size_t size);
/* a .shader file's shaders (fileName as the renderer lists it) */
bool ShaderFile(const std::string &fileName, const std::vector<uint8_t> &data, ShaderStats &st);
/* scripts/dc.shaders: the shaders named, of all the files given; empty
 * with no .shader files */
void WriteShaders(std::vector<uint8_t> &data, ShaderStats &st);

#endif
