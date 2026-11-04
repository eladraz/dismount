# RV32 only: aes32 and sha512 halves, amocas.d with register pairs
aes32dsi a0, a1, a2, 1
aes32dsmi a0, a1, a2, 2
aes32esi a0, a1, a2, 3
aes32esmi a0, a1, a2, 0
sha512sig0h a0, a1, a2
sha512sig0l a0, a1, a2
sha512sig1h a0, a1, a2
sha512sig1l a0, a1, a2
sha512sum0r a0, a1, a2
sha512sum1r a0, a1, a2
