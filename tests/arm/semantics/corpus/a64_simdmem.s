// A64 Advanced SIMD structure loads and stores: directed cases (the register list wraps from v31 to v0, the
// post index by an immediate / a register / the stack pointer, a transfer next to the end of the window).
// The window is 0x10100000000 + 0x10000.
ld4 {v30.16b, v31.16b, v0.16b, v1.16b}, [x0]
ld3 {v31.4s, v0.4s, v1.4s}, [x0], #48
ld2 {v31.8h, v0.8h}, [x0], x1  // @state: x1=0 @state: x1=10 @state: x1=ffffffffffffffe0
ld1 {v31.2d, v0.2d, v1.2d, v2.2d}, [x0], #64
ld4 {v0.8b, v1.8b, v2.8b, v3.8b}, [sp]
ld4 {v0.8b, v1.8b, v2.8b, v3.8b}, [sp], #32
st4 {v30.16b, v31.16b, v0.16b, v1.16b}, [x0], x2
st2 {v31.2s, v0.2s}, [sp], #16
st1 {v31.1d, v0.1d}, [x0], #16
ld1 {v31.s}[3], [x0], x1
ld1 {v0.b}[15], [x0], #1
ld1 {v0.h}[7], [x0], #2
ld1 {v0.d}[1], [x0], #8
ld2 {v31.s, v0.s}[2], [x0], #8
ld3 {v31.b, v0.b, v1.b}[9], [x0], x1
ld4 {v31.d, v0.d, v1.d, v2.d}[1], [x0], #32
st1 {v31.s}[3], [x0], x1
st3 {v31.h, v0.h, v1.h}[5], [x0], #6
st4 {v30.b, v31.b, v0.b, v1.b}[15], [x0]
ld1r {v31.16b}, [x0], #1
ld2r {v31.8h, v0.8h}, [x0], x1
ld3r {v31.2s, v0.2s, v1.2s}, [x0], #12
ld4r {v30.2d, v31.2d, v0.2d, v1.2d}, [x0], x1
ld4r {v30.1d, v31.1d, v0.1d, v1.1d}, [x0]
ld1 {v0.16b}, [x0]  // @state: x0=10100000000
ld1 {v0.16b}, [x0]  // @state: x0=1010000fff0
ld1 {v0.16b}, [x0]  // @state: x0=1010000fff1
ld1 {v0.16b}, [x0]  // @state: x0=1010000fffff
ld4 {v0.4s, v1.4s, v2.4s, v3.4s}, [x0]  // @state: x0=1010000ffd0
st1 {v0.16b}, [x0]  // @state: x0=1010000fff1
st4 {v0.4s, v1.4s, v2.4s, v3.4s}, [x0]  // @state: x0=1010000ffd0
ld1 {v0.s}[1], [x0]  // @state: x0=1010000fffd
st1 {v0.d}[1], [x0]  // @state: x0=1010000fffd
