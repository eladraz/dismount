#!/usr/bin/env python3
"""
Regenerates tests/ia32/corpus/*.asm and tests/ia32/corpus64/*.asm (see
tests/ia32/TEST_PLAN.md, "Corpus format").

  python3 tests/ia32/tools/gen_corpus.py [--nasm nasm] [--out DIR] [--out64 DIR]
                                         [--only 32|64]

* Hand-built matrices: 00_addressing32, 01_addressing16, 02_prefixes,
  03_immediates, 04_branches, 05_bits16 (32 bit corpus) and 00_addressing64,
  02_prefixes64, 03_immediates64, 04_branches64 (64 bit corpus).
* Per-feature files: parsed from insns.dat (nasm's x86 instruction table, BSD-2,
  copied next to this script). Macro lines ($bwdq, $br, ...) are expanded.
  Operand types are instantiated with concrete values; every candidate line is
  validated by nasm in batches and only lines nasm accepts are kept. The 64 bit
  corpus is the same table read in long mode (NOLONG entries dropped, LONG and
  X86_64 kept, REX registers r8-r15 / xmm8-xmm15 / spl-dil, rip relative
  memory).

Nothing here talks to dismount. The output is plain nasm.
"""
import argparse
import collections
import itertools
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INSNS = os.path.join(HERE, 'insns.dat')
DEFAULT_OUT = os.path.normpath(os.path.join(HERE, '..', 'corpus'))
DEFAULT_OUT64 = os.path.normpath(os.path.join(HERE, '..', 'corpus64'))

# True while the 64 bit (long mode) corpus is generated
LONG = False

# ---------------------------------------------------------------------------
# Feature mapping (insns.dat flag -> Linux /proc/cpuinfo name)
# ---------------------------------------------------------------------------

# Checked in order; the first matching token decides the feature of an entry.
FEATURE_RULES = [
    ('FPU', 'fpu'),
    ('AVX10_2', 'avx10_2'), ('AVX10_VNNIINT', 'avx10_2'),
    ('AVX512FP16', 'avx512_fp16'), ('AVX512BF16', 'avx512_bf16'),
    ('AVX512VP2INTERSECT', 'avx512_vp2intersect'),
    ('AVX512VPOPCNTDQ', 'avx512_vpopcntdq'), ('AVX512BITALG', 'avx512_bitalg'),
    ('AVX512VBMI2', 'avx512_vbmi2'), ('AVX512VBMI', 'avx512vbmi'),
    ('AVX512VNNI', 'avx512_vnni'), ('AVX512IFMA', 'avx512ifma'),
    ('AVX5124VNNIW', 'avx512_4vnniw'), ('AVX5124FMAPS', 'avx512_4fmaps'),
    ('AVX512VL', 'avx512vl'), ('AVX512BW', 'avx512bw'), ('AVX512DQ', 'avx512dq'),
    ('AVX512CD', 'avx512cd'), ('AVX512ER', 'avx512er'), ('AVX512PF', 'avx512pf'),
    ('AVX512F', 'avx512f'), ('AVX512', 'avx512f'),
    ('AVXVNNIINT8', 'avx_vnni_int8'), ('AVXVNNIINT16', 'avx_vnni_int16'),
    ('AVXVNNI', 'avx_vnni'), ('AVXIFMA', 'avx_ifma'),
    ('AVXNECONVERT', 'avx_ne_convert'),
    ('VAES', 'vaes'), ('VPCLMULQDQ', 'vpclmulqdq'), ('GFNI', 'gfni'),
    ('SHA512', 'sha512'), ('SHA', 'sha_ni'), ('HSM3', 'sm3'), ('HSM4', 'sm4'),
    ('AESKLEWIDE_KL', 'aes_wide_kl'), ('AESKLE', 'aes_kl'),
    ('AVX2', 'avx2'), ('FMA', 'fma'), ('F16C', 'f16c'), ('AVX', 'avx'),
    ('SSE42', 'sse4_2'), ('SSE41', 'sse4_1'), ('SSSE3', 'ssse3'),
    ('PRESCOTT', 'pni'), ('SSE3', 'pni'),
    ('SSE2', 'sse2'), ('SSE', 'sse'), ('KATMAI', 'sse'),
    ('MMX', 'mmx'),
    ('CLWB', 'clwb'), ('CLFLUSHOPT', 'clflushopt'), ('CLDEMOTE', 'cldemote'),
    ('RDPID', 'rdpid'), ('MOVDIR64B', 'movdir64b'), ('SERIALIZE', 'serialize'),
    ('SGX', 'sgx'), ('INVPCID', 'invpcid'), ('SMAP', 'smap'),
    ('PCONFIG', 'pconfig'), ('ENQCMD', 'enqcmd'), ('PKU', 'pku'),
    ('WAITPKG', 'waitpkg'), ('TSXLDTRK', 'tsxldtrk'), ('MPX', 'mpx'),
    ('RTM', 'rtm'), ('HLE', 'hle'), ('MONITOR', 'monitor'), ('VMX', 'vmx'),
    ('CET', 'cet_ss'), ('BMI2', 'bmi2'), ('BMI1', 'bmi1'), ('ADX', 'adx'),
    ('MOVRS', 'movrs'), ('UINTR', 'uintr'), ('PREFETCHWT1', 'prefetchwt1'),
    ('HRESET', 'hreset'), ('MSRLIST', 'msrlist'), ('MSR_IMM', 'msr_imm'),
    ('PREFETCHI', 'prefetchi'), ('WBNOINVD', 'wbnoinvd'), ('PBNDKB', 'pbndkb'),
    ('P6', 'p6'), ('WILLAMETTE', 'p4'), ('PENT', 'pentium'), ('486', 'i486'),
    ('386', 'i386'), ('286', 'i386'), ('186', 'i386'), ('8086', 'i386'),
]

# Families and encodings that are out of scope (plan: AMD-only/obsolete, 64-bit)
EXCLUDE_AMD_OBSOLETE = {'3DNOW', 'SSE4A', 'SSE5', 'XOP', 'FMA4', 'TBM', 'CYRIX',
                        'UNDOC', 'OBSOLETE', 'AMD', 'CLZERO', 'MONITORX', 'PBNDKB_X'}
EXCLUDE_64BIT = {'LONG', 'X86_64', 'APX', 'ND', 'PSEUDO', 'NEVER'}
# Plan says AMX is excluded together with the 64-bit-only families
EXCLUDE_PREFIX = ('AMX',)

# Feature name -> file name (only where they differ)
FILE_NAME = {'fpu': 'x87'}

# nasm macro size letters -> bit widths / suffixes
BITS = {'b': '8', 'w': '16', 'd': '32', 'q': '64'}
REG_AX = {'b': 'al', 'w': 'ax', 'd': 'eax', 'q': 'rax'}
REG_CX = {'b': 'cl', 'w': 'cx', 'd': 'ecx', 'q': 'rcx'}

# Memory operand used everywhere a memory form is needed
MEM_BASE = '[ebx+ecx*2+0x10]'
MEM_ABS = '[0x12345678]'
SIZE_KW = {8: 'byte', 16: 'word', 32: 'dword', 64: 'qword', 80: 'tword',
           128: 'oword', 256: 'yword', 512: 'zword'}
BRANCH_MN = {'CALL', 'LOOP', 'LOOPE', 'LOOPZ', 'LOOPNE', 'LOOPNZ', 'JECXZ',
             'JCXZ', 'XBEGIN'}


# Long mode memory operands: a REX.B base with an index, and rip relative
MEM_BASE64 = '[r9+rcx*2+0x10]'
MEM_RIP64 = '[rel $+0x20]'


def sized_mem(bits, brd=None):
    """Memory candidates for an operand of `bits` width (None = no keyword)."""
    kw = SIZE_KW.get(bits, '') if bits else ''
    pre = (kw + ' ') if kw else ''
    first = pre + (MEM_BASE64 if LONG else MEM_BASE)
    if brd:
        first += brd
    return [first, pre + (MEM_RIP64 if LONG else MEM_ABS)]


GPR8 = ['al', 'cl']
GPR16 = ['ax', 'cx']
GPR32 = ['eax', 'ecx']
# Long mode: bl / sil need or avoid a REX prefix, r10b needs REX.B
GPR8_64 = ['bl', 'sil', 'r10b']
GPR16_64 = ['bx', 'r9w']
GPR32_64 = ['ebx', 'r10d']
GPR64_64 = ['rbx', 'r9']
XMM_64 = ['xmm1', 'xmm9']


def _decorate_reg(regs, decos):
    out = []
    for r in regs:
        out.append(r)
        if 'mask' in decos:
            out.append(r + '{k1}')
        if 'z' in decos and 'mask' in decos:
            out.append(r + '{k1}{z}')
        if 'er' in decos:
            out.append(r + '{rn-sae}')
        if 'sae' in decos:
            out.append(r + '{sae}')
    return out


def operand_candidates(op, mn):
    """Concrete spellings for one insns.dat operand type; None if unsupported."""
    raw = op
    op = op.replace('*', '').replace('?', '').replace('+', '')
    parts = op.split('|')
    base = parts[0]
    decos = set(parts[1:])
    bm = re.search(r'b(16|32|64)$', ''.join(p for p in decos if re.match(r'b\d\d$', p)) or '')
    brd_bits = None
    for p in decos:
        m = re.match(r'b(16|32|64)$', p)
        if m:
            brd_bits = int(m.group(1))
    if base == 'void':
        return ['']
    if base in ('al', 'cl', 'ax', 'cx', 'eax', 'ecx', 'dx', 'edx', 'bx', 'ebx'):
        return [base]
    if base in ('rax', 'rcx', 'rdx'):
        return [base] if LONG else None
    if base in ('near', 'short') or (base.startswith('imm') and 'near' in decos) \
            or base == 'short' or (base == 'imm' and mn in BRANCH_MN) \
            or (base.startswith('imm') and mn.startswith('J') and not base.startswith('imm8') is None):
        pass
    # Relative branches
    if base == 'short':
        return ['short $+0x10', 'short $-0x10']
    if base == 'near':
        return ['$+0x10', '$-0x20']
    if base.startswith('imm') and ('near' in decos or 'abs' in decos or
                                   mn.startswith('J') or mn in BRANCH_MN):
        if base == 'imm8' or 'short' in decos:
            return ['short $+0x10', '$-0x20']
        if 'far' in decos:
            return ['0x1234:0x12345678']
        return ['$+0x10', '$-0x20']
    if base == 'imm' and mn in BRANCH_MN:
        return ['$+0x10', '$-0x20']
    if base.startswith('imm') and 'far' in decos:
        return ['0x1234:0x12345678']
    if base in ('imm8', 'imm'):
        return ['0x12', '0x1234', '0x12345678'] if base == 'imm' else ['0x12', '-1']
    if base == 'imm64' and LONG:
        return ['0x1122334455667788']
    if base in ('udword64', 'sdword64') and LONG:
        return ['0x12345678', '-1']
    if base == 'imm16':
        return ['0x1234']
    if base == 'imm32':
        return ['0x12345678']
    if base == 'sbyte':
        return ['0x12', '-1']
    if base == 'one':
        return ['1']
    if base == 'mem_offs':
        return ['[qword 0x1122334455667788]'] if LONG else [MEM_ABS]
    if base in ('imm64', 'udword64', 'sdword64'):
        return None
    if base == 'rm_sel' or base.startswith('spec'):
        return None
    # Segment / control / debug / test registers
    fixed = {'reg_al': ['al'], 'reg_ax': ['ax'], 'reg_eax': ['eax'],
             'reg_cl': ['cl'], 'reg_cx': ['cx'], 'reg_ecx': ['ecx'],
             'reg_dx': ['dx'], 'reg_edx': ['edx'], 'reg_bx': ['bx'],
             'reg_ebx': ['ebx'], 'reg_cs': ['cs'], 'reg_ds': ['ds'],
             'reg_es': ['es'], 'reg_fs': ['fs'], 'reg_gs': ['gs'],
             'reg_ss': ['ss'], 'reg_sreg': ['fs', 'ds'],
             'reg_creg': ['cr0', 'cr8'] if LONG else ['cr0', 'cr4'],
             'reg_dreg': ['dr0', 'dr7'],
             'reg_treg': ['tr3'], 'reg32na': ['eax'],
             'reg_rax': ['rax'] if LONG else None,
             'fpu0': ['st0'], 'fpureg': ['st1', 'st0'], 'bndreg': ['bnd0', 'bnd1'],
             'xmm0': ['xmm0'], 'tmmreg': ['tmm1', 'tmm2']}
    if base in fixed:
        return fixed[base]
    if base in ('reg_rax', 'reg_rcx'):
        return [base[4:]] if LONG else None
    # Macro-expanded size-specific accumulator/count registers
    if base.startswith('reg_') or base.startswith('reg'):
        m = re.match(r'^reg(\d+)$', base)
        if m and not LONG:
            bits = int(m.group(1))
            return {8: ['al', 'cl'], 16: ['ax', 'cx'], 32: ['eax', 'ecx']}.get(bits)
    if base.startswith('kreg'):
        return _decorate_reg(['k1', 'k2'], decos)
    if base.startswith('krm'):
        bits = re.search(r'(\d+)$', base)
        mem = sized_mem(int(bits.group(1)) if bits else None)
        return ['k2'] + mem if bits else ['k2', MEM_BASE]
    if base == 'mmxreg':
        return ['mm1', 'mm2']
    if base == 'mmxreg64' and LONG:
        return None
    if base in ('mmxrm', 'mmxrm64'):
        return ['mm2'] + sized_mem(64)
    if base.startswith('xmem') or base.startswith('ymem') or base.startswith('zmem'):
        bits = int(re.search(r'(\d+)$', base).group(1))
        idx = {'x': 'xmm2', 'y': 'ymm2', 'z': 'zmm2'}[base[0]]
        pre = (SIZE_KW.get(bits, '') + ' ') if bits else ''
        return [pre + '[%s+%s*4+0x10]' % ('r9' if LONG else 'ebx', idx)]
    if base == 'mem':
        return [MEM_BASE64, MEM_RIP64] if LONG else [MEM_BASE, MEM_ABS]
    m = re.match(r'^mem(\d+)$', base)
    if m:
        return sized_mem(int(m.group(1)))
    # Vector registers and their r/m forms
    m = re.match(r'^(xmm|ymm|zmm)(reg|rm)(\d*)$', base)
    if m:
        kind, form, bits = m.group(1), m.group(2), m.group(3)
        regs = {'xmm': ['xmm1', 'xmm2'], 'ymm': ['ymm1', 'ymm2'],
                'zmm': ['zmm1', 'zmm2']}[kind]
        if LONG:
            # The registers which need REX.R / REX.B (EVEX: R' / X)
            regs = {'xmm': ['xmm1', 'xmm10'], 'ymm': ['ymm1', 'ymm10'],
                    'zmm': ['zmm1', 'zmm18']}[kind]
        if form == 'reg':
            return _decorate_reg(regs, decos)
        size = int(bits) if bits else {'xmm': 128, 'ymm': 256, 'zmm': 512}[kind]
        # Rounded regs only apply to register forms, broadcast to memory forms
        regs_c = _decorate_reg([regs[1]], decos)
        mem = sized_mem(size, '{1to%d}' % (size // brd_bits) if brd_bits else None)
        return regs_c + mem
    if base == 'xmmrm':
        return _decorate_reg(['xmm10' if LONG else 'xmm2'], decos) + sized_mem(128)
    m = re.match(r'^xmmrm(\d+)$', base)
    if m:
        bits = int(m.group(1))
        mem = sized_mem(bits, '{1to%d}' % (128 // brd_bits) if brd_bits else None)
        return _decorate_reg(['xmm10' if LONG else 'xmm2'], decos) + mem
    m = re.match(r'^(reg|rm)(8|16|32|64)$', base)
    if m:
        form, bits = m.group(1), int(m.group(2))
        if LONG:
            regs = {8: GPR8_64, 16: GPR16_64, 32: GPR32_64, 64: GPR64_64}[bits]
            if form == 'reg':
                return list(regs)
            return list(regs[:2]) + sized_mem(bits)
        if bits == 64:
            return None
        regs = {8: GPR8, 16: GPR16, 32: GPR32}[bits]
        if form == 'reg':
            return list(regs)
        return [regs[0]] + sized_mem(bits)
    m = re.match(r'^reg(\d+)?$', base)
    return None


def _size_bits_of(base):
    m = re.search(r'(\d+)$', base)
    return int(m.group(1)) if m else None


def expand_candidates(entry):
    """Cartesian product of operand candidates for one instruction entry."""
    lists = []
    for op in entry['ops']:
        cands = operand_candidates(op, entry['mn'])
        if cands is None:
            return None, 'unsupported operand %s' % op
        if cands == ['']:
            continue
        lists.append(cands[:4])
    if not lists:
        return [entry['mn'].lower()], None
    out = []
    for combo in itertools.islice(itertools.product(*lists), 96):
        out.append(entry['mn'].lower() + ' ' + ', '.join(combo))
    return out, None


# ---------------------------------------------------------------------------
# insns.dat parsing and macro expansion
# ---------------------------------------------------------------------------

LINE_RE = re.compile(r'^\s*(\S+)\s+(\S+)\s+\[([^\]]*)\]\s*(.*?)\s*$')


def _expand_token(tok, letter):
    """Expand one operand/mnemonic token of a macro line for size `letter`."""
    if '|' in tok:
        head, rest = tok.split('|', 1)
        return _expand_token(head, letter) + '|' + rest
    if tok.startswith('ax#') or re.match(r'^(ax|cx)#', tok):
        name = tok[:2]
        rest = tok[3:] if tok.startswith(name + '#') else tok[2:]
        table = REG_AX if name == 'ax' else REG_CX
        return table[letter] + rest.replace('#', '')
    if '#' not in tok:
        return tok
    base = tok.replace('#', '')
    if base.startswith('sbyte'):
        return 'sbyte'
    if base.startswith('kreg'):
        return base
    if base.startswith('krm') or base.startswith('mem') or base.startswith('rm') \
            or base.startswith('reg') or base.startswith('imm'):
        return base + BITS[letter]
    return base


def parse_insns(path):
    """Yields (mnemonic, [operands], code, flagset, source line number)."""
    entries = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip('\n')
            stripped = line.strip()
            if not stripped or stripped.startswith(';'):
                continue
            sizes = None
            macro = None
            if stripped.startswith('$'):
                m = re.match(r'^\$(\w+)\s+(.*)$', stripped)
                if not m:
                    continue
                macro = m.group(1)
                body = m.group(2)
                # "$br $wdq JCX#Z" carries a second macro
                m2 = re.match(r'^\$(\w+)\s+(.*)$', body)
                if m2:
                    macro = macro + ',' + m2.group(1)
                    body = m2.group(2)
                if '[' not in body:
                    continue  # $arith / $shift group headers
                stripped = body
            m = LINE_RE.match(stripped)
            if not m:
                continue
            mn, ops, code, flags = m.groups()
            if mn in ('DB', 'DW', 'DD', 'DQ', 'DT', 'DO', 'DY', 'DZ', 'RESB', 'RESW',
                      'RESD', 'RESQ', 'REST', 'RESO', 'RESY', 'RESZ', 'INCBIN', 'EQU'):
                continue
            flagset = set(re.findall(r'[A-Z][A-Z0-9_]*', flags))
            sizes_list = [None]
            if macro:
                letters = set()
                for mname in macro.split(','):
                    letters |= set(c for c in mname if c in 'bwdq')
                # keep b/w/d/q ordering used by nasm
                sizes_list = [c for c in 'bwdq' if c in letters] or [None]
                if macro.split(',')[0] == 'zwdq':
                    pass
            ops_list = [o for o in ops.split(',') if o]
            for letter in sizes_list:
                if letter is None:
                    mn_s = mn
                    ops_s = ops_list
                else:
                    if '%' in mn:
                        suffix = '' if letter == 'z' else letter
                        mn_s = mn.replace('%', suffix)
                    elif 'X#Z' in mn:
                        mn_s = mn.replace('#', '').replace('X', 'X')
                        mn_s = {'w': 'JCXZ', 'd': 'JECXZ'}.get(letter, mn.replace('#', ''))
                    else:
                        mn_s = mn
                    ops_s = [_expand_token(o, letter) for o in ops_list]
                if letter is not None and letter not in 'bwdq':
                    continue
                entries.append({'mn': mn_s, 'ops': ops_s, 'code': code,
                                'flags': flagset, 'line': lineno,
                                'size': letter})
    return entries


# Hand-written groups the nasm table generates with $arith / $shift
SYNTH = []
for _mn in ['ADD', 'OR', 'ADC', 'SBB', 'AND', 'SUB', 'XOR', 'CMP']:
    SYNTH += [
        {'mn': _mn, 'ops': ['rm8', 'reg8'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'b'},
        {'mn': _mn, 'ops': ['rm32', 'reg32'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'd'},
        {'mn': _mn, 'ops': ['reg8', 'rm8'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'b'},
        {'mn': _mn, 'ops': ['reg32', 'rm32'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'd'},
        {'mn': _mn, 'ops': ['reg_al', 'imm8'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'b'},
        {'mn': _mn, 'ops': ['reg_eax', 'imm32'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'd'},
        {'mn': _mn, 'ops': ['rm8', 'imm8'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'b'},
        {'mn': _mn, 'ops': ['rm32', 'imm32'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'd'},
        {'mn': _mn, 'ops': ['rm32', 'sbyte'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'd'},
    ]
for _mn in ['ROL', 'ROR', 'RCL', 'RCR', 'SHL', 'SAL', 'SHR', 'SAR']:
    SYNTH += [
        {'mn': _mn, 'ops': ['rm8', 'one'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'b'},
        {'mn': _mn, 'ops': ['rm32', 'one'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'd'},
        {'mn': _mn, 'ops': ['rm8', 'reg_cl'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'b'},
        {'mn': _mn, 'ops': ['rm32', 'reg_cl'], 'code': '', 'flags': {'8086'}, 'line': 0, 'size': 'd'},
        {'mn': _mn, 'ops': ['rm8', 'imm8'], 'code': '', 'flags': {'186'}, 'line': 0, 'size': 'b'},
        {'mn': _mn, 'ops': ['rm32', 'imm8'], 'code': '', 'flags': {'386'}, 'line': 0, 'size': 'd'},
    ]


def classify(entry):
    """Returns (feature or None, drop-reason or None)."""
    flags = entry['flags']
    if LONG:
        # Long mode: the entries which are not valid in it are dropped, the
        # 64 bit ones kept. APX (extended EVEX) and AMX stay out of the plan
        if flags & {'NOLONG', 'PSEUDO', 'NEVER', 'ND', 'APX'}:
            return None, 'not valid in 64-bit mode or pseudo'
        if re.search(r'evex\.(nf|nd|ndd|ndx|zu)[.0-9]', entry['code']):
            return None, 'APX (excluded by plan)'
        if any(tok in EXCLUDE_AMD_OBSOLETE - {'AMD'} for tok in flags):
            return None, 'AMD-only/obsolete family'
        if any(tok.startswith(EXCLUDE_PREFIX) for tok in flags):
            return None, 'AMX (excluded by plan)'
    else:
        if flags & EXCLUDE_64BIT or 'PSEUDO' in flags:
            return None, '64-bit-only or pseudo'
        if re.search(r'evex\.(nf|nd|ndd|ndx|zu)[.0-9]', entry['code']) or 'o64' in entry['code'].split():
            return None, '64-bit-only (APX/REX.W)'
        if any(tok in EXCLUDE_AMD_OBSOLETE for tok in flags):
            return None, 'AMD-only/obsolete family'
        if any(tok.startswith(EXCLUDE_PREFIX) for tok in flags):
            return None, 'AMX (excluded by plan)'
        if any(o.startswith('reg64') or o.startswith('rm64') or o.startswith('mmxreg64')
               or o in ('imm64', 'imm64|abs', 'imm64|near', 'udword64', 'sdword64')
               or o.startswith('reg64:') for o in entry['ops']):
            return None, '64-bit operand'
    for tok, feat in FEATURE_RULES:
        if tok in flags:
            return feat, None
    return 'i386', None


# ---------------------------------------------------------------------------
# nasm validation
# ---------------------------------------------------------------------------

ERR_RE = re.compile(r'^(.*?):(\d+): error:', re.M)


def nasm_accepts(nasm, lines, bits=32):
    """Returns list of bool (True = nasm assembles the line in its own context)."""
    accepted = [True] * len(lines)
    current = list(range(len(lines)))
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'batch.asm')
        while current:
            with open(src, 'w') as f:
                f.write('bits %d\n' % bits)
                for i in current:
                    f.write(lines[i] + '\n')
            res = subprocess.run([nasm, '-f', 'bin', '-o', os.devnull, src],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 universal_newlines=True)
            errs = [int(m.group(2)) for m in ERR_RE.finditer(res.stderr)]
            if not errs:
                if res.returncode != 0:
                    raise RuntimeError('nasm failed without line errors:\n' + res.stderr)
                break
            bad = set()
            for ln in errs:
                if ln >= 2:
                    bad.add(current[ln - 2])
            if not bad:
                raise RuntimeError('unexpected nasm output:\n' + res.stderr)
            for i in bad:
                accepted[i] = False
            current = [i for i in current if i not in bad]
    return accepted


def nasm_check_file_lines(nasm, lines, bits):
    """Splits hand-built lines into accepted and rejected (for reporting)."""
    ok = nasm_accepts(nasm, lines, bits)
    good = [l for l, a in zip(lines, ok) if a]
    bad = [l for l, a in zip(lines, ok) if not a]
    return good, bad


# ---------------------------------------------------------------------------
# Hand-built matrices
# ---------------------------------------------------------------------------

REGS32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
DISPS = ['', '+0x7f', '+0x80', '-0x80', '-0x81', '+0x12345678']


def addressing32_mems():
    mems = []
    for b in REGS32:
        for d in DISPS:
            mems.append('[%s%s]' % (b, d))
    index_regs = ['eax', 'ecx', 'edx', 'ebx', 'ebp', 'esi', 'edi']
    for i in index_regs:
        for s in (1, 2, 4, 8):
            for b in ('eax', 'esp', 'ebp', 'esi'):
                for d in ('', '+0x10', '-0x10'):
                    mems.append('[%s+%s*%d%s]' % (b, i, s, d))
    for i in index_regs:
        for s in (1, 2, 4, 8):
            mems.append('[%s*%d+0x10]' % (i, s))
            mems.append('[%s*%d+0x12345678]' % (i, s))
    mems += ['[0x12345678]', '[0x7f]', '[0x80]', '[esp]', '[ebp]']
    return mems


def gen_00():
    lines = []
    templates = ['mov eax, {m}', 'mov {m}, eax', 'add ecx, {m}', 'add {m}, ecx',
                 'lea edx, {m}', 'inc dword {m}', 'push dword {m}', 'test {m}, ecx',
                 'movzx eax, byte {m}', 'imul eax, {m}', 'imul eax, {m}, 0x12']
    for m in addressing32_mems():
        for t in templates:
            lines.append(t.format(m=m))
    for a in REGS32:
        for b in REGS32:
            lines.append('add %s, %s' % (a, b))
            lines.append('mov %s, %s' % (a, b))
    return lines


def gen_01():
    mems = ['[bx+si]', '[bx+di]', '[bp+si]', '[bp+di]', '[si]', '[di]', '[bp]', '[bx]',
            '[bx+si+0x7f]', '[bx+si+0x80]', '[bx+si-0x80]', '[bx+si-0x81]',
            '[bp+di-0x81]', '[bp+0x12]', '[si+0x7f]', '[di-0x80]', '[bx+0x1234]',
            '[bp+0x1234]', '[si-0x1234]', '[bx+di+0x7fff]', '[0x1234]', '[bx]']
    templates = ['mov ax, {m}', 'mov {m}, ax', 'add cx, {m}', 'lea ax, {m}',
                 'inc word {m}', 'push word {m}', 'movzx eax, byte {m}',
                 'test {m}, cx', 'mov eax, {m}', 'mov {m}, eax']
    lines = []
    for m in mems:
        for t in templates:
            lines.append(t.format(m=m))
    return lines


def gen_02():
    lines = []
    # operand-size override (0x66)
    lines += ['mov ax, bx', 'mov ax, [ebx]', 'mov [ebx], ax', 'add ax, 0x1234',
              'add ax, 0x12', 'push ax', 'pop cx', 'inc ax', 'xchg ax, bx',
              'lea ax, [ebx+ecx*2+0x10]', 'shl ax, 1', 'cwd', 'pushfw', 'popfw',
              'pushaw', 'popaw', 'retw', 'retw 8', 'iretw', 'movsw', 'stosw',
              'lodsw', 'scasw', 'cmpsw', 'mov ax, 0x1234', 'mov word [ebx], 0x1234',
              'push word 0x12', 'sub ax, cx', 'bsf ax, bx', 'movzx ax, byte [ebx]',
              'cmp word [esi+ecx*4], 0x12', 'xadd [ebx], ax', 'imul ax, cx, 0x1234']
    # segment overrides
    for seg in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
        lines += ['mov eax, [%s:ebx]' % seg,
                  'mov [%s:ecx+0x10], edx' % seg,
                  'add eax, [%s:0x1234]' % seg,
                  'mov al, [%s:esi]' % seg,
                  'mov eax, [%s:ebx+ecx*4]' % seg,
                  'mov ax, [%s:ebx]' % seg,
                  'push dword [%s:ebp+0x7f]' % seg,
                  'lea eax, [%s:ebx]' % seg,
                  'movsb' if seg == 'es' else 'lodsb']
    # lock prefix
    lines += ['lock add [ebx], eax', 'lock inc dword [ecx]',
              'lock dec dword [esi+0x10]', 'lock cmpxchg [ebx], ecx',
              'lock xadd [edx], eax', 'lock xchg [ebx], eax', 'lock btc [eax], ecx',
              'lock neg dword [ebx]', 'lock not byte [eax]', 'lock or [eax], ecx',
              'lock and dword [edi], 0x12', 'lock sub [eax], ecx', 'lock adc [ebx], eax',
              'lock sbb [ebx], eax', 'lock xor [ebx], ecx', 'lock add [ebx], ax',
              'lock cmpxchg8b [esi]', 'lock bts [ebx+ecx*8], eax']
    # rep / repe / repne on string instructions
    for s in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb',
              'cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw', 'scasd', 'insb', 'outsb'):
        for p in ('rep', 'repe', 'repne', 'repz', 'repnz'):
            lines.append('%s %s' % (p, s))
    lines += ['rep movsw', 'rep stosw', 'rep movsd', 'repe cmpsw', 'repne scasw',
              'lock rep movsb', 'rep fs movsb', 'rep ds movsd']
    return lines


def gen_03():
    lines = []
    for op in ('add', 'or', 'adc', 'sbb', 'and', 'sub', 'xor', 'cmp', 'test'):
        for imm in ('0x7f', '0x80', '-0x80', '-0x81', '0x12', '0x12345678',
                    '-1', '0xff', '0x7fffffff', '-0x80000000'):
            lines.append('%s eax, %s' % (op, imm))
        lines.append('%s ecx, 0x7f' % op)
        lines.append('%s dword [ebx], 0x80' % op)
        lines.append('%s byte [ebx], 0x12' % op)
        lines.append('%s word [ebx], 0x1234' % op)
        lines.append('%s ax, 0x1234' % op)
        lines.append('%s al, 0x12' % op)
    lines += ['mov al, 0x12', 'mov ax, 0x1234', 'mov eax, 0x12345678',
              'mov eax, 0', 'mov eax, -1', 'mov byte [eax], 0x12', 'mov word [eax], 0x1234',
              'mov dword [eax], -1', 'mov dword [eax+ebx*4], 0x80',
              'push 0x7f', 'push 0x80', 'push -0x80', 'push -0x81', 'push 0x12345678',
              'push word 0x12', 'push dword 0x12', 'push 0x12345678',
              'imul eax, ebx, 0x12', 'imul eax, ebx, 0x12345678',
              'imul ecx, dword [ebx], 0x7f', 'imul ecx, dword [ebx], -0x81',
              'imul eax, ebx, -1', 'test eax, 0x7f', 'and ecx, 0xff', 'or edx, -2',
              'cmp ebx, 0x7fffffff', 'cmp ebx, -0x80000000',
              'enter 0x10, 0', 'enter 0, 0', 'ret 0x10', 'ret 0xfffe', 'int 0x80',
              'int 3', 'int 0x21', 'in al, 0x60', 'out 0x80, al', 'in eax, dx',
              'out dx, al', 'shl eax, 4', 'rol ecx, 0x1f', 'sar edx, 31', 'shr bx, 8',
              'mov al, [0x12]', 'mov eax, [0x12345678]', 'mov [0x12345678], eax',
              'mov byte [0x12345678], 0x12', 'mov ecx, [0x12345678]',
              'test al, 0x80', 'test ax, 0x8000', 'xor eax, eax', 'and al, 0x7f']
    return lines


def gen_04():
    lines = []
    ccs = ['jo', 'jno', 'jb', 'jnb', 'jz', 'jnz', 'jbe', 'ja', 'js', 'jns', 'jp',
           'jnp', 'jl', 'jge', 'jle', 'jg', 'jc', 'jnc', 'je', 'jne', 'jnae', 'jae',
           'jna', 'jnbe', 'jpe', 'jpo', 'jnge', 'jnl', 'jng', 'jnle']
    distances = ['$+0x10', '$-0x10', '$+0x7f', '$-0x7f', '$+0x80', '$-0x80',
                 '$+0x1000', '$-0x1000']
    for mn in ccs + ['jmp', 'call']:
        for d in distances:
            lines.append('%s %s' % (mn, d))
        if mn != 'call':
            lines.append('%s short $+0x10' % mn)
        lines.append('%s near $+0x10' % mn)
    for mn in ('loop', 'loope', 'loopne', 'loopz', 'loopnz', 'jecxz'):
        for d in ('$+0x10', '$-0x10', '$+0x7f', '$-0x7c', '$+0x2'):
            lines.append('%s %s' % (mn, d))
    lines += ['jmp 0x1234:0x12345678', 'call 0x1234:0x12345678', 'jmp far dword [eax]',
              'call far dword [ebx+0x10]', 'jmp dword [eax]', 'call dword [eax]',
              'jmp [eax]', 'call [eax]', 'call eax', 'jmp ecx', 'call dword [ecx+ebx*4]',
              'jmp ax', 'call bx', 'jmp word [esi]', 'jmp dword [ebp-0x10]',
              'ret', 'ret 8', 'ret 0x7fff', 'retf', 'retf 4', 'iret', 'iretd', 'leave',
              'into', 'int1', 'jcxz $+0x10', 'xbegin $+0x10', 'call $+0x7fffff']
    return lines


def gen_05():
    return [
        'mov ax, bx', 'mov [bx+si], ax', 'mov ax, [bp+di+0x10]', 'mov ax, [0x1234]',
        'mov al, 0x12', 'mov ax, 0x1234', 'add ax, 0x7f', 'add ax, 0x80',
        'add ax, -0x80', 'add ax, -0x81', 'push ax', 'pop bx', 'inc si', 'dec word [bx]',
        'lea ax, [bx+si+0x10]', 'mov eax, ebx', 'mov eax, [ebx+ecx*2]',
        'mov eax, [bx]', 'movzx ax, byte [bx]', 'test ax, 0x7f', 'imul ax, bx, 0x12',
        'shl ax, 1', 'shl ax, cl', 'rol bx, 4', 'xchg ax, bx', 'mov es, ax',
        'mov [cs:bx], ax', 'call $+0x10', 'jmp $+0x10', 'jz $+0x10', 'jnz $-0x10',
        'jcxz $+0x10', 'loop $-0x10', 'call 0x1234:0x5678', 'jmp far word [bx]',
        'ret', 'ret 4', 'retf', 'int 0x21', 'pusha', 'popa', 'pushf', 'popf',
        'pushad', 'popad', 'iret', 'in al, 0x60', 'out 0x80, al', 'movsw',
        'rep movsw', 'lock inc word [bx]', 'enter 0x10, 0', 'push 0x1234',
        'mov ax, [si]', 'mov [di+0x7f], dx', 'cmp byte [bx+si], 0x12',
    ]


HAND_FILES = [
    ('00_addressing32', 'i386', 32, gen_00, 'ModRM/SIB/disp matrix (32-bit addressing)'),
    ('01_addressing16', 'i386', 32, gen_01, '16-bit addressing via 0x67 in 32-bit mode'),
    ('02_prefixes', 'i386', 32, gen_02, '0x66, segment overrides, lock, rep family'),
    ('03_immediates', 'i386', 32, gen_03, 'immediate sizes, sign extension, boundaries'),
    ('04_branches', 'i386', 32, gen_04, 'jcc/jmp/call/loop, short and near, far'),
    ('05_bits16', 'i386', 16, gen_05, 'core coverage in bits 16 mode'),
]


# ---------------------------------------------------------------------------
# Hand-built matrices of the 64 bit corpus
# ---------------------------------------------------------------------------

REGS64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
          'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']
REGS32_64 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi',
             'r8d', 'r9d', 'r10d', 'r11d', 'r12d', 'r13d', 'r14d', 'r15d']
REGS16_64 = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di',
             'r8w', 'r9w', 'r10w', 'r11w', 'r12w', 'r13w', 'r14w', 'r15w']
REGS8_REX = ['al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil',
             'r8b', 'r9b', 'r10b', 'r11b', 'r12b', 'r13b', 'r14b', 'r15b']
REGS8_LEGACY = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
DISPS64 = ['', '+0x7f', '+0x80', '-0x80', '-0x81', '+0x12345678', '-0x12345678']


def addressing64_mems():
    mems = []
    # [base + disp] for all the 16 bases (rsp / r12 need a SIB byte,
    # rbp / r13 need a displacement)
    for b in REGS64:
        for d in DISPS64:
            mems.append('[%s%s]' % (b, d))
    # [base + index * scale + disp]; the index rsp does not exist, r12 does
    index_regs = [r for r in REGS64 if r != 'rsp']
    for i in index_regs:
        for sc in (1, 2, 4, 8):
            for b in ('rax', 'rsp', 'rbp', 'r8', 'r12', 'r13', 'r15'):
                for d in ('', '+0x10', '-0x10'):
                    mems.append('[%s+%s*%d%s]' % (b, i, sc, d))
    # no base, a disp32
    for i in index_regs:
        for sc in (1, 2, 4, 8):
            mems.append('[%s*%d+0x10]' % (i, sc))
            mems.append('[%s*%d-0x12345678]' % (i, sc))
    # absolute (sign extended disp32 in a SIB byte)
    mems += ['[0x12345678]', '[0x7f]', '[0x80]', '[-0x80000000]',
             '[0xffffffff80000000]', '[0]']
    # rip relative
    for d in ('$+0x10', '$-0x10', '$+0x7f', '$+0x80', '$-0x80', '$+0x12345678',
              '$-0x12345678', '$+0x1000', '$+0x6', '$+0x7'):
        mems.append('[rel %s]' % d)
    # 32 bit addresses (0x67): the registers are 32 bit, [eip + disp32]
    for b in REGS32_64:
        for d in ('', '+0x10', '+0x12345678'):
            mems.append('[%s%s]' % (b, d))
    for i in ('eax', 'r9d', 'r12d', 'r15d'):
        for sc in (1, 2, 4, 8):
            for b in ('eax', 'esp', 'ebp', 'r8d', 'r13d'):
                mems.append('[%s+%s*%d+0x10]' % (b, i, sc))
            mems.append('[%s*%d+0x10]' % (i, sc))
    mems += ['[a32 0x12345678]', '[a32 rel $+0x10]', '[a32 rel $-0x10]']
    return mems


def gen64_00():
    lines = []
    templates = ['mov eax, {m}', 'mov {m}, r9d', 'add r10, {m}', 'lea r11, {m}',
                 'inc dword {m}', 'push qword {m}', 'movzx eax, byte {m}',
                 'imul rax, {m}', 'cmp qword {m}, 0x12', 'test {m}, cl',
                 'movzx r8d, word {m}', 'mov byte {m}, 0x12']
    for m in addressing64_mems():
        for t in templates:
            lines.append(t.format(m=m))
    for a in REGS64:
        for b in REGS64:
            lines.append('add %s, %s' % (a, b))
            lines.append('mov %s, %s' % (a, b))
    for a in REGS32_64:
        for b in REGS32_64:
            lines.append('xor %s, %s' % (a, b))
    for a in REGS16_64:
        lines.append('mov %s, bx' % a)
        lines.append('mov cx, %s' % a)
    # The byte registers: spl..dil need a REX prefix, ah..bh forbid it
    for a in REGS8_REX:
        for b in REGS8_REX:
            lines.append('mov %s, %s' % (a, b))
        for b in REGS8_LEGACY:
            lines.append('mov %s, %s' % (a, b))
            lines.append('mov %s, %s' % (b, a))
    for a in REGS8_LEGACY:
        for b in REGS8_LEGACY:
            lines.append('mov %s, %s' % (a, b))
    for r in REGS8_REX:
        lines += ['mov %s, 0x12' % r, 'inc %s' % r, 'neg %s' % r,
                  'movzx eax, %s' % r, 'movsx rax, %s' % r, 'test %s, 1' % r,
                  'mov [rbx], %s' % r, 'mov %s, [rbx]' % r]
    for r, r32 in zip(REGS64, REGS32_64):
        lines += ['push %s' % r, 'pop %s' % r, 'inc %s' % r, 'dec %s' % r,
                  'bswap %s' % r, 'xchg rax, %s' % r, 'xchg %s, rbx' % r,
                  'mov %s, 0x1122334455667788' % r, 'not %s' % r,
                  'call %s' % r, 'jmp %s' % r, 'imul rax, %s, 0x12' % r,
                  'shl %s, 4' % r, 'shr %s, cl' % r, 'movsxd rax, %s' % r32]
    for r in REGS32_64:
        lines += ['mov %s, 0x12345678' % r, 'bswap %s' % r, 'inc %s' % r,
                  'mul %s' % r, 'idiv %s' % r]
    for r in REGS16_64:
        lines += ['mov %s, 0x1234' % r, 'push %s' % r, 'pop %s' % r, 'inc %s' % r,
                  'movzx eax, %s' % r, 'movsx rax, %s' % r]
    # SSE: the extended registers
    for x in ('xmm0', 'xmm7', 'xmm8', 'xmm15'):
        for y in ('xmm1', 'xmm9', 'xmm15'):
            lines += ['addps %s, %s' % (x, y), 'movaps %s, %s' % (x, y),
                      'pxor %s, %s' % (x, y), 'cvtsd2ss %s, %s' % (x, y)]
        for r in ('rax', 'r8', 'r15'):
            lines += ['cvtsi2sd %s, %s' % (x, r), 'cvttsd2si %s, %s' % (r, x),
                      'movq %s, %s' % (x, r), 'movq %s, %s' % (r, x),
                      'movmskps %s, %s' % (r, x)]
        for r in ('eax', 'r9d'):
            lines += ['movd %s, %s' % (x, r), 'movd %s, %s' % (r, x),
                      'cvtsi2ss %s, %s' % (x, r), 'cvttss2si %s, %s' % (r, x),
                      'pextrw %s, %s, 1' % (r, x), 'pinsrw %s, %s, 1' % (x, r)]
    return lines


def gen64_02():
    lines = []
    # operand-size override (0x66) and REX.W, which wins over it
    lines += ['mov ax, bx', 'mov ax, [rbx]', 'mov [rbx], ax', 'add ax, 0x1234',
              'add ax, 0x12', 'push ax', 'pop cx', 'inc ax', 'xchg ax, bx',
              'lea ax, [rbx+rcx*2+0x10]', 'shl ax, 1', 'cwd', 'pushfw', 'popfw',
              'retw', 'retw 8', 'iretw', 'movsw', 'stosw', 'lodsw', 'scasw',
              'cmpsw', 'mov ax, 0x1234', 'mov word [rbx], 0x1234',
              'push word 0x12', 'push word 0x1234', 'sub ax, cx', 'bsf ax, bx',
              'movzx ax, byte [rbx]', 'cmp word [rsi+rcx*4], 0x12',
              'xadd [rbx], ax', 'imul ax, cx, 0x1234', 'mov r8w, r9w',
              'mov ax, [r9+r10*2]', 'push word [rax]', 'pop word [rax]',
              'cbw', 'leavew', 'enterw 0x10, 0', 'xchg r8w, ax', 'nop',
              'xchg ax, ax', 'xchg eax, eax', 'xchg r8d, eax', 'xchg rax, r8',
              'xchg rax, rax', 'o16 nop']
    lines += ['mov rax, rbx', 'mov rax, [rbx]', 'mov [rbx], rax', 'add rax, rbx',
              'push qword [rax]', 'pop qword [rax]', 'cdqe', 'cqo', 'cwde', 'cdq',
              'movsq', 'stosq', 'lodsq', 'scasq', 'cmpsq', 'movsd', 'stosd',
              'lodsd', 'scasd', 'cmpsd', 'movsb', 'insd', 'outsd', 'insb', 'outsb',
              'iretq', 'iret', 'iretd', 'retfq', 'retf', 'pushf', 'popf', 'pushfq',
              'popfq', 'leave', 'leaveq', 'enter 0x10, 0', 'enterq 0x10, 0',
              'lea rax, [rbx]', 'lea eax, [rbx]', 'cmpxchg16b [rax]',
              'cmpxchg8b [rax]', 'cmpxchg16b [r9+r10*8]', 'cmpxchg8b [r9]',
              'movsxd rax, ecx', 'movsxd rax, dword [rax]', 'movsxd eax, ecx',
              'movsxd r8, r9d', 'syscall', 'sysret', 'swapgs',
              'mov rax, cr0', 'mov cr0, rax', 'mov cr8, rax', 'mov r9, cr8',
              'mov rax, cr2', 'mov cr3, r15', 'mov cr4, r8', 'mov rax, dr0',
              'mov dr7, rax', 'mov r10, dr3', 'lgdt [rax]', 'lidt [rax]',
              'sgdt [rax]', 'sidt [rax]', 'lgdt [r9]', 'fxsave [rax]',
              'fxrstor [rax]', 'fxsave64 [rax]', 'fxrstor64 [r9]', 'ldmxcsr [rax]',
              'stmxcsr [r9+rax*4]', 'invlpg [rax]', 'bt r9, rax', 'bts [rax], r10',
              'btr rax, 5', 'btc qword [rax], 0x3f', 'shld rax, rbx, 4',
              'shrd r9, r10, cl', 'movnti [rax], r9', 'movnti [rax], ecx',
              'lss eax, [rax]', 'lfs rax, [rbx]', 'lgs eax, [rbx]',
              'lss rax, [rbx]', 'in al, dx', 'in eax, dx', 'out dx, eax',
              'in al, 0x60', 'out 0x80, al', 'in eax, 0x60', 'out 0x60, eax']
    # segment overrides: only fs and gs have a base in 64 bit mode
    for seg in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
        lines += ['mov eax, [%s:rbx]' % seg,
                  'mov [%s:rcx+0x10], edx' % seg,
                  'add rax, [%s:0x1234]' % seg,
                  'mov al, [%s:rsi]' % seg,
                  'mov r9, [%s:rbx+r10*4]' % seg,
                  'mov ax, [%s:rbx]' % seg,
                  'push qword [%s:rbp+0x7f]' % seg,
                  'lea eax, [%s:rbx]' % seg,
                  'mov eax, [%s:rel $+0x10]' % seg,
                  'mov al, [%s:qword 0x1122334455667788]' % seg,
                  'movsb' if seg == 'es' else 'lodsb']
    # lock prefix
    lines += ['lock add [rbx], eax', 'lock inc dword [rcx]',
              'lock dec qword [rsi+0x10]', 'lock cmpxchg [rbx], rcx',
              'lock xadd [rdx], rax', 'lock xchg [rbx], rax', 'lock btc [rax], rcx',
              'lock neg dword [rbx]', 'lock not byte [rax]', 'lock or [rax], r9',
              'lock and dword [rdi], 0x12', 'lock sub [rax], rcx', 'lock adc [rbx], r8',
              'lock sbb [rbx], eax', 'lock xor [rbx], r10d', 'lock add [rbx], ax',
              'lock cmpxchg8b [rsi]', 'lock cmpxchg16b [rsi]', 'lock bts [rbx+rcx*8], rax']
    # rep / repe / repne on string instructions (with REX.W)
    for sn in ('movsb', 'movsw', 'movsd', 'movsq', 'stosb', 'stosw', 'stosd', 'stosq',
               'lodsb', 'lodsq', 'cmpsb', 'cmpsw', 'cmpsd', 'cmpsq', 'scasb', 'scasw',
               'scasd', 'scasq', 'insb', 'outsb', 'insd', 'outsd'):
        for pf in ('rep', 'repe', 'repne', 'repz', 'repnz'):
            lines.append('%s %s' % (pf, sn))
    lines += ['rep movsq', 'rep stosq', 'repe cmpsq', 'repne scasq', 'lock rep movsb',
              'rep fs movsb', 'rep gs movsq', 'a32 rep movsb', 'a32 movsq',
              'a32 rep stosq', 'a32 lodsb', 'a32 scasd', 'a32 cmpsb', 'a32 movsb',
              'a32 movsd', 'a32 stosb', 'a32 loop $+0x10']
    # REX prefix implied by the operands
    lines += ['mov spl, al', 'mov sil, dil', 'mov bpl, 5', 'mov r8b, r9b',
              'mov al, r15b', 'mov [r8], sil', 'mov dil, [r9+rax]', 'mov ah, al',
              'movzx eax, ah', 'movzx eax, spl', 'movzx r8d, r9b',
              'movsx r8, r9b', 'movsx r8, r9w', 'movzx rax, word [rax]',
              'movzx rax, bl', 'setz sil', 'setz r9b', 'setz ah', 'xchg sil, al',
              'push r8', 'pop r15', 'push r8w', 'pop r15w', 'push fs', 'pop gs',
              'push qword fs', 'pop qword gs', 'nop dword [rax]', 'nop word [rax+rax]',
              'nop qword [rax]', 'nop r8d', 'prefetcht0 [r9]', 'prefetchnta [rax+r10*8]']
    return lines


def gen64_03():
    lines = []
    for op in ('add', 'or', 'adc', 'sbb', 'and', 'sub', 'xor', 'cmp', 'test'):
        for imm in ('0x7f', '0x80', '-0x80', '-0x81', '0x12', '0x12345678',
                    '-1', '0x7fffffff', '-0x80000000'):
            lines.append('%s rax, %s' % (op, imm))
            lines.append('%s r9, %s' % (op, imm))
            lines.append('%s qword [rbx], %s' % (op, imm))
        for imm in ('0x7f', '0x80', '-0x80', '-0x81', '0x12', '0x12345678', '-1',
                    '0xff', '0x7fffffff', '-0x80000000'):
            lines.append('%s eax, %s' % (op, imm))
        lines.append('%s ecx, 0x7f' % op)
        lines.append('%s r10d, 0x7f' % op)
        lines.append('%s dword [rbx], 0x80' % op)
        lines.append('%s byte [rbx], 0x12' % op)
        lines.append('%s word [rbx], 0x1234' % op)
        lines.append('%s ax, 0x1234' % op)
        lines.append('%s al, 0x12' % op)
        lines.append('%s r11b, 0x12' % op)
        lines.append('%s sil, 0x12' % op)
    lines += ['mov al, 0x12', 'mov ax, 0x1234', 'mov eax, 0x12345678',
              'mov eax, 0', 'mov eax, -1', 'mov byte [rax], 0x12',
              'mov word [rax], 0x1234', 'mov dword [rax], -1',
              'mov dword [rax+rbx*4], 0x80', 'mov qword [rax], -1',
              'mov qword [rax], 0x12345678', 'mov qword [rax], -0x80000000',
              'mov qword [r9+r10*8], 0x7fffffff',
              'mov rax, 0x1122334455667788', 'mov r8, 0x1122334455667788',
              'mov r15, 0x8877665544332211', 'mov rax, 0x12345678',
              'mov rax, 0xffffffff', 'mov rax, 0x100000000', 'mov rax, -1',
              'mov rax, -0x80000000', 'mov rax, -0x80000001',
              'mov rax, 0xffffffff80000000', 'mov rax, 0x7fffffff',
              'mov rax, 0x80000000', 'mov r8d, 0xffffffff', 'mov r9d, 0x12345678',
              'mov r8w, 0x1234', 'mov r8b, 0x12', 'mov rcx, 0',
              'mov ecx, 0x80000000',
              'push 0x7f', 'push 0x80', 'push -0x80', 'push -0x81', 'push 0x12345678',
              'push -0x12345678', 'push qword 0x12', 'push word 0x12',
              'push word 0x1234', 'push 0x7fffffff', 'push -0x80000000',
              'imul eax, ebx, 0x12', 'imul rax, rbx, 0x12345678',
              'imul r9, [rbx], 0x7f', 'imul r10, qword [rbx], -0x81',
              'imul rax, rbx, -1', 'imul ecx, dword [rbx], 0x12345678',
              'test rax, 0x7f', 'test rax, 0x12345678', 'test eax, 0x7f',
              'test r9, -1', 'and rcx, 0xff', 'or rdx, -2',
              'cmp rbx, 0x7fffffff', 'cmp rbx, -0x80000000',
              'enter 0x10, 0', 'enter 0, 0', 'ret 0x10', 'ret 0xfffe', 'int 0x80',
              'int 3', 'int 0x21', 'in al, 0x60', 'out 0x80, al', 'in eax, dx',
              'out dx, al', 'shl rax, 4', 'rol r9, 0x1f', 'sar rdx, 63', 'shr bx, 8',
              'shl r8d, 1', 'shl r8, 1', 'rcl r10b, 1', 'sar qword [rax], 1',
              'shl rax, cl', 'ror qword [r9], cl',
              'mov al, [qword 0x1122334455667788]', 'mov ax, [qword 0x1122334455667788]',
              'mov eax, [qword 0x1122334455667788]', 'mov rax, [qword 0x1122334455667788]',
              'mov [qword 0x1122334455667788], al', 'mov [qword 0x1122334455667788], ax',
              'mov [qword 0x1122334455667788], eax', 'mov [qword 0x1122334455667788], rax',
              'mov al, [qword 0]', 'mov rax, [qword -1]',
              'mov al, [a32 0x12345678]', 'mov eax, [a32 0x12345678]',
              'mov rax, [a32 0x12345678]', 'mov [a32 0x12345678], eax',
              'mov [a32 0x12345678], al', 'mov eax, [fs:qword 0x1122334455667788]',
              'mov al, [0x12]', 'mov eax, [0x12345678]', 'mov [0x12345678], eax',
              'mov rax, [0x12345678]', 'mov rax, [-0x80000000]',
              'mov byte [0x12345678], 0x12', 'mov ecx, [0x12345678]',
              'test al, 0x80', 'test ax, 0x8000', 'xor eax, eax', 'and al, 0x7f',
              'xor r9d, r9d', 'xor r8, r8', 'add rsp, 0x28', 'sub rsp, 0x1000',
              'lea rax, [rsp+0x20]', 'cmp byte [rel $+0x20], 0x12',
              'mov dword [rel $+0x20], 0x12345678', 'mov qword [rel $+0x20], -1',
              'add qword [rel $+0x20], 0x12345678', 'cmp word [rel $+0x20], 0x1234',
              'imul eax, [rel $+0x20], 0x12', 'shl qword [rel $+0x20], 4',
              'pshufd xmm9, [rel $+0x20], 0x12', 'cmpps xmm8, [rel $+0x20], 3',
              'enter 0xffff, 0xff', 'enter 0x10, 3']
    return lines


def gen64_04():
    lines = []
    ccs = ['jo', 'jno', 'jb', 'jnb', 'jz', 'jnz', 'jbe', 'ja', 'js', 'jns', 'jp',
           'jnp', 'jl', 'jge', 'jle', 'jg', 'jc', 'jnc', 'je', 'jne', 'jnae', 'jae',
           'jna', 'jnbe', 'jpe', 'jpo', 'jnge', 'jnl', 'jng', 'jnle']
    distances = ['$+0x10', '$-0x10', '$+0x7f', '$-0x7f', '$+0x80', '$-0x80',
                 '$+0x1000', '$-0x1000', '$+0x7fffff', '$-0x7fffff']
    for mn in ccs + ['jmp', 'call']:
        for d in distances:
            lines.append('%s %s' % (mn, d))
        if mn != 'call':
            lines.append('%s short $+0x10' % mn)
        lines.append('%s near $+0x10' % mn)
    for mn in ('loop', 'loope', 'loopne', 'loopz', 'loopnz', 'jrcxz', 'jecxz'):
        for d in ('$+0x10', '$-0x10', '$+0x7f', '$-0x7c', '$+0x2'):
            lines.append('%s %s' % (mn, d))
    for mn in ('loop', 'loope', 'loopne'):
        lines += ['%s $+0x10, ecx' % mn, '%s $+0x10, rcx' % mn]
    for r in REGS64:
        lines += ['call %s' % r, 'jmp %s' % r, 'call qword [%s]' % r,
                  'jmp qword [%s+0x10]' % r, 'push qword [%s]' % r]
    lines += ['call [rax]', 'jmp [rax]', 'call qword [rbx+rcx*4]',
              'jmp qword [rbp-0x10]', 'call qword [r9+r10*8+0x12345678]',
              'jmp qword [rel $+0x20]', 'call qword [rel $-0x20]',
              'call [rel $+0x10]', 'jmp [a32 0x12345678]', 'call qword [0x12345678]',
              'jmp far [rax]', 'call far [rax]', 'jmp far dword [rax]',
              'call far dword [rax]', 'jmp far qword [rax]', 'call far qword [r9]',
              'jmp far word [rax]',
              'ret', 'ret 8', 'ret 0x7fff', 'retf', 'retf 4', 'retfq', 'retfq 8',
              'iret', 'iretq', 'iretw', 'leave', 'int1', 'int3', 'xbegin $+0x10',
              'call $+0x7fffff', 'jmp near $+0x10', 'jmp $+0x2', 'jmp $+0x3',
              'jz $+0x2', 'jz $+0x6', 'call $+0x5', 'syscall', 'sysret', 'ud2',
              'hlt', 'cpuid', 'rdtsc', 'rdmsr', 'wrmsr', 'swapgs', 'sysenter',
              'sysexit']
    return lines


HAND_FILES64 = [
    ('00_addressing64', 'i386', 64, gen64_00, 'ModRM/SIB/RIP/disp matrix, REX registers, 0x67'),
    ('02_prefixes64', 'i386', 64, gen64_02, '0x66, REX.W, segment overrides, lock, rep family'),
    ('03_immediates64', 'i386', 64, gen64_03, 'imm64, sign extended imm32, moffs64'),
    ('04_branches64', 'i386', 64, gen64_04, 'jcc/jmp/call/loop/jrcxz, indirect, far'),
]


# ---------------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------------

def write_file(path, file_feature, bits, items):
    with open(path, 'w') as f:
        f.write('; @file-feature: %s\n' % file_feature)
        f.write('; Generated by tests/ia32/tools/gen_corpus.py. Do not edit.\n')
        f.write('bits %d\n' % bits)
        for text, feat in items:
            f.write('%-44s ; @feature: %s\n' % (text, feat))


def generate(out_dir, hand_files, bits, nasm):
    """Writes the hand matrices and the insns.dat feature files of one
    corpus. `bits` is the operating mode of the feature files."""
    global LONG
    LONG = (bits == 64)
    os.makedirs(out_dir, exist_ok=True)
    for old in os.listdir(out_dir):
        if old.endswith('.asm'):
            os.remove(os.path.join(out_dir, old))

    summary = []
    # 1. hand-built matrices
    for name, feat, mbits, gen, desc in hand_files:
        raw = gen()
        # de-duplicate, keep order
        seen = set()
        uniq = []
        for l in raw:
            if l not in seen:
                seen.add(l)
                uniq.append(l)
        good, bad = nasm_check_file_lines(nasm, uniq, mbits)
        if bad:
            sys.stderr.write('%s: %d hand-written lines rejected by nasm: %s\n'
                             % (name, len(bad), '; '.join(bad[:10])))
        write_file(os.path.join(out_dir, name + '.asm'), feat, mbits,
                   [(l, feat) for l in good])
        summary.append((name, len(good), len(bad), 'hand-built: ' + desc))

    # 2. per-feature files from insns.dat
    entries = parse_insns(INSNS) + SYNTH
    drops = collections.Counter()
    form_drops = collections.Counter()
    candidates = []        # (feature, text, entry_index)
    for idx, e in enumerate(entries):
        feat, reason = classify(e)
        if reason:
            drops[reason] += 1
            continue
        cands, why = expand_candidates(e)
        if cands is None:
            drops[why] += 1
            continue
        for c in cands:
            candidates.append((feat, c, idx))

    # Batch-validate all candidate lines with nasm
    texts = [c[1] for c in candidates]
    accepted = nasm_accepts(nasm, texts, 64 if LONG else 32)

    per_feature = collections.OrderedDict()
    seen_text = set()
    accepted_entries = set()
    for (feat, text, idx), ok in zip(candidates, accepted):
        if not ok:
            continue
        norm = ' '.join(text.lower().split())
        accepted_entries.add(idx)
        if norm in seen_text:
            drops['duplicate line'] += 1
            continue
        seen_text.add(norm)
        per_feature.setdefault(feat, []).append((norm, feat))
    for idx, e in enumerate(entries):
        feat, reason = classify(e)
        if reason is None and idx not in accepted_entries:
            form_drops[e['mn']] += 1
    total_forms_rejected = sum(form_drops.values())

    for feat, items in per_feature.items():
        fname = FILE_NAME.get(feat, feat)
        write_file(os.path.join(out_dir, fname + '.asm'), feat, bits, items)
        summary.append((fname, len(items), 0, 'insns.dat feature ' + feat))
    print('== %s (bits %d)' % (out_dir, bits))
    for name, n, bad, desc in summary:
        print('%-18s %6d lines  rejected=%-3d %s' % (name, n, bad, desc))
    print()
    print('insns.dat entries: %d (incl. %d synthetic ALU/shift forms)' % (len(entries), len(SYNTH)))
    print('dropped entries by reason:')
    for reason, n in sorted(drops.items(), key=lambda kv: -kv[1]):
        print('  %-40s %d' % (reason, n))
    print('entries with no nasm-accepted spelling (%d-bit): %d' % (bits, total_forms_rejected))
    print('total generated feature lines: %d' % sum(len(v) for v in per_feature.values()))
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--nasm', default='nasm')
    ap.add_argument('--out', default=DEFAULT_OUT, help='the 16/32 bit corpus directory')
    ap.add_argument('--out64', default=DEFAULT_OUT64, help='the 64 bit corpus directory')
    ap.add_argument('--only', choices=('32', '64'), help='generate one corpus only')
    args = ap.parse_args()
    if args.only != '64':
        generate(args.out, HAND_FILES, 32, args.nasm)
    if args.only != '32':
        generate(args.out64, HAND_FILES64, 64, args.nasm)


if __name__ == '__main__':
    main()
