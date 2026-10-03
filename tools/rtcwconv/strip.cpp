/*
 * Triangle strips, from strippy's optimize_mesh / join_strips.
 *
 * RtCW models are drawn as triangle lists, so the strips are written back as
 * triangles in strip order: each triangle shares an edge with the one before
 * it, which a PVR backend can send as one strip.
 */
#include <algorithm>
#include <array>
#include <map>
#include <stdio.h>
#include <string.h>

#include "rtcwconv.h"
#include "tri_stripper.h"

static bool CanJoinStrips( const std::vector<size_t> &a, const std::vector<size_t> &b ) {
	if ( a.size() < 3 || b.size() < 3 ) {
		return false;
	}
	/* only when edges match in the same order (winding compatible) */
	return a[a.size() - 1] == b[1] && a[a.size() - 2] == b[0];
}

static std::vector<std::vector<size_t>> JoinStrips( const triangle_stripper::primitive_vector &prims ) {
	std::vector<std::vector<size_t>> strips, result;

	for ( const auto &p : prims ) {
		if ( p.Type == triangle_stripper::TRIANGLE_STRIP ) {
			strips.push_back( std::vector<size_t>( p.Indices.begin(), p.Indices.end() ) );
		}
	}

	std::vector<bool> used( strips.size(), false );
	for ( size_t i = 0; i < strips.size(); i++ ) {
		if ( used[i] ) {
			continue;
		}
		std::vector<size_t> cur = strips[i];
		used[i] = true;

		bool joined;
		do {
			joined = false;
			for ( size_t j = 0; j < strips.size(); j++ ) {
				if ( !used[j] && CanJoinStrips( cur, strips[j] ) ) {
					cur.insert( cur.end(), strips[j].begin() + 2, strips[j].end() );
					used[j] = true;
					joined = true;
					break;
				}
			}
		} while ( joined );

		result.push_back( cur );
	}
	return result;
}

/* a triangle, turned so its smallest index comes first: the same triangle
 * wound the same way always gives the same key */
static std::array<uint32_t, 3> TriKey( uint32_t a, uint32_t b, uint32_t c ) {
	if ( b < a && b < c ) {
		return { b, c, a };
	}
	if ( c < a && c < b ) {
		return { c, a, b };
	}
	return { a, b, c };
}

int StripOrder( std::vector<uint32_t> &tris, int *stripTris ) {
	using namespace triangle_stripper;

	*stripTris = 0;
	if ( tris.size() < 6 ) {
		return 0;
	}

	indices in( tris.begin(), tris.end() );
	primitive_vector prims;
	tri_stripper stripper( in );
	stripper.SetMinStripSize( 0 );
	stripper.SetCacheSize( 0 );
	stripper.SetBackwardSearch( true );
	stripper.SetPushCacheHits( true );
	stripper.Strip( &prims );

	std::vector<std::vector<size_t>> strips = JoinStrips( prims );

	std::vector<uint32_t> out;
	out.reserve( tris.size() );
	for ( const auto &s : strips ) {
		for ( size_t i = 0; i + 2 < s.size(); i++ ) {
			/* odd triangles of a strip are wound the other way */
			if ( i & 1 ) {
				out.insert( out.end(), { (uint32_t)s[i + 1], (uint32_t)s[i], (uint32_t)s[i + 2] } );
			} else {
				out.insert( out.end(), { (uint32_t)s[i], (uint32_t)s[i + 1], (uint32_t)s[i + 2] } );
			}
			( *stripTris )++;
		}
	}
	for ( const auto &p : prims ) {
		if ( p.Type == TRIANGLES ) {
			out.insert( out.end(), p.Indices.begin(), p.Indices.end() );
		}
	}

	/* the same triangles, wound the same way, or keep the original order */
	std::map<std::array<uint32_t, 3>, int> count;
	for ( size_t i = 0; i < tris.size(); i += 3 ) {
		count[TriKey( tris[i], tris[i + 1], tris[i + 2] )]++;
	}
	for ( size_t i = 0; i < out.size(); i += 3 ) {
		auto it = count.find( TriKey( out[i], out[i + 1], out[i + 2] ) );
		if ( it == count.end() || it->second == 0 ) {
			*stripTris = 0;
			return -1;
		}
		it->second--;
	}
	if ( out.size() != tris.size() ) {
		*stripTris = 0;
		return -1;
	}

	tris = out;
	return (int)strips.size();
}

std::vector<uint16_t> StripIndexes( const std::vector<uint32_t> &tris, long *strips ) {
	std::vector<uint16_t> out;
	uint32_t last[3] = { ~0u, ~0u, ~0u };
	bool odd = false;

	for ( size_t t = 0; t + 2 < tris.size(); t += 3 ) {
		uint32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
		bool on = odd ? a == last[0] && b == last[2] : a == last[2] && b == last[1];
		odd = on ? !odd : false;
		if ( !on ) {
			out.insert( out.end(), { (uint16_t)( a | 0x8000 ), (uint16_t)b } );
			( *strips )++;
		}
		out.push_back( (uint16_t)c );
		last[0] = a;
		last[1] = b;
		last[2] = c;
	}
	return out;
}

template <typename T> static T GetAt( const std::vector<uint8_t> &b, size_t at ) {
	T v;
	memcpy( &v, &b[at], sizeof( v ) );
	return v;
}

bool StripModel( std::vector<uint8_t> &b, bool mdc, ModelStripStats &st ) {
	/* where they are in the header and a surface: md3Header_t / mdcHeader_t,
	   md3Surface_t / mdcSurface_t */
	const size_t hNumSurfaces = 84, hOfsSurfaces = mdc ? 104 : 100;
	const size_t sNumVerts = mdc ? 84 : 80, sNumTris = mdc ? 88 : 84, sOfsTris = mdc ? 92 : 88;
	const size_t sOfsEnd = mdc ? 120 : 104, sSize = mdc ? 124 : 108;

	if ( b.size() < hOfsSurfaces + 8 ) {
		return false;
	}
	int numSurfaces = GetAt<int>( b, hNumSurfaces );
	size_t surf = GetAt<int>( b, hOfsSurfaces );
	for ( int i = 0; i < numSurfaces; i++ ) {
		if ( surf + sSize > b.size() ) {
			return false;
		}
		int numVerts = GetAt<int>( b, surf + sNumVerts ), numTris = GetAt<int>( b, surf + sNumTris );
		int ofsTris = GetAt<int>( b, surf + sOfsTris ), ofsEnd = GetAt<int>( b, surf + sOfsEnd );
		if ( numTris < 0 || ofsTris < 0 || ofsEnd <= 0 || surf + ofsEnd > b.size() ||
			 (size_t)ofsTris + numTris * 12 > (size_t)ofsEnd ) {
			return false;
		}
		std::vector<uint32_t> tris( numTris * 3 );
		memcpy( tris.data(), &b[surf + ofsTris], tris.size() * 4 );
		for ( uint32_t v : tris ) {
			if ( v >= (uint32_t)numVerts ) {
				return false;
			}
		}
		int stripTris;
		int strips = StripOrder( tris, &stripTris );
		if ( strips > 0 ) {
			memcpy( &b[surf + ofsTris], tris.data(), tris.size() * 4 );
			st.strips += strips;
			st.stripTris += stripTris;
		}
		st.tris += numTris;
		surf += ofsEnd;
	}
	st.files++;
	return true;
}
