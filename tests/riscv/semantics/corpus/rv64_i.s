# RV64I only
addw a0, a1, a2
subw a0, a1, a2
sllw a0, a1, a2
srlw a0, a1, a2
sraw a0, a1, a2
addiw a0, a1, 1
addiw a0, a1, -1
addiw a0, a1, 2047
slliw a0, a1, 1
slliw a0, a1, 31
srliw a0, a1, 0
srliw a0, a1, 7
sraiw a0, a1, 3
slli a0, a1, 32
slli a0, a1, 63
srli a0, a1, 63
srai a0, a1, 40
srai a0, a1, 63
lwu a0, 0(a1)
lwu a0, 4(a1)
ld a0, 0(a1)
ld a0, -8(a1)
ld a0, 2040(a1)
sd a2, 0(a1)
sd a2, -8(a1)
sd a2, 2040(a1)
addw zero, a1, a2
lwu zero, 0(a1)
