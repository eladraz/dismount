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

# The vector syntax: the mask operand, the vtype words, the (base) offset token,
# the case, the spaces, the immediates
LINES += [
    "vadd.vv v1, v2, v3, v0.t", "vadd.vv v1, v2, v3,v0.t", "vadd.vv v1,v2,v3 , v0.t", "vadd.vv v1, v2, v3, v0",
    "vadd.vv v1, v2, v3, v0.T", "vadd.vv v1, v2, v3, V0.t", "vadd.vv v1, v2, v3, v0.t, v0.t", "vadd.vv v1, v2, v0.t, v3",
    "vadd.vv v1, v2, v3,", "vadd.vv v1, v2, v3 v0.t", "vadd.vv v1, v2, v3, v1.t", "vadd.vv v0, v2, v3, v0.t",
    "vadd.vv V1, v2, v3", "VADD.VV v1, v2, v3", "Vadd.Vv v1, v2, v3", "vadd.vx v1, v2, X1", "vadd.vx v1, v2, x1",
    "vadd.vx v1, v2, fp", "vadd.vx v1, v2, FP", "vadd.vi v1, v2, 0X5", "vadd.vi v1, v2, 0b101", "vadd.vi v1, v2, 010",
    "vadd.vi v1, v2, 1+2", "vadd.vi v1, v2, -(3)", "vadd.vi v1, v2, 15", "vadd.vi v1, v2, 16", "vadd.vi v1, v2, -16",
    "vadd.vi v1, v2, -17", "vsll.vi v1, v2, 31", "vsll.vi v1, v2, 32", "vsll.vi v1, v2, -1", "vror.vi v1, v2, 63",
    "vror.vi v1, v2, 64", "vadd.vi v1, v2, sym", "vadd.vi v1, v2, 1.5", "vadd.vi v1, v2, v3",
    "vle8.v v1, (a0)", "vle8.v v1, 0(a0)", "vle8.v v1, 0x0(a0)", "vle8.v v1, 00(a0)", "vle8.v v1, 0b0(a0)",
    "vle8.v v1, -0(a0)", "vle8.v v1, +0(a0)", "vle8.v v1, 0+0(a0)", "vle8.v v1, (0)(a0)", "vle8.v v1, 1(a0)",
    "vle8.v v1, 0 (a0)", "vle8.v v1, ( a0 )", "vle8.v v1, 0( a0 )", "vle8.v v1, ((a0))", "vle8.v v1, (A0)",
    "vle8.v v1, (a0), v0.t", "vle8.v v1, (a0),v0.t", "vle8.v v1, (a0) , v0.t", "vle8.v v1, (a0), v0", "vle8.v v1, (fa0)",
    "vle8.v v1, (v1)", "vle8.v v1, (x10)", "vle8.v v1, (fp)", "lr.w a0, 0+0(a1)", "lr.w a0, -0(a1)", "cbo.clean 0+0(a0)",
    "vsetvli a0, a1, e8", "vsetvli a0, a1, e8,", "vsetvli a0, a1, e8, m1,", "vsetvli a0, a1, e8,,m1", "vsetvli a0, a1, m2",
    "vsetvli a0, a1, ta", "vsetvli a0, a1, ma", "vsetvli a0, a1, mu, ta", "vsetvli a0, a1, e8, ta, m1",
    "vsetvli a0, a1, e8, m1, ta, ma, mu", "vsetvli a0, a1, e8alt", "vsetvli a0, a1, e16alt, mf8", "vsetvli a0, a1, e32alt",
    "vsetvli a0, a1, e64alt", "vsetvli a0, a1, e07", "vsetvli a0, a1, e008, m01", "vsetvli a0, a1, e+8", "vsetvli a0, a1, e",
    "vsetvli a0, a1, e8, mf1", "vsetvli a0, a1, e8, m3", "vsetvli a0, a1, e8, mf16", "vsetvli a0, a1, E8", "vsetvli a0, a1, e8, M1",
    "vsetvli a0, a1, 7", "vsetvli a0, a1, 7,", "vsetvli a0, a1, 2047", "vsetvli a0, a1, 2048", "vsetvli a0, a1, -1",
    "vsetvli a0, a1, 0X10", "vsetvli a0, a1, 1<<4", "vsetvli a0, a1, sym", "vsetvli a0, a1, (e8)", "vsetvli a0, 3, e8",
    "vsetivli a0, 31, e8", "vsetivli a0, 32, e8", "vsetivli a0, -1, e8", "vsetivli a0, 3, 1023", "vsetivli a0, 3, 1024",
    "vsetivli a0, a1, e8", "vsetvl a0, a1, a2", "vsetvl a0, a1, 3", "vsetvl a0, a1",
    "vmv2r.v v1, v2", "vmv2r.v v2, v4", "vmv8r.v v8, v16", "vmv8r.v v8, v12", "vl2re8.v v2, (a0)", "vl2re8.v v1, (a0)",
    "vwadd.vv v2, v2, v4", "vwadd.vv v2, v3, v4", "vlseg8e8.v v28, (a0)", "vlseg3e8.v v30, (a0)", "vlseg2e8.v v0, (a0), v0.t",
    "vluxseg2ei8.v v1, (a0), v2", "vluxseg2ei8.v v1, (a0), v3", "vsuxseg2ei8.v v2, (a0), v2",
    "vmsge.vx v1, v2, a0", "vmsge.vx v1, v2, a0, v0.t, v3", "vmsge.vx v1, v2, a0, v0.t, v1", "vmsge.vx v1, v2, a0, v3",
    "vmsge.vx v0, v2, a0, v0.t", "vmsge.vi v1, v2, 16", "vmsge.vi v1, v2, 17", "vmslt.vi v1, v2, 0", "vmsltu.vi v1, v2, 0",
    "vmsgeu.vi v1, v2, 0", "vmsge.vi v1, v2, a0", "vmsge.vi v1, v2, 3, v0.t", "vmsge.vi v1, v2, 3, v0",
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
