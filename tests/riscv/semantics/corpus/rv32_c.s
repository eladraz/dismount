# C extension, RV32 only
c.jal 8
c.jal -8
c.flw fa0, 4(a1)
c.fsw fa0, 4(a1)
c.flwsp fa0, 4(sp)
c.fswsp fa1, 4(sp)
