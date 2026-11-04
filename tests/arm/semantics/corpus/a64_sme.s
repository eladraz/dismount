// SME in the streaming mode: only the host runs these (SVL 512; Unicorn has no SME and reports
// ORACLE_ERR). `@mode: sm` puts z / p in the state, `@mode: za` also the ZA array and zt0.
smstart                      // @mode: sm
add z0.s, z1.s, z2.s         // @mode: sm
fmla z0.s, p0/m, z1.s, z2.s  // @mode: sm
rdsvl x0, #1                 // @mode: sm
addsvl x0, x1, #2            // @mode: sm
cntsd x0                     // @mode: sm
zero {za}                    // @mode: za
fmopa za0.s, p0/m, p1/m, z0.s, z1.s  // @mode: za
smopa za1.s, p0/m, p1/m, z0.b, z1.b  // @mode: za
mova z0.s, p0/m, za0h.s[w12, 0]      // @mode: za
mova za1v.d[w13, 1], p2/m, z3.d      // @mode: za
ld1w {za0h.s[w12, 0]}, p0/z, [x4]    // @mode: za
st1b {za0v.b[w12, 3]}, p1, [x4]      // @mode: za
ldr za[w12, 0], [x4]                 // @mode: za
str za[w13, 1], [x4]                 // @mode: za
addha za0.s, p0/m, p1/m, z0.s        // @mode: za
fmla za.s[w8, 0, vgx2], {z0.s, z1.s}, z2.s  // @mode: za
