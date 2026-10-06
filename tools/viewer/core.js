//<core>
// decoders for the Dreamcast build's models and textures, as the game reads
// them (mdsc/mdsc.c, renderer tr_animation.c / tr_surface.c, pvr_gl.c)
const MV = (() => {
'use strict';

const str = ( u8, o, n ) => { let s = ''; for ( let i = 0; i < n && u8[o + i]; i++ ) s += String.fromCharCode( u8[o + i] ); return s; };
const noExt = p => p.replace( /\.[^./]*$/, '' );

/* ---------------- triangles ---------------- */

// R_StripTriangles: a b c d e -> a b c, c b d, c d e (every other turned)
function stripTris( dv, o, numTris ) {
	const out = new Uint32Array( numTris * 3 );
	let k = 0;
	while ( numTris > 0 ) {
		let a = dv.getUint16( o, true ) & 0x7fff, b = dv.getUint16( o + 2, true ), odd = 0;
		o += 4;
		do {
			const c = dv.getUint16( o, true ); o += 2;
			out[k++] = odd ? b : a; out[k++] = odd ? a : b; out[k++] = c;
			a = b; b = c; odd ^= 1;
		} while ( --numTris > 0 && !( dv.getUint16( o, true ) & 0x8000 ) );
	}
	return out;
}

function intTris( dv, o, numTris ) {
	const out = new Uint32Array( numTris * 3 );
	for ( let i = 0; i < out.length; i++ ) out[i] = dv.getInt32( o + i * 4, true );
	return out;
}

/* ---------------- MDSC ---------------- */

const KEYS_BITMAP = 0x40000000;
const QUAT_RANGE = 0.70710678;

function trackKeys( dv, ab, nk, ofsK ) {
	const n = nk & ~KEYS_BITMAP, keys = new Int32Array( n );
	if ( !( nk & KEYS_BITMAP ) ) {
		for ( let i = 0; i < n; i++ ) keys[i] = dv.getUint16( ab + ofsK + 2 * i, true );
		return { keys, list: true };
	}
	const nb = dv.getUint16( ab + ofsK, true ), bits = ab + ofsK + 2 * ( ( nb + 2 ) & ~1 );
	let c = 0;
	for ( let w = 0; w < nb * 2; w++ ) {
		const word = dv.getUint32( bits + 4 * w, true );
		if ( !word ) continue;
		for ( let b = 0; b < 32; b++ ) if ( ( word >>> b ) & 1 ) keys[c++] = w * 32 + b;
	}
	return { keys, list: false };
}

// kind: 'f3' / 'f10' floats, 'q' packed quaternions, 'd' short[4] directions
function parseTrack( dv, ab, o, kind ) {
	const nk = dv.getInt32( o, true ), ofsK = dv.getInt32( o + 4, true ), ofsV = dv.getInt32( o + 8, true );
	const { keys, list } = trackKeys( dv, ab, nk, ofsK ), n = keys.length;
	let vals, size;
	if ( kind === 'q' ) {
		vals = new Uint32Array( n ); size = 4;
		for ( let i = 0; i < n; i++ ) vals[i] = dv.getUint32( ab + ofsV + 4 * i, true );
	} else if ( kind === 'd' ) {
		vals = new Float32Array( n * 3 ); size = 8;
		for ( let i = 0; i < n; i++ ) for ( let j = 0; j < 3; j++ ) vals[i * 3 + j] = dv.getInt16( ab + ofsV + 8 * i + 2 * j, true );
	} else {
		const c = kind === 'f3' ? 3 : 10; size = c * 4;
		vals = new Float32Array( n * c );
		for ( let i = 0; i < n * c; i++ ) vals[i] = dv.getFloat32( ab + ofsV + 4 * i, true );
	}
	return { keys, vals, keyBytes: size + ( list ? 2 : 0 ) };
}

// k with keys[k] <= f < keys[k+1], and how far on to keys[k+1]
function findKey( keys, f ) {
	let lo = 0, hi = keys.length - 1;
	while ( lo < hi ) { const mid = ( lo + hi + 1 ) >> 1; if ( keys[mid] <= f ) lo = mid; else hi = mid - 1; }
	const t = lo + 1 < keys.length && keys[lo] !== f ? ( f - keys[lo] ) / ( keys[lo + 1] - keys[lo] ) : 0;
	return [lo, t];
}

function unpackQuat( p, q ) {
	const big = p >>> 30;
	let shift = 20, sum = 0;
	for ( let i = 0; i < 4; i++ ) {
		if ( i === big ) continue;
		q[i] = ( ( p >>> shift ) & 1023 ) * ( 2 * QUAT_RANGE / 1023 ) - QUAT_RANGE;
		sum += q[i] * q[i]; shift -= 10;
	}
	q[big] = sum < 1 ? Math.sqrt( 1 - sum ) : 0;
}

function nlerp( a, b, t, out ) {
	const dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], tb = dot < 0 ? -t : t;
	for ( let i = 0; i < 4; i++ ) out[i] = a[i] * ( 1 - t ) + b[i] * tb;
	const l = Math.hypot( out[0], out[1], out[2], out[3] ) || 1;
	for ( let i = 0; i < 4; i++ ) out[i] /= l;
}

// w x y z, M applied as M * v, rows m[r*3+c]
function quatToMatrix( q, m, o ) {
	const w = q[0], x = q[1], y = q[2], z = q[3];
	m[o] = 1 - 2 * ( y * y + z * z ); m[o + 1] = 2 * ( x * y - w * z ); m[o + 2] = 2 * ( x * z + w * y );
	m[o + 3] = 2 * ( x * y + w * z ); m[o + 4] = 1 - 2 * ( x * x + z * z ); m[o + 5] = 2 * ( y * z - w * x );
	m[o + 6] = 2 * ( x * z - w * y ); m[o + 7] = 2 * ( y * z + w * x ); m[o + 8] = 1 - 2 * ( x * x + y * y );
}

function matrixToQuat( m, o, q ) {
	const m00 = m[o], m01 = m[o + 1], m02 = m[o + 2], m10 = m[o + 3], m11 = m[o + 4], m12 = m[o + 5], m20 = m[o + 6], m21 = m[o + 7], m22 = m[o + 8];
	const t = m00 + m11 + m22;
	let s;
	if ( t > 0 ) { s = Math.sqrt( t + 1 ) * 2; q[0] = s / 4; q[1] = ( m21 - m12 ) / s; q[2] = ( m02 - m20 ) / s; q[3] = ( m10 - m01 ) / s; }
	else if ( m00 > m11 && m00 > m22 ) { s = Math.sqrt( 1 + m00 - m11 - m22 ) * 2; q[0] = ( m21 - m12 ) / s; q[1] = s / 4; q[2] = ( m01 + m10 ) / s; q[3] = ( m02 + m20 ) / s; }
	else if ( m11 > m22 ) { s = Math.sqrt( 1 + m11 - m00 - m22 ) * 2; q[0] = ( m02 - m20 ) / s; q[1] = ( m01 + m10 ) / s; q[2] = s / 4; q[3] = ( m12 + m21 ) / s; }
	else { s = Math.sqrt( 1 + m22 - m00 - m11 ) * 2; q[0] = ( m10 - m01 ) / s; q[1] = ( m02 + m20 ) / s; q[2] = ( m12 + m21 ) / s; q[3] = s / 4; }
}

function parseAnim( dv, ab, numBones ) {
	const a = { frame: parseTrack( dv, ab, ab, 'f3' ), cull: parseTrack( dv, ab, ab + 12, 'f10' ), rot: [], dir: [] };
	for ( let i = 0; i < numBones; i++ ) a.rot.push( parseTrack( dv, ab, ab + 24 + 12 * i, 'q' ) );
	for ( let i = 0; i < numBones; i++ ) a.dir.push( parseTrack( dv, ab, ab + 24 + 12 * ( numBones + i ), 'd' ) );
	return a;
}

function parseMDSC( ab ) {
	const dv = new DataView( ab ), u8 = new Uint8Array( ab ), I = o => dv.getInt32( o, true );
	if ( str( u8, 0, 4 ) !== 'MDSC' ) throw new Error( 'not an MDSC' );
	const m = {
		kind: 'mds', version: I( 4 ), numFrames: I( 80 ), numBones: I( 84 ), torsoParent: I( 96 ),
		bones: [], tags: [], surfaces: [], bytes: ab.byteLength,
	};
	const ofsFrames = I( 88 ), ofsBones = I( 92 ), numSurfaces = I( 100 ), ofsSurfaces = I( 104 ), numTags = I( 108 ), ofsTags = I( 112 );
	for ( let i = 0; i < m.numBones; i++ ) {
		const o = ofsBones + 16 * i;
		m.bones.push( { parent: I( o ), torsoWeight: dv.getFloat32( o + 4, true ), parentDist: dv.getFloat32( o + 8, true ), flags: I( o + 12 ) } );
	}
	for ( let i = 0; i < numTags; i++ ) {
		const o = ofsTags + 72 * i;
		m.tags.push( { name: str( u8, o, 64 ).toLowerCase(), torsoWeight: dv.getFloat32( o + 64, true ), bone: I( o + 68 ) } );
	}
	let meshBytes = 0;
	for ( let s = 0, o = ofsSurfaces; s < numSurfaces; s++ ) {
		const nv = I( o + 144 ), ov = I( o + 148 ), nt = I( o + 152 ), ot = I( o + 156 ), oc = I( o + 160 ), end = I( o + 172 );
		const surf = { name: str( u8, o + 4, 64 ).toLowerCase(), shader: str( u8, o + 68, 64 ), minLod: I( o + 136 ), numVerts: nv,
			tris: stripTris( dv, o + ot, nt ), st: new Float32Array( nv * 2 ), collapse: new Int32Array( nv ),
			wStart: new Int32Array( nv + 1 ), wBone: [], wWeight: [], wOfs: [] };
		let p = o + ov, wc = 0;
		const wb = [], ww = [], wo = [];
		for ( let v = 0; v < nv; v++ ) {
			surf.st[v * 2] = dv.getInt16( p, true ) / 16384; surf.st[v * 2 + 1] = dv.getInt16( p + 2, true ) / 16384;
			const n = u8[p + 7];
			surf.wStart[v] = wc;
			p += 8;
			for ( let k = 0; k < n; k++, p += 10, wc++ ) {
				wo.push( dv.getInt16( p, true ) / 256, dv.getInt16( p + 2, true ) / 256, dv.getInt16( p + 4, true ) / 256 );
				ww.push( dv.getUint16( p + 6, true ) / 65535 );
				wb.push( u8[p + 8] );
			}
		}
		surf.wStart[nv] = wc;
		surf.wBone = Uint8Array.from( wb ); surf.wWeight = Float32Array.from( ww ); surf.wOfs = Float32Array.from( wo );
		for ( let v = 0; v < nv; v++ ) surf.collapse[v] = dv.getUint16( o + oc + 2 * v, true );
		m.surfaces.push( surf );
		meshBytes += end;
		o += end;
	}
	m.meshBytes = meshBytes;
	if ( m.version === 9 ) {
		const sb = ofsFrames, segs = [];
		const numSeg = I( sb + 68 ), ofsSeg = I( sb + 72 ), ofsAnim = I( sb + 76 );
		for ( let i = 0; i < numSeg; i++ ) {
			const o = sb + ofsSeg + 16 * i;
			segs.push( { first: I( o ), count: I( o + 4 ), srcFirst: I( o + 8 ), fromBase: I( o + 12 ) } );
		}
		m.share = { base: str( u8, sb, 64 ), segs };
		m.anim = parseAnim( dv, sb + ofsAnim, m.numBones );
	} else {
		m.anim = parseAnim( dv, ofsFrames, m.numBones );
	}
	m.animBytes = m.bytes - ofsFrames;
	return m;
}

// bytes of keys on each of the character's frames (shared base frames: 0)
function mdsFrameCost( m ) {
	const cost = new Float32Array( m.numFrames ), a = m.anim;
	let toChar = null;
	if ( m.share ) {
		toChar = new Int32Array( 65536 ).fill( -1 );
		for ( const s of m.share.segs ) if ( !s.fromBase ) for ( let j = 0; j < s.count; j++ ) toChar[s.srcFirst + j] = s.first + j;
	}
	const add = ( tr, own ) => { for ( const k of tr.keys ) { const f = own && toChar ? toChar[k] : k; if ( f >= 0 && f < cost.length ) cost[f] += tr.keyBytes; } };
	add( a.cull, false );
	add( a.frame, true );
	for ( const t of a.rot ) add( t, true );
	for ( const t of a.dir ) add( t, true );
	return cost;
}

function fromBase( m, f ) {
	if ( !m.share ) return false;
	for ( const s of m.share.segs ) if ( f >= s.first && f < s.first + s.count ) return !!s.fromBase;
	return false;
}

// DecodePose: each bone's model space matrix and the direction to it from
// its parent, and the root's offset
function decodePose( anim, bones, f ) {
	const nb = bones.length, M = new Float32Array( nb * 9 ), D = new Float32Array( nb * 3 ), P = new Float32Array( 3 );
	const q = [0, 0, 0, 0], qa = [0, 0, 0, 0], qb = [0, 0, 0, 0], L = new Float32Array( 9 );
	{
		const [k, t] = findKey( anim.frame.keys, f ), v = anim.frame.vals;
		for ( let c = 0; c < 3; c++ ) P[c] = t ? v[k * 3 + c] + ( v[k * 3 + 3 + c] - v[k * 3 + c] ) * t : v[k * 3 + c];
	}
	for ( let i = 0; i < nb; i++ ) {
		const rt = anim.rot[i], [k, t] = findKey( rt.keys, f );
		if ( t ) { unpackQuat( rt.vals[k], qa ); unpackQuat( rt.vals[k + 1], qb ); nlerp( qa, qb, t, q ); } else unpackQuat( rt.vals[k], q );
		const p = bones[i].parent;
		if ( p < 0 ) { quatToMatrix( q, M, i * 9 ); continue; }
		quatToMatrix( q, L, 0 );
		const po = p * 9, io = i * 9;
		for ( let r = 0; r < 3; r++ ) for ( let c = 0; c < 3; c++ )
			M[io + r * 3 + c] = M[po + r * 3] * L[c] + M[po + r * 3 + 1] * L[3 + c] + M[po + r * 3 + 2] * L[6 + c];
		const dt = anim.dir[i], [dk, dtt] = findKey( dt.keys, f ), dv = dt.vals, d = [0, 0, 0];
		for ( let c = 0; c < 3; c++ ) d[c] = dtt ? dv[dk * 3 + c] + ( dv[dk * 3 + 3 + c] - dv[dk * 3 + c] ) * dtt : dv[dk * 3 + c];
		let x = M[po] * d[0] + M[po + 1] * d[1] + M[po + 2] * d[2];
		let y = M[po + 3] * d[0] + M[po + 4] * d[1] + M[po + 5] * d[2];
		let z = M[po + 6] * d[0] + M[po + 7] * d[1] + M[po + 8] * d[2];
		const l = Math.hypot( x, y, z ) || 1;
		D[i * 3] = x / l; D[i * 3 + 1] = y / l; D[i * 3 + 2] = z / l;
	}
	return { M, D, P };
}

// a whole frame, as MDSC_DecodeFrame / MDSC_DecodeSharedFrame
function mdsPose( m, f ) {
	f = Math.max( 0, Math.min( m.numFrames - 1, f | 0 ) );
	if ( !m.share ) return decodePose( m.anim, m.bones, f );
	const segs = m.share.segs;
	let lo = 0, hi = segs.length - 1;
	while ( lo < hi ) { const mid = ( lo + hi + 1 ) >> 1; if ( segs[mid].first <= f ) lo = mid; else hi = mid - 1; }
	const s = segs[lo];
	if ( s.fromBase ) {
		const b = m.baseModel;
		if ( !b ) return null;
		return decodePose( b.anim, b.bones, Math.max( 0, Math.min( b.numFrames - 1, s.srcFirst + f - s.first ) ) );
	}
	return decodePose( m.anim, m.bones, s.srcFirst + f - s.first );
}

// per bone: B's share w[i] (a number: all the same)
function blendPose( A, B, w, keepP ) {
	const nb = A.M.length / 9, M = new Float32Array( nb * 9 ), D = new Float32Array( nb * 3 ), P = new Float32Array( 3 );
	const qa = [0, 0, 0, 0], qb = [0, 0, 0, 0], q = [0, 0, 0, 0];
	for ( let i = 0; i < nb; i++ ) {
		const t = typeof w === 'number' ? w : w[i];
		if ( t <= 0 || t >= 1 ) {
			const S = t >= 1 ? B : A;
			M.set( S.M.subarray( i * 9, i * 9 + 9 ), i * 9 ); D.set( S.D.subarray( i * 3, i * 3 + 3 ), i * 3 );
			continue;
		}
		matrixToQuat( A.M, i * 9, qa ); matrixToQuat( B.M, i * 9, qb ); nlerp( qa, qb, t, q ); quatToMatrix( q, M, i * 9 );
		let x = A.D[i * 3] + ( B.D[i * 3] - A.D[i * 3] ) * t, y = A.D[i * 3 + 1] + ( B.D[i * 3 + 1] - A.D[i * 3 + 1] ) * t, z = A.D[i * 3 + 2] + ( B.D[i * 3 + 2] - A.D[i * 3 + 2] ) * t;
		const l = Math.hypot( x, y, z ) || 1;
		D[i * 3] = x / l; D[i * 3 + 1] = y / l; D[i * 3 + 2] = z / l;
	}
	const tp = keepP ? 0 : typeof w === 'number' ? w : 0;
	for ( let c = 0; c < 3; c++ ) P[c] = A.P[c] + ( B.P[c] - A.P[c] ) * tp;
	return { M, D, P };
}

// R_CalcBone's translations: the root at the offset, each bone parentDist
// from its parent
function boneOrigins( m, pose ) {
	const nb = m.numBones, T = new Float32Array( nb * 3 );
	for ( let i = 0; i < nb; i++ ) {
		const p = m.bones[i].parent;
		for ( let c = 0; c < 3; c++ ) T[i * 3 + c] = p < 0 ? pose.P[c] : T[p * 3 + c] + m.bones[i].parentDist * pose.D[i * 3 + c];
	}
	return T;
}

// RB_SurfaceAnim: each vertex the sum of its weights' bone * offset
function skinSurface( s, pose, T, out ) {
	const M = pose.M;
	for ( let v = 0; v < s.numVerts; v++ ) {
		let x = 0, y = 0, z = 0;
		for ( let k = s.wStart[v]; k < s.wStart[v + 1]; k++ ) {
			const b = s.wBone[k], w = s.wWeight[k], o = b * 9, ox = s.wOfs[k * 3], oy = s.wOfs[k * 3 + 1], oz = s.wOfs[k * 3 + 2];
			x += w * ( M[o] * ox + M[o + 1] * oy + M[o + 2] * oz + T[b * 3] );
			y += w * ( M[o + 3] * ox + M[o + 4] * oy + M[o + 5] * oz + T[b * 3 + 1] );
			z += w * ( M[o + 6] * ox + M[o + 7] * oy + M[o + 8] * oz + T[b * 3 + 2] );
		}
		out[v * 3] = x; out[v * 3 + 1] = y; out[v * 3 + 2] = z;
	}
}

/* ---------------- MD3 / MDC / MDB ---------------- */

const MDC_DIST = 0.05, MDC_MAX_OFS = 127;

function parseVertModel( ab ) {
	const dv = new DataView( ab ), u8 = new Uint8Array( ab ), I = o => dv.getInt32( o, true ), S = o => dv.getInt16( o, true );
	const id = str( u8, 0, 4 );
	if ( id !== 'IDP3' && id !== 'IDPC' ) throw new Error( 'not an MD3/MDC: ' + id );
	const mdc = id === 'IDPC';
	const m = { kind: 'vert', mdc, numFrames: I( 76 ), surfaces: [], bytes: ab.byteLength, animBytes: 0 };
	const numSurf = I( 84 ), ofsSurf = I( mdc ? 104 : 100 );
	for ( let s = 0, o = ofsSurf; s < numSurf; s++ ) {
		const surf = { name: str( u8, o + 4, 64 ).toLowerCase() };
		let nv, nt, ot, osh, ost, oxyz, end;
		if ( mdc ) {
			surf.numComp = I( o + 72 ); surf.numBase = I( o + 76 );
			nv = I( o + 84 ); nt = I( o + 88 ); ot = I( o + 92 ); osh = I( o + 96 ); ost = I( o + 100 ); oxyz = I( o + 104 );
			surf.oComp = o + I( o + 108 ); surf.oFBF = o + I( o + 112 ); surf.oFCF = o + I( o + 116 ); end = I( o + 120 );
			surf.type = surf.numComp === -1 ? 'mdb bones' : surf.numComp === -2 ? 'mdb keys' : 'mdc';
		} else {
			nv = I( o + 80 ); nt = I( o + 84 ); ot = I( o + 88 ); osh = I( o + 92 ); ost = I( o + 96 ); oxyz = I( o + 100 ); end = I( o + 104 );
			surf.type = 'md3';
		}
		surf.numVerts = nv; surf.oXyz = o + oxyz;
		surf.shader = str( u8, o + osh, 64 );
		surf.tris = intTris( dv, o + ot, nt );
		surf.st = new Float32Array( nv * 2 );
		for ( let i = 0; i < nv * 2; i++ ) surf.st[i] = dv.getFloat32( o + ost + 4 * i, true );
		// everything after the texture coordinates is the animation
		m.animBytes += end - ( ost + nv * 8 );
		m.surfaces.push( surf );
		o += end;
	}
	m.dv = dv;
	return m;
}

function addComp( dv, o, out, i ) {
	const v = dv.getUint32( o, true );
	out[i] += ( ( v & 255 ) - MDC_MAX_OFS ) * MDC_DIST;
	out[i + 1] += ( ( ( v >>> 8 ) & 255 ) - MDC_MAX_OFS ) * MDC_DIST;
	out[i + 2] += ( ( ( v >>> 16 ) & 255 ) - MDC_MAX_OFS ) * MDC_DIST;
}

function baseXyz( dv, s, base, out, w, add ) {
	const o = s.oXyz + base * s.numVerts * 8;
	for ( let v = 0; v < s.numVerts; v++ ) for ( let c = 0; c < 3; c++ ) {
		const x = dv.getInt16( o + v * 8 + c * 2, true ) / 64;
		out[v * 3 + c] = add ? out[v * 3 + c] + x * w : x * w;
	}
}

// one surface's vertexes on frame f (whole), into out
function vertFrame( m, s, f, out ) {
	const dv = m.dv;
	f = Math.max( 0, Math.min( m.numFrames - 1, f | 0 ) );
	if ( s.type === 'md3' ) { baseXyz( dv, s, f, out, 1, false ); return; }
	if ( s.type === 'mdc' ) {
		baseXyz( dv, s, dv.getInt16( s.oFBF + 2 * f, true ), out, 1, false );
		const comp = s.numComp > 0 ? dv.getInt16( s.oFCF + 2 * f, true ) : -1;
		if ( comp >= 0 ) for ( let v = 0; v < s.numVerts; v++ ) addComp( dv, s.oComp + ( comp * s.numVerts + v ) * 4, out, v * 3 );
		return;
	}
	// .mdb: the frame is kept frames key and key + 1, lerp of the way on
	const key = dv.getInt16( s.oFCF + 4 * f, true ), t = dv.getInt16( s.oFCF + 4 * f + 2, true ) / 32767;
	const ws = t > 0 ? [[key, 1 - t], [key + 1, t]] : [[key, 1]];
	if ( s.type === 'mdb keys' ) {
		const tmp = new Float32Array( s.numVerts * 3 );
		out.fill( 0 );
		for ( const [k, w] of ws ) {
			const base = dv.getInt16( s.oFBF + 4 * k, true ), comp = dv.getInt16( s.oFBF + 4 * k + 2, true );
			baseXyz( dv, s, base, tmp, 1, false );
			if ( comp >= 0 ) for ( let v = 0; v < s.numVerts; v++ ) addComp( dv, s.oComp + ( comp * s.numVerts + v ) * 4, tmp, v * 3 );
			for ( let i = 0; i < tmp.length; i++ ) out[i] += tmp[i] * w;
		}
		return;
	}
	// bones: numBase of them, each a run of the rest vertexes
	const nb = s.numBase, first = ( ws[0][0] * nb ) * 20;
	let v = 0;
	for ( let b = 0; b < nb; b++ ) {
		const q = [0, 0, 0, 0], tr = [0, 0, 0], f0 = s.oFBF + ( ws[0][0] * nb + b ) * 20;
		for ( const [k, w] of ws ) {
			const p = s.oFBF + ( k * nb + b ) * 20;
			let dot = 0;
			for ( let i = 0; i < 4; i++ ) dot += dv.getInt16( p + 2 * i, true ) * dv.getInt16( f0 + 2 * i, true );
			const sw = dot < 0 ? -w : w;
			for ( let i = 0; i < 4; i++ ) q[i] += dv.getInt16( p + 2 * i, true ) * sw;
			for ( let i = 0; i < 3; i++ ) tr[i] += dv.getFloat32( p + 8 + 4 * i, true ) * w;
		}
		let l = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
		l = l > 0 ? 2 / l : 0;
		const [x, y, z, ww] = q;
		const r = [1 - ( y * y + z * z ) * l, ( x * y - ww * z ) * l, ( x * z + ww * y ) * l,
			( x * y + ww * z ) * l, 1 - ( x * x + z * z ) * l, ( y * z - ww * x ) * l,
			( x * z - ww * y ) * l, ( y * z + ww * x ) * l, 1 - ( x * x + y * y ) * l];
		const count = dv.getInt16( s.oComp + 2 * b, true );
		for ( let i = 0; i < count; i++, v++ ) {
			const o = s.oXyz + v * 8, px = dv.getInt16( o, true ) / 64, py = dv.getInt16( o + 2, true ) / 64, pz = dv.getInt16( o + 4, true ) / 64;
			out[v * 3] = r[0] * px + r[1] * py + r[2] * pz + tr[0];
			out[v * 3 + 1] = r[3] * px + r[4] * py + r[5] * pz + tr[1];
			out[v * 3 + 2] = r[6] * px + r[7] * py + r[8] * pz + tr[2];
		}
	}
	void first;
}

/* ---------------- .dt textures ---------------- */

function twid( x, y, w, h ) {
	const m = Math.min( w, h );
	let idx = 0, sh = 0;
	for ( let b = 1; b < m; b <<= 1 ) { if ( y & b ) idx |= 1 << sh; sh++; if ( x & b ) idx |= 1 << sh; sh++; }
	const lm = Math.log2( m );
	if ( w > h ) idx += ( x >> lm ) * m * m; else if ( h > w ) idx += ( y >> lm ) * m * m;
	return idx;
}

// RGBA of a .dt's top level: { w, h, rgba, alpha } or null
function decodeDT( ab ) {
	const dv = new DataView( ab ), u8 = new Uint8Array( ab );
	if ( ab.byteLength < 32 || str( u8, 0, 4 ) !== 'DcTx' ) return null;
	const chunk = dv.getUint32( 4, true ), cbs = u8[10], type = dv.getUint32( 16, true ), data = ( u8[9] + 1 ) * 32;
	const w = 8 << ( ( type >>> 3 ) & 7 ), h = 8 << ( type & 7 ), pf = ( type >>> 27 ) & 7;
	const vq = ( type >>> 30 ) & 1, mip = type >>> 31, twiddled = !( ( type >>> 26 ) & 1 );
	if ( pf > 3 && pf !== 6 ) return null;
	let raw;
	if ( vq ) {
		const n = cbs + 1;
		let idx = data + n * 8;
		if ( mip ) { let off = 1; for ( let s = 2; s < w; s *= 2 ) off += s * s / 4; idx += off; }
		raw = ( x, y ) => {
			const i = u8[idx + twid( x >> 1, y >> 1, w >> 1, h >> 1 )];
			return dv.getUint16( data + 8 * ( i - 256 + n ) + 2 * ( ( ( x & 1 ) << 1 ) | ( y & 1 ) ), true );
		};
	} else if ( pf === 6 ) {
		let top = data;
		if ( mip ) { top += 3; for ( let s = 1; s < w; s *= 2 ) top += s * s; }
		raw = ( x, y ) => u8[top + ( twiddled ? twid( x, y, w, h ) : y * w + x )];
	} else {
		let top = data;
		if ( mip ) { top += 6; for ( let s = 1; s < w; s *= 2 ) top += 2 * s * s; }
		raw = ( x, y ) => dv.getUint16( top + 2 * ( twiddled ? twid( x, y, w, h ) : y * w + x ), true );
	}
	const pal = [];
	if ( pf === 6 ) for ( let o = chunk; o + 4 <= ab.byteLength; o += 4 ) pal.push( dv.getUint32( o, true ) );
	const rgba = new Uint8Array( w * h * 4 );
	for ( let y = 0; y < h; y++ ) for ( let x = 0; x < w; x++ ) {
		const o = ( y * w + x ) * 4;
		let r, g, b, a = 255;
		if ( pf === 6 ) {
			const c = pal[raw( x, y )] || 0;
			a = c >>> 24; r = ( c >>> 16 ) & 255; g = ( c >>> 8 ) & 255; b = c & 255;
		} else if ( pf === 3 ) {
			const e = raw( x & ~1, y ), d = raw( x | 1, y ), Y = ( x & 1 ? d : e ) >> 8, U = ( e & 255 ) - 128, V = ( d & 255 ) - 128;
			r = Y + 1.402 * V; g = Y - 0.344 * U - 0.714 * V; b = Y + 1.772 * U;
		} else {
			const v = raw( x, y );
			if ( pf === 0 ) { a = v & 0x8000 ? 255 : 0; r = ( ( v >> 10 ) & 31 ) * 255 / 31; g = ( ( v >> 5 ) & 31 ) * 255 / 31; b = ( v & 31 ) * 255 / 31; }
			else if ( pf === 1 ) { r = ( ( v >> 11 ) & 31 ) * 255 / 31; g = ( ( v >> 5 ) & 63 ) * 255 / 63; b = ( v & 31 ) * 255 / 31; }
			else { a = ( ( v >> 12 ) & 15 ) * 17; r = ( ( v >> 8 ) & 15 ) * 17; g = ( ( v >> 4 ) & 15 ) * 17; b = ( v & 15 ) * 17; }
		}
		rgba[o] = r; rgba[o + 1] = g; rgba[o + 2] = b; rgba[o + 3] = a;
	}
	return { w, h, rgba, alpha: pf === 0 || pf === 2 || pf === 6, format: ['1555', '565', '4444', 'yuv', '', '', 'pal8'][pf] + ( vq ? ' vq' : '' ) };
}

/* ---------------- wolfanim.cfg / .skin ---------------- */

function parseAnimCfg( text ) {
	const anims = [];
	let on = false;
	for ( const line of text.split( /\r?\n/ ) ) {
		const l = line.replace( /\/\/.*/, '' ).trim();
		if ( !l ) continue;
		if ( /^STARTANIMS/i.test( l ) ) { on = true; continue; }
		if ( /^ENDANIMS/i.test( l ) ) break;
		if ( !on ) continue;
		const t = l.split( /\s+/ );
		if ( t.length < 3 || !/^\d+$/.test( t[1] ) ) continue;
		anims.push( { name: t[0], first: +t[1], length: +t[2], loop: +( t[3] || 0 ), fps: +( t[4] || 20 ) || 20, rest: t.slice( 5 ).join( '\t' ) } );
	}
	return anims;
}

function parseSkin( text ) {
	const m = {};
	for ( const line of text.split( /\r?\n/ ) ) {
		const i = line.indexOf( ',' );
		if ( i < 0 ) continue;
		const name = line.slice( 0, i ).trim().toLowerCase(), p = line.slice( i + 1 ).trim().replace( /"/g, '' );
		if ( p && !name.startsWith( 'md3_' ) ) m[name] = p;
	}
	return m;
}

/* ---------------- .wld (the world) / .col entities / shader scripts ---------------- */

const SURF_SKY = 0x4, SURF_NODRAW = 0x80;

// the map's surfaces put together by shader: one mesh each
function parseWLD( ab ) {
	const dv = new DataView( ab ), u8 = new Uint8Array( ab ), I = o => dv.getInt32( o, true ), F = o => dv.getFloat32( o, true );
	if ( str( u8, 0, 4 ) !== 'RWLD' ) throw new Error( 'not a .wld' );
	const lump = i => [I( 8 + i * 8 ), I( 12 + i * 8 )];
	const [so, sl] = lump( 0 ), [fo, fl] = lump( 3 ), [vo] = lump( 4 ), [io] = lump( 5 );
	const shaders = [];
	for ( let o = so; o < so + sl; o += 72 ) shaders.push( { name: str( u8, o, 64 ).toLowerCase(), flags: I( o + 64 ), contents: I( o + 68 ) } );
	const surfs = [];
	for ( let o = fo; o + 140 <= fo + fl; o += 140 ) {
		surfs.push( { kind: I( o ), shader: I( o + 4 ), firstVert: I( o + 16 ), numVerts: I( o + 20 ), firstIndex: I( o + 24 ), numIndexes: I( o + 28 ),
			origin: [F( o + 60 ), F( o + 64 ), F( o + 68 )], xyzStep: F( o + 72 ), stOrigin: [F( o + 76 ), F( o + 80 )], stStep: F( o + 84 ) } );
	}
	const groups = new Map();
	let lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9], numVerts = 0, numTris = 0;
	for ( const s of surfs ) {
		if ( s.kind === 3 || !s.numVerts || ( shaders[s.shader] && shaders[s.shader].flags & SURF_NODRAW ) ) continue;
		let g = groups.get( s.shader );
		if ( !g ) groups.set( s.shader, g = { shader: shaders[s.shader] || { name: '?', flags: 0 }, surfs: [], nv: 0, ni: 0 } );
		g.surfs.push( s ); g.nv += s.numVerts; g.ni += s.numIndexes;
	}
	const meshes = [];
	for ( const g of groups.values() ) {
		const pos = new Float32Array( g.nv * 3 ), nrm = new Float32Array( g.nv * 3 ), st = new Float32Array( g.nv * 2 ), col = new Uint8Array( g.nv * 4 ), tris = new Uint32Array( g.ni );
		let v = 0, t = 0;
		for ( const s of g.surfs ) {
			const strip = stripTris( dv, io + s.firstIndex * 2, s.numIndexes / 3 );
			for ( let i = 0; i < strip.length; i++ ) tris[t++] = strip[i] + v;
			for ( let i = 0; i < s.numVerts; i++, v++ ) {
				const o = vo + ( s.firstVert + i ) * 16;
				for ( let c = 0; c < 3; c++ ) {
					const x = s.origin[c] + dv.getInt16( o + c * 2, true ) * s.xyzStep;
					pos[v * 3 + c] = x; lo[c] = Math.min( lo[c], x ); hi[c] = Math.max( hi[c], x );
				}
				const a = u8[o + 6] * Math.PI * 2 / 256, b = u8[o + 7] * Math.PI * 2 / 256;
				nrm[v * 3] = Math.cos( a ) * Math.sin( b ); nrm[v * 3 + 1] = Math.sin( a ) * Math.sin( b ); nrm[v * 3 + 2] = Math.cos( b );
				st[v * 2] = s.stOrigin[0] + dv.getInt16( o + 8, true ) * s.stStep;
				st[v * 2 + 1] = s.stOrigin[1] + dv.getInt16( o + 10, true ) * s.stStep;
				for ( let c = 0; c < 4; c++ ) col[v * 4 + c] = u8[o + 12 + c];
			}
		}
		numVerts += g.nv; numTris += g.ni / 3;
		meshes.push( { name: g.shader.name, shader: g.shader.name, sky: !!( g.shader.flags & SURF_SKY ), numVerts: g.nv, pos, nrm, st, col, tris, surfaces: g.surfs.length } );
	}
	return { kind: 'wld', bytes: ab.byteLength, shaders, numSurfaces: surfs.length, meshes, numVerts, numTris, lo, hi };
}

// the .col's entity string: [{classname, origin, ...}]
function parseEntities( ab ) {
	const dv = new DataView( ab ), u8 = new Uint8Array( ab );
	const o = dv.getInt32( 8 + 9 * 8, true ), l = dv.getInt32( 12 + 9 * 8, true );
	const text = str( u8, o, l ), ents = [];
	for ( const m of text.matchAll( /\{([^{}]*)\}/g ) ) {
		const e = {};
		for ( const kv of m[1].matchAll( /"([^"]*)"\s+"([^"]*)"/g ) ) e[kv[1].toLowerCase()] = kv[2];
		ents.push( e );
	}
	return ents;
}

// shader scripts: name -> the image its first stage that isn't $lightmap etc. maps
function parseShaderScript( text, out ) {
	const t = text.replace( /\/\/[^\n]*/g, '' ).match( /[{}]|[^\s{}]+/g ) || [];
	for ( let i = 0; i < t.length; ) {
		const name = t[i++].toLowerCase();
		if ( t[i] !== '{' ) continue;
		let depth = 0, img = null, own = null;
		for ( ; i < t.length; i++ ) {
			const w = t[i].toLowerCase();
			if ( w === '{' ) depth++;
			else if ( w === '}' ) { if ( --depth === 0 ) { i++; break; } }
			else if ( depth === 2 && ( w === 'map' || w === 'clampmap' ) && t[i + 1] && t[i + 1][0] !== '$' ) {
				// the stage of its own image if it has one (a glow under it can come first)
				if ( !img ) img = t[i + 1];
				if ( noExt( t[i + 1].toLowerCase() ) === name ) own = t[i + 1];
			}
			else if ( !img && depth === 2 && w === 'animmap' && t[i + 2] ) img = t[i + 2];
		}
		if ( own || img ) out.set( name, ( own || img ).toLowerCase() );
	}
	return out;
}

return { parseWLD, parseEntities, parseShaderScript, str, noExt, parseMDSC, mdsFrameCost, fromBase, mdsPose, blendPose, boneOrigins, skinSurface,
	parseVertModel, vertFrame, decodeDT, parseAnimCfg, parseSkin, matrixToQuat };
})();
if ( typeof module !== 'undefined' ) module.exports = MV;
//</core>
