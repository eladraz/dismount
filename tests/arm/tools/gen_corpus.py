#!/usr/bin/env python3
"""Generate the A64 corpus, tests/arm/corpus/a64/<feature>.s
(tests/arm/TEST_PLAN.md).

The lines come from two sources:
  - every encoding of the dismount tables (dismount_cli entries), instantiated
    with the operand fields all zero, all one, alternating and random (seeded),
  - a frozen, seeded sample of the random words which objdump decodes, with at
    most a few per mnemonic. The classes dismount does not implement yet (the
    scalar floating point and Advanced SIMD, SVE and SME) are in simd.s,
    sve.s and sme.s: the next implementation finds them there.

The text of a line is the objdump text of the word, with the branch target
written as an offset from the line (".+8"): the corpus does not depend on the
address of the line. The feature of a line is what clang asks for
("instruction requires: lse"), the lines are in a file named after their
features ("base.s", "lse.s", "mops+mte.s").

    gen_corpus.py [--arch a64] [--per-mnemonic N]
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib

CORPUS = os.path.join(armlib.REPO, 'tests', 'arm', 'corpus')

# The mnemonics whose last operand can be an address
_PCREL = re.compile(r'^(b|bl|b\.\w+|bc\.\w+|cbz|cbnz|tbz|tbnz|cb\w+|adr|adrp|ldr|ldrsw|prfm|'
                    r'retaasppc|retabsppc|autiasppc|autibsppc) ')
_TARGET = re.compile(r'(?<![#\w-])(0x[0-9a-f]+)$')


def to_relative(text, address):
    """Replace the absolute target of a pc relative instruction by '.+offset'."""
    if not _PCREL.match(text):
        return text
    m = _TARGET.search(text)
    if not m:
        return text
    target = int(m.group(1), 16)
    if text.startswith('adrp '):
        # clang leaves a relocation for ".+N"; "#N" is the page offset
        return text[:m.start()] + '#%d' % (target - (address & ~0xfff))
    offset = target - address
    # 64 bit wraparound: objdump prints the address modulo 2^64
    if offset >= 1 << 63:
        offset -= 1 << 64
    if offset < -(1 << 63):
        offset += 1 << 64
    sign = '+' if offset >= 0 else '-'
    return text[:m.start()] + '.%s%d' % (sign, abs(offset))


def candidate_words(rows, rng, per_entry_random=6):
    words = []
    for mask, value, feature, flags, size, mnemonic in rows:
        if size != 4:
            continue
        free = ~mask & 0xffffffff
        words.append(value)
        words.append(value | free)
        words.append(value | (free & 0x55555555))
        words.append(value | (free & 0xaaaaaaaa))
        for _ in range(per_entry_random):
            words.append(value | (rng.getrandbits(32) & free))
    return words


def random_class_words(rng, wanted_class, count):
    words = []
    while len(words) < count:
        w = rng.getrandbits(32)
        if armlib.a64_class(w) == wanted_class:
            words.append(w)
    return words


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--arch', default='a64', choices=['a64'])
    parser.add_argument('--per-mnemonic', type=int, default=6)
    args = parser.parse_args()

    rng = armlib.seeded_rng(20240607)
    rows = armlib.entries(args.arch)
    sources = []        # (word, class)
    for w in candidate_words(rows, rng):
        sources.append(w)
    for cls, count in (('base', 40000), ('simd', 60000), ('sve', 60000), ('sme', 60000)):
        sources.extend(random_class_words(rng, cls, count))
    sources = sorted(set(sources))

    texts, addresses = armlib.objdump_chunks(sources, args.arch)
    # Decodable words: the table instantiations all, the random ones capped per
    # mnemonic
    table_words = set(candidate_words(rows, armlib.seeded_rng(20240607)))
    seen = collections.Counter()
    chosen = []        # (text relative, class)
    seen_text = set()
    for word, address, text in zip(sources, addresses, texts):
        if text == '<unknown>':
            continue
        cls = armlib.a64_class(word)
        mnemonic = armlib.mnemonic_of(text)
        if word not in table_words:
            key = (cls, mnemonic)
            if seen[key] >= args.per_mnemonic:
                continue
            seen[key] += 1
        line = to_relative(text, address)
        if line in seen_text:
            continue
        seen_text.add(line)
        chosen.append((line, cls))

    # The features of each line: clang without any feature
    results = armlib.clang_assemble([line for line, _ in chosen], args.arch, [])
    files = collections.defaultdict(list)
    dropped = collections.Counter()
    for (line, cls), (status, data) in zip(chosen, results):
        if status == 'ok':
            required = []
        else:
            m = re.match(r'instruction requires: (.*)$', data)
            if not m:
                # Not an instruction clang takes in this form: not a corpus line
                dropped[data.split(' ')[0] if data else 'error'] += 1
                continue
            required = sorted(m.group(1).split())
        name = '+'.join(required) if required else 'base'
        if cls in ('simd', 'sve', 'sme'):
            name = cls
        files[name].append((line, required))
    # Where objdump decodes and clang takes the line with all the features, it
    # was not required: the lines clang refuses are dropped above

    out_dir = os.path.join(CORPUS, args.arch)
    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(out_dir):
        if name.endswith('.s'):
            os.unlink(os.path.join(out_dir, name))
    total = 0
    for name, lines in sorted(files.items()):
        with open(os.path.join(out_dir, name + '.s'), 'w') as f:
            f.write('// Generated by tests/arm/tools/gen_corpus.py. One instruction per line;\n')
            f.write('// "// @feature: x" is the LLVM feature the line needs (none: base).\n')
            for line, required in lines:
                if required:
                    f.write('%s // @feature: %s\n' % (line, ' '.join(required)))
                else:
                    f.write('%s\n' % line)
        total += len(lines)
        print('%-24s %d' % (name + '.s', len(lines)))
    print('%d lines; dropped %s' % (total, dict(dropped)))


if __name__ == '__main__':
    main()
