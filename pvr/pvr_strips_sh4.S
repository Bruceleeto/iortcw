/*
 * pvr_strips_sh4.S: the .wld fast strip path straight into the TA, in SH4
 * (DrawStripsDirect in pvr_gl.c is the C of it, kept for the sim).
 *
 *   int pvrgl_DrawStripsDirectSH4( const pvrglPackedVert_t *verts,   r4
 *                                  const unsigned short *strips,     r5
 *                                  int numIndexes,                   r6
 *                                  const pvrglStripXform_t *p );     r7
 *
 * returns the strips drawn. p is the draw's matrix m[16] (column major,
 * x' = m0 x + m4 y + m8 z + m12, w = m3 x + m7 y + m11 z + m15, with the
 * origin and step folded in) then s0, t0, ss (st = s0 + ss * st16) and ds
 * (sz = ds / w). The caller has the opaque list open and the header in;
 * each vertex goes to a store queue and off to the TA; pvr_dr_addr is
 * left as the half last used so KOS's pvr_dr_target carries on right.
 *
 * Each vertex: 4 mov.l (y:x, junk:z, t:s, rgba), exts/swap unpack, lds +
 * float, ftrv through XMTRX (loaded here with the rows turned so the
 * result is w, x', y', 1), fdiv 1/w, 3 fmul, 2 fmac for st, 4 int ops
 * swap r and b for the ARGB, 5 pair/long stores (SZ=1) backwards into
 * the SQ, pref. Two vertexes in flight: while one's ftrv and fdiv run, the
 * next is loaded and converted (fv4 / fv8 alternate), and the one after
 * that is prefetched, which hides most of the vertex read misses.
 *
 * Register use in the loop:
 *   r1 address of V (the vertex being emitted)   r2 address of N (the next)
 *   r13/r14 the PVR command of V / N (VERTEX, or EOL where the next index
 *           starts a strip), swapping roles every vertex
 *   r4 verts  r5 next index  r6 indexes left to read  r7 SQ pointer
 *   r8 0x7fff  r9 0x00ff00ff  r12 strips   r0 r3 r10 r11 scratch
 *   fr0 ss  fr2 ds  fr1 fr3 st temps  dr12 st out  dr14 (s0, t0)
 *   fv4 / fv8 the two vertexes
 */

	.little
	/* with the packed draw in the hot text region (DC_HOT in dc_prof.h) */
	.section .text.hot.10d,"ax",@progbits
	.align 2
	.global _pvrgl_DrawStripsDirectSH4
	.type _pvrgl_DrawStripsDirectSH4, @function

/* emit V (converted in fv \fx, address r1, command \cv), convert N (address
   r2) into \fy, and unless last: read the index after N, prefetch its
   vertex, and give N its command in \cn. The SQ half is \end's. Ends with
   N as V (r1) and that one as N (r2). */
.macro HALF fx, x0, x1, x2, x3, dx0, dx2, y0, y1, y2, y3, cv, cn, end, last, pool
	ftrv	xmtrx, \fx
	mov.l	@(12,r1), r3		/* V rgba */
.if \last == 0
	mov.w	@r5+, r11		/* the index after N */
.endif
	mov.l	@(8,r1), r1		/* V t:s */
	mov.l	.Lalpha\pool, r10
	swap.w	r3, r0
	xor	r3, r0
	and	r9, r0
	xor	r0, r3			/* r and b swapped: ARGB */
	or	r10, r3			/* alpha 255 */
	fdiv	\x0, \x3		/* 1 / w (ftrv + 5) */
	exts.w	r1, r0			/* s */
	lds	r0, FPUL
	swap.w	r1, r1
	float	FPUL, fr1
	exts.w	r1, r1			/* t */
	lds	r1, FPUL
	fmov	dr14, dr12		/* s0, t0 */
	float	FPUL, fr3
	mov.l	@r2, r0			/* N y:x */
	mov.l	@(4,r2), r1		/* N junk:z */
	fmac	fr0, fr1, fr12		/* s0 + ss * s */
	exts.w	r0, r10			/* N x */
	swap.w	r0, r0
	exts.w	r0, r0			/* N y */
	fmac	fr0, fr3, fr13		/* t0 + ss * t */
	lds	r10, FPUL
	float	FPUL, \y0
	exts.w	r1, r1			/* N z */
	lds	r0, FPUL
	float	FPUL, \y1
	lds	r1, FPUL
	float	FPUL, \y2
	fldi1	\y3
.if \last == 0
	mov	r11, r0
	and	r8, r0
	shll2	r0
	shll2	r0
	add	r4, r0			/* the vertex after N */
	pref	@r0
	mov.l	.Lcmd\pool, \cn
	cmp/pz	r11
	bt	1f
	shar	\cn			/* it starts a strip: N ends one */
	add	#1, r12
1:
.else
	mov	r2, r0			/* nothing after N: N stays as N */
.endif
	mov.l	\end\pool, r7
	mov	#0, r10
	fmul	\x3, \x1		/* sx (fdiv + 12) */
	fmul	\x3, \x2		/* sy */
	fmul	fr2, \x3		/* sz = ds / w */
	mov.l	r10, @-r7		/* 7: fog */
	mov.l	r3, @-r7		/* 6: ARGB */
	fmov	dr12, @-r7		/* 4 5: s t */
	mov	r2, r1			/* N is V next */
	mov	r0, r2			/* and the one after is N */
	fmov	\dx2, @-r7		/* 2 3: sy sz */
	fmov	\dx0, @-r7		/* 0 1: (w) sx */
	mov.l	\cv, @r7		/* 0: the command */
	pref	@r7			/* off to the TA */
.endm

_pvrgl_DrawStripsDirectSH4:
	mov	#3, r0
	cmp/ge	r0, r6
	bt	.Lgo
	rts
	mov	#0, r0
.Lgo:
	mov.l	r8, @-r15
	mov.l	r9, @-r15
	mov.l	r10, @-r15
	mov.l	r11, @-r15
	mov.l	r12, @-r15
	mov.l	r13, @-r15
	mov.l	r14, @-r15
	fmov.s	fr12, @-r15
	fmov.s	fr13, @-r15
	fmov.s	fr14, @-r15
	fmov.s	fr15, @-r15

	/* XMTRX from m with the rows turned: row 0 = m's w row, 1 = x, 2 = y,
	   3 = (0 0 0 1), so ftrv gives (w, x', y', 1) and w is out first */
	mov	r7, r3
	frchg
	fmov.s	@r3+, fr1
	fmov.s	@r3+, fr2
	add	#4, r3
	fmov.s	@r3+, fr0
	fldi0	fr3
	fmov.s	@r3+, fr5
	fmov.s	@r3+, fr6
	add	#4, r3
	fmov.s	@r3+, fr4
	fldi0	fr7
	fmov.s	@r3+, fr9
	fmov.s	@r3+, fr10
	add	#4, r3
	fmov.s	@r3+, fr8
	fldi0	fr11
	fmov.s	@r3+, fr13
	fmov.s	@r3+, fr14
	add	#4, r3
	fmov.s	@r3+, fr12
	fldi1	fr15
	frchg
	fmov.s	@r3+, fr14		/* s0 */
	fmov.s	@r3+, fr15		/* t0 */
	fmov.s	@r3+, fr0		/* ss */
	fmov.s	@r3+, fr2		/* ds */
	fschg				/* pair moves from here on */

	mov.l	.Lmask, r8
	mov.l	.Largb, r9
	mov	#0, r12
	add	#-2, r6			/* indexes left to read once the first two are */

	/* the first vertex, and the second's address and the first's command */
	mov.w	@r5+, r1
	and	r8, r1
	shll2	r1
	shll2	r1
	add	r4, r1
	mov.w	@r5+, r11
	mov.l	.Lcmd1, r13
	cmp/pz	r11
	bt	1f
	shar	r13
	add	#1, r12
1:
	mov	r11, r2
	and	r8, r2
	shll2	r2
	shll2	r2
	add	r4, r2
	pref	@r2
	mov.l	@r1, r0
	mov.l	@(4,r1), r3
	exts.w	r0, r10
	swap.w	r0, r0
	exts.w	r0, r0
	exts.w	r3, r3
	lds	r10, FPUL
	float	FPUL, fr4
	lds	r0, FPUL
	float	FPUL, fr5
	lds	r3, FPUL
	float	FPUL, fr6
	bra	.Lcheck
	fldi1	fr7

.Lloop:
	add	#-1, r6
	HALF fv4, fr4, fr5, fr6, fr7, dr4, dr6, fr8, fr9, fr10, fr11, r13, r14, .LendA, 0, 1
	cmp/pl	r6
	bf	.LtailB
	add	#-1, r6
	HALF fv8, fr8, fr9, fr10, fr11, dr8, dr10, fr4, fr5, fr6, fr7, r14, r13, .LendB, 0, 1
.Lcheck:
	cmp/pl	r6
	bt	.Lloop
	bra	.LtailA
	nop

	/* the literals (mov.l @(disp,PC) only reaches forward): the prologue's
	   and the loop's here, the tails' and the end's at the end */
	.align 2
.Lmask:	.long 0x7fff
.Largb:	.long 0x00ff00ff
.Lalpha1: .long 0xff000000
.Lcmd1:	.long 0xe0000000		/* PVR_CMD_VERTEX */
.LendA1: .long 0xe0000020		/* the end of store queue 0 */
.LendB1: .long 0xe0000040		/* and of 1 */

	/* no index left to read: V (in fv8) has its command, N is the last */
.LtailB:
	mov.l	.Leol2, r13
	add	#1, r12
	HALF fv8, fr8, fr9, fr10, fr11, dr8, dr10, fr4, fr5, fr6, fr7, r14, r13, .LendB, 1, 2
	HALF fv4, fr4, fr5, fr6, fr7, dr4, dr6, fr8, fr9, fr10, fr11, r13, r14, .LendA, 1, 2
	bra	.Ldone
	nop

	/* the same with V in fv4 */
.LtailA:
	mov.l	.Leol2, r14
	add	#1, r12
	HALF fv4, fr4, fr5, fr6, fr7, dr4, dr6, fr8, fr9, fr10, fr11, r13, r14, .LendA, 1, 2
	HALF fv8, fr8, fr9, fr10, fr11, dr8, dr10, fr4, fr5, fr6, fr7, r14, r13, .LendB, 1, 2

.Ldone:
	mov.l	.Lpdr2, r0
	mov.l	r7, @r0			/* the half last used */
	mov	r12, r0			/* the strips, before r12 is the caller's again */
	fschg
	fmov.s	@r15+, fr15
	fmov.s	@r15+, fr14
	fmov.s	@r15+, fr13
	fmov.s	@r15+, fr12
	mov.l	@r15+, r14
	mov.l	@r15+, r13
	mov.l	@r15+, r12
	mov.l	@r15+, r11
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8

	.align 2
.Lalpha2: .long 0xff000000
.Lcmd2:	.long 0xe0000000
.Leol2:	.long 0xf0000000		/* PVR_CMD_VERTEX_EOL */
.LendA2: .long 0xe0000020
.LendB2: .long 0xe0000040
.Lpdr2:	.long _pvr_dr_addr

	.size _pvrgl_DrawStripsDirectSH4, . - _pvrgl_DrawStripsDirectSH4
