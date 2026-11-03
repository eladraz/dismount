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


# ---------------------------------------------------------------------------
# State v2 (see "State v2" in TEST_PLAN.md): segments, x87, SSE
# ---------------------------------------------------------------------------

SEGS = ['es', 'cs', 'ss', 'ds', 'fs', 'gs']
UC_SEG = {n: getattr(X, 'UC_X86_REG_' + n.upper()) for n in SEGS}
DEFAULT_FCW = 0x37f
DEFAULT_MXCSR = 0x1f80
GDT_ADDRESS = 0x000f0000
GDT_LIMIT = 0x4f
# The fixed GDT of the 32-bit mode: 8-byte descriptors, the selector is index * 8
GDT = bytes([
    0, 0, 0, 0, 0, 0, 0, 0,                          # 0x00 null
    0xff, 0xff, 0, 0, 0, 0x9a, 0xcf, 0x00,           # 0x08 code32 DPL0 base 0 4G
    0xff, 0xff, 0, 0, 0, 0x92, 0xcf, 0x00,           # 0x10 data DPL0 base 0 4G rw
    0xff, 0xff, 0, 0, 0, 0x93, 0xcf, 0x10,           # 0x18 data DPL0 base 0x10000000 rw, accessed
    0xff, 0xff, 0, 0x20, 0, 0x9a, 0xcf, 0x00,        # 0x20 code32 DPL0 base 0x2000
    0xff, 0xff, 0, 0, 0, 0x9a, 0x00, 0x00,           # 0x28 code16 DPL0 limit 0xffff
    0xff, 0xff, 0, 0, 0, 0x90, 0xcf, 0x00,           # 0x30 data read only
    0xff, 0xff, 0, 0, 0, 0x12, 0xcf, 0x00,           # 0x38 data not present
    0xff, 0xff, 0, 0, 0, 0xf2, 0xcf, 0x20,           # 0x40 data DPL3 base 0x20000000
    0x00, 0x30, 0x08, 0, 0, 0xec, 0x00, 0x00,        # 0x48 call gate to 0x08:0x3000
])
# The selectors the harness may put in the initial state (present segments)
VALID_DATA_SELECTORS = [0x10, 0x18, 0x40]


def segment_base(bits, sel):
    """The base of the segment `sel` of the initial state (as the CLI computes it)."""
    if bits == 16:
        return (sel & 0xFFFF) << 4
    if sel & 0xFFFC == 0:
        return 0
    idx = sel >> 3
    if sel & 4 or idx * 8 >= len(GDT) or not GDT[idx * 8 + 5] & 0x10:
        raise OracleError('bad initial selector %#x' % sel)
    d = GDT[idx * 8:idx * 8 + 8]
    return d[2] | d[3] << 8 | d[4] << 16 | d[7] << 24


def state_defaults(state):
    """The v2 keys of a state, with their defaults."""
    return {
        'seg': dict(state.get('seg', {})),
        'fcw': state.get('fcw', DEFAULT_FCW),
        'fsw': state.get('fsw', 0),
        'ftw': state.get('ftw', 0),
        'fpr': list(state.get('fpr', [0] * 8)),
        'mxcsr': state.get('mxcsr', DEFAULT_MXCSR),
        'xmm': list(state.get('xmm', [0] * 8)),
        'mem': dict(state.get('mem', {})),
    }


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


def is_far_transfer(code):
    """True for jmp / call far (ptr16:16/32 and m16:16/32), retf and iret."""
    i = 0
    while i < len(code) and code[i] in PREFIX_BYTES:
        i += 1
    if i >= len(code):
        return False
    if code[i] in (0xEA, 0x9A, 0xCA, 0xCB, 0xCF):
        return True
    return code[i] == 0xFF and i + 1 < len(code) and ((code[i + 1] >> 3) & 7) in (3, 5)


# ---------------------------------------------------------------------------
# Corrections of QEMU 5 (Unicorn 2.1.4) for the MMX / SSE instructions, see
# oracle_bugs.txt: an MMX register write doesn't set the x87 exponent to ones,
# QEMU doesn't raise #GP for a misaligned 128 bit operand, nor for a reserved
# bit of ldmxcsr
# ---------------------------------------------------------------------------

def _decode_simd(code):
    """(prefixes, opcode map, opcode, index of the ModRM byte) of a 0F
    instruction, or None."""
    i = 0
    pre = set()
    while i < len(code) and code[i] in PREFIX_BYTES:
        pre.add(code[i])
        i += 1
    if i + 1 >= len(code) or code[i] != 0x0F:
        return None
    if code[i + 1] == 0x38 and i + 2 < len(code):
        return pre, '0f38', code[i + 2], i + 3
    if code[i + 1] == 0x3A and i + 2 < len(code):
        return pre, '0f3a', code[i + 2], i + 3
    return pre, '0f', code[i + 1], i + 2


def mmx_written_register(code):
    """The number of the mm register that an MMX instruction writes, or None."""
    d = _decode_simd(code)
    if d is None or d[1] != '0f' or d[3] >= len(code):
        return None
    pre, _, op, at = d
    mod, reg, rm = code[at] >> 6, (code[at] >> 3) & 7, code[at] & 7
    if pre & {0x66, 0xF2, 0xF3}:
        if op in (0x2C, 0x2D) and 0x66 in pre:         # cvt(t)pd2pi
            return reg
        if op == 0xD6 and 0xF2 in pre:                 # movdq2q
            return reg
        return None
    if (0x60 <= op <= 0x6B or op in (0x6E, 0x6F, 0x70, 0x74, 0x75, 0x76, 0xC4, 0x2C, 0x2D) or
            0xD1 <= op <= 0xD5 or 0xD8 <= op <= 0xDF or 0xE0 <= op <= 0xE5 or
            0xE8 <= op <= 0xEF or 0xF1 <= op <= 0xF6 or 0xF8 <= op <= 0xFE):
        return reg
    if op in (0x71, 0x72, 0x73) and mod == 3:           # shifts by an immediate
        return rm
    if op == 0x7F and mod == 3:                         # movq mm, mm
        return rm
    return None


def needs_alignment(code):
    """True for the SSE / SSE2 instructions whose 128 bit memory operand must
    be 16 byte aligned (#GP otherwise, SDM Vol. 2 "Exceptions"). The unaligned
    moves (movups, movupd, movdqu) and the 32 / 64 bit operands are not."""
    d = _decode_simd(code)
    if d is None:
        return False
    pre, mapid, op, _ = d
    if mapid == '0f38':
        return 0x66 in pre and 0xDB <= op <= 0xDF           # aesimc .. aesdeclast
    if mapid == '0f3a':
        return 0x66 in pre and op in (0x44, 0xDF)           # pclmulqdq, aeskeygenassist
    p66, f2, f3 = 0x66 in pre, 0xF2 in pre, 0xF3 in pre
    if f2 or f3:
        # cvttps2dq, pshufhw (F3); pshuflw, cvtpd2dq (F2). movdqu is unaligned
        return (f3 and op in (0x5B, 0x70)) or (f2 and op in (0x70, 0xE6))
    if op in (0x28, 0x29, 0x2B, 0x14, 0x15, 0xC6, 0xC2, 0x5B):
        return True
    if 0x51 <= op <= 0x59 or 0x5C <= op <= 0x5F:
        return True
    if op in (0x5A, 0xE6):                    # cvtpd2ps, cvttpd2dq: m128 only with 66
        return p66
    if not p66:
        return False
    if op in (0x2C, 0x2D, 0x6F, 0x7F, 0x70, 0x74, 0x75, 0x76, 0xE7) or 0x60 <= op <= 0x6D:
        return True
    return (0xD1 <= op <= 0xD5 or 0xD8 <= op <= 0xDF or 0xE0 <= op <= 0xE5 or
            0xE8 <= op <= 0xEF or 0xF1 <= op <= 0xF6 or 0xF8 <= op <= 0xFE)


def memory_operand_address(bits, code, state, v2, modrm_at):
    """The linear address of the ModRM memory operand at code[modrm_at], or None
    for a register operand."""
    modrm = code[modrm_at]
    mod, rm = modrm >> 6, modrm & 7
    if mod == 3:
        return None
    count = 0
    while code[count] in PREFIX_BYTES:
        count += 1
    prefixes = code[:count]
    wide = (bits == 32) != (0x67 in prefixes)
    seg = 'ds'
    override = False
    for b in prefixes:
        if b in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65):
            seg = {0x26: 'es', 0x2E: 'cs', 0x36: 'ss', 0x3E: 'ds', 0x64: 'fs', 0x65: 'gs'}[b]
            override = True
    names = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
    pos = modrm_at + 1
    if wide:
        offset = 0
        base = None
        has_disp32 = (mod == 0 and rm == 5)
        if rm == 4:
            sib = code[pos]
            pos += 1
            scale, index, b = 1 << (sib >> 6), (sib >> 3) & 7, sib & 7
            if index != 4:
                offset += (state[names[index]] & M32) * scale
            if b == 5 and mod == 0:
                has_disp32 = True
            else:
                base = b
                offset += state[names[b]] & M32
        elif not has_disp32:
            base = rm
            offset += state[names[rm]] & M32
        if mod == 1:
            disp = code[pos] - 256 if code[pos] >= 128 else code[pos]
        elif mod == 2 or has_disp32:
            disp = int.from_bytes(code[pos:pos + 4], 'little', signed=True)
        else:
            disp = 0
        offset = (offset + disp) & M32
        if base in (4, 5) and not override:
            seg = 'ss'
    else:
        table = [('ebx', 'esi'), ('ebx', 'edi'), ('ebp', 'esi'), ('ebp', 'edi'),
                 ('esi',), ('edi',), ('ebp',), ('ebx',)]
        offset = 0
        if not (mod == 0 and rm == 6):
            offset = sum(state[n] & 0xFFFF for n in table[rm])
            if rm in (2, 3, 6) and not override:
                seg = 'ss'
        if mod == 1:
            disp = code[pos] - 256 if code[pos] >= 128 else code[pos]
        elif mod == 2 or (mod == 0 and rm == 6):
            disp = int.from_bytes(code[pos:pos + 2], 'little', signed=True)
        else:
            disp = 0
        offset = (offset + disp) & 0xFFFF
    return (segment_base(bits, v2['seg'].get(seg, 0)) + offset) & M32


def simd_fault(bits, code, state, v2):
    """True if the SDM raises #GP(0) for the instruction in this state and QEMU 5
    doesn't: a misaligned 128 bit operand, or ldmxcsr with a reserved bit."""
    d = _decode_simd(code)
    if d is None or d[3] >= len(code):
        return False
    pre, mapid, op, at = d
    if needs_alignment(code):
        address = memory_operand_address(bits, code, state, v2, at)
        return address is not None and address % 16 != 0
    if mapid == '0f' and op == 0xAE and (code[at] >> 3) & 7 == 2:
        address = memory_operand_address(bits, code, state, v2, at)
        if address is None:
            return False
        value = 0
        for i in range(4):
            a = (address + i) & M32
            value |= v2['mem'].get(a, pattern_byte(a)) << (8 * i)
        return value & 0xFFFF0000 != 0
    return False


def align_memory_operand(bits, code, state, rng):
    """In most of the states of an instruction that needs an aligned memory
    operand, move its base register so the operand is aligned (the others keep
    the random address and test the #GP)."""
    d = _decode_simd(code)
    if bits != 32 or d is None or d[3] >= len(code) or not needs_alignment(code):
        return
    pre, _, _, at = d
    if 0x67 in pre or rng.random() < 0.2:
        return
    v2 = state_defaults(state)
    address = memory_operand_address(bits, code, state, v2, at)
    modrm = code[at]
    if address is None or (modrm & 7) == 4 or (modrm >> 6 == 0 and (modrm & 7) == 5):
        return
    names = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
    base = names[modrm & 7]
    state[base] = (state[base] + (-address & 15)) & M32


def flush_to_zero_lanes(code, mxcsr, xmm, rex=0):
    """Two corrections of the floating point results of QEMU 5 (oracle_bugs.txt):
      * It ignores MXCSR.FZ: a denormal result of an arithmetic or narrowing
        conversion instruction is kept. With FZ the SDM returns a zero with the
        sign of the true result.
      * minps / maxps (and the pd, ss, sd forms) return the original bits of a
        denormal operand under DAZ. The SDM converts a denormal source operand
        to a zero of its sign before the computation, so the result is that
        zero.
    Returns (register, new value) of the destination xmm register, or None.
    `rex` is the REX prefix of a 64 bit mode instruction (`code` has no REX)."""
    d = _decode_simd(code)
    if d is None or d[1] != '0f' or d[3] >= len(code):
        return None
    pre, _, op, at = d
    reg = ((code[at] >> 3) & 7) | ((rex & 4) << 1)
    p66, f2, f3 = 0x66 in pre, 0xF2 in pre, 0xF3 in pre
    if op in (0x51, 0x58, 0x59, 0x5C, 0x5E) and mxcsr & 0x8000:
        pass
    elif op in (0x5D, 0x5F) and mxcsr & 0x40:
        pass
    elif op == 0x5A and (p66 or f2) and mxcsr & 0x8000:        # cvtpd2ps, cvtsd2ss
        pass
    else:
        return None
    if op == 0x5A:
        lanes = (32, 2) if p66 else (32, 1)
    elif f3:
        lanes = (32, 1)
    elif f2:
        lanes = (64, 1)
    elif p66:
        lanes = (64, 2)
    else:
        lanes = (32, 4)
    bits, count = lanes
    value = xmm[reg]
    exponent_mask = ((1 << (8 if bits == 32 else 11)) - 1) << (23 if bits == 32 else 52)
    fraction_mask = (1 << (23 if bits == 32 else 52)) - 1
    for i in range(count):
        lane = (value >> (bits * i)) & ((1 << bits) - 1)
        if lane & exponent_mask == 0 and lane & fraction_mask != 0:
            value &= ~(((1 << bits) - 1) << (bits * i))
            value |= (lane & (1 << (bits - 1))) << (bits * i)
    return reg, value


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
        v2 = state_defaults(state)
        if simd_fault(self.bits, code, state, v2):
            # #GP(0) (QEMU 5 doesn't raise it): the instruction doesn't execute
            res = self.run(b'\x90', state)
            res['exit'] = 'trap:13'
            return res
        segs = {n: v2['seg'].get(n, 0) for n in SEGS}
        try:
            # The code is at cs_base + eip
            code_at = (segment_base(self.bits, segs['cs']) + eip) & M32
            for base in sorted({((code_at + i) & M32) & ~(PAGE - 1) for i in range(len(code) + 1)}):
                self._map(base)
            # mem= (before the code, which replaces it), and the GDT
            for a in v2['mem']:
                if (a & ~(PAGE - 1)) not in self.pages:
                    self._map(a & ~(PAGE - 1))
            for a, b in v2['mem'].items():
                uc.mem_write(a, bytes([b]))
            for i, b in enumerate(code):
                uc.mem_write((code_at + i) & M32, bytes([b]))
            if self.bits == 32:
                for base in sorted({(GDT_ADDRESS + i) & ~(PAGE - 1) for i in range(len(GDT))}):
                    if base not in self.pages:
                        self._map(base)
                uc.mem_write(GDT_ADDRESS, GDT)
                uc.reg_write(X.UC_X86_REG_GDTR, (0, GDT_ADDRESS, GDT_LIMIT, 0))
            for n in REGS:
                uc.reg_write(UC_REG[n], state[n] & M32)
            uc.reg_write(X.UC_X86_REG_EFLAGS, state['eflags'] & M32)
            uc.reg_write(X.UC_X86_REG_EIP, eip)
            # Segments: in 32-bit mode a non-null selector is loaded from the
            # GDT (with the descriptor checks), the null ones keep Unicorn's
            # flat start. In 16-bit mode the base is selector * 16. cs first
            for n in ['cs', 'ss', 'ds', 'es', 'fs', 'gs']:
                if segs[n] != 0:
                    uc.reg_write(UC_SEG[n], segs[n])
            if self.bits == 32:
                # The loads set the accessed bits: restore the fixed GDT
                uc.mem_write(GDT_ADDRESS, GDT)
            # The writes of the setup (the accessed bits of the loads) aren't the
            # instruction's
            self.writes = []
            # x87: FP0..FP7 are the physical registers (ST(i) is (TOP+i)&7);
            # the tag register of Unicorn is the full 2-bit tag word where
            # only "3 = empty" is kept, see TEST_PLAN
            uc.reg_write(X.UC_X86_REG_FPCW, v2['fcw'])
            uc.reg_write(X.UC_X86_REG_FPSW, v2['fsw'])
            full_tag = 0
            for i in range(8):
                if not (v2['ftw'] >> i) & 1:
                    full_tag |= 3 << (2 * i)
            uc.reg_write(X.UC_X86_REG_FPTAG, full_tag)
            for i in range(8):
                uc.reg_write(X.UC_X86_REG_FP0 + i, (v2['fpr'][i] & (2 ** 64 - 1), v2['fpr'][i] >> 64))
            uc.reg_write(X.UC_X86_REG_MXCSR, v2['mxcsr'])
            for i in range(8):
                uc.reg_write(X.UC_X86_REG_XMM0 + i, v2['xmm'][i])

            rep = False
            for b in code[:14]:
                if b not in PREFIX_BYTES:
                    break
                rep = rep or b in (0xF2, 0xF3)
            # In 16-bit mode Unicorn takes the start as a linear address (it
            # sets ip = begin - cs * 16); in 32-bit mode it is eip
            begin = code_at if self.bits == 16 else eip
            invalid = False
            steps = 0
            while True:
                steps += 1
                if steps > MAX_STEPS:
                    raise OracleError('rep iteration limit')
                try:
                    uc.emu_start(begin, 0, count=1)
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
                code_page = code_at & ~(PAGE - 1)
                touched = any(((a + i) & M32) & ~(PAGE - 1) == code_page
                              for a, s in self.writes for i in range(s))
                if not (rep or touched):
                    break

            code_range = {(code_at + i) & M32 for i in range(len(code))}
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
            if self.bits == 32 and not exit_.startswith('trap:') and is_far_transfer(code):
                # QEMU's ljmp / lcall / lret don't set the accessed bit of the
                # code segment descriptor they load (the SDM: the processor
                # sets it when a selector is loaded into a segment register).
                # Apply the SDM, see oracle_bugs.txt
                idx = uc.reg_read(X.UC_X86_REG_CS) >> 3
                if (uc.reg_read(X.UC_X86_REG_CS) & 4) == 0 and 0 < idx < len(GDT) // 8:
                    a = GDT_ADDRESS + idx * 8 + 5
                    mem[a] = uc.mem_read(a, 1)[0] | 1
            res = {
                'exit': exit_,
                'regs': {n: uc.reg_read(UC_REG[n]) for n in REGS},
                'eip': uc.reg_read(X.UC_X86_REG_EIP),
                'eflags': (uc.reg_read(X.UC_X86_REG_EFLAGS) & EFLAGS_MASK) | 2,
                'mem': mem,
            }
            res['seg'] = {n: uc.reg_read(UC_SEG[n]) for n in SEGS}
            res['fcw'] = uc.reg_read(X.UC_X86_REG_FPCW)
            res['fsw'] = uc.reg_read(X.UC_X86_REG_FPSW)
            tag = uc.reg_read(X.UC_X86_REG_FPTAG)
            res['ftw'] = sum(1 << i for i in range(8) if (tag >> (2 * i)) & 3 != 3)
            fpr = []
            for i in range(8):
                mant, exp = uc.reg_read(X.UC_X86_REG_FP0 + i)
                fpr.append((exp << 64) | mant)
            mm = mmx_written_register(code) if exit_ in ('fall', 'branch') else None
            if mm is not None:
                # An MMX write sets the exponent and the sign of the x87
                # register to ones; QEMU leaves them
                fpr[mm] = (fpr[mm] & (2 ** 64 - 1)) | (0xFFFF << 64)
            res['fpr'] = fpr
            res['mxcsr'] = uc.reg_read(X.UC_X86_REG_MXCSR)
            res['xmm'] = [uc.reg_read(X.UC_X86_REG_XMM0 + i) for i in range(8)]
            if exit_ == 'fall':
                flushed = flush_to_zero_lanes(code, v2['mxcsr'], res['xmm'])
                if flushed is not None:
                    res['xmm'][flushed[0]] = flushed[1]
            return res
        finally:
            self.uc = None


class RosettaOracle(object):
    """The second oracle (rosetta_probe.c): the register-only SSE / SSE2
    instructions executed on the host's x86 floating point unit (Rosetta 2 on
    Apple Silicon). Unlike Unicorn it implements FZ, the NaN rules of the SSE and
    reports the exception flags of MXCSR. It supports 32-bit mode lines with
    xmm and general register operands only (no memory, no mm registers)."""

    def __init__(self, path):
        self.path = path
        self.proc = None
        self.pid = None

    def _process(self):
        import subprocess
        if self.proc is None or self.pid != os.getpid():
            self.pid = os.getpid()
            self.proc = subprocess.Popen([self.path], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, universal_newlines=True, bufsize=1)
        return self.proc

    @staticmethod
    def supported(code):
        """Raise OracleError unless the instruction is a register-only xmm one."""
        d = _decode_simd(code)
        if d is None or d[1] != '0f' or d[3] >= len(code):
            raise OracleError('rosetta: not an SSE instruction')
        pre, _, op, at = d
        # The general purpose opcodes of the 0F map, whose 66 / F3 are the
        # operand size or a different instruction (shld ax, cx, cl; popcnt)
        if op < 0x10 or 0x18 <= op <= 0x27 or 0x30 <= op <= 0x4F or \
                0x80 <= op <= 0xC1 or op == 0xC7 or op >= 0xC8 and op <= 0xCF:
            raise OracleError('rosetta: not an SSE instruction')
        modrm = code[at]
        if modrm >> 6 != 3:
            raise OracleError('rosetta: memory operand')
        p66, f2, f3 = 0x66 in pre, 0xF2 in pre, 0xF3 in pre
        if not (p66 or f2 or f3):
            sse = (0x10 <= op <= 0x17 or op in (0x28, 0x29, 0x2E, 0x2F, 0x50, 0xC2, 0xC6) or 0x51 <= op <= 0x5F)
            if not sse or op in (0x12, 0x16) and False:
                raise OracleError('rosetta: mm registers')
        elif (p66 and op in (0x2A, 0x2C, 0x2D)) or ((f2 or f3) and op == 0xD6):
            raise OracleError('rosetta: mm registers')
        elif op >= 0xC4 and op in (0xC4,):
            pass
        # The instructions with a general register operand must not use esp
        general = None
        if op in (0x2A,) and (f2 or f3):
            general = modrm & 7
        elif op in (0x2C, 0x2D) and (f2 or f3):
            general = (modrm >> 3) & 7
        elif op in (0x50, 0xC5, 0xD7):
            general = (modrm >> 3) & 7
        elif op == 0xC4:
            general = modrm & 7
        elif op in (0x6E, 0x7E) and p66:
            general = modrm & 7
        if general == 4:
            raise OracleError('rosetta: esp operand')

    def run(self, code, state):
        self.supported(code)
        v2 = state_defaults(state)
        line = '%s ; %s eflags=%x mxcsr=%x %s' % (
            code.hex(), ' '.join('%s=%x' % (n, state[n] & M32) for n in REGS if n != 'esp'),
            state['eflags'] & M32, v2['mxcsr'],
            ' '.join('xmm%d=%032x' % (i, v) for i, v in enumerate(v2['xmm'])))
        proc = self._process()
        proc.stdin.write(line + '\n')
        proc.stdin.flush()
        answer = proc.stdout.readline()
        if not answer.startswith('OK '):
            raise OracleError('rosetta: %s' % answer.strip())
        kv = dict(tok.split('=') for tok in answer[3:].split())
        regs = {n: state[n] & M32 for n in REGS}
        for n in REGS:
            if n in kv:
                regs[n] = int(kv[n], 16)
        flags_mask = 0x8D5
        eflags = ((state['eflags'] & ~flags_mask) | (int(kv['eflags'], 16) & flags_mask) | 2) & EFLAGS_MASK | 2
        res = {
            'exit': 'fall',
            'regs': regs,
            'eip': (state['eip'] + len(code)) & M32,
            'eflags': eflags,
            'mem': {},
            'seg': {n: v2['seg'].get(n, 0) for n in SEGS},
            'fcw': v2['fcw'], 'fsw': v2['fsw'], 'ftw': v2['ftw'], 'fpr': list(v2['fpr']),
            'mxcsr': int(kv['mxcsr'], 16),
            'xmm': [int(kv['xmm%d' % i], 16) for i in range(8)],
        }
        return res


_oracles = {}


_backend = {'rosetta': None}


def use_rosetta(path):
    """Run the 32-bit lines on the host x86 FPU (RosettaOracle) instead of Unicorn."""
    _backend['rosetta'] = path


def get_oracle(bits):
    if bits == 64:
        if 64 not in _oracles:
            import oracle64
            _oracles[64] = oracle64.Oracle64()
        return _oracles[64]
    if bits == 32 and _backend['rosetta']:
        if 'rosetta' not in _oracles:
            _oracles['rosetta'] = RosettaOracle(_backend['rosetta'])
        return _oracles['rosetta']
    if bits not in _oracles:
        _oracles[bits] = Oracle(bits)
    return _oracles[bits]


# ---------------------------------------------------------------------------
# The dismount_cli run line format
# ---------------------------------------------------------------------------

def format_state(code, state):
    """One input line of `dismount_cli run`. The v2 tokens are written only
    when they differ from their defaults. A state with 'rax' is a 64 bit one."""
    if 'rax' in state:
        import oracle64
        return oracle64.format_state(code, state)
    out = '%s ; %s eip=%x eflags=%x' % (
        code.hex(), ' '.join('%s=%x' % (n, state[n] & M32) for n in REGS),
        state['eip'] & M32, state['eflags'] & M32)
    for n in SEGS:
        if state.get('seg', {}).get(n, 0):
            out += ' %s=%x' % (n, state['seg'][n])
    if state.get('fcw', DEFAULT_FCW) != DEFAULT_FCW:
        out += ' fcw=%x' % state['fcw']
    if state.get('fsw', 0):
        out += ' fsw=%x' % state['fsw']
    if state.get('ftw', 0):
        out += ' ftw=%x' % state['ftw']
    for i, v in enumerate(state.get('fpr', [])):
        if v:
            out += ' r%d=%020x' % (i, v)
    if state.get('mxcsr', DEFAULT_MXCSR) != DEFAULT_MXCSR:
        out += ' mxcsr=%x' % state['mxcsr']
    for i, v in enumerate(state.get('xmm', [])):
        if v:
            out += ' xmm%d=%032x' % (i, v)
    if state.get('mem'):
        out += ' mem=' + format_mem(state['mem'])
    return out


def parse_state(line):
    if ' rax=' in line:
        import oracle64
        return oracle64.parse_state(line)
    hexs, _, rest = line.partition(';')
    st = {}
    for tok in rest.split():
        k, _, v = tok.partition('=')
        if k in SEGS:
            st.setdefault('seg', {})[k] = int(v, 16)
        elif k.startswith('xmm') and len(k) == 4:
            st.setdefault('xmm', [0] * 8)[int(k[3])] = int(v, 16)
        elif k[0] == 'r' and len(k) == 2 and k[1].isdigit():
            st.setdefault('fpr', [0] * 8)[int(k[1])] = int(v, 16)
        elif k == 'mem':
            st['mem'] = parse_mem(v)
        else:
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


def format_result(res, undef=0, fundef=0):
    if 'rip' in res:
        import oracle64
        return oracle64.format_result(res, undef, fundef)
    s = 'OK exit=%s undef=%08x fundef=%04x %s eip=%08x eflags=%08x' % (
        res['exit'], undef, fundef, ' '.join('%s=%08x' % (n, res['regs'][n]) for n in REGS),
        res['eip'], res['eflags'])
    s += ' ' + ' '.join('%s=%04x' % (n, res['seg'][n]) for n in SEGS)
    s += ' fcw=%04x fsw=%04x ftw=%02x' % (res['fcw'], res['fsw'], res['ftw'])
    s += ' ' + ' '.join('r%d=%020x' % (i, v) for i, v in enumerate(res['fpr']))
    s += ' mxcsr=%08x' % res['mxcsr']
    s += ' ' + ' '.join('xmm%d=%032x' % (i, v) for i, v in enumerate(res['xmm']))
    return s + ' mem=%s' % format_mem(res['mem'])


def parse_result(line):
    """Parses an `OK ...` / `ERR ...` output line. Returns a dict with the key
    'err' (the message) on ERR, otherwise the oracle shape plus 'undef',
    'fundef' and the segment bases 'base' (dismount only)."""
    if ' rax=' in line:
        import oracle64
        return oracle64.parse_result(line)
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
            'fundef': int(kv.get('fundef', '0'), 16),
            'regs': {n: int(kv[n], 16) for n in REGS},
            'eip': int(kv['eip'], 16),
            'eflags': int(kv['eflags'], 16),
            'seg': {n: int(kv[n], 16) for n in SEGS},
            'base': {n: int(kv[n + '_base'], 16) for n in SEGS},
            'fcw': int(kv['fcw'], 16),
            'fsw': int(kv['fsw'], 16),
            'ftw': int(kv['ftw'], 16),
            'fpr': [int(kv['r%d' % i], 16) for i in range(8)],
            'mxcsr': int(kv['mxcsr'], 16),
            'xmm': [int(kv['xmm%d' % i], 16) for i in range(8)],
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
