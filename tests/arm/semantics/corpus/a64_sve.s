// A64 SVE instructions that are legal in the streaming mode (FEAT_SME without FEAT_SME_FA64):
// the host runs them in the streaming mode (SVL 512), Unicorn in SVE mode (VL given by --vl).
// `@mode: sve` puts z, p and ffr in the state.
add z0.s, z1.s, z2.s       // @mode: sve
sub z0.b, z1.b, z2.b       // @mode: sve
add z0.d, z0.d, #5         // @mode: sve
mul z0.h, z0.h, #3         // @mode: sve
mul z0.h, z0.h, z1.h       // @mode: sve   (SVE2 unpredicated: Unicorn has no SVE2)
and z0.d, z1.d, z2.d       // @mode: sve
eor z0.d, z1.d, z2.d       // @mode: sve
orr z0.d, z0.d, #0xff00    // @mode: sve
bic z0.d, z1.d, z2.d       // @mode: sve
lsl z0.s, z1.s, #5         // @mode: sve
lsr z0.h, z1.h, #3         // @mode: sve
asr z0.d, z1.d, #9         // @mode: sve
add z0.s, p0/m, z0.s, z1.s // @mode: sve
sub z0.b, p1/m, z0.b, z1.b // @mode: sve
mul z0.d, p2/m, z0.d, z1.d // @mode: sve
smax z0.s, p3/m, z0.s, z1.s // @mode: sve
umin z0.h, p4/m, z0.h, z1.h // @mode: sve
neg z0.s, p0/m, z1.s       // @mode: sve
abs z0.d, p1/m, z1.d       // @mode: sve
cnt z0.s, p0/m, z1.s       // @mode: sve
clz z0.d, p2/m, z1.d       // @mode: sve
sdiv z0.s, p0/m, z0.s, z1.s // @mode: sve
udiv z0.d, p1/m, z0.d, z1.d // @mode: sve
fadd z0.s, p0/m, z0.s, z1.s // @mode: sve
fsub z0.d, p1/m, z0.d, z1.d // @mode: sve
fmul z0.h, p2/m, z0.h, z1.h // @mode: sve
fdiv z0.s, p0/m, z0.s, z1.s // @mode: sve
fmla z0.s, p0/m, z1.s, z2.s // @mode: sve
fadd z0.s, z1.s, z2.s      // @mode: sve
fmul z0.d, z1.d, z2.d      // @mode: sve
fabs z0.s, p0/m, z1.s      // @mode: sve
fsqrt z0.d, p1/m, z1.d     // @mode: sve
fcvtzs z0.s, p0/m, z1.s    // @mode: sve
scvtf z0.d, p0/m, z1.d     // @mode: sve
faddv s0, p0, z1.s         // @mode: sve
fcmeq p0.s, p1/z, z0.s, z1.s // @mode: sve
cmpgt p0.s, p1/z, z0.s, z1.s // @mode: sve
cmpeq p0.b, p1/z, z0.b, #3   // @mode: sve
and p0.b, p1/z, p2.b, p3.b   // @mode: sve
eor p0.b, p1/z, p2.b, p3.b   // @mode: sve
ptrue p0.s                 // @mode: sve
ptrue p0.b, vl7            // @mode: sve
pfalse p0.b                // @mode: sve
whilelt p0.s, x1, x2       // @mode: sve
whilelo p0.d, x1, x2       // @mode: sve
cntp x0, p0, p1.b          // @mode: sve
cntw x0                    // @mode: sve
cntd x0, all, mul #2       // @mode: sve
incw x0                    // @mode: sve
addvl x0, x1, #3           // @mode: sve
rdvl x0, #2                // @mode: sve
index z0.s, #1, #2         // @mode: sve
index z0.d, x1, x2         // @mode: sve
dup z0.s, w1               // @mode: sve
dup z0.h, #5               // @mode: sve
dup z0.s, z1.s[2]          // @mode: sve
mov z0.d, p0/m, z1.d       // @mode: sve
sel z0.s, p0, z1.s, z2.s   // @mode: sve
zip1 z0.s, z1.s, z2.s      // @mode: sve
uzp2 z0.d, z1.d, z2.d      // @mode: sve
trn1 z0.h, z1.h, z2.h      // @mode: sve
rev z0.s, z1.s             // @mode: sve
splice z0.s, p0, z0.s, z1.s // @mode: sve
uaddv d0, p0, z1.s         // @mode: sve
smaxv s0, p0, z1.s         // @mode: sve
sunpklo z0.s, z1.h         // @mode: sve
uxtb z0.h, p0/m, z1.h      // @mode: sve
ld1w {z0.s}, p0/z, [x4]    // @mode: sve
ld1b {z0.b}, p1/z, [x4, x5]  // @mode: sve
ld1d {z0.d}, p2/z, [x4, #1, mul vl] // @mode: sve
st1w {z0.s}, p0, [x4]      // @mode: sve
st1h {z0.h}, p1, [x4, #2, mul vl] // @mode: sve
ld1rw {z0.s}, p0/z, [x4, #4] // @mode: sve
ld2w {z0.s, z1.s}, p0/z, [x4] // @mode: sve
ldr z0, [x4, #2, mul vl]   // @mode: sve
str z1, [x4, #3, mul vl]   // @mode: sve
ldr p1, [x4, #1, mul vl]   // @mode: sve
str p2, [x4]               // @mode: sve
