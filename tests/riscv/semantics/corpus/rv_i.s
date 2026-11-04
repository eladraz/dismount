# RV32I / RV64I common instructions (run for both: run_semantics.py --arch rv64|rv32 --file rv_i)
add a0, a1, a2
sub a0, a1, a2
sll a0, a1, a2
slt a0, a1, a2
sltu a0, a1, a2
xor a0, a1, a2
srl a0, a1, a2
sra a0, a1, a2
or a0, a1, a2
and a0, a1, a2
add x5, x5, x6
add a0, zero, a2
add a0, a1, zero
sub a0, a0, a0
add zero, a1, a2
addi a0, a1, 0
addi a0, a1, 1
addi a0, a1, -1
addi a0, a1, 2047
addi a0, a1, -2048
addi a0, a1, 255
slti a0, a1, 0
slti a0, a1, 1
slti a0, a1, -1
slti a0, a1, 2047
slti a0, a1, -2048
slti a0, a1, 255
sltiu a0, a1, 0
sltiu a0, a1, 1
sltiu a0, a1, -1
sltiu a0, a1, 2047
sltiu a0, a1, -2048
sltiu a0, a1, 255
xori a0, a1, 0
xori a0, a1, 1
xori a0, a1, -1
xori a0, a1, 2047
xori a0, a1, -2048
xori a0, a1, 255
ori a0, a1, 0
ori a0, a1, 1
ori a0, a1, -1
ori a0, a1, 2047
ori a0, a1, -2048
ori a0, a1, 255
andi a0, a1, 0
andi a0, a1, 1
andi a0, a1, -1
andi a0, a1, 2047
andi a0, a1, -2048
andi a0, a1, 255
slli a0, a1, 0
slli a0, a1, 1
slli a0, a1, 7
slli a0, a1, 31
srli a0, a1, 0
srli a0, a1, 1
srli a0, a1, 7
srli a0, a1, 31
srai a0, a1, 0
srai a0, a1, 1
srai a0, a1, 7
srai a0, a1, 31
lui a0, 0
lui a0, 1
lui a0, 0x80000
lui a0, 0xfffff
lui a0, 0x7ffff
lui zero, 5
auipc a0, 0
auipc a0, 1
auipc a0, 0xfffff
auipc a0, 0x80000
lb a0, 0(a1)
lb a0, 4(a1)
lb a0, -8(a1)
lb a0, 2040(a1)
lb a0, -2048(a1)
lh a0, 0(a1)
lh a0, 4(a1)
lh a0, -8(a1)
lh a0, 2040(a1)
lh a0, -2048(a1)
lw a0, 0(a1)
lw a0, 4(a1)
lw a0, -8(a1)
lw a0, 2040(a1)
lw a0, -2048(a1)
lbu a0, 0(a1)
lbu a0, 4(a1)
lbu a0, -8(a1)
lbu a0, 2040(a1)
lbu a0, -2048(a1)
lhu a0, 0(a1)
lhu a0, 4(a1)
lhu a0, -8(a1)
lhu a0, 2040(a1)
lhu a0, -2048(a1)
lw zero, 0(a1)
lw a1, 0(a1)
sb a2, 0(a1)
sb a2, 4(a1)
sb a2, -8(a1)
sb a2, 2040(a1)
sh a2, 0(a1)
sh a2, 4(a1)
sh a2, -8(a1)
sh a2, 2040(a1)
sw a2, 0(a1)
sw a2, 4(a1)
sw a2, -8(a1)
sw a2, 2040(a1)
beq a0, a1, 8
beq a0, a1, -8
beq a0, a1, 6
beq a0, a1, 4094
bne a0, a1, 8
bne a0, a1, -8
bne a0, a1, 6
bne a0, a1, 4094
blt a0, a1, 8
blt a0, a1, -8
blt a0, a1, 6
blt a0, a1, 4094
bge a0, a1, 8
bge a0, a1, -8
bge a0, a1, 6
bge a0, a1, 4094
bltu a0, a1, 8
bltu a0, a1, -8
bltu a0, a1, 6
bltu a0, a1, 4094
bgeu a0, a1, 8
bgeu a0, a1, -8
bgeu a0, a1, 6
bgeu a0, a1, 4094
jal ra, 8
jal x0, -8
jal a0, 1000
jal zero, 6
jalr ra, 0(a1)
jalr a0, 4(a1)
jalr zero, 0(ra)
jalr a1, 0(a1)
jalr x0, 0(x6)
fence
fence rw, rw
fence i, o
fence.tso
fence.i
ecall
ebreak
nop
pause
csrrw a0, fflags, a1
csrrs a0, frm, zero
csrrw a0, fcsr, a1
csrrs a0, fcsr, a1
csrrc a0, fcsr, a1
csrrwi a0, fflags, 5
csrrsi a0, frm, 3
csrrci a0, fcsr, 31
csrr a0, fcsr
csrw frm, a1
csrrw zero, fcsr, a1
csrrs zero, fflags, zero
csrwi fcsr, 31
csrrw a0, frm, a1
