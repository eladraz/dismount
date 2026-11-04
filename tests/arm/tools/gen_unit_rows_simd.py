#!/usr/bin/env python3
"""Generate tests/arm/AArch32TestRows_Simd.inc, the VFP / Advanced SIMD rows of
tests/TestArmAArch32Disassembler.cpp:

    AARCH32_SIMD_A32_ROWS      { address, word, "text" }
    AARCH32_SIMD_T32_ROWS      { address, size, encoding, itState, "text" }
    AARCH32_SIMD_INVALID       { encoding, size, isT32 }

A few shapes of every mnemonic of the A32 corpus; their T32 forms (the A32
word with the top byte of the T32 encoding: 0xF2 / 0xF3 -> 0xEF / 0xFF,
0xF4 -> 0xF9, the condition -> 0xE), plain and inside an it block (the text
has the condition: "vaddeq.f32"); invalid words. The texts are what objdump
prints, so the unit test pins the LLVM syntax.

    gen_unit_rows_simd.py [--per-mnemonic N]
"""
import argparse
import collections
import glob
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib
import gen_corpus_aarch32

OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'AArch32TestRows_Simd.inc')
CORPUS = os.path.join(armlib.REPO, 'tests', 'arm', 'corpus', 'a32')
SIMD_MNEMONIC = re.compile(r'^(v|fldm|fstm|aes|sha)')


def shape(text):
    return re.sub(r'\b([rsdq])\d+\b', r'\1N', re.sub(r'0x[0-9a-f]+|\d+', 'N', text))


def to_t32(word):
    """The T32 form of an A32 VFP / NEON word, None if it has none."""
    top = word >> 24
    if top in (0xf2, 0xf3):
        return (0xef + 0x10 * (top - 0xf2)) << 24 | (word & 0xffffff)
    if top == 0xf4:
        return 0xf9 << 24 | (word & 0xffffff)
    if top in (0xfc, 0xfd, 0xfe):
        return word
    if (top & 0xf) in (0xe, 0xc, 0xd):
        return 0xe << 28 | (word & 0x0fffffff)
    return None


def thumb_texts(encodings, in_it):
    """objdump (thumb) of 32 bit encodings, each alone or after 'it eq'."""
    tmp = tempfile.mkdtemp(prefix='armrows')
    src = os.path.join(tmp, 'a.s')
    obj = os.path.join(tmp, 'a.o')
    with open(src, 'w') as f:
        f.write('.thumb\n')
        for e in encodings:
            if in_it:
                f.write('it eq\n')
            f.write('.inst.w 0x%08x\n' % e)
    subprocess.run(['clang', '-target', 'thumbv8a-none-eabi', '-c', src, '-o', obj], check=True)
    r = subprocess.run(['objdump', '-d', '--mattr=' + armlib.OBJDUMP_MATTR['t32'], obj],
                       capture_output=True, text=True)
    texts = []
    for line in r.stdout.splitlines():
        m = re.match(r'^\s*[0-9a-f]+:\s+([0-9a-f]{4})( [0-9a-f]{4})?\s+(.*)$', line)
        if m:
            texts.append((m.group(2) is not None, armlib.normalize(m.group(3))))
    # only the 32 bit instructions (the it is a 16 bit one)
    return [t for wide, t in texts if wide] if True else texts


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--per-mnemonic', type=int, default=3)
    args = parser.parse_args()

    lines = []
    for path in sorted(glob.glob(os.path.join(CORPUS, '*.s'))):
        for raw in open(path):
            if raw.startswith('//') or not raw.strip():
                continue
            lines.append(re.sub(r'\s*// @feature:.*$', '', raw.rstrip('\n')).strip())
    lines = [l for l in lines if SIMD_MNEMONIC.match(l)]
    results = armlib.clang_assemble(lines, 'a32')
    words = [armlib.hex_bytes_to_word(d) for (s, d) in results if s == 'ok']
    texts, addresses = armlib.objdump_chunks(words, 'a32')
    names = gen_corpus_aarch32.base_names(armlib.entries('a32'))
    picked = collections.defaultdict(list)
    for word, address, text in zip(words, addresses, texts):
        if text == '<unknown>':
            continue
        key = gen_corpus_aarch32.key_of(text, names)
        shapes = [shape(t) for _, _, t in picked[key]]
        if len(picked[key]) < args.per_mnemonic and shape(text) not in shapes:
            picked[key].append((address, word, text))
    a32_rows = []
    for key in sorted(picked):
        a32_rows.extend(picked[key])

    # The T32 forms: one or two shapes per mnemonic
    t32_words = []
    for key in sorted(picked):
        for address, word, text in picked[key][:2]:
            t = to_t32(word)
            if t is not None:
                t32_words.append(t)
    plain = thumb_texts(t32_words, False)
    in_it = thumb_texts(t32_words, True)
    t32_rows = []
    for i, w in enumerate(t32_words):
        if i >= len(plain) or plain[i] == '<unknown>':
            continue
        t32_rows.append((armlib.BASE + 4 * i, w, 0, plain[i]))
        # an it eq block: ITSTATE 0x08 applies to the instruction
        if i < len(in_it) and in_it[i] != '<unknown>' and in_it[i] != plain[i] and i % 3 == 0:
            t32_rows.append((armlib.BASE + 4 * i, w, 0x08, in_it[i]))

    rng = armlib.seeded_rng(11)
    invalid = []
    while len(invalid) < 40:
        w = rng.getrandbits(32)
        if (w >> 24) in (0xf2, 0xf3, 0xf4, 0xee, 0xec, 0xed):
            invalid.append(w)
    inv_texts = armlib.objdump_words(invalid, 'a32')
    invalid = [w for w, t in zip(invalid, inv_texts) if t == '<unknown>']

    with open(OUTPUT, 'w') as f:
        f.write('// Generated by tests/arm/tools/gen_unit_rows_simd.py: the VFP / Advanced SIMD\n')
        f.write('// rows of tests/TestArmAArch32Disassembler.cpp. The texts are the LLVM ones.\n')
        f.write('#define AARCH32_SIMD_A32_ROWS \\\n')
        for address, word, text in a32_rows:
            f.write('    { 0x%x, 0x%08x, "%s" }, \\\n' % (address, word, text.replace('"', '\\"')))
        f.write('\n// { address, size, encoding (hw1 << 16 | hw2), itState, text }\n')
        f.write('#define AARCH32_SIMD_T32_ROWS \\\n')
        for address, word, it, text in t32_rows:
            f.write('    { 0x%x, 4, 0x%08x, 0x%02x, "%s" }, \\\n' % (address, word, it, text.replace('"', '\\"')))
        f.write('\n// { encoding, size, isT32 }\n')
        f.write('#define AARCH32_SIMD_INVALID \\\n')
        for w in invalid:
            f.write('    { 0x%08x, 4, false }, \\\n' % w)
            t = to_t32(w)
            if t is not None and (t >> 24) in (0xef, 0xff, 0xf9):
                pass
        f.write('\n')
    print('%d A32 rows, %d T32 rows, %d invalid words' % (len(a32_rows), len(t32_rows), len(invalid)))


if __name__ == '__main__':
    main()
