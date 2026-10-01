/*
 * scripts/dc.shaders: of every .shader file, only the shaders something
 * names, compressed as the renderer would (COM_Compress), in one file that
 * the renderer loads in place of scanning them all. The renderer otherwise
 * keeps the text of every shader there is.
 *
 * A shader is kept when its name is a word in the maps (entities and
 * shader lump), models, scripts, skins, menus and the like, or in the game's
 * own source; a source string with a % in it (a name made at run time, like
 * "gfx/2d/numbers/%s", or "textures/%s" with a map's key) keeps the shaders
 * it begins whose rest is named somewhere too, or is short (a number); not
 * when it goes on to a path or a file (models/players/%s/head.md3 names no
 * shader).
 */
#include <algorithm>
#include <map>
#include <set>
#include <stdio.h>
#include <string.h>

#include "rtcwconv.h"

namespace {

struct Shader {
	std::string name, text;     /* text: "name { ... }", compressed */
};

std::set<std::string> words;
std::vector<std::string> prefixes;
std::map<std::string, std::vector<Shader>> files;   /* by file name, as the renderer lists them */

bool WordChar( char c ) {
	return ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '/' || c == '.' || c == '-' || c == '%';
}

/* comments out, whitespace down to one space or newline, as COM_Compress:
 * a shader's parameters are read a line at a time */
std::string Compress( const std::string &in ) {
	std::string out;
	size_t i = 0;
	char space = 0;     /* ' ' or '\n' to put before the next word */
	auto gap = [&]( char c ) {
		if ( c == '\n' || !space ) {
			space = c;
		}
	};
	auto word = [&]() {
		if ( space && !out.empty() ) {
			out += space;
		}
		space = 0;
	};
	while ( i < in.size() ) {
		char c = in[i];
		if ( c == '/' && i + 1 < in.size() && in[i + 1] == '/' ) {
			while ( i < in.size() && in[i] != '\n' ) {
				i++;
			}
		} else if ( c == '/' && i + 1 < in.size() && in[i + 1] == '*' ) {
			i += 2;
			while ( i + 1 < in.size() && !( in[i] == '*' && in[i + 1] == '/' ) ) {
				gap( in[i] == '\n' ? '\n' : ' ' );
				i++;
			}
			i += 2;
			gap( ' ' );
		} else if ( (unsigned char)c <= ' ' ) {
			gap( c == '\n' ? '\n' : ' ' );
			i++;
		} else if ( c == '"' ) {
			word();
			size_t end = in.find( '"', i + 1 );
			end = end == std::string::npos ? in.size() : end + 1;
			out.append( in, i, end - i );
			i = end;
		} else {
			word();
			out += c;
			i++;
		}
	}
	return out;
}

}

void ShaderNames( const uint8_t *data, size_t size ) {
	std::string w;
	for ( size_t i = 0; i <= size; i++ ) {
		char c = i < size ? tolower( data[i] ) : 0;
		if ( i < size && WordChar( c ) ) {
			w += c;
			continue;
		}
		if ( w.size() >= 4 ) {
			size_t pct = w.find( '%' );
			if ( pct != std::string::npos ) {
				if ( pct >= 4 && w.find( '/' ) < pct && w.find_first_of( "/.", pct ) == std::string::npos ) {
					prefixes.push_back( w.substr( 0, pct ) );
				}
			} else {
				words.insert( w );
				size_t dot = w.rfind( '.' );
				if ( dot != std::string::npos && w.find( '/', dot ) == std::string::npos ) {
					words.insert( w.substr( 0, dot ) );
				}
			}
		}
		w.clear();
	}
}

/* top level "name { ... }"s, as the renderer finds them; a file with a name
 * not followed by a brace is left out whole, and one with a stray } ends
 * there, as the renderer does */
bool ShaderFile( const std::string &fileName, const std::vector<uint8_t> &data, ShaderStats &st ) {
	std::string text = Compress( std::string( data.begin(), data.end() ) );
	std::vector<Shader> shaders;
	size_t i = 0;
	st.files++;
	st.bytesIn += data.size();
	while ( i < text.size() ) {
		while ( i < text.size() && ( text[i] == ' ' || text[i] == '\n' ) ) {
			i++;
		}
		if ( i >= text.size() ) {
			break;
		}
		size_t start = i;
		while ( i < text.size() && text[i] != ' ' && text[i] != '\n' && text[i] != '{' ) {
			i++;
		}
		std::string name = text.substr( start, i - start );
		while ( i < text.size() && ( text[i] == ' ' || text[i] == '\n' ) ) {
			i++;
		}
		if ( name == "}" ) {
			fprintf( stderr, "%s: stray }, the rest left out as the game does\n", fileName.c_str() );
			break;
		}
		if ( i >= text.size() || text[i] != '{' ) {
			fprintf( stderr, "%s: shader \"%s\" with no {, file left out as the game does\n", fileName.c_str(), name.c_str() );
			return true;
		}
		int depth = 0;
		for ( ; i < text.size(); i++ ) {
			if ( text[i] == '{' ) {
				depth++;
			} else if ( text[i] == '}' && --depth == 0 ) {
				i++;
				break;
			}
		}
		std::string body = text.substr( start, i - start );
		for ( ; depth > 0; depth-- ) {
			body += "\n}";
		}
		std::transform( name.begin(), name.end(), name.begin(), ::tolower );
		shaders.push_back( { name, body } );
	}
	files[fileName] = std::move( shaders );
	return true;
}

void WriteShaders( std::vector<uint8_t> &data, ShaderStats &st ) {
	data.clear();
	if ( files.empty() ) {
		return;
	}
	/* the renderer joins the files last listed first */
	std::string out;
	for ( auto it = files.rbegin(); it != files.rend(); ++it ) {
		for ( const Shader &s : it->second ) {
			st.shaders++;
			bool used = words.count( s.name ) > 0;
			for ( size_t p = 0; !used && p < prefixes.size(); p++ ) {
				const std::string &pre = prefixes[p];
				if ( !s.name.compare( 0, pre.size(), pre ) ) {
					std::string rest = s.name.substr( pre.size() );
					used = rest.size() < 4 || words.count( rest ) > 0;
				}
			}
			if ( used ) {
				st.kept++;
				out += s.text;
				out += '\n';
			}
		}
	}
	st.bytesOut = out.size();
	data.assign( out.begin(), out.end() );
}
