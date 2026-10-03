// included after <math.h> (q_shared.h, the splines' q_splineshared.h)
#ifndef Q_FASTMATH_H
#define Q_FASTMATH_H

#ifdef USE_SH4ZAM
// sin, cos and tan are sh4zam's, the SH4's fsca: right to about 200000
// radians (it's a 16 bit angle). The rest are the C library's float ones,
// not its double ones (the game's float): sh4zam's atan2 is up to half a
// degree out, its acos and asin a quarter, too much for aiming and the
// view, and its pow's rough, wrong for 0 and less.
#include <sh4zam/shz_trig.h>
#define sin( x )        shz_sinf( x )
#define cos( x )        shz_cosf( x )
#define tan( x )        shz_tanf( x )
#define asin( x )       asinf( x )
#define acos( x )       acosf( x )
#define atan( x )       atanf( x )
#define atan2( y, x )   atan2f( y, x )
#define pow( x, y )     powf( x, y )
#endif

#endif
