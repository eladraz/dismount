#!/usr/bin/env python3
"""Generate tests/riscv/corpus/{rv32,rv64}/*.s: the lines which run_compat.py
compares with llvm-mc and llvm-objdump.

Every instruction of tools/riscvinsns.py gets a line per value of each of its
operands (the other operands at their base value): all the registers of the
class, the edge values of the immediates and the values just outside of them
(which LLVM rejects: dismount has to reject them too), every rounding mode,
every fence set, every CSR name. The multi instruction pseudo instructions
(li of many values, la, call, tail, the symbol loads and stores) are in
pseudo.s. A line is "<text>  # @feature: <llvm names>".

  python3 tests/riscv/tools/gen_corpus.py
"""
import itertools
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rvlib
import riscvdsl
import riscvinsns

CORPUS = os.path.join(rvlib.REPO, 'tests', 'riscv', 'corpus')

XREGS = ['zero', 'ra', 'sp', 'gp', 'tp', 't0', 't1', 't2', 's0', 's1', 'a0', 'a1', 'a2', 'a3', 'a4',
         'a5', 'a6', 'a7', 's2', 's3', 's4', 's5', 's6', 's7', 's8', 's9', 's10', 's11', 't3', 't4',
         't5', 't6']
XREGS_EXTRA = ['x0', 'x1', 'x5', 'x31', 'fp']
FREGS = ['ft0', 'ft1', 'ft2', 'ft3', 'ft4', 'ft5', 'ft6', 'ft7', 'fs0', 'fs1', 'fa0', 'fa1', 'fa2',
         'fa3', 'fa4', 'fa5', 'fa6', 'fa7', 'fs2', 'fs3', 'fs4', 'fs5', 'fs6', 'fs7', 'fs8', 'fs9',
         'fs10', 'fs11', 'ft8', 'ft9', 'ft10', 'ft11']
FREGS_EXTRA = ['f0', 'f31']
CREGS = ['s0', 's1', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5']
FLI = ['-1.0', 'min', '1.52587890625e-05', '3.0517578125e-05', '0.00390625', '0.0078125', '0.0625',
       '0.125', '0.25', '0.3125', '0.375', '0.4375', '0.5', '0.625', '0.75', '0.875', '1.0', '1.25',
       '1.5', '1.75', '2.0', '2.5', '3.0', '4.0', '8.0', '16.0', '128.0', '256.0', '32768.0',
       '65536.0', 'inf', 'nan']

# scheme -> (signed, bits, align, special values)
SCHEMES = {
    'I': (True, 12, 1), 'S': (True, 12, 1), 'B': (True, 13, 2), 'U': (False, 20, 1),
    'J': (True, 21, 2), 'CSR_UIMM5': (False, 5, 1), 'PREFETCH': (True, 12, 32),
    'AES_BS': (False, 2, 1), 'AES_RNUM': (False, 4, 1), 'LPAD': (False, 20, 1),
    'V_SIMM5': (True, 5, 1), 'V_UIMM5': (False, 5, 1),
    'CI': (True, 6, 1), 'CI_LUI': (False, 20, 1), 'CI_ADDI16SP': (True, 10, 16),
    'CIW': (False, 10, 4), 'CI_LWSP': (False, 8, 4), 'CI_LDSP': (False, 9, 8),
    'CSS_SWSP': (False, 8, 4), 'CSS_SDSP': (False, 9, 8), 'CL_W': (False, 7, 4),
    'CL_D': (False, 8, 8), 'CLB': (False, 2, 1), 'CLH': (False, 2, 2), 'CB': (True, 9, 2),
    'CJ': (True, 12, 2),
}


def immediate_values(scheme):
    if scheme == 'CI_LUI':
        return [1, 2, 31, 0xfffe0, 0xfffe1, 0xfffff, 0, 32, 0x100000, 0xfffdf]
    if scheme == 'AES_RNUM':
        return list(range(0, 12))
    signed, bits, align = SCHEMES[scheme]
    low = -(1 << (bits - 1)) if signed else 0
    high = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1
    values = {low, low + align, -align, 0, align, 2 * align, high - (high % align),
              high - (high % align) - align, low - align, high + align, 1, align + 1, 5 * align,
              (high // 3) - ((high // 3) % align), (low // 3) - ((low // 3) % align)}
    for k in (3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 127, 128, 255, 256, 511, 512, 1000, 1024):
        values.add(k * align)
        values.add(-k * align)
    if scheme == 'U' or scheme == 'LPAD':
        values |= {0x7ff, 0x800, 0x12345, 0x80000, 0x7ffff}
    return sorted(values)


def token_values(name, token, bits):
    """The values (texts) of a token."""
    cpp = token.cpp
    if name.startswith('imm_'):
        scheme = name[4:].upper()
        if scheme.endswith('_NZ'):
            scheme = scheme[:-3]
        return [str(v) for v in immediate_values(scheme)]
    if name.startswith('mem_'):
        scheme = None
        for s in SCHEMES:
            if 'RISCV_IMM_%s)' % s in cpp:
                scheme = s
        compressed = 'RISCV_MEMORY_COMPRESSED' in cpp
        sp = 'RISCV_MEMORY_SP' in cpp
        regs = CREGS if compressed else XREGS
        base_reg = token.base[token.base.index('(') + 1:-1]
        offsets = [str(v) for v in immediate_values(scheme)]
        values = ['%s(%s)' % (o, base_reg) for o in offsets]
        if not sp:
            values += ['%s(%s)' % (token.base.split('(')[0], r) for r in regs]
        return values
    if name == 'mem0':
        return ['(%s)' % r for r in XREGS] + ['0(a1)']
    if name.startswith('br_'):
        scheme = {'br_b': 'B', 'br_j': 'J', 'br_cb': 'CB', 'br_cj': 'CJ'}[name]
        return [str(v) for v in immediate_values(scheme)]
    if name in ('shamt6', 'cshamt', 'cshamt_nz'):
        return ['0', '1', '2', '16', '31', '32', '33', '63', '64']
    if name == 'shamt5':
        return ['0', '1', '16', '31', '32']
    if name == 'fli':
        return FLI
    if name == 'rm' or name == 'rm_rne':
        return ['rne', 'rtz', 'rdn', 'rup', 'rmm', 'dyn']
    if name == 'rm_fixed':
        return ['rtz', 'rne', 'dyn']
    if name == 'fence_p' or name == 'fence_s':
        sets = []
        for n in range(1, 16):
            sets.append(''.join(c for i, c in enumerate('iorw') if n & (8 >> i)))
        return sets + ['0']
    if name == 'aqrl':
        return ['', '.aq', '.rl', '.aqrl']
    if name.startswith('csr'):
        return csr_values(bits)
    if name in ('rdp', 'rs1p', 'rs2p'):
        return CREGS
    if name in ('frdp', 'frs2p'):
        return ['fs0', 'fs1', 'fa0', 'fa1', 'fa2', 'fa3', 'fa4', 'fa5']
    if name.startswith('frd') or name.startswith('frs') or name.startswith('cfr'):
        return FREGS + FREGS_EXTRA
    if name.startswith('rd') or name.startswith('rs') or name.startswith('crd') or name.startswith('crs'):
        return XREGS + XREGS_EXTRA
    return [token.base]


_csr_cache = {}


def csr_values(bits):
    if bits not in _csr_cache:
        import gen_csr_table
        names = gen_csr_table.names(bits)
        _csr_cache[bits] = [n for n in names if n is not None] + ['0', '1', '2047', '2048', '4095', '4096', '3072']
    return _csr_cache[bits]


def render(entry, values_by_index, aqrl='.aq'):
    parts = []
    suffix = ''
    for token, value in zip(entry.tokens(), values_by_index):
        if token.part:
            suffix += value
        elif not token.hidden:
            parts.append(value)
    if entry.probe:
        text = entry.probe.format(*parts)
    else:
        text = entry.mn + suffix + ((' ' + ', '.join(parts)) if parts else '')
    return text


def entry_lines(entry, bits, names):
    tokens = entry.tokens()
    base = [t.base for t in tokens]
    # the aq / rl part is part of the mnemonic: base ''
    for i, t in enumerate(tokens):
        if t.part:
            base[i] = ''
    lines = [render(entry, base)]
    for i, (token, name) in enumerate(zip(tokens, names)):
        if token.hidden or isinstance(name, riscvdsl.Token):
            continue
        for value in token_values(name, token, bits):
            current = list(base)
            current[i] = value
            lines.append(render(entry, current))
    # a few combinations: the same register everywhere, the zero register
    regs = [i for i, t in enumerate(tokens) if t.cpp.startswith('S(RISCV_KIND_GPR,') and t.fields]
    for reg in ('a0', 'zero', 'sp'):
        if len(regs) >= 2:
            current = list(base)
            for i in regs:
                current[i] = reg
            lines.append(render(entry, current))
    return lines


def feature_names(entry, llvm_names):
    names = []
    for feat in (entry.feat, entry.feat2):
        if feat and feat not in ('FEAT_I', 'FEAT_PRIVILEGED'):
            names.append(llvm_names[feat])
    return names


def load_feature_names():
    import re
    header = open(os.path.join(rvlib.REPO, 'Include', 'dismount', 'proc', 'riscv', 'RiscvFeatures.h')).read()
    order = re.findall(r'^\s+(FEAT_[A-Z0-9_]+),?', header, re.M)
    out = rvlib.run([rvlib.CLI, 'features', '--arch', 'rv64'], '')
    names = {}
    for line in out.stdout.splitlines():
        index, name = line.split()[:2]
        names[order[int(index)]] = name
    return names


def collect():
    entries = []
    current = ['']

    def table(name):
        current[0] = name

    def ins(mn, ops, feat, flags=(), flow=None, feat2=None, rv=None, probe=None, cond=None, width=32):
        e = riscvdsl.Entry(mn, ops, 'FEAT_' + feat, list(flags), flow, ('FEAT_' + feat2) if feat2 else None,
                           rv, probe, cond, width)
        entries.append((current[0], e))

    riscvinsns.define(ins, table)
    return entries


def pseudo_lines(bits):
    rng = random.Random(7)
    values = set()
    for shift in range(0, 64):
        for delta in (-2, -1, 0, 1, 2):
            values.add((1 << shift) + delta)
            values.add(-(1 << shift) + delta)
            values.add(((1 << shift) - 1) << (64 - shift) if shift else 0)
    for _ in range(400):
        kind = rng.randrange(6)
        v = rng.getrandbits(64)
        if kind == 1:
            v &= 0xffffffff
        elif kind == 2:
            v = (v >> rng.randrange(0, 56)) << rng.randrange(0, 40)
        elif kind == 3:
            v = (rng.getrandbits(20) << 12)
        elif kind == 4:
            v = rng.getrandbits(32) << 32
        elif kind == 5:
            v = (1 << rng.randrange(64)) | (1 << rng.randrange(64))
        values.add(v)
    lines = []
    for v in sorted(values, key=lambda x: (x & 0xffffffffffffffff)):
        v &= 0xffffffffffffffff
        if bits == 64:
            lines.append('li a0, %d' % (v if v < (1 << 63) else v - (1 << 64)))
            if v & 7 == 0:
                lines.append('li t1, 0x%x' % v)
        else:
            w = v & 0xffffffff
            lines.append('li a0, %d' % (w if w < (1 << 31) else w - (1 << 32)))
            lines.append('li s1, 0x%x' % w)
    lines += ['li %s, 0x12345678' % r for r in XREGS]
    for r in XREGS:
        lines += ['la %s, symbol' % r, 'lla %s, symbol' % r]
    lines += ['call symbol', 'call ra, symbol', 'tail symbol']
    for mn in ('lb', 'lh', 'lw', 'lbu', 'lhu') + (('lwu', 'ld') if bits == 64 else ()):
        lines += ['%s a0, symbol' % mn, '%s s2, symbol' % mn]
    for mn in ('sb', 'sh', 'sw') + (('sd',) if bits == 64 else ()):
        lines += ['%s a0, symbol, t0' % mn, '%s s2, symbol, a1' % mn]
    for mn in ('flw', 'fld', 'flh'):
        lines += ['%s fa0, symbol, t0' % mn, '%s ft1, symbol, a1' % mn]
    for mn in ('fsw', 'fsd', 'fsh'):
        lines += ['%s fa0, symbol, t0' % mn, '%s ft1, symbol, a1' % mn]
    # the pseudo instructions which are one instruction (the table has them too)
    lines += ['nop', 'ret', 'j symbol', 'jal symbol', 'jr a0', 'jalr a0']
    for mn in ('zext.h', 'sext.b', 'sext.h') + (('zext.w',) if bits == 64 else ()):
        lines += ['%s a0, a1' % mn, '%s zero, s1' % mn]
    return lines


def main():
    llvm_names = load_feature_names()
    entries = collect()
    for bits in (32, 64):
        directory = os.path.join(CORPUS, 'rv%d' % bits)
        os.makedirs(directory, exist_ok=True)
        for name in os.listdir(directory):
            os.unlink(os.path.join(directory, name))
        files = {}
        for table, entry in entries:
            if entry.rv not in (None, bits):
                continue
            features = feature_names(entry, llvm_names)
            if table == 'privileged':
                file_name = 'privileged'
            elif features:
                file_name = features[0]
            else:
                file_name = 'base'
            tokens = entry.ops
            lines = entry_lines(entry, bits, tokens)
            bucket = files.setdefault(file_name, [])
            for line in lines:
                bucket.append((line, features))
        for file_name, lines in sorted(files.items()):
            seen = set()
            with open(os.path.join(directory, file_name + '.s'), 'w') as f:
                f.write('# GENERATED by tests/riscv/tools/gen_corpus.py: do not edit\n')
                for line, features in lines:
                    key = (line, tuple(features))
                    if key in seen:
                        continue
                    seen.add(key)
                    f.write('%s%s\n' % (line, ('  # @feature: ' + ' '.join(features)) if features else ''))
        with open(os.path.join(directory, 'pseudo.s'), 'w') as f:
            f.write('# GENERATED by tests/riscv/tools/gen_corpus.py: do not edit\n')
            for line in pseudo_lines(bits):
                f.write(line + '\n')
        total = sum(len(open(os.path.join(directory, n)).read().splitlines()) for n in os.listdir(directory))
        print('rv%d: %d files, %d lines' % (bits, len(os.listdir(directory)), total))


if __name__ == '__main__':
    main()
