#!/usr/bin/env python3
"""Learn the encodings of the SVE class from objdump: sample the words, group
them by mnemonic and by the shape of the text (the operand forms) and print,
for each group, the bits which never change (0 / 1) and the ones which do
(x), the number of words, and an example.

    sve_learn.py [--count N] [--seed S] [--out FILE] [--mnemonic RE] [--tops 04,05]

The words are cached (--cache FILE) so the tool can be re-run for other
mnemonics without calling objdump again.
"""
import argparse
import collections
import os
import pickle
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib

TOPS = [0x04, 0x05, 0x24, 0x25, 0x44, 0x45, 0x64, 0x65, 0x84, 0x85, 0xa4, 0xa5,
        0xc4, 0xc5, 0xe4, 0xe5]


def shape(text):
    """The text with the numbers, the register numbers and the element sizes
    removed: the operand form."""
    t = re.sub(r'\.[bhsdq]\b', '.T', text)
    t = re.sub(r'\b(vl\d+|pow2|mul3|mul4|all)\b', 'PAT', t)
    t = re.sub(r'\b(pldl[123](keep|strm)|pstl[123](keep|strm))\b', 'PRF', t)
    t = re.sub(r'-?0x[0-9a-f]+', 'I', t)
    t = re.sub(r'\b-?\d+\.\d+', 'F', t)
    t = re.sub(r'\b([xwvqdshbzp]|pn)\d+', r'\1N', t)
    t = re.sub(r'(?<=\[)\w+N', 'R', t) if False else t
    t = re.sub(r'\b(sp|xzr|wzr|wsp)\b', 'GP', t)
    t = re.sub(r'\bxN\b', 'GP', t)
    t = re.sub(r'\bwN\b', 'GP', t)
    t = re.sub(r'#\d+', '#I', t)
    t = re.sub(r'\[\d+\]', '[I]', t)
    return t


def sample(count, seed, cache, tops):
    if cache and os.path.exists(cache):
        return pickle.load(open(cache, 'rb'))
    rng = armlib.seeded_rng(seed)
    result = []
    for top in tops:
        words = [(top << 24) | rng.getrandbits(24) for _ in range(count)]
        texts, _ = armlib.objdump_chunks(words)
        for w, t in zip(words, texts):
            if t != '<unknown>':
                result.append((w, t))
    if cache:
        pickle.dump(result, open(cache, 'wb'))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--count', type=int, default=150000, help='words per top byte')
    parser.add_argument('--seed', type=int, default=17)
    parser.add_argument('--cache')
    parser.add_argument('--mnemonic', help='regex of the mnemonics to show')
    parser.add_argument('--tops', default=','.join('%02x' % t for t in TOPS))
    parser.add_argument('--out')
    parser.add_argument('--min', type=int, default=1)
    args = parser.parse_args()
    tops = [int(t, 16) for t in args.tops.split(',')]
    data = sample(args.count, args.seed, args.cache, tops)
    wanted = re.compile(args.mnemonic) if args.mnemonic else None

    groups = {}
    for word, text in data:
        mnemonic = text.split(' ')[0]
        if wanted and not wanted.search(mnemonic):
            continue
        key = (mnemonic, shape(text))
        g = groups.get(key)
        if g is None:
            groups[key] = [word, word, 1, (word, text)]
        else:
            g[0] &= word
            g[1] |= word
            g[2] += 1
    out = open(args.out, 'w') if args.out else sys.stdout
    for (mnemonic, sh), (and_bits, or_bits, count, example) in sorted(groups.items()):
        if count < args.min:
            continue
        varying = and_bits ^ or_bits
        bits = ''
        for bit in range(31, -1, -1):
            if (varying >> bit) & 1:
                bits += 'x'
            else:
                bits += '1' if (and_bits >> bit) & 1 else '0'
            if bit % 4 == 0 and bit:
                bits += '_'
        out.write('%-10s %s  n=%-6d %08x %s\n' % (mnemonic, bits, count, example[0], example[1]))


if __name__ == '__main__':
    main()
