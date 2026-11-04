#!/usr/bin/env python3
"""Shared code of the RISC-V compatibility tools (tests/riscv/TEST_PLAN.md).

The oracles are llvm-mc (the assembler) and llvm-objdump (the disassembler) of
LLVM 23; the system under test is out/bin/dismount_cli. The LLVM tools are
looked up in $RISCV_LLVM_BIN (default: /opt/homebrew/opt/llvm/bin): the system
clang / objdump are Apple's and don't know RISC-V. Python 3, no third party
packages.
"""
import os
import re
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
CLI = os.path.join(REPO, 'out', 'bin', 'dismount_cli')
LLVM_BIN = os.environ.get('RISCV_LLVM_BIN', '/opt/homebrew/opt/llvm/bin')

# The address of the first instruction of a listing: the pc relative operands
# print the absolute addresses, so both sides use the same base
BASE = 0x10000

# The scalar, compressed and privileged features dismount implements, by their
# LLVM names (RiscvFeatures::getName), without the vector ones (the vector
# agent's table); "all" of the tests is this list
SCALAR_FEATURES = [
    'm', 'zmmul', 'zalrsc', 'zaamo', 'f', 'd', 'zicsr', 'zicntr', 'zihpm',
    'zifencei', 'zihintpause', 'zihintntl', 'zicbom', 'zicbop', 'zicboz',
    'zicond', 'zimop', 'zawrs', 'zacas', 'zabha', 'experimental-zicfilp',
    'experimental-zicfiss', 'zba', 'zbb', 'zbs', 'zbc', 'zbkb', 'zbkc',
    'zbkx', 'zknd', 'zkne', 'zknh', 'zksed', 'zksh', 'zkr', 'zkt', 'zfa',
    'zfh', 'zfhmin', 'zfbfmin', 'zca', 'zcf', 'zcd', 'zcb', 'zcmop',
    'ziccif', 'ziccrse', 'ziccamoa', 'ziccamoc', 'zicclsm', 'za64rs',
    'zic64b', 'zama16b', 'supm', 'svinval', 'h', 'sstc', 'sscofpmf', 'ssnpm',
    'ssstateen', 'svpbmt', 'svnapot', 'svade', 'svbare', 'ssccptr',
    'sstvecd', 'sstvala', 'sscounterenw', 'ssu64xl', 'shcounterenw',
    'shvstvala', 'shtvala', 'shvstvecd', 'shvsatpa', 'shgatpa', 'sdtrig',
    'ssstrict', 'svvptc', 'sspm',
]
VECTOR_FEATURES = [
    'zve32x', 'zve32f', 'zve64x', 'zve64f', 'zve64d', 'v', 'zvfh', 'zvfhmin',
    'zvfbfmin', 'zvfbfwma', 'zvkb', 'zvbb', 'zvbc', 'zvkg', 'zvkned',
    'zvknha', 'zvknhb', 'zvksed', 'zvksh', 'zvkt',
]


def tool(name):
    return os.path.join(LLVM_BIN, name)


def run(cmd, input_text=None):
    return subprocess.run(cmd, input=input_text, capture_output=True, text=True)


def triple(bits):
    return 'riscv64' if bits == 64 else 'riscv32'


def mattr_arg(features):
    """The --mattr / -mattr value of a feature list (names without signs)."""
    return ','.join('+' + f for f in features)


def all_features(bits, vector=False):
    """The 'all' feature list of the tests for an XLEN. Zcf is RV32 only: LLVM
    refuses it on RV64."""
    features = list(SCALAR_FEATURES)
    if vector:
        features += VECTOR_FEATURES
    if bits == 64:
        features = [f for f in features if f != 'zcf']
    return features


# ---------------------------------------------------------------------------
# Text

_COMMENT = re.compile(r'\s*#.*$')
_SYMBOL = re.compile(r'\s*<[^>]*>\s*$')


def normalize(text):
    """The text of an instruction as it is compared: no comment, no symbol of
    a branch target, the blanks collapsed."""
    text = _COMMENT.sub('', text)
    text = _SYMBOL.sub('', text)
    return ' '.join(text.split())


# ---------------------------------------------------------------------------
# ELF: a relocatable object with one .text section and nothing else, so that
# llvm-objdump disassembles every byte as an instruction (the .word of an
# assembly file are data)

def make_elf(data, bits):
    shstrtab = b'\0.text\0.shstrtab\0'
    if bits == 64:
        ehsize, shentsize = 64, 64
    else:
        ehsize, shentsize = 52, 40
    text_off = ehsize
    str_off = text_off + len(data)
    pad = (-(str_off + len(shstrtab))) % 8
    sh_off = str_off + len(shstrtab) + pad
    ident = b'\x7fELF' + bytes([2 if bits == 64 else 1, 1, 1, 0]) + bytes(8)
    if bits == 64:
        header = ident + struct.pack('<HHIQQQIHHHHHH', 1, 243, 1, 0, 0, sh_off, 0,
                                     ehsize, 0, 0, shentsize, 3, 2)
        sections = [struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
                    struct.pack('<IIQQQQIIQQ', 1, 1, 6, 0, text_off, len(data), 0, 0, 2, 0),
                    struct.pack('<IIQQQQIIQQ', 7, 3, 0, 0, str_off, len(shstrtab), 0, 0, 1, 0)]
    else:
        header = ident + struct.pack('<HHIIIIIHHHHHH', 1, 243, 1, 0, 0, sh_off, 0,
                                     ehsize, 0, 0, shentsize, 3, 2)
        sections = [struct.pack('<IIIIIIIIII', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
                    struct.pack('<IIIIIIIIII', 1, 1, 6, 0, text_off, len(data), 0, 0, 2, 0),
                    struct.pack('<IIIIIIIIII', 7, 3, 0, 0, str_off, len(shstrtab), 0, 0, 1, 0)]
    return header + data + shstrtab + bytes(pad) + b''.join(sections)


def objdump_bytes(data, bits, features, flags=(), base=BASE):
    """Disassemble raw bytes placed at 'base'. Return a list of
    (address, size, text): '<unknown>' text for the invalid units. 'flags' are
    more objdump arguments (['-M', 'no-aliases']). Immediates are printed in
    decimal (--no-print-imm-hex), as llvm-mc does."""
    with tempfile.TemporaryDirectory(prefix='rvobjdump') as tmp:
        obj = os.path.join(tmp, 'a.o')
        with open(obj, 'wb') as f:
            f.write(make_elf(data, bits))
        cmd = [tool('llvm-objdump'), '-d', '--no-print-imm-hex',
               '--adjust-vma=0x%x' % base, '--disassemble-zeroes',
               '--mattr=' + mattr_arg(features)] + list(flags) + [obj]
        r = run(cmd)
        if r.returncode != 0:
            raise RuntimeError('llvm-objdump failed: ' + r.stderr[:500])
        out = []
        for line in r.stdout.splitlines():
            if '\t' not in line:
                continue
            head, text = line.split('\t', 1)
            m = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2,8}\s+)*[0-9a-f]{2,8})\s*$', head)
            if not m:
                continue
            size = sum(len(g) // 2 for g in m.group(2).split())
            text = text.strip()
            out.append((int(m.group(1), 16), size, text if text == '<unknown>' else normalize(text)))
        return out


# ---------------------------------------------------------------------------
# llvm-mc: the bytes of every line of a listing, through an object file. Every
# line gets a label, so the size of each one is known (the pseudo instructions
# of more than one instruction too); the lines LLVM rejects are reported.

def read_elf(path):
    """Return (the bytes of .text, {symbol name: offset}) of a relocatable ELF."""
    data = open(path, 'rb').read()
    is64 = data[4] == 2
    if is64:
        shoff, = struct.unpack_from('<Q', data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3a)
    else:
        shoff, = struct.unpack_from('<I', data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
    sections = []
    for i in range(shnum):
        base = shoff + i * shentsize
        if is64:
            name, kind, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from(
                '<IIQQQQIIQQ', data, base)
        else:
            name, kind, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from(
                '<IIIIIIIIII', data, base)
        sections.append((name, kind, offset, size, link, entsize))

    def cstring(offset):
        end = data.index(b'\0', offset)
        return data[offset:end].decode()

    shstr = sections[shstrndx]
    text = b''
    text_index = None
    for i, (name, kind, offset, size, link, entsize) in enumerate(sections):
        if cstring(shstr[2] + name) == '.text':
            text = data[offset:offset + size]
            text_index = i
    symbols = {}
    for name, kind, offset, size, link, entsize in sections:
        if kind != 2:
            continue
        strtab = sections[link]
        count = size // entsize
        for i in range(count):
            base = offset + i * entsize
            if is64:
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', data, base)
            else:
                st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', data, base)
            if st_shndx == text_index:
                symbols[cstring(strtab[2] + st_name)] = st_value
    return text, symbols


def llvm_assemble(lines, bits, features, rvc=False):
    """Assemble the lines with llvm-mc. Return a list of ('ok', bytes) or
    ('error', message), one per line. With rvc=False the instructions are not
    compressed (.option norvc); explicit c.* lines need rvc=True."""
    alive = list(range(len(lines)))
    errors = {}
    mattr = mattr_arg(features)
    with tempfile.TemporaryDirectory(prefix='rvmc') as tmp:
        obj = os.path.join(tmp, 'a.o')
        for _ in range(8):
            header = ['.text']
            if not rvc:
                header.insert(0, '.option norvc')
            header.append('1:')
            body = ['L%d: %s' % (i, lines[i]) for i in alive]
            source = '\n'.join(header + body + ['Lend:', '1:', '']) + '\n'
            p = run([tool('llvm-mc'), '-triple=' + triple(bits), '-mattr=' + mattr,
                     '-filetype=obj', '-o', obj], source)
            if p.returncode == 0:
                break
            bad = set()
            for m in re.finditer(r'<stdin>:(\d+):\d+: error: (.*)', p.stderr):
                index = int(m.group(1)) - 1 - len(header)
                if 0 <= index < len(alive):
                    bad.add(alive[index])
                    errors[alive[index]] = m.group(2)
            if not bad:
                raise RuntimeError('llvm-mc failed: ' + p.stderr[:500])
            alive = [i for i in alive if i not in bad]
        else:
            raise RuntimeError('llvm-mc kept failing')
        text, symbols = read_elf(obj)
    result = []
    offsets = [symbols['L%d' % i] for i in alive] + [symbols['Lend']]
    sizes = {}
    for k, i in enumerate(alive):
        sizes[i] = text[offsets[k]:offsets[k + 1]]
    for i in range(len(lines)):
        if i in sizes:
            result.append(('ok', sizes[i]))
        else:
            result.append(('error', errors.get(i, '?')))
    return result


# ---------------------------------------------------------------------------
# dismount_cli

def cli_lines(args, lines):
    p = run([CLI] + args, ''.join(l + '\n' for l in lines))
    out = p.stdout.split('\n')
    if out and out[-1] == '':
        out.pop()
    if len(out) != len(lines):
        raise RuntimeError('dismount_cli %s: %d lines for %d inputs (%s)' % (
            ' '.join(args), len(out), len(lines), p.stderr[:300]))
    return out


def dismount_asm(lines, bits, mattr=None, rvc=True, addresses=None):
    """Assemble with dismount. Return ('OK', bytes) or ('ERR', message)."""
    args = ['asm', '--arch', 'rv%d' % bits]
    if mattr:
        args += ['--mattr', mattr]
    if not rvc:
        args.append('--no-rvc')
    sent = ['@%x %s' % (addresses[i] if addresses else BASE, line) for i, line in enumerate(lines)]
    result = []
    for line in cli_lines(args, sent):
        if line.startswith('OK '):
            result.append(('OK', bytes.fromhex(line[3:].strip())))
        else:
            result.append(('ERR', line[4:]))
    return result


def unit_hex(data):
    """The hex of one instruction as dismount_cli dis reads it: the value."""
    return '%0*x' % (len(data) * 2, int.from_bytes(data, 'little'))


def dismount_dis(units, bits, mattr=None, fmt=None):
    """Disassemble units [(address, bytes)] with dismount. Return ('OK', text),
    ('INVALID', '') or ('ERR', message)."""
    args = ['dis', '--arch', 'rv%d' % bits]
    if mattr:
        args += ['--mattr', mattr]
    if fmt:
        args += ['--format', fmt]
    sent = ['@%x %s' % (a, unit_hex(d)) for a, d in units]
    result = []
    for line in cli_lines(args, sent):
        if line.startswith('OK '):
            result.append(('OK', normalize(line.split(' ', 2)[2])))
        elif line.startswith('INVALID'):
            result.append(('INVALID', ''))
        else:
            result.append(('ERR', line))
    return result


def objdump_units(units, bits, features, flags=()):
    """llvm-objdump of units [(address, bytes)] placed one after the other
    from the first address: the text of each, '<unknown>' if invalid."""
    if not units:
        return []
    base = units[0][0]
    data = b''.join(d for _, d in units)
    records = {}
    for address, size, text in objdump_bytes(data, bits, features, flags, base):
        if text == '<unknown>':
            for i in range(size):
                records[address + i] = (1, text)
        else:
            records[address] = (size, text)
    out = []
    position = base
    for address, d in units:
        # (the units are contiguous: the address is the running position)
        record = records.get(position)
        out.append(record[1] if record and record[0] == len(d) else '<unknown>')
        position += len(d)
    return out


def feature_closure(bits, spec):
    """The LLVM feature names of a dismount feature spec ('none,+zba,-zbb')."""
    p = run([CLI, 'set', '--arch', 'rv%d' % bits, '--mattr', spec], '')
    return p.stdout.split()
