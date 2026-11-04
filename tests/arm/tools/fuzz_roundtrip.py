#!/usr/bin/env python3
"""The round trip of dismount (tests/arm/TEST_PLAN.md): random words are
disassembled, the text is assembled again and has to give the same word.

A word that comes back different is a failure, except when the assembled word
disassembles to the same text: the encodings with ignored fields (the "should
be zero" bits which LLVM ignores) have no canonical word, they are counted as
"same text" and not as failures.

    fuzz_roundtrip.py --arch a64 --count 200000 --seed 1
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib
import fuzz_decode


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--arch', default='a64', choices=['a64'])
    parser.add_argument('--count', type=int, default=20000)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--show', type=int, default=3)
    args = parser.parse_args()

    rows = armlib.entries(args.arch)
    words = fuzz_decode.generate(args.arch, args.count, args.seed, rows)
    words = [w for w in words if armlib.a64_class(w) == 'base']
    addresses = [armlib.BASE + 4 * i for i in range(len(words))]
    decoded = armlib.dis_words(args.arch, words, None, addresses)

    ok_words = [w for w, d in zip(words, decoded) if d[0] == 'OK']
    ok_texts = [d[1] for d in decoded if d[0] == 'OK']
    ok_addresses = [a for a, d in zip(addresses, decoded) if d[0] == 'OK']
    assembled = armlib.asm_lines(args.arch, ok_texts, None, ok_addresses)

    stats = collections.Counter()
    again = []        # (index) of the words which came back different
    groups = collections.defaultdict(list)
    for i, (word, text, address, (status, data)) in enumerate(
            zip(ok_words, ok_texts, ok_addresses, assembled)):
        if status != 'OK':
            groups[('ASM-ERR', armlib.mnemonic_of(text))].append((word, text, data))
            continue
        back = armlib.hex_bytes_to_word(data)
        if back == word:
            stats['same word'] += 1
        else:
            again.append((i, back))
    # A different word: does it disassemble to the same text?
    if again:
        back_words = [b for _, b in again]
        back_addresses = [ok_addresses[i] for i, _ in again]
        back_decoded = armlib.dis_words(args.arch, back_words, None, back_addresses)
        for (i, back), (status, text) in zip(again, back_decoded):
            if status == 'OK' and text == ok_texts[i]:
                stats['same text, other word'] += 1
            else:
                groups[('DIFF', armlib.mnemonic_of(ok_texts[i]))].append(
                    (ok_words[i], ok_texts[i], '%08x %s' % (back, text)))

    bugs = sum(len(v) for v in groups.values())
    stats['decoded'] = len(ok_words)
    for key in sorted(stats):
        print('%-28s %d' % (key, stats[key]))
    print('%-28s %d' % ('failures', bugs))
    for (kind, mnemonic), items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print('\n%s %s (%d)' % (kind, mnemonic, len(items)))
        for word, text, other in items[:args.show]:
            print('   %08x  %s' % (word, text))
            print('             -> %s' % other)
    return 1 if bugs else 0


if __name__ == '__main__':
    sys.exit(main())
