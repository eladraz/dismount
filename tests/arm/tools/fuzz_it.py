#!/usr/bin/env python3
"""The it blocks of T32 (tests/arm/TEST_PLAN.md): random it instructions
followed by random instructions which objdump decodes, so the conditions, the
"s" of the 16 bit instructions and the instructions that are not allowed in
the block are compared with dismount. A sequence is the it, the instructions of
its block and one more after it.

    fuzz_it.py --count 20000 --seed 1
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib
import fuzz_decode


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--count', type=int, default=20000)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--show', type=int, default=5)
    args = parser.parse_args()

    rng = armlib.seeded_rng(args.seed)
    rows = armlib.entries('t32')
    # The instructions that objdump decodes (outside an it block)
    pool = fuzz_decode.generate_t32(max(args.count * 2, 4000), args.seed + 1000, rows)
    texts, _ = armlib.objdump_unit_chunks(pool)
    # (the floating point and Advanced SIMD are the work of another table: they
    # are left out until it is complete)
    valid = [u for u, t in zip(pool, texts) if t != '<unknown>' and not armlib.t32_is_it(u) and
             not re.match(r'^(v|fld|fst|aes|sha)', t) and not t.startswith('<')]

    sequences = []
    for _ in range(args.count):
        firstcond = rng.randrange(16)
        mask = rng.randrange(1, 16)
        count = 4 - ((mask & -mask).bit_length() - 1)
        seq = [(2, 0xbf00 | (firstcond << 4) | mask)]
        seq += [rng.choice(valid) for _ in range(count + 1)]
        sequences.append(seq)

    # (the addresses restart in each chunk of the listing)
    theirs = []
    ours = []
    for start in range(0, len(sequences), 5000):
        part = sequences[start:start + 5000]
        theirs.extend(armlib.objdump_units([u for seq in part for u in seq])[0])
        ours.extend(armlib.dis_sequences(part))

    stats = collections.Counter()
    groups = collections.defaultdict(list)
    position = 0
    for seq, mine in zip(sequences, ours):
        want = theirs[position:position + len(seq)]
        position += len(seq)
        if mine is not None and mine == want:
            stats['same text'] += 1
            continue
        stats['different'] += 1
        mnemonic = armlib.mnemonic_of(want[0])
        groups[mnemonic].append((seq, want, mine))
    for key in sorted(stats):
        print('%-20s %d' % (key, stats[key]))
    for mnemonic, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print('\n%s (%d)' % (mnemonic, len(items)))
        for seq, want, mine in items[:args.show]:
            print('   units  %s' % ' ; '.join(armlib.t32_hex(u) for u in seq))
            print('   objdump  %s' % ' ; '.join(want))
            print('   dismount %s' % (' ; '.join(mine) if mine else 'INVALID'))
    return 1 if stats['different'] else 0


if __name__ == '__main__':
    sys.exit(main())
