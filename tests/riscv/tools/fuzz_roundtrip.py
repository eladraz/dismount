#!/usr/bin/env python3
"""Random valid encodings: dismount bytes -> text -> dismount bytes, and the
text through llvm-mc.

  python3 tests/riscv/tools/fuzz_roundtrip.py [--count N] [--seed S] [--sets all,rv32gc,...]

For every random unit which dismount decodes, the text it prints (the default
text of the 32 bit instructions and the no-aliases text of all of them, with the
branch targets as offsets) is assembled by dismount and by llvm-mc: the bytes
have to be the original ones, or other bytes which disassemble to the same
text (ALT, counted). A text which an assembler rejects is a mismatch. Exit
status 1 if any unit mismatches.
"""
import argparse
import os
import random
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rvlib
import fuzz_decode

BRANCHES = set('beq bne blt bge bltu bgeu beqz bnez blez bgez bltz bgtz j jal c.j c.jal c.beqz c.bnez'.split())


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


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--count', type=int, default=100000)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--sets', default='all,rva23u64,rv64gc,rv32gc')
    parser.add_argument('--show', type=int, default=12)
    args = parser.parse_args()
    failures = 0
    for name in args.sets.split(','):
        bits = 32 if name.startswith('rv32') else 64
        mattr, _ = fuzz_decode.SETS[name]
        features = fuzz_decode.feature_list(bits, mattr)
        entries = fuzz_decode.read_entries(bits)
        rng = random.Random(args.seed * 7919 + sum(map(ord, name)))
        units = []
        while len(units) < args.count:
            size, value = fuzz_decode.random_unit(rng, entries, bits)
            if size in (2, 4):
                units.append((rvlib.BASE, value.to_bytes(size, 'little')))
        decoded = rvlib.dismount_dis(units, bits, mattr)
        decoded_na = rvlib.dismount_dis(units, bits, mattr, 'no-aliases')
        total = alt = bad = both_reject = 0
        shown = 0
        for kind, texts in (('default', decoded), ('no-aliases', decoded_na)):
            check = []
            for (address, data), t in zip(units, texts):
                if t[0] != 'OK':
                    continue
                if kind == 'default' and len(data) == 2:
                    continue
                # (the vector instructions are not part of this scope)
                check.append((data, relative(t[1], address, bits)))
            lines = [t for _, t in check]
            mine = rvlib.dismount_asm(lines, bits, mattr, rvc=False)
            # llvm-mc: the 32 bit and the compressed lines in their own runs
            llvm = [None] * len(lines)
            for compressed in (False, True):
                index = [i for i, l in enumerate(lines) if l.startswith('c.') == compressed]
                if not index:
                    continue
                got = rvlib.llvm_assemble([lines[i] for i in index], bits, features, rvc=compressed)
                for i, g in zip(index, got):
                    llvm[i] = g
            for i, (data, text) in enumerate(check):
                total += 1
                problems = []
                if mine[i][0] != 'OK':
                    problems.append('dismount rejects: %s' % mine[i][1])
                elif mine[i][1] != data:
                    other = rvlib.dismount_dis([(rvlib.BASE, mine[i][1])], bits, mattr,
                                               'no-aliases' if kind == 'no-aliases' else None)[0]
                    if other[0] == 'OK' and other[1] == text:
                        alt += 1
                    else:
                        problems.append('dismount gives %s' % mine[i][1].hex())
                if llvm[i][0] != 'ok':
                    problems.append('llvm-mc rejects: %s' % llvm[i][1])
                elif llvm[i][1] != data:
                    problems.append('llvm-mc gives %s' % llvm[i][1].hex())
                # (LLVM prints what it can't read: aes64ks1i with a round number > 10;
                # when both assemblers reject the text it is not a mismatch)
                if mine[i][0] != 'OK' and llvm[i][0] != 'ok':
                    both_reject += 1
                    continue
                if problems:
                    bad += 1
                    if shown < args.show:
                        shown += 1
                        print('%s/%s %s "%s": %s' % (name, kind, data.hex(), text, '; '.join(problems)))
        # The compression: the 32 bit texts assembled with C on, dismount vs llvm-mc
        if 'zca' in features:
            lines = []
            for (address, data), t in zip(units, decoded):
                # (the expansions of the 16 bit instructions are the compressible ones)
                if t[0] == 'OK' and not t[1].startswith('c.'):
                    lines.append(relative(t[1], address, bits))
            ours = rvlib.dismount_asm(lines, bits, mattr, rvc=True)
            theirs = rvlib.llvm_assemble(lines, bits, features, rvc=True)
            packed = 0
            for line, o, l in zip(lines, ours, theirs):
                if l[0] != 'ok':
                    continue
                if len(l[1]) == 2:
                    packed += 1
                if o[0] != 'OK' or o[1] != l[1]:
                    bad += 1
                    if shown < args.show:
                        shown += 1
                        print('%s/compress "%s": llvm-mc %s dismount %s' % (
                            name, line, l[1].hex(), o[1].hex() if o[0] == 'OK' else o[1]))
            print('%-9s %7d compressed texts (%d became 16 bit)' % (name, len(lines), packed))
        print('%-9s %7d texts, %d alt, %d rejected by both (LLVM prints what it cannot read), %d mismatches' % (
            name, total, alt, both_reject, bad))
        failures += bad
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
