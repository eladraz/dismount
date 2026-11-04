# Zba, Zbb, Zbs, Zbkb (RV64 only)
add.uw a0, a1, a2
sh1add.uw a0, a1, a2
sh2add.uw a0, a1, a2
sh3add.uw a0, a1, a2
slli.uw a0, a1, 3
slli.uw a0, a1, 40
zext.w a0, a1
clzw a0, a1
ctzw a0, a1
cpopw a0, a1
rolw a0, a1, a2
rorw a0, a1, a2
roriw a0, a1, 5
packw a0, a1, a2
rori a0, a1, 40
rori a0, a1, 63
bclri a0, a1, 63
bexti a0, a1, 40
binvi a0, a1, 32
bseti a0, a1, 63
