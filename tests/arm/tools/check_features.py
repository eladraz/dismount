#!/usr/bin/env python3
"""Check the features of the AArch32 tables against clang (tests/arm/TEST_PLAN.md):
every corpus line is assembled by clang for the triples of the architecture
versions (armv4t ... armv8-a, without any other feature) and by dismount with
the features of the same version (--mattr armvN): the two must accept the same
lines. A mismatch means that an entry has the wrong feature (or the version
lacks a feature in ArmFeatureSet::fromArchitecture).

    check_features.py --arch a32 [--file NAME[,NAME...]] [--show N]
"""
import argparse
import collections
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

# (name, the clang triple, the clang features on top of it, the --mattr of
# dismount). The architecture versions, and the floating point variants of
# them (-mfpu=vfpv3, no neon...)
VERSIONS = {
    'a32': [('armv4t', 'armv4t-none-eabi', [], 'none,armv4t'),
            ('armv5te', 'armv5te-none-eabi', [], 'none,armv5te'),
            ('armv6', 'armv6-none-eabi', [], 'none,armv6'),
            ('armv6t2', 'armv6t2-none-eabi', [], 'none,armv6t2'),
            ('armv7-a', 'armv7a-none-eabi', [], 'none,armv7-a'),
            ('armv7-a no neon', 'armv7a-none-eabi', ['-neon'], 'none,armv7-a,-neon'),
            ('armv8-a', 'armv8a-none-eabi', [], 'none,armv8-a'),
            ('armv8-a no crypto', 'armv8a-none-eabi', ['-aes', '-sha2'], 'none,armv8-a,-aes,-sha2'),
            ('armv8-a no crc', 'armv8a-none-eabi', ['-crc'], 'none,armv8-a,-crc')],
    't32': [('armv4t', 'thumbv4t-none-eabi', [], 'none,armv4t'),
            ('armv6', 'thumbv6-none-eabi', [], 'none,armv6'),
            ('armv6t2', 'thumbv6t2-none-eabi', [], 'none,armv6t2'),
            ('armv7-a', 'thumbv7a-none-eabi', [], 'none,armv7-a'),
            ('armv7-a no neon', 'thumbv7a-none-eabi', ['-neon'], 'none,armv7-a,-neon'),
            ('armv8-a', 'thumbv8a-none-eabi', [], 'none,armv8-a')],
}


# What clang accepts in spite of the architecture: the ARMv6 extend and saturate
# instructions are not gated by the architecture in the LLVM 21 assembler
QUIRKS = re.compile(r'^(sxt|uxt|ssat|usat)')


def read_lines(arch, names):
    lines = []
    for path in sorted(glob.glob(os.path.join(armlib.REPO, 'tests', 'arm', 'corpus', arch, '*.s'))):
        if names and os.path.basename(path)[:-2] not in names:
            continue
        for raw in open(path):
            raw = raw.rstrip('\n')
            if not raw.strip() or raw.startswith('//'):
                continue
            lines.append(re.sub(r'\s*// @feature:.*$', '', raw).strip())
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--arch', default='a32', choices=['a32', 't32'])
    parser.add_argument('--file')
    parser.add_argument('--show', type=int, default=4)
    args = parser.parse_args()
    lines = read_lines(args.arch, args.file.split(',') if args.file else None)
    status = 0
    for version, triple, extra, mattr in VERSIONS[args.arch]:
        clang = armlib.clang_assemble(lines, args.arch, extra, triple)
        ours = armlib.asm_lines(args.arch, lines, mattr,
                                [armlib.BASE + 4 * i for i in range(len(lines))])
        groups = collections.defaultdict(list)
        same = 0
        for line, (cs, cdata), (os_, odata) in zip(lines, clang, ours):
            clang_ok = cs == 'ok'
            ours_ok = os_ == 'OK'
            if clang_ok == ours_ok or (clang_ok and QUIRKS.match(line)):
                same += 1
            elif clang_ok:
                groups['clang accepts, dismount: %s' % (odata if isinstance(odata, str) else 'error')[:60]].append(line)
            else:
                groups['dismount accepts, clang: %s' % cdata[:60]].append(line)
        print('%-8s %d lines, %d agree, %d differ' % (version, len(lines), same, len(lines) - same))
        for key, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            print('    %4d  %s' % (len(items), key))
            for item in items[:args.show]:
                print('            %s' % item)
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main())
