#!/usr/bin/env python3
"""
IA-32 compatibility driver: dismount vs. nasm (see tests/ia32/TEST_PLAN.md).

  python3 tests/ia32/run_compat.py [--file NAME[,NAME...]] [--features F[,F...]]
          [--grep REGEX] [--host-only] [--corpus 32|64|all] [--cli PATH] [--out DIR]

The corpus is corpus/ (bits 16 and 32) and corpus64/ (bits 64, the lines are
reported as corpus64/NAME.asm); --corpus selects one of them (default: both).

Per corpus file: nasm gives the reference bytes (listing), dismount_cli
assembles every line (ASM) and disassembles nasm's bytes (DIS). Normalized
dismount text is re-assembled by nasm in one batch (REASM). ndisasm decides
whether two different byte strings are the same instruction (ASM=ALT).

Writes OUT/report.json, OUT/summary.md, OUT/failures.txt and prints a table.
Exit status is 0 unless the harness itself breaks.
"""
import argparse
import collections
import glob
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import time

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(TESTS_DIR, '..', '..'))
CORPUS_DIR = os.path.join(TESTS_DIR, 'corpus')
CORPUS64_DIR = os.path.join(TESTS_DIR, 'corpus64')
OPCODE_TABLE = os.path.join(REPO, 'Source', 'dismount', 'proc', 'ia32', 'opcodeTable.cpp')
OPCODE_TABLE_AMD64 = os.path.join(REPO, 'Source', 'dismount', 'proc', 'ia32', 'opcodeTableAMD64.cpp')
DEFAULT_CLI = os.path.join(REPO, 'out', 'bin', 'dismount_cli')
DEFAULT_OUT = os.path.join(REPO, 'out', 'compat')

# Features the host is assumed to have (the i386 baseline and the old cores)
BASELINE = {'i386', 'i486', 'pentium', 'p4', 'p6'}

# Linux names for the sysctl/macOS feature spellings
SYSCTL_MAP = {
    'sse3': 'pni', 'pclmulqdq': 'pclmulqdq', 'ssse3': 'ssse3', 'sse4.1': 'sse4_1',
    'sse4.2': 'sse4_2', 'avx1.0': 'avx', 'clfsh': 'clflush', 'mon': 'monitor',
    'sha': 'sha_ni', 'x2apic': 'x2apic', 'cx16': 'cx16', 'ds': 'ds', 'tm2': 'tm2',
    'pbe': 'pbe', 'acpi': 'acpi', 'htt': 'ht', 'psn': 'psn', 'ss': 'ss', 'tm': 'tm',
    'smx': 'smx', 'vmx': 'vmx', 'dscpl': 'ds_cpl', 'est': 'est', 'cnxtid': 'cnxt_id',
    'tpr': 'tpr', 'pdcm': 'pdcm', 'dtes64': 'dtes64', 'mon': 'monitor',
    'sep': 'sep', 'fpu': 'fpu', 'vme': 'vme', 'de': 'de', 'pse': 'pse', 'tsc': 'tsc',
    'msr': 'msr', 'pae': 'pae', 'mce': 'mce', 'cx8': 'cx8', 'apic': 'apic',
    'mtrr': 'mtrr', 'pge': 'pge', 'mca': 'mca', 'cmov': 'cmov', 'pat': 'pat',
    'pse36': 'pse36', 'mmx': 'mmx', 'fxsr': 'fxsr', 'sse': 'sse', 'sse2': 'sse2',
    'popcnt': 'popcnt', 'aes': 'aes', 'xsave': 'xsave', 'osxsave': 'osxsave',
    'avx': 'avx', 'rdrand': 'rdrand', 'f16c': 'f16c', 'fma': 'fma',
    'movbe': 'movbe', 'pcid': 'pcid', 'fsgsbase': 'fsgsbase', 'bmi1': 'bmi1',
    'avx2': 'avx2', 'smep': 'smep', 'bmi2': 'bmi2', 'erms': 'erms',
    'invpcid': 'invpcid', 'rtm': 'rtm', 'avx512f': 'avx512f', 'rdseed': 'rdseed',
    'adx': 'adx', 'smap': 'smap', 'clflushopt': 'clflushopt', 'clwb': 'clwb',
    'avx512pf': 'avx512pf', 'avx512er': 'avx512er', 'avx512cd': 'avx512cd',
    'avx512dq': 'avx512dq', 'avx512bw': 'avx512bw', 'avx512vl': 'avx512vl',
    'avx512ifma': 'avx512ifma', 'avx512vbmi': 'avx512vbmi', 'sha_ni': 'sha_ni',
}


# ---------------------------------------------------------------------------
# Corpus parsing
# ---------------------------------------------------------------------------

def parse_corpus(path):
    info = {'name': os.path.basename(path), 'file_feature': None, 'bits': 32,
            'lines': []}
    with open(path, encoding='utf-8') as f:
        for no, raw in enumerate(f, 1):
            line = raw.rstrip('\n')
            if not line.strip():
                continue
            code, _, comment = line.partition(';')
            code = code.strip()
            if not code:
                m = re.search(r'@file-feature:\s*(\S+)', comment)
                if m:
                    info['file_feature'] = m.group(1)
                continue
            m = re.match(r'^bits\s+(16|32|64)$', code)
            if m:
                info['bits'] = int(m.group(1))
                continue
            tag = re.search(r'@feature:\s*(\S+)', comment)
            info['lines'].append({
                'no': no, 'text': code,
                'feature': tag.group(1) if tag else None,
                'comment': comment,
            })
    for rec in info['lines']:
        if rec['feature'] is None:
            rec['feature'] = info['file_feature'] or info['name']
    return info


# ---------------------------------------------------------------------------
# nasm reference bytes (listing based, with error-line removal)
# ---------------------------------------------------------------------------

ERR_RE = re.compile(r'^(.*?):(\d+): error: (.*)$', re.M)
LST_RE = re.compile(r'^\s*(\d+)\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)-?\s*$|'
                    r'^\s*(\d+)\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)-?\s')


def nasm_assemble(nasm, texts, bits, tmp):
    """Assembles each text line separately (in one batch). Returns a list of
    (hex, None) for accepted lines and (None, message) for rejected ones."""
    result = [None] * len(texts)
    current = list(range(len(texts)))
    while current:
        src = os.path.join(tmp, 'batch.asm')
        lst = os.path.join(tmp, 'batch.lst')
        binf = os.path.join(tmp, 'batch.bin')
        with open(src, 'w') as f:
            f.write('bits %d\n' % bits)
            for i in current:
                f.write(texts[i] + '\n')
        res = subprocess.run([nasm, '-f', 'bin', '-l', lst, '-o', binf, src],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             universal_newlines=True)
        errs = {}
        for m in ERR_RE.finditer(res.stderr):
            ln = int(m.group(2))
            if ln >= 2:
                errs.setdefault(ln, m.group(3).strip())
        if errs:
            for ln, msg in errs.items():
                if ln - 2 < len(current):
                    result[current[ln - 2]] = (None, msg)
            current = [i for i in current if result[i] is None]
            continue
        if res.returncode != 0:
            raise RuntimeError('nasm failed: ' + res.stderr)
        # Parse listing: source line number -> hex bytes (continuation lines
        # share the source line number and end with '-')
        bytes_by_line = collections.defaultdict(str)
        with open(lst, encoding='utf-8', errors='replace') as f:
            for raw in f:
                m = re.match(r'^\s*(\d+)\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)', raw)
                if m:
                    bytes_by_line[int(m.group(1))] += m.group(3).lower()
        for k, i in enumerate(current):
            result[i] = (bytes_by_line.get(k + 2, ''), None)
        current = []
    return result


# ---------------------------------------------------------------------------
# dismount_cli driver with crash recovery
# ---------------------------------------------------------------------------

def run_cli(cli, mode, bits, inputs):
    outputs = []
    pos = 0
    while pos < len(inputs):
        chunk = inputs[pos:]
        proc = subprocess.run([cli, mode, '--bits', str(bits)],
                              input=''.join(s + '\n' for s in chunk),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True)
        got = proc.stdout.splitlines()
        outputs.extend(got)
        pos += len(got)
        if pos >= len(inputs) and proc.returncode == 0:
            break
        if pos < len(inputs):
            # The CLI died on inputs[pos]; record it and resume after it
            tail = (proc.stderr.strip().splitlines() or [''])[-1][:200]
            outputs.append('ERR CLI aborted (rc=%d) %s' % (proc.returncode, tail))
            pos += 1
    return outputs


# Name-template expansion (see the OpcodeEntry comment in opcodeTable.h):
#   #d  -> 'd' (32 bit) or nothing (16 bit)      pusha#d    -> pusha, pushad
#   #e  -> 'e' (32 bit) or nothing (16 bit)      j#ecxz     -> jecxz, jcxz
#   ##  -> 'd' (32 bit) or 'w' (16 bit)         movs##     -> movsd, movsw
#   a/b -> the first or the second part          cbw/cwde   -> cbw, cwde
#   a/b/c -> the 16, 32 or 64 bit operand size   cbw/cwde/cdqe
# In 64 bit mode '#d' and '##' give 'q' too (pushfq, movsq) and '#e' gives 'r'
# (jrcxz)
# A name is accepted when any of its expansions matches the instruction.

def expand_mnemonic(name):
    variants = {''}
    for part in name.split('/'):
        forms = {part}
        for _ in range(2):
            nxt = set()
            for f in forms:
                if '##' in f:
                    nxt.add(f.replace('##', 'd', 1)); nxt.add(f.replace('##', 'w', 1))
                    nxt.add(f.replace('##', 'q', 1))
                elif '#d' in f:
                    nxt.add(f.replace('#d', 'd', 1)); nxt.add(f.replace('#d', '', 1))
                    nxt.add(f.replace('#d', 'q', 1))
                elif '#e' in f:
                    nxt.add(f.replace('#e', 'e', 1)); nxt.add(f.replace('#e', '', 1))
                    nxt.add(f.replace('#e', 'r', 1))
                else:
                    nxt.add(f)
            forms = nxt
        variants |= forms
    # 'ecxz' and 'cxz' forms are alternatives of the same name, keep them all
    return {v.lower() for v in variants if v}


def table_mnemonics(long_mode=False):
    """The mnemonics of the opcode tables. In 64 bit mode also the ones of
    opcodeTableAMD64.cpp."""
    names = set()
    for path in ((OPCODE_TABLE, OPCODE_TABLE_AMD64) if long_mode else (OPCODE_TABLE,)):
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                for line in f:
                    m = re.match(r'^\s*\{\s*"([^"]+)"\s*,', line)
                    if m:
                        names |= expand_mnemonic(m.group(1))
        except IOError:
            pass
    # The nasm pseudo-ops which spell an imm8 (cmpltps, pclmulhqlqdq) are
    # assembled by the assembler onto cmpps/pclmulqdq
    for sfx in ('ps', 'ss', 'pd', 'sd'):
        if 'cmp' + sfx in names:
            names |= {'cmp' + p + sfx for p in
                      ('eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord')}
    if 'pclmulqdq' in names:
        names |= {'pclmul' + q + 'qdq' for q in ('lql', 'hql', 'lqh', 'hqh')}
    return names


# The features whose instructions are VEX encoded forms of mnemonics which the
# tables have as an unrelated instruction (not implemented, see UNSUPPORTED)
VEX_ONLY_FEATURES = {'msr_imm'}

# The nasm lines which assemble to the opcode without its ModR/M byte ('ud0' is
# 0F FF, 'ud1' is 0F B9): not an instruction, ndisasm says db. The tables have
# ud0 / ud1 with the ModR/M byte only ('ud0 eax, ecx')
OPCODE_ONLY_TEXTS = {'ud0', 'ud1'}

# The instructions which take a size suffix in nasm (see isSizeSuffixed in
# IA32IntelNotation.cpp)
SUFFIXED = {'ret', 'retf', 'iret', 'leave', 'enter', 'pushf', 'popf', 'pusha', 'popa'}

# Prefix words that may lead a line; the mnemonic is the first token after them
PREFIX_WORDS = {'lock', 'rep', 'repe', 'repz', 'repne', 'repnz',
                'cs', 'ds', 'es', 'ss', 'fs', 'gs', 'o16', 'o32', 'a16', 'a32',
                'o64', 'a64'}


def split_prefixes(text):
    """Returns (prefix words, mnemonic, rest of the operands)."""
    toks = text.split()
    prefixes = []
    while len(toks) > 1 and toks[0].lower().rstrip(':') in PREFIX_WORDS:
        prefixes.append(toks.pop(0))
    if not toks:
        return prefixes, '', ''
    return prefixes, toks[0].lower(), ' '.join(toks[1:])


# ---------------------------------------------------------------------------
# ndisasm and text normalization
# ---------------------------------------------------------------------------

_ndis_cache = {}


def ndisasm(ndis, bits, hexbytes, tmp):
    key = (bits, hexbytes)
    if key in _ndis_cache:
        return _ndis_cache[key]
    binf = os.path.join(tmp, 'ndis.bin')
    with open(binf, 'wb') as f:
        f.write(bytes.fromhex(hexbytes))
    res = subprocess.run([ndis, '-b', str(bits), '-o', '0', binf],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True)
    out = None
    for raw in res.stdout.splitlines():
        m = re.match(r'^[0-9A-Fa-f]{8}\s+([0-9A-Fa-f]+)\s+(.*)$', raw)
        if m:
            out = (len(m.group(1)) // 2, m.group(2).strip())
            break
    _ndis_cache[key] = out
    return out


REG_SEG = ('es', 'cs', 'ss', 'ds', 'fs', 'gs')
BRANCH_RE = re.compile(r'^(j[a-z]*|call|loop[a-z]*|xbegin)$')


def _norm_mem(expr, keep_order=False):
    seg = ''
    m = re.match(r'^(es|cs|ss|ds|fs|gs):(.*)$', expr)
    if m:
        seg, expr = m.group(1) + ':', m.group(2)
    # The keywords of nasm inside the brackets (64 bit mode): [rel $+N] is
    # rip relative, a32 selects the 32 bit address, qword a 64 bit offset
    keywords = ''
    while True:
        m = re.match(r'^\s*(rel|a32|a64|qword)\s+(.*)$', expr)
        if not m:
            break
        keywords += m.group(1) + ' '
        expr = m.group(2)
    expr = expr.replace(' ', '')
    regs = []
    disp = 0
    for term in re.findall(r'[+-]?[^+-]+', expr):
        sign = -1 if term.startswith('-') else 1
        body = term.lstrip('+-')
        if re.match(r'^(0x[0-9a-f]+|[0-9]+)$', body):
            disp += sign * int(body, 0)
        else:
            regs.append(body)
    # Sorting makes the text order-insensitive, which is right for the TEXT
    # comparison. A SIB byte has a base and an index, so the reassembly keeps
    # the order the disassembler printed.
    if not keep_order:
        regs.sort()
    out = '+'.join(regs)
    if disp or not regs:
        if regs:
            out += ('-0x%x' % -disp) if disp < 0 else ('+0x%x' % disp)
        else:
            out = ('-0x%x' % -disp) if disp < 0 else '0x%x' % disp
    return seg + keywords + out


def norm_text(text, keep_order=False):
    """Mechanical normalization from TEST_PLAN "Text normalization". It only
    rewrites the spellings listed there and never adds or removes meaning:
    the size keywords short/near are kept as printed."""
    s = text.strip().lower()
    s = re.sub(r'\s*;.*$', '', s)
    s = re.sub(r'\b(byte|word|dword|qword|tword|oword|yword|zword)\s+ptr\b', r'\1', s)
    s = re.sub(r'\bptr\b', '', s)
    s = re.sub(r'\b(es|cs|ss|ds|fs|gs):\[', r'[\1:', s)
    # The formatter writes hex as 2, 4 or 8 digits and an 'h' (FFh, 0Ah,
    # 00000010h). One digit before the h would be the register ah, bh, ch or
    # dh, so those stay registers. nasm needs a leading digit (0FFh) to read
    # the number, which the 0x form also gives.
    s = re.sub(r'\b([0-9a-f]{2,})h\b', lambda m: '0x' + m.group(1), s)
    s = re.sub(r'\[([^\]]*)\]', lambda m: '[' + _norm_mem(m.group(1), keep_order) + ']', s)
    s = re.sub(r'\s+', ' ', s).strip()
    m = re.match(r'^([a-z0-9_]+)\s+(.*)$', s)
    mn, ops = (m.group(1), m.group(2)) if m else (s, '')
    if BRANCH_RE.match(mn):
        # An absolute target (address 0 based) becomes the nasm $+N form
        m2 = re.match(r'^((?:short|near) )?(0x[0-9a-f]+|[0-9]+)$', ops)
        if m2:
            kw = m2.group(1) or ''
            target = int(m2.group(2), 0) & 0xffffffff
            if target >= 0x80000000:
                ops = kw + '$-0x%x' % (0x100000000 - target)
            else:
                ops = kw + '$+0x%x' % target
    s = (mn + ' ' + ops).strip()
    s = re.sub(r'\s*,\s*', ', ', s)
    return s


def reasm_text(dis_text):
    """Text handed to nasm for the reassembly check: the normalized
    disassembly and nothing else. The branch size is whatever the
    disassembler printed (see the 'near' rule in IA32IntelNotation)."""
    return norm_text(dis_text, keep_order=True)


def selftest():
    """Asserts for the normalization. Run at start-up and by --selftest."""
    cases = [
        ('mov al, ah', 'mov al, ah'),
        ('mov bh, 0ah', 'mov bh, 0x0a'),
        ('mov ch, dh', 'mov ch, dh'),
        ('add eax, 0ffh', 'add eax, 0x0ff'),
        ('or al, FFh', 'or al, 0xff'),
        ('or eax, -01h', 'or eax, -0x01'),
        ('mov dh, 0Ah', 'mov dh, 0x0a'),
        ('mov eax, dword ptr [esi + 7Fh]', 'mov eax, dword [esi+0x7f]'),
        ('jo near $+10h', 'jo near $+0x10'),
        ('jo short $+10h', 'jo short $+0x10'),
        ('jo 0x12', 'jo $+0x12'),
    ]
    for source, want in cases:
        got = norm_text(source)
        assert got == want, 'norm_text(%r) = %r, want %r' % (source, got, want)
    assert reasm_text('jo near $+10h') == 'jo near $+0x10'
    assert reasm_text('jo $+10h') == 'jo $+0x10'


selftest()


# The lines which the assembler must reject in 32 bit mode (the malformed
# lines, and the 64 bit instructions) and in 64 bit mode (the instructions that
# are invalid in 64 bit mode, the registers that REX can't reach, ...)
ASM_ERRORS = [(os.path.join(TESTS_DIR, 'asm_errors.txt'), 32),
              (os.path.join(TESTS_DIR, 'asm_errors64.txt'), 64)]


def check_asm_errors(cli):
    """Every malformed line of asm_errors.txt (asm_errors64.txt in 64 bit mode)
    must be rejected with 'ERR <message>' and the CLI must survive all of
    them. Returns the number of lines checked."""
    total = 0
    for path, bits in ASM_ERRORS:
        with open(path) as f:
            lines = [line.rstrip('\n') for line in f if line.strip()]
        outputs = run_cli(cli, 'asm', bits, lines)
        bad = []
        if len(outputs) != len(lines):
            bad.append('expected %d outputs, got %d' % (len(lines), len(outputs)))
        for line, out in zip(lines, outputs):
            ok = out.startswith('ERR ') and out[4:].strip() != '' and 'CLI aborted' not in out
            if not ok:
                bad.append('%r -> %r' % (line[:80], out[:120]))
        assert not bad, '%s: %d line(s) not rejected:\n  %s' % (
            os.path.basename(path), len(bad), '\n  '.join(bad))
        total += len(lines)
    return total


# ---------------------------------------------------------------------------
# Host features
# ---------------------------------------------------------------------------

def host_features(out_dir):
    """Returns (set of feature names, description)."""
    if platform.system() == 'Darwin':
        probe = os.path.join(out_dir, 'cpuprobe')
        try:
            subprocess.run(['clang', '-arch', 'x86_64', '-O1', '-o', probe,
                            os.path.join(TESTS_DIR, 'cpuprobe.c')],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            res = subprocess.run([probe], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, universal_newlines=True,
                                 timeout=60)
            line = res.stdout.strip()
            if res.returncode == 0 and line and not line.startswith('none'):
                return set(line.split()), 'cpuprobe (x86_64 build, Rosetta on arm64)'
        except (OSError, subprocess.SubprocessError):
            pass
        try:
            res = subprocess.run(['sysctl', '-n', 'machdep.cpu.features',
                                  'machdep.cpu.leaf7_features'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 universal_newlines=True)
            if res.returncode == 0 and res.stdout.strip():
                names = set()
                for tok in res.stdout.replace('\n', ' ').split():
                    low = tok.lower()
                    names.add(SYSCTL_MAP.get(low, low))
                return names, 'sysctl machdep.cpu.*'
        except OSError:
            pass
    try:
        with open('/proc/cpuinfo', encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('flags'):
                    return set(line.split(':', 1)[1].split()), '/proc/cpuinfo'
    except IOError:
        pass
    return set(), 'unknown (no CPUID probe available)'


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def check_file(info, args, ctx, tmp):
    bits = info['bits']
    lines = info['lines']
    if args.features:
        lines = [r for r in lines if r['feature'] in args.features]
    if args.grep:
        pat = re.compile(args.grep)
        lines = [r for r in lines if pat.search(r['text'])]
    if args.host_only:
        lines = [r for r in lines if ctx['host_ok'](r['feature'])]
    if not lines:
        return []

    mnemonics = ctx['mnemonics64'] if bits == 64 else ctx['mnemonics']
    texts = [r['text'] for r in lines]
    for r in lines:
        _, r['mn'], _ = split_prefixes(r['text'])
        # nasm size suffixes (retw, iretd, enterw, pushfq) spell a table mnemonic
        bare = r['mn'][:-1] if r['mn'][-1:] in ('w', 'd', 'q') and \
            r['mn'][:-1] in SUFFIXED else ''
        r['supported'] = (not mnemonics) or r['mn'] in mnemonics or \
            (bare != '' and bare in mnemonics)
        # The VEX encoded forms of mnemonics which the tables have as another
        # instruction ('rdmsr rbx, imm32' against the plain rdmsr)
        if r['feature'] in VEX_ONLY_FEATURES:
            r['supported'] = False
        if r['text'].strip() in OPCODE_ONLY_TEXTS:
            r['supported'] = False
        r['host'] = ctx['host_ok'](r['feature'])
    ref = nasm_assemble(ctx['nasm'], texts, bits, tmp)
    recs = []
    supported = []
    for r, (hexb, err) in zip(lines, ref):
        rec = {'file': info['name'], 'line': r['no'], 'source': r['text'],
               'feature': r['feature'], 'host': 'yes' if r['host'] else 'no',
               'nasm_hex': hexb, 'nasm_error': err,
               'asm': {'status': None, 'hex': None, 'error': None},
               'dis': {'status': None, 'len': None, 'text': None, 'error': None},
               'text': {'status': 'NA'},
               'rt': {'status': None, 'hex': None, 'error': None}}
        if err is not None:
            rec['asm']['status'] = 'ERROR'
            rec['asm']['error'] = 'nasm: ' + err
            rec['dis']['status'] = 'ERROR'
            rec['dis']['error'] = 'nasm rejected the line'
        elif not r['supported']:
            # Not implemented: no mnemonic in any table, so neither direction runs
            rec['asm']['status'] = 'UNSUPPORTED'
            rec['dis']['status'] = 'UNSUPPORTED'
        else:
            supported.append(rec)
        recs.append(rec)
    if not supported:
        return recs

    # ASM: dismount assembles the source text
    asm_out = run_cli(ctx['cli'], 'asm', bits, [r['source'] for r in supported])
    for rec, out in zip(supported, asm_out):
        if out.startswith('OK '):
            rec['asm']['hex'] = out[3:].strip()
            if rec['asm']['hex'] == rec['nasm_hex']:
                rec['asm']['status'] = 'PASS'
            else:
                # ALT: both byte strings are one instruction (ndisasm consumes
                # all of each) and decode to the same text. Lengths may differ.
                alt = False
                nd_nasm = ndisasm(ctx['ndisasm'], bits, rec['nasm_hex'], tmp)
                nd_dis = ndisasm(ctx['ndisasm'], bits, rec['asm']['hex'], tmp) \
                    if rec['asm']['hex'] else None
                if nd_nasm and nd_dis and nd_nasm[0] == len(rec['nasm_hex']) // 2 \
                        and nd_dis[0] == len(rec['asm']['hex']) // 2 \
                        and nd_nasm[1] == nd_dis[1]:
                    alt = True
                rec['asm']['status'] = 'ALT' if alt else 'MISMATCH'
                if not alt:
                    rec['asm']['error'] = 'bytes differ'
        else:
            rec['asm']['status'] = 'ERROR'
            rec['asm']['error'] = out[4:] if out.startswith('ERR ') else out

    # DIS: dismount decodes nasm's bytes
    dis_recs = [rec for rec in supported if rec['nasm_error'] is None]
    dis_out = run_cli(ctx['cli'], 'dis', bits, [r['nasm_hex'] for r in dis_recs])
    reasm_idx = []
    for rec, out in zip(dis_recs, dis_out):
        m = re.match(r'^(OK|INVALID) (\d+) ?(.*)$', out)
        if m:
            rec['dis']['len'] = int(m.group(2))
            rec['dis']['text'] = m.group(3).rstrip()
            nasm_len = len(rec['nasm_hex']) // 2
            if m.group(1) == 'INVALID':
                # Not implemented (mnemonic missing from the tables) vs. invalid
                rec['dis']['status'] = 'INVALID'
            elif rec['dis']['len'] != nasm_len:
                rec['dis']['status'] = 'LENGTH'
                rec['dis']['error'] = 'decoded %d bytes, nasm %d' % (rec['dis']['len'], nasm_len)
            else:
                reasm_idx.append(rec)
                rec['dis']['status'] = 'PENDING'
            # The size keyword is informative only here: dismount prints
            # 'near' when it matters, and never 'short'
            text_key = lambda t: norm_text(t).replace(' short ', ' ').replace(' near ', ' ')
            rec['text']['status'] = 'SAME' if text_key(rec['source']) == text_key(rec['dis']['text']) else 'DIFF'
        else:
            rec['dis']['status'] = 'ERROR'
            rec['dis']['error'] = out[4:] if out.startswith('ERR ') else out

    # REASM: nasm re-assembles the normalized dismount text in one batch
    if reasm_idx:
        texts = [reasm_text(r['dis']['text']) for r in reasm_idx]
        re_out = nasm_assemble(ctx['nasm'], texts, bits, tmp)
        for rec, (hexb, err) in zip(reasm_idx, re_out):
            rec['dis']['reasm_text'] = None
            if err is not None:
                rec['dis']['status'] = 'REASM_DIFF'
                rec['dis']['error'] = 'nasm: ' + err
            elif hexb == rec['nasm_hex']:
                rec['dis']['status'] = 'PASS'
            else:
                rec['dis']['status'] = 'REASM_DIFF'
                rec['dis']['error'] = 'reassembled %s' % hexb
    for rec in recs:
        if rec['dis']['status'] == 'PENDING':
            rec['dis']['status'] = 'ERROR'

    # RT (round trip): dismount assembles its own disassembly of nasm's bytes
    # (the same text nasm got for REASM), and must write nasm's bytes again.
    # Only the lines with DIS=PASS take part: for them nasm's bytes are the
    # reference of the text, too
    rt_recs = [rec for rec in dis_recs if rec['dis']['status'] == 'PASS']
    if rt_recs:
        rt_out = run_cli(ctx['cli'], 'asm', bits,
                         [reasm_text(r['dis']['text']) for r in rt_recs])
        for rec, out in zip(rt_recs, rt_out):
            if out.startswith('OK '):
                rec['rt']['hex'] = out[3:].strip()
                if rec['rt']['hex'] == rec['nasm_hex']:
                    rec['rt']['status'] = 'PASS'
                else:
                    rec['rt']['status'] = 'MISMATCH'
                    rec['rt']['error'] = 'bytes differ'
            else:
                rec['rt']['status'] = 'ERROR'
                rec['rt']['error'] = out[4:] if out.startswith('ERR ') else out
    return recs


def summarize_and_write(recs, out_dir, args, host_source, files):
    os.makedirs(out_dir, exist_ok=True)
    totals = collections.Counter()
    feat_stats = collections.OrderedDict()
    for rec in recs:
        rec_feature = rec['feature'] + (' [64]' if rec['file'].startswith('corpus64/') else '')
        totals['asm:' + rec['asm']['status']] += 1
        totals['dis:' + (rec['dis']['status'] or 'NONE')] += 1
        if rec['rt']['status']:
            totals['rt:' + rec['rt']['status']] += 1
        if rec['text']['status'] in ('SAME', 'DIFF'):
            totals['text:' + rec['text']['status']] += 1
        s = feat_stats.setdefault(rec_feature, collections.Counter())
        s['lines'] += 1
        s['asm:' + rec['asm']['status']] += 1
        s['dis:' + (rec['dis']['status'] or 'NONE')] += 1
        s['host:' + rec['host']] += 1
        if rec['asm']['status'] == 'PASS' and rec['dis']['status'] == 'PASS':
            s['full'] += 1
    report = {
        'generated': time.strftime('%Y-%m-%d %H:%M:%S'),
        'host_features_source': host_source,
        'files': files,
        'totals': dict(totals),
        'lines': recs,
    }
    with open(os.path.join(out_dir, 'report.json'), 'w') as f:
        json.dump(report, f, indent=1)

    # failures.txt
    with open(os.path.join(out_dir, 'failures.txt'), 'w') as f:
        for rec in recs:
            a, d = rec['asm']['status'], rec['dis']['status']
            if a in ('PASS', 'ALT') and d == 'PASS':
                continue
            if a == 'ALT' and d in (None, 'PASS'):
                continue
            dis_part = rec['dis']['text'] if rec['dis']['text'] is not None else ''
            reason = '; '.join(x for x in ('ASM: ' + rec['asm']['error'] if rec['asm']['error'] else '',
                                           'DIS: ' + rec['dis']['error'] if rec['dis']['error'] else '')
                               if x)
            f.write('%s:%d | %s | nasm=%s | dismount=%s / %s | ASM=%s DIS=%s | %s\n' % (
                rec['file'], rec['line'], rec['source'], rec['nasm_hex'],
                rec['asm']['hex'] or '-', dis_part, a, d, reason))

    # rt_failures.txt: the round trip (dismount's text -> dismount's bytes)
    with open(os.path.join(out_dir, 'rt_failures.txt'), 'w') as f:
        for rec in recs:
            if rec['rt']['status'] in (None, 'PASS'):
                continue
            f.write('%s:%d | %s | nasm=%s | dismount text: %s | dismount=%s | RT=%s | %s\n' % (
                rec['file'], rec['line'], rec['source'], rec['nasm_hex'],
                rec['dis']['text'], rec['rt']['hex'] or '-', rec['rt']['status'],
                rec['rt']['error'] or ''))

    # summary.md
    cols = ['asm:PASS', 'asm:ALT', 'asm:MISMATCH', 'asm:ERROR', 'asm:UNSUPPORTED',
            'dis:PASS', 'dis:LENGTH', 'dis:REASM_DIFF', 'dis:INVALID', 'dis:ERROR',
            'dis:UNSUPPORTED']
    lines_md = ['# IA-32 compatibility summary', '',
                'Generated %s. Host features from: %s.' % (report['generated'], host_source),
                '', 'Lines: %d. Fully compatible (ASM=PASS and DIS=PASS): %d.' % (
                    len(recs), sum(1 for r in recs if r['asm']['status'] == 'PASS'
                                   and r['dis']['status'] == 'PASS')),
                '']
    header = ['feature', 'lines', 'host'] + [c.replace('asm:', 'ASM ').replace('dis:', 'DIS ') for c in cols] + ['full']
    lines_md.append('| ' + ' | '.join(header) + ' |')
    lines_md.append('|' + '---|' * len(header))
    for feat, s in feat_stats.items():
        row = [feat, str(s['lines']), 'yes' if s['host:yes'] and not s['host:no'] else
               ('mixed' if s['host:yes'] else 'no')]
        row += [str(s.get(c, 0)) for c in cols] + [str(s.get('full', 0))]
        lines_md.append('| ' + ' | '.join(row) + ' |')
    lines_md.append('')
    lines_md.append('## Top failure clusters (mnemonic)')
    lines_md.append('')
    clusters = collections.Counter()
    sample = {}
    for rec in recs:
        for kind, st in (('ASM', rec['asm']['status']), ('DIS', rec['dis']['status'])):
            if st in (None, 'PASS', 'ALT'):
                continue
            key = (kind, st, rec['source'].split()[0].lower() if rec['source'] else '?')
            clusters[key] += 1
            sample.setdefault(key, rec['source'])
    lines_md.append('| check | status | mnemonic | count | example |')
    lines_md.append('|---|---|---|---|---|')
    for (kind, st, mn), n in clusters.most_common(40):
        lines_md.append('| %s | %s | %s | %d | `%s` |' % (kind, st, mn, n, sample[(kind, st, mn)]))
    lines_md.append('')
    with open(os.path.join(out_dir, 'summary.md'), 'w') as f:
        f.write('\n'.join(lines_md) + '\n')
    return totals


def main():
    ap = argparse.ArgumentParser(description='dismount vs nasm IA-32 compatibility run')
    ap.add_argument('--file', action='append', default=[],
                    help='corpus file (name or path, repeatable or comma-separated)')
    ap.add_argument('--features', default='', help='comma-separated feature tags')
    ap.add_argument('--grep', default='', help='regex on the instruction text')
    ap.add_argument('--host-only', action='store_true')
    ap.add_argument('--corpus', choices=('32', '64', 'all'), default='all',
                    help='32: corpus/ (bits 16 and 32), 64: corpus64/ (bits 64)')
    ap.add_argument('--cli', default=DEFAULT_CLI)
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--nasm', default='nasm')
    ap.add_argument('--ndisasm', default='ndisasm')
    ap.add_argument('--selftest', action='store_true', help='run the normalization asserts and exit')
    ap.add_argument('--strict', action='store_true',
                    help='exit 1 if a line that dismount implements is not ASM PASS and DIS PASS '
                         '(and RT PASS for the 64 bit corpus)')
    ap.add_argument('--strict-asm64', action='store_true',
                    help='no effect: --strict checks the assembler of the 64 bit corpus')
    args = ap.parse_args()
    if args.selftest:
        if not os.path.exists(args.cli):
            sys.stderr.write('dismount_cli not found: %s\n' % args.cli)
            return 1
        count = check_asm_errors(os.path.abspath(args.cli))
        print('selftest OK (%d asm error lines rejected)' % count)
        return 0
    args.features = set(f for f in args.features.split(',') if f)
    wanted = set()
    for item in args.file:
        for part in item.split(','):
            if part:
                wanted.add(os.path.basename(part))
                wanted.add(os.path.basename(part) + '.asm')

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    host, host_source = host_features(out_dir)
    host_ok = lambda feat: feat in BASELINE or feat in host
    ctx = {'nasm': args.nasm, 'ndisasm': args.ndisasm, 'cli': os.path.abspath(args.cli),
           'host_ok': host_ok, 'mnemonics': table_mnemonics(),
           'mnemonics64': table_mnemonics(True)}
    if not os.path.exists(ctx['cli']):
        sys.stderr.write('dismount_cli not found: %s (build with -DDISMOUNT_BUILD_TESTS=ON)\n'
                         % ctx['cli'])
        return 1

    paths = []
    if args.corpus in ('32', 'all'):
        paths += sorted(glob.glob(os.path.join(CORPUS_DIR, '*.asm')))
    if args.corpus in ('64', 'all'):
        paths += sorted(glob.glob(os.path.join(CORPUS64_DIR, '*.asm')))
    if wanted:
        paths = [p for p in paths if os.path.basename(p) in wanted]
    all_recs = []
    files = {}
    if args.host_only and not host:
        sys.stderr.write('warning: no host feature set found, --host-only keeps baseline only\n')
    with tempfile.TemporaryDirectory() as tmp:
        for path in paths:
            info = parse_corpus(path)
            if os.path.dirname(path) == CORPUS64_DIR:
                # The files of both corpora have the same names
                info['name'] = 'corpus64/' + info['name']
            recs = check_file(info, args, ctx, tmp)
            all_recs.extend(recs)
            files[info['name']] = {'bits': info['bits'], 'lines': len(recs)}
            c = collections.Counter(r['asm']['status'] for r in recs)
            sys.stderr.write('  %-22s %6d lines  ASM PASS=%d ALT=%d\n' % (
                info['name'], len(recs), c.get('PASS', 0), c.get('ALT', 0)))
    totals = summarize_and_write(all_recs, out_dir, args, host_source, files)

    print('dismount vs nasm: %d lines in %d files (host features: %s, %d flags)' % (
        len(all_recs), len(paths), host_source, len(host)))
    rows = [('ASM', ['PASS', 'ALT', 'MISMATCH', 'ERROR', 'UNSUPPORTED']),
            ('DIS', ['PASS', 'LENGTH', 'REASM_DIFF', 'INVALID', 'ERROR', 'UNSUPPORTED'])]
    for kind, sts in rows:
        print('  %s: ' % kind + '  '.join('%s=%d' % (s, totals.get('%s:%s' % (kind.lower(), s), 0))
                                         for s in sts))
    print('  RT (round trip: dismount text -> dismount bytes == nasm bytes): PASS=%d  MISMATCH=%d  ERROR=%d' % (
        totals.get('rt:PASS', 0), totals.get('rt:MISMATCH', 0), totals.get('rt:ERROR', 0)))
    print('  TEXT: SAME=%d DIFF=%d' % (totals.get('text:SAME', 0), totals.get('text:DIFF', 0)))
    print('  fully compatible (ASM=PASS and DIS=PASS): %d' % sum(
        1 for r in all_recs if r['asm']['status'] == 'PASS' and r['dis']['status'] == 'PASS'))
    print('reports: %s/{report.json,summary.md,failures.txt,rt_failures.txt}' % out_dir)
    if args.strict:
        # UNSUPPORTED lines are not implemented yet; every other line must be
        # exact, and the round trip of the lines of the 64 bit corpus too
        failed = [r for r in all_recs
                  if r['asm']['status'] not in ('PASS', 'UNSUPPORTED') or
                  r['dis']['status'] not in ('PASS', 'UNSUPPORTED') or
                  (r['file'].startswith('corpus64/') and
                   r['rt']['status'] not in (None, 'PASS'))]
        if failed:
            print('strict: %d lines failed, see %s/failures.txt' % (len(failed), out_dir))
            return 1
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
