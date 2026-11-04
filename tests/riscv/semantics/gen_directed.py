#!/usr/bin/env python3
"""
Generate tests/riscv/semantics/corpus/{rv,rv64,rv32}_dir_*.s: the directed scalar corpus of the semantics tests.

The generated corpus (gen_corpus.py) has every instruction with every register and the edge immediates, over
random states. These lines fix the values of the operands (`@state:`) to the cases where the processors
differ: the special floating point values of every format in every operand position, the NaN boxing, the
integer conversion boundaries, the division by zero and the overflow, the shifts by the width, the memory
accesses at the edges of a page and of the window, the reservation of lr / sc, the cache blocks. The states
still draw the other registers and the rounding mode (frm) at random.

  python3 tests/riscv/semantics/gen_directed.py
"""
import itertools
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'corpus')
sys.path.insert(0, HERE)

# ---------------------------------------------------------------------------
# Floating point values: name -> bits, by format
# ---------------------------------------------------------------------------

SINGLE = {
    '+0': 0x00000000, '-0': 0x80000000, '1': 0x3f800000, '-1': 0xbf800000, '1.5': 0x3fc00000,
    '2.5': 0x40200000, '-2.5': 0xc0200000, '0.5': 0x3f000000, 'max': 0x7f7fffff, '-max': 0xff7fffff,
    'minnorm': 0x00800000, 'den': 0x00000001, 'denmax': 0x007fffff, '-den': 0x80000001,
    'inf': 0x7f800000, '-inf': 0xff800000, 'qnan': 0x7fc00000, 'snan': 0x7f800001, '-qnan': 0xffc00000,
    'nan2': 0x7fc12345, 'snan2': 0xff800123, '2^31': 0x4f000000, '-2^31': 0xcf000000,
    '2^32': 0x4f800000, '2^63': 0x5f000000, '-2^63': 0xdf000000, '2^64': 0x5f800000,
    '1+e': 0x3f800001, '1-e': 0x3f7fffff, '16777215': 0x4b7fffff, '3': 0x40400000,
}
DOUBLE = {
    '+0': 0x0000000000000000, '-0': 0x8000000000000000, '1': 0x3ff0000000000000, '-1': 0xbff0000000000000,
    '1.5': 0x3ff8000000000000, '2.5': 0x4004000000000000, '-2.5': 0xc004000000000000,
    '0.5': 0x3fe0000000000000, 'max': 0x7fefffffffffffff, '-max': 0xffefffffffffffff,
    'minnorm': 0x0010000000000000, 'den': 0x0000000000000001, 'denmax': 0x000fffffffffffff,
    '-den': 0x8000000000000001, 'inf': 0x7ff0000000000000, '-inf': 0xfff0000000000000,
    'qnan': 0x7ff8000000000000, 'snan': 0x7ff0000000000001, '-qnan': 0xfff8000000000000,
    'nan2': 0x7ff8000012345678, 'snan2': 0xfff0000000000123, '2^31': 0x41e0000000000000,
    '-2^31': 0xc1e0000000000000, '2^32': 0x41f0000000000000, '2^63': 0x43e0000000000000,
    '-2^63': 0xc3e0000000000000, '2^64': 0x43f0000000000000, '1+e': 0x3ff0000000000001,
    '1-e': 0x3fefffffffffffff, '2^31-0.5': 0x41dfffffffc00000, '-2^31-0.5': 0xc1e0000000200000,
    '2^32-0.5': 0x41efffffffe00000, '2^53+2': 0x4340000000000001, '3': 0x4008000000000000,
    '2^31+1': 0x41e0000000200000, '2^40+5': 0x4270000000000005, '1e300': 0x7e37e43c8800759c,
}
HALF = {
    '+0': 0x0000, '-0': 0x8000, '1': 0x3c00, '-1': 0xbc00, '1.5': 0x3e00, '2.5': 0x4100, '-2.5': 0xc100,
    '0.5': 0x3800, 'max': 0x7bff, '-max': 0xfbff, 'minnorm': 0x0400, 'den': 0x0001, 'denmax': 0x03ff,
    '-den': 0x8001, 'inf': 0x7c00, '-inf': 0xfc00, 'qnan': 0x7e00, 'snan': 0x7c01, '-qnan': 0xfe00,
    'nan2': 0x7e55, 'snan2': 0xfc23, '1+e': 0x3c01, '1-e': 0x3bff, '2048': 0x6800, '65504': 0x7bff,
    '3': 0x4200,
}
BFLOAT = {
    '+0': 0x0000, '-0': 0x8000, '1': 0x3f80, '-1': 0xbf80, '1.5': 0x3fc0, 'max': 0x7f7f, 'minnorm': 0x0080,
    'den': 0x0001, 'denmax': 0x007f, 'inf': 0x7f80, '-inf': 0xff80, 'qnan': 0x7fc0, 'snan': 0x7f81,
    'nan2': 0x7fc5, '1+e': 0x3f81, '3': 0x4040,
}
FORMATS = {'s': (32, SINGLE), 'd': (64, DOUBLE), 'h': (16, HALF)}


def boxed(bits, width):
    """The 64 bit register hex of a boxed value."""
    if width == 64:
        return '%016x' % bits
    return '%016x' % ((((1 << (64 - width)) - 1) << width) | bits)


class Out:
    def __init__(self):
        self.files = {}

    def add(self, name, line):
        self.files.setdefault(name, []).append(line)

    def split_by_xlen(self):
        """The lines of a shared file which RV32 can't assemble (RV64 instructions) move to the RV64 file."""
        import rvasm
        sys_path = HERE
        for name in [n for n in list(self.files) if n.startswith('rv_')]:
            lines = self.files[name]
            texts = [l.split('  #')[0].split(' @')[0].strip() for l in lines]
            texts = [t.split('  # ')[0] for t in texts]
            res = rvasm.assemble(32, texts)
            keep, move = [], []
            for line, (hexb, err) in zip(lines, res):
                (move if err else keep).append(line)
            if move:
                self.files[name] = keep
                self.files.setdefault('rv64_' + name[3:], []).extend(move)

    def write(self):
        self.split_by_xlen()
        for name in os.listdir(OUT):
            if '_dir_' in name:
                os.unlink(os.path.join(OUT, name))
        total = 0
        for name, lines in sorted(self.files.items()):
            seen = set()
            with open(os.path.join(OUT, name + '.s'), 'w') as f:
                f.write('# GENERATED by tests/riscv/semantics/gen_directed.py: do not edit\n')
                for line in lines:
                    if line in seen:
                        continue
                    seen.add(line)
                    f.write(line + '\n')
                    total += 1
        print('%d lines in %d files' % (total, len(self.files)))


def state(**kw):
    items = ['%s=%s' % (k, v) for k, v in kw.items() if v != '']
    return ('  # @state: ' + ' '.join(items)) if items else ''


# ---------------------------------------------------------------------------
# Floating point
# ---------------------------------------------------------------------------

BINARY = ['fadd', 'fsub', 'fmul', 'fdiv', 'fmin', 'fmax', 'fsgnj', 'fsgnjn', 'fsgnjx']
COMPARE = ['feq', 'flt', 'fle']
ZFA_BINARY = ['fminm', 'fmaxm']
ZFA_COMPARE = ['fleq', 'fltq']


def float_lines(out):
    for sfx, (width, table) in FORMATS.items():
        names = list(table)
        # The pairs of values: the first against a core set and the core set against each
        core = ['+0', '-0', '1', '-inf', 'qnan', 'snan', 'den', '1.5']
        pairs = [(a, b) for a in names for b in core] + [(a, b) for a in core for b in names]
        extra = ''
        if sfx == 'h':
            extra = ''
        file = 'rv_dir_float_%s' % sfx
        for a, b in sorted(set(pairs)):
            fa, fb = boxed(table[a], width), boxed(table[b], width)
            for op in BINARY:
                out.add(file, '%s.%s fa0, fa1, fa2%s' % (op, sfx, state(f11=fa, f12=fb)))
            for op in COMPARE + ZFA_COMPARE:
                out.add(file, '%s.%s a0, fa1, fa2%s' % (op, sfx, state(f11=fa, f12=fb)))
            for op in ZFA_BINARY:
                out.add(file, '%s.%s fa0, fa1, fa2%s' % (op, sfx, state(f11=fa, f12=fb)))
        # unary
        for a in names:
            fa = boxed(table[a], width)
            for op in ('fsqrt', 'fround', 'froundnx'):
                out.add(file, '%s.%s fa0, fa1%s' % (op, sfx, state(f11=fa)))
            out.add(file, 'fclass.%s a0, fa1%s' % (sfx, state(f11=fa)))
            for dst in ('w', 'wu', 'l', 'lu'):
                out.add(file, 'fcvt.%s.%s a0, fa1%s' % (dst, sfx, state(f11=fa)))
            out.add(file, 'fmv.x.%s a0, fa1%s' % ({'s': 'w', 'd': 'd', 'h': 'h'}[sfx], state(f11=fa)))
        # the fused multiply-adds: three values
        small = ['+0', '-0', '1', '-1', 'inf', 'qnan', 'snan', 'den', 'max', '1.5']
        for a, b, c in itertools.product(small, repeat=3):
            if len({a, b, c}) == 1 and a in ('+0', '-0', 'qnan'):
                continue
            for op in ('fmadd', 'fmsub', 'fnmsub', 'fnmadd'):
                out.add(file, '%s.%s fa0, fa1, fa2, fa3%s' % (op, sfx, state(
                    f11=boxed(table[a], width), f12=boxed(table[b], width), f13=boxed(table[c], width))))
        # the NaN boxing of every operand
        one = table['1']
        for pattern in (0, 0x0000ffff, 0x7fffffff, 0xffff0000, 0xfffffffe, 0x00000001):
            if width == 64:
                break
            bad = ((pattern << width) | one) & 0xFFFFFFFFFFFFFFFF
            if width == 16:
                bad = ((pattern << 16) | one) & 0xFFFFFFFFFFFFFFFF
            for op in ('fadd', 'fmul', 'fmin', 'fsgnj', 'fsgnjx'):
                out.add(file, '%s.%s fa0, fa1, fa2%s' % (op, sfx, state(f11='%016x' % bad, f12=boxed(table['1.5'], width))))
                out.add(file, '%s.%s fa0, fa1, fa2%s' % (op, sfx, state(f11=boxed(table['1.5'], width), f12='%016x' % bad)))
            out.add(file, 'fclass.%s a0, fa1%s' % (sfx, state(f11='%016x' % bad)))
            out.add(file, 'fsqrt.%s fa0, fa1%s' % (sfx, state(f11='%016x' % bad)))
            out.add(file, 'fcvt.w.%s a0, fa1%s' % (sfx, state(f11='%016x' % bad)))
            out.add(file, 'feq.%s a0, fa1, fa1%s' % (sfx, state(f11='%016x' % bad)))
            out.add(file, 'fmadd.%s fa0, fa1, fa2, fa3%s' % (sfx, state(f11='%016x' % bad, f12=boxed(one, width), f13=boxed(one, width))))
            out.add(file, 'fmv.x.%s a0, fa1%s' % ({'s': 'w', 'h': 'h'}[sfx], state(f11='%016x' % bad)))
            # a store doesn't look at the boxing
            out.add(file, 'fs%s fa1, 8(a1)%s' % ({'s': 'w', 'h': 'h'}[sfx], state(f11='%016x' % bad)))
        # the integer to float conversions: the boundaries
        ints = ['0', '1', '-1', '2147483647', '-2147483648', '2147483649', '4294967295', '4294967296',
                '9007199254740993', '9223372036854775807', '-9223372036854775808', '16777217', '65519',
                '65520', '65535', '65536', '2049', '1000000007', '-16777217', '549755813889']
        for value in ints:
            v = int(value)
            for src in ('w', 'wu', 'l', 'lu'):
                out.add(file, 'fcvt.%s.%s fa0, a1%s' % (sfx, src, state(x11='%x' % (v & 0xFFFFFFFFFFFFFFFF))))
        # fmv.x / fmv.f with every pattern
        for value in ('0', '8000', '7fff', 'ffff', '7c00', '7e00', '80000000', 'ffffffff', '7f800000',
                      '7fc00000', 'ffffffffffffffff', '8000000000000000', '7ff0000000000001'):
            out.add(file, 'fmv.%s.x fa0, a1%s' % ({'s': 'w', 'd': 'd', 'h': 'h'}[sfx], state(x11=value)))
        # the conversions to another format
    # conversions between the formats
    for (from_sfx, to_sfx) in (('s', 'd'), ('d', 's'), ('h', 's'), ('s', 'h'), ('h', 'd'), ('d', 'h')):
        width, table = FORMATS[from_sfx]
        for a in table:
            out.add('rv_dir_float_cvt', 'fcvt.%s.%s fa0, fa1%s' % (to_sfx, from_sfx, state(f11=boxed(table[a], width))))
    for a, bits in BFLOAT.items():
        out.add('rv_dir_float_cvt', 'fcvt.s.bf16 fa0, fa1%s' % state(f11=boxed(bits, 16)))
    for a in SINGLE:
        out.add('rv_dir_float_cvt', 'fcvt.bf16.s fa0, fa1%s' % state(f11=boxed(SINGLE[a], 32)))
    # the bad boxing of the conversions
    for pattern in (0, 0x0000ffff, 0xfffffffe):
        out.add('rv_dir_float_cvt', 'fcvt.d.s fa0, fa1%s' % state(f11='%016x' % ((pattern << 32) | 0x3f800000)))
        out.add('rv_dir_float_cvt', 'fcvt.s.h fa0, fa1%s' % state(f11='%016x' % ((pattern << 16) | 0x3c00)))
        out.add('rv_dir_float_cvt', 'fcvt.h.s fa0, fa1%s' % state(f11='%016x' % ((pattern << 32) | 0x3f800000)))
        out.add('rv_dir_float_cvt', 'fcvt.bf16.s fa0, fa1%s' % state(f11='%016x' % ((pattern << 32) | 0x3f800000)))
        out.add('rv_dir_float_cvt', 'fcvt.s.bf16 fa0, fa1%s' % state(f11='%016x' % ((pattern << 16) | 0x3f80)))
    # Zfa fli: every immediate
    for sfx in 'sdh':
        for index in range(32):
            pass
    # fcvtmod.w.d
    for a in DOUBLE:
        out.add('rv_dir_float_cvt', 'fcvtmod.w.d a0, fa1, rtz%s' % state(f11=boxed(DOUBLE[a], 64)))
    for bits in (0x41dfffffffc00000, 0x41dfffffffe00000, 0xc1e0000000000000, 0xc1e0000000100000,
                 0x41e0000000000000, 0x41efffffffffffff, 0x4200000000000000, 0x4330000000000000,
                 0x43e0000000000000, 0x43f0000000000000, 0x4410000000000000, 0xc3e0000000000000,
                 0x3ff0000000000000, 0xbff0000000000000, 0x3fefffffffffffff, 0x0000000000000001,
                 0x41f0000000000001, 0xc1f0000000100000, 0x7fefffffffffffff, 0x4170000000000000):
        out.add('rv_dir_float_cvt', 'fcvtmod.w.d a0, fa1, rtz%s' % state(f11=boxed(bits, 64)))
    # the rounding modes (the encoded ones) of the single operand instructions on the ties
    for sfx, (width, table) in FORMATS.items():
        for rm in ('rne', 'rtz', 'rdn', 'rup', 'rmm'):
            for a in ('1.5', '2.5', '-2.5', '0.5', '1+e', '1-e', '3', 'max', 'den', 'inf', 'snan'):
                out.add('rv_dir_float_rm', 'fround.%s fa0, fa1, %s%s' % (sfx, rm, state(f11=boxed(table[a], width))))
                out.add('rv_dir_float_rm', 'froundnx.%s fa0, fa1, %s%s' % (sfx, rm, state(f11=boxed(table[a], width))))
                out.add('rv_dir_float_rm', 'fcvt.w.%s a0, fa1, %s%s' % (sfx, rm, state(f11=boxed(table[a], width))))
                out.add('rv_dir_float_rm', 'fcvt.lu.%s a0, fa1, %s%s' % (sfx, rm, state(f11=boxed(table[a], width))))
        for rm in ('rne', 'rtz', 'rdn', 'rup', 'rmm'):
            for a, b in (('1', '1+e'), ('1', 'den'), ('max', 'max'), ('minnorm', '0.5'), ('1-e', '1-e'), ('3', '1.5')):
                for op in ('fadd', 'fsub', 'fmul', 'fdiv'):
                    out.add('rv_dir_float_rm', '%s.%s fa0, fa1, fa2, %s%s' % (op, sfx, rm, state(f11=boxed(table[a], width), f12=boxed(table[b], width))))
                out.add('rv_dir_float_rm', 'fmadd.%s fa0, fa1, fa2, fa3, %s%s' % (sfx, rm, state(
                    f11=boxed(table[a], width), f12=boxed(table[b], width), f13=boxed(table['den'], width))))
                out.add('rv_dir_float_rm', 'fsqrt.%s fa0, fa1, %s%s' % (sfx, rm, state(f11=boxed(table[a], width))))
    # the dynamic rounding mode with every value of frm
    for frm in range(8):
        for sfx, (width, table) in FORMATS.items():
            out.add('rv_dir_float_rm', 'fadd.%s fa0, fa1, fa2%s' % (sfx, state(f11=boxed(table['1'], width), f12=boxed(table['1+e'], width), fcsr='%x' % (frm << 5))))
            out.add('rv_dir_float_rm', 'fcvt.w.%s a0, fa1%s' % (sfx, state(f11=boxed(table['2.5'], width), fcsr='%x' % (frm << 5))))
            out.add('rv_dir_float_rm', 'fsqrt.%s fa0, fa1%s' % (sfx, state(f11=boxed(table['3'], width), fcsr='%x' % (frm << 5))))
            out.add('rv_dir_float_rm', 'fmadd.%s fa0, fa1, fa2, fa3%s' % (sfx, state(f11=boxed(table['1'], width), f12=boxed(table['1'], width), f13=boxed(table['den'], width), fcsr='%x' % (frm << 5))))
            out.add('rv_dir_float_rm', 'fround.%s fa0, fa1%s' % (sfx, state(f11=boxed(table['2.5'], width), fcsr='%x' % (frm << 5))))
            out.add('rv_dir_float_rm', 'fcvt.%s.l fa0, a1%s' % (sfx, state(x11='7fffffffffffffff', fcsr='%x' % (frm << 5))))
            out.add('rv_dir_float_rm', 'fcvt.%s.w fa0, a1%s' % (sfx, state(x11='7fffffff', fcsr='%x' % (frm << 5))))
        out.add('rv_dir_float_rm', 'fcvt.d.s fa0, fa1%s' % state(f11=boxed(SINGLE['1'], 32), fcsr='%x' % (frm << 5)))
        out.add('rv_dir_float_rm', 'fcvt.s.d fa0, fa1%s' % state(f11=boxed(DOUBLE['1+e'], 64), fcsr='%x' % (frm << 5)))
        out.add('rv_dir_float_rm', 'fcvt.s.h fa0, fa1%s' % state(f11=boxed(HALF['1'], 16), fcsr='%x' % (frm << 5)))
        out.add('rv_dir_float_rm', 'fcvt.d.w fa0, a1%s' % state(x11='ffffffff', fcsr='%x' % (frm << 5)))
        out.add('rv_dir_float_rm', 'fcvt.bf16.s fa0, fa1%s' % state(f11=boxed(SINGLE['1+e'], 32), fcsr='%x' % (frm << 5)))
        out.add('rv_dir_float_rm', 'fcvt.s.bf16 fa0, fa1%s' % state(f11=boxed(BFLOAT['1'], 16), fcsr='%x' % (frm << 5)))
    # the sticky flags are kept
    for flags in (0, 1, 2, 4, 8, 0x10, 0x1f):
        out.add('rv_dir_float_rm', 'fadd.s fa0, fa1, fa2%s' % state(f11=boxed(SINGLE['1'], 32), f12=boxed(SINGLE['1'], 32), fcsr='%x' % flags))
        out.add('rv_dir_float_rm', 'fdiv.d fa0, fa1, fa2%s' % state(f11=boxed(DOUBLE['1'], 64), f12=boxed(DOUBLE['+0'], 64), fcsr='%x' % flags))
        out.add('rv_dir_float_rm', 'feq.s a0, fa1, fa2%s' % state(f11=boxed(SINGLE['snan'], 32), f12=boxed(SINGLE['1'], 32), fcsr='%x' % flags))
    # Zfa fli
    for sfx, (width, table) in FORMATS.items():
        for name in ('-1.0', 'min', '1.52587890625e-05', '3.0517578125e-05', '0.00390625', '0.0078125', '0.0625',
                     '0.125', '0.25', '0.3125', '0.375', '0.4375', '0.5', '0.625', '0.75', '0.875', '1.0', '1.25',
                     '1.5', '1.75', '2.0', '2.5', '3.0', '4.0', '8.0', '16.0', '128.0', '256.0', '32768.0',
                     '65536.0', 'inf', 'nan'):
            out.add('rv_dir_float_cvt', 'fli.%s fa0, %s' % (sfx, name))


# ---------------------------------------------------------------------------
# The memory accesses
# ---------------------------------------------------------------------------

WINDOW = 0x4000


def memory_lines(out):
    loads = [('lb', 1), ('lbu', 1), ('lh', 2), ('lhu', 2), ('lw', 4), ('lwu', 4), ('ld', 8), ('flh', 2), ('flw', 4),
             ('fld', 8)]
    stores = [('sb', 1), ('sh', 2), ('sw', 4), ('sd', 8), ('fsh', 2), ('fsw', 4), ('fsd', 8)]
    offsets = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17]
    for name, size in loads:
        dest = 'fa0' if name.startswith('f') else 'a0'
        file = 'rv64_dir_memory' if name in ('lwu', 'ld') else 'rv_dir_memory'
        for base in (0xff0, 0xff8, 0xffc, 0xfff, 0x1ff8, 0x1fff, 0x3ff8, 0x3ffc, 0x3fff, 0x4000, 0x4001, 0x7fff, 0x0, 0x8):
            for offset in (0, 1, 4, 7, 8, -8, -1, 2047, -2048):
                out.add(file, '%s %s, %d(a1)%s' % (name, dest, offset, state(x11='win+%x' % (base + 0x10000 if base < 0 else base))))
        # x0 as the destination still faults
        out.add(file, '%s %s, 0(a1)%s' % (name, 'zero' if not name.startswith('f') else 'fa0', state(x11='win+4000')))
        out.add(file, '%s %s, 0(a1)%s' % (name, 'zero' if not name.startswith('f') else 'fa0', state(x11='win+3ffd')))
    for name, size in stores:
        src = 'fa0' if name.startswith('f') else 'a0'
        file = 'rv64_dir_memory' if name == 'sd' else 'rv_dir_memory'
        for base in (0xff0, 0xff8, 0xffc, 0xfff, 0x1ff8, 0x1fff, 0x3ff8, 0x3ffc, 0x3fff, 0x4000, 0x4001, 0x7fff, 0x0):
            for offset in (0, 1, 4, 7, 8, -8, -1):
                out.add(file, '%s %s, %d(a1)%s' % (name, src, offset, state(x11='win+%x' % base)))
        if not name.startswith('f'):
            out.add(file, '%s zero, 0(a1)%s' % (name, state(x11='win+3ffd')))
            out.add(file, '%s a1, 0(a1)%s' % (name, state(x11='win+10')))
            out.add(file, '%s a1, 8(a1)%s' % (name, state(x11='win+1ff9')))
    # the instruction reads and writes through the same register
    for name in ('lb', 'lh', 'lw', 'lbu', 'lhu'):
        out.add('rv_dir_memory', '%s a1, 0(a1)%s' % (name, state(x11='win+80')))
        out.add('rv_dir_memory', '%s a1, 3(a1)%s' % (name, state(x11='win+80')))
    out.add('rv64_dir_memory', 'ld a1, 0(a1)' + state(x11='win+80'))
    out.add('rv64_dir_memory', 'ld a1, 1(a1)' + state(x11='win+80'))
    out.add('rv64_dir_memory', 'lwu a1, 0(a1)' + state(x11='win+80'))
    out.add('rv_dir_memory', 'lw a1, 0(a1)' + state(x11='win+80'))

    # The atomics. Zama16b: a misaligned AMO inside a 16 byte region completes, across one it faults
    for width, sfx in ((4, 'w'), (8, 'd')):
        file = 'rv_dir_atomic' if sfx == 'w' else 'rv64_dir_atomic'
        ops = ['amoswap', 'amoadd', 'amoxor', 'amoand', 'amoor', 'amomin', 'amomax', 'amominu', 'amomaxu']
        for op in ops:
            for base in (0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x107, 0x108, 0x109, 0x10c, 0x10d, 0x10f, 0x110, 0x3ff8, 0x3ffc, 0x3ffd, 0x3fff, 0x4000):
                out.add(file, '%s.%s a0, a2, (a1)%s' % (op, sfx, state(x11='win+%x' % base)))
            for suffix in ('', '.aq', '.rl', '.aqrl'):
                out.add(file, '%s.%s%s a0, a2, (a1)%s' % (op, sfx, suffix, state(x11='win+200')))
            # the destination is the source, the address, x0
            out.add(file, '%s.%s a1, a2, (a1)%s' % (op, sfx, state(x11='win+200')))
            out.add(file, '%s.%s a2, a2, (a1)%s' % (op, sfx, state(x11='win+200')))
            out.add(file, '%s.%s zero, a2, (a1)%s' % (op, sfx, state(x11='win+200')))
            out.add(file, '%s.%s a0, zero, (a1)%s' % (op, sfx, state(x11='win+200')))
            out.add(file, '%s.%s a0, a2, (zero)%s' % (op, sfx, state()))
        # the signed and unsigned comparisons: operands around the sign
        for a, b in (('7fffffff', '80000000'), ('80000000', '7fffffff'), ('ffffffff', '1'), ('1', 'ffffffff'),
                     ('ffffffff', 'ffffffff'), ('7fffffffffffffff', '8000000000000000'),
                     ('8000000000000000', '7fffffffffffffff'), ('ffffffffffffffff', '1')):
            for op in ('amomin', 'amomax', 'amominu', 'amomaxu', 'amoadd'):
                out.add(file, '%s.%s a0, a2, (a1)%s' % (op, sfx, state(x11='win+300', x12=a, mem='win+300:' + ''.join('%02x' % ((int(b, 16) >> (8 * i)) & 0xff) for i in range(8)))))
    # Zabha
    for sfx in ('b', 'h'):
        for op in ('amoswap', 'amoadd', 'amoxor', 'amoand', 'amoor', 'amomin', 'amomax', 'amominu', 'amomaxu'):
            for base in (0x100, 0x101, 0x10f, 0x110, 0x3fff, 0x4000):
                out.add('rv_dir_atomic', '%s.%s a0, a2, (a1)%s' % (op, sfx, state(x11='win+%x' % base)))
        for base in (0x100, 0x101, 0x10f, 0x3fff, 0x4000):
            out.add('rv_dir_atomic', 'amocas.%s a0, a2, (a1)%s' % (sfx, state(x11='win+%x' % base)))
    # lr / sc
    sizes = (('w', 4), ('d', 8))
    for sfx, width in sizes:
        file = 'rv_dir_atomic' if sfx == 'w' else 'rv64_dir_atomic'
        for base in (0x400, 0x401, 0x402, 0x404, 0x407, 0x408, 0x40c, 0x3ff8, 0x3ffc, 0x4000):
            out.add(file, 'lr.%s a0, (a1)%s' % (sfx, state(x11='win+%x' % base)))
            out.add(file, 'sc.%s a0, a2, (a1)%s' % (sfx, state(x11='win+%x' % base)))
            out.add(file, 'lr.%s a0, (a1) ; sc.%s a3, a2, (a1)%s' % (sfx, sfx, state(x11='win+%x' % base)))
        out.add(file, 'lr.%s a0, (a1) ; sc.%s a3, a2, (a4)%s' % (sfx, sfx, state(x11='win+400', x14='win+408')))
        out.add(file, 'lr.%s a0, (a1) ; sc.%s a3, a2, (a4)%s' % (sfx, sfx, state(x11='win+400', x14='win+400')))
        out.add(file, 'lr.%s a0, (a1) ; sc.%s a3, a2, (a1) ; sc.%s a4, a2, (a1)%s' % (sfx, sfx, sfx, state(x11='win+400')))
        out.add(file, 'lr.%s a0, (a1) ; lr.%s a0, (a4) ; sc.%s a3, a2, (a1)%s' % (sfx, sfx, sfx, state(x11='win+400', x14='win+800')))
        out.add(file, 'lr.%s a0, (a1) ; lr.%s a0, (a4) ; sc.%s a3, a2, (a4)%s' % (sfx, sfx, sfx, state(x11='win+400', x14='win+800')))
        out.add(file, 'lr.%s zero, (a1) ; sc.%s a3, a2, (a1)%s' % (sfx, sfx, state(x11='win+400')))
        out.add(file, 'lr.%s a0, (a1) ; sc.%s zero, a2, (a1)%s' % (sfx, sfx, state(x11='win+400')))
        out.add(file, 'lr.%s a0, (a1) ; sc.%s a1, a2, (a1)%s' % (sfx, sfx, state(x11='win+400')))
        out.add(file, 'lr.%s a0, (a1) ; sc.%s a2, a2, (a1)%s' % (sfx, sfx, state(x11='win+400')))
        out.add(file, 'lr.%s a1, (a1) ; sc.%s a3, a2, (a1)%s' % (sfx, sfx, state(x11='win+400')))
        out.add(file, 'lr.%s a0, (a1) ; sw a2, 0(a1) ; sc.%s a3, a2, (a1)%s' % (sfx, sfx, state(x11='win+400')))
        out.add(file, 'lr.%s a0, (a1) ; sc.%s a3, zero, (a1)%s' % (sfx, sfx, state(x11='win+400')))
        for suffix in ('', '.aq', '.rl', '.aqrl'):
            out.add(file, 'lr.%s%s a0, (a1) ; sc.%s%s a3, a2, (a1)%s' % (sfx, suffix, sfx, suffix, state(x11='win+400')))
    out.add('rv64_dir_atomic', 'lr.w a0, (a1) ; sc.d a3, a2, (a1)' + state(x11='win+400'))
    out.add('rv64_dir_atomic', 'lr.d a0, (a1) ; sc.w a3, a2, (a1)' + state(x11='win+400'))
    out.add('rv64_dir_atomic', 'lr.d a0, (a1) ; sc.w a3, a2, (a4)' + state(x11='win+400', x14='win+404'))
    out.add('rv64_dir_atomic', 'lr.w a0, (a1) ; sc.d a3, a2, (a4)' + state(x11='win+400', x14='win+404'))
    out.add('rv64_dir_atomic', 'lr.w a0, (a1) ; sc.w a3, a2, (a4)' + state(x11='win+400', x14='win+404'))
    # the reservation survives other instructions, the ones that load too
    out.add('rv_dir_atomic', 'lr.w a0, (a1) ; lw a5, 0(a1) ; sc.w a3, a2, (a1)' + state(x11='win+400'))
    out.add('rv_dir_atomic', 'lr.w a0, (a1) ; amoadd.w a5, a2, (a1) ; sc.w a3, a2, (a1)' + state(x11='win+400'))
    out.add('rv_dir_atomic', 'lr.w a0, (a1) ; fence ; sc.w a3, a2, (a1)' + state(x11='win+400'))
    out.add('rv_dir_atomic', 'lr.w a0, (a1) ; addi a5, a5, 1 ; sc.w a3, a2, (a1)' + state(x11='win+400'))
    out.add('rv_dir_atomic', 'lr.w a0, (a1) ; sc.w a3, a2, (a1)' + state(x11='win+400'))

    # Zacas: the pairs
    for base in (0x100, 0x104, 0x108, 0x10c, 0x10f, 0x3ff8, 0x4000):
        out.add('rv_dir_atomic', 'amocas.w a0, a2, (a1)%s' % state(x11='win+%x' % base))
        out.add('rv64_dir_atomic', 'amocas.d a0, a2, (a1)%s' % state(x11='win+%x' % base))
        out.add('rv64_dir_atomic', 'amocas.q a2, a4, (a1)%s' % state(x11='win+%x' % base))
        out.add('rv32_dir_atomic', 'amocas.d a2, a4, (a1)%s' % state(x11='win+%x' % base))
    for rd in ('a0', 'a2', 'a4', 'zero', 't1', 's2', 'a6'):
        for rs2 in ('a0', 'a2', 'a4', 'zero', 't1', 's2', 'a6'):
            out.add('rv64_dir_atomic', 'amocas.q %s, %s, (a1)%s' % (rd, rs2, state(x11='win+100')))
            out.add('rv32_dir_atomic', 'amocas.d %s, %s, (a1)%s' % (rd, rs2, state(x11='win+100')))
            out.add('rv64_dir_atomic', 'amocas.d %s, %s, (a1)%s' % (rd, rs2, state(x11='win+100')))
            out.add('rv_dir_atomic', 'amocas.w %s, %s, (a1)%s' % (rd, rs2, state(x11='win+100')))
    # a matching compare value: the memory holds the expected value (rd) in the state
    for sfx, size in (('w', 4), ('d', 8), ('b', 1), ('h', 2)):
        out.add('rv64_dir_atomic' if sfx in 'd' else 'rv_dir_atomic', 'amocas.%s a0, a2, (a1)%s' % (sfx, state(
            x11='win+100', x10='0', mem='win+100:' + '00' * size)))
        out.add('rv64_dir_atomic' if sfx in 'd' else 'rv_dir_atomic', 'amocas.%s a0, a2, (a1)%s' % (sfx, state(
            x11='win+100', x10='ffffffffffffffff', mem='win+100:' + 'ff' * size)))

    # The cache block operations
    for name in ('cbo.zero', 'cbo.clean', 'cbo.flush', 'cbo.inval'):
        for base in (0x0, 0x1, 0x3f, 0x40, 0x41, 0x7f, 0x80, 0xfc0, 0xfff, 0x1000, 0x3fc0, 0x3fff, 0x4000, 0x4040, 0x100000):
            out.add('rv_dir_cache', '%s (a1)%s' % (name, state(x11='win+%x' % base)))
        out.add('rv_dir_cache', '%s (zero)%s' % (name, state()))
        out.add('rv_dir_cache', '%s (sp)%s' % (name, state(x2='win+88')))
    for name in ('prefetch.i', 'prefetch.r', 'prefetch.w'):
        for offset in (0, 32, 2016, -2048, -32, 1024):
            for base in (0x0, 0x100, 0x4000, 0x100000):
                out.add('rv_dir_cache', '%s %d(a1)%s' % (name, offset, state(x11='win+%x' % base)))


# ---------------------------------------------------------------------------
# Integer
# ---------------------------------------------------------------------------

def integer_lines(out):
    values = ['0', '1', '3', '7f', '80', '8000', 'ffff', '7fffffff', '80000000', 'ffffffff', 'fffffffe', '55555555',
              'aaaaaaaa', '12345678']
    values64 = values + ['100000000', '7fffffffffffffff', '8000000000000000', 'ffffffffffffffff', '5555555555555555',
                         '123456789abcdef0', 'ffffffff00000000', '80000000ffffffff']
    ops3 = ['add', 'sub', 'sll', 'slt', 'sltu', 'xor', 'srl', 'sra', 'or', 'and', 'mul', 'mulh', 'mulhsu', 'mulhu',
            'div', 'divu', 'rem', 'remu', 'andn', 'orn', 'xnor', 'rol', 'ror', 'max', 'maxu', 'min', 'minu', 'bclr',
            'bext', 'binv', 'bset', 'clmul', 'clmulh', 'clmulr', 'pack', 'packh', 'xperm4', 'xperm8', 'sh1add',
            'sh2add', 'sh3add', 'czero.eqz', 'czero.nez']
    ops3w = ['addw', 'subw', 'sllw', 'srlw', 'sraw', 'mulw', 'divw', 'divuw', 'remw', 'remuw', 'rolw', 'rorw',
             'packw', 'add.uw', 'sh1add.uw', 'sh2add.uw', 'sh3add.uw']
    ops2 = ['clz', 'ctz', 'cpop', 'sext.b', 'sext.h', 'zext.h', 'orc.b', 'rev8', 'brev8']
    ops2w = ['clzw', 'ctzw', 'cpopw']
    for a in values64:
        for b in values64:
            for op in ops3:
                out.add('rv_dir_int' if (len(a) <= 8 and len(b) <= 8) else 'rv64_dir_int',
                        '%s a0, a1, a2%s' % (op, state(x11=a, x12=b)))
            for op in ops3w:
                out.add('rv64_dir_int', '%s a0, a1, a2%s' % (op, state(x11=a, x12=b)))
    for a in values64:
        file = 'rv_dir_int' if len(a) <= 8 else 'rv64_dir_int'
        for op in ops2:
            out.add(file, '%s a0, a1%s' % (op, state(x11=a)))
        for op in ops2w:
            out.add('rv64_dir_int', '%s a0, a1%s' % (op, state(x11=a)))
    for a in values:
        out.add('rv32_dir_int', 'zip a0, a1' + state(x11=a))
        out.add('rv32_dir_int', 'unzip a0, a1' + state(x11=a))
    # shifts and rotates by the immediates around the width
    for shamt in (0, 1, 2, 7, 8, 15, 16, 17, 31):
        for a in values:
            for op in ('slli', 'srli', 'srai', 'rori', 'bclri', 'bexti', 'binvi', 'bseti'):
                out.add('rv_dir_int', '%s a0, a1, %d%s' % (op, shamt, state(x11=a)))
    for shamt in (32, 33, 47, 62, 63):
        for a in values64:
            for op in ('slli', 'srli', 'srai', 'rori', 'bclri', 'bexti', 'binvi', 'bseti'):
                out.add('rv64_dir_int', '%s a0, a1, %d%s' % (op, shamt, state(x11=a)))
    for shamt in range(0, 32):
        for a in ('1', '80000000', 'ffffffff', '7fffffff', 'deadbeef'):
            for op in ('slliw', 'srliw', 'sraiw', 'roriw'):
                out.add('rv64_dir_int', '%s a0, a1, %d%s' % (op, shamt, state(x11=a)))
            out.add('rv64_dir_int', 'slli.uw a0, a1, %d%s' % (shamt, state(x11=a)))
    # the register shifts by the width
    for count in ('0', '1', '1f', '20', '21', '3f', '40', '41', '7f', '80', 'ffffffff', 'ffffffffffffffff', '100000020'):
        for a in ('1', '80000000', 'ffffffff', '7fffffff', '8000000000000000'):
            for op in ('sll', 'srl', 'sra', 'rol', 'ror', 'bclr', 'bext', 'binv', 'bset'):
                out.add('rv64_dir_int', '%s a0, a1, a2%s' % (op, state(x11=a, x12=count)))
                out.add('rv_dir_int', '%s a0, a1, a2%s' % (op, state(x11=a if len(a) <= 8 else '80000000', x12=count if len(count) <= 8 else 'ffffffff')))
            for op in ('sllw', 'srlw', 'sraw', 'rolw', 'rorw'):
                out.add('rv64_dir_int', '%s a0, a1, a2%s' % (op, state(x11=a, x12=count)))
    # lui / auipc / the immediates
    for imm in (0, 1, 0x7ffff, 0x80000, 0xfffff, 0x12345):
        out.add('rv_dir_int', 'lui a0, %d' % imm)
        out.add('rv_dir_int', 'auipc a0, %d' % imm)
    for imm in (0, 1, -1, 2047, -2048, 0x7ff, 255, -256):
        for op in ('addi', 'slti', 'sltiu', 'xori', 'ori', 'andi'):
            for a in values:
                out.add('rv_dir_int', '%s a0, a1, %d%s' % (op, imm, state(x11=a)))
    for imm in (0, 1, -1, 2047, -2048):
        for a in values64:
            out.add('rv64_dir_int', 'addiw a0, a1, %d%s' % (imm, state(x11=a)))
    # rd == rs1 == rs2
    for op in ops3 + ['addw', 'subw', 'mulw']:
        for reg in ('a0',):
            out.add('rv64_dir_int' if op.endswith('w') else 'rv_dir_int', '%s a0, a0, a0%s' % (op, state(x10='deadbeef')))
            out.add('rv64_dir_int' if op.endswith('w') else 'rv_dir_int', '%s a0, a0, a1%s' % (op, state(x10='deadbeef', x11='3')))
            out.add('rv64_dir_int' if op.endswith('w') else 'rv_dir_int', '%s a0, a1, a0%s' % (op, state(x10='deadbeef', x11='3')))
    # the branches
    for op in ('beq', 'bne', 'blt', 'bge', 'bltu', 'bgeu'):
        for a in ('0', '1', 'ffffffff', '7fffffff', '80000000'):
            for b in ('0', '1', 'ffffffff', '7fffffff', '80000000'):
                for off in (8, -8, 4094):
                    out.add('rv_dir_int', '%s a1, a2, %d%s' % (op, off, state(x11=a, x12=b)))
        out.add('rv64_dir_int', '%s a1, a2, 8%s' % (op, state(x11='ffffffffffffffff', x12='7fffffffffffffff')))
        out.add('rv64_dir_int', '%s a1, a2, 8%s' % (op, state(x11='8000000000000000', x12='7fffffffffffffff')))
        out.add('rv_dir_int', '%s a1, a1, 8%s' % (op, state(x11='5')))
        out.add('rv_dir_int', '%s zero, a1, 8%s' % (op, state(x11='5')))
        out.add('rv_dir_int', '%s a1, zero, 8%s' % (op, state(x11='5')))
    # jal / jalr: the link register, the target with bit 0, the base == rd
    for rd in ('ra', 'zero', 't0', 'a0', 't1', 'sp'):
        out.add('rv_dir_int', 'jal %s, 8' % rd)
        out.add('rv_dir_int', 'jal %s, -8' % rd)
        out.add('rv_dir_int', 'jal %s, 1048574' % rd)
        out.add('rv_dir_int', 'jal %s, -1048576' % rd)
        for rs in ('a1', 'ra', 't0', 'a0', 'zero', 't2', 'sp'):
            for off in (0, 4, 8, 1, 3, -4, 2047, -2048):
                for base in ('80001100', '80001101', '0', 'ffffffff', '80002fff'):
                    out.add('rv_dir_int', 'jalr %s, %d(%s)%s' % (rd, off, rs, state(**{('x%d' % {'a1': 11, 'ra': 1, 't0': 5, 'a0': 10, 'zero': 0, 't2': 7, 'sp': 2}[rs]): base} if rs != 'zero' else {})))
    # Zimop: every mop reads its sources and writes zero
    for n in range(32):
        out.add('rv_dir_int', 'mop.r.%d a0, a1%s' % (n, state(x10='1234', x11='55')))
    for n in range(8):
        out.add('rv_dir_int', 'mop.rr.%d a0, a1, a2%s' % (n, state(x10='1234', x11='55', x12='66')))
    # hints and fences
    for text in ('nop', 'pause', 'fence', 'fence.tso', 'fence.i', 'fence rw, rw', 'fence w, r', 'fence i, o', 'fence r, r', 'fence w, w',
                 'ntl.p1', 'ntl.pall', 'ntl.s1', 'ntl.all', 'c.nop', 'c.ntl.p1', 'c.ntl.all', 'wrs.nto', 'wrs.sto'):
        out.add('rv_dir_int', text)


# ---------------------------------------------------------------------------
# Zicsr
# ---------------------------------------------------------------------------

def csr_lines(out):
    names = ['fflags', 'frm', 'fcsr']
    for name in names:
        for value in ('0', '1', '5', '1f', '20', '40', '7f', '80', 'e0', 'ff', '100', 'ffffffff', 'ffffffffffffffff', 'a5'):
            for op in ('csrrw', 'csrrs', 'csrrc'):
                for rd in ('a0', 'zero'):
                    out.add('rv_dir_csr', '%s %s, %s, a1%s' % (op, rd, name, state(x11=value)))
            out.add('rv_dir_csr', 'csrrs a0, %s, zero%s' % (name, state(x11=value)))
            out.add('rv_dir_csr', 'csrrc a0, %s, zero%s' % (name, state(x11=value)))
        for imm in (0, 1, 5, 7, 15, 16, 31):
            for op in ('csrrwi', 'csrrsi', 'csrrci'):
                out.add('rv_dir_csr', '%s a0, %s, %d%s' % (op, name, imm, state(fcsr='ff')))
                out.add('rv_dir_csr', '%s a0, %s, %d%s' % (op, name, imm, state(fcsr='a5')))
                out.add('rv_dir_csr', '%s zero, %s, %d%s' % (op, name, imm, state(fcsr='5a')))
        for fcsr in ('0', 'ff', 'a5', '5a', 'e0', '1f'):
            out.add('rv_dir_csr', 'csrr a0, %s%s' % (name, state(fcsr=fcsr)))
    # a read of the counters / the vector registers
    for name in ('vstart', 'vxsat', 'vxrm', 'vcsr', 'vl', 'vtype', 'vlenb'):
        for fixed in ({}, {'vl': '3', 'vtype': '0', 'vstart': '1', 'vxrm': '2', 'vxsat': '1'}):
            out.add('rv_dir_csr', 'csrr a0, %s%s' % (name, state(**fixed)))
            out.add('rv_dir_csr', 'csrrs a0, %s, zero%s' % (name, state(**fixed)))
            out.add('rv_dir_csr', 'csrrsi a0, %s, 0%s' % (name, state(**fixed)))
        out.add('rv_dir_csr', 'csrrw a0, %s, a1' % name)
        out.add('rv_dir_csr', 'csrrw zero, %s, zero' % name)
        out.add('rv_dir_csr', 'csrrs a0, %s, a1' % name)
        out.add('rv_dir_csr', 'csrrc a0, %s, a1' % name)
        out.add('rv_dir_csr', 'csrrwi a0, %s, 1' % name)
        out.add('rv_dir_csr', 'csrrsi a0, %s, 1' % name)
        out.add('rv_dir_csr', 'csrrci a0, %s, 0' % name)
    # the writes of the vector CSRs, read back by the next instruction (the state of the result has no vector
    # part for a scalar line)
    for value in ('0', '1', '2', '3', '4', '7', '8', 'f', '10', 'ff', '7fff', 'ffff', '10000', 'ffffffff',
                  'ffffffffffffffff', '8000000000000000'):
        for name in ('vstart', 'vxsat', 'vxrm', 'vcsr'):
            out.add('rv_dir_csr', 'csrw %s, a1 ; csrr a0, %s%s' % (name, name, state(x11=value)))
            out.add('rv_dir_csr', 'csrrw a2, %s, a1 ; csrr a0, %s%s' % (name, name, state(x11=value)))
            out.add('rv_dir_csr', 'csrrs a2, %s, a1 ; csrr a0, %s%s' % (name, name, state(x11=value, vstart='3', vxrm='1', vxsat='1')))
            out.add('rv_dir_csr', 'csrrc a2, %s, a1 ; csrr a0, %s%s' % (name, name, state(x11=value, vstart='3', vxrm='2', vxsat='1')))
        out.add('rv_dir_csr', 'csrw vxsat, a1 ; csrr a0, vcsr%s' % state(x11=value))
        out.add('rv_dir_csr', 'csrw vxrm, a1 ; csrr a0, vcsr%s' % state(x11=value))
        out.add('rv_dir_csr', 'csrw vcsr, a1 ; csrr a0, vxrm ; csrr a2, vxsat%s' % state(x11=value))
        out.add('rv_dir_csr', 'csrw fcsr, a1 ; csrr a0, frm ; csrr a2, fflags%s' % state(x11=value))
        out.add('rv_dir_csr', 'csrw frm, a1 ; csrr a0, fcsr%s' % state(x11=value))
        out.add('rv_dir_csr', 'csrw fflags, a1 ; csrr a0, fcsr%s' % state(x11=value, fcsr='e0'))
    for imm in range(0, 32, 3):
        for name in ('vstart', 'vxsat', 'vxrm', 'vcsr', 'fflags', 'frm', 'fcsr'):
            out.add('rv_dir_csr', 'csrwi %s, %d ; csrr a0, %s' % (name, imm, name))
            out.add('rv_dir_csr', 'csrrsi a2, %s, %d ; csrr a0, %s%s' % (name, imm, name, state(vstart='2', vxrm='1')))
            out.add('rv_dir_csr', 'csrrci a2, %s, %d ; csrr a0, %s%s' % (name, imm, name, state(vstart='2', vxrm='3')))
    # the read only CSRs: the write is illegal, the read isn't
    for name in ('cycle', 'time', 'instret', 'hpmcounter3', 'hpmcounter31'):
        for text in ('csrrw a0, %s, a1', 'csrrs a0, %s, a1', 'csrrc a0, %s, a1', 'csrrwi a0, %s, 0', 'csrrsi a0, %s, 1',
                     'csrrci a0, %s, 1', 'csrrw zero, %s, zero'):
            out.add('rv_dir_csr', text % name)
    # the privileged CSRs trap in user mode
    for number in (0x300, 0x100, 0x305, 0x341, 0xf11, 0x7c0, 0x180):
        out.add('rv_dir_csr', 'csrr a0, 0x%x' % number)
        out.add('rv_dir_csr', 'csrrw zero, 0x%x, a1' % number)
    # the floating point flags are the sticky result of the operations
    for text in ('fadd.s fa0, fa1, fa2 ; csrr a0, fflags', 'fdiv.d fa0, fa1, fa2 ; csrr a0, fcsr',
                 'fsqrt.s fa0, fa1 ; csrr a0, fflags ; csrw fflags, zero ; csrr a2, fflags',
                 'csrwi frm, 3 ; fadd.s fa0, fa1, fa2 ; csrr a0, frm',
                 'csrrwi a0, frm, 1 ; fmul.d fa0, fa1, fa2 ; csrr a2, fflags',
                 'csrwi frm, 6 ; fadd.s fa0, fa1, fa2', 'csrwi frm, 7 ; fadd.s fa0, fa1, fa2', 'csrwi frm, 5 ; fcvt.w.s a0, fa1',
                 'csrwi frm, 4 ; fcvt.w.s a0, fa1', 'csrw fcsr, a1 ; fadd.s fa0, fa1, fa2', 'frflags a0 ; fsflags a2',
                 'frrm a0 ; fsrm a2', 'frcsr a0 ; fscsr a2'):
        out.add('rv_dir_csr', text)
        out.add('rv_dir_csr', text + state(f11='ffffffff3f800000', f12='ffffffff00000000', x11='3', fcsr='a5'))


# ---------------------------------------------------------------------------
# Control flow integrity
# ---------------------------------------------------------------------------

def tagged(line):
    """Append the cfi mode tag to a line: in the comment of the state if it has one."""
    return line + (' @mode: cfi' if '# @state:' in line else '  # @mode: cfi')


def cfi_lines(out):
    # Every instruction starts with the check of elp: the states draw elp 0 and 1
    for text in ('add a0, a1, a2', 'addi a0, a1, 1', 'lw a0, 0(a1)', 'sw a0, 0(a1)', 'fadd.s fa0, fa1, fa2',
                 'flw fa0, 0(a1)', 'beq a0, a1, 8', 'jal ra, 8', 'jalr ra, 0(a1)', 'jalr zero, 0(ra)',
                 'jalr zero, 0(t0)', 'jalr zero, 0(t2)', 'jalr ra, 0(t2)', 'jalr t0, 0(t2)', 'jalr a0, 8(a1)',
                 'jalr zero, 0(a1)', 'jalr zero, 16(a2)', 'jalr ra, 0(ra)', 'jalr t0, 0(t0)', 'jalr t0, 0(ra)',
                 'c.jr a1', 'c.jr ra', 'c.jr t0', 'c.jr t2', 'c.jalr a1', 'c.jalr t2', 'c.jalr ra', 'c.jalr t0',
                 'c.nop', 'c.addi a0, 1', 'ecall', 'ebreak', 'unimp', 'fence', 'fence.i', 'amoadd.w a0, a2, (a1)',
                 'lr.w a0, (a1)', 'csrr a0, fcsr', 'csrr a0, ssp', 'csrrw a0, ssp, a1', 'wrs.nto',
                 'cbo.zero (a1)', 'mop.r.0 a0, a1', 'mop.rr.0 a0, a1, a2', 'c.mop.1', 'c.mop.3', 'c.mop.15'):
        out.add('rv_dir_cfi', tagged(text))
    for label in (0, 1, 2, 0x12345, 0xfffff, 0x80000, 0x7ffff):
        out.add('rv_dir_cfi', 'lpad %d  # @mode: cfi' % label)
        out.add('rv_dir_cfi', 'lpad %d%s @mode: cfi' % (label, state(x7='%x' % (label << 12))))
        out.add('rv_dir_cfi', 'lpad %d%s @mode: cfi' % (label, state(x7='%x' % (label << 12 | 0xfff))))
        out.add('rv_dir_cfi', 'lpad %d%s @mode: cfi' % (label, state(x7='%x' % (((label + 1) & 0xfffff) << 12))))
        out.add('rv_dir_cfi', 'lpad %d' % label)
        # lpad with elp and the label
        out.add('rv_dir_cfi', 'lpad %d%s @mode: cfi' % (label, state(elp='1', x7='%x' % (label << 12))))
        out.add('rv_dir_cfi', 'lpad %d%s @mode: cfi' % (label, state(elp='1', x7='%x' % (((label + 1) & 0xfffff) << 12))))
        out.add('rv_dir_cfi', 'lpad %d%s @mode: cfi' % (label, state(elp='0', x7='%x' % (((label + 1) & 0xfffff) << 12))))
    # the shadow stack
    for text in ('sspush ra', 'sspush t0', 'sspopchk ra', 'sspopchk t0', 'c.sspush ra', 'c.sspopchk t0', 'ssrdp a0', 'ssrdp t1',
                 'ssamoswap.w a0, a2, (a1)', 'ssamoswap.d a0, a2, (a1)', 'ssamoswap.w zero, a2, (a1)',
                 'ssamoswap.w a0, zero, (a1)', 'ssamoswap.w a1, a2, (a1)', 'ssamoswap.d a1, a1, (a1)'):
        out.add('rv_dir_cfi', tagged(text))
        out.add('rv_dir_cfi', text)
    # an access to the shadow stack with ssp at the page edges
    for ssp in (0x80200008, 0x80200ff8, 0x80201000, 0x80200000, 0x80200ffc, 0x80200004):
        for text in ('sspush ra', 'sspopchk ra', 'sspush t0', 'sspopchk t0', 'ssrdp a0'):
            # (Spike clears the low bits of ssp when the guest sets it: a misaligned ssp is for QEMU only)
            only = '' if ssp % 8 == 0 else ' @oracle: qemu'
            out.add('rv_dir_cfi', text + state(ssp='%x' % ssp) + ' @mode: cfi' + only)


def main():
    out = Out()
    float_lines(out)
    memory_lines(out)
    integer_lines(out)
    csr_lines(out)
    cfi_lines(out)
    out.write()


if __name__ == '__main__':
    main()
