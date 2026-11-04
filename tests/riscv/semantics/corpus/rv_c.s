# C extension (common); the compressed instructions are lifted by their expansion
c.nop
c.addi a0, 1
c.addi a0, -32
c.addi a0, 31
c.li a0, 5
c.li a0, -1
c.lui a0, 1
c.lui a0, 31
c.lui a0, 0xfffe1
c.addi16sp sp, 16
c.addi16sp sp, -512
c.addi4spn a0, sp, 8
c.addi4spn a0, sp, 1020
c.slli a0, 1
c.slli a0, 31
c.srli a0, 1
c.srai a0, 3
c.andi a0, 7
c.andi a0, -1
c.mv a0, a1
c.add a0, a1
c.and a0, a1
c.or a0, a1
c.xor a0, a1
c.sub a0, a1
c.lw a0, 4(a1)
c.lw a0, 124(a1)
c.sw a0, 4(a1)
c.sw a0, 124(a1)
c.lwsp a0, 4(sp)
c.lwsp a0, 252(sp)
c.swsp a1, 4(sp)
c.swsp a1, 252(sp)
c.fld fa0, 8(a1)
c.fsd fa0, 8(a1)
c.fldsp fa0, 8(sp)
c.fsdsp fa1, 8(sp)
c.j 8
c.j -8
c.j 2046
c.beqz a0, 8
c.bnez a0, -8
c.jr a1
c.jalr a1
c.jr ra
c.ebreak
c.lbu a0, 1(a1)
c.lhu a0, 2(a1)
c.lh a0, 2(a1)
c.sb a0, 1(a1)
c.sh a0, 2(a1)
c.zext.b a0
c.sext.b a0
c.zext.h a0
c.sext.h a0
c.not a0
c.mul a0, a1
