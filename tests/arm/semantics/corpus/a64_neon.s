// A64 Advanced SIMD and floating point
// --- scalar floating point
fadd s0, s1, s2
fsub d0, d1, d2
fmul h0, h1, h2
fdiv d0, d1, d2
fsqrt s0, s1
fmadd d0, d1, d2, d3
fnmsub s0, s1, s2, s3
fmax d0, d1, d2
fminnm s0, s1, s2
fabs d0, d1
fneg s0, s1
fcmp d0, d1
fcmpe s0, #0.0
fccmp s0, s1, #5, ne
fcsel d0, d1, d2, gt
fmov d0, #1.5
fmov x0, d1
fmov v0.d[1], x1
fmov s0, w1
frintn d0, d1
frintz s0, s1
frintx d0, d1
fcvt d0, s1
fcvt s0, d1
fcvt h0, s1
fcvtzs w0, s1
fcvtzu x0, d1
fcvtns x0, d1
scvtf d0, x1
ucvtf s0, w1
scvtf d0, w1, #8
frecpe s0, s1
frsqrte d0, d1
frecps s0, s1, s2
// --- vector integer
add v0.4s, v1.4s, v2.4s
sub v0.16b, v1.16b, v2.16b
mul v0.8h, v1.8h, v2.8h
mla v0.4s, v1.4s, v2.4s
and v0.16b, v1.16b, v2.16b
orr v0.8b, v1.8b, v2.8b
eor v0.16b, v1.16b, v2.16b
bic v0.16b, v1.16b, v2.16b
bsl v0.16b, v1.16b, v2.16b
bit v0.16b, v1.16b, v2.16b
not v0.16b, v1.16b
neg v0.2d, v1.2d
abs v0.4s, v1.4s
cmeq v0.4s, v1.4s, v2.4s
cmgt v0.8h, v1.8h, #0
cmhi v0.16b, v1.16b, v2.16b
shl v0.4s, v1.4s, #3
ushr v0.2d, v1.2d, #7
sshr v0.8h, v1.8h, #4
ssra v0.4s, v1.4s, #9
sshl v0.4s, v1.4s, v2.4s
sqadd v0.8h, v1.8h, v2.8h
uqsub v0.16b, v1.16b, v2.16b
sqrdmulh v0.4s, v1.4s, v2.4s
smax v0.4s, v1.4s, v2.4s
umin v0.8h, v1.8h, v2.8h
uabd v0.16b, v1.16b, v2.16b
saddl v0.8h, v1.8b, v2.8b
umull2 v0.2d, v1.4s, v2.4s
uaddlv h0, v1.16b
addv s0, v1.4s
addp v0.4s, v1.4s, v2.4s
cnt v0.16b, v1.16b
clz v0.4s, v1.4s
rev64 v0.4s, v1.4s
xtn v0.8b, v1.8h
sqxtn v0.4h, v1.4s
uzp1 v0.4s, v1.4s, v2.4s
zip2 v0.8h, v1.8h, v2.8h
trn1 v0.16b, v1.16b, v2.16b
ext v0.16b, v1.16b, v2.16b, #5
tbl v0.16b, {v1.16b}, v2.16b
dup v0.4s, w1
dup v0.8h, v1.h[3]
ins v0.s[2], w1
umov w0, v1.b[7]
smov x0, v1.h[1]
movi v0.4s, #0x12, lsl #8
mvni v0.4h, #0xff
// --- vector floating point
fadd v0.4s, v1.4s, v2.4s
fsub v0.2d, v1.2d, v2.2d
fmul v0.8h, v1.8h, v2.8h
fdiv v0.4s, v1.4s, v2.4s
fmla v0.4s, v1.4s, v2.4s
fmla v0.4s, v1.4s, v2.s[1]
fmax v0.4s, v1.4s, v2.4s
fabs v0.2d, v1.2d
fcmeq v0.4s, v1.4s, v2.4s
fcmgt v0.2d, v1.2d, #0.0
faddp v0.4s, v1.4s, v2.4s
fcvtzs v0.4s, v1.4s
scvtf v0.2d, v1.2d
frintn v0.4s, v1.4s
fcvtl v0.2d, v1.2s
fcvtn v0.4h, v1.4s
fsqrt v0.2d, v1.2d
frecpe v0.4s, v1.4s
// --- vector memory
ld1 {v0.4s}, [x4]
ld1 {v0.16b, v1.16b}, [x4], #32
st1 {v0.2d}, [x4]
ld2 {v0.4s, v1.4s}, [x4]
ld1r {v0.8h}, [x4]
ld1 {v0.s}[2], [x4]
st4 {v0.8b, v1.8b, v2.8b, v3.8b}, [x4]
ldr q0, [x4, #16]
str d0, [x4, #8]
ldr s0, [x4, #4]
ldp q0, q1, [x4]
// --- crypto
aese v0.16b, v1.16b
aesmc v0.16b, v1.16b
sha1h s0, s1
sha256h q0, q1, v2.4s
pmull v0.1q, v1.1d, v2.1d
// --- directed states: the saturation flag (FPSR.QC), the shift counts, the rounding, the NaNs
sqadd v0.4s, v1.4s, v2.4s   // @state: v1=7fffffff80000000ffffffff00000001 v2=000000018000000001000000ffffffff
uqadd v0.16b, v1.16b, v2.16b   // @state: v1=ff80017f00ff1020ffffffffffffffff v2=01ff0101ff01f0f0ff00ff00ff00ff00
sqsub v0.8h, v1.8h, v2.8h   // @state: v1=8000800080007fff7fff8000ffff0000 v2=00018000ffff7fff80008000ffff0001
sqshl v0.4s, v1.4s, #31   // @state: v1=00000001ffffffff8000000040000000
sqshlu v0.2d, v1.2d, #63   // @state: v1=0000000000000001ffffffffffffffff
uqshl v0.8b, v1.8b, v2.8b   // @state: v1=ff80017f00ff1020 v2=01ff0101ff01f0f0
sqrdmulh v0.8h, v1.8h, v2.8h   // @state: v1=80008000800080007fff7fff7fff7fff v2=8000800080007fff80007fff80007fff
sqdmull v0.2d, v1.2s, v2.2s   // @state: v1=8000000080000000 v2=8000000080000000
sqxtun v0.8b, v1.8h   // @state: v1=ffff80000000ffff7fff0100fffe0001
srshr v0.4s, v1.4s, #1   // @state: v1=7fffffff80000000ffffffff00000001
urshr v0.2d, v1.2d, #64   // @state: v1=ffffffffffffffff8000000000000000
sshl v0.4s, v1.4s, v2.4s   // @state: v1=80000000800000008000000080000000 v2=000000200000001f00000000ffffffff
srshl v0.16b, v1.16b, v2.16b   // @state: v1=8081827f7e7d7c7b7a79787776757473 v2=ff01fe02fd03fc04fb05fa06f907f808
sli v0.4s, v1.4s, #31   // @state: v0=ffffffffffffffffffffffffffffffff v1=00000001ffffffff8000000040000000
sri v0.2d, v1.2d, #64   // @state: v0=ffffffffffffffffffffffffffffffff v1=ffffffffffffffff0000000000000001
neg v0.2d, v1.2d   // @state: v1=80000000000000000000000000000000
sqneg v0.4s, v1.4s   // @state: v1=80000000800000007fffffff00000000
abs v0.8h, v1.8h   // @state: v1=80008000ffff00017fff80010000fffe
fadd v0.4s, v1.4s, v2.4s   // @state: v1=7fc0000000000001ff8000007f800000 v2=3f8000003f800000ff80000000000001 fpcr=0
fmax v0.2d, v1.2d, v2.2d   // @state: v1=7ff00000000000010000000000000000 v2=3ff00000000000008000000000000000 fpcr=2000000
fminnm v0.4s, v1.4s, v2.4s   // @state: v1=7fc000003f800000bf8000007f800001 v2=3f8000007fc000007f800001bf800000 fpcr=0
fmla v0.2s, v1.2s, v2.2s   // @state: v0=7fc0000000000000 v1=7f8000000000000a v2=00000000ff800000 fpcr=3000000
fcvtzs v0.4s, v1.4s   // @state: v1=7fc0000050000000d0000000bf800000 fpcr=0 fpsr=0
fcvtzu v0.2d, v1.2d   // @state: v1=bff0000000000000c3e0000000000000
frintx v0.4s, v1.4s   // @state: v1=3fc000007f80000000000001ff800000 fpcr=c00000
fcvt h0, s1   // @state: v1=33800000 fpcr=4000000
fcvt s0, h1   // @state: v1=7c01 fpcr=4000000
