#!/usr/bin/env python3
"""Check which entries of the AArch32 encoding tables (A32, T16, T32, VFP / Advanced SIMD) have a
semantic table entry, and compare it with the support marks of ArmFeatures.cpp.

The semantic tables are the ARM32_ENTRY lines of Source/dismount/proc/arm/ArmSemanticTable.cpp
and of the ArmSemanticTable*.inc files (the semantic search ignores the ".w" of the wide T32
encodings). For every AArch32 feature this prints the number of encodings and how many of them are
lifted, the mark the feature should have (SUPPORT_LIFT when all are lifted, SUPPORT_PARTIAL_LIFT
when some are, none when no encoding is) and the mark it has.

    check_a32_semantic_coverage.py [--list-unlifted] [--strict]

--strict exits 1 when a feature is marked SUPPORT_LIFT with an encoding which is not lifted, or
marked SUPPORT_PARTIAL_LIFT / SUPPORT_LIFT with no lifted encoding, or has no mark with all its
encodings lifted.
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

ARM = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm')
SUPPORT_LIFT = 0x04
SUPPORT_PARTIAL_LIFT = 0x08
SUPPORT_LIFT_VERIFIED = 0x10


def lifted_names():
    names = set()
    sources = [os.path.join(ARM, 'ArmSemanticTable.cpp')] + sorted(glob.glob(os.path.join(ARM, 'ArmSemanticTable*.inc')))
    for path in sources:
        for match in re.finditer(r'ARM32_ENTRY(?:_DATA)?\("([^"]+)"', open(path).read()):
            names.add(match.group(1))
    return names


def base(name):
    return name[:-2] if name.endswith('.w') else name


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--list-unlifted', action='store_true')
    parser.add_argument('--strict', action='store_true')
    args = parser.parse_args()
    names = lifted_names()
    stripped = set(base(n) for n in names)
    total, lifted, unlifted = {}, {}, {}
    for arch in ('a32', 't32'):
        for mask, value, feature, flags, size, mnemonic in armlib.entries(arch):
            total[feature] = total.get(feature, 0) + 1
            if base(mnemonic) in stripped:
                lifted[feature] = lifted.get(feature, 0) + 1
            else:
                unlifted.setdefault(feature, set()).add(base(mnemonic))
    support = {}
    for arch in ('a32', 't32'):
        for index, name, isa, bits, entries in armlib.features(arch):
            support[index] = (name, bits)
    problems = []
    print('%-4s %-16s %8s %8s  %-10s %-10s' % ('id', 'feature', 'entries', 'lifted', 'should be', 'is'))
    for feature in sorted(total):
        name, bits = support.get(feature, ('?', 0))
        count, done = total[feature], lifted.get(feature, 0)
        should = 'LIFT' if done == count else ('PARTIAL' if done else '-')
        is_ = 'LIFT' if bits & SUPPORT_LIFT else ('PARTIAL' if bits & SUPPORT_PARTIAL_LIFT else '-')
        print('%-4d %-16s %8d %8d  %-10s %-10s%s' % (feature, name, count, done, should, is_,
                                                     '' if should == is_ else '  <-'))
        if is_ == 'LIFT' and should != 'LIFT':
            problems.append('%s is marked SUPPORT_LIFT and %d of %d encodings are not lifted' % (name, count - done, count))
        elif is_ == 'PARTIAL' and not done:
            problems.append('%s is marked SUPPORT_PARTIAL_LIFT and no encoding is lifted' % name)
        elif is_ == '-' and done:
            problems.append('%s has %d lifted encodings and no lift mark' % (name, done))
        if args.list_unlifted and unlifted.get(feature):
            print('       not lifted: %s' % ' '.join(sorted(unlifted[feature])))
    for problem in problems:
        print('PROBLEM: ' + problem)
    return 1 if (problems and args.strict) else 0


if __name__ == '__main__':
    sys.exit(main())
