#!/usr/bin/env python3
"""
The Unicorn side of the A64 semantics tests (see tests/arm/semantics/TEST_PLAN.md):
the bytes of one instruction (or a short straight-line sequence) run from a given
state on UC_ARCH_ARM64, with the result in the shape of the `dismount_cli run
--arch a64` output (armstate.py).

What Unicorn 2.1.4 (QEMU 5.0.1) does, verified with the probes in the notes of
the TEST_PLAN:

  * The cpu model is UC_CPU_ARM64_MAX (`uc.ctl_set_cpu_model` before anything
    else): Advanced SIMD, FP16, dot product, SHA/AES/PMULL, atomics, CRC32,
    SVE (ID_AA64PFR0.SVE = 1, ID_AA64ZFR0 = 0: SVE but not SVE2, no SME,
    no BF16/I8MM). The default model (A57) has no SVE either.
  * The engine starts at EL1h with the exceptions masked. FP and SVE are
    disabled until the harness enables them: CPACR_EL1.FPEN / ZEN = 3 and
    CPTR_EL3.EZ = 1 (the max cpu has EL3, and with EZ = 0 an SVE instruction
    traps to EL3, which shows up as an UNDEF). These are system registers
    which Unicorn only exposes through UC_ARM64_REG_CP_REG, with the tuple
    (crn, crm, op0, op1, op2, value).
  * The vector length is ZCR_ELx.LEN of EL1, EL2 and EL3 (the smallest wins):
    all three are set to vl / 128 - 1. The max cpu accepts up to 2048 bits;
    the default of an unconfigured engine is 128 bits. The harness runs
    the vl of the state (128 .. 512).
  * Unicorn 2.1.4 has no Z / P / FFR register ids. The harness loads and
    stores them with SVE instructions in a prologue and an epilogue that run
    around the instruction under test, over a transfer buffer (see Arm64Oracle).
  * SME does not exist (QEMU 5): `smstart` and every SME instruction is an
    UNDEF. A state with sm=1 or za=1 is an OracleError (use the host oracle).
  * An instruction that raises an exception does not return an error from
    emu_start when UC_HOOK_INTR is installed: the hook gets the QEMU exception
    index (1 UDEF, 2 SWI, 7 BKPT, 3 / 4 prefetch / data abort), mapped to
    trap:undef / svc / brk / fault. An access to an unmapped address is a
    UcError (trap:fault).
"""
import struct

from unicorn import (Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_INTR,
                     UC_HOOK_MEM_WRITE, UC_ERR_INSN_INVALID, UC_ERR_EXCEPTION)
from unicorn import unicorn_const as UC
from unicorn.arm64_const import *

import armstate as S

CPU_MAX = UC_CPU_ARM64_MAX
PAGE = 0x1000
HOST_PAGE = 0x4000                  # the page size of the host runner (macOS arm64)
SLED_WORD = 0xD4200000 | (0x1234 << 5)      # brk #0x1234
END_WORD = 0xD4200000 | (0x1235 << 5)       # brk #0x1235
MAX_INSNS = 256
# The prologue / epilogue pages, and the transfer buffer of the z / p registers
BUF_BASE = 0x0000010200000000
BUF_SIZE = 0x10000

# QEMU's exception indices (target/arm/cpu.h)
EXCP_UDEF, EXCP_SWI, EXCP_PREFETCH_ABORT, EXCP_DATA_ABORT = 1, 2, 3, 4
EXCP_BKPT = 7


class OracleError(Exception):
    """The state or the instruction can't be run on this oracle."""


def xreg(i):
    """The Unicorn id of x<i>: x0..x28 are consecutive, x29 and x30 are FP and LR."""
    return UC_ARM64_REG_X0 + i if i < 29 else (UC_ARM64_REG_X29 if i == 29 else UC_ARM64_REG_X30)


def cp_reg(crn, crm, op0, op1, op2):
    return (crn, crm, op0, op1, op2)


# ---------------------------------------------------------------------------
# The prologue and the epilogue: SVE instructions that move z, p and ffr between
# the registers and the transfer buffer
# ---------------------------------------------------------------------------

def _imm9(imm):
    return ((imm >> 3) & 0x3F) << 16 | (imm & 7) << 10


def ldr_z(zt, rn, imm):
    return 0x85804000 | _imm9(imm) | rn << 5 | zt


def str_z(zt, rn, imm):
    return 0xE5804000 | _imm9(imm) | rn << 5 | zt


def ldr_p(pt, rn, imm):
    return 0x85800000 | _imm9(imm) | rn << 5 | pt


def str_p(pt, rn, imm):
    return 0xE5800000 | _imm9(imm) | rn << 5 | pt


WRFFR_P0 = 0x25289000           # wrffr p0.b
RDFFR_P0 = 0x2519F000           # rdffr p0.b
RET_MARKER = 0xD503201F         # nop


def prologue_words():
    """ldr z0..z31 from [x0 + i * VL], ldr p0..p15 from [x1 + i * PL], ffr from [x1 + 16 * PL]"""
    words = []
    for i in range(32):
        words.append(ldr_z(i, 0, i))
    words.append(ldr_p(0, 1, 16))
    words.append(WRFFR_P0)
    for i in range(16):
        words.append(ldr_p(i, 1, i))
    return words


def epilogue_words():
    words = []
    for i in range(32):
        words.append(str_z(i, 0, i))
    for i in range(16):
        words.append(str_p(i, 1, i))
    words.append(RDFFR_P0)
    words.append(str_p(0, 1, 16))
    return words


def _words(words):
    return b''.join(struct.pack('<I', w) for w in words)


class Arm64Oracle(object):
    def __init__(self, vl=S.DEFAULT_VL):
        if vl not in (128, 256, 384, 512, 640, 768, 896, 1024, 2048) and vl % 128:
            raise OracleError('vl %d' % vl)
        self.vl = vl
        self.uc = None
        self.writes = set()
        self.trap = None
        self.branch_pc = None
        self.fetch_fault = False
        self.watching = False
        self.lo = self.hi = 0

    # -- hooks ---------------------------------------------------------------
    def _intr(self, uc, intno, user):
        self.trap = {EXCP_UDEF: 'undef', EXCP_SWI: 'svc', EXCP_BKPT: 'brk',
                     EXCP_PREFETCH_ABORT: 'fault', EXCP_DATA_ABORT: 'fault'}.get(intno, 'undef')
        uc.emu_stop()

    def _write(self, uc, access, addr, size, value, user):
        if self.watching:
            for i in range(size):
                self.writes.add(addr + i)

    def _code(self, uc, addr, size, user):
        # The code under test is [lo, hi): a branch leaves it (and hi is the fall through)
        if self.watching and not self.lo <= addr < self.hi:
            if addr != self.hi:
                self.branch_pc = addr
            uc.emu_stop()

    # -- the engine -----------------------------------------------------------
    def _new_engine(self):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.ctl_set_cpu_model(CPU_MAX)
        # FP and SVE: CPACR_EL1.FPEN / ZEN and CPTR_EL3.EZ, then the vector length
        uc.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20 | 3 << 16)
        uc.reg_write(UC_ARM64_REG_CP_REG, cp_reg(1, 1, 3, 6, 2) + (1 << 8,))
        lenfield = self.vl // 128 - 1
        for op1 in (0, 4, 6):                               # ZCR_EL1, ZCR_EL2, ZCR_EL3
            uc.reg_write(UC_ARM64_REG_CP_REG, cp_reg(1, 2, 3, op1, 0) + (lenfield,))
        uc.hook_add(UC_HOOK_INTR, self._intr)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._write)
        uc.hook_add(UC_HOOK_CODE, self._code)
        return uc

    def run(self, code, state):
        """code: bytes. state: armstate dict. Returns a result dict (armstate.parse_result shape).
        Raises OracleError when the state can't be run."""
        if state.get('sm') or state.get('za'):
            raise OracleError('Unicorn has no SME (streaming mode / ZA)')
        sve = bool(state.get('sve'))
        if sve and state.get('vl', S.DEFAULT_VL) != self.vl:
            raise OracleError('state vl %d, oracle vl %d' % (state.get('vl'), self.vl))
        if len(code) % 4 or not code:
            raise OracleError('A64 code is a multiple of 4 bytes')
        pc = state['pc']
        end = pc + len(code)
        uc = self.uc = self._new_engine()
        self.writes = set()
        self.trap = None
        self.branch_pc = None
        self.fetch_fault = False
        self.watching = False
        self.lo, self.hi = pc, end
        vlb = self.vl // 8

        # Memory: the code region has the layout of the host runner (host_arm64.c): three 16K
        # pages, the prologue in the first, the instruction at the pc, `brk #0x1235` after it
        # and `brk #0x1234` in every other word (a pc relative load reads the same words on
        # both engines)
        page = pc & ~(PAGE - 1)
        region = (pc & ~(HOST_PAGE - 1)) - HOST_PAGE
        uc.mem_map(region, 3 * HOST_PAGE)
        uc.mem_write(region, struct.pack('<I', SLED_WORD) * (3 * HOST_PAGE // 4))
        uc.mem_write(pc, code + struct.pack('<I', END_WORD))
        pro_at = region
        epi_at = region + 3 * HOST_PAGE - PAGE
        uc.mem_map(S.A64_MEM_BASE, S.MEM_SIZE)
        uc.mem_write(S.A64_MEM_BASE, S.pattern_window(S.A64_MEM_BASE))
        for a, b in state['mem'].items():
            if not S.A64_MEM_BASE <= a < S.A64_MEM_BASE + S.MEM_SIZE:
                raise OracleError('mem outside of the window: %x' % a)
            uc.mem_write(a, bytes([b]))
        if sve:
            uc.mem_map(BUF_BASE, BUF_SIZE)

        # Initial state. The prologue (z and p from the buffer; x0 and x1 are the bases) also sets
        # the low 128 bits of z, so the v registers are written after it
        uc.reg_write(UC_ARM64_REG_FPCR, state['fpcr'])
        uc.reg_write(UC_ARM64_REG_FPSR, state['fpsr'])
        if sve:
            buf = b''.join(z.to_bytes(vlb, 'little') for z in state['z'])
            pbuf = b''.join(p.to_bytes(vlb // 8, 'little') for p in state['p'] + [state['ffr']])
            uc.mem_write(BUF_BASE, buf)
            uc.mem_write(BUF_BASE + 0x8000, pbuf)
            words = prologue_words()
            uc.mem_write(pro_at, _words(words))
            uc.reg_write(UC_ARM64_REG_X0, BUF_BASE)
            uc.reg_write(UC_ARM64_REG_X1, BUF_BASE + 0x8000)
            if self._start(pro_at, pro_at + 4 * len(words)):
                raise OracleError('the SVE prologue trapped: %s' % self.trap)
        for i in range(32):
            uc.reg_write(UC_ARM64_REG_Q0 + i, state['v'][i])
        for i in range(31):
            uc.reg_write(xreg(i), state['x'][i])
        uc.reg_write(UC_ARM64_REG_SP, state['sp'])
        uc.reg_write(UC_ARM64_REG_NZCV, state['nzcv'] << 28)

        # The instruction
        self.watching = True
        exit_ = self._start(pc, end)
        self.watching = False
        final_pc = uc.reg_read(UC_ARM64_REG_PC)
        if exit_ is None:
            if self.branch_pc is not None:
                exit_, final_pc = 'branch', self.branch_pc
            elif self.fetch_fault:
                exit_ = 'branch'
            elif final_pc == end:
                exit_ = 'fall'
            else:
                exit_, final_pc = 'branch', final_pc
        res = {'exit': exit_, 'sve': sve, 'sm': False, 'za': False, 'vl': self.vl,
               'svl': state.get('svl', S.DEFAULT_SVL), 'pc': final_pc}
        res['x'] = [uc.reg_read(xreg(i)) for i in range(31)]
        res['sp'] = uc.reg_read(UC_ARM64_REG_SP)
        res['nzcv'] = (uc.reg_read(UC_ARM64_REG_NZCV) >> 28) & 0xF
        res['fpcr'] = uc.reg_read(UC_ARM64_REG_FPCR)
        res['fpsr'] = uc.reg_read(UC_ARM64_REG_FPSR)
        res['v'] = [uc.reg_read(UC_ARM64_REG_Q0 + i) for i in range(32)]
        res['mem'] = {a: uc.mem_read(a, 1)[0] for a in self.writes
                      if S.A64_MEM_BASE <= a < S.A64_MEM_BASE + S.MEM_SIZE}
        if sve:
            if exit_ == 'fall':
                # The epilogue stores z, p and ffr to the buffer (it clobbers p0 with rdffr, after
                # the store of p0). x0 and x1 were read above
                words = epilogue_words()
                uc.mem_write(epi_at, _words(words))
                uc.reg_write(UC_ARM64_REG_X0, BUF_BASE)
                uc.reg_write(UC_ARM64_REG_X1, BUF_BASE + 0x8000)
                if self._start(epi_at, epi_at + 4 * len(words)):
                    raise OracleError('the SVE epilogue trapped: %s' % self.trap)
                zraw = bytes(uc.mem_read(BUF_BASE, 32 * vlb))
                pl = vlb // 8
                praw = bytes(uc.mem_read(BUF_BASE + 0x8000, 17 * pl))
                res['z'] = [int.from_bytes(zraw[i * vlb:(i + 1) * vlb], 'little') for i in range(32)]
                res['p'] = [int.from_bytes(praw[i * pl:(i + 1) * pl], 'little') for i in range(16)]
                res['ffr'] = int.from_bytes(praw[16 * pl:17 * pl], 'little')
            else:
                # z and p are not read after a branch or a trap
                res['sve'] = False
        return res

    def _start(self, begin, until):
        """emu_start with the trap mapping. Returns the exit ('trap:...') or None."""
        try:
            self.uc.emu_start(begin, until, count=MAX_INSNS)
        except UcError as e:
            if e.errno == UC.UC_ERR_FETCH_UNMAPPED:
                # A taken branch to an address that isn't mapped: the pc is the target
                self.fetch_fault = True
            elif e.errno in (UC.UC_ERR_READ_UNMAPPED, UC.UC_ERR_WRITE_UNMAPPED,
                           UC.UC_ERR_READ_UNALIGNED,
                           UC.UC_ERR_WRITE_UNALIGNED, UC.UC_ERR_FETCH_UNALIGNED,
                           UC.UC_ERR_READ_PROT, UC.UC_ERR_WRITE_PROT, UC.UC_ERR_FETCH_PROT):
                self.trap = 'fault'
            elif e.errno in (UC_ERR_INSN_INVALID, UC_ERR_EXCEPTION):
                self.trap = 'undef'
            else:
                raise OracleError('unicorn: %s' % e)
        if self.trap:
            return 'trap:' + self.trap
        return None


_oracles = {}


def get_oracle(vl=S.DEFAULT_VL):
    if vl not in _oracles:
        _oracles[vl] = Arm64Oracle(vl)
    return _oracles[vl]


def run_line(line, vl=S.DEFAULT_VL):
    """Oracle answer for one input line (protocol), as an output line."""
    try:
        code, st = S.parse_state('a64', line)
        return S.format_result('a64', get_oracle(st.get('vl', vl) if st.get('sve') else vl).run(code, st))
    except OracleError as e:
        return 'ERR oracle: %s' % e
