/*
 * .aas -> .aasc: the same file with the 3d boundary of the areas cut down to
 * what the game still reads.
 *
 * With the reachability, clusters and route tables made offline, the only
 * thing the botlib does with faces, edges and vertexes in game is
 * AAS_AgainstLadder, which walks the ladder faces of a ladder area. So only
 * the ladder faces of ladder areas, and the edges and vertexes they use, are
 * kept; every other face index points at the dummy face 0, which has no
 * flags. The areas stay as they are: the route cache (.rcd) checks them by
 * CRC. Planes, nodes and the rest are copied as they are.
 */
#include <map>
#include <stdio.h>
#include <string.h>

#include "rtcwconv.h"

#define AASID           ( ( 'S' << 24 ) + ( 'A' << 16 ) + ( 'A' << 8 ) + 'E' )
#define AASVERSION      8
#define AAS_LUMPS       14

enum {
	LUMP_BBOXES, LUMP_VERTEXES, LUMP_PLANES, LUMP_EDGES, LUMP_EDGEINDEX, LUMP_FACES,
	LUMP_FACEINDEX, LUMP_AREAS, LUMP_AREASETTINGS, LUMP_REACHABILITY, LUMP_NODES,
	LUMP_PORTALS, LUMP_PORTALINDEX, LUMP_CLUSTERS
};

#define FACE_LADDER     2
#define AREA_LADDER     2

struct AasLump { int32_t ofs, len; };
struct AasHeader { int32_t ident, version, bspchecksum; AasLump lumps[AAS_LUMPS]; };
struct AasEdge { int32_t v[2]; };
struct AasFace { int32_t planenum, faceflags, numedges, firstedge, frontarea, backarea; };
struct AasArea { int32_t areanum, numfaces, firstface; float mins[3], maxs[3], center[3]; };
struct AasAreaSettings {
	int32_t contents, areaflags, presencetype, cluster, clusterareanum;
	int32_t numreachableareas, firstreachablearea;
	float groundsteepness;
};

/* the header past ident and version is xored, as the game reads it */
static void Scramble( AasHeader &h ) {
	uint8_t *p = (uint8_t *)&h + 8;
	for ( size_t i = 0; i < sizeof( h ) - 8; i++ ) {
		p[i] ^= (uint8_t)( i * 119 );
	}
}

template <class T> static bool Lump( const std::vector<uint8_t> &in, const AasHeader &h, int n,
									 std::vector<T> &out ) {
	const AasLump &l = h.lumps[n];
	if ( l.ofs < 0 || l.len < 0 || (size_t)l.ofs + l.len > in.size() || l.len % sizeof( T ) ) {
		return false;
	}
	out.resize( l.len / sizeof( T ) );
	memcpy( out.data(), in.data() + l.ofs, l.len );
	return true;
}

bool ConvertAas( const std::vector<uint8_t> &in, std::vector<uint8_t> &out, AasStats &st, const char *name ) {
	AasHeader h;
	if ( in.size() < sizeof( h ) ) {
		fprintf( stderr, "%s: not an aas file\n", name );
		return false;
	}
	memcpy( &h, in.data(), sizeof( h ) );
	if ( h.ident != AASID || h.version != AASVERSION ) {
		fprintf( stderr, "%s: not a version %d aas file\n", name, AASVERSION );
		return false;
	}
	Scramble( h );

	std::vector<float> vertexes;	/* 3 each */
	std::vector<AasEdge> edges;
	std::vector<int32_t> edgeindex, faceindex;
	std::vector<AasFace> faces;
	std::vector<AasArea> areas;
	std::vector<AasAreaSettings> settings;
	if ( !Lump( in, h, LUMP_VERTEXES, vertexes ) || vertexes.size() % 3 || !Lump( in, h, LUMP_EDGES, edges ) ||
		 !Lump( in, h, LUMP_EDGEINDEX, edgeindex ) || !Lump( in, h, LUMP_FACES, faces ) ||
		 !Lump( in, h, LUMP_FACEINDEX, faceindex ) || !Lump( in, h, LUMP_AREAS, areas ) ||
		 !Lump( in, h, LUMP_AREASETTINGS, settings ) || settings.size() != areas.size() ) {
		fprintf( stderr, "%s: bad lumps\n", name );
		return false;
	}

	/* index 0 of faces, edges and vertexes stays the dummy */
	std::vector<AasFace> newFaces( 1, AasFace() );
	std::vector<AasEdge> newEdges( 1, AasEdge() );
	std::vector<float> newVertexes( 3, 0.0f );
	std::vector<int32_t> newFaceindex( faceindex.size(), 0 ), newEdgeindex;
	std::map<int, int> faceMap, edgeMap, vertexMap;

	auto vertex = [&]( int v ) -> int32_t {
		auto it = vertexMap.find( v );
		if ( it != vertexMap.end() ) {
			return it->second;
		}
		int n = newVertexes.size() / 3;
		newVertexes.insert( newVertexes.end(), &vertexes[v * 3], &vertexes[v * 3] + 3 );
		vertexMap[v] = n;
		return n;
	};
	auto edge = [&]( int e ) -> int32_t {
		auto it = edgeMap.find( e );
		if ( it != edgeMap.end() ) {
			return it->second;
		}
		AasEdge ne = { { vertex( edges[e].v[0] ), vertex( edges[e].v[1] ) } };
		newEdges.push_back( ne );
		return edgeMap[e] = newEdges.size() - 1;
	};
	auto face = [&]( int f ) -> int32_t {
		auto it = faceMap.find( f );
		if ( it != faceMap.end() ) {
			return it->second;
		}
		AasFace nf = faces[f];
		nf.firstedge = newEdgeindex.size();
		for ( int i = 0; i < faces[f].numedges; i++ ) {
			int e = edgeindex[faces[f].firstedge + i];
			newEdgeindex.push_back( e < 0 ? -edge( -e ) : edge( e ) );
		}
		newFaces.push_back( nf );
		return faceMap[f] = newFaces.size() - 1;
	};

	for ( size_t a = 0; a < areas.size(); a++ ) {
		const AasArea &area = areas[a];
		if ( !( settings[a].areaflags & AREA_LADDER ) ) {
			continue;
		}
		for ( int i = 0; i < area.numfaces; i++ ) {
			int f = faceindex[area.firstface + i];
			if ( faces[f < 0 ? -f : f].faceflags & FACE_LADDER ) {
				newFaceindex[area.firstface + i] = f < 0 ? -face( -f ) : face( f );
			}
		}
	}

	/* the lumps in the order they were, the changed ones swapped in */
	struct Data { const void *p; size_t len; };
	Data data[AAS_LUMPS];
	for ( int n = 0; n < AAS_LUMPS; n++ ) {
		data[n].p = in.data() + h.lumps[n].ofs;
		data[n].len = h.lumps[n].len;
	}
	data[LUMP_VERTEXES] = { newVertexes.data(), newVertexes.size() * sizeof( float ) };
	data[LUMP_EDGES] = { newEdges.data(), newEdges.size() * sizeof( AasEdge ) };
	data[LUMP_EDGEINDEX] = { newEdgeindex.data(), newEdgeindex.size() * sizeof( int32_t ) };
	data[LUMP_FACES] = { newFaces.data(), newFaces.size() * sizeof( AasFace ) };
	data[LUMP_FACEINDEX] = { newFaceindex.data(), newFaceindex.size() * sizeof( int32_t ) };

	AasHeader oh = h;
	out.assign( sizeof( oh ), 0 );
	for ( int n = 0; n < AAS_LUMPS; n++ ) {
		oh.lumps[n].ofs = out.size();
		oh.lumps[n].len = data[n].len;
		out.insert( out.end(), (const uint8_t *)data[n].p, (const uint8_t *)data[n].p + data[n].len );
	}
	Scramble( oh );
	memcpy( out.data(), &oh, sizeof( oh ) );

	st.files++;
	st.bytesIn += in.size();
	st.bytesOut += out.size();
	st.facesIn += faces.size();
	st.facesOut += newFaces.size();
	return true;
}
