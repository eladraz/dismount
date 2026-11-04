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
