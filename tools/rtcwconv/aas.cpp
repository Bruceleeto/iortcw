/*
 * .aas -> .aasc: what the game still reads of the bot navigation, in the
 * smaller structs of a botlib built with AAS_COMPACT (SP/code/botlib/aasfile.h).
 *
 * With the reachability, clusters and route tables made offline, the only
 * thing the botlib does with faces, edges and vertexes in game is
 * AAS_AgainstLadder, which walks the ladder faces of a ladder area. So an
 * area's faces are only its ladder faces (none for an area that isn't a
 * ladder area), and only the faces, edges and vertexes they use are kept.
 * An area keeps only its center (its bounds and number nothing reads), the
 * area settings and nodes go in shorts and bytes, a reachability loses its
 * face and edge (only an elevator's or func_bob's, and there are none), and the planes are only the
 * pairs the nodes and ladder faces use (a trace flips a plane to its pair,
 * planenum ^ 1, and one that hits nothing is plane 0, so pair 0 stays first).
 * The rest is copied as it is.
 *
 * The file is read back and every value the game reads checked against the
 * .aas; any that doesn't fit, or comes out different, fails the file.
 */
#include <map>
#include <stdio.h>
#include <string.h>

#include "rtcwconv.h"

#define AASID           ( ( 'S' << 24 ) + ( 'A' << 16 ) + ( 'A' << 8 ) + 'E' )
#define AASVERSION      8
#define AASVERSION_DC   101
#define AAS_LUMPS       14

enum {
	LUMP_BBOXES, LUMP_VERTEXES, LUMP_PLANES, LUMP_EDGES, LUMP_EDGEINDEX, LUMP_FACES,
	LUMP_FACEINDEX, LUMP_AREAS, LUMP_AREASETTINGS, LUMP_REACHABILITY, LUMP_NODES,
	LUMP_PORTALS, LUMP_PORTALINDEX, LUMP_CLUSTERS
};

#define FACE_LADDER     2
#define AREA_LADDER     2

#define TRAVEL_ELEVATOR 11
#define TRAVEL_FUNCBOB  19

struct AasLump { int32_t ofs, len; };
struct AasHeader { int32_t ident, version, bspchecksum; AasLump lumps[AAS_LUMPS]; };
struct AasPlane { float normal[3], dist; int32_t type; };
struct AasEdge { int32_t v[2]; };
struct AasFace { int32_t planenum, faceflags, numedges, firstedge, frontarea, backarea; };
struct AasArea { int32_t areanum, numfaces, firstface; float mins[3], maxs[3], center[3]; };
struct AasAreaSettings {
	int32_t contents, areaflags, presencetype, cluster, clusterareanum;
	int32_t numreachableareas, firstreachablearea;
	float groundsteepness;
};
struct AasNode { int32_t planenum, children[2]; };
struct AasReach {
	int32_t areanum, facenum, edgenum;
	float start[3], end[3];
	int32_t traveltype;
	uint16_t traveltime, pad;
};

/* the AAS_COMPACT structs */
struct DcArea { float center[3]; uint16_t firstface, numfaces; };
struct DcAreaSettings {
	int32_t contents, firstreachablearea;
	float groundsteepness;
	uint16_t areaflags;
	int16_t cluster;
	uint16_t clusterareanum;
	uint8_t presencetype, numreachableareas;
};
struct DcNode { uint16_t planenum; int16_t children[2]; };
/* no face or edge: only an elevator's or func_bob's reachability has them
   (their mover), and there are none (ConvertAas) */
struct DcReach {
	int32_t areanum;
	float start[3], end[3];
	uint16_t traveltime;
	uint8_t traveltype, pad;
};
static_assert( sizeof( DcArea ) == 16 && sizeof( DcAreaSettings ) == 20 && sizeof( DcNode ) == 6
			   && sizeof( AasReach ) == 44 && sizeof( DcReach ) == 32, "the botlib's AAS_COMPACT structs" );

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

template <class T> static bool Same( const T &a, const T &b ) {
	return !memcmp( &a, &b, sizeof( T ) );
}

/* the .aasc read back as the game reads it, checked against the .aas */
static bool Check( const std::vector<uint8_t> &out, const std::vector<AasPlane> &planes,
				   const std::vector<float> &vertexes, const std::vector<AasEdge> &edges,
				   const std::vector<int32_t> &edgeindex, const std::vector<AasFace> &faces,
				   const std::vector<int32_t> &faceindex, const std::vector<AasArea> &areas,
				   const std::vector<AasAreaSettings> &settings, const std::vector<AasNode> &nodes,
				   const std::vector<AasReach> &reach, const char *name ) {
	AasHeader h;
	memcpy( &h, out.data(), sizeof( h ) );
	if ( h.ident != AASID || h.version != AASVERSION_DC ) {
		fprintf( stderr, "%s: .aasc check: bad header\n", name );
		return false;
	}
	Scramble( h );
	std::vector<AasPlane> dPlanes;
	std::vector<float> dVertexes;
	std::vector<AasEdge> dEdges;
	std::vector<int32_t> dEdgeindex, dFaceindex;
	std::vector<AasFace> dFaces;
	std::vector<DcArea> dAreas;
	std::vector<DcAreaSettings> dSettings;
	std::vector<DcNode> dNodes;
	std::vector<DcReach> dReach;
	if ( !Lump( out, h, LUMP_PLANES, dPlanes ) || !Lump( out, h, LUMP_VERTEXES, dVertexes ) ||
		 !Lump( out, h, LUMP_EDGES, dEdges ) || !Lump( out, h, LUMP_EDGEINDEX, dEdgeindex ) ||
		 !Lump( out, h, LUMP_FACES, dFaces ) || !Lump( out, h, LUMP_FACEINDEX, dFaceindex ) ||
		 !Lump( out, h, LUMP_AREAS, dAreas ) || !Lump( out, h, LUMP_AREASETTINGS, dSettings ) ||
		 !Lump( out, h, LUMP_NODES, dNodes ) || !Lump( out, h, LUMP_REACHABILITY, dReach ) ||
		 dAreas.size() != areas.size() || dSettings.size() != settings.size() || dNodes.size() != nodes.size() ||
		 dReach.size() != reach.size() ) {
		fprintf( stderr, "%s: .aasc check: bad lumps\n", name );
		return false;
	}
	auto fail = [&]( const char *what, size_t i ) {
		fprintf( stderr, "%s: .aasc check: %s %zu differs\n", name, what, i );
		return false;
	};
	/* a plane and its pair, as a node or face uses it */
	auto samePlane = [&]( int d, int o ) {
		return d >= 0 && (size_t)( d | 1 ) < dPlanes.size() && o >= 0 && (size_t)( o | 1 ) < planes.size()
			   && ( d & 1 ) == ( o & 1 ) && Same( dPlanes[d], planes[o] ) && Same( dPlanes[d ^ 1], planes[o ^ 1] );
	};
	if ( !samePlane( 0, 0 ) ) {
		return fail( "plane", 0 );
	}
	for ( size_t i = 0; i < nodes.size(); i++ ) {
		if ( dNodes[i].children[0] != nodes[i].children[0] || dNodes[i].children[1] != nodes[i].children[1]
			 || !samePlane( dNodes[i].planenum, nodes[i].planenum ) ) {
			return fail( "node", i );
		}
	}
	for ( size_t i = 0; i < settings.size(); i++ ) {
		const AasAreaSettings &o = settings[i];
		const DcAreaSettings &d = dSettings[i];
		if ( d.contents != o.contents || d.areaflags != o.areaflags || d.presencetype != o.presencetype
			 || d.cluster != o.cluster || d.clusterareanum != o.clusterareanum
			 || d.numreachableareas != o.numreachableareas || d.firstreachablearea != o.firstreachablearea
			 || memcmp( &d.groundsteepness, &o.groundsteepness, sizeof( float ) ) ) {
			return fail( "area settings", i );
		}
	}
	for ( size_t i = 0; i < reach.size(); i++ ) {
		const AasReach &o = reach[i];
		const DcReach &d = dReach[i];
		if ( d.areanum != o.areanum || d.traveltype != o.traveltype || d.traveltime != o.traveltime
			 || memcmp( d.start, o.start, sizeof( d.start ) ) || memcmp( d.end, o.end, sizeof( d.end ) ) ) {
			return fail( "reachability", i );
		}
	}
	/* an area's ladder faces, in order: plane, flags and every edge's ends */
	for ( size_t a = 0; a < areas.size(); a++ ) {
		const DcArea &d = dAreas[a];
		if ( memcmp( d.center, areas[a].center, sizeof( d.center ) ) ) {
			return fail( "area", a );
		}
		std::vector<int32_t> want;
		if ( settings[a].areaflags & AREA_LADDER ) {
			for ( int i = 0; i < areas[a].numfaces; i++ ) {
				int f = faceindex[areas[a].firstface + i];
				if ( faces[f < 0 ? -f : f].faceflags & FACE_LADDER ) {
					want.push_back( f );
				}
			}
		}
		if ( d.numfaces != want.size() || (size_t)d.firstface + d.numfaces > dFaceindex.size() ) {
			return fail( "area's faces", a );
		}
		for ( size_t i = 0; i < want.size(); i++ ) {
			int df = dFaceindex[d.firstface + i], of = want[i];
			if ( ( df < 0 ) != ( of < 0 ) || df == 0 || (size_t)( df < 0 ? -df : df ) >= dFaces.size() ) {
				return fail( "area's face", a );
			}
			const AasFace &fd = dFaces[df < 0 ? -df : df], &fo = faces[of < 0 ? -of : of];
			if ( fd.faceflags != fo.faceflags || fd.numedges != fo.numedges || fd.frontarea != fo.frontarea
				 || fd.backarea != fo.backarea || !samePlane( fd.planenum, fo.planenum )
				 || fd.firstedge < 0 || (size_t)fd.firstedge + fd.numedges > dEdgeindex.size() ) {
				return fail( "area's face", a );
			}
			for ( int e = 0; e < fd.numedges; e++ ) {
				int ed = dEdgeindex[fd.firstedge + e], eo = edgeindex[fo.firstedge + e];
				if ( ( ed < 0 ) != ( eo < 0 ) || (size_t)( ed < 0 ? -ed : ed ) >= dEdges.size() ) {
					return fail( "area's face edge", a );
				}
				const AasEdge &xd = dEdges[ed < 0 ? -ed : ed], &xo = edges[eo < 0 ? -eo : eo];
				for ( int v = 0; v < 2; v++ ) {
					if ( xd.v[v] < 0 || (size_t)xd.v[v] * 3 + 3 > dVertexes.size()
						 || memcmp( &dVertexes[xd.v[v] * 3], &vertexes[xo.v[v] * 3], 3 * sizeof( float ) ) ) {
						return fail( "area's face vertex", a );
					}
				}
			}
		}
	}
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

	std::vector<AasPlane> planes;
	std::vector<float> vertexes;	/* 3 each */
	std::vector<AasEdge> edges;
	std::vector<int32_t> edgeindex, faceindex;
	std::vector<AasFace> faces;
	std::vector<AasArea> areas;
	std::vector<AasAreaSettings> settings;
	std::vector<AasNode> nodes;
	std::vector<AasReach> reach;
	if ( !Lump( in, h, LUMP_PLANES, planes ) || planes.size() < 2 || planes.size() % 2 ||
		 !Lump( in, h, LUMP_VERTEXES, vertexes ) || vertexes.size() % 3 || !Lump( in, h, LUMP_EDGES, edges ) ||
		 !Lump( in, h, LUMP_EDGEINDEX, edgeindex ) || !Lump( in, h, LUMP_FACES, faces ) ||
		 !Lump( in, h, LUMP_FACEINDEX, faceindex ) || !Lump( in, h, LUMP_AREAS, areas ) ||
		 !Lump( in, h, LUMP_AREASETTINGS, settings ) || settings.size() != areas.size() ||
		 !Lump( in, h, LUMP_NODES, nodes ) || !Lump( in, h, LUMP_REACHABILITY, reach ) ) {
		fprintf( stderr, "%s: bad lumps\n", name );
		return false;
	}
	auto tooBig = [&]( const char *what, size_t i ) {
		fprintf( stderr, "%s: %s %zu doesn't fit the .aasc\n", name, what, i );
		return false;
	};

	/* the reachability without its face and edge: a travel type (no flags)
	   in a byte, and no elevator or func_bob, the only ones with a face and
	   edge read */
	std::vector<DcReach> newReach( reach.size() );
	for ( size_t i = 0; i < reach.size(); i++ ) {
		const AasReach &o = reach[i];
		if ( o.traveltype < 0 || o.traveltype > 0xff || o.traveltype == TRAVEL_ELEVATOR
			 || o.traveltype == TRAVEL_FUNCBOB ) {
			return tooBig( "reachability", i );
		}
		DcReach &d = newReach[i];
		memset( &d, 0, sizeof( d ) );
		d.areanum = o.areanum;
		memcpy( d.start, o.start, sizeof( d.start ) );
		memcpy( d.end, o.end, sizeof( d.end ) );
		d.traveltime = o.traveltime;
		d.traveltype = (uint8_t)o.traveltype;
	}

	/* the plane pairs used, pair 0 first: a plane's new number */
	std::map<int, int> pairMap;
	pairMap[0] = 0;
	auto plane = [&]( int p ) -> int {
		auto it = pairMap.find( p >> 1 );
		if ( it == pairMap.end() ) {
			int n = pairMap.size();
			it = pairMap.emplace( p >> 1, n ).first;
		}
		return it->second * 2 + ( p & 1 );
	};

	/* index 0 of faces, edges and vertexes stays the dummy */
	std::vector<AasFace> newFaces( 1, AasFace() );
	std::vector<AasEdge> newEdges( 1, AasEdge() );
	std::vector<float> newVertexes( 3, 0.0f );
	std::vector<int32_t> newFaceindex, newEdgeindex;
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
		nf.planenum = plane( faces[f].planenum );
		nf.firstedge = newEdgeindex.size();
		for ( int i = 0; i < faces[f].numedges; i++ ) {
			int e = edgeindex[faces[f].firstedge + i];
			newEdgeindex.push_back( e < 0 ? -edge( -e ) : edge( e ) );
		}
		newFaces.push_back( nf );
		return faceMap[f] = newFaces.size() - 1;
	};

	std::vector<DcNode> newNodes( nodes.size() );
	for ( size_t i = 0; i < nodes.size(); i++ ) {
		const AasNode &n = nodes[i];
		if ( n.planenum < 0 || (size_t)n.planenum >= planes.size() ) {
			return tooBig( "node", i );
		}
		int p = plane( n.planenum );
		if ( p > 0xffff || n.children[0] != (int16_t)n.children[0] || n.children[1] != (int16_t)n.children[1] ) {
			return tooBig( "node", i );
		}
		newNodes[i] = { (uint16_t)p, { (int16_t)n.children[0], (int16_t)n.children[1] } };
	}

	std::vector<DcArea> newAreas( areas.size() );
	std::vector<DcAreaSettings> newSettings( areas.size() );
	for ( size_t a = 0; a < areas.size(); a++ ) {
		const AasArea &area = areas[a];
		DcArea &na = newAreas[a];
		memcpy( na.center, area.center, sizeof( na.center ) );
		size_t first = newFaceindex.size();
		if ( settings[a].areaflags & AREA_LADDER ) {
			for ( int i = 0; i < area.numfaces; i++ ) {
				int f = faceindex[area.firstface + i];
				if ( faces[f < 0 ? -f : f].faceflags & FACE_LADDER ) {
					newFaceindex.push_back( f < 0 ? -face( -f ) : face( f ) );
				}
			}
		}
		if ( newFaceindex.size() > 0xffff ) {
			return tooBig( "area", a );
		}
		na.firstface = newFaceindex.size() == first ? 0 : first;
		na.numfaces = newFaceindex.size() - first;

		const AasAreaSettings &s = settings[a];
		if ( s.areaflags < 0 || s.areaflags > 0xffff || s.presencetype < 0 || s.presencetype > 0xff
			 || s.cluster != (int16_t)s.cluster || s.clusterareanum < 0 || s.clusterareanum > 0xffff
			 || s.numreachableareas < 0 || s.numreachableareas > 0xff ) {
			return tooBig( "area settings", a );
		}
		DcAreaSettings &ns = newSettings[a];
		memset( &ns, 0, sizeof( ns ) );
		ns.contents = s.contents;
		ns.firstreachablearea = s.firstreachablearea;
		ns.groundsteepness = s.groundsteepness;
		ns.areaflags = s.areaflags;
		ns.cluster = s.cluster;
		ns.clusterareanum = s.clusterareanum;
		ns.presencetype = s.presencetype;
		ns.numreachableareas = s.numreachableareas;
	}

	/* the planes by their new numbers */
	std::vector<AasPlane> newPlanes( pairMap.size() * 2 );
	for ( const auto &pm : pairMap ) {
		newPlanes[pm.second * 2] = planes[pm.first * 2];
		newPlanes[pm.second * 2 + 1] = planes[pm.first * 2 + 1];
	}

	/* the lumps in the order they were, the changed ones swapped in */
	struct Data { const void *p; size_t len; };
	Data data[AAS_LUMPS];
	for ( int n = 0; n < AAS_LUMPS; n++ ) {
		data[n].p = in.data() + h.lumps[n].ofs;
		data[n].len = h.lumps[n].len;
	}
	data[LUMP_VERTEXES] = { newVertexes.data(), newVertexes.size() * sizeof( float ) };
	data[LUMP_PLANES] = { newPlanes.data(), newPlanes.size() * sizeof( AasPlane ) };
	data[LUMP_EDGES] = { newEdges.data(), newEdges.size() * sizeof( AasEdge ) };
	data[LUMP_EDGEINDEX] = { newEdgeindex.data(), newEdgeindex.size() * sizeof( int32_t ) };
	data[LUMP_FACES] = { newFaces.data(), newFaces.size() * sizeof( AasFace ) };
	data[LUMP_FACEINDEX] = { newFaceindex.data(), newFaceindex.size() * sizeof( int32_t ) };
	data[LUMP_AREAS] = { newAreas.data(), newAreas.size() * sizeof( DcArea ) };
	data[LUMP_AREASETTINGS] = { newSettings.data(), newSettings.size() * sizeof( DcAreaSettings ) };
	data[LUMP_NODES] = { newNodes.data(), newNodes.size() * sizeof( DcNode ) };
	data[LUMP_REACHABILITY] = { newReach.data(), newReach.size() * sizeof( DcReach ) };

	AasHeader oh = h;
	oh.version = AASVERSION_DC;
	out.assign( sizeof( oh ), 0 );
	for ( int n = 0; n < AAS_LUMPS; n++ ) {
		oh.lumps[n].ofs = out.size();
		oh.lumps[n].len = data[n].len;
		out.insert( out.end(), (const uint8_t *)data[n].p, (const uint8_t *)data[n].p + data[n].len );
	}
	Scramble( oh );
	memcpy( out.data(), &oh, sizeof( oh ) );

	if ( !Check( out, planes, vertexes, edges, edgeindex, faces, faceindex, areas, settings, nodes, reach, name ) ) {
		return false;
	}

	st.files++;
	st.bytesIn += in.size();
	st.bytesOut += out.size();
	st.facesIn += faces.size();
	st.facesOut += newFaces.size();
	st.planesIn += planes.size();
	st.planesOut += newPlanes.size();
	return true;
}
