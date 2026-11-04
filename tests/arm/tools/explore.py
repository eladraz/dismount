#!/usr/bin/env python3
"""Show how objdump decodes a word and its 32 one-bit neighbours: the quickest
way to learn the fields of an encoding.

    explore.py 0x7c23023b [more words]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib


def main():
    summary = '--summary' in sys.argv
    for arg in sys.argv[1:]:
        if arg.startswith('--'):
            continue
        word = int(arg, 16)
        words = [word] + [word ^ (1 << bit) for bit in range(32)]
        texts = armlib.objdump_words(words)
        print('%08x  %s' % (word, texts[0]))
        if summary:
            # 'o' where the bit leaves the mnemonic alone, '.' where it changes
            # it, per bit from 31 to 0
            mnemonic = texts[0].split(' ')[0]
            line = ''
            for bit in range(31, -1, -1):
                line += 'o' if texts[1 + bit].split(' ')[0] == mnemonic else '.'
                if bit % 4 == 0:
                    line += ' '
            print('   ' + line)
            continue
        for bit in range(31, -1, -1):
            print('   bit %2d -> %08x  %s' % (bit, word ^ (1 << bit), texts[1 + bit]))


if __name__ == '__main__':
    main()
