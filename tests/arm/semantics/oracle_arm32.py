#!/usr/bin/env python3
"""
The Unicorn side of the A32 / T32 semantics tests (see tests/arm/semantics/TEST_PLAN.md): the
bytes of one instruction (or a T32 `it` block) run from a given state on UC_ARCH_ARM with
UC_MODE_ARM or UC_MODE_THUMB, with the result in the shape of the `dismount_cli run --arch
a32|t32` output (armstate.py).

What Unicorn 2.1.4 (QEMU 5.0.1) does, verified with the probes in the notes of the TEST_PLAN:

  * The cpu model is UC_CPU_ARM_MAX (`uc.ctl_set_cpu_model` before anything else): ARMv8-A
    AArch32 with VFPv4 + Advanced SIMD (32 d registers), CRC32, the crypto extensions, vrint*
    and vmaxnm, but no FP16 arithmetic, no dot product and no BF16 (clang accepts them, the
    CPU doesn't: UNDEF). UC_CPU_ARM_CORTEX_A15 is ARMv7 (VFPv4, NEON, sdiv/udiv) and lacks the
    ARMv8 instructions.
  * The engine starts in SVC mode. The harness switches to user mode (CPSR.M = 0x10) with the
    flags of the state, so the privileged instructions UNDEF as they do for a process.
  * The FPU is disabled until CPACR.cp10 / cp11 = 3 (UC_ARM_REG_C1_C0_2 = 0xF << 20) and
    FPEXC.EN = 1 (UC_ARM_REG_FPEXC = 1 << 30): both are set before the first instruction.
  * Thumb: UC_MODE_THUMB and the start address with bit 0 set (`pc | 1`). An `it` block is part
    of the code, run as a unit: the harness stops when the pc reaches the end of the code
    (the code hook also ends the run at the first instruction outside of it, which is a branch).
  * The exception indices of QEMU reach UC_HOOK_INTR: 1 UDEF, 2 SWI, 7 BKPT, 3 / 4 aborts. An
    access to an unmapped address is a UcError (trap:fault).
  * fpscr: QEMU keeps the cumulative flags in the softfloat status and merges them on a read of
    FPSCR (vmrs / UC_ARM_REG_FPSCR); the QC bit of the saturating NEON instructions is kept in
    vfp.qc and also merged. Both are read through UC_ARM_REG_FPSCR.
"""
import struct

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_THUMB, UC_HOOK_CODE,
                     UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_ERR_INSN_INVALID, UC_ERR_EXCEPTION)
from unicorn import unicorn_const as UC
from unicorn import arm_const as A

import armstate as S

PAGE = 0x1000
MAX_INSNS = 256
EXCP_UDEF, EXCP_SWI, EXCP_PREFETCH_ABORT, EXCP_DATA_ABORT = 1, 2, 3, 4
EXCP_BKPT = 7
CPSR_USER = 0x10
CPSR_T = 0x20
CPU = A.UC_CPU_ARM_MAX


class OracleError(Exception):
    """The state or the instruction can't be run on this oracle."""


class Arm32Oracle(object):
    def __init__(self, thumb):
        self.thumb = thumb
        self.uc = None
        self.writes = set()
        self.trap = None
        self.branch_pc = None
        self.fetch_fault = False
        self.lo = self.hi = 0

    def _intr(self, uc, intno, user):
        self.trap = {EXCP_UDEF: 'undef', EXCP_SWI: 'svc', EXCP_BKPT: 'brk',
                     EXCP_PREFETCH_ABORT: 'fault', EXCP_DATA_ABORT: 'fault'}.get(intno, 'undef')
        uc.emu_stop()

    def _write(self, uc, access, addr, size, value, user):
        for i in range(size):
            self.writes.add(addr + i)

    def _code(self, uc, addr, size, user):
        if not self.lo <= addr < self.hi:
            if addr != self.hi:
                self.branch_pc = addr
            uc.emu_stop()

    def _new_engine(self):
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB if self.thumb else UC_MODE_ARM)
        uc.ctl_set_cpu_model(CPU)
        # The FPU: CPACR cp10 / cp11 full access, FPEXC.EN
        uc.reg_write(A.UC_ARM_REG_C1_C0_2, 0xF << 20)
        uc.reg_write(A.UC_ARM_REG_FPEXC, 1 << 30)
        uc.hook_add(UC_HOOK_INTR, self._intr)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._write)
        uc.hook_add(UC_HOOK_CODE, self._code)
        return uc

    def run(self, code, state):
        """code: bytes. state: armstate dict (A32). Returns a result dict."""
        if not code or (len(code) % (2 if self.thumb else 4)):
            raise OracleError('bad code size %d' % len(code))
        pc = state['pc']
        end = pc + len(code)
        uc = self.uc = self._new_engine()
        self.writes = set()
        self.trap = None
        self.branch_pc = None
        self.fetch_fault = False
        self.lo, self.hi = pc, end

        region = (pc & ~(PAGE - 1)) - PAGE
        uc.mem_map(region, 3 * PAGE)
        uc.mem_write(pc, code)
        uc.mem_map(S.A32_MEM_BASE, S.MEM_SIZE)
        uc.mem_write(S.A32_MEM_BASE, S.pattern_window(S.A32_MEM_BASE))
        for a, b in state['mem'].items():
            if not S.A32_MEM_BASE <= a < S.A32_MEM_BASE + S.MEM_SIZE:
                raise OracleError('mem outside of the window: %x' % a)
            uc.mem_write(a, bytes([b]))

        # The mode first: sp and lr are banked, the ones written in the SVC mode the engine
        # starts in are not the ones of the user mode
        uc.reg_write(A.UC_ARM_REG_CPSR, (state['apsr'] & S.APSR_MASK) | CPSR_USER |
                     (CPSR_T if self.thumb else 0))
        uc.reg_write(A.UC_ARM_REG_FPSCR, state['fpscr'])
        for i in range(16):
            uc.reg_write(A.UC_ARM_REG_Q0 + i, state['q'][i])
        for i in range(13):
            uc.reg_write(A.UC_ARM_REG_R0 + i, state['r'][i])
        uc.reg_write(A.UC_ARM_REG_SP, state['sp'])
        uc.reg_write(A.UC_ARM_REG_LR, state['lr'])

        exit_ = self._start(pc | 1 if self.thumb else pc, end)
        final_pc = uc.reg_read(A.UC_ARM_REG_PC)
        cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
        if exit_ is None:
            if self.branch_pc is not None:
                exit_, final_pc = 'branch', self.branch_pc
            elif self.fetch_fault:
                exit_ = 'branch'
            elif (final_pc & ~1) == end:
                exit_ = 'fall'
            else:
                exit_ = 'branch'
        res = {'exit': exit_, 'pc': final_pc & ~1, 't': 1 if cpsr & CPSR_T else 0}
        res['r'] = [uc.reg_read(A.UC_ARM_REG_R0 + i) for i in range(13)]
        res['sp'] = uc.reg_read(A.UC_ARM_REG_SP)
        res['lr'] = uc.reg_read(A.UC_ARM_REG_LR)
        res['apsr'] = cpsr & S.APSR_MASK
        res['fpscr'] = uc.reg_read(A.UC_ARM_REG_FPSCR)
        res['q'] = [uc.reg_read(A.UC_ARM_REG_Q0 + i) for i in range(16)]
        res['mem'] = {a: uc.mem_read(a, 1)[0] for a in self.writes
                      if S.A32_MEM_BASE <= a < S.A32_MEM_BASE + S.MEM_SIZE}
        return res

    def _start(self, begin, until):
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
            elif e.errno in (UC_ERR_EXCEPTION, UC_ERR_INSN_INVALID) and \
                    (self.uc.reg_read(A.UC_ARM_REG_PC) & ~1) == self.hi:
                # `yield` raises EXCP_YIELD, which Unicorn reports as an unhandled exception
                # (UC_ERR_INSN_INVALID) after the instruction completed, with the pc at the
                # end of the code (the interrupt hook is not called for it; an UNDEFINED
                # instruction goes through the hook)
                pass
            elif e.errno in (UC_ERR_INSN_INVALID, UC_ERR_EXCEPTION):
                self.trap = 'undef'
            else:
                raise OracleError('unicorn: %s' % e)
        if self.trap:
            return 'trap:' + self.trap
        return None


_oracles = {}


def get_oracle(thumb):
    if thumb not in _oracles:
        _oracles[thumb] = Arm32Oracle(thumb)
    return _oracles[thumb]


def run_line(arch, line):
    """Oracle answer for one input line (protocol), as an output line."""
    try:
        code, st = S.parse_state(arch, line)
        return S.format_result(arch, get_oracle(arch == 't32').run(code, st))
    except OracleError as e:
        return 'ERR oracle: %s' % e
