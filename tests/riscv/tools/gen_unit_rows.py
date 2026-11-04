#!/usr/bin/env python3
"""Generate tests/riscv/RiscvTestRows.inc, the rows of tests/TestRiscvDisassembler.cpp:
{bits, size, encoding, "text", "no aliases text", "assembly text"} for a few shapes
of every mnemonic of the corpus (RV32 and RV64, the compressed instructions
and the aliases), as llvm-objdump prints them, so the unit test pins the LLVM
syntax. The assembly text is the text with the branch targets as offsets.

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


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--per-mnemonic', type=int, default=3)
    args = parser.parse_args()
    rows = []
    for bits in (32, 64):
        lines = []
        for path in sorted(glob.glob(os.path.join(CORPUS, 'rv%d' % bits, '*.s'))):
            name = os.path.basename(path)[:-2]
            if name == 'pseudo':
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
