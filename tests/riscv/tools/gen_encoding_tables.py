#!/usr/bin/env python3
"""Generate the RISC-V encoding tables of Source/dismount/proc/riscv/
(RiscvEncodingTable.cpp, RiscvEncodingTableCompressed.cpp,
RiscvEncodingTablePrivileged.cpp and their .inc parts) from the instruction
lists of riscvinsns.py. The masks and the values come from llvm-mc.

  python3 tests/riscv/tools/gen_encoding_tables.py
"""
import concurrent.futures
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rvlib
import riscvdsl
import riscvinsns
import riscvvector

OUT = os.path.join(rvlib.REPO, 'Source', 'dismount', 'proc', 'riscv')
CACHE = os.path.join(rvlib.REPO, 'out', 'riscv-probe-cache.json')

ENTRIES = []   # (table name, Entry)
CURRENT = ['']
TABLE_ORDER = []


def table(name):
    CURRENT[0] = name
    if name not in TABLE_ORDER:
        TABLE_ORDER.append(name)


def ins(mn, ops, feat, flags=(), flow=None, feat2=None, rv=None, probe=None, cond=None, width=32):
    e = riscvdsl.Entry(mn, ops, 'AUTO' if feat == 'AUTO' else 'FEAT_' + feat, list(flags), flow,
                       ('FEAT_' + feat2) if feat2 else None, rv, probe, cond, width)
    ENTRIES.append((CURRENT[0], e))


def assemble(bits, text, width):
    features = rvlib.all_features(bits, vector=True)
    p = rvlib.run([rvlib.tool('llvm-mc'), '-triple=' + rvlib.triple(bits),
                   '-mattr=' + rvlib.mattr_arg(features), '-show-encoding'],
                  ('.option norvc\n' if width == 32 else '') + text + '\n')
    m = re.search(r'encoding: \[(.*)\]', p.stdout)
    if not m or p.returncode != 0:
        return None, (p.stderr.strip().splitlines() or ['?'])[0]
    data = bytes(int(x, 16) for x in m.group(1).replace('0x', '').split(','))
    return int.from_bytes(data, 'little'), len(data)


def llvm_features(bits, names):
    """The LLVM feature list of a feature set (names), with what they imply
    (dismount_cli set)."""
    out = rvlib.run([rvlib.CLI, 'set', '--arch', 'rv%d' % bits, '--mattr',
                     'none,' + ','.join('+' + n for n in names)], '')
    return out.stdout.split()


def mark_ungated():
    """An entry which llvm-objdump decodes with no feature but its second
    feature (and the C extension for the compressed ones) is decoded by the
    disassembler whichever m_feature is enabled (RISCV_ENTRY_DECODE_UNGATED)."""
    feat_names = {}
    header = open(os.path.join(rvlib.REPO, 'Include', 'dismount', 'proc', 'riscv', 'RiscvFeatures.h')).read()
    order = re.findall(r'^\s+(FEAT_[A-Z0-9_]+),?', header, re.M)
    out = rvlib.run([rvlib.CLI, 'features', '--arch', 'rv64'], '')
    for line in out.stdout.splitlines():
        index, name = line.split()[:2]
        feat_names[order[int(index)]] = name
    for bits in (32, 64):
        group = [e for _, e in ENTRIES if e.rv == bits or (e.rv is None and bits == 64)]
        for e in group:
            if e.feat == 'FEAT_ZCA':
                continue
            base = []
            if e.feat2 and e.feat2 not in ('FEAT_I', 'FEAT_PRIVILEGED'):
                base.append(feat_names[e.feat2])
            if e.width == 16 and 'zca' not in base:
                base.append('zca')
            features = llvm_features(bits, base) if base else []
            value, size = cache_get(bits, e.probe_text(False), e.width)
            result = rvlib.objdump_bytes(value.to_bytes(size, 'little'), bits, features)
            text = result[0][2] if result else '<unknown>'
            word = text.split(' ')[0]
            # (decoded as this instruction: the mnemonic, with the aq / rl of the atomics)
            if text != '<unknown>' and word.startswith(e.mn):
                if 'RISCV_ENTRY_DECODE_UNGATED' not in e.flags:
                    e.flags.append('RISCV_ENTRY_DECODE_UNGATED')


VECTOR_CANDIDATES = ['zve32x', 'zve32f', 'zve64x', 'zve64f', 'zve64d', 'v', 'zvfhmin', 'zvfh',
                     'zvfbfmin', 'zvfbfwma', 'zvkb', 'zvbb', 'zvbc', 'zvkg', 'zvkned', 'zvknha',
                     'zvknhb', 'zvksed', 'zvksh']
# The names of the dismount features of the candidates
VECTOR_FEATURE_NAMES = {
    'zve32x': 'FEAT_ZVE32X', 'zve32f': 'FEAT_ZVE32F', 'zve64x': 'FEAT_ZVE64X',
    'zve64f': 'FEAT_ZVE64F', 'zve64d': 'FEAT_ZVE64D', 'v': 'FEAT_V', 'zvfhmin': 'FEAT_ZVFHMIN',
    'zvfh': 'FEAT_ZVFH', 'zvfbfmin': 'FEAT_ZVFBFMIN', 'zvfbfwma': 'FEAT_ZVFBFWMA',
    'zvkb': 'FEAT_ZVKB', 'zvbb': 'FEAT_ZVBB', 'zvbc': 'FEAT_ZVBC', 'zvkg': 'FEAT_ZVKG',
    'zvkned': 'FEAT_ZVKNED', 'zvknha': 'FEAT_ZVKNHA', 'zvknhb': 'FEAT_ZVKNHB',
    'zvksed': 'FEAT_ZVKSED', 'zvksh': 'FEAT_ZVKSH'}


def mc_accepts(lines, bits, mattr):
    """Which of the lines (one statement each) llvm-mc assembles with -mattr."""
    source = '\n'.join(lines) + '\n'
    p = rvlib.run([rvlib.tool('llvm-mc'), '-triple=' + rvlib.triple(bits), '-mattr=' + mattr,
                   '-filetype=null'], source)
    bad = set()
    for m in re.finditer(r'<stdin>:(\d+):\d+: error:', p.stderr):
        bad.add(int(m.group(1)) - 1)
    return [i not in bad for i in range(len(lines))]


def vector_text(entry, alt, overrides):
    """The text of a vector entry with the texts of some roles replaced (None
    removes the operand)."""
    parts = []
    for t in entry.tokens():
        if t.hidden:
            continue
        text = t.alt if alt else t.base
        if t.role in overrides:
            text = overrides[t.role]
        if text:
            parts.append(text)
    if entry.probe:
        return entry.probe.format(*parts)
    return entry.mn + ((' ' + ', '.join(parts)) if parts else '')


def resolve_vector(all_entries=None):
    """Find the feature and the constraints of the vector entries (the
    entries of 'feat' AUTO of the list, default: ENTRIES) from llvm-mc."""
    if all_entries is None:
        all_entries = [e for _, e in ENTRIES]
    entries = [e for e in all_entries if e.feat == 'AUTO']
    lines = [e.probe_text(False) for e in entries]
    accepted = {}
    for cand in VECTOR_CANDIDATES:
        accepted[cand] = mc_accepts(lines, 64, '+' + cand)
    closures = {c: set(llvm_features(64, [c])) for c in VECTOR_CANDIDATES}
    errors = 0
    for k, e in enumerate(entries):
        enabling = [c for c in VECTOR_CANDIDATES if accepted[c][k]]
        weakest = [c for c in enabling if all(c in closures[o] for o in enabling)]
        if not weakest:
            print('ERROR: no weakest feature for %s: %s' % (lines[k], enabling))
            errors += 1
            e.feat = 'FEAT_I'
            continue
        e.feat = VECTOR_FEATURE_NAMES[weakest[0]]
    # The constraints
    probes = []
    for e in entries:
        roles = {t.role: t for t in e.tokens() if t.role}
        row = {}
        if 'vd' in roles:
            # (the sources far from v0: a segment tuple of the destination would
            # overlap a source of the base text)
            far = {'vd': 'v0', 'vs2': 'v24', 'vs1': 'v28'}
            if 'vm' in roles:
                row['mask'] = vector_text(e, False, dict(far, vm='v0.t'))
            row['v0'] = vector_text(e, False, dict(far, vm=''))
            if 'vs2' in roles:
                row['vs2'] = vector_text(e, False, {'vd': roles['vs2'].base})
            if 'vs1' in roles:
                row['vs1'] = vector_text(e, False, {'vd': roles['vs1'].base})
        probes.append(row)
    flat = []
    for row in probes:
        for key, text in row.items():
            flat.append(text)
    ok = mc_accepts(flat, 64, rvlib.mattr_arg(rvlib.all_features(64, vector=True)))
    position = 0
    for e, row in zip(entries, probes):
        flags = []
        results = {}
        for key in row:
            results[key] = ok[position]
            position += 1
        if not results.get('mask', True):
            flags.append('RISCV_VR_NOT_MASK_WHEN_MASKED')
        if not results.get('v0', True):
            flags.append('RISCV_VR_NOT_V0')
        if not results.get('vs2', True):
            flags.append('RISCV_VR_NO_VS2_OVERLAP')
        if not results.get('vs1', True):
            flags.append('RISCV_VR_NO_VS1_OVERLAP')
        e.vflags = ' | '.join(flags) if flags else '0'
    return errors


CACHE_DATA = {}


def cache_get(bits, text, width):
    return CACHE_DATA['%d|%s|%d' % (bits, text, width)]


def main():
    riscvinsns.define(ins, table)
    riscvvector.define_vector(ins, table)
    if resolve_vector():
        sys.exit(1)
    cache = {}
    if os.path.exists(CACHE):
        cache = json.load(open(CACHE))
    jobs = {}
    for _, e in ENTRIES:
        bits_list = [e.rv] if e.rv else [64, 32]
        for bits in bits_list:
            for alt in (False, True):
                jobs[(bits, e.probe_text(alt), e.width)] = None
    todo = [k for k in jobs if '%d|%s|%d' % k not in cache]
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        for k, r in zip(todo, pool.map(lambda k: assemble(*k), todo)):
            cache['%d|%s|%d' % k] = list(r)
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    json.dump(cache, open(CACHE, 'w'))

    CACHE_DATA.update(cache)
    errors = 0
    for _, e in ENTRIES:
        bits_list = [e.rv] if e.rv else [64, 32]
        results = []
        for bits in bits_list:
            pair = []
            for alt in (False, True):
                text = e.probe_text(alt)
                value, size = cache['%d|%s|%d' % (bits, text, e.width)]
                if value is None:
                    print('ERROR: %s (rv%d): %s' % (text, bits, size))
                    errors += 1
                    pair = None
                    break
                if size * 8 != e.width:
                    print('ERROR: %s (rv%d): %d bytes, expected %d' % (text, bits, size, e.width // 8))
                    errors += 1
                    pair = None
                    break
                pair.append(value)
            results.append(pair)
        if any(r is None for r in results):
            e.mask = e.value = 0
            continue
        full = (1 << e.width) - 1
        mask = full & ~e.fields()
        base = results[0][0]
        e.mask = mask
        e.value = base & mask
        for pair in results:
            for v in pair:
                if v & mask != e.value:
                    print('ERROR: %s: example bits differ outside the fields: %08x vs %08x (mask %08x)'
                          % (e.probe_text(False), v & mask, e.value, mask))
                    errors += 1
    if errors:
        print('%d errors' % errors)
        sys.exit(1)

    mark_ungated()

    # The files
    header = '''#include "dismount/dismount.h"
/*
 * %(name)s
 *
 * %(what)s
 *
 * GENERATED by tests/riscv/tools/gen_encoding_tables.py from the instruction
 * lists of tests/riscv/tools/riscvinsns.py and riscvvector.py (the masks and
 * the values come from llvm-mc): edit the generator, not this file. The order of the entries is the
 * order of the decoder: an alias comes before the general encoding it prints
 * for.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
'''
    by_table = {}
    for name, e in ENTRIES:
        by_table.setdefault(name, []).append(e)
    for name in TABLE_ORDER:
        lines = [e.cpp() + ',' for e in by_table[name]]
        with open(os.path.join(OUT, 'RiscvEncodingTable_%s.inc' % name), 'w') as f:
            f.write('// GENERATED by tests/riscv/tools/gen_encoding_tables.py: do not edit\n')
            f.write('\n'.join(lines) + '\n')
    wrappers = [
        ('RiscvEncodingTable.cpp', 'The encoding table of the 32 bit scalar instructions (see RiscvEncodingTable.h).',
         'gRiscvBaseEncodingTable', ['base_int', 'base_csr', 'base_m_a', 'base_fp', 'base_bit', 'base_misc']),
        ('RiscvEncodingTableCompressed.cpp', 'The encoding table of the 16 bit compressed instructions (see RiscvEncodingTable.h).',
         'gRiscvCompressedEncodingTable', ['compressed']),
        ('RiscvEncodingTablePrivileged.cpp', 'The encoding table of the privileged instructions (see RiscvEncodingTable.h).',
         'gRiscvPrivilegedEncodingTable', ['privileged']),
        ('RiscvEncodingTableVector.cpp', 'The encoding table of the vector instructions: V, the Zve* subsets (see RiscvEncodingTable.h).',
         'gRiscvVectorEncodingTable', ['vector_config', 'vector_int', 'vector_mask', 'vector_fp']),
        ('RiscvEncodingTableVectorMemory.cpp', 'The encoding table of the vector loads and stores (see RiscvEncodingTable.h).',
         'gRiscvVectorMemoryEncodingTable', ['vector_mem']),
        ('RiscvEncodingTableVectorExtension.cpp', 'The encoding table of the vector extensions: Zvfbfmin, Zvfbfwma, Zvbb, Zvkb, Zvbc and the vector cryptography (see RiscvEncodingTable.h).',
         'gRiscvVectorExtensionEncodingTable', ['vector_ext']),
    ]
    for filename, what, array, parts in wrappers:
        with open(os.path.join(OUT, filename), 'w') as f:
            f.write(header % {'name': filename, 'what': what})
            f.write('''#include "xStl/types.h"
#include "dismount/Opcode.h"
#include "dismount/proc/riscv/RiscvEncodingTable.h"
#include "dismount/proc/riscv/RiscvEncodingTableInternal.h"
#include "dismount/proc/riscv/RiscvOperandCodec.h"

namespace riscvdis {
''')
            if array == 'gRiscvBaseEncodingTable':
                f.write('''
namespace {

// Zimop with Zicfiss: mop.r.28 is sspopchk / ssrdp, which have rd != x0 (ssrdp)
// or rs1 in x1, x5 (sspopchk, matched before); the other encodings of rd = x0
// are invalid in LLVM's decoder
bool conditionRdNotZero(uint32 e)
{
    return ((e >> 7) & 31) != 0;
}

// ... and mop.rr.7 is sspush (matched before): rd = rs1 = x0 is invalid
bool conditionNotSspush(uint32 e)
{
    return !((((e >> 7) & 31) == 0) && (((e >> 15) & 31) == 0));
}

} // anonymous namespace
''')
            f.write('''
const RiscvEncodingEntry %s[] = {
''' % array)
            for part in parts:
                f.write('#include "RiscvEncodingTable_%s.inc"\n' % part)
            f.write('''    { NULL, 0, 0, { }, 0, 0, 0, NULL, 0 }
};

} // namespace riscvdis
''')
    print('%d entries' % len(ENTRIES))


if __name__ == '__main__':
    main()
