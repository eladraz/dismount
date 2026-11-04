# C extension, RV64 only
c.addiw a0, 1
c.addiw a0, -1
c.subw a0, a1
c.addw a0, a1
c.ld a0, 8(a1)
c.sd a0, 8(a1)
c.ldsp a0, 8(sp)
c.sdsp a1, 8(sp)
c.srli a0, 63
c.slli a0, 63
c.zext.w a0
