#!/usr/bin/env python3
"""The instruction lists of the encoding tables: the mnemonics, the operand
tokens (riscvdsl.py), the features. The order is the order of the decoder: an
alias before the general encoding it prints for. gen_encoding_tables.py
derives the masks and the values with llvm-mc."""
import riscvdsl

A = 'RISCV_ENTRY_ALIAS'
ASM = 'RISCV_ENTRY_ASSEMBLE_ONLY'
HINT = 'RISCV_ENTRY_HINT'
PRIV = 'RISCV_ENTRY_PRIVILEGED'
SER = 'RISCV_ENTRY_SERIALIZING'
EXP = 'RISCV_ENTRY_EXPANSION_ALIAS'
DEC = 'RISCV_ENTRY_DECODE_ONLY'
UNG = 'RISCV_ENTRY_DECODE_UNGATED'
JUMP, CALL, COND, JREG, CREG, RET, TRAP = ('FLOW_JUMP', 'FLOW_CALL', 'FLOW_COND', 'FLOW_JUMP_REGISTER',
                                           'FLOW_CALL_REGISTER', 'FLOW_RETURN', 'FLOW_TRAP')


def define(ins, table):
    tie = riscvdsl.tie

    # ------------------------------------------------------------------
    table('base_int')
    ins('lpad', ['imm_lpad'], 'ZICFILP', [A])
    ins('lui', ['rd', 'imm_u'], 'I')
    ins('auipc', ['rd', 'imm_u'], 'I')
    ins('j', ['br_j'], 'I', [A], JUMP, probe='j {0}')
    ins('jal', ['br_j'], 'I', [A], CALL, probe='jal {0}')
    ins('jal', ['rd', 'br_j'], 'I', [], CALL)
    ins('ret', [], 'I', [A], RET, probe='ret')
    ins('jr', ['rs1'], 'I', [A], JREG, probe='jr {0}')
    ins('jr', ['mem_i'], 'I', [A], JREG, probe='jr {0}')
    ins('jalr', ['rs1'], 'I', [A], CREG, probe='jalr {0}')
    ins('jalr', ['mem_i'], 'I', [A], CREG, probe='jalr ra, {0}')
    ins('jalr', ['rd', 'rs1'], 'I', [A], CREG, probe='jalr {0}, {1}')
    ins('jalr', ['rd', 'mem_i'], 'I', [], CREG)
    # (read by LLVM, never printed: jalr rd, rs1, offset)
    ins('jalr', ['rd', 'rs1', 'imm_i'], 'I', [ASM], CREG)
    ins('beqz', ['rs1', 'br_b'], 'I', [A], COND, probe='beqz {0}, {1}')
    ins('bnez', ['rs1', 'br_b'], 'I', [A], COND, probe='bnez {0}, {1}')
    ins('blez', ['rs2', 'br_b'], 'I', [A], COND, probe='blez {0}, {1}')
    ins('bgez', ['rs1', 'br_b'], 'I', [A], COND, probe='bgez {0}, {1}')
    ins('bltz', ['rs1', 'br_b'], 'I', [A], COND, probe='bltz {0}, {1}')
    ins('bgtz', ['rs2', 'br_b'], 'I', [A], COND, probe='bgtz {0}, {1}')
    for mn, base in (('beq', 0), ('bne', 1), ('blt', 4), ('bge', 5), ('bltu', 6), ('bgeu', 7)):
        ins(mn, ['rs1', 'rs2', 'br_b'], 'I', [], COND)
    # (read by LLVM, never printed: the operands are swapped)
    for mn, real in (('bgt', 'blt'), ('ble', 'bge'), ('bgtu', 'bltu'), ('bleu', 'bgeu')):
        ins(mn, ['rs2', 'rs1', 'br_b'], 'I', [ASM], COND, probe=mn + ' {0}, {1}, {2}')
    for mn in ('lb', 'lh', 'lw', 'lbu', 'lhu'):
        ins(mn, ['rd', 'mem_i'], 'I')
    ins('lwu', ['rd', 'mem_i'], 'I', rv=64)
    ins('ld', ['rd', 'mem_i'], 'I', rv=64)
    for mn in ('sb', 'sh', 'sw'):
        ins(mn, ['rs2', 'mem_s'], 'I')
    ins('sd', ['rs2', 'mem_s'], 'I', rv=64)
    # the prefetch hints (ori x0, rs1, offset)
    for mn in ('prefetch.i', 'prefetch.r', 'prefetch.w'):
        ins(mn, ['mem_prefetch'], 'ZICBOP', [HINT])
    ins('nop', [], 'I', [A, HINT], probe='nop')
    ins('li', ['rd', 'imm_i'], 'I', [A], probe='li {0}, {1}')
    ins('mv', ['rd', 'rs1'], 'I', [A], probe='mv {0}, {1}')
    ins('addi', ['rd', 'rs1', 'imm_i'], 'I')
    ins('slti', ['rd', 'rs1', 'imm_i'], 'I')
    ins('seqz', ['rd', 'rs1'], 'I', [A], probe='seqz {0}, {1}')
    ins('sltiu', ['rd', 'rs1', 'imm_i'], 'I')
    ins('not', ['rd', 'rs1'], 'I', [A], probe='not {0}, {1}')
    ins('xori', ['rd', 'rs1', 'imm_i'], 'I')
    ins('ori', ['rd', 'rs1', 'imm_i'], 'I')
    ins('zext.b', ['rd', 'rs1'], 'I', [A], probe='zext.b {0}, {1}')
    ins('andi', ['rd', 'rs1', 'imm_i'], 'I')
    ins('slli', ['rd', 'rs1', 'shamt6'], 'I')
    ins('srli', ['rd', 'rs1', 'shamt6'], 'I')
    ins('srai', ['rd', 'rs1', 'shamt6'], 'I')
    ins('sext.w', ['rd', 'rs1'], 'I', [A], rv=64, probe='sext.w {0}, {1}')
    ins('addiw', ['rd', 'rs1', 'imm_i'], 'I', rv=64)
    ins('slliw', ['rd', 'rs1', 'shamt5'], 'I', rv=64)
    ins('srliw', ['rd', 'rs1', 'shamt5'], 'I', rv=64)
    ins('sraiw', ['rd', 'rs1', 'shamt5'], 'I', rv=64)
    ins('neg', ['rd', 'rs2'], 'I', [A], probe='neg {0}, {1}')
    ins('negw', ['rd', 'rs2'], 'I', [A], rv=64, probe='negw {0}, {1}')
    ins('snez', ['rd', 'rs2'], 'I', [A], probe='snez {0}, {1}')
    ins('sltz', ['rd', 'rs1'], 'I', [A], probe='sltz {0}, {1}')
    ins('sgtz', ['rd', 'rs2'], 'I', [A], probe='sgtz {0}, {1}')
    for feat in ('ZBB', 'ZBKB'):
        ins('zext.h', ['rd', 'rs1'], feat, [], rv=32, probe='zext.h {0}, {1}')
        ins('zext.h', ['rd', 'rs1'], feat, [], rv=64, probe='zext.h {0}, {1}')
    ins('zext.w', ['rd', 'rs1'], 'ZBA', [A], rv=64, probe='zext.w {0}, {1}')
    # the non temporal hints (add x0, x0, x2-x5)
    for mn in ('ntl.p1', 'ntl.pall', 'ntl.s1', 'ntl.all'):
        ins(mn, [], 'ZIHINTNTL', [A, HINT], probe=mn)
    # (only the expansion of c.mv prints as mv: LLVM prints add rd, zero, rs2)
    ins('mv', ['rd', 'rs2'], 'I', [A, EXP, DEC], probe='add {0}, zero, {1}')
    for mn in ('add', 'sub', 'sll', 'slt', 'sltu', 'xor', 'srl', 'sra', 'or', 'and'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'I')
    for mn in ('addw', 'subw', 'sllw', 'srlw', 'sraw'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'I', rv=64)
    ins('pause', [], 'ZIHINTPAUSE', [A, HINT], probe='pause')
    ins('fence.tso', [], 'I', [A], probe='fence.tso')
    ins('fence', [], 'I', [A], probe='fence')
    ins('fence', ['fence_p', 'fence_s'], 'I', [])
    ins('fence.i', [], 'ZIFENCEI', [SER])
    ins('ecall', [], 'I')
    ins('ebreak', [], 'I', [], TRAP)
    ins('unimp', [], 'ZICSR', [], TRAP, probe='unimp')

    # ------------------------------------------------------------------
    table('base_csr')
    # csrrs
    for mn, number in (('rdcycle', 0xc00), ('rdtime', 0xc01), ('rdinstret', 0xc02)):
        ins(mn, ['rd'], 'ZICNTR', [A], probe=mn + ' {0}')
    for mn in ('rdcycleh', 'rdtimeh', 'rdinstreth'):
        ins(mn, ['rd'], 'ZICNTR', [A], rv=32, probe=mn + ' {0}')
    for mn in ('frcsr', 'frrm', 'frflags'):
        ins(mn, ['rd'], 'F', [A], probe=mn + ' {0}')
    ins('csrr', ['rd', 'csr'], 'ZICSR', [A], probe='csrr {0}, {1}')
    ins('csrs', ['csr', 'rs1'], 'ZICSR', [A], probe='csrs {0}, {1}')
    ins('csrrs', ['rd', 'csr', 'rs1'], 'ZICSR')
    # csrrw
    for mn in ('fscsr', 'fsrm', 'fsflags'):
        ins(mn, ['rs1'], 'F', [A], probe=mn + ' {0}')
        ins(mn, ['rd', 'rs1'], 'F', [A], probe=mn + ' {0}, {1}')
    ins('csrw', ['csr', 'rs1'], 'ZICSR', [A], probe='csrw {0}, {1}')
    ins('csrrw', ['rd', 'csr', 'rs1'], 'ZICSR')
    # csrrc
    ins('csrc', ['csr', 'rs1'], 'ZICSR', [A], probe='csrc {0}, {1}')
    ins('csrrc', ['rd', 'csr', 'rs1'], 'ZICSR')
    # the immediate forms
    for mn in ('fsrmi', 'fsflagsi'):
        ins(mn, ['imm_csr_uimm5'], 'F', [A], probe=mn + ' {0}')
        ins(mn, ['rd', 'imm_csr_uimm5'], 'F', [A], probe=mn + ' {0}, {1}')
    ins('csrwi', ['csr', 'imm_csr_uimm5'], 'ZICSR', [A], probe='csrwi {0}, {1}')
    ins('csrrwi', ['rd', 'csr', 'imm_csr_uimm5'], 'ZICSR')
    ins('csrsi', ['csr', 'imm_csr_uimm5'], 'ZICSR', [A], probe='csrsi {0}, {1}')
    ins('csrrsi', ['rd', 'csr', 'imm_csr_uimm5'], 'ZICSR')
    ins('csrci', ['csr', 'imm_csr_uimm5'], 'ZICSR', [A], probe='csrci {0}, {1}')
    ins('csrrci', ['rd', 'csr', 'imm_csr_uimm5'], 'ZICSR')

    # ------------------------------------------------------------------
    table('base_m_a')
    for mn in ('mul', 'mulh', 'mulhsu', 'mulhu'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'ZMMUL')
    ins('mulw', ['rd', 'rs1', 'rs2'], 'ZMMUL', rv=64)
    for mn in ('div', 'divu', 'rem', 'remu'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'M')
    for mn in ('divw', 'divuw', 'remw', 'remuw'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'M', rv=64)
    for size, rv in (('w', None), ('d', 64)):
        ins('lr.' + size, ['aqrl', 'rd', 'mem0'], 'ZALRSC', rv=rv, probe=None)
        ins('sc.' + size, ['aqrl', 'rd', 'rs2', 'mem0'], 'ZALRSC', rv=rv)
        for amo in ('amoswap', 'amoadd', 'amoxor', 'amoand', 'amoor', 'amomin', 'amomax',
                    'amominu', 'amomaxu'):
            ins('%s.%s' % (amo, size), ['aqrl', 'rd', 'rs2', 'mem0'], 'ZAAMO', rv=rv)
    for size in ('b', 'h'):
        for amo in ('amoswap', 'amoadd', 'amoxor', 'amoand', 'amoor', 'amomin', 'amomax',
                    'amominu', 'amomaxu'):
            ins('%s.%s' % (amo, size), ['aqrl', 'rd', 'rs2', 'mem0'], 'ZABHA')
    ins('amocas.w', ['aqrl', 'rd', 'rs2', 'mem0'], 'ZACAS')
    ins('amocas.d', ['aqrl', 'rd_even', 'rs2_even', 'mem0'], 'ZACAS', rv=32)
    ins('amocas.d', ['aqrl', 'rd', 'rs2', 'mem0'], 'ZACAS', rv=64)
    ins('amocas.q', ['aqrl', 'rd_even', 'rs2_even', 'mem0'], 'ZACAS', rv=64)
    ins('amocas.b', ['aqrl', 'rd', 'rs2', 'mem0'], 'ZABHA', feat2='ZACAS')
    ins('amocas.h', ['aqrl', 'rd', 'rs2', 'mem0'], 'ZABHA', feat2='ZACAS')
    # Zicfiss
    ins('ssamoswap.w', ['aqrl', 'rd', 'rs2', 'mem0'], 'ZICFISS')
    ins('ssamoswap.d', ['aqrl', 'rd', 'rs2', 'mem0'], 'ZICFISS', rv=64)

    # ------------------------------------------------------------------
    table('base_fp')
    fmts = (('s', 'F', 'F'), ('d', 'D', 'D'), ('h', 'ZFH', 'ZFHMIN'))
    for f, feat, minfeat in fmts:
        for mn in ('fmadd', 'fmsub', 'fnmsub', 'fnmadd'):
            ins('%s.%s' % (mn, f), ['frd', 'frs1', 'frs2', 'frs3', 'rm'], feat)
        for mn in ('fadd', 'fsub', 'fmul', 'fdiv'):
            ins('%s.%s' % (mn, f), ['frd', 'frs1', 'frs2', 'rm'], feat)
        ins('fsqrt.' + f, ['frd', 'frs1', 'rm'], feat)
        ins('fmv.' + f, ['frd', 'frs1', tie(20, 15)], feat, [A], probe='fmv.%s {0}, {1}' % f)
        ins('fneg.' + f, ['frd', 'frs1', tie(20, 15)], feat, [A], probe='fneg.%s {0}, {1}' % f)
        ins('fabs.' + f, ['frd', 'frs1', tie(20, 15)], feat, [A], probe='fabs.%s {0}, {1}' % f)
        for mn in ('fsgnj', 'fsgnjn', 'fsgnjx', 'fmin', 'fmax'):
            ins('%s.%s' % (mn, f), ['frd', 'frs1', 'frs2'], feat)
        for mn in ('feq', 'flt', 'fle'):
            ins('%s.%s' % (mn, f), ['rd', 'frs1', 'frs2'], feat)
        ins('fclass.' + f, ['rd', 'frs1'], feat)
        for to in ('w', 'wu'):
            ins('fcvt.%s.%s' % (to, f), ['rd', 'frs1', 'rm'], feat)
        for to in ('l', 'lu'):
            ins('fcvt.%s.%s' % (to, f), ['rd', 'frs1', 'rm'], feat, rv=64)
        for src in ('w', 'wu'):
            ins('fcvt.%s.%s' % (f, src), ['frd', 'rs1', 'rm_rne' if f == 'd' else 'rm'], feat)
        for src in ('l', 'lu'):
            ins('fcvt.%s.%s' % (f, src), ['frd', 'rs1', 'rm'], feat, rv=64)
        if f == 's':
            ins('fmv.x.w', ['rd', 'frs1'], 'F')
            ins('fmv.w.x', ['frd', 'rs1'], 'F')
        if f == 'd':
            ins('fmv.x.d', ['rd', 'frs1'], 'D', rv=64)
            ins('fmv.d.x', ['frd', 'rs1'], 'D', rv=64)
        if f == 'h':
            ins('fmv.x.h', ['rd', 'frs1'], 'ZFHMIN')
            ins('fmv.h.x', ['frd', 'rs1'], 'ZFHMIN')
    ins('fcvt.s.d', ['frd', 'frs1', 'rm'], 'D')
    ins('fcvt.d.s', ['frd', 'frs1', 'rm_rne'], 'D')
    ins('fcvt.s.h', ['frd', 'frs1', 'rm_rne'], 'ZFHMIN')
    ins('fcvt.h.s', ['frd', 'frs1', 'rm'], 'ZFHMIN')
    ins('fcvt.d.h', ['frd', 'frs1', 'rm_rne'], 'ZFHMIN', feat2='D')
    ins('fcvt.h.d', ['frd', 'frs1', 'rm'], 'ZFHMIN', feat2='D')
    ins('fcvt.bf16.s', ['frd', 'frs1', 'rm'], 'ZFBFMIN')
    ins('fcvt.s.bf16', ['frd', 'frs1', 'rm_rne'], 'ZFBFMIN')
    ins('flh', ['frd', 'mem_i'], 'ZFHMIN')
    ins('flw', ['frd', 'mem_i'], 'F')
    ins('fld', ['frd', 'mem_i'], 'D')
    ins('fsh', ['frs2', 'mem_s'], 'ZFHMIN')
    ins('fsw', ['frs2', 'mem_s'], 'F')
    ins('fsd', ['frs2', 'mem_s'], 'D')
    # Zfa
    for f, feat, minfeat in fmts:
        zfa2 = 'ZFA'
        ins('fli.' + f, ['frd', 'fli'], 'ZFA', feat2=(None if f == 's' else ('D' if f == 'd' else 'ZFH')))
        ins('fminm.' + f, ['frd', 'frs1', 'frs2'], 'ZFA', feat2=(None if f == 's' else ('D' if f == 'd' else 'ZFH')))
        ins('fmaxm.' + f, ['frd', 'frs1', 'frs2'], 'ZFA', feat2=(None if f == 's' else ('D' if f == 'd' else 'ZFH')))
        ins('fround.' + f, ['frd', 'frs1', 'rm'], 'ZFA', feat2=(None if f == 's' else ('D' if f == 'd' else 'ZFH')))
        ins('froundnx.' + f, ['frd', 'frs1', 'rm'], 'ZFA', feat2=(None if f == 's' else ('D' if f == 'd' else 'ZFH')))
        ins('fleq.' + f, ['rd', 'frs1', 'frs2'], 'ZFA', feat2=(None if f == 's' else ('D' if f == 'd' else 'ZFH')))
        ins('fltq.' + f, ['rd', 'frs1', 'frs2'], 'ZFA', feat2=(None if f == 's' else ('D' if f == 'd' else 'ZFH')))
    ins('fcvtmod.w.d', ['rd', 'frs1', 'rm_fixed'], 'ZFA', feat2='D')
    ins('fmvh.x.d', ['rd', 'frs1'], 'ZFA', rv=32, feat2='D')
    ins('fmvp.d.x', ['frd', 'rs1', 'rs2'], 'ZFA', rv=32, feat2='D')

    # ------------------------------------------------------------------
    table('base_bit')
    # Zba
    for mn in ('sh1add', 'sh2add', 'sh3add'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'ZBA')
    ins('add.uw', ['rd', 'rs1', 'rs2'], 'ZBA', rv=64)
    for mn in ('sh1add.uw', 'sh2add.uw', 'sh3add.uw'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'ZBA', rv=64)
    ins('slli.uw', ['rd', 'rs1', 'shamt6'], 'ZBA', rv=64)
    # Zbb (and Zbkb)
    for feat in ('ZBB', 'ZBKB'):
        for mn in ('andn', 'orn', 'xnor', 'rol', 'ror'):
            ins(mn, ['rd', 'rs1', 'rs2'], feat)
        ins('rori', ['rd', 'rs1', 'shamt6'], feat)
        ins('rolw', ['rd', 'rs1', 'rs2'], feat, rv=64)
        ins('rorw', ['rd', 'rs1', 'rs2'], feat, rv=64)
        ins('roriw', ['rd', 'rs1', 'shamt5'], feat, rv=64)
        ins('rev8', ['rd', 'rs1'], feat, rv=32)
        ins('rev8', ['rd', 'rs1'], feat, rv=64)
    for mn in ('clz', 'ctz', 'cpop', 'sext.b', 'sext.h', 'orc.b'):
        ins(mn, ['rd', 'rs1'], 'ZBB')
    for mn in ('clzw', 'ctzw', 'cpopw'):
        ins(mn, ['rd', 'rs1'], 'ZBB', rv=64)
    for mn in ('max', 'maxu', 'min', 'minu'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'ZBB')
    # Zbs
    for mn in ('bclr', 'bext', 'binv', 'bset'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'ZBS')
    for mn in ('bclri', 'bexti', 'binvi', 'bseti'):
        ins(mn, ['rd', 'rs1', 'shamt6'], 'ZBS')
    # Zbc / Zbkc
    for feat in ('ZBC', 'ZBKC'):
        ins('clmul', ['rd', 'rs1', 'rs2'], feat)
        ins('clmulh', ['rd', 'rs1', 'rs2'], feat)
    ins('clmulr', ['rd', 'rs1', 'rs2'], 'ZBC')
    # Zbkb
    ins('pack', ['rd', 'rs1', 'rs2'], 'ZBKB')
    ins('packh', ['rd', 'rs1', 'rs2'], 'ZBKB')
    ins('packw', ['rd', 'rs1', 'rs2'], 'ZBKB', rv=64)
    ins('brev8', ['rd', 'rs1'], 'ZBKB')
    ins('zip', ['rd', 'rs1'], 'ZBKB', rv=32)
    ins('unzip', ['rd', 'rs1'], 'ZBKB', rv=32)
    # Zbkx
    ins('xperm4', ['rd', 'rs1', 'rs2'], 'ZBKX')
    ins('xperm8', ['rd', 'rs1', 'rs2'], 'ZBKX')
    # scalar cryptography
    for mn in ('aes32dsi', 'aes32dsmi'):
        ins(mn, ['rd', 'rs1', 'rs2', 'imm_aes_bs'], 'ZKND', rv=32)
    for mn in ('aes32esi', 'aes32esmi'):
        ins(mn, ['rd', 'rs1', 'rs2', 'imm_aes_bs'], 'ZKNE', rv=32)
    for mn in ('aes64ds', 'aes64dsm'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'ZKND', rv=64)
    for mn in ('aes64es', 'aes64esm'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'ZKNE', rv=64)
    ins('aes64im', ['rd', 'rs1'], 'ZKND', rv=64)
    for feat in ('ZKND', 'ZKNE'):
        ins('aes64ks1i', ['rd', 'rs1', 'imm_aes_rnum'], feat, rv=64)
        ins('aes64ks2', ['rd', 'rs1', 'rs2'], feat, rv=64)
    for mn in ('sha256sig0', 'sha256sig1', 'sha256sum0', 'sha256sum1'):
        ins(mn, ['rd', 'rs1'], 'ZKNH')
    for mn in ('sha512sig0', 'sha512sig1', 'sha512sum0', 'sha512sum1'):
        ins(mn, ['rd', 'rs1'], 'ZKNH', rv=64)
    for mn in ('sha512sig0h', 'sha512sig0l', 'sha512sig1h', 'sha512sig1l', 'sha512sum0r',
               'sha512sum1r'):
        ins(mn, ['rd', 'rs1', 'rs2'], 'ZKNH', rv=32)
    for mn in ('sm3p0', 'sm3p1'):
        ins(mn, ['rd', 'rs1'], 'ZKSH')
    for mn in ('sm4ed', 'sm4ks'):
        ins(mn, ['rd', 'rs1', 'rs2', 'imm_aes_bs'], 'ZKSED')

    # ------------------------------------------------------------------
    table('base_misc')
    ins('czero.eqz', ['rd', 'rs1', 'rs2'], 'ZICOND')
    ins('czero.nez', ['rd', 'rs1', 'rs2'], 'ZICOND')
    ins('cbo.clean', ['mem0'], 'ZICBOM')
    ins('cbo.flush', ['mem0'], 'ZICBOM')
    ins('cbo.inval', ['mem0'], 'ZICBOM')
    ins('cbo.zero', ['mem0'], 'ZICBOZ')
    ins('wrs.nto', [], 'ZAWRS')
    ins('wrs.sto', [], 'ZAWRS')
    # Zicfiss on the Zimop encodings
    # (LLVM decodes them with Zimop only; llvm-mc needs Zicfiss)
    for mn, ops in (('sspush', ['ra']), ('sspush', ['t0']), ('sspopchk', ['ra']),
                    ('sspopchk', ['t0'])):
        ins(mn, ops, 'ZICFISS', [UNG], feat2='ZIMOP')
    ins('ssrdp', ['rd_nz'], 'ZICFISS', [UNG], feat2='ZIMOP', probe='ssrdp {0}')
    for n in range(32):
        # (the encodings of Zicfiss which are not sspopchk / ssrdp are invalid)
        ins('mop.r.%d' % n, ['rd', 'rs1'], 'ZIMOP', cond='conditionRdNotZero' if n == 28 else None)
    for n in range(8):
        ins('mop.rr.%d' % n, ['rd', 'rs1', 'rs2'], 'ZIMOP', cond='conditionNotSspush' if n == 7 else None)

    # ------------------------------------------------------------------
    table('compressed')
    ntl = (('c.ntl.p1', 'ntl.p1'), ('c.ntl.pall', 'ntl.pall'), ('c.ntl.s1', 'ntl.s1'),
           ('c.ntl.all', 'ntl.all'))
    for mn, _ in ntl:
        ins(mn, [], 'ZIHINTNTL', [A, HINT], probe=mn, width=16, feat2='ZCA')
    ins('c.unimp', [], 'ZCA', [], TRAP, probe='c.unimp', width=16)
    # quadrant 0
    ins('c.addi4spn', ['rdp', 'sp', 'imm_ciw_nz'], 'ZCA', width=16)
    ins('c.fld', ['frdp', 'mem_cd'], 'ZCD', width=16)
    ins('c.lw', ['rdp', 'mem_cw'], 'ZCA', width=16)
    ins('c.flw', ['frdp', 'mem_cw'], 'ZCF', rv=32, width=16)
    ins('c.ld', ['rdp', 'mem_cd'], 'ZCA', rv=64, width=16)
    ins('c.lbu', ['rdp', 'mem_clb'], 'ZCB', width=16)
    ins('c.lhu', ['rdp', 'mem_clh'], 'ZCB', width=16)
    ins('c.lh', ['rdp', 'mem_clh'], 'ZCB', width=16)
    ins('c.sb', ['rs2p', 'mem_clb'], 'ZCB', width=16)
    ins('c.sh', ['rs2p', 'mem_clh'], 'ZCB', width=16)
    ins('c.fsd', ['frs2p', 'mem_cd'], 'ZCD', width=16)
    ins('c.sw', ['rs2p', 'mem_cw'], 'ZCA', width=16)
    ins('c.fsw', ['frs2p', 'mem_cw'], 'ZCF', rv=32, width=16)
    ins('c.sd', ['rs2p', 'mem_cd'], 'ZCA', rv=64, width=16)
    # quadrant 1
    ins('c.nop', [], 'ZCA', [], probe='c.nop', width=16)
    ins('c.nop', ['imm_ci_nz'], 'ZCA', [HINT], width=16)
    ins('c.addi', ['crd_nz', 'imm_ci_nz'], 'ZCA', width=16)
    ins('c.addi', ['crd', 'imm_ci'], 'ZCA', [HINT], width=16)
    ins('c.jal', ['br_cj'], 'ZCA', [], CALL, rv=32, width=16)
    ins('c.addiw', ['crd_nz', 'imm_ci'], 'ZCA', rv=64, width=16)
    ins('c.li', ['crd_nz', 'imm_ci'], 'ZCA', width=16)
    ins('c.li', ['zero', 'imm_ci'], 'ZCA', [HINT], width=16)
    ins('c.addi16sp', ['sp', 'imm_ci_addi16sp_nz'], 'ZCA', width=16)
    ins('c.lui', ['crd_nzsp', 'imm_ci_lui_nz'], 'ZCA', width=16)
    ins('c.lui', ['zero', 'imm_ci_lui_nz'], 'ZCA', [HINT], width=16)
    ins('c.srli', ['rs1p', 'cshamt_nz'], 'ZCA', width=16)
    ins('c.srli', ['rs1p', 'cshamt'], 'ZCA', [HINT], width=16)
    ins('c.srai', ['rs1p', 'cshamt_nz'], 'ZCA', width=16)
    ins('c.srai', ['rs1p', 'cshamt'], 'ZCA', [HINT], width=16)
    ins('c.andi', ['rs1p', 'imm_ci'], 'ZCA', width=16)
    for mn in ('c.sub', 'c.xor', 'c.or', 'c.and'):
        ins(mn, ['rs1p', 'rs2p'], 'ZCA', width=16)
    for mn in ('c.subw', 'c.addw'):
        ins(mn, ['rs1p', 'rs2p'], 'ZCA', rv=64, width=16)
    ins('c.mul', ['rs1p', 'rs2p'], 'ZCB', feat2='ZMMUL', width=16)
    ins('c.zext.b', ['rs1p'], 'ZCB', width=16)
    ins('c.sext.b', ['rs1p'], 'ZCB', feat2='ZBB', width=16)
    ins('c.zext.h', ['rs1p'], 'ZCB', feat2='ZBB', width=16)
    ins('c.sext.h', ['rs1p'], 'ZCB', feat2='ZBB', width=16)
    ins('c.zext.w', ['rs1p'], 'ZCB', feat2='ZBA', rv=64, width=16)
    ins('c.not', ['rs1p'], 'ZCB', width=16)
    ins('c.j', ['br_cj'], 'ZCA', [], JUMP, width=16)
    ins('c.beqz', ['rs1p', 'br_cb'], 'ZCA', [], COND, width=16)
    ins('c.bnez', ['rs1p', 'br_cb'], 'ZCA', [], COND, width=16)
    # quadrant 2
    ins('c.slli', ['crd_nz', 'cshamt_nz'], 'ZCA', width=16)
    ins('c.slli', ['crd', 'cshamt'], 'ZCA', [HINT], width=16)
    ins('c.fldsp', ['cfrd', 'mem_ldsp'], 'ZCD', width=16)
    ins('c.lwsp', ['crd_nz', 'mem_lwsp'], 'ZCA', width=16)
    ins('c.flwsp', ['cfrd', 'mem_lwsp'], 'ZCF', rv=32, width=16)
    ins('c.ldsp', ['crd_nz', 'mem_ldsp'], 'ZCA', rv=64, width=16)
    ins('c.jr', ['ra'], 'ZCA', [], RET, width=16)
    ins('c.jr', ['crd_nz'], 'ZCA', [], JREG, width=16)
    ins('c.mv', ['crd_nz', 'crs2_nz'], 'ZCA', width=16)
    ins('c.mv', ['zero', 'crs2_nz'], 'ZCA', [HINT], width=16)
    ins('c.ebreak', [], 'ZCA', [], TRAP, probe='c.ebreak', width=16)
    ins('c.jalr', ['crd_nz'], 'ZCA', [], CREG, width=16)
    ins('c.add', ['crd_nz', 'crs2_nz'], 'ZCA', width=16)
    ins('c.add', ['zero', 'crs2_nz'], 'ZCA', [HINT], width=16)
    ins('c.fsdsp', ['cfrs2', 'mem_sdsp'], 'ZCD', width=16)
    ins('c.swsp', ['crs2', 'mem_swsp'], 'ZCA', width=16)
    ins('c.fswsp', ['cfrs2', 'mem_swsp'], 'ZCF', rv=32, width=16)
    ins('c.sdsp', ['crs2', 'mem_sdsp'], 'ZCA', rv=64, width=16)
    # Zcmop / Zicfiss
    ins('c.sspush', ['ra'], 'ZICFISS', [UNG], width=16, feat2='ZCMOP')
    ins('c.sspopchk', ['t0'], 'ZICFISS', [UNG], width=16, feat2='ZCMOP')
    for n in range(1, 16, 2):
        ins('c.mop.%d' % n, [], 'ZCMOP', [], probe='c.mop.%d' % n, width=16)

    # ------------------------------------------------------------------
    table('privileged')
    ins('mret', [], 'PRIVILEGED', [PRIV], RET)
    ins('sret', [], 'PRIVILEGED', [PRIV], RET)
    ins('wfi', [], 'PRIVILEGED', [PRIV])
    ins('sfence.vma', [], 'PRIVILEGED', [A, PRIV], probe='sfence.vma')
    ins('sfence.vma', ['rs1'], 'PRIVILEGED', [A, PRIV], probe='sfence.vma {0}')
    ins('sfence.vma', ['rs1', 'rs2'], 'PRIVILEGED', [PRIV])
    ins('sinval.vma', ['rs1', 'rs2'], 'SVINVAL', [PRIV])
    ins('sfence.w.inval', [], 'SVINVAL', [PRIV])
    ins('sfence.inval.ir', [], 'SVINVAL', [PRIV])
    for mn in ('hfence.vvma', 'hfence.gvma'):
        ins(mn, [], 'H', [A, PRIV], probe=mn)
        ins(mn, ['rs1'], 'H', [A, PRIV], probe=mn + ' {0}')
        ins(mn, ['rs1', 'rs2'], 'H', [PRIV])
    ins('hinval.vvma', ['rs1', 'rs2'], 'H', [PRIV], feat2='SVINVAL')
    ins('hinval.gvma', ['rs1', 'rs2'], 'H', [PRIV], feat2='SVINVAL')
    for mn in ('hlv.b', 'hlv.bu', 'hlv.h', 'hlv.hu', 'hlvx.hu', 'hlv.w', 'hlvx.wu'):
        ins(mn, ['rd', 'mem0'], 'H', [PRIV])
    ins('hlv.wu', ['rd', 'mem0'], 'H', [PRIV], rv=64)
    ins('hlv.d', ['rd', 'mem0'], 'H', [PRIV], rv=64)
    for mn in ('hsv.b', 'hsv.h', 'hsv.w'):
        ins(mn, ['rs2', 'mem0'], 'H', [PRIV])
    ins('hsv.d', ['rs2', 'mem0'], 'H', [PRIV], rv=64)
