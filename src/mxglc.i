; mxglc.i: a vertex program with clipping and culling on VU1, by hand: the EE sends the
; vertices of a draw without looking at them. mxglc.vsm is libgl's own with it. Another
; program sets MXGLC_COLOUR to 1 (its colours then replace c * cmul + cadd in draws with
; a negative cadd), and has the macros mxglc_colour_load (once for many
; vertices: vf22-vf29 are free) and mxglc_colour (vf14 the vertex's colour as integers, to
; vf17 with 1.5 * 2^23 added; vf25-vf29, Q and I are free); its layout is mxglc.h's with
; a uniform CLIP at quadword 9, for which libgl fills in the clipping's.
; MXGLC_FOG 1: XYZF2, with the macro mxglc_fog (vf12.w the vertex's w, to vf26.w F 0..255;
; vf26, vf27, Q and I are free). F goes over the flags in w just before the kick; pass 2's
; marks are in ST's w, and pass 3 interpolates f in ST's w.
;
; Pass 1, two vertices at a time, without branches: mxglv's transform and output, and
; CLIPW against the guard band and the depth planes. XYZ2's w, which the GS reads only
; bit 15 of, gets ADC (from pos.w's low half, mxgl.c's MXGL_ADC_W) and the clip flags of
; this vertex and the one before.
; It also works out each window's facing on the screen (the sign of the cross product,
; in ST's w, which the GIF does not read).
; Pass 2, per vertex, only for a batch with a vertex outside or with culling on: a
; triangle outside one plane is dropped (ADC), a triangle facing away too; one across
; planes gets ADC and a mark in w:
;   bit 15 ADC, bits 0-5 the planes (near, far, +x, -x, +y, -y), bit 6 winding reversed
; Pass 3, after the batch's XGKICK, only when a triangle was marked: each marked
; triangle again from the inputs, cut by its planes (Sutherland-Hodgman, as mxgl.c's
; clip_poly, each edge from its inside vertex), culled, and sent as a packet of its
; own in R0 or R1 by turns.
;
; Strips: pos.w's low half (mxgl.c's MXGL_W) says what a vertex is: bit 15 it closes no
; triangle, bit 14 the last three vertices are in the reverse of the triangle's order.
;
; Memory, quadwords: buffers 0 and 432 (counts 0, inputs 1, packet 220); contexts
; 864, 882; polygons WS_A 900, WS_B 927; packets R0 954, R1 982; SAVE 1010.
; Context: 0 tag, 1-4 mvp, 5 cscale, 6 tms, 7 tmt, 8 cmul, 9 cadd,
;   10 S (xyz scale of the clip test; w: 0x20 cull), 11 O (xyz: its offset times w;
;   w: 0x20 cull where det >= 0), 12-17 the planes (near, far, +x, -x, +y, -y)
;
; vf1 tms  vf2 (tms.z, tmt.z)  vf3 tmt  vf4-7 mvp  vf8 cmul  vf9 cadd  vf10 cscale
; vf19.x 1.5 * 2^23  vf20 S  vf21 O   (all kept from batch to batch); vf8 is cmul *
; cscale, vf9 cadd * cscale + 1.5 * 2^23: the colour comes out rounded to an integer
; vi10 cull  vi11 cull sense  vi13 R0 or R1
; vi14 the buffer  vi3 the vertex  vi4 vertices left  vi7 its index
	nop				xitop vi2
	nop				lq.xyzw vf1, 6(vi2)
	nop				lq.xyzw vf3, 7(vi2)
	nop				lq.xyzw vf4, 1(vi2)
	nop				lq.xyzw vf5, 2(vi2)
	nop				lq.xyzw vf6, 3(vi2)
	nop				lq.xyzw vf7, 4(vi2)
	nop				lq.xyzw vf8, 8(vi2)
	nop				lq.xyzw vf9, 9(vi2)
	nop				lq.xyzw vf10, 5(vi2)
	nop				lq.xyz vf20, 10(vi2)
	nop				lq.xyz vf21, 11(vi2)
	nop				ilw.w vi10, 10(vi2)
	nop				ilw.w vi11, 11(vi2)
	addz.xyz vf2, vf0, vf1z		loi 0x4b400000
	addz.y vf2, vf0, vf3z		nop
	addi.x vf19, vf0, i		nop
.if MXGLC_COLOUR
	; the program's colours only for a draw that needs them: a negative cadd
	subx.xyzw vf11, vf9, vf0x	nop
	nop				nop
	nop				nop
	nop				nop
	nop				fsand vi1, 2
	nop				isw.w vi1, 1011(vi0)
.endif
	mul.xyzw vf8, vf8, vf10		iaddiu vi13, vi0, 954
	mul.xyzw vf9, vf9, vf10		nop
	nop				nop
	nop				nop
	nop				nop
	addx.xyzw vf9, vf9, vf19x	nop
batch:
	nop				xtop vi14
	nop				xitop vi2
	nop				ilw.x vi4, 0(vi14)
	nop				ilw.x vi3, 0(vi2)
	nop				lq.xyzw vf11, 0(vi2)
	nop				iaddiu vi12, vi0, 0
	nop				iaddiu vi7, vi0, 0
	nop				nop
	nop				nop
	nop				ior vi2, vi3, vi4
	nop				sq.xyzw vf11, 220(vi14)
	nop				isw.x vi2, 220(vi14)
	nop				nop
	nop				ibeq vi4, vi0, kick
	nop				iaddiu vi3, vi14, 1
	nop				lq.xyzw vf11, 0(vi3)
	nop				lq.xyzw vf22, 3(vi3)
loop:
	mulaw.xyzw acc, vf7, vf0w	lq.xyzw vf12, 1(vi3)
	maddax.xyzw acc, vf4, vf11x	lq.xyzw vf13, 2(vi3)
	madday.xyzw acc, vf5, vf11y	lq.xyzw vf23, 4(vi3)
	maddz.xyzw vf14, vf6, vf11z	lq.xyzw vf24, 5(vi3)
	mulaw.xyzw acc, vf7, vf0w	ilw.w vi5, 0(vi3)
	maddax.xyzw acc, vf4, vf22x	ilw.w vi6, 3(vi3)
	madday.xyzw acc, vf5, vf22y	nop
	maddz.xyzw vf25, vf6, vf22z	div q, vf0w, vf14w
	mulaw.xyzw acc, vf2, vf0w	nop
	maddax.xyzw acc, vf1, vf12x	nop
	maddy.xyzw vf15, vf3, vf12y	nop
	mulaw.xyz acc, vf21, vf14w	nop
	madd.xyz vf17, vf14, vf20	nop
	itof0.xyzw vf13, vf13		nop
	sub.xyz vf12, vf31, vf30	nop
	mulaw.xyzw acc, vf9, vf0w	nop
	madd.xyzw vf16, vf8, vf13	nop
	clipw.xyz vf17, vf14w		waitq
	mulq.xyz vf18, vf14, q		nop
	mulq.xy vf15, vf15, q		nop
	addq.z vf15, vf0, q		nop
	mulaw.xyz acc, vf21, vf25w	div q, vf0w, vf25w
	madd.xyz vf28, vf25, vf20	fcget vi1
	ftoi4.xyz vf19, vf18		ior vi7, vi7, vi1
	sub.xyz vf13, vf18, vf30	ior vi5, vi5, vi1
	mulaw.xyzw acc, vf2, vf0w	sq.xyz vf15, 220(vi3)
	maddax.xyzw acc, vf1, vf23x	sq.xyzw vf16, 221(vi3)
	maddy.xyzw vf26, vf3, vf23y	mfir.w vf19, vi5
	itof0.xyzw vf24, vf24		lq.xyzw vf11, 6(vi3)
	opmula.xyz acc, vf12, vf13	sq.xyzw vf19, 222(vi3)
	opmsub.xyz vf17, vf13, vf12	lq.xyzw vf22, 9(vi3)
	clipw.xyz vf28, vf25w		nop
	nop				nop
	nop				nop
	nop				fmand vi1, vi10
	mulaw.xyzw acc, vf9, vf0w	waitq
	madd.xyzw vf27, vf8, vf24	isw.w vi1, 220(vi3)
	mulq.xyz vf29, vf25, q		iaddi vi4, vi4, -2
	mulq.xy vf26, vf26, q		fcget vi1
	addq.z vf26, vf0, q		ior vi7, vi7, vi1
	sub.xyz vf12, vf18, vf31	ior vi6, vi6, vi1
	ftoi4.xyz vf10, vf29		nop
	sub.xyz vf13, vf29, vf31	nop
	nop				sq.xyz vf26, 223(vi3)
	nop				sq.xyzw vf27, 224(vi3)
	nop				mfir.w vf10, vi6
	opmula.xyz acc, vf12, vf13	nop
	opmsub.xyz vf17, vf13, vf12	sq.xyzw vf10, 225(vi3)
	nop				move.xyz vf30, vf18
	nop				move.xyz vf31, vf29
	nop				nop
	nop				fmand vi1, vi10
	nop				isw.w vi1, 223(vi3)
	nop				ibgtz vi4, loop
	nop				iaddi vi3, vi3, 6
.if MXGLC_COLOUR
	; the program's colours, over what the loop wrote
	nop				ilw.w vi1, 1011(vi0)
	nop				nop
	nop				nop
	nop				nop
	nop				nop
	nop				ibeq vi1, vi0, nocolours
	nop				nop
	mxglc_colour_load
	nop				ilw.x vi4, 0(vi14)
	nop				iaddiu vi3, vi14, 1
colours:
	nop				lq.xyzw vf14, 2(vi3)
	mxglc_colour
	nop				iaddi vi4, vi4, -1
	nop				sq.xyzw vf17, 221(vi3)
	nop				ibne vi4, vi0, colours
	nop				iaddi vi3, vi3, 3
nocolours:
.endif
	; pass 2?
	nop				ior vi7, vi7, vi10
	nop				ilw.x vi4, 0(vi14)
	nop				ibeq vi7, vi0, kick
	nop				iaddiu vi3, vi14, 7
	nop				nop
	nop				isubiu vi4, vi4, 2
	nop				ilw.w vi7, 219(vi3)
	nop				iblez vi4, kick
	nop				nop
tri:
	; vi5 this vertex's w, vi7 the last's as pass 1 left it, vi6 its facing (st.w)
	nop				ilw.w vi5, 222(vi3)
	nop				ilw.w vi6, 220(vi3)
	nop				iaddiu vi2, vi0, 0xfff
	nop				nop
	nop				ior vi1, vi5, vi7
	nop				iand vi1, vi1, vi2
	nop				nop
	nop				ibltz vi5, isadc
	nop				nop
	; it closes a triangle; vi8 its window is reversed
	nop				iaddiu vi2, vi0, 0x4000
	nop				iand vi8, vi5, vi2
	nop				ibne vi1, vi0, marked
	nop				nop
	nop				ibeq vi10, vi0, nextv
	nop				nop
	nop				iadd vi2, vi6, vi11
	nop				nop
	nop				ibeq vi8, vi0, faces
	nop				nop
	nop				iaddiu vi2, vi2, 0x20
faces:
	nop				iand vi2, vi2, vi10
	nop				nop
	nop				ibeq vi2, vi0, nextv
	nop				nop
	nop				iaddiu vi6, vi0, 0x7fff
	nop				iaddi vi6, vi6, 1
	nop				b store
	nop				nop
marked:
	; a plane all three are outside of: nothing to cut; the others to the mark
	nop				iaddiu vi6, vi0, 0x7fff
	nop				iaddi vi6, vi6, 1
	nop				iaddiu vi2, vi0, 16
	nop				iaddiu vi1, vi0, 1024
	nop				iand vi2, vi2, vi5
	nop				iand vi9, vi1, vi5
	nop				iand vi1, vi1, vi7
	nop				ior vi15, vi2, vi9
	nop				ior vi15, vi15, vi1
	nop				nop
	nop				ibeq vi15, vi0, np0
	nop				nop
	nop				nop
	nop				ibeq vi2, vi0, in0
	nop				nop
	nop				ibeq vi9, vi0, in0
	nop				nop
	nop				ibeq vi1, vi0, in0
	nop				nop
	nop				b reject
	nop				nop
in0:
	nop				iaddiu vi6, vi6, 1
np0:
	nop				iaddiu vi2, vi0, 32
	nop				iaddiu vi1, vi0, 2048
	nop				iand vi2, vi2, vi5
	nop				iand vi9, vi1, vi5
	nop				iand vi1, vi1, vi7
	nop				ior vi15, vi2, vi9
	nop				ior vi15, vi15, vi1
	nop				nop
	nop				ibeq vi15, vi0, np1
	nop				nop
	nop				nop
	nop				ibeq vi2, vi0, in1
	nop				nop
	nop				ibeq vi9, vi0, in1
	nop				nop
	nop				ibeq vi1, vi0, in1
	nop				nop
	nop				b reject
	nop				nop
in1:
	nop				iaddiu vi6, vi6, 2
np1:
	nop				iaddiu vi2, vi0, 1
	nop				iaddiu vi1, vi0, 64
	nop				iand vi2, vi2, vi5
	nop				iand vi9, vi1, vi5
	nop				iand vi1, vi1, vi7
	nop				ior vi15, vi2, vi9
	nop				ior vi15, vi15, vi1
	nop				nop
	nop				ibeq vi15, vi0, np2
	nop				nop
	nop				nop
	nop				ibeq vi2, vi0, in2
	nop				nop
	nop				ibeq vi9, vi0, in2
	nop				nop
	nop				ibeq vi1, vi0, in2
	nop				nop
	nop				b reject
	nop				nop
in2:
	nop				iaddiu vi6, vi6, 4
np2:
	nop				iaddiu vi2, vi0, 2
	nop				iaddiu vi1, vi0, 128
	nop				iand vi2, vi2, vi5
	nop				iand vi9, vi1, vi5
	nop				iand vi1, vi1, vi7
	nop				ior vi15, vi2, vi9
	nop				ior vi15, vi15, vi1
	nop				nop
	nop				ibeq vi15, vi0, np3
	nop				nop
	nop				nop
	nop				ibeq vi2, vi0, in3
	nop				nop
	nop				ibeq vi9, vi0, in3
	nop				nop
	nop				ibeq vi1, vi0, in3
	nop				nop
	nop				b reject
	nop				nop
in3:
	nop				iaddiu vi6, vi6, 8
np3:
	nop				iaddiu vi2, vi0, 4
	nop				iaddiu vi1, vi0, 256
	nop				iand vi2, vi2, vi5
	nop				iand vi9, vi1, vi5
	nop				iand vi1, vi1, vi7
	nop				ior vi15, vi2, vi9
	nop				ior vi15, vi15, vi1
	nop				nop
	nop				ibeq vi15, vi0, np4
	nop				nop
	nop				nop
	nop				ibeq vi2, vi0, in4
	nop				nop
	nop				ibeq vi9, vi0, in4
	nop				nop
	nop				ibeq vi1, vi0, in4
	nop				nop
	nop				b reject
	nop				nop
in4:
	nop				iaddiu vi6, vi6, 16
np4:
	nop				iaddiu vi2, vi0, 8
	nop				iaddiu vi1, vi0, 512
	nop				iand vi2, vi2, vi5
	nop				iand vi9, vi1, vi5
	nop				iand vi1, vi1, vi7
	nop				ior vi15, vi2, vi9
	nop				ior vi15, vi15, vi1
	nop				nop
	nop				ibeq vi15, vi0, np5
	nop				nop
	nop				nop
	nop				ibeq vi2, vi0, in5
	nop				nop
	nop				ibeq vi9, vi0, in5
	nop				nop
	nop				ibeq vi1, vi0, in5
	nop				nop
	nop				b reject
	nop				nop
in5:
	nop				iaddiu vi6, vi6, 32
np5:
	nop				iaddiu vi12, vi0, 1
	nop				ibeq vi8, vi0, store
	nop				nop
	nop				iaddiu vi6, vi6, 0x40
	nop				b store
	nop				nop
reject:
	nop				iaddiu vi6, vi0, 0x7fff
	nop				iaddi vi6, vi6, 1
	nop				b store
	nop				nop
isadc:
	nop				iaddiu vi6, vi0, 0x7fff
	nop				iaddi vi6, vi6, 1
store:
	nop				isw.w vi6, 222(vi3)
.if MXGLC_FOG
	nop				isw.w vi6, 220(vi3)
	nop				b nextm
	nop				iaddiu vi7, vi5, 0
.endif
nextv:
	nop				iaddiu vi7, vi5, 0
.if MXGLC_FOG
	nop				isw.w vi0, 220(vi3)
nextm:
.endif
	nop				iaddi vi3, vi3, 3
	nop				iaddi vi4, vi4, -1
	nop				nop
	nop				ibne vi4, vi0, tri
	nop				nop
kick:
.if MXGLC_FOG
	; F into w over the clip flags, ADC kept
	nop				ilw.x vi4, 0(vi14)
	nop				iaddiu vi3, vi14, 1
	nop				iaddiu vi15, vi0, 0x7fff
	nop				iaddi vi15, vi15, 1
	nop				nop
	nop				ibeq vi4, vi0, nofog
	nop				nop
fogs:
	nop				lq.xyzw vf13, 0(vi3)
	nop				ilw.w vi2, 222(vi3)
	nop				nop
	nop				nop
	mulaw.w acc, vf7, vf0w		nop
	maddax.w acc, vf4, vf13x	nop
	madday.w acc, vf5, vf13y	nop
	maddz.w vf12, vf6, vf13z	iand vi2, vi2, vi15
	mxglc_fog
	ftoi4.w vf26, vf26		nop
	nop				nop
	nop				nop
	nop				nop
	nop				mtir vi1, vf26w
	nop				iaddi vi4, vi4, -1
	nop				ior vi2, vi2, vi1
	nop				isw.w vi2, 222(vi3)
	nop				ibne vi4, vi0, fogs
	nop				iaddi vi3, vi3, 3
nofog:
.endif
	nop				iaddiu vi2, vi14, 220
	nop				nop
	nop				xgkick vi2
	nop				nop
	nop				ibeq vi12, vi0, end
	nop				nop
	; pass 3: the marked triangles
	nop				ilw.x vi4, 0(vi14)
	nop				iaddiu vi3, vi14, 7
	nop				nop
	nop				isubiu vi4, vi4, 2
	nop				nop
	nop				iblez vi4, end
	nop				nop
scan:
.if MXGLC_FOG
	nop				ilw.w vi5, 220(vi3)
.else
	nop				ilw.w vi5, 222(vi3)
.endif
	nop				iaddiu vi2, vi0, 0x3f
	nop				nop
	nop				nop
	nop				iand vi2, vi5, vi2
	nop				nop
	nop				ibeq vi2, vi0, next
	nop				nop
	nop				isw.x vi3, 1010(vi0)
	nop				isw.y vi4, 1010(vi0)
	nop				isw.z vi5, 1010(vi0)
	; the triangle into WS_A
	nop				iaddiu vi6, vi0, 900
	nop				iaddi vi7, vi3, -6
	nop				bal vi15, xf
	nop				nop
	nop				bal vi15, xf
	nop				nop
	nop				bal vi15, xf
	nop				nop
	nop				iaddiu vi3, vi0, 900
	nop				iaddiu vi4, vi0, 927
	nop				iaddiu vi5, vi0, 3
	nop				xitop vi12
	nop				nop
	nop				iaddiu vi12, vi12, 12
	; the planes to cut by: those of the mark; all after the depth planes (a vertex
	; behind the eye is judged against |w|: not marked is not inside)
	nop				ilw.z vi2, 1010(vi0)
	nop				iaddiu vi1, vi0, 0x3f
	nop				iaddiu vi6, vi0, 3
	nop				nop
	nop				iand vi1, vi2, vi1
	nop				iand vi2, vi2, vi6
	nop				nop
	nop				ibeq vi2, vi0, onlythese
	nop				nop
	nop				iaddiu vi1, vi0, 0x3f
onlythese:
	nop				isw.z vi1, 1011(vi0)
	nop				iaddiu vi2, vi0, 1
	nop				isw.y vi2, 1011(vi0)
plane:
	nop				ilw.y vi2, 1011(vi0)
	nop				ilw.z vi1, 1011(vi0)
	nop				nop
	nop				nop
	nop				nop
	nop				iand vi1, vi1, vi2
	nop				nop
	nop				ibeq vi1, vi0, noplane
	nop				nop
	; vi3 from, vi4 to, vi5 vertices; vi6 made; vi1 the next made; vi7 the vertex;
	; vi2 left; vi15 the last was outside; vi9 this one is
	nop				lq.xyzw vf26, 0(vi12)
	nop				iaddiu vi7, vi3, 0
	nop				iaddiu vi2, vi5, 0
	nop				iaddiu vi6, vi0, 0
	nop				mr32.xyzw vf19, vf26
	nop				nop
	nop				nop
	nop				nop
	nop				mr32.xyzw vf25, vf19
	nop				nop
	nop				nop
	nop				nop
	nop				mr32.xyzw vf10, vf25
touch:
	nop				lq.xyzw vf11, 0(vi7)
	nop				iaddi vi7, vi7, 3
	nop				iaddi vi2, vi2, -1
	nop				nop
	mulax.x acc, vf26, vf11x	nop
	madday.x acc, vf19, vf11y	nop
	maddaz.x acc, vf25, vf11z	nop
	maddw.x vf14, vf10, vf11w	nop
	nop				nop
	nop				nop
	nop				nop
	nop				nop
	nop				fsand vi1, 2
	nop				ior vi6, vi6, vi1
	nop				nop
	nop				ibne vi2, vi0, touch
	nop				nop
	nop				nop
	nop				ibeq vi6, vi0, noplane
	nop				nop
	nop				iadd vi2, vi5, vi5
	nop				iadd vi2, vi2, vi5
	nop				isubiu vi2, vi2, 3
	nop				iadd vi7, vi3, vi2
	nop				lq.xyzw vf22, 0(vi7)
	nop				lq.xyzw vf23, 1(vi7)
	nop				lq.xyzw vf24, 2(vi7)
	mulax.x acc, vf26, vf22x	nop
	madday.x acc, vf19, vf22y	nop
	maddaz.x acc, vf25, vf22z	nop
	maddw.x vf27, vf10, vf22w	nop
	nop				nop
	nop				nop
	nop				nop
	nop				nop
	nop				fsand vi15, 2
	nop				iaddiu vi7, vi3, 0
	nop				iaddiu vi1, vi4, 0
	nop				iaddiu vi6, vi0, 0
	nop				iaddiu vi2, vi5, 0
edge:
	nop				lq.xyzw vf11, 0(vi7)
	nop				lq.xyzw vf12, 1(vi7)
	nop				lq.xyzw vf13, 2(vi7)
	mulax.x acc, vf26, vf11x	nop
	madday.x acc, vf19, vf11y	nop
	maddaz.x acc, vf25, vf11z	nop
	maddw.x vf14, vf10, vf11w	nop
	nop				nop
	nop				nop
	nop				nop
	nop				nop
	nop				fsand vi9, 2
	nop				nop
	nop				nop
	nop				ibeq vi9, vi15, same
	nop				nop
	; the plane is between them: a from the inside, b the other
	nop				nop
	nop				ibne vi15, vi0, ainv
	nop				nop
	nop				move.xyzw vf28, vf22
	nop				move.xyzw vf29, vf23
	nop				move.xyzw vf30, vf24
	nop				move.x vf31, vf27
	nop				move.xyzw vf15, vf11
	nop				move.xyzw vf16, vf12
	nop				move.xyzw vf17, vf13
	nop				b cut
	nop				move.x vf18, vf14
ainv:
	nop				move.xyzw vf28, vf11
	nop				move.xyzw vf29, vf12
	nop				move.xyzw vf30, vf13
	nop				move.x vf31, vf14
	nop				move.xyzw vf15, vf22
	nop				move.xyzw vf16, vf23
	nop				move.xyzw vf17, vf24
	nop				move.x vf18, vf27
cut:
	; at most 9 vertices: rounding could cut one polygon more than twice by a plane,
	; and WS_A, WS_B, R0 and R1 hold 9
	nop				isubiu vi8, vi6, 9
	nop				nop
	nop				ibgez vi8, same
	nop				nop
	sub.x vf18, vf31, vf18		nop
	sub.xyzw vf15, vf15, vf28	nop
	sub.xyzw vf16, vf16, vf29	nop
	sub.xyzw vf17, vf17, vf30	div q, vf31x, vf18x
	nop				waitq
	mulq.xyzw vf15, vf15, q		nop
	mulq.xyzw vf16, vf16, q		nop
	mulq.xyzw vf17, vf17, q		nop
	add.xyzw vf15, vf15, vf28	nop
	add.xyzw vf16, vf16, vf29	nop
	add.xyzw vf17, vf17, vf30	nop
	nop				sq.xyzw vf15, 0(vi1)
	nop				sq.xyzw vf16, 1(vi1)
	nop				sq.xyzw vf17, 2(vi1)
	nop				iaddi vi1, vi1, 3
	nop				iaddi vi6, vi6, 1
same:
	nop				isubiu vi8, vi6, 9
	nop				nop
	nop				ibne vi9, vi0, gone
	nop				nop
	nop				ibgez vi8, gone
	nop				nop
	nop				sq.xyzw vf11, 0(vi1)
	nop				sq.xyzw vf12, 1(vi1)
	nop				sq.xyzw vf13, 2(vi1)
	nop				iaddi vi1, vi1, 3
	nop				iaddi vi6, vi6, 1
gone:
	nop				move.xyzw vf22, vf11
	nop				move.xyzw vf23, vf12
	nop				move.xyzw vf24, vf13
	nop				move.x vf27, vf14
	nop				iaddiu vi15, vi9, 0
	nop				iaddi vi7, vi7, 3
	nop				iaddi vi2, vi2, -1
	nop				nop
	nop				ibne vi2, vi0, edge
	nop				nop
	; the next plane, from what was made
	nop				iaddiu vi5, vi6, 0
	nop				iaddiu vi2, vi3, 0
	nop				iaddiu vi3, vi4, 0
	nop				iaddiu vi4, vi2, 0
	nop				isubiu vi2, vi5, 3
	nop				nop
	nop				ibltz vi2, done
	nop				nop
noplane:
	nop				ilw.y vi2, 1011(vi0)
	nop				iaddi vi12, vi12, 1
	nop				nop
	nop				nop
	nop				iadd vi2, vi2, vi2
	nop				isw.y vi2, 1011(vi0)
	nop				isubiu vi2, vi2, 64
	nop				nop
	nop				ibltz vi2, plane
	nop				nop
	; culled? its first three vertices, on the screen
	nop				nop
	nop				ibeq vi10, vi0, send
	nop				nop
	nop				lq.xyzw vf11, 0(vi3)
	nop				lq.xyzw vf12, 3(vi3)
	nop				lq.xyzw vf13, 6(vi3)
	nop				div q, vf0w, vf11w
	nop				waitq
	mulq.xyz vf11, vf11, q		div q, vf0w, vf12w
	nop				waitq
	mulq.xyz vf12, vf12, q		div q, vf0w, vf13w
	nop				waitq
	mulq.xyz vf13, vf13, q		nop
	sub.xyz vf23, vf12, vf11	nop
	sub.xyz vf26, vf13, vf11	nop
	opmula.xyz acc, vf23, vf26	nop
	opmsub.xyz vf27, vf26, vf23	nop
	nop				nop
	nop				nop
	nop				nop
	nop				nop
	nop				fmand vi2, vi10
	nop				ilw.z vi1, 1010(vi0)
	nop				iaddiu vi6, vi0, 0x40
	nop				nop
	nop				nop
	nop				iand vi1, vi1, vi6
	nop				nop
	nop				ibeq vi1, vi0, sameway
	nop				nop
	nop				iaddiu vi1, vi0, 0x20
	nop				iadd vi2, vi2, vi1
sameway:
	nop				iadd vi2, vi2, vi11
	nop				iand vi2, vi2, vi10
	nop				nop
	nop				ibne vi2, vi0, done
	nop				nop
send:
	; the tag, then a strip: 0, 1, n-1, 2, n-2, ...
	nop				xitop vi2
	nop				nop
	nop				nop
	nop				lq.xyzw vf11, 0(vi2)
	nop				ilw.x vi2, 0(vi2)
	nop				nop
	nop				nop
	nop				ior vi2, vi2, vi5
	nop				sq.xyzw vf11, 0(vi13)
	nop				isw.x vi2, 0(vi13)
	nop				iaddi vi1, vi13, 1
	; vi6 the next from the low end, vi2 from the high end, vi9 left
	nop				iaddi vi6, vi3, 3
	nop				iadd vi2, vi5, vi5
	nop				iadd vi2, vi2, vi5
	nop				iadd vi2, vi2, vi3
	nop				isubiu vi2, vi2, 3
	nop				isubiu vi9, vi5, 1
	nop				iaddiu vi7, vi3, 0
	nop				bal vi15, put
	nop				nop
strip:
	nop				iaddiu vi7, vi6, 0
	nop				bal vi15, put
	nop				iaddi vi6, vi6, 3
	nop				iaddi vi9, vi9, -1
	nop				nop
	nop				ibeq vi9, vi0, sent
	nop				nop
	nop				iaddiu vi7, vi2, 0
	nop				bal vi15, put
	nop				isubiu vi2, vi2, 3
	nop				iaddi vi9, vi9, -1
	nop				nop
	nop				ibne vi9, vi0, strip
	nop				nop
sent:
	nop				nop
	nop				xgkick vi13
	nop				iaddiu vi2, vi0, 1936
	nop				isub vi13, vi2, vi13
done:
	nop				ilw.x vi3, 1010(vi0)
	nop				ilw.y vi4, 1010(vi0)
	nop				nop
	nop				nop
next:
	nop				iaddi vi3, vi3, 3
	nop				iaddi vi4, vi4, -1
	nop				nop
	nop				ibne vi4, vi0, scan
	nop				nop
end:
	nop[e]				nop
	nop				nop
	nop				b batch
	nop				nop

; a vertex for the cutting: from the inputs at vi7 to P, ST, C at vi6
xf:
.if MXGLC_COLOUR
	mxglc_colour_load
.endif
	nop				lq.xyzw vf13, 0(vi7)
	nop				lq.xyzw vf15, 1(vi7)
	nop				lq.xyzw vf14, 2(vi7)
	mulaw.xyzw acc, vf7, vf0w	nop
	maddax.xyzw acc, vf4, vf13x	nop
	madday.xyzw acc, vf5, vf13y	nop
	maddz.xyzw vf12, vf6, vf13z	nop
	mulaw.xyzw acc, vf2, vf0w	nop
	maddax.xyzw acc, vf1, vf15x	nop
	maddy.xyzw vf15, vf3, vf15y	nop
.if MXGLC_COLOUR
	mxglc_colour
.else
	itof0.xyzw vf14, vf14		nop
	mulaw.xyzw acc, vf9, vf0w	nop
	madd.xyzw vf17, vf8, vf14	nop
.endif
.if MXGLC_FOG
	mxglc_fog
	nop				move.w vf15, vf26
.endif
	nop				sq.xyzw vf12, 0(vi6)
	nop				sq.xyzw vf15, 1(vi6)
	nop				nop
	nop				nop
	nop				nop
	nop				nop
	nop				sq.xyzw vf17, 2(vi6)
	nop				iaddi vi6, vi6, 3
	nop				iaddi vi7, vi7, 3
	nop				jr vi15
	nop				nop

; a polygon's vertex at vi7 to the packet at vi1; keeps vi2, vi6
put:
	nop				lq.xyzw vf11, 0(vi7)
	nop				lq.xyzw vf12, 1(vi7)
	nop				lq.xyzw vf13, 2(vi7)
	nop				nop
	nop				nop
	nop				nop
.if MXGLC_FOG
	ftoi4.w vf27, vf12		nop
.endif
	nop				div q, vf0w, vf11w
	nop				waitq
	mulq.xyzw vf12, vf12, q		nop
	mulq.xyzw vf14, vf11, q		nop
	addq.z vf12, vf0, q		nop
	ftoi4.xyzw vf16, vf14		nop
	nop				sq.xyzw vf13, 1(vi1)
	nop				sq.xyzw vf12, 0(vi1)
.if MXGLC_FOG
	nop				move.w vf16, vf27
.else
	nop				mfir.w vf16, vi0
.endif
	nop				nop
	nop				sq.xyzw vf16, 2(vi1)
	nop				iaddi vi1, vi1, 3
	nop				jr vi15
	nop				nop
