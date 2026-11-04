// A64 extensions beyond ARMv8.0 that the oracles may or may not implement the same way (FP16,
// FHM, dot product, RDM, FCMA, JSCVT, FRINTTS, BF16, I8MM, SHA3, SM3/SM4, RCPC, FlagM, PAuth
// free instructions). Where Unicorn (QEMU 5.0) lacks the feature the line is UNDEF there.
fjcvtzs w0, d1
frint32z s0, s1
frint64x d0, d1
frint32x v0.4s, v1.4s
bfcvt h0, s1
bfdot v0.4s, v1.8h, v2.8h
bfmmla v0.4s, v1.8h, v2.8h
smmla v0.4s, v1.16b, v2.16b
usdot v0.4s, v1.16b, v2.16b
sdot v0.4s, v1.16b, v2.16b
udot v0.2s, v1.8b, v2.4b[1]
fmlal v0.4s, v1.4h, v2.4h
fmlsl2 v0.2s, v1.2h, v2.2h
sqrdmlah v0.4s, v1.4s, v2.4s
sqrdmlsh v0.8h, v1.8h, v2.h[3]
fcmla v0.4s, v1.4s, v2.4s, #90
fcadd v0.2d, v1.2d, v2.2d, #270
fadd v0.8h, v1.8h, v2.8h
fmulx v0.4s, v1.4s, v2.4s
fmulx d0, d1, d2
frecpx d0, d1
frecps v0.2d, v1.2d, v2.2d
frsqrts s0, s1, s2
fcvtxn v0.2s, v1.2d
fcvtpu v0.2d, v1.2d
fabd v0.4s, v1.4s, v2.4s
facge v0.4s, v1.4s, v2.4s
fmaxnmv s0, v1.4s
fminp v0.4s, v1.4s, v2.4s
fcvtas x0, d1
fcvtzs d0, d1, #12
ucvtf h0, w1
eor3 v0.16b, v1.16b, v2.16b, v3.16b
bcax v0.16b, v1.16b, v2.16b, v3.16b
rax1 v0.2d, v1.2d, v2.2d
xar v0.2d, v1.2d, v2.2d, #19
sha512h q0, q1, v2.2d
sm3partw1 v0.4s, v1.4s, v2.4s
sm4e v0.4s, v1.4s
sha256su1 v0.4s, v1.4s, v2.4s
aesd v0.16b, v1.16b
aesimc v0.16b, v1.16b
pmull2 v0.8h, v1.16b, v2.16b
tbx v0.16b, {v1.16b, v2.16b}, v3.16b
sdot v0.4s, v1.16b, v2.4b[3]
addv b0, v1.16b
sqdmulh v0.8h, v1.8h, v2.8h
sqshl v0.4s, v1.4s, #5
sqshlu v0.2d, v1.2d, #17
uqrshrn v0.4h, v1.4s, #7
srshr v0.4s, v1.4s, #3
urshl v0.2d, v1.2d, v2.2d
raddhn v0.8b, v1.8h, v2.8h
sabal v0.4s, v1.4h, v2.4h
shadd v0.4s, v1.4s, v2.4s
urhadd v0.16b, v1.16b, v2.16b
mls v0.4s, v1.4s, v2.s[2]
pmul v0.16b, v1.16b, v2.16b
rev32 v0.8h, v1.8h
rbit v0.16b, v1.16b
urecpe v0.4s, v1.4s
ursqrte v0.2s, v1.2s
suqadd v0.4s, v1.4s
usqadd v0.2d, v1.2d
sqabs v0.8h, v1.8h
sqneg d0, d1
ldapr x0, [x4]
ldapur w0, [x4, #-4]
ldapur w0, [x4, #-3]
cfinv
setf8 w0
rmif x0, #3, #5
axflag
xaflag
ldaprb w0, [x4]
casp x0, x1, x2, x3, [x4]
ldaddal x1, x0, [x4]
ldsmax w1, w0, [x4]
stumin x1, [x4]
swpal x1, x0, [x4]
prfm pldl1keep, [x4]
dmb ish
dsb sy
isb
yield
