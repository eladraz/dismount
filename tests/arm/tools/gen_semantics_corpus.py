#!/usr/bin/env python3
"""Write tests/arm/semantics/corpus/a64_base_all.s, the comprehensive corpus of the A64 base
semantics (tests/arm/semantics/TEST_PLAN.md): every mnemonic which gArm64SemanticTable lifts,
with the register and operand variety of the A64 encoding corpus (tests/arm/corpus/a64/*.s),
plus hand written groups (the exclusives joined with their stores, the system registers).

The lines are chosen so that every engine runs them the same way:
  * no x18 / x28 (reserved by the host runner);
  * no CONSTRAINED UNPREDICTABLE register overlaps (a store exclusive whose status register is
    its data or base register, a writeback whose base is a transferred register, a load pair
    of one register twice);
  * no branch to itself, no pointer authentication keys, no random / counter / identification
    system registers;
  * the Advanced SIMD / SVE / SME operands are not here (the lifters of those areas have
    their own corpora), the SIMD&FP loads and stores of the base table are.

    gen_semantics_corpus.py [--per-mnemonic N]
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

TABLE = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm', 'Arm64SemanticTable.cpp')
CORPUS = os.path.join(armlib.REPO, 'tests', 'arm', 'corpus', 'a64')
OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'semantics', 'corpus', 'a64_base_all.s')
FIELDS_OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'semantics', 'corpus', 'a64_base_fields.s')

# The encoding corpus files which hold base instructions (the SIMD, SVE and SME ones are skipped)
SKIP_FILES = {'sve.s', 'sme.s'}


def lifted_names():
    return set(re.findall(r'A64\("([^"]+)"', open(TABLE).read()))


def mnemonic_of(text):
    word = text.split()[0].lower()
    return word.split('.')[0]


def registers(text):
    """The general purpose register numbers named in the text, in order."""
    return [(m.group(1), int(m.group(2))) for m in re.finditer(r'\b([xw])(\d+)\b', text)]


def is_unpredictable(text):
    t = text.lower()
    mnemonic = mnemonic_of(t)
    regs = registers(t)
    base = re.search(r'\[\s*(x\d+|sp)', t)
    base_no = None
    if base and base.group(1) != 'sp':
        base_no = int(base.group(1)[1:])
    writeback = bool(re.search(r'\]!|\],\s*[#x-]', t))
    operands = t.split(None, 1)[1] if len(t.split(None, 1)) > 1 else ''
    before = operands.split('[')[0]
    data = []
    for m in re.finditer(r'\b(?:[xwsdqbh](\d+)|(xzr|wzr))\b', before):
        data.append(31 if m.group(2) else int(m.group(1)))
    # a writeback with the base among the transferred registers
    if writeback and base_no is not None and re.match(r'^(ld|st)', mnemonic) and base_no in data:
        return True
    # a load pair of one register twice
    if re.match(r'^(ldp|ldnp|ldpsw|ldtp|ldtnp|ldxp|ldaxp)$', mnemonic) and len(data) >= 2 and data[0] == data[1]:
        return True
    # a store exclusive: the status register is not a data register or the base
    if re.match(r'^st(l?)(x[rp]|txr|ltxr)[bh]?$', mnemonic) or mnemonic in ('sttxr', 'stltxr'):
        if not data:
            return False
        status = data[0]
        if status != 31 and (status in data[1:] or status == base_no):
            return True
    # ldapr / stlur with writeback: the base is not the data register (handled above); the pair
    # forms of FEAT_LRCPC3 are not lifted
    # a branch to itself (an endless loop on some engines)
    m = re.match(r'^(b|bl|cbz|cbnz|tbz|tbnz|b\.[a-z]+|bc\.[a-z]+)\s+.*(?:#|\.)\+?(-?\d+|0x[0-9a-f]+)$', t)
    if m and int(m.group(2), 0) == 0:
        return True
    # the unscaled acquire / release accesses fault when they are unaligned on the M4
    m = re.match(r'^(ldapur|stlur)(sb|sh|sw|b|h)?\s+([a-z])(?:\d+|zr),\s*\[[^,\]]+(?:,\s*#(-?(?:0x)?[0-9a-f]+))?\]$', t)
    if m:
        size = {'b': 1, 'h': 2, 'sb': 1, 'sh': 2, 'sw': 4}.get(m.group(2))
        if size is None:
            size = {'w': 4, 'x': 8, 'b': 1, 'h': 2, 's': 4, 'd': 8, 'q': 16}[m.group(3)]
        offset = int(m.group(4), 0) if m.group(4) else 0
        if offset % size != 0:
            return True
    return False


def is_selected(text, names):
    t = text.lower()
    mnemonic = mnemonic_of(t)
    if mnemonic not in names:
        return False
    # the reserved registers of the host runner
    if re.search(r'\b[xw](18|28)\b', t):
        return False
    # the vector operands belong to the SIMD, SVE and SME corpora (the scalar SIMD&FP register
    # loads and stores stay)
    if re.search(r'\bv\d+\.|\bz\d+|\bp\d+|\bza|\{|\bpn\d+', t):
        return False
    # the system registers: the unknown ones are helpers; the selected ones are in the hand
    # written group below
    if mnemonic in ('mrs', 'msr', 'sys', 'sysl', 'hint', 'dc', 'ic'):
        return False
    if mnemonic == 'psb':
        return False
    # FEAT_CMPBR: clang can't assemble it and no processor runs it (TestArm64Semantics.cpp covers it)
    if re.match(r'^cb(b|h)?(gt|ge|hi|hs|eq|ne|lt|lo)$', mnemonic):
        return False
    if re.match(r'^(pac|aut)', mnemonic) and mnemonic not in ('xpaci',):
        return False
    return not is_unpredictable(t)


def exclusive_group():
    """The load exclusives joined with a store exclusive (the monitor is open), a store to
    another address (it fails), and a bare store exclusive (the monitor is closed)."""
    lines = []
    for size, suffix, regs in (('b', 'b', 'w'), ('h', 'h', 'w'), ('w', '', 'w'), ('x', '', 'x')):
        for acq, rel in (('', ''), ('a', ''), ('', 'l'), ('a', 'l')):
            ld = 'ld%sxr%s %s0, [x4]' % ('a' if acq else '', suffix, regs)
            st = 'st%sxr%s w5, %s6, [x4]' % ('l' if rel else '', suffix, regs)
            lines.append('%s ; %s' % (ld, st))
            lines.append('%s ; %s' % (ld, st.replace('[x4]', '[x7]')))
            lines.append(st)
    for regs in ('w', 'x'):
        for acq in ('', 'a'):
            for rel in ('', 'l'):
                ld = 'ld%sxp %s0, %s1, [x4]' % (acq, regs, regs)
                st = 'st%sxp w5, %s6, %s7, [x4]' % (rel, regs, regs)
                lines.append('%s ; %s' % (ld, st))
                lines.append('%s ; %s' % (ld, st.replace('[x4]', '[x8]')))
                lines.append(st)
    lines.append('ldxr x0, [x4] ; clrex ; stxr w5, x6, [x4]')
    lines.append('ldxr x0, [x4] ; ldxr x1, [x7] ; stxr w5, x6, [x4]')
    lines.append('ldxr x0, [x4] ; stxr w5, x6, [x4] ; stxr w9, x6, [x4]')
    return lines


def system_group():
    return [
        'mrs x0, nzcv', 'msr nzcv, x1', 'msr nzcv, x1 ; mrs x2, nzcv',
        'mrs x0, fpcr', 'mrs x0, fpsr',
        'msr fpcr, x1 ; mrs x2, fpcr  // @state: x1=0', 'msr fpcr, x1  // @state: x1=0 fpcr=0',
        'msr fpcr, x1  // @state: x1=c00000', 'msr fpcr, x1  // @state: x1=1000000',
        'msr fpcr, x1  // @state: x1=2000000', 'msr fpsr, x1  // @state: x1=80000093',
        'msr fpsr, x1 ; mrs x2, fpsr  // @state: x1=f800009f',
        'dc zva, x3  // @state: x3=10100004040', 'dc zva, x3  // @state: x3=1010000403f',
        'dc civac, x3  // @state: x3=10100004040', 'dc cvac, x3  // @state: x3=10100004040',
        'dc cvau, x3  // @state: x3=10100004040', 'dc cvap, x3  // @state: x3=10100004040',
        'ic ivau, x3  // @state: x3=10100004040',
        'xpaci x0', 'xpacd x1', 'xpaclri',
        'xpaci x0  // @state: x0=00ff80ff12345678', 'xpaci x0  // @state: x0=0080ffff12345678',
        'xpaci x0  // @state: x0=ff80ffff12345678', 'xpacd x0  // @state: x0=0000ffff12345678',
        'xpaclri  // @state: x30=ff80ffff12345678', 'xpaclri  // @state: x30=0000ffff12345678',
        'cfinv  // @state: nzcv=0', 'cfinv  // @state: nzcv=f',
        'axflag  // @state: nzcv=0', 'axflag  // @state: nzcv=1', 'axflag  // @state: nzcv=a',
        'xaflag  // @state: nzcv=0', 'xaflag  // @state: nzcv=2', 'xaflag  // @state: nzcv=6',
        'xaflag  // @state: nzcv=8', 'xaflag  // @state: nzcv=f',
        'nop', 'yield', 'dgh', 'csdb', 'esb', 'tsb csync', 'clrbhb', 'bti', 'bti c', 'bti j',
        'bti jc', 'sb', 'isb', 'dmb ish', 'dmb ishld', 'dmb ishst', 'dmb sy', 'dmb ld', 'dmb st', 'dsb sy',
        'dsb ish', 'dsb st', 'dsb ld', 'ssbb', 'pssbb', 'clrex', 'clrex #7',
    ]


CONDITIONS = ['eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al', 'nv']


def directed_group():
    """The edge cases of the arithmetic and the conditions, with directed states."""
    all_flags = '// ' + ' '.join('@state: nzcv=%x' % n for n in range(16))
    lines = []
    for cond in CONDITIONS:
        lines.append('csel x0, x1, x2, %s   %s' % (cond, all_flags))
        lines.append('csinc w0, w1, w2, %s   %s' % (cond, all_flags))
        lines.append('csinv x0, x1, x2, %s   %s' % (cond, all_flags))
        lines.append('csneg w0, w1, w2, %s   %s' % (cond, all_flags))
        lines.append('ccmp x1, x2, #9, %s   %s' % (cond, all_flags))
        lines.append('ccmn w1, #7, #6, %s   %s' % (cond, all_flags))
        lines.append('ccmp w1, w2, #2, %s   %s' % (cond, all_flags))
        if cond not in ('al', 'nv'):
            lines.append('cset x0, %s   %s' % (cond, all_flags))
            lines.append('b.%s #8   %s' % (cond, all_flags))
    for op in ('adc', 'adcs', 'sbc', 'sbcs'):
        for width in ('x', 'w'):
            lines.append('%s %s0, %s1, %s2   // @state: nzcv=0 @state: nzcv=2 @state: nzcv=2 x1=ffffffffffffffff x2=0'
                         ' @state: nzcv=0 x1=0 x2=0 @state: nzcv=2 x1=7fffffffffffffff x2=0'
                         ' @state: nzcv=2 x1=80000000 x2=ffffffff' % (op, width, width, width))
    edges = ['x1=8000000000000000 x2=1', 'x1=7fffffffffffffff x2=1', 'x1=0 x2=1', 'x1=1 x2=ffffffffffffffff',
             'x1=80000000 x2=1', 'x1=7fffffff x2=ffffffff', 'x1=ffffffff x2=ffffffff', 'x1=8000000000000000 x2=ffffffffffffffff',
             'x1=ffffffffffffffff x2=ffffffffffffffff', 'x1=0 x2=0']
    states = '// ' + ' '.join('@state: %s' % e for e in edges)
    for text in ('adds x0, x1, x2', 'subs x0, x1, x2', 'adds w0, w1, w2', 'subs w0, w1, w2', 'cmp x1, x2',
                 'cmn w1, w2', 'negs x0, x1', 'sdiv x0, x1, x2', 'udiv x0, x1, x2', 'sdiv w0, w1, w2',
                 'udiv w0, w1, w2', 'smulh x0, x1, x2', 'umulh x0, x1, x2', 'smull x0, w1, w2', 'umull x0, w1, w2',
                 'smnegl x0, w1, w2', 'madd x0, x1, x2, x3', 'msub w0, w1, w2, w3', 'asr x0, x1, x2',
                 'lsl w0, w1, w2', 'ror x0, x1, x2', 'rorv w0, w1, w2', 'extr x0, x1, x2, #1', 'extr w0, w1, w2, #31',
                 'clz x0, x1', 'cls w0, w1', 'cls x0, x1', 'rbit w0, w1', 'rev x0, x1', 'rev16 x0, x1', 'rev32 x0, x1',
                 'rev w0, w1', 'rev16 w0, w1', 'crc32b w0, w1, w2', 'crc32h w0, w1, w2', 'crc32w w0, w1, w2',
                 'crc32x w0, w1, x2', 'crc32cb w0, w1, w2', 'crc32ch w0, w1, w2', 'crc32cw w0, w1, w2',
                 'crc32cx w0, w1, x2'):
        lines.append('%s   %s' % (text, states))
    return lines


def forms_group():
    """The general forms behind the aliases (the disassembler prints the alias)."""
    return [
        'ubfm x0, x1, #3, #62', 'ubfm x0, x1, #62, #3', 'ubfm w0, w1, #5, #31', 'ubfm w0, w1, #31, #0',
        'sbfm x0, x1, #3, #62', 'sbfm x0, x1, #62, #3', 'sbfm w0, w1, #5, #31', 'sbfm w0, w1, #31, #0',
        'bfm x0, x1, #3, #62', 'bfm x0, x1, #62, #3', 'bfm w0, w1, #5, #31', 'bfm w0, w1, #31, #0',
        'bfc x0, #3, #20', 'bfc w0, #31, #1', 'bfc w0, #0, #32', 'bfi w0, w1, #0, #32', 'bfi x0, x1, #63, #1',
        'movz x0, #0x1234, lsl #48', 'movz x0, #0, lsl #32', 'movz w0, #0xffff, lsl #16', 'movz w0, #0',
        'movn x0, #0x1234, lsl #48', 'movn w0, #0xffff, lsl #16', 'movk x0, #0xabcd, lsl #48', 'movk w0, #0, lsl #16',
        'hint #0', 'hint #6', 'hint #21', 'hint #127', 'orr x0, xzr, #0x5555555555555555', 'orr w0, wzr, #0xaaaaaaaa',
        'mov sp, x1', 'mov x0, sp', 'add sp, sp, #16', 'sub sp, sp, #1, lsl #12',
    ]


def logical_immediate_is_valid(n, immr, imms, sf):
    """True if N:immr:imms encodes a bitmask immediate (DecodeBitMasks of the Arm ARM)."""
    value = (n << 6) | ((~imms) & 0x3f)
    if value == 0:
        return False
    length = value.bit_length() - 1
    if length < 1:
        return False
    if length == 6 and not sf:
        return False
    levels = (1 << length) - 1
    return (imms & levels) != levels


def fields_group():
    """Words (.inst) which cover the whole space of the fields of the bitfield, the logical
    immediate and the extended register instructions."""
    words = []
    for sf in (0, 1):
        size = 64 if sf else 32
        for opc in (0, 1, 2):
            for immr in range(size):
                for imms in range(size):
                    words.append(('bitfield', (sf << 31) | (opc << 29) | (0x26 << 23) | (sf << 22) | (immr << 16) |
                                  (imms << 10) | (1 << 5) | 0))
    valid = [(n, r, i, sf) for sf in (0, 1) for n in (0, 1) for r in range(64) for i in range(64)
             if logical_immediate_is_valid(n, r, i, sf) and (sf or n == 0) and (sf or (r < 32 and i < 63))]
    for index, (n, r, i, sf) in enumerate(valid):
        for opc in (0, 1, 2, 3):
            # every 4th encoding for each opcode
            if (index + opc) % 4 == 0:
                words.append(('logical immediate', (sf << 31) | (opc << 29) | (0x24 << 23) | (n << 22) |
                              (r << 16) | (i << 10) | (1 << 5) | (0 if opc != 3 else 0)))
    for sf in (0, 1):
        for op in (0, 1):
            for s in (0, 1):
                for option in range(8):
                    for amount in range(5):
                        words.append(('extended register', (sf << 31) | (op << 30) | (s << 29) | (0x0b << 24) |
                                      (1 << 21) | (2 << 16) | (option << 13) | (amount << 10) | (1 << 5) | 0))
    for sf in (0, 1):
        for op in (0, 1):
            for s in (0, 1):
                for shift in range(3):
                    for amount in (0, 1, 7, 31, 32 if sf else 30, 63 if sf else 31):
                        words.append(('shifted register', (sf << 31) | (op << 30) | (s << 29) | (0x0b << 24) |
                                      (shift << 22) | (2 << 16) | (amount << 10) | (1 << 5) | 0))
    for sf in (0, 1):
        for opc in range(4):
            for shift in range(3):
                for amount in (0, 1, 13, 31, 32 if sf else 30, 63 if sf else 31):
                    for n in (0, 1):
                        words.append(('logical register', (sf << 31) | (opc << 29) | (0x0a << 24) | (shift << 22) |
                                      (n << 21) | (2 << 16) | (amount << 10) | (1 << 5) | 0))
    return words


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--per-mnemonic', type=int, default=80)
    args = parser.parse_args()
    names = lifted_names()
    groups = collections.OrderedDict()
    seen = set()
    for path in sorted(glob.glob(os.path.join(CORPUS, '*.s'))):
        if os.path.basename(path) in SKIP_FILES:
            continue
        for raw in open(path):
            if raw.startswith('//') or not raw.strip():
                continue
            m = re.match(r'^(.*?)\s*//\s*@feature:\s*(\S+)\s*$', raw.rstrip('\n'))
            text, feature = (m.group(1), m.group(2)) if m else (raw.split('//')[0].strip(), '')
            text = text.strip()
            if not text or text in seen or not is_selected(text, names):
                continue
            seen.add(text)
            groups.setdefault(mnemonic_of(text), []).append((text, feature))

    out = ['// Generated by tests/arm/tools/gen_semantics_corpus.py: do not edit.',
           '// The A64 base instructions which dismount lifts, run with',
           '//   run_semantics.py --arch a64 --file a64_base_all [--oracle host]',
           '// No x18 / x28 (the host runner), no UNPREDICTABLE register overlaps.', '']
    total = 0
    for mnemonic in sorted(groups):
        items = groups[mnemonic]
        if len(items) > args.per_mnemonic:
            # evenly spaced, deterministic
            step = len(items) / float(args.per_mnemonic)
            items = [items[int(i * step)] for i in range(args.per_mnemonic)]
        out.append('// --- %s' % mnemonic)
        for text, feature in items:
            out.append('%s%s' % (text, ('   // @feature: ' + feature) if feature else ''))
            total += 1
    out.append('// --- the exclusives, joined with their stores')
    for line in exclusive_group():
        out.append(line)
        total += 1
    out.append('// --- the edge cases of the arithmetic and the conditions')
    for line in directed_group():
        out.append(line)
        total += 1
    out.append('// --- the general forms of the aliases')
    for line in forms_group():
        out.append(line)
        total += 1
    out.append('// --- the system instructions')
    for line in system_group():
        out.append(line)
        total += 1
    with open(OUTPUT, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote %s: %d lines, %d mnemonics' % (OUTPUT, total, len(groups)))
    fields = ['// Generated by tests/arm/tools/gen_semantics_corpus.py: do not edit.',
              '// The whole space of the fields of the bitfield, logical immediate, shifted and extended',
              '// register instructions (.inst words, Rd = x0, Rn = x1, Rm = x2).', '']
    for kind, word in fields_group():
        fields.append('.inst 0x%08x   // %s' % (word, kind))
    with open(FIELDS_OUTPUT, 'w') as f:
        f.write('\n'.join(fields) + '\n')
    print('wrote %s: %d words' % (FIELDS_OUTPUT, len(fields) - 4))


if __name__ == '__main__':
    main()
