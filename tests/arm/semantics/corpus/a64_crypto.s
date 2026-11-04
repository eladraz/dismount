// A64 cryptographic instructions: directed cases (zero, all ones, one-hot states, in place forms)
aese v0.16b, v1.16b  // @state: v0=00112233445566778899aabbccddeeff v1=000102030405060708090a0b0c0d0e0f @state: v0=0 v1=0 @state: v0=ffffffffffffffffffffffffffffffff v1=0
aesd v0.16b, v1.16b  // @state: v0=69c4e0d86a7b0430d8cdb78070b4c55a v1=0 @state: v0=0 v1=0
aesmc v0.16b, v1.16b  // @state: v1=db135345f20a225c01010101c6c6c6c6 @state: v1=0 @state: v1=ffffffffffffffffffffffffffffffff
aesimc v0.16b, v1.16b  // @state: v1=8e4da1bc9fdc589d01010101c6c6c6c6 @state: v1=0
aese v0.16b, v0.16b
aesmc v0.16b, v0.16b
sha1c q0, s1, v2.4s  // @state: v0=0 v1=0 v2=0 @state: v0=ffffffffffffffffffffffffffffffff v1=ffffffff v2=ffffffffffffffffffffffffffffffff
sha1m q0, s1, v2.4s  // @state: v0=0 v1=0 v2=0 @state: v0=ffffffffffffffffffffffffffffffff v1=ffffffff v2=ffffffffffffffffffffffffffffffff
sha1p q0, s1, v2.4s  // @state: v0=0 v1=0 v2=0 @state: v0=ffffffffffffffffffffffffffffffff v1=ffffffff v2=ffffffffffffffffffffffffffffffff
sha1h s0, s1  // @state: v1=ffffffff @state: v1=1 @state: v1=80000000
sha1su0 v0.4s, v1.4s, v2.4s
sha1su1 v0.4s, v1.4s
sha256h q0, q1, v2.4s
sha256h2 q0, q1, v2.4s
sha256su0 v0.4s, v1.4s
sha256su1 v0.4s, v1.4s, v2.4s
sha512h q0, q1, v2.2d
sha512h2 q0, q1, v2.2d
sha512su0 v0.2d, v1.2d
sha512su1 v0.2d, v1.2d, v2.2d
sha512h q0, q0, v0.2d
eor3 v0.16b, v1.16b, v2.16b, v3.16b
bcax v0.16b, v1.16b, v2.16b, v3.16b
rax1 v0.2d, v1.2d, v2.2d
xar v0.2d, v1.2d, v2.2d, #0
xar v0.2d, v1.2d, v2.2d, #63
xar v0.2d, v1.2d, v1.2d, #17
sm3ss1 v0.4s, v1.4s, v2.4s, v3.4s
sm3tt1a v0.4s, v1.4s, v2.s[0]
sm3tt1b v0.4s, v1.4s, v2.s[3]
sm3tt2a v0.4s, v1.4s, v2.s[2]
sm3tt2b v0.4s, v1.4s, v2.s[1]
sm3partw1 v0.4s, v1.4s, v2.4s
sm3partw2 v0.4s, v1.4s, v2.4s
sm4e v0.4s, v1.4s  // @state: v0=1032547698badcfeefcdab8967452301 v1=0
sm4ekey v0.4s, v1.4s, v2.4s
pmull v0.8h, v1.8b, v2.8b  // @state: v1=ffffffffffffffff v2=ffffffffffffffff @state: v1=0101010101010101 v2=ffffffffffffffff
pmull2 v0.8h, v1.16b, v2.16b
pmull v0.1q, v1.1d, v2.1d  // @state: v1=ffffffffffffffff v2=ffffffffffffffff @state: v1=8000000000000000 v2=8000000000000000
pmull2 v0.1q, v1.2d, v2.2d  // @state: v1=ffffffffffffffff0000000000000000 v2=ffffffffffffffff0000000000000000
