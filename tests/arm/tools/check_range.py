#!/usr/bin/env python3
"""Compare dismount with objdump over a range of words, exhaustively (or every
Nth word). The ranges of the sparse spaces (the system instructions are
0xd5000000-0xd53fffff) are small enough to be checked completely.

    check_range.py 0xd5000000 0xd5400000 [--step N] [--show N]
    check_range.py --top 19,59,7c [--step N]      # whole top bytes
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzz_decode


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('start', nargs='?', type=lambda x: int(x, 0))
    parser.add_argument('end', nargs='?', type=lambda x: int(x, 0))
    parser.add_argument('--top', help='comma separated top bytes (hex) to check')
    parser.add_argument('--step', type=int, default=1)
    parser.add_argument('--show', type=int, default=3)
    parser.add_argument('--arch', default='a64')
    args = parser.parse_args()
    ranges = []
    if args.top:
        for top in args.top.split(','):
            ranges.append((int(top, 16) << 24, (int(top, 16) + 1) << 24))
    else:
        ranges.append((args.start, args.end))
    code = 0
    chunk = 1 << 20
    for start, end in ranges:
        words = list(range(start, end, args.step))
        # Chunks of 1M words keep the memory of the report small
        for first in range(0, len(words), chunk):
            code |= fuzz_decode.compare_and_report(args.arch, words[first:first + chunk], args.show)
    return code


if __name__ == '__main__':
    sys.exit(main())
