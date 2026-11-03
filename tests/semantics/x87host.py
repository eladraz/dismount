#!/usr/bin/env python3
"""
The host x87 oracle of the semantics tests (an alternative to Unicorn for the x87
instructions, see "The host x87 oracle" in tests/semantics/TEST_PLAN.md).

tests/semantics/tools/x87_host.c runs the instruction on the x87 unit of the host: a real
x86-64 processor, or Rosetta 2 on an Apple silicon Mac. This module builds that helper, turns
a test state into its input and its output into the result dictionary of oracle.Oracle.run().

The instruction is run in a 64-bit process: its memory operand is rewritten to [rdi], which
points to a scratch copy of the operand (the bytes of the state at the effective address), and
the bytes of the scratch area after the run are the writes.
"""
import os
import platform
import subprocess
import sys

import oracle

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, 'tools', 'x87_host.c')
M32 = 0xFFFFFFFF
PREFIXES = (0x66, 0x67, 0xf0, 0xf2, 0xf3, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65)
REG_NAMES = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']


def binary_path():
    return os.path.join(os.path.dirname(os.path.dirname(HERE)), 'out', 'bin', 'x87_host')


def available():
    """True if the helper can be built and run on this machine."""
    machine = platform.machine().lower()
    return machine in ('x86_64', 'amd64') or (sys.platform == 'darwin' and machine == 'arm64')


def build():
    path = binary_path()
    if os.path.exists(path) and os.path.getmtime(path) >= os.path.getmtime(SOURCE):
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    cmd = ['cc', '-O1', '-o', path, SOURCE]
    if sys.platform == 'darwin' and platform.machine().lower() == 'arm64':
        cmd = ['clang', '-arch', 'x86_64', '-O1', '-o', path, SOURCE]
    subprocess.check_call(cmd, stderr=subprocess.DEVNULL)
    return path


def decode_operand(code, state):
    """(rewritten code, effective address, area size) of the instruction at code. The address is
    None for an instruction without a memory operand."""
    i = 0
    kept = bytearray()
    while code[i] in PREFIXES:
        if code[i] in (0x66, 0xf2, 0xf3):
            kept.append(code[i])
        i += 1
    opcode = bytearray([code[i]])
    i += 1
    if opcode[0] == 0x0f:
        opcode.append(code[i])
        i += 1
    modrm = code[i]
    i += 1
    mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
    if mod == 3:
        return bytes(kept + opcode + bytes([modrm]) + code[i:]), None, 0
    ea = 0
    if rm == 4:
        sib = code[i]
        i += 1
        scale, index, base = 1 << (sib >> 6), (sib >> 3) & 7, sib & 7
        if index != 4:
            ea += state[REG_NAMES[index]] * scale
        if base == 5 and mod == 0:
            ea += int.from_bytes(code[i:i + 4], 'little')
            i += 4
        else:
            ea += state[REG_NAMES[base]]
    elif rm == 5 and mod == 0:
        ea += int.from_bytes(code[i:i + 4], 'little')
        i += 4
    else:
        ea += state[REG_NAMES[rm]]
    if mod == 1:
        ea += int.from_bytes(code[i:i + 1], 'little', signed=True)
        i += 1
    elif mod == 2:
        ea += int.from_bytes(code[i:i + 4], 'little', signed=True)
        i += 4
    ea &= M32
    size = 16
    first = opcode[0]
    if first == 0x0f and reg in (0, 1):
        size = 512
    elif first in (0xd9, 0xdd) and reg in (4, 6):
        size = 112
    return bytes(kept + opcode + bytes([(reg << 3) | 7])), ea, size


def full_tag_word(abridged, registers):
    """The 16 bit tag word the SDM defines: 00 valid, 01 zero, 10 special (NaN, infinity, denormal,
    unsupported encodings), 11 empty. Rosetta 2 reports every register that isn't empty as zero."""
    word = 0
    for i in range(8):
        if not (abridged >> i) & 1:
            tag = 3
        else:
            v = registers[i]
            exponent, mantissa = (v >> 64) & 0x7fff, v & ((1 << 64) - 1)
            if exponent == 0x7fff:
                tag = 2
            elif exponent == 0:
                tag = 1 if mantissa == 0 else 2
            else:
                tag = 0 if mantissa >> 63 else 2
        word |= tag << (2 * i)
    return word


def memory_byte(state, address):
    return state.get('mem', {}).get(address & M32, oracle.pattern_byte(address))


class HostX87(object):
    """One helper process."""

    def __init__(self):
        self.proc = subprocess.Popen([binary_path()], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     universal_newlines=True)

    def exchange(self, line):
        self.proc.stdin.write(line + '\n')
        self.proc.stdin.flush()
        return self.proc.stdout.readline().strip()

    def run(self, code, state):
        """Same contract as oracle.Oracle.run(); raises OracleError when the host can't run it."""
        # fsave, fclex, finit... are the wait (9B) and the no wait instruction: one instruction at a time
        if code[0] == 0x9b:
            code = code[:1]
        v2 = oracle.state_defaults(state)
        regs = {n: state[n] & M32 for n in oracle.REGS}
        if code == b'\x9b':
            rewritten, ea, size = code, None, 0
        elif code == b'\xdb\xe4':
            # fsetpm: Rosetta 2 raises SIGILL, the 387 and later treat it as fnop
            rewritten, ea, size = b'\xd9\xd0', None, 0
        elif code[0] == 0xdf and 0xc0 <= code[1] <= 0xc7:
            # ffreep st(i): Rosetta 2 raises SIGILL. It is ffree st(i), then a pop (ffree st(0), fincstp)
            rewritten, ea, size = bytes([0xdd, code[1], 0xdd, 0xc0, 0xd9, 0xf7]), None, 0
        else:
            rewritten, ea, size = decode_operand(code, regs)
        result = {
            'exit': 'fall', 'regs': dict(regs), 'eip': (state['eip'] + len(code)) & M32,
            'eflags': state['eflags'], 'seg': dict(v2['seg']) if v2['seg'] else {n: 0 for n in oracle.SEGS},
            'fcw': v2['fcw'], 'fsw': v2['fsw'], 'ftw': v2['ftw'], 'fpr': list(v2['fpr']),
            'mxcsr': v2['mxcsr'], 'xmm': list(v2['xmm']), 'mem': {},
        }
        for n in oracle.SEGS:
            result['seg'].setdefault(n, 0)
        if ea is not None and size == 512 and ea & 0xf:
            # fxsave / fxrstor of an unaligned area: #GP
            result['exit'] = 'trap:13'
            return result
        memory = b''
        if ea is not None:
            memory = bytes(memory_byte(v2, ea + i) for i in range(size))
        line = 'code=%s fcw=%x fsw=%x ftw=%x eflags=%x eax=%x' % (
            rewritten.hex(), v2['fcw'], v2['fsw'], v2['ftw'], state['eflags'] & 0xcd5, regs['eax'])
        line += ' ' + ' '.join('r%d=%020x' % (i, v) for i, v in enumerate(v2['fpr']))
        line += ' mxcsr=%x ' % v2['mxcsr'] + ' '.join('xmm%d=%032x' % (i, v) for i, v in enumerate(v2['xmm']))
        if memory:
            line += ' mem=' + memory.hex()
        answer = self.exchange(line)
        if not answer.startswith('fcw='):
            raise oracle.OracleError('host x87: ' + answer)
        kv = dict(tok.split('=') for tok in answer.split())
        result['fcw'] = int(kv['fcw'], 16)
        result['fsw'] = int(kv['fsw'], 16)
        result['ftw'] = int(kv['ftw'], 16)
        result['fpr'] = [int(kv['r%d' % i], 16) for i in range(8)]
        result['mxcsr'] = int(kv['mxcsr'], 16)
        result['xmm'] = [int(kv['xmm%d' % i], 16) for i in range(8)]
        result['regs']['eax'] = int(kv['eax'], 16)
        # only the arithmetic flags and DF come from the run (the system bits, IF, are the process')
        result['eflags'] = (state['eflags'] & ~0xcd5) | (int(kv['eflags'], 16) & 0xcd5) | 2
        if ea is not None:
            data = bytearray(bytes.fromhex(kv.get('mem', '')))
            if size == 112 and rewritten[-2] in (0xd9, 0xdd) and (rewritten[-1] >> 3) & 7 == 6:
                # fnstenv, fnsave: the tag word from the register contents
                tag = full_tag_word(v2['ftw'], v2['fpr']).to_bytes(2, 'little')
                offset = 4 if 0x66 in rewritten[:-2] else 8
                data[offset:offset + 2] = tag
            if size == 512 and (rewritten[-1] >> 3) & 7 == 0:
                # Rosetta 2 doesn't write the opcode and the instruction and data pointers
                # (bytes 6 to 23), they are not kept here either (zeros)
                data[5:24] = bytes(19)
                # ... and it clears the bytes 288 to 511 (reserved / available to software), which
                # nothing else writes
                data[288:512] = memory[288:512]
            for i, b in enumerate(data):
                result['mem'][(ea + i) & M32] = b
        return result


_hosts = {}


def get_host():
    pid = os.getpid()
    if pid not in _hosts:
        _hosts[pid] = HostX87()
    return _hosts[pid]


def is_nan(value):
    return (value >> 64) & 0x7fff == 0x7fff and value & ((1 << 63) - 1) != 0


def known_quirk(source, d, o):
    """A reason when the host (Rosetta 2) is known to differ from the SDM in this state, and the
    state isn't compared; otherwise None. See tests/semantics/host_x87_quirks.txt."""
    stack_fault = d['fsw'] & 0x40
    if stack_fault and source.startswith('fbld') and not o['fsw'] & 0x40:
        return 'fbld does not detect the stack overflow'
    if stack_fault and any(d['fpr'][i] != o['fpr'][i] and is_nan(o['fpr'][i]) for i in range(8)):
        return 'a NaN operand wins over the stack fault response'
    return None


def fix_unicorn_fxsave(code, state, result):
    """Unicorn (QEMU) leaves the reserved bytes of the FXSAVE image alone: the opcode and the
    padding of the registers. The processor writes zeros there (Rosetta 2 does for the registers),
    and so does dismount: add the writes to the result of Unicorn."""
    try:
        rewritten, ea, size = decode_operand(code, {n: state[n] & M32 for n in oracle.REGS})
    except IndexError:
        return
    if ea is None or size != 512 or rewritten[-2:-1] != b'\xae' or (rewritten[-1] >> 3) & 7 != 0:
        return
    if result['exit'] != 'fall':
        return
    for offset in [6, 7] + [32 + 16 * i + k for i in range(8) for k in range(10, 16)]:
        result['mem'][(ea + offset) & M32] = 0
