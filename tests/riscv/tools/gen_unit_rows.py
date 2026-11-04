#!/usr/bin/env python3
"""Generate tests/riscv/RiscvTestRows.inc, the rows of tests/TestRiscvDisassembler.cpp:
{bits, size, encoding, "text", "no aliases text", "assembly text"} for a few shapes
of every mnemonic of the corpus (RV32 and RV64, the compressed instructions
and the aliases), as llvm-objdump prints them, so the unit test pins the LLVM
syntax. The assembly text is the text with the branch targets as offsets.

The vector rows (the corpora v_*.s) go to tests/riscv/RiscvVectorRows.inc,
RiscvVectorDecodeRows.inc and RiscvVectorAsmRows.inc, which
tests/TestRiscvVector.cpp includes: the text of a few shapes of every vector
mnemonic in the three formats, the decoding of encodings with feature sets
(gating, reserved vtype values, misaligned register groups) and the
assembling of lines (the constraints of the destination, the pseudo
instructions, the vtype spellings: accepted bytes or rejected).

  python3 tests/riscv/tools/gen_unit_rows.py [--per-mnemonic N]
"""
import argparse
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rvlib

OUTPUT = os.path.join(rvlib.REPO, 'tests', 'riscv', 'RiscvTestRows.inc')
CORPUS = os.path.join(rvlib.REPO, 'tests', 'riscv', 'corpus')
BRANCHES = set('beq bne blt bge bltu bgeu beqz bnez blez bgez bltz bgtz j jal c.j c.jal c.beqz c.bnez'.split())


def shape(text):
    """The form of a text: the registers and the numbers removed."""
    text = re.sub(r'\b(x|f|v)\d+\b', 'R', text)
    text = re.sub(r'\b(zero|ra|sp|gp|tp|t\d+|s\d+|a\d+|fp|ft\d+|fs\d+|fa\d+)\b', 'R', text)
    return re.sub(r'0x[0-9a-f]+|-?\d+', 'N', text)


def literal(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def relative(text, address, bits):
    words = text.split(' ', 1)
    if words[0] not in BRANCHES or len(words) < 2:
        return text
    parts = words[1].split(', ')
    m = re.fullmatch(r'0x([0-9a-f]+)', parts[-1])
    if not m:
        return text
    offset = int(m.group(1), 16) - address
    if bits == 32:
        offset = (offset + (1 << 31)) % (1 << 32) - (1 << 31)
    parts[-1] = str(offset)
    return words[0] + ' ' + ', '.join(parts)


PSEUDO_SETS = [('none', []), ('all', None),
               ('none,+zca,+zba,+zbs,+zbkb', ['zca', 'zba', 'zbs', 'zbkb']),
               ('none,+zca,+zbb', ['zca', 'zbb'])]


def pseudo_rows():
    """The rows of the pseudo instructions (li, la, call, tail, the symbol loads
    and stores): llvm-mc's bytes without and with the compression."""
    rows = []
    for bits in (32, 64):
        texts = []
        values = [0, 1, -1, 2047, 2048, -2048, -2049, 0x7ff, 0x800, 0x12345, 0x12345678, 0x7fffffff,
                  -0x80000000, 0x17fff, 0x1000, 0x4400, -0x1000]
        if bits == 64:
            values += [0x123456789, 0x80000001, 0xffffffff, 0x100000000, 0x100000001, 0x1fffffffe,
                       0x4000000000800, 0x7ffffffffffffff, -0x7ffffffffffffff, 0x519dd3b4f67ff0b3,
                       -4611686018427387907, -4294967297, 0x800000000000, 0x7fffffffffffffff,
                       -0x8000000000000000, 0x2aaaaaaaaaaaaaaa, 0x1234567800000000,
                       0x0000123400005678, 70368744177665]
        for v in values:
            texts.append('li a0, %d' % v)
        texts += ['la a0, symbol', 'lla s1, symbol', 'call symbol', 'call t1, symbol', 'tail symbol',
                  'lw a0, symbol', 'lb s2, symbol', 'sw a0, symbol, t0', 'flw fa0, symbol, a1',
                  'fsd ft1, symbol, t2', 'zext.h a0, a1', 'sext.b a0, a1', 'sext.h s1, s2']
        if bits == 64:
            texts += ['ld a0, symbol', 'sd a1, symbol, t0', 'zext.w a0, a1']
        for mattr, features in PSEUDO_SETS:
            names = rvlib.all_features(bits) if features is None else features
            plain = rvlib.llvm_assemble(texts, bits, names, rvc=False)
            packed = rvlib.llvm_assemble(texts, bits, names, rvc=True)
            for text, a, b in zip(texts, plain, packed):
                if a[0] == 'ok' and b[0] == 'ok':
                    rows.append((bits, mattr, text, a[1].hex(), b[1].hex()))
    return rows


VECTOR_ASM = [
    # the constraints of the destination
    'vadd.vv v0, v2, v3, v0.t', 'vadd.vv v0, v2, v3', 'vadd.vv v1, v2, v3, v0.t',
    'vwadd.vv v2, v2, v4', 'vwadd.vv v2, v3, v4', 'vwadd.vv v2, v4, v2', 'vwadd.vx v2, v2, a0',
    'vwadd.wv v2, v2, v4', 'vwadd.wv v2, v4, v2', 'vnsrl.wv v2, v2, v4', 'vnsrl.wv v2, v4, v2',
    'vrgather.vv v1, v1, v3', 'vrgather.vv v1, v2, v1', 'vrgather.vx v1, v1, a0',
    'vrgatherei16.vv v1, v1, v3', 'vslideup.vx v1, v1, a0', 'vslideup.vi v1, v1, 1',
    'vslidedown.vx v1, v1, a0', 'vslide1up.vx v1, v1, a0', 'vcompress.vm v1, v1, v3',
    'vcompress.vm v1, v2, v1', 'viota.m v2, v2', 'viota.m v0, v2, v0.t', 'vid.v v0, v0.t',
    'vid.v v0', 'vmsbf.m v2, v2', 'vmseq.vv v0, v2, v3, v0.t', 'vmand.mm v0, v2, v3',
    'vadc.vvm v0, v2, v3, v0', 'vadc.vvm v1, v2, v3, v0', 'vmerge.vvm v0, v2, v3, v0',
    'vmerge.vvm v1, v2, v3, v1', 'vmerge.vvm v1, v2, v3', 'vmadc.vvm v0, v2, v3, v0',
    'vfmerge.vfm v0, v2, fa0, v0', 'vzext.vf2 v1, v1', 'vfwcvt.f.f.v v1, v1',
    'vfncvt.f.f.w v1, v1', 'vwredsum.vs v1, v1, v1', 'vlseg2e8.v v0, (a0), v0.t',
    'vlseg2e8.v v1, (a0), v0.t', 'vlseg2e8.v v0, (a0)', 'vluxseg2ei8.v v2, (a0), v2',
    'vluxseg2ei8.v v2, (a0), v3', 'vluxseg2ei8.v v1, (a0), v2', 'vluxseg2ei8.v v1, (a0), v3',
    'vluxseg3ei8.v v1, (a0), v3', 'vluxseg3ei8.v v1, (a0), v4', 'vsuxseg2ei8.v v2, (a0), v2',
    'vle8.v v0, (a0), v0.t', 'vle8.v v1, 0(a0)', 'vle8.v v1, 4(a0)', 'vle8.v v1, (a0), v1',
    'vlse8.v v1, (a0), a1', 'vlse8.v v1, (a0)', 'vloxei64.v v1, (a0), v2',
    'vmv2r.v v1, v2', 'vmv2r.v v2, v4', 'vmv4r.v v4, v6', 'vmv8r.v v8, v16', 'vl2re8.v v1, (a0)',
    'vl2re8.v v2, (a0)', 'vl8re16.v v24, (a0)', 'vl8re16.v v28, (a0)', 'vs4r.v v6, (a0)',
    'vs4r.v v4, (a0)', 'vl1re8.v v1, (a0)', 'vl1r.v v1, (a0)', 'vle1.v v1, (a0)', 'vse1.v v1, (a0)',
    # the aliases
    'vneg.v v1, v2', 'vnot.v v1, v2, v0.t', 'vwcvt.x.x.v v2, v4', 'vwcvtu.x.x.v v2, v4, v0.t',
    'vncvt.x.x.w v1, v2', 'vfneg.v v1, v2', 'vfabs.v v1, v2, v0.t', 'vmmv.m v1, v2',
    'vmclr.m v1', 'vmset.m v1', 'vmnot.m v1, v2', 'vmsgt.vv v1, v2, v3', 'vmsgtu.vv v1, v2, v3',
    'vmsge.vv v1, v2, v3', 'vmsgeu.vv v1, v2, v3', 'vmfgt.vv v1, v2, v3', 'vmfge.vv v1, v2, v3',
    'vfredsum.vs v1, v2, v3', 'vfwredsum.vs v1, v2, v3', 'vpopc.m a0, v2', 'vmandnot.mm v1, v2, v3',
    'vmornot.mm v1, v2, v3', 'vmfirst.m a0, v2',
    # the pseudo instructions of more than one instruction
    'vmsge.vx v1, v2, a0', 'vmsgeu.vx v1, v2, a0', 'vmsge.vx v1, v2, a0, v0.t',
    'vmsgeu.vx v1, v2, a0, v0.t', 'vmsge.vx v0, v2, a0, v0.t', 'vmsge.vx v1, v2, a0, v0.t, v3',
    'vmsgeu.vx v1, v2, a0, v0.t, v3', 'vmsge.vx v0, v2, a0, v0.t, v3', 'vmsge.vx v1, v2, a0, v0.t, v1',
    'vmsge.vx v1, v2, a0, v0.t, v0', 'vmsge.vx v1, v2, a0, v3', 'vmsge.vx v0, v2, a0',
    'vmsge.vi v1, v2, 5', 'vmsge.vi v1, v2, -15', 'vmsge.vi v1, v2, -16', 'vmsge.vi v1, v2, 16',
    'vmsge.vi v1, v2, 17', 'vmsgeu.vi v1, v2, 0', 'vmsgeu.vi v1, v2, 1', 'vmsgeu.vi v1, v2, -1',
    'vmsgeu.vi v1, v2, 0, v0.t', 'vmsge.vi v0, v2, 5, v0.t',
    # vsetvli and its vtype
    'vsetvli a0, a1, e32, m1, ta, ma', 'vsetvli a0, a1, e8', 'vsetvli a0, a1, ta', 'vsetvli a0, a1, m2',
    'vsetvli a0, a1, e16alt, mf8, ta, ma', 'vsetvli a0, a1, e32alt', 'vsetvli a0, a1, e64, mf8',
    'vsetvli a0, a1, e8, m1,', 'vsetvli a0, a1, e8,,m1', 'vsetvli a0, a1, ma, ta', 'vsetvli a0, a1, E8',
    'vsetvli a0, a1, 7', 'vsetvli a0, a1, 2047', 'vsetvli a0, a1, 2048', 'vsetvli a0, a1, 0X10',
    'vsetivli a0, 31, e8, m1', 'vsetivli a0, 32, e8, m1', 'vsetivli a0, 3, 1023', 'vsetivli a0, 3, 1024',
    'vsetvl a0, a1, a2', 'vsetvli zero, zero, e64, m8, tu, mu',
    'vadd.vi v1, v2, 15', 'vadd.vi v1, v2, 16', 'vadd.vi v1, v2, -16', 'vadd.vi v1, v2, -17',
    'vsll.vi v1, v2, 31', 'vsll.vi v1, v2, 32', 'vror.vi v1, v2, 63', 'vror.vi v1, v2, 64',
    'vadd.vv V1, v2, v3', 'vadd.vv v1, v2, v3, V0.T', 'VADD.VV v1, v2, v3', 'vadd.vx v1, v2, A0',
]

# The feature sets of the decode rows (the dismount spec, the LLVM features)
DECODE_SETS = ['none,+zve32x', 'none,+zve32f', 'none,+zve64x', 'rva23u64']


def vector_corpus_lines(bits):
    lines = []
    for path in sorted(glob.glob(os.path.join(CORPUS, 'rv%d' % bits, 'v_*.s'))):
        if os.path.basename(path) == 'v_pseudo.s':
            continue
        for raw in open(path):
            if raw.startswith('#') or not raw.strip():
                continue
            lines.append(re.sub(r'\s*# @feature:.*$', '', raw.rstrip('\n')).strip())
    return lines


def vector_rows(per_mnemonic):
    """The text rows of the vector instructions, the decode rows and the
    assembly rows."""
    text_rows = []
    first = {}
    for bits in (32, 64):
        lines = vector_corpus_lines(bits)
        features = rvlib.all_features(bits)
        reference = rvlib.llvm_assemble(lines, bits, features, rvc=False)
        units = []
        position = rvlib.BASE
        for line, (status, data) in zip(lines, reference):
            if status == 'ok' and len(data) == 4:
                units.append((position, data))
                position += len(data)
        default = rvlib.objdump_units(units, bits, features)
        noalias = rvlib.objdump_units(units, bits, features, ['-M', 'no-aliases'])
        numeric = rvlib.objdump_units(units, bits, features, ['-M', 'numeric'])
        picked = collections.defaultdict(list)
        for (address, data), d, n, u in zip(units, default, noalias, numeric):
            if d == '<unknown>':
                continue
            key = d.split(' ')[0] + '/' + n.split(' ')[0]
            kind = shape(d) + '|' + shape(n)
            if len(picked[key]) < per_mnemonic and kind not in [k for k, _ in picked[key]]:
                picked[key].append((kind, (data, d, n, u)))
        for key in sorted(picked):
            for _, (data, d, n, u) in picked[key]:
                encoding = int.from_bytes(data, 'little')
                text_rows.append((bits, encoding, d, n, u))
                first.setdefault((bits, key), encoding)

    # The decode rows: one encoding of every mnemonic under the feature sets
    # (what the features gate), the reserved vtype values and the misaligned
    # groups
    decode_rows = []
    for bits in (32, 64):
        encodings = [e for (b, key), e in sorted(first.items()) if b == bits]
        extra = [0x0045f557, 0x0205f557, 0x0405f557, 0x0805f557, 0x1005f557, 0x1085f557, 0x1105f557,
                 0x1205f557, 0x3005f557, 0x7ff5f557, 0x8005f557, 0xc01ff557, 0xc20ff557, 0xff1ff557,
                 0x9e00b057 | (1 << 7), 0x9e00b057 | (1 << 20), 0x9e01b057 | (2 << 7),
                 0x9e03b057 | (4 << 20), 0x02850087 | (1 << 7) | (1 << 12) | (2 << 12)]
        for mattr in DECODE_SETS:
            closure = rvlib.feature_closure(bits, mattr)
            units = []
            position = rvlib.BASE
            for e in encodings + extra:
                units.append((position, e.to_bytes(4, 'little')))
                position += 4
            texts = rvlib.objdump_units(units, bits, closure)
            for (address, data), t in zip(units, texts):
                decode_rows.append((bits, mattr, int.from_bytes(data, 'little'),
                                    '' if t == '<unknown>' else t))

    asm_rows = []
    for bits in (32, 64):
        for mattr, spec in (('all', None), ('none,+zve32x', 'none,+zve32x'),
                            ('none,+zve32f', 'none,+zve32f'),
                            ('none,+zve64x,+zvbb', 'none,+zve64x,+zvbb')):
            if bits == 32 and mattr != 'all':
                continue
            names = rvlib.all_features(bits) if spec is None else rvlib.feature_closure(bits, spec)
            for text in VECTOR_ASM:
                # (a statement of more than one instruction is one line)
                status, data = rvlib.llvm_assemble([text], bits, names, rvc=False)[0]
                asm_rows.append((bits, mattr, text, data.hex() if status == 'ok' else ''))
    return text_rows, decode_rows, asm_rows


def write_vector_rows(per_mnemonic):
    text_rows, decode_rows, asm_rows = vector_rows(per_mnemonic)
    tests = os.path.join(rvlib.REPO, 'tests', 'riscv')
    with open(os.path.join(tests, 'RiscvVectorTextRows.inc'), 'w') as f:
        f.write('// Generated by tests/riscv/tools/gen_unit_rows.py: the rows of\n')
        f.write('// tests/TestRiscvVector.cpp. {bits, encoding, text, no aliases text, numeric text}\n')
        for bits, encoding, d, n, u in text_rows:
            f.write('    { %d, 0x%08x, %s, %s, %s },\n' % (bits, encoding, literal(d), literal(n), literal(u)))
    with open(os.path.join(tests, 'RiscvVectorDecodeRows.inc'), 'w') as f:
        f.write('// Generated by tests/riscv/tools/gen_unit_rows.py: the rows of\n')
        f.write('// tests/TestRiscvVector.cpp. {bits, features, encoding, text ("" if invalid)}\n')
        for bits, mattr, encoding, text in decode_rows:
            f.write('    { %d, %s, 0x%08x, %s },\n' % (bits, literal(mattr), encoding, literal(text)))
    with open(os.path.join(tests, 'RiscvVectorAsmRows.inc'), 'w') as f:
        f.write('// Generated by tests/riscv/tools/gen_unit_rows.py: the rows of\n')
        f.write('// tests/TestRiscvVector.cpp. {bits, features, text, hex bytes ("" if rejected)}\n')
        for bits, mattr, text, hexbytes in asm_rows:
            f.write('    { %d, %s, %s, "%s" },\n' % (bits, literal(mattr), literal(text), hexbytes))
    print('%d vector text rows, %d decode rows, %d assembly rows' % (
        len(text_rows), len(decode_rows), len(asm_rows)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--per-mnemonic', type=int, default=3)
    args = parser.parse_args()
    rows = []
    for bits in (32, 64):
        lines = []
        for path in sorted(glob.glob(os.path.join(CORPUS, 'rv%d' % bits, '*.s'))):
            name = os.path.basename(path)[:-2]
            if name == 'pseudo' or name.startswith('v_'):
                continue
            for raw in open(path):
                if raw.startswith('#') or not raw.strip():
                    continue
                lines.append(re.sub(r'\s*# @feature:.*$', '', raw.rstrip('\n')).strip())
        features = rvlib.all_features(bits)
        reference = [None] * len(lines)
        for compressed in (False, True):
            index = [i for i, l in enumerate(lines) if l.startswith('c.') == compressed]
            got = rvlib.llvm_assemble([lines[i] for i in index], bits, features, rvc=compressed)
            for i, g in zip(index, got):
                reference[i] = g
        units = []
        position = rvlib.BASE
        for line, (status, data) in zip(lines, reference):
            if status == 'ok' and len(data) in (2, 4):
                units.append((position, data))
                position += len(data)
        default = rvlib.objdump_units(units, bits, features)
        noalias = rvlib.objdump_units(units, bits, features, ['-M', 'no-aliases'])
        picked = collections.defaultdict(list)
        for (address, data), d, n in zip(units, default, noalias):
            if d == '<unknown>':
                continue
            key = d.split(' ')[0] + '/' + n.split(' ')[0]
            seen = [s for s, _ in picked[key]]
            if len(picked[key]) < args.per_mnemonic and shape(d) + '|' + shape(n) not in seen:
                picked[key].append((shape(d) + '|' + shape(n), (address, data, d, n)))
        for key in sorted(picked):
            for _, (address, data, d, n) in picked[key]:
                assembly = relative(n if len(data) == 2 else d, address, bits)
                rows.append((bits, len(data), int.from_bytes(data, 'little'), address, d, n, assembly))
    with open(OUTPUT, 'w') as f:
        f.write('// Generated by tests/riscv/tools/gen_unit_rows.py: the rows of\n')
        f.write('// tests/TestRiscvDisassembler.cpp. {bits, size, address, encoding, text,\n')
        f.write('// no aliases text, assembly text}: LLVM text.\n')
        f.write('#define RISCV_TEXT_ROWS \\\n')
        for bits, size, encoding, address, d, n, assembly in rows:
            f.write('    { %d, %d, 0x%x, 0x%08x, %s, %s, %s }, \\\n' % (
                bits, size, address, encoding, literal(d), literal(n), literal(assembly)))
        f.write('\n')
    print('%d rows -> %s' % (len(rows), OUTPUT))
    write_vector_rows(2)
    pseudo = pseudo_rows()
    with open(os.path.join(rvlib.REPO, 'tests', 'riscv', 'RiscvPseudoRows.inc'), 'w') as f:
        f.write('// Generated by tests/riscv/tools/gen_unit_rows.py: the rows of the pseudo\n')
        f.write('// instructions of tests/TestRiscvDisassembler.cpp. {bits, features, text, hex,\n')
        f.write('// compressed hex}: llvm-mc bytes.\n')
        f.write('#define RISCV_PSEUDO_ROWS \\\n')
        for bits, mattr, text, a, b in pseudo:
            f.write('    { %d, %s, %s, "%s", "%s" }, \\\n' % (bits, literal(mattr), literal(text), a, b))
        f.write('\n')
    print('%d pseudo rows' % len(pseudo))


if __name__ == '__main__':
    main()
