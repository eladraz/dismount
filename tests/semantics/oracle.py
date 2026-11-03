#!/usr/bin/env python3
"""
The Unicorn side of the semantics tests (see tests/semantics/TEST_PLAN.md).

Runs ONE instruction (its bytes) from a given state in 16-bit real mode or in
32-bit mode, on Unicorn (QEMU's x86 CPU), and returns the result in the shape
of the `dismount_cli run` output: exit, registers, eip, eflags and the bytes
the instruction wrote.

Behaviour of Unicorn 2.x that matters here (all verified, see the notes):
  * An engine is created for every instruction: a trap leaves the exception
    "in flight" and the next #DE of a reused engine comes out as #DF (trap:8).
  * Both modes start with every segment selector, base and the cs/ds/es/ss/
    fs/gs state at 0: flat addressing with base 0 works without a GDT, also in
    32-bit mode (the null selectors are not checked).
  * Memory is mapped lazily from the UC_HOOK_MEM_UNMAPPED hook, in 4K pages
    filled with the memory pattern. The hook runs for every page of an access
    that crosses a page border, and the 32-bit wrap-around at 0xFFFFFFFF works
    (the second half of the access is at address 0).
  * A `rep` instruction stops after each iteration with eip unchanged:
    emu_start(count=1) is repeated while eip stays at the instruction.
  * #DE, int3, int n, into, bound (#BR) arrive in the UC_HOOK_INTR hook with
    the vector in `intno` and eip still at/after the instruction (not
    compared). An invalid opcode does not reach the hook: emu_start raises
    UC_ERR_INSN_INVALID, which is reported as trap:6.
  * A store to the code page (not to the instruction) ends the translated
    block and Unicorn restarts the instruction with eip unchanged: it is
    stepped again, like a rep iteration.
  * A write that overlaps the instruction's own bytes makes Unicorn restart
    the translated block forever (no progress); this is reported as an
    OracleError (self-modifying instruction), the state is not comparable.
  * In 16-bit mode Unicorn does not check the segment limit: an access at
    offset >= 0x10000 (a 32-bit address override) or a push that crosses
    sp = 0xFFFF goes to linear addresses above 64K.
"""
import functools
import os
import sys

from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_16, UC_MODE_32,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_HOOK_INTR,
                     UC_ERR_INSN_INVALID)
from unicorn import x86_const as X

try:
    import numpy as _np
except ImportError:
    _np = None

REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
UC_REG = {n: getattr(X, 'UC_X86_REG_' + n.upper()) for n in REGS}

# CF PF AF ZF SF DF OF IF AC ID and the always-set bit 1
EFLAGS_MASK = 0x1 | 0x4 | 0x10 | 0x40 | 0x80 | 0x400 | 0x800 | 0x200 | 0x40000 | 0x200000 | 0x2
M32 = 0xFFFFFFFF
PAGE = 0x1000
PREFIX_BYTES = {0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65}
MAX_STEPS = 4096


class OracleError(Exception):
    pass


def pattern_byte(a):
    """The memory pattern of the protocol: byte(A) = ((A * 2654435761) mod 2^32) >> 24"""
    return (((a & M32) * 2654435761) & M32) >> 24


@functools.lru_cache(maxsize=1024)
def pattern_page(base):
    if _np is not None:
        a = _np.arange(base, base + PAGE, dtype=_np.uint64)
        return (((a * 2654435761) & M32) >> 24).astype(_np.uint8).tobytes()
    k = 2654435761
    v = (base * k) & M32
    out = bytearray(PAGE)
    for i in range(PAGE):
        out[i] = v >> 24
        v = (v + k) & M32
    return bytes(out)


class Oracle(object):
    def __init__(self, bits):
        assert bits in (16, 32)
        self.bits = bits
        self.uc = None
        self.pages = []
        self.writes = []
        self.trap = None

    def _new_engine(self):
        # A fresh engine for every instruction (0.1 ms): after a trap Unicorn
        # keeps the exception in flight, and the next #DE becomes a #DF (trap:8)
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_32 if self.bits == 32 else UC_MODE_16)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._write)
        uc.hook_add(UC_HOOK_INTR, self._intr)
        return uc

    # --- hooks -----------------------------------------------------------
    def _map(self, base):
        self.uc.mem_map(base, PAGE)
        self.uc.mem_write(base, pattern_page(base))
        self.pages.append(base)

    def _unmapped(self, uc, access, addr, size, value, user):
        try:
            self._map(addr & M32 & ~(PAGE - 1))
        except UcError:
            return False
        return True

    def _write(self, uc, access, addr, size, value, user):
        self.writes.append((addr & M32, size))

    def _intr(self, uc, intno, user):
        self.trap = intno
        uc.emu_stop()

    # --- one instruction -------------------------------------------------
    def run(self, code, state):
        """code: bytes. state: dict with eax..edi, eip, eflags (ints).
        Returns {'exit', 'regs', 'eip', 'eflags', 'mem'} (mem: addr -> byte).
        Raises OracleError when the state can't be compared."""
        uc = self._new_engine()
        eip = state['eip'] & M32
        self.pages = []
        self.writes = []
        self.trap = None
        try:
            for base in sorted({((eip + i) & M32) & ~(PAGE - 1) for i in range(len(code) + 1)}):
                self._map(base)
            for i, b in enumerate(code):
                uc.mem_write((eip + i) & M32, bytes([b]))
            for n in REGS:
                uc.reg_write(UC_REG[n], state[n] & M32)
            uc.reg_write(X.UC_X86_REG_EFLAGS, state['eflags'] & M32)
            uc.reg_write(X.UC_X86_REG_EIP, eip)

            rep = False
            for b in code[:14]:
                if b not in PREFIX_BYTES:
                    break
                rep = rep or b in (0xF2, 0xF3)
            invalid = False
            steps = 0
            while True:
                steps += 1
                if steps > MAX_STEPS:
                    raise OracleError('rep iteration limit')
                try:
                    uc.emu_start(eip, 0, count=1)
                except UcError as e:
                    if e.errno == UC_ERR_INSN_INVALID:
                        invalid = True
                        break
                    raise OracleError('unicorn: %s' % e)
                if self.trap is not None:
                    break
                if uc.reg_read(X.UC_X86_REG_EIP) != eip:
                    break
                # A store to the page of the code ends the translated block
                # and Unicorn restarts the instruction: step it again
                code_page = eip & ~(PAGE - 1)
                touched = any(((a + i) & M32) & ~(PAGE - 1) == code_page
                              for a, s in self.writes for i in range(s))
                if not (rep or touched):
                    break

            code_range = {(eip + i) & M32 for i in range(len(code))}
            addrs = set()
            for a, s in self.writes:
                for i in range(s):
                    addrs.add((a + i) & M32)
            if addrs & code_range:
                raise OracleError('self-modifying instruction')
            mem = {}
            for a in addrs:
                mem[a] = uc.mem_read(a, 1)[0]

            if invalid:
                exit_ = 'trap:6'
            elif self.trap is not None:
                exit_ = 'trap:%d' % self.trap
            else:
                new_eip = uc.reg_read(X.UC_X86_REG_EIP)
                exit_ = 'fall' if new_eip == (eip + len(code)) & M32 else 'branch'
            res = {
                'exit': exit_,
                'regs': {n: uc.reg_read(UC_REG[n]) for n in REGS},
                'eip': uc.reg_read(X.UC_X86_REG_EIP),
                'eflags': (uc.reg_read(X.UC_X86_REG_EFLAGS) & EFLAGS_MASK) | 2,
                'mem': mem,
            }
            return res
        finally:
            self.uc = None


_oracles = {}


def get_oracle(bits):
    if bits not in _oracles:
        _oracles[bits] = Oracle(bits)
    return _oracles[bits]


# ---------------------------------------------------------------------------
# The dismount_cli run line format
# ---------------------------------------------------------------------------

def format_state(code, state):
    """One input line of `dismount_cli run`."""
    return '%s ; %s eip=%x eflags=%x' % (
        code.hex(), ' '.join('%s=%x' % (n, state[n] & M32) for n in REGS),
        state['eip'] & M32, state['eflags'] & M32)


def parse_state(line):
    hexs, _, rest = line.partition(';')
    st = {}
    for tok in rest.split():
        k, _, v = tok.partition('=')
        st[k] = int(v, 16)
    return bytes.fromhex(hexs.strip()), st


def format_mem(mem):
    """Sorted bytes merged into runs: 00001000:aabbcc,00002000:11 or '-'."""
    if not mem:
        return '-'
    runs = []
    for a in sorted(mem):
        if runs and runs[-1][0] + len(runs[-1][1]) == a:
            runs[-1][1].append(mem[a])
        else:
            runs.append((a, bytearray([mem[a]])))
    return ','.join('%08x:%s' % (a, bytes(b).hex()) for a, b in runs)


def parse_mem(text):
    mem = {}
    if text in ('-', ''):
        return mem
    for run in text.split(','):
        a, _, data = run.partition(':')
        base = int(a, 16)
        for i in range(len(data) // 2):
            mem[(base + i) & M32] = int(data[2 * i:2 * i + 2], 16)
    return mem


def format_result(res, undef=0):
    return 'OK exit=%s undef=%08x %s eip=%08x eflags=%08x mem=%s' % (
        res['exit'], undef, ' '.join('%s=%08x' % (n, res['regs'][n]) for n in REGS),
        res['eip'], res['eflags'], format_mem(res['mem']))


def parse_result(line):
    """Parses an `OK ...` / `ERR ...` output line. Returns a dict with the key
    'err' (the message) on ERR, otherwise the oracle shape plus 'undef'."""
    if not line.startswith('OK '):
        return {'err': line[4:].strip() if line.startswith('ERR ') else line.strip()}
    kv = {}
    for tok in line[3:].split():
        k, _, v = tok.partition('=')
        kv[k] = v
    try:
        undef = kv.get('undef', '0')
        undef = 0 if undef in ('-', '') else int(undef, 16)
        return {
            'exit': kv['exit'],
            'undef': undef,
            'regs': {n: int(kv[n], 16) for n in REGS},
            'eip': int(kv['eip'], 16),
            'eflags': int(kv['eflags'], 16),
            'mem': parse_mem(kv.get('mem', '-')),
        }
    except (KeyError, ValueError) as e:
        return {'err': 'malformed output (%s): %s' % (e, line[:120])}


def run_line(bits, line):
    """Oracle answer for one `dismount_cli run` input line, as an output line."""
    try:
        code, st = parse_state(line)
        return format_result(get_oracle(bits).run(code, st))
    except OracleError as e:
        return 'ERR oracle: %s' % e


if __name__ == '__main__':
    # Stand-alone: oracle.py [--bits 16|32] < input-lines > output-lines
    # (the same protocol as `dismount_cli run`; used as the fake CLI in tests)
    b = 32
    if len(sys.argv) > 2 and sys.argv[1] == '--bits':
        b = int(sys.argv[2])
    for ln in sys.stdin:
        if ln.strip():
            sys.stdout.write(run_line(b, ln) + '\n')
    sys.stdout.flush()
