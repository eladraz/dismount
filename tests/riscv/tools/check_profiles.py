#!/usr/bin/env python3
"""Check the profiles and the implications of RiscvFeatureSet against clang:
the -target-feature list of `clang --target=riscv64 -march=<profile> -###`
(rva23u64, rva23s64, rva22u64, rva22s64, rva20u64, rvi20u64, rvi20u32) and
of the extensions with their implications ("-march=rv64i_zvkng", "rv64gc", ...)
against `dismount_cli set --mattr <the same>`, limited to the features
dismount knows. Exit status 1 on any difference.

  python3 tests/riscv/tools/check_profiles.py
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rvlib

PROFILES = ['rva23u64', 'rva23s64', 'rva22u64', 'rva22s64', 'rva20u64', 'rvi20u64', 'rvi20u32']
ISA = ['rv64gc', 'rv32gc', 'rv64imafdc_zba_zbb_zbs', 'rv64i_zvkng', 'rv64i_zvksg', 'rv64i_zvknc',
       'rv64imc_zcb_zcmop', 'rv32imafc_zce', 'rv64gv', 'rv64i_sha', 'rv64i_zk', 'rv64i_zkn',
       'rv64i_zks', 'rv64gc_zfh_zfa', 'rv64i_zfbfmin', 'rv64i_zvfbfwma', 'rv64i_zacas_zabha',
       'rv64ima_zicond_zimop_zcmop', 'rv64i_zbc', 'rv32i_zcf', 'rva23u64_zvkng_zfh',
       'rva23s64_zacas_zicfilp_zicfiss']


def clang_features(march, bits):
    target = 'riscv%d' % bits
    p = subprocess.run([rvlib.tool('clang'), '--target=' + target, '-march=' + march,
                        '-menable-experimental-extensions', '-###', '-c', '-x', 'c', '/dev/null'],
                       capture_output=True, text=True)
    if 'error:' in p.stderr:
        return None
    tokens = p.stderr.replace('"', '').split()
    return set(tokens[i + 1][1:] for i, t in enumerate(tokens)
               if t == '-target-feature' and tokens[i + 1].startswith('+'))


def known_names(bits):
    out = rvlib.run([rvlib.CLI, 'features', '--arch', 'rv%d' % bits], '')
    names = set()
    for line in out.stdout.splitlines():
        name = line.split()[1]
        if name != '-':
            names.add(name)
    return names


def main():
    failures = 0
    for march in PROFILES + ISA:
        bits = 32 if (march.startswith('rv32') or march == 'rvi20u32') else 64
        # (the experimental extensions need the version of the draft in -march)
        flags = march
        theirs = clang_features(flags, bits)
        if theirs is None and ('zicfilp' in march or 'zicfiss' in march):
            flags = march.replace('zicfilp', 'zicfilp1p0').replace('zicfiss', 'zicfiss1p0')
            theirs = clang_features(flags, bits)
        if theirs is None:
            print('SKIP %s: clang rejects it' % march)
            continue
        names = known_names(bits)
        # (the shorthands of clang which are not features: a, b, c, g, v, sha...)
        mine = set(rvlib.run([rvlib.CLI, 'set', '--arch', 'rv%d' % bits, '--mattr', march], '').stdout.split())
        if bits == 64:
            mine.discard('zcf')  # (inert in RV64: its entries are RV32 only)
        theirs_known = set(n for n in theirs if n in names or ('experimental-' + n) in names)
        theirs_known = set(('experimental-' + n) if ('experimental-' + n) in names else n for n in theirs_known)
        # dismount's closure may be larger for zcf (RV32 only) and the zvl lengths (not features)
        missing = sorted(theirs_known - mine)
        extra = sorted(mine - theirs_known)
        # clang lists the implied extensions of "g" as separate features; the ones
        # of dismount's shorthand "c" (zca, zcd, zcf) as well
        status = 'ok' if not missing and not extra else 'DIFF'
        print('%-34s %s' % (march, status))
        if missing or extra:
            failures += 1
            print('    only in clang:', ' '.join(missing))
            print('    only in dismount:', ' '.join(extra))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
