#!/usr/bin/env python3
"""Compare the accepted syntax of dismount and llvm-mc on odd lines (the number
forms, the blanks, the case, the fence sets, the CSR numbers...): lists the
lines where one accepts and the other rejects, or the bytes differ.

  python3 tests/riscv/tools/syntax_probe.py [--arch rv64] [file with lines]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rvlib

LINES = [
    "fence wr, rw", "fence rw, rwr", "fence rwx, rw", "fence ir, ow", "fence iorw", "fence w", "fence 0,0",
    "fence i, i", "fence ww, w", "fence.tso x", "csrr a0, 0x1000", "csrr a0, -1", "csrr a0, 4095", "csrr a0, 0",
    "li a0, 1.5", "fli.s fa0, 1", "fli.s fa0, 0.50", "fli.s fa0, 1.0e0", "fli.s fa0, +1.0", "fli.s fa0, -1",
    "fli.s fa0, 1.5e-5", "fli.d fa0, nan", "fli.d fa0, NaN", "fli.h fa0, inf", "fli.s fa0, 0x1p0",
    "lw a0, 0x10(a1)", "lw a0, -0(a1)", "lw a0, +8(a1)", "lw a0, 8 (a1)", "lw a0, 8( a1 )", "lw a0 , 8(a1)",
    "jal x0, +8", "jal 0x10", "jal ra, -8", "addi a0, a0, 0b101", "addi a0, a0, 017", "addi a0, a0, 0o17",
    "add a0 a1 a2", "addi a0, a0, 1+1", "addi a0,a0,-(1)", "LI a0, 5", "ADDI A0, A1, 0X10",
    "add a0, a1, a2 # c", "add a0, a1, a2 ; add a0, a1, a2", "add\ta0,\ta1,\ta2", "  add a0, a1, a2  ",
    "addi a0, a0, 4294967295", "addi a0, a0, -2048", "addi a0, a0, 2048", "addi a0, a0, 0x800",
    "addi a0, a0, 0xfffffffffffff800", "ld a0, 0(a1)", "lui a0, 0xfffff", "lui a0, 1048576", "auipc a0, -1",
    "slli a0, a0, 64", "slli a0, a0, 63", "slli a0, a0, -1", "sraiw a0, a0, 32", "fcvt.w.s a0, fa0, RTZ",
    "fcvt.w.s a0, fa0, rtz, rtz", "fadd.s fa0, fa1, fa2, DYN", "fmadd.s fa0,fa1,fa2,fa3", "csrrw a0, MSTATUS, a1",
    "csrrw a0, mstatus , a1", "csrrw a0, 0x300, a1", "csrrwi a0, mstatus, 32", "csrrwi a0, mstatus, -1",
    "mv a0, a1", "mv a0, fa1", "mv a0, x32", "mv a0, f1", "fmv.s fa0, a1", "mv sp, s0", "mv fp, tp", "mv s0, fp",
    "lr.w a0, 0(a1)", "lr.w.aq.rl a0, (a1)", "lr.w.rl.aq a0, (a1)", "amoadd.w.aqrl a0, a1, (a2)", "amoadd.w a0, a1, a2",
    "sc.w a0, a1, 4(a2)", "beq a0, a1, .", "beq a0, a1, .+4", "beq a0, a1, . + 4", "beq a0, a1, .+4096",
    "j .-2", "j 1", "beq a0, a1, 1", "jalr a0, 0(a1)", "jalr a0, a1, 4", "jalr 4(a1)", "jalr a1", "jr a1",
    "ret x", "ret", "nop 1", "nop", "ecall x", "ebreak", "unimp", "wfi", "mret", "sfence.vma a0", "sfence.vma a0, a1",
    "sfence.vma", "hlv.b a0, 0(a1)", "hlv.b a0, (a1)", "fence.i", "pause", "ntl.p1", "ntl.pall",
    "c.addi a0, 1", "C.ADDI A0, 1", "c.addi a0, 0x1f", "c.addi a0, -32", "c.addi a0, 32", "c.lui a0, 31",
    "c.lui a0, 0xfffe0", "c.lui a0, -1", "c.lui a0, 0x20", "c.addi4spn a0, sp, 1020", "c.addi4spn a0, sp, 1024",
    "c.lw a0, 124(a1)", "c.lw a0, 128(a1)", "c.lw a0, 2(a1)", "c.j 2046", "c.j 2048", "c.beqz a0, -256",
    "c.beqz a0, 256", "c.jr zero", "c.jalr ra", "c.mv a0, zero", "c.add a0, zero", "c.slli a0, 0", "c.srli a0, 0",
    "c.nop 31", "c.nop 32", "c.mop.1", "c.mop.2", "c.mop.15", "c.mop.17", "c.lbu a0, 3(a1)", "c.lh a0, 2(a1)", "c.lh a0, 4(a1)",
    "c.sext.b a0", "c.zext.w a0", "c.not a0", "c.mul a0, a1",
    "FADD.S fa0, fa1, fa2", "AMOADD.W.AQ a0, a1, (a2)", "C.NOP", "C.ADDI A0, 1", "c.ADDI a0, 1", "c.addi A0, 1",
    "Csrr a0, mstatus", "Fence.I", "SH1ADD a0, a1, a2", "fadd.s fa0, fa1, fa2, Rtz", "csrrw a0, Mstatus, a1",
    "fence IORW, rw", "fli.s fa0, MIN", "beq a0, a1, .+4096", "bne a0, a1, .-4098", "beqz a0, .+8192",
    "bgt a0, a1, .+5000", "blt a0, a1, .+4094", "blt a0, a1, .+4096", "beq a0, a1, .+2097152", "beq a0, a1, 4096",
    "jalr a0, a1, 4", "jalr a0, a1, 2048", "add a0, a1, a2 ; sub a0, a1, a2", "add a0, a1, a2;", ";", "li a0, 0x100 ; li a1, 1",
    "addi a0, a0, (1 << 4) | 3", "addi a0, a0, 7 / 2", "addi a0, a0, 7 % 4", "addi a0, a0, ~0", "addi a0, a0, !0",
    "addi a0, a0, 1 / 0", "addi a0, a0, 2 * (3 + 4)", "lw a0, 4 * 2(a1)", "lw a0, (1 + 1)(a1)", "lw a0, -(1)(a1)",
]


def main():
    arch = 'rv64'
    args = sys.argv[1:]
    if args and args[0] in ('rv32', 'rv64'):
        arch = args.pop(0)
    bits = int(arch[2:])
    lines = LINES
    if args:
        lines = [l.rstrip('\n') for l in open(args[0]) if l.strip()]
    features = rvlib.all_features(bits)
    ref = [None] * len(lines)
    for compressed in (False, True):
        index = [i for i, l in enumerate(lines) if l.lower().strip().startswith('c.') == compressed]
        got = rvlib.llvm_assemble([lines[i] for i in index], bits, features, rvc=compressed)
        for i, g in zip(index, got):
            ref[i] = g
    ours = rvlib.dismount_asm(lines, bits, None, rvc=False)
    bad = 0
    for line, r, o in zip(lines, ref, ours):
        a = r[0] == 'ok'
        b = o[0] == 'OK'
        if a != b or (a and r[1] != o[1]):
            bad += 1
            print('%-36s llvm: %-28s dismount: %s' % (line, r[1].hex() if a else 'REJECT (%s)' % r[1][:40],
                                                    o[1].hex() if b else 'REJECT'))
    print('%d lines, %d differ' % (len(lines), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
