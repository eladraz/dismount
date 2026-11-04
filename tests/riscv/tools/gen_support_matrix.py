#!/usr/bin/env python3
"""Generate tests/riscv/SUPPORT.md: per feature the RVA23 status, the number of
encodings, what dismount does with them (the marks of RiscvFeatures::getSupport)
and the corpus lines which pass (from the reports of run_compat.py).

  python3 tests/riscv/tools/gen_support_matrix.py [--reports out/compat-riscv] [--semantics DIR...]

Run tests/riscv/run_compat.py first (the reports are OUT-rv32 / OUT-rv64). The semantics columns come from the
reports of tests/riscv/semantics/run_semantics.py (out/semantics_riscv/<arch>_<oracle>/report.json: rv32 and
rv64, QEMU and Spike): per feature the lines whose first mnemonic belongs to the feature, those which PASS or
are an ORACLE_BUG over those which ran (not SKIP). The script exits with 1 if a feature marked SUPPORT_LIFT_VERIFIED
has a FAIL or UNLIFTED line.
"""
import argparse
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rvlib

STATUS = {0: 'none', 1: 'RVA23U64 mandatory', 2: 'RVA23U64 optional', 3: 'RVA23S64 mandatory',
          4: 'RVA23S64 optional'}
SUPPORT = {'disassemble': 0x01, 'assemble': 0x02, 'lift': 0x04, 'partial lift': 0x08,
           'lift verified': 0x10, 'encoding verified': 0x20, 'no instructions': 0x40}
RV32_ONLY = 0x8
RV64_ONLY = 0x10
CHECKS = ('asm', 'asmc', 'dis', 'disna', 'disnum', 'rt', 'feat')


def features(bits):
    rows = {}
    for line in rvlib.run([rvlib.CLI, 'features', '--arch', 'rv%d' % bits], '').stdout.splitlines():
        index, name, status, support, count = line.split()
        rows[int(index)] = (name, int(status), int(support, 16))
    return rows


def entry_counts():
    counts = {32: collections.Counter(), 64: collections.Counter()}
    for line in rvlib.run([rvlib.CLI, 'entries', '--arch', 'rv64'], '').stdout.splitlines():
        mask, value, feature, flags, size, mnemonic = line.split()
        flags = int(flags, 16)
        for bits, only in ((32, RV64_ONLY), (64, RV32_ONLY)):
            if not flags & only and not flags & 0x4 and not flags & 0x2:
                counts[bits][int(feature)] += 1
    return counts


def corpus(report_dir, bits):
    path = os.path.join(report_dir + '-rv%d' % bits, 'report.json')
    result = collections.defaultdict(lambda: [0, 0])
    if not os.path.exists(path):
        return result
    for r in json.load(open(path))['lines']:
        features = r['features']
        key = features[0] if features else {'privileged.s': 'privileged', 'pseudo.s': 'pseudo'}.get(r['file'], 'base')
        result[key][0] += 1
        if not any(r[c]['status'] == 'FAIL' for c in CHECKS):
            result[key][1] += 1
    return result


IMPLIED_ONLY = ('v', 'zve64f', 'zve64d', 'zvfh', 'zvfhmin', 'zvknhb')


def mnemonic_features():
    """{mnemonic: the feature index of its first entry} of the encoding tables (the aliases too)."""
    result = {}
    for line in rvlib.run([rvlib.CLI, 'entries', '--arch', 'rv64'], '').stdout.splitlines():
        mask, value, feature, flags, size, mnemonic = line.split()
        result.setdefault(mnemonic, int(feature))
    for line in rvlib.run([rvlib.CLI, 'entries', '--arch', 'rv32'], '').stdout.splitlines():
        mask, value, feature, flags, size, mnemonic = line.split()
        result.setdefault(mnemonic, int(feature))
    return result


def semantics(directories, by_mnemonic):
    """{(bits, feature index): Counter of PASS / ORACLE_BUG / FAIL / UNLIFTED / SKIP...} from the semantics
    reports: a line counts once per architecture, as the worst result of its oracles."""
    order = ['UNLIFTED', 'FAIL', 'DIS_ERR', 'ASM_ERR', 'ORACLE_ERR', 'ORACLE_BUG', 'PASS', 'SKIP']
    lines = {}
    for directory in directories:
        path = os.path.join(directory, 'report.json')
        if not os.path.exists(path):
            continue
        report = json.load(open(path))
        bits = 64 if report['arch'] == 'rv64' else 32
        for r in report['lines']:
            key = (bits, r['file'], r['line'], r.get('mode'))
            status = r['status']
            if key not in lines or order.index(status) < order.index(lines[key][0]):
                lines[key] = (status, r['mnemonic'])
    result = collections.defaultdict(collections.Counter)
    for (bits, file, line, mode), (status, mnemonic) in lines.items():
        base = mnemonic
        for suffix in ('.aqrl', '.aq', '.rl'):
            if base.endswith(suffix):
                base = base[:-len(suffix)]
        feature = by_mnemonic.get(base, by_mnemonic.get(mnemonic))
        if feature is None:
            feature = -1
        result[(bits, feature)][status] += 1
    return result


def marks(support, name=''):
    if support & 0x40:
        if name in IMPLIED_ONLY:
            text = 'no encodings of its own (the instructions of the features it implies), verified'
            if support & 0x08:
                text += '; partial lift (the integer instructions of Zve32x / Zve64x are lifted, the floating point ones of Zve32f / Zve64d are not)'
            return text
        return 'no instructions' + (' (CSR names)' if support & 0x2 else '')
    return ', '.join(n for n, b in SUPPORT.items() if support & b)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--reports', default=os.path.join(rvlib.REPO, 'out', 'compat-riscv'))
    parser.add_argument('--semantics', nargs='*', default=[
        os.path.join(rvlib.REPO, 'out', 'semantics_riscv', name)
        for name in ('rv64_qemu', 'rv64_spike', 'rv32_qemu', 'rv32_spike')])
    args = parser.parse_args()
    rows = features(64)
    counts = entry_counts()
    corp = {32: corpus(args.reports, 32), 64: corpus(args.reports, 64)}
    sem = semantics(args.semantics, mnemonic_features())
    problems = []
    out = []
    out.append('# RISC-V support matrix\n')
    out.append('Generated by `tests/riscv/tools/gen_support_matrix.py` from `RiscvFeatures::getSupport()`, '
               'the encoding tables and the reports of `tests/riscv/run_compat.py`; do not edit.\n')
    out.append('The names are the LLVM 23 target features (`llvm-mc -mattr`). The RVA23 column is the status in the '
               'ratified RVA23 profile specification:\n')
    mandatory_u = [rows[i][0] for i in sorted(rows) if rows[i][1] == 1 and rows[i][0] != '-']
    optional_u = [rows[i][0] for i in sorted(rows) if rows[i][1] == 2]
    mandatory_s = [rows[i][0] for i in sorted(rows) if rows[i][1] == 3 and rows[i][0] != '-']
    optional_s = [rows[i][0] for i in sorted(rows) if rows[i][1] == 4]
    out.append('* RVA23U64 mandatory (the base I, M, A, F, D, C and B are their parts): %s' % ', '.join(mandatory_u))
    out.append('* RVA23U64 optional: %s (and Zvkng, Zvksg: their parts Zvkned, Zvknhb, Zvkg, Zvksed, Zvksh)' % ', '.join(optional_u))
    out.append('* RVA23S64 mandatory, more than RVA23U64 (Sha is H with Ssstateen, Shcounterenw, Shvstvala, Shtvala, '
               'Shvstvecd, Shvsatpa, Shgatpa): %s' % ', '.join(mandatory_s))
    out.append('* RVA23S64 optional: the RVA23U64 optional ones and %s (Sv48, Sv57 and Svadu have no feature here)\n'
               % ', '.join(optional_s))
    out.append('The marks: *disassemble* and *assemble* are the LLVM syntax decoded and encoded; *lift*: '
               'every instruction of the feature has semantics in the lifter (`RiscvSemantics`); *partial lift*: '
               'some do; *lift verified*: the instructions were compared with QEMU and Spike by the semantics tests '
               '(`tests/riscv/semantics`) -- the privileged instructions, which user mode can not run, are lifted to '
               'the helper `RISCV_HELPER_PRIVILEGED` and are not verified; *encoding verified*: the decoder and the assembler are compared with '
               'llvm-objdump and llvm-mc (the corpus, `fuzz_decode.py`, `fuzz_roundtrip.py`). The vector features '
               '(Zve*, V, Zvf*, Zvk*, Zvb*) are in `RiscvEncodingTableVector*.cpp`: LLVM gates each instruction by '
               'the weakest of them (the integer ones by Zve32x, the floating point ones by Zve32f, the 64 bit loads '
               'and Zvbc by Zve64x), so V, Zve64f, Zve64d, Zvfh, Zvfhmin and Zvknhb have no encodings of their own. The '
               'corpus columns are the lines of `tests/riscv/corpus` whose first feature is the row, with no FAIL in '
               'any check (lines/passing).\n')
    out.append('The semantics columns are the lines of `tests/riscv/semantics/corpus` run by '
               '`run_semantics.py` (QEMU 11 and Spike, 16 states per line at least) whose first mnemonic belongs '
               'to the feature, PASS or ORACLE_BUG over the ones which ran (not SKIP); the line counts as passing '
               'when both oracles agree with the lifter.\n')
    out.append('| feature | RVA23 | encodings RV32 / RV64 | marks | corpus RV32 | corpus RV64 | semantics RV32 | '
               'semantics RV64 |')
    out.append('|---|---|---|---|---|---|---|---|')
    totals = [0, 0, 0, 0, 0, 0]
    for index in sorted(rows):
        name, status, support = rows[index]
        shown = name if name != '-' else ('(base)' if index == 0 else '(privileged)')
        key = name if name != '-' else ('base' if index == 0 else 'privileged')
        c32 = corp[32].get(key, [0, 0])
        c64 = corp[64].get(key, [0, 0])
        texts = []
        for bits in (32, 64):
            c = sem.get((bits, index), collections.Counter())
            ran = sum(v for k, v in c.items() if k != 'SKIP')
            good = c['PASS'] + c['ORACLE_BUG']
            texts.append('%d/%d' % (good, ran) if ran else ('-' if not c['SKIP'] else 'skipped %d' % c['SKIP']))
            if (support & 0x10) and (c['FAIL'] or c['UNLIFTED'] or c['DIS_ERR']):
                problems.append('%s is marked SUPPORT_LIFT_VERIFIED and has %d FAIL / %d UNLIFTED lines in RV%d' % (
                    shown, c['FAIL'], c['UNLIFTED'], bits))
        out.append('| %s | %s | %d / %d | %s | %d/%d | %d/%d | %s | %s |' % (
            shown, STATUS[status], counts[32][index], counts[64][index], marks(support, name),
            c32[0], c32[1], c64[0], c64[1], texts[0], texts[1]))
        totals[0] += counts[32][index]
        totals[1] += counts[64][index]
        totals[2] += c32[0]
        totals[3] += c32[1]
        totals[4] += c64[0]
        totals[5] += c64[1]
    p32 = corp[32].get('pseudo', [0, 0])
    p64 = corp[64].get('pseudo', [0, 0])
    out.append('| **total** | | %d / %d | | %d/%d | %d/%d | | |' % tuple(totals[:2] + totals[2:]))
    out.append('')
    out.append('The pseudo instructions (`pseudo.s`: li of many values, la, call, tail, the symbol loads and '
               'stores, zext/sext without their extension) are not attributed to a feature: %d/%d lines pass in RV32 '
               'and %d/%d in RV64 (the li sequences of every bit manipulation extension are also compared by '
               '`RiscvPseudoRows.inc` of the unit test).' % (p32[1], p32[0], p64[1], p64[0]))
    open(os.path.join(rvlib.REPO, 'tests', 'riscv', 'SUPPORT.md'), 'w').write('\n'.join(out) + '\n')
    print('SUPPORT.md written')
    for problem in problems:
        print('INCONSISTENT: ' + problem)
    if problems:
        sys.exit(1)


if __name__ == '__main__':
    main()
