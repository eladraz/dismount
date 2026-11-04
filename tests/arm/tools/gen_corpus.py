#!/usr/bin/env python3
"""Generate the A64 corpus, tests/arm/corpus/a64/<feature>.s
(tests/arm/TEST_PLAN.md).

The lines come from two sources:
  - every encoding of the dismount tables (dismount_cli entries), instantiated
    with the operand fields all zero, all one, alternating and random (seeded),
  - a frozen, seeded sample of the random words which objdump decodes, with at
    most a few per mnemonic. The classes dismount does not implement yet (the
    scalar floating point and Advanced SIMD, SVE and SME) are in simd.s,
    sve.s and sme.s: the next implementation finds them there. The SVE lines
    (sve*.s, from the SVE table and a random sample of its class) are named
    after the feature they need, which is found by decoding them with only
    that feature on ("sve.s", "sve-sve2p1.s"...).

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

# The features which clang names in "instruction requires:" differently from the
# -target-feature that enables them
FEATURE_ALIASES = {'frint3264': 'fptoint'}

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


# The features of the SVE table, the weaker ones first (a line needs the first
# one with which dismount decodes it)
SVE_FEATURES = ['sve', 'sve2', 'sve2p1', 'sve2p2', 'sve2p3', 'sve2-aes', 'sve2-sha3',
                'sve2-sm4', 'sve2-bitperm', 'f32mm', 'f64mm', 'i8mm', 'bf16', 'sve-b16b16',
                'sve-aes2', 'sve-bfscale', 'lut', 'fp8', 'fp8dot2', 'fp8dot4', 'fp8fma',
                'f8f32mm', 'f8f16mm', 'cpa', 'faminmax', 'frintts', 'sme']


def is_sve_row(row):
    """A row of the SVE table: the SVE class and the multi vector contiguous loads and
    stores of SVE2.1 (the SME class, but not the loads of the tiles)"""
    mask, value, feature, flags, size, mnemonic = row
    return armlib.a64_class(value) == 'sve' or (
        (value >> 25) == 0x50 and re.match(r'(ld|st)(nt)?1[bhwd]$', mnemonic) is not None)


def is_sme_row(row):
    return armlib.a64_class(row[1]) == 'sme' and not is_sve_row(row)


def is_sve_word(word):
    """The words of the SVE table: the SVE class, and the multi vector
    contiguous loads and stores of SVE2.1 which are in the SME class."""
    return armlib.a64_class(word) == 'sve' or ((word >> 25) & 0x7f) == 0x50


def sve_feature(words, texts):
    """The SVE feature of each word (None if dismount doesn't decode it): the
    first of SVE_FEATURES with which it decodes to the objdump text."""
    result = [None] * len(words)
    pending = list(range(len(words)))
    addresses = [armlib.BASE + 4 * i for i in range(len(words))]
    for feature in SVE_FEATURES:
        if not pending:
            break
        decoded = armlib.dis_words('a64', [words[i] for i in pending], 'none,+' + feature,
                                   [addresses[i] for i in pending])
        still = []
        for i, (status, text) in zip(pending, decoded):
            if status == 'OK' and text == texts[i]:
                result[i] = feature
            else:
                still.append(i)
        pending = still
    return result


def ssve_feature(text):
    """The streaming feature of an FP8 line of SVE: fmlalb... ssve-fp8fma, fdot z.h ssve-fp8dot2,
    fdot z.s ssve-fp8dot4 (None for the others)"""
    mnemonic = armlib.mnemonic_of(text)
    if mnemonic.startswith('fmlal'):
        return 'ssve-fp8fma'
    if mnemonic == 'fdot':
        return 'ssve-fp8dot2' if re.match(r'fdot z\d+\.h', text) else 'ssve-fp8dot4'
    return None


def generate_sve(args, rows):
    """tests/arm/corpus/a64/sve*.s: the lines of the SVE table (every entry
    with several operand values) and a random sample of the SVE class, named
    after the feature they need."""
    rng = armlib.seeded_rng(20240608)
    sve_rows = [r for r in rows if r[4] == 4 and is_sve_row(r)]
    words = list(candidate_words(sve_rows, rng, per_entry_random=8))
    table_words = set(words)
    words.extend(random_class_words(rng, 'sve', 20000))
    # The multi vector loads and stores of SVE2.1 (the SME class)
    while len(words) < len(table_words) + 20000 + 4000:
        words.append((rng.choice((0xa0, 0xa1)) << 24) | rng.getrandbits(24))
    words = sorted(set(words))
    texts, addresses = armlib.objdump_chunks(words, args.arch)

    seen = collections.Counter()
    items = []
    seen_text = set()
    for word, text in zip(words, texts):
        if text == '<unknown>' or not is_sve_word(word):
            continue
        mnemonic = armlib.mnemonic_of(text)
        if armlib.a64_class(word) != 'sve' and mnemonic not in (
                'ld1b', 'ld1h', 'ld1w', 'ld1d', 'ldnt1b', 'ldnt1h', 'ldnt1w', 'ldnt1d',
                'st1b', 'st1h', 'st1w', 'st1d', 'stnt1b', 'stnt1h', 'stnt1w', 'stnt1d'):
            continue
        if word not in table_words:
            if seen[mnemonic] >= args.per_mnemonic * 8:
                continue
            seen[mnemonic] += 1
        if text in seen_text:
            continue
        seen_text.add(text)
        items.append((word, text))
    features = sve_feature([w for w, _ in items], [t for _, t in items])
    results = armlib.clang_assemble([t for _, t in items], args.arch)
    files = collections.defaultdict(list)
    dropped = collections.Counter()
    for (word, text), feature, (status, data) in zip(items, features, results):
        if feature is None:
            dropped['not decoded'] += 1
            continue
        if status != 'ok':
            dropped['clang'] += 1
            continue
        files['sve' if feature == 'sve' else 'sve-' + feature].append((text, [feature]))
        # The FP8 instructions of SVE are guarded by "ssve-fp8fma or (sve2 and fp8fma)": the streaming
        # feature of each line goes to its own file ("sve-ssve-fp8dot2.s")
        if feature == 'fp8dot2':
            streaming = ssve_feature(text)
            if streaming:
                files['sve-' + streaming].append((text, [streaming]))
    out_dir = os.path.join(CORPUS, args.arch)
    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(out_dir):
        if name.startswith('sve') and name.endswith('.s'):
            os.unlink(os.path.join(out_dir, name))
    total = 0
    for name, lines in sorted(files.items()):
        with open(os.path.join(out_dir, name + '.s'), 'w') as f:
            f.write('// Generated by tests/arm/tools/gen_corpus.py. One instruction per line;\n')
            f.write('// "// @feature: x" is the LLVM feature the line needs (none: base).\n')
            for line, required in lines:
                f.write('%s // @feature: %s\n' % (line, ' '.join(required)))
        total += len(lines)
        print('%-24s %d' % (name + '.s', len(lines)))
    print('%d SVE lines; dropped %s' % (total, dict(dropped)))


def is_sme_word(word, text):
    """The words of the SME table: the SME class but the multi vector contiguous
    loads and stores of SVE2.1 (the SVE table)"""
    if armlib.a64_class(word) != 'sme':
        return False
    return not (re.match(r'(ld|st)(nt)?1[bhwd] ', text) and 'pn' in text)


def generate_sme(args, rows):
    """tests/arm/corpus/a64/sme*.s: the lines of the SME table (every entry with
    several operand values) and a random sample of the SME class, named after
    the features clang asks for ("sme.s", "sme2.s", "sme-f64f64+sme2.s", "sme-fp8+sme2.s")"""
    rng = armlib.seeded_rng(20240609)
    sme_rows = [r for r in rows if r[4] == 4 and is_sme_row(r)]
    words = list(candidate_words(sme_rows, rng, per_entry_random=3))
    table_words = set(words)
    # The random sample: the top bytes of the class, fully random below
    tops = (0x80, 0x81, 0xa0, 0xa1, 0xc0, 0xc1, 0xe0, 0xe1)
    for _ in range(300000):
        words.append((rng.choice(tops) << 24) | rng.getrandbits(24))
    words = sorted(set(words))
    texts, addresses = armlib.objdump_chunks(words, args.arch)

    seen = collections.Counter()
    items = []
    seen_text = set()
    for word, text in zip(words, texts):
        if text == '<unknown>' or not is_sme_word(word, text):
            continue
        key = (armlib.mnemonic_of(text), re.sub(r'\d+', 'N', text))
        if word not in table_words:
            if seen[key] >= 2:
                continue
            seen[key] += 1
        if text in seen_text:
            continue
        seen_text.add(text)
        items.append((word, text))
    results = armlib.clang_assemble([t for _, t in items], args.arch, [])
    files = collections.defaultdict(list)
    dropped = collections.Counter()
    for (word, text), (status, data) in zip(items, results):
        if status == 'ok':
            required = []
        else:
            m = re.match(r'instruction requires: (.*)$', data)
            if not m:
                dropped[data.split(' ')[0] if data else 'error'] += 1
                continue
            # "a or b": the line is tagged with the first one
            required = sorted(m.group(1).split(' or ')[0].split())
        name = 'sme' if required in (['sme'], []) else (
            '+'.join(required) if required[0].startswith('sme') else 'sme-' + '+'.join(required))
        files[name].append((text, required))
    out_dir = os.path.join(CORPUS, args.arch)
    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(out_dir):
        if name.startswith('sme') and name.endswith('.s'):
            os.unlink(os.path.join(out_dir, name))
    total = 0
    for name, lines in sorted(files.items()):
        with open(os.path.join(out_dir, name + '.s'), 'w') as f:
            f.write('// Generated by tests/arm/tools/gen_corpus.py. One instruction per line;\n')
            f.write('// "// @feature: x" is the LLVM feature the line needs (none: base).\n')
            for line, required in lines:
                f.write('%s // @feature: %s\n' % (line, ' '.join(required or ['sme'])))
        total += len(lines)
        print('%-28s %d' % (name + '.s', len(lines)))
    print('%d SME lines; dropped %s' % (total, dict(dropped)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--arch', default='a64', choices=['a64', 'a32', 't32'])
    parser.add_argument('--per-mnemonic', type=int, default=6)
    parser.add_argument('--class', dest='only_class', choices=['base', 'simd', 'sve', 'sme'],
                        help='write only the files of this encoding class (simd: simd.s and '
                             'simd-<feature>.s) and leave the others alone')
    parser.add_argument('--random', type=int, default=200000,
                        help='AArch32: the number of random words to sample')
    args = parser.parse_args()
    if args.arch != 'a64':
        import gen_corpus_aarch32
        gen_corpus_aarch32.generate(args.arch, args)
        return

    rng = armlib.seeded_rng(20240607)
    rows = armlib.entries(args.arch)
    # The instantiations of the scalar floating point and Advanced SIMD
    # encodings have their own random stream, so the lines of the other
    # classes don't change when that table changes
    if args.only_class == 'sve':
        generate_sve(args, rows)
        return
    if args.only_class == 'sme':
        generate_sme(args, rows)
        return
    simd_rows = [r for r in rows if armlib.a64_class(r[1]) == 'simd']
    all_rows = rows
    rows = [r for r in rows if armlib.a64_class(r[1]) != 'simd' and not is_sme_row(r)]
    sources = []        # (word, class)
    for w in candidate_words(rows, rng):
        sources.append(w)
    simd_table_words = candidate_words(simd_rows, armlib.seeded_rng(20240608), 12)
    sources.extend(simd_table_words)
    for cls, count in (('base', 40000), ('simd', 60000), ('sve', 60000), ('sme', 60000)):
        sources.extend(random_class_words(rng, cls, count))
    sources = sorted(set(sources))

    texts, addresses = armlib.objdump_chunks(sources, args.arch)
    # Decodable words: the table instantiations all, the random ones capped per
    # mnemonic
    table_words = set(candidate_words(rows, armlib.seeded_rng(20240607)))
    table_words |= set(simd_table_words)
    seen = collections.Counter()
    chosen = []        # (text relative, class)
    seen_text = set()
    for word, address, text in zip(sources, addresses, texts):
        if text == '<unknown>' or is_sve_word(word) or is_sme_word(word, text):
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
        if args.only_class and cls != args.only_class:
            continue
        if status == 'ok':
            required = []
        else:
            m = re.match(r'instruction requires: (.*)$', data)
            if not m:
                # Not an instruction clang takes in this form: not a corpus line
                dropped[data.split(' ')[0] if data else 'error'] += 1
                continue
            required = sorted(FEATURE_ALIASES.get(f, f) for f in m.group(1).split())
        name = '+'.join(required) if required else 'base'
        if cls in ('sve', 'sme'):
            name = cls
        elif cls == 'simd':
            # The scalar floating point and Advanced SIMD of ARMv8.0; the
            # extensions are in the files simd-<feature>.s
            name = 'simd-' + name if required else cls
        files[name].append((line, required))
    # Where objdump decodes and clang takes the line with all the features, it
    # was not required: the lines clang refuses are dropped above

    out_dir = os.path.join(CORPUS, args.arch)
    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(out_dir):
        # (the files sve*.s and sme*.s are the ones of generate_sve and generate_sme)
        if not name.endswith('.s') or name.startswith('sve') or name.startswith('sme'):
            continue
        # With --class only the files of the class are replaced
        if args.only_class == 'simd' and not (name == 'simd.s' or name.startswith('simd-')):
            continue
        if args.only_class == 'base' and name != 'base.s':
            continue
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
    if args.only_class is None:
        generate_sve(args, all_rows)
        generate_sme(args, all_rows)


if __name__ == '__main__':
    main()
