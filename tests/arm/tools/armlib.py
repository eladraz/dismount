#!/usr/bin/env python3
"""Shared code of the ARM compatibility tools (tests/arm/TEST_PLAN.md).

The oracles are clang (the integrated assembler) and objdump (the LLVM
disassembler), the system under test is out/bin/dismount_cli. Python 3, no
third party packages.
"""
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
CLI = os.path.join(REPO, 'out', 'bin', 'dismount_cli')

# The address of the first instruction of a listing: the pc relative operands
# print the absolute addresses, so both sides use the same base
BASE = 0x10000

TRIPLES = {
    'a64': 'aarch64-none-elf',
    'a32': 'armv8a-none-eabi',
    't32': 'thumbv8a-none-eabi',
}


def run(cmd, input_text=None):
    return subprocess.run(cmd, input=input_text, capture_output=True, text=True)


# ---------------------------------------------------------------------------
# Text

_COMMENT = re.compile(r'(//|\s@\s).*$')
_SYMBOL = re.compile(r'\s*<[^>]*>\s*$')


def normalize(text):
    """The text of an instruction as it is compared: no comment, no symbol of
    a branch target, the blanks collapsed."""
    if text.strip() == '<unknown>':
        return '<unknown>'
    text = _COMMENT.sub('', text)
    text = _SYMBOL.sub('', text)
    return ' '.join(text.split())


# ---------------------------------------------------------------------------
# objdump

_LINE = re.compile(r'^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)$')


# The features which objdump needs to decode everything dismount knows of
# AArch32 (the architecture of the object is not enough)
OBJDUMP_MATTR = {
    'a32': '+v8,+neon,+fp-armv8,+crypto,+crc,+dotprod,+fullfp16,+fp16fml,+i8mm,+bf16,+sb,+ras,'
           '+virtualization,+trustzone,+mp,+dsp,+hwdiv,+hwdiv-arm,+aes,+sha2,+v8.4a,+d32',
}
OBJDUMP_MATTR['t32'] = OBJDUMP_MATTR['a32']


def objdump_words(words, arch='a64', base=BASE, mattr=None):
    """Disassemble 32 bit words (A64, A32) placed one after the other from
    'base'. Return the normalized texts, '<unknown>' for the invalid words."""
    if not words:
        return []
    tmp = tempfile.mkdtemp(prefix='armobjdump')
    try:
        src = os.path.join(tmp, 'a.s')
        obj = os.path.join(tmp, 'a.o')
        with open(src, 'w') as f:
            if arch == 'a32':
                f.write('.arm\n')
            for word in words:
                f.write('.inst 0x%08x\n' % word)
        cmd = ['clang', '-target', TRIPLES[arch], '-c', src, '-o', obj]
        r = run(cmd)
        if r.returncode != 0:
            raise RuntimeError('clang failed: ' + r.stderr[:500])
        cmd = ['objdump', '-d', '--adjust-vma=0x%x' % base, '--disassemble-zeroes', obj]
        if mattr is None:
            mattr = OBJDUMP_MATTR.get(arch)
        if mattr:
            cmd.insert(2, '--mattr=' + mattr)
        r = run(cmd)
        if r.returncode != 0:
            raise RuntimeError('objdump failed: ' + r.stderr[:500])
        texts = []
        for line in r.stdout.splitlines():
            m = _LINE.match(line)
            if m:
                texts.append(normalize(m.group(3)))
        if len(texts) != len(words):
            raise RuntimeError('objdump gave %d instructions for %d words'
                               % (len(texts), len(words)))
        return texts
    finally:
        for name in os.listdir(tmp):
            os.unlink(os.path.join(tmp, name))
        os.rmdir(tmp)


def objdump_chunks(words, arch='a64', mattr=None, chunk=100000):
    """objdump_words() of a long list, in chunks (the addresses restart at
    BASE in each chunk). Return (texts, addresses)."""
    texts = []
    addresses = []
    for start in range(0, len(words), chunk):
        part = words[start:start + chunk]
        texts.extend(objdump_words(part, arch, BASE, mattr))
        addresses.extend(BASE + 4 * i for i in range(len(part)))
    return texts, addresses


# ---------------------------------------------------------------------------
# T32: instruction units. A unit is (size, value): size 2 and the halfword, or
# size 4 and hw1 << 16 | hw2 (the bit numbers of the Arm ARM). The first
# halfword decides the size, as it does in the processor and in objdump.

def t32_size(hw1):
    return 4 if (hw1 >> 11) >= 0x1d else 2


def t32_unit(value32):
    """The unit that starts with the halfword hw1 of a 32 bit random value."""
    hw1 = value32 >> 16
    if t32_size(hw1) == 4:
        return (4, value32 & 0xffffffff)
    return (2, hw1)


def t32_is_it(unit):
    """True for an it instruction (0xbf00 with a mask): the state carries over
    to the next instructions, so the random streams don't have them."""
    return unit[0] == 2 and (unit[1] & 0xff00) == 0xbf00 and (unit[1] & 0xf) != 0


def t32_hex(unit):
    size, value = unit
    if size == 2:
        return '%04x' % value
    return '%04x %04x' % (value >> 16, value & 0xffff)


_THUMB_LINE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{4}\s)+)\s*(.*)$')


def objdump_units(units, mattr=None, base=BASE, arch='t32'):
    """Disassemble a stream of T32 units with objdump (the it state carries
    over the units). Return (texts, addresses, sizes) with a text per unit."""
    if not units:
        return [], [], []
    tmp = tempfile.mkdtemp(prefix='armobjdump')
    try:
        src = os.path.join(tmp, 'a.s')
        obj = os.path.join(tmp, 'a.o')
        with open(src, 'w') as f:
            f.write('.thumb\n')
            for size, value in units:
                if size == 2:
                    f.write('.inst.n 0x%04x\n' % value)
                else:
                    f.write('.inst.w 0x%08x\n' % value)
        r = run(['clang', '-target', TRIPLES['t32'], '-c', src, '-o', obj])
        if r.returncode != 0:
            raise RuntimeError('clang failed: ' + r.stderr[:500])
        cmd = ['objdump', '-d', '--adjust-vma=0x%x' % base, '--disassemble-zeroes', obj]
        if mattr is None:
            mattr = OBJDUMP_MATTR.get(arch)
        if mattr:
            cmd.insert(2, '--mattr=' + mattr)
        r = run(cmd)
        if r.returncode != 0:
            raise RuntimeError('objdump failed: ' + r.stderr[:500])
        texts = []
        addresses = []
        sizes = []
        for line in r.stdout.splitlines():
            m = _THUMB_LINE.match(line)
            if m:
                groups = m.group(2).split()
                addresses.append(int(m.group(1), 16))
                sizes.append(2 * len(groups))
                texts.append(normalize(m.group(3)))
        if len(texts) != len(units) or sizes != [u[0] for u in units]:
            raise RuntimeError('objdump gave %d instructions for %d units' % (len(texts), len(units)))
        return texts, addresses, sizes
    finally:
        for name in os.listdir(tmp):
            os.unlink(os.path.join(tmp, name))
        os.rmdir(tmp)


def objdump_unit_chunks(units, mattr=None, chunk=50000):
    texts = []
    addresses = []
    for start in range(0, len(units), chunk):
        part = units[start:start + chunk]
        t, a, _ = objdump_units(part, mattr)
        texts.extend(t)
        addresses.extend(a)
    return texts, addresses


def dis_units(units, mattr=None, addresses=None):
    """Disassemble T32 units with dismount, one unit per line. Return
    (status, text) tuples as dis_words()."""
    args = ['dis', '--arch', 't32']
    if mattr:
        args.extend(['--mattr', mattr])
    lines = []
    for i, unit in enumerate(units):
        address = addresses[i] if addresses else BASE + 4 * i
        lines.append('@%x %s' % (address, t32_hex(unit)))
    return _parse_dis(cli_lines(args, lines))


def dis_sequences(sequences, mattr=None, base=BASE):
    """Disassemble sequences of T32 units, each one by one disassembler (the it
    state is kept over a sequence). Return a list with, for each sequence, the
    list of the texts of its instructions (None if one is invalid)."""
    args = ['dis', '--arch', 't32']
    if mattr:
        args.extend(['--mattr', mattr])
    lines = []
    address = base
    for i, seq in enumerate(sequences):
        lines.append('@%x %s' % (address, ' ; '.join(t32_hex(u) for u in seq)))
        address += sum(u[0] for u in seq)
    result = []
    for line in cli_lines(args, lines):
        if line.startswith('OK '):
            texts = line.split(' ', 2)[2]
            result.append([normalize(t) for t in texts.split(' ; ')])
        else:
            result.append(None)
    return result


# ---------------------------------------------------------------------------
# clang

_ERROR = re.compile(r'^[^:\n]+:(\d+):\d+: error: (.*)$')


def llvm_features(arch='a64'):
    """The LLVM feature names of dismount's support matrix."""
    names = []
    for _index, name, _isa, _support, _entries in features(arch):
        if name != '-' and name not in names:
            names.append(name)
    return names


def clang_assemble(lines, arch='a64', features_on=None, triple=None):
    """Assemble instruction lines with the integrated assembler.

    features_on is the list of the LLVM features to enable (default: all of
    the matrix). Return a list with, for each line, ('ok', bytes) or
    ('err', message). Lines which expand to several instructions are errors
    of the corpus: they would shift the others, so they are assembled one by
    one."""
    if features_on is None:
        features_on = llvm_features(arch)
    if arch in ('a32', 't32'):
        return _clang_assemble_labeled(lines, arch, features_on, triple)
    results = [None] * len(lines)
    pending = list(range(len(lines)))
    tmp = tempfile.mkdtemp(prefix='armclang')
    try:
        for _attempt in range(50):
            if not pending:
                break
            src = os.path.join(tmp, 'a.s')
            obj = os.path.join(tmp, 'a.o')
            with open(src, 'w') as f:
                if arch == 'a32':
                    f.write('.arm\n.syntax unified\n')
                elif arch == 't32':
                    f.write('.thumb\n.syntax unified\n')
                for index in pending:
                    f.write(lines[index] + '\n')
            cmd = ['clang', '-cc1as', '-triple', TRIPLES[arch], '-filetype', 'obj',
                   src, '-o', obj]
            for name in features_on:
                cmd.extend(['-target-feature', '+' + name])
            r = run(cmd)
            if r.returncode == 0:
                data = _text_bytes(obj)
                if arch == 'a64':
                    if len(data) != 4 * len(pending):
                        # Pseudo instructions: assemble one by one
                        for index in pending:
                            results[index] = clang_assemble_one(lines[index], arch, features_on)
                        pending = []
                        break
                    for k, index in enumerate(pending):
                        results[index] = ('ok', data[4 * k:4 * k + 4])
                else:
                    # AArch32: sizes vary, one by one
                    for index in pending:
                        results[index] = clang_assemble_one(lines[index], arch, features_on)
                pending = []
                break
            # The lines with an error: report them and assemble the others
            offset = 2 if arch in ('a32', 't32') else 0
            bad = {}
            for err_line in r.stderr.splitlines():
                m = _ERROR.match(err_line)
                if m:
                    position = int(m.group(1)) - 1 - offset
                    if 0 <= position < len(pending) and position not in bad:
                        bad[position] = m.group(2)
            if not bad:
                for index in pending:
                    results[index] = ('err', r.stderr.strip().splitlines()[0] if r.stderr else 'error')
                pending = []
                break
            for position, message in bad.items():
                results[pending[position]] = ('err', message)
            pending = [index for k, index in enumerate(pending) if k not in bad]
        for index in pending:
            results[index] = clang_assemble_one(lines[index], arch, features_on)
        return results
    finally:
        for name in os.listdir(tmp):
            os.unlink(os.path.join(tmp, name))
        os.rmdir(tmp)


def _text_symbols(obj):
    """The offsets of the labels L<n> of an object: {n: offset}."""
    r = run(['objdump', '-t', obj])
    symbols = {}
    for line in r.stdout.splitlines():
        m = re.match(r'^([0-9a-f]+)\s+\w+\s+\.text\s+[0-9a-f]+\s+L(\d+)$', line)
        if m:
            symbols[int(m.group(2))] = int(m.group(1), 16)
    return symbols


def _clang_assemble_labeled(lines, arch, features_on, triple=None):
    """clang_assemble() of the AArch32 sets: a label before each line gives
    its bytes (the sizes vary). A line with ';' is several instructions (an
    it block): their bytes are together. Return ('ok', bytes) / ('err', msg)."""
    header = ['.arm', '.syntax unified'] if arch == 'a32' else ['.thumb', '.syntax unified']
    results = [None] * len(lines)
    pending = list(range(len(lines)))
    tmp = tempfile.mkdtemp(prefix='armclang')
    try:
        for _attempt in range(60):
            if not pending:
                break
            src = os.path.join(tmp, 'a.s')
            obj = os.path.join(tmp, 'a.o')
            line_of = {}          # source line number -> position in pending
            text = list(header)
            for k, index in enumerate(pending):
                text.append('L%d:' % index)
                for part in lines[index].split(';'):
                    text.append(part.strip())
                    line_of[len(text)] = k
            with open(src, 'w') as f:
                f.write('\n'.join(text) + '\n')
            cmd = ['clang', '-cc1as', '-triple', triple or TRIPLES[arch], '-filetype', 'obj', src, '-o', obj]
            for name in features_on:
                cmd.extend(['-target-feature', name if name[0] in '+-' else '+' + name])
            r = run(cmd)
            if r.returncode == 0:
                data = _text_bytes(obj)
                symbols = _text_symbols(obj)
                offsets = [symbols.get(index) for index in pending]
                for k, index in enumerate(pending):
                    if offsets[k] is None:
                        results[index] = ('err', 'no label')
                        continue
                    end = offsets[k + 1] if k + 1 < len(pending) and offsets[k + 1] is not None else len(data)
                    results[index] = ('ok', data[offsets[k]:end])
                pending = []
                break
            bad = {}
            for err_line in r.stderr.splitlines():
                m = _ERROR.match(err_line)
                if m:
                    position = line_of.get(int(m.group(1)))
                    if position is not None and position not in bad:
                        bad[position] = m.group(2)
            if not bad:
                for index in pending:
                    results[index] = ('err', r.stderr.strip().splitlines()[0] if r.stderr else 'error')
                pending = []
                break
            for position, message in bad.items():
                results[pending[position]] = ('err', message)
            pending = [index for k, index in enumerate(pending) if k not in bad]
        return results
    finally:
        for name in os.listdir(tmp):
            os.unlink(os.path.join(tmp, name))
        os.rmdir(tmp)


def clang_assemble_one(line, arch, features_on):
    tmp = tempfile.mkdtemp(prefix='armclang1')
    try:
        src = os.path.join(tmp, 'a.s')
        obj = os.path.join(tmp, 'a.o')
        with open(src, 'w') as f:
            if arch == 'a32':
                f.write('.arm\n.syntax unified\n')
            elif arch == 't32':
                f.write('.thumb\n.syntax unified\n')
            f.write(line + '\n')
        cmd = ['clang', '-cc1as', '-triple', TRIPLES[arch], '-filetype', 'obj', src, '-o', obj]
        for name in features_on:
            cmd.extend(['-target-feature', '+' + name])
        r = run(cmd)
        if r.returncode != 0:
            for err_line in r.stderr.splitlines():
                m = _ERROR.match(err_line)
                if m:
                    return ('err', m.group(2))
            return ('err', 'error')
        return ('ok', _text_bytes(obj))
    finally:
        for name in os.listdir(tmp):
            os.unlink(os.path.join(tmp, name))
        os.rmdir(tmp)


def _text_bytes(obj):
    """The bytes of the .text section of an object (ELF), through objdump -s."""
    r = run(['objdump', '-s', '-j', '.text', obj])
    data = bytearray()
    for line in r.stdout.splitlines():
        m = re.match(r'^\s*[0-9a-f]+\s+((?:[0-9a-f]{2,8}\s?)+)', line)
        if m and 'Contents' not in line:
            data.extend(bytes.fromhex(''.join(m.group(1).split())))
    return bytes(data)


def clang_requires(line, arch='a64'):
    """The message of clang for a line assembled without any feature: the
    'instruction requires: x y' feature names, [] if it assembles."""
    results = clang_assemble([line], arch, [])
    if results[0][0] == 'ok':
        return []
    m = re.match(r'instruction requires: (.*)$', results[0][1])
    if m:
        return m.group(1).split()
    return None


# ---------------------------------------------------------------------------
# dismount_cli

def cli(args, lines):
    """Run dismount_cli with the lines on its input. Return the output lines,
    or None if the process died (the output is then shorter)."""
    r = run([CLI] + args, '\n'.join(lines) + '\n')
    out = r.stdout.splitlines()
    if r.returncode < 0 or len(out) != len(lines):
        return None, out
    return out, out


def cli_lines(args, lines):
    """cli() that bisects a crash: a line which kills the process becomes
    'CRASH'."""
    if not lines:
        return []
    out, partial = cli(args, lines)
    if out is not None:
        return out
    if len(lines) == 1:
        return ['CRASH']
    middle = len(lines) // 2
    return cli_lines(args, lines[:middle]) + cli_lines(args, lines[middle:])


def dis_words(arch, words, mattr=None, addresses=None):
    """Disassemble 32 bit words with dismount. Return (status, text) tuples:
    ('OK', text), ('INVALID', ''), ('ERR', message) or ('CRASH', '')."""
    args = ['dis', '--arch', arch]
    if mattr:
        args.extend(['--mattr', mattr])
    lines = []
    for i, word in enumerate(words):
        address = addresses[i] if addresses else BASE + 4 * i
        lines.append('@%x %08x' % (address, word))
    return _parse_dis(cli_lines(args, lines))


def _parse_dis(output):
    result = []
    for line in output:
        if line.startswith('OK '):
            parts = line.split(' ', 2)
            result.append(('OK', normalize(parts[2]) if len(parts) > 2 else ''))
        elif line.startswith('INVALID'):
            result.append(('INVALID', ''))
        elif line == 'CRASH':
            result.append(('CRASH', ''))
        else:
            result.append(('ERR', line[4:]))
    return result


def asm_lines(arch, lines, mattr=None, addresses=None):
    """Assemble instruction lines with dismount. Return ('OK', bytes) or
    ('ERR', message) tuples."""
    args = ['asm', '--arch', arch]
    if mattr:
        args.extend(['--mattr', mattr])
    sent = []
    for i, line in enumerate(lines):
        address = addresses[i] if addresses else BASE + 4 * i
        sent.append('@%x %s' % (address, line))
    result = []
    for line in cli_lines(args, sent):
        if line.startswith('OK '):
            result.append(('OK', bytes.fromhex(line[3:].strip())))
        elif line == 'CRASH':
            result.append(('CRASH', 'crash'))
        else:
            result.append(('ERR', line[4:]))
    return result


_features_cache = {}


def features(arch='a64'):
    """The support matrix: (index, llvm name, set, support bits, entries)."""
    if arch not in _features_cache:
        r = run([CLI, 'features', '--arch', arch], '')
        rows = []
        for line in r.stdout.splitlines():
            index, name, isa, support, entries = line.split()
            rows.append((int(index), name, isa, int(support, 16), int(entries)))
        _features_cache[arch] = rows
    return _features_cache[arch]


def feature_name(index, arch='a64'):
    for row in features(arch):
        if row[0] == index:
            return row[1]
    return '-'


def entries(arch='a64'):
    """The encodings of the dismount tables: (mask, value, feature index,
    flags, size, mnemonic)."""
    r = run([CLI, 'entries', '--arch', arch], '')
    rows = []
    for line in r.stdout.splitlines():
        mask, value, feature, flags, size, mnemonic = line.split()
        rows.append((int(mask, 16), int(value, 16), int(feature), int(flags, 16),
                     int(size), mnemonic))
    return rows


# ---------------------------------------------------------------------------
# A64 encoding classes (the top level of the Arm ARM decode tree)

def a64_class(word):
    """'base' for the encodings the base table decodes (data processing,
    branches, system, loads and stores), 'simd' for the scalar floating point,
    Advanced SIMD and the structure loads, 'sve', 'sme' or 'reserved'."""
    op1 = (word >> 25) & 0xf
    if op1 == 0:
        # The reserved class holds udf, which the base table decodes
        return 'sme' if (word >> 31) & 1 else 'base'
    if op1 == 2:
        return 'sve'
    if op1 in (8, 9, 10, 11, 5, 13):
        return 'base' if op1 != 7 else 'simd'
    if op1 in (7, 15):
        return 'simd'
    # loads and stores: x1x0
    if op1 in (4, 6, 12, 14):
        # Advanced SIMD structures: bits 29-25 = 00110 with bit 31 clear
        if ((word >> 31) & 1) == 0 and ((word >> 25) & 0x1f) == 0x06:
            return 'simd'
        # SME loads and stores are in op1 == 0 (1110000x), already handled
        return 'base'
    return 'base'


def mnemonic_of(text):
    """The mnemonic (with its condition suffix removed) of a normalized
    instruction text."""
    return text.split(' ')[0].split('.')[0] if text else ''


def random_word(rng):
    return rng.getrandbits(32)


def seeded_rng(seed):
    return random.Random(seed)


def hex_bytes_to_word(data):
    return int.from_bytes(data[:4], 'little')


def log(*args):
    print(*args, file=sys.stderr)
    sys.stderr.flush()
